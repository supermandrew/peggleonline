#include "net.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <deque>
#include <mutex>

#include "log.h"
#include "protocol.h"

namespace net {

namespace {

constexpr uint32_t kMaxFrame = 1 << 20;
constexpr uint32_t kMaxHello = 256;
constexpr DWORD kHelloWait = 5000;

std::mutex g_mutex;  // guards everything below except the sockets' blocking calls
HANDLE g_worker;  // not std::thread: one still running at process exit would call terminate()
std::atomic<State> g_state{State::Idle};
std::atomic<bool> g_stop{false};
std::string g_error;
std::string g_peer;
SOCKET g_listen = INVALID_SOCKET;
SOCKET g_sock = INVALID_SOCKET;
std::mutex g_send_mutex;
std::deque<Message> g_inbox;
std::atomic<DWORD> g_last_receive{0};

void EnsureWinsock()
{
    static bool done = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)done;
}

void Fail(const std::string& why)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state != State::Closed && !g_stop)
        Log("net: %s", why.c_str());
    if (g_error.empty())
        g_error = why;
    g_state = State::Closed;
}

std::string AddrToString(const sockaddr* sa)
{
    char host[INET6_ADDRSTRLEN] = "?";
    if (sa->sa_family == AF_INET)
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, host, sizeof host);
    else if (sa->sa_family == AF_INET6)
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, host, sizeof host);
    return host;
}

bool RecvAll(SOCKET s, void* buf, int len)
{
    auto* p = static_cast<char*>(buf);
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n <= 0)
            return false;
        p += n;
        len -= n;
    }
    return true;
}

void ReceiveLoop()
{
    for (;;) {
        uint32_t len;
        if (!RecvAll(g_sock, &len, 4) || len == 0 || len > kMaxFrame) {
            Fail(g_stop ? "closed" : "The connection to your opponent was lost.");
            return;
        }
        Message m;
        m.data.resize(len);
        if (!RecvAll(g_sock, m.data.data(), static_cast<int>(len))) {
            Fail("The connection to your opponent was lost.");
            return;
        }
        m.type = m.data[0];
        m.data.erase(m.data.begin());
        g_last_receive = GetTickCount();
        std::lock_guard<std::mutex> lock(g_mutex);
        g_inbox.push_back(std::move(m));
    }
}

void OnConnected(SOCKET s, const sockaddr* peer)
{
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&one), sizeof one);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_sock = s;
        g_peer = AddrToString(peer);
        g_last_receive = GetTickCount();
        g_state = State::Connected;
    }
    Log("net: connected to %s", g_peer.c_str());
    ReceiveLoop();
}

// There's only one opponent, and a port open to the internet gets all sorts of callers: port scanners, a join that
// gave up. Whoever connects has to say hello before the host stops waiting for an opponent.
bool ReadHello(SOCKET s, Message& hello)
{
    DWORD wait = kHelloWait;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&wait), sizeof wait);
    uint32_t len = 0;
    if (!RecvAll(s, &len, 4) || len == 0 || len > kMaxHello)
        return false;
    hello.data.resize(len);
    if (!RecvAll(s, hello.data.data(), static_cast<int>(len)) || hello.data[0] != proto::kHello)
        return false;
    hello.type = hello.data[0];
    hello.data.erase(hello.data.begin());
    wait = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&wait), sizeof wait);
    return true;
}

void HostThread(uint16_t port)
{
    // Dual-stack listener, so both IPv4 and IPv6 opponents can connect.
    SOCKET ls = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    DWORD v6only = 0;
    setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&v6only), sizeof v6only);
    BOOL exclusive = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive), sizeof exclusive);
    sockaddr_in6 addr = {};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    if (ls == INVALID_SOCKET || bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        listen(ls, 1) != 0) {
        int err = WSAGetLastError();
        if (ls != INVALID_SOCKET)
            closesocket(ls);
        Fail(err == WSAEADDRINUSE ? "Port " + std::to_string(port) + " is already in use on this computer."
                                  : "Could not start hosting (error " + std::to_string(err) + ").");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_listen = ls;
        g_state = State::Listening;
    }
    Log("net: listening on port %u", port);
    for (;;) {
        sockaddr_storage peer = {};
        int peer_len = sizeof peer;
        SOCKET s = accept(ls, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (s == INVALID_SOCKET) {
            Fail(g_stop ? "closed" : "Hosting stopped unexpectedly.");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_sock = s;  // (Shutdown closes it, which ends the wait for a hello)
        }
        Message hello;
        if (ReadHello(s, hello)) {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                closesocket(g_listen);  // one opponent per session
                g_listen = INVALID_SOCKET;
                g_inbox.push_back(std::move(hello));
            }
            OnConnected(s, reinterpret_cast<sockaddr*>(&peer));
            return;
        }
        Log("net: %s came and said nothing, still waiting", AddrToString(reinterpret_cast<sockaddr*>(&peer)).c_str());
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_sock == s) {
            closesocket(s);
            g_sock = INVALID_SOCKET;
        }
    }
}

