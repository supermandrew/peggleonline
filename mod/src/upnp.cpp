#include "upnp.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <mutex>
#include <set>

#include "log.h"

namespace upnp {

namespace {

std::mutex g_mutex;
Result g_result;
HANDLE g_thread;  // not std::thread: see net.cpp
uint16_t g_port;
std::string g_control_url, g_service;  // where our mapping was made

struct Url {
    std::string host, path;
    uint16_t port = 80;
};

bool ParseUrl(const std::string& url, Url& out)
{
    const std::string scheme = "http://";
    if (url.compare(0, scheme.size(), scheme) != 0)
        return false;
    size_t start = scheme.size();
    size_t slash = url.find('/', start);
    std::string hostport = url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    out.path = slash == std::string::npos ? "/" : url.substr(slash);
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        out.host = hostport.substr(0, colon);
        out.port = static_cast<uint16_t>(atoi(hostport.c_str() + colon + 1));
    } else {
        out.host = hostport;
    }
    return !out.host.empty();
}

std::string Resolve(const std::string& base, const std::string& ref)
{
    if (ref.compare(0, 7, "http://") == 0)
        return ref;
    Url u;
    if (!ParseUrl(base, u))
        return ref;
    std::string root = "http://" + u.host + ":" + std::to_string(u.port);
    return ref.empty() || ref[0] != '/' ? root + "/" + ref : root + ref;
}

// Text of the first leaf element <name> or <prefix:name>.
std::string Tag(const std::string& xml, const std::string& name)
{
    for (size_t pos = 0; (pos = xml.find(name + ">", pos)) != std::string::npos; pos += name.size()) {
        size_t lt = xml.rfind('<', pos);
        if (lt == std::string::npos || xml[lt + 1] == '/')
            continue;
        size_t open_end = pos + name.size() + 1;
        size_t close = xml.find("</", open_end);
        if (close == std::string::npos)
            return {};
        return xml.substr(open_end, close - open_end);
    }
    return {};
}

// Minimal HTTP/1.1 request with Connection: close. Returns the status code, body in `body`.
int Http(const std::string& url, const std::string& method, const std::string& extra_headers,
         const std::string& payload, std::string& body)
{
    Url u;
    if (!ParseUrl(url, u))
        return -1;
    addrinfo hints = {}, *ai = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &ai) != 0 || !ai)
        return -1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD timeout = 4000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof timeout);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof timeout);
    int status = -1;
    if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
        std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) +
                          "\r\nConnection: close\r\n" + extra_headers;
        if (!payload.empty())
            req += "Content-Length: " + std::to_string(payload.size()) + "\r\n";
        req += "\r\n" + payload;
        send(s, req.data(), static_cast<int>(req.size()), 0);
        std::string resp;
        char buf[4096];
        int n;
        while ((n = recv(s, buf, sizeof buf, 0)) > 0)
            resp.append(buf, n);
        if (resp.compare(0, 5, "HTTP/") == 0)
            status = atoi(resp.c_str() + resp.find(' ') + 1);
        size_t split = resp.find("\r\n\r\n");
        body = split == std::string::npos ? std::string() : resp.substr(split + 4);
    }
    closesocket(s);
    freeaddrinfo(ai);
    return status;
}

int Soap(const std::string& control, const std::string& service, const std::string& action,
         const std::string& args, std::string& body)
{
    std::string envelope =
        "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" +
        service + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
    std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + service + "#" + action +
                          "\"\r\n";
    return Http(control, "POST", headers, envelope, body);
}

std::string DefaultRouteIp()
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);  // no packet is sent; this only picks the outgoing adapter
    std::string ip;
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0) {
        sockaddr_in me = {};
        int len = sizeof me;
        getsockname(s, reinterpret_cast<sockaddr*>(&me), &len);
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &me.sin_addr, text, sizeof text);
        ip = text;
    }
    closesocket(s);
    return ip;
}

std::set<std::string> Discover(const std::string& local_ip)
{
    std::set<std::string> locations;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    in_addr iface = {};
    inet_pton(AF_INET, local_ip.c_str(), &iface);
    // Send discovery out of the adapter that has the default route (VPN or virtual adapters can win otherwise).
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<char*>(&iface), sizeof iface);
    sockaddr_in me = {};
    me.sin_family = AF_INET;
    me.sin_addr = iface;
    bind(s, reinterpret_cast<sockaddr*>(&me), sizeof me);
    sockaddr_in group = {};
    group.sin_family = AF_INET;
    group.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &group.sin_addr);
    const char* targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                             "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                             "urn:schemas-upnp-org:service:WANIPConnection:1",
                             "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    for (int round = 0; round < 2; ++round)
        for (const char* st : targets) {
            std::string msg = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                                          "MX: 2\r\nST: ") + st + "\r\n\r\n";
            sendto(s, msg.data(), static_cast<int>(msg.size()), 0, reinterpret_cast<sockaddr*>(&group), sizeof group);
        }
    DWORD until = GetTickCount() + 3000;
    for (;;) {
        DWORD now = GetTickCount();
        if (static_cast<int>(until - now) <= 0)
            break;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv = {0, static_cast<long>((until - now) * 1000)};
        if (select(0, &rd, nullptr, nullptr, &tv) != 1)
            break;
        char buf[2048];
        int n = recv(s, buf, sizeof buf - 1, 0);
        if (n <= 0)
            continue;
        std::string resp(buf, n);
        std::string lower = resp;
        for (char& c : lower)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        size_t at = lower.find("\nlocation:");
        if (at == std::string::npos)
            continue;
        size_t start = resp.find_first_not_of(" \t", at + 10);
        size_t end = resp.find_first_of("\r\n", start);
        locations.insert(resp.substr(start, end - start));
    }
    closesocket(s);
    return locations;
}