void JoinThread(std::string host, uint16_t port)
{
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list) != 0 || !list) {
        Fail("Could not find \"" + host + "\". Check the address and try again.");
        return;
    }
    std::string why = "Could not connect to " + host + ". Make sure your opponent is hosting.";
    for (addrinfo* ai = list; ai && !g_stop; ai = ai->ai_next) {
        SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_sock = s;  // so Shutdown() can abort the connect
        }
        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);
        connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        fd_set wr, ex;
        FD_ZERO(&wr);
        FD_ZERO(&ex);
        FD_SET(s, &wr);
        FD_SET(s, &ex);
        timeval tv = {10, 0};
        bool ok = select(0, nullptr, &wr, &ex, &tv) == 1 && FD_ISSET(s, &wr);
        if (ok) {
            nonblocking = 0;
            ioctlsocket(s, FIONBIO, &nonblocking);
            sockaddr_storage peer = {};
            memcpy(&peer, ai->ai_addr, ai->ai_addrlen);
            freeaddrinfo(list);
            OnConnected(s, reinterpret_cast<sockaddr*>(&peer));
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        closesocket(s);
        g_sock = INVALID_SOCKET;
    }
    freeaddrinfo(list);
    Fail(g_stop ? "closed" : why);
}

void Begin(State state)
{
    Shutdown();
    EnsureWinsock();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stop = false;
    g_error.clear();
    g_peer.clear();
    g_inbox.clear();
    g_state = state;
}

}  // namespace

struct JoinArgs {
    std::string host;
    uint16_t port;
};

DWORD WINAPI HostThreadMain(void* arg)
{
    HostThread(static_cast<uint16_t>(reinterpret_cast<uintptr_t>(arg)));
    return 0;
}

DWORD WINAPI JoinThreadMain(void* arg)
{
    JoinArgs* args = static_cast<JoinArgs*>(arg);
    JoinThread(args->host, args->port);
    delete args;
    return 0;
}

void StartHost(uint16_t port)
{
    Begin(State::Listening);
    g_worker = CreateThread(nullptr, 0, HostThreadMain, reinterpret_cast<void*>(static_cast<uintptr_t>(port)), 0,
                            nullptr);
}

void StartJoin(const std::string& host, uint16_t port)
{
    Begin(State::Connecting);
    g_worker = CreateThread(nullptr, 0, JoinThreadMain, new JoinArgs{host, port}, 0, nullptr);
}

void Shutdown()
{
    // While connected, close gracefully: what we sent last (a goodbye) goes out before our end-of-stream, and
    // the receive thread keeps reading until the opponent closes too. Closing with unread data would reset the
    // connection instead, and a reset can make the opponent lose that last message.
    bool graceful;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
        if (g_listen != INVALID_SOCKET)
            closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        graceful = g_sock != INVALID_SOCKET && g_state == State::Connected && g_worker;
        if (graceful)
            shutdown(g_sock, SD_SEND);
    }
    if (graceful)
        WaitForSingleObject(g_worker, 1000);  // an unresponsive opponent gets cut off below
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_sock != INVALID_SOCKET) {
            shutdown(g_sock, SD_BOTH);
            closesocket(g_sock);
        }
        g_sock = INVALID_SOCKET;
    }
    if (g_worker) {
        WaitForSingleObject(g_worker, INFINITE);
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    g_state = State::Idle;
}

State GetState() { return g_state; }

std::string GetError()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_error;
}

std::string PeerAddress()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_peer;
}

uint32_t MillisSinceReceive() { return GetTickCount() - g_last_receive; }

bool Send(uint8_t type, const void* data, size_t len)
{
    if (g_state != State::Connected)
        return false;
    std::vector<char> frame(5 + len);
    uint32_t n = static_cast<uint32_t>(len + 1);
    memcpy(frame.data(), &n, 4);
    frame[4] = static_cast<char>(type);
    if (len)
        memcpy(frame.data() + 5, data, len);
    std::lock_guard<std::mutex> lock(g_send_mutex);
    const char* p = frame.data();
    int left = static_cast<int>(frame.size());
    while (left > 0) {
        int sent = send(g_sock, p, left, 0);
        if (sent <= 0) {
            Fail("The connection to your opponent was lost.");
            return false;
        }
        p += sent;
        left -= sent;
    }
    return true;
}

bool Receive(Message& out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_inbox.empty())
        return false;
    out = std::move(g_inbox.front());
    g_inbox.pop_front();
    return true;
}

bool ParseAddress(const std::string& text, std::string& host, uint16_t& port)
{
    std::string t;
    for (char c : text)
        if (c != ' ' && c != '\t')
            t += c;
    port = kDefaultPort;
    if (t.empty())
        return false;
    if (t[0] == '[') {  // [v6 address]:port
        size_t close = t.find(']');
        if (close == std::string::npos)
            return false;
        host = t.substr(1, close - 1);
        if (close + 2 < t.size() && t[close + 1] == ':')
            port = static_cast<uint16_t>(atoi(t.c_str() + close + 2));
        return !host.empty();
    }
    size_t colon = t.find(':');
    if (colon != std::string::npos && t.find(':', colon + 1) == std::string::npos) {
        host = t.substr(0, colon);
        port = static_cast<uint16_t>(atoi(t.c_str() + colon + 1));
    } else {
        host = t;  // plain host name, IPv4, or bare IPv6
    }
    if (port == 0)
        port = kDefaultPort;
    return !host.empty();
}

}  // namespace net