void SetResult(const Result& r)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_result = r;
}

void MapThread(uint16_t port)
{
    Result r;
    r.local_ip = DefaultRouteIp();
    if (r.local_ip.empty()) {
        r.done = true;
        r.error = "This computer does not seem to be connected to a network.";
        SetResult(r);
        return;
    }
    std::set<std::string> locations = Discover(r.local_ip);
    std::string port_text = std::to_string(port);
    if (locations.empty()) {
        r.done = true;
        r.error = "No router answered the request to open port " + port_text + " (UPnP may be turned off).";
        SetResult(r);
        Log("upnp: no gateway answered discovery");
        return;
    }
    std::string last_error =
        "Your router didn't respond to the request to open port " + port_text + " (UPnP may be turned off).";
    for (const std::string& loc : locations) {
        std::string desc;
        int status = Http(loc, "GET", "", "", desc);
        Log("upnp: description %s -> HTTP %d", loc.c_str(), status);
        if (status != 200)
            continue;
        std::string base = Tag(desc, "URLBase");
        if (base.empty())
            base = loc;
        // Every <service> block: keep the WAN connection services (IP first, then PPP).
        for (const char* wanted : {"WANIPConnection", "WANPPPConnection"}) {
            for (size_t open = desc.find("<service>"); open != std::string::npos;
                 open = desc.find("<service>", open + 1)) {
                size_t close = desc.find("</service>", open);
                if (close == std::string::npos)
                    break;
                std::string svc = desc.substr(open, close - open);
                std::string type = Tag(svc, "serviceType");
                if (type.find(wanted) == std::string::npos)
                    continue;
                std::string control = Resolve(base, Tag(svc, "controlURL"));
                std::string body;
                if (Soap(control, type, "GetExternalIPAddress", "", body) == 200)
                    r.external_ip = Tag(body, "NewExternalIPAddress");
                std::string args_head = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
                                        "</NewExternalPort><NewProtocol>TCP</NewProtocol><NewInternalPort>" +
                                        std::to_string(port) + "</NewInternalPort><NewInternalClient>" + r.local_ip +
                                        "</NewInternalClient><NewEnabled>1</NewEnabled>"
                                        "<NewPortMappingDescription>Peggle Nights online duel"
                                        "</NewPortMappingDescription>";
                // Prefer a mapping that expires on its own (in case the game is killed); some routers only
                // accept permanent ones (lease 0).
                for (const char* lease : {"14400", "0"}) {
                    int status = Soap(control, type, "AddPortMapping",
                                      args_head + "<NewLeaseDuration>" + lease + "</NewLeaseDuration>", body);
                    Log("upnp: AddPortMapping via %s (lease %s) -> HTTP %d %s", control.c_str(), lease, status,
                        Tag(body, "errorDescription").c_str());
                    if (status == 200) {
                        r.done = r.mapped = true;
                        {
                            std::lock_guard<std::mutex> lock(g_mutex);
                            g_control_url = control;
                            g_service = type;
                        }
                        SetResult(r);
                        return;
                    }
                    std::string code = Tag(body, "errorCode");
                    if (code == "718")
                        last_error = "Port " + port_text + " is already opened on your router for another device.";
                    else if (!code.empty())
                        last_error = "Your router refused to open port " + port_text + " (error " + code + ").";
                }
            }
        }
    }
    r.done = true;
    r.error = last_error;
    SetResult(r);
}

}  // namespace

DWORD WINAPI MapThreadMain(void* arg)
{
    MapThread(static_cast<uint16_t>(reinterpret_cast<uintptr_t>(arg)));
    return 0;
}

void WaitForThread()
{
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

void StartMapping(uint16_t port)
{
    WaitForThread();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_result = Result();
        g_port = port;
        g_control_url.clear();
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_thread = CreateThread(nullptr, 0, MapThreadMain, reinterpret_cast<void*>(static_cast<uintptr_t>(port)), 0, nullptr);
}

Result GetResult()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_result;
}

void RemoveMapping()
{
    WaitForThread();
    std::string control, service;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        control.swap(g_control_url);
        service = g_service;
    }
    if (control.empty())
        return;
    std::string body;
    int status = Soap(control, service, "DeletePortMapping",
                      "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(g_port) +
                          "</NewExternalPort><NewProtocol>TCP</NewProtocol>",
                      body);
    Log("upnp: DeletePortMapping -> HTTP %d", status);
}

}  // namespace upnp
