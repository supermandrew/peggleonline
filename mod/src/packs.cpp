#include "packs.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game.h"
#include "log.h"
#include "net.h"
#include "protocol.h"

namespace packs {

namespace {

constexpr uint32_t kPakMagic = 0xBAC04AC0;
constexpr uint8_t kPakXor = 0xF7;
constexpr uint8_t kPakEndOfTable = 0x80;
constexpr size_t kMaxTable = 8 << 20;
constexpr uint32_t kPiece = 16 * 1024;  // small: the game thread's own messages wait their turn behind each one
constexpr size_t kMaxFileName = 60;

std::string Folder() { return game::GameFolder() + "\\levelpacks\\"; }

std::string Lowered(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(c == '\\' ? '/' : tolower(static_cast<unsigned char>(c)));
    return s;
}

bool EndsWith(const std::string& s, const std::string& end)
{
    return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}

uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t size)
{
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit)
                c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
            t[i] = c;
        }
        return t;
    }();
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

// The files a pack holds, as lower-case paths with forward slashes. (A .pak is XORed through and through: magic,
// version, then per file flags, name length, name, size and time, then all the files' bytes.)
bool ReadTable(const std::string& path, uint32_t file_size, std::vector<std::string>& names)
{
    size_t n = file_size < kMaxTable ? file_size : kMaxTable;
    std::vector<uint8_t> d(n);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    size_t got = fread(d.data(), 1, n, f);
    fclose(f);
    if (got != n || n < 9)
        return false;
    for (uint8_t& b : d)
        b ^= kPakXor;
    uint32_t magic;
    memcpy(&magic, d.data(), 4);
    if (magic != kPakMagic)
        return false;
    uint64_t data_size = 0;
    size_t pos = 8;
    for (;;) {
        if (pos >= n)
            return false;
        uint8_t flags = d[pos++];
        if (flags & kPakEndOfTable)
            break;
        if (pos >= n)
            return false;
        size_t length = d[pos++];
        if (pos + length + 12 > n)
            return false;
        names.push_back(Lowered(std::string(reinterpret_cast<const char*>(d.data()) + pos, length)));
        pos += length;
        uint32_t size;
        memcpy(&size, d.data() + pos, 4);
        pos += 12;
        data_size += size;
    }
    return pos + data_size <= file_size;
}

// What makes a file a level pack the game can load, and keeps it to itself.
bool LooksLikeAPack(const std::string& path, const std::string& file, uint32_t size)
{
    std::vector<std::string> names;
    if (!ReadTable(path, size, names))
        return false;
    std::string settings = Lowered(file.substr(0, file.size() - 4)) + ".cfg";  // (the game looks for this)
    bool has_settings = false, has_level = false;
    for (const std::string& name : names) {
        if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos ||
            name.find(':') != std::string::npos)
            return false;
        has_settings |= name == settings;
        has_level |= EndsWith(name, ".dat");
    }
    return has_settings && has_level;
}

std::atomic<bool> g_stop_sending{false};
std::atomic<uint32_t> g_sent{0};
std::atomic<int> g_throttle{0};
std::atomic<bool> g_corrupt{false};
HANDLE g_sender;  // not std::thread: see net.cpp

struct SendJob {
    std::string path;
    uint32_t size;
};

DWORD WINAPI SendThread(void* arg)
{
    SendJob job = *static_cast<SendJob*>(arg);
    delete static_cast<SendJob*>(arg);
    HANDLE f = CreateFileA(job.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    uint32_t offset = 0, crc = 0xFFFFFFFF;
    if (f != INVALID_HANDLE_VALUE) {
        std::vector<uint8_t> piece(sizeof(uint32_t) + kPiece);
        DWORD n = 0;
        while (!g_stop_sending && ReadFile(f, piece.data() + 4, kPiece, &n, nullptr) && n) {
            memcpy(piece.data(), &offset, 4);
            crc = Crc32(crc, piece.data() + 4, n);
            if (g_corrupt)
                piece[4] ^= 1;
            if (!net::Send(proto::kPackData, piece.data(), 4 + n))
                break;
            offset += n;
            g_sent = offset;
            int pause = g_throttle;
            if (pause)
                Sleep(pause);
        }
        CloseHandle(f);
    }
    if (g_stop_sending)
        return 0;
    if (offset == job.size) {
        proto::PackEnd end = {~crc};
        net::Send(proto::kPackEnd, &end, sizeof end);
    } else {
        Log("packs: could not read %s (%u of %u bytes)", job.path.c_str(), offset, job.size);
        net::Send(proto::kPackCancel, nullptr, 0);
    }
    return 0;
}

HANDLE g_incoming = INVALID_HANDLE_VALUE;
Info g_incoming_pack;
std::string g_incoming_path;
uint32_t g_received, g_received_crc;

}  // namespace

bool FindWithLevel(const std::string& level_id, Info& out)
{
    std::string want = "/" + Lowered(level_id) + ".dat";
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA((Folder() + "*.pak").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE)
        return false;
    bool hit = false;
    do {
        std::vector<std::string> names;
        if (found.nFileSizeHigh || !ReadTable(Folder() + found.cFileName, found.nFileSizeLow, names))
            continue;
        for (const std::string& name : names)
            hit |= EndsWith(name, want);
        if (hit) {
            out.file = found.cFileName;
            out.size = found.nFileSizeLow;
        }
    } while (!hit && FindNextFileA(search, &found));
    FindClose(search);
    return hit;
}

bool Acceptable(const Info& offered, std::string& why)
{
    const std::string& name = offered.file;
    bool clean = name.size() > 4 && name.size() <= kMaxFileName && name[0] != '.' && EndsWith(Lowered(name), ".pak");
    for (char c : name)
        clean &= isalnum(static_cast<unsigned char>(c)) || strchr(" ._-()", c) != nullptr;
    if (!clean)
        why = "its file name isn't one to save";
    else if (offered.size == 0 || offered.size > kMaxSize)
        why = "its size";
    else if (GetFileAttributesA((Folder() + name).c_str()) != INVALID_FILE_ATTRIBUTES)
        why = "there is a pack by that name here already";
    else
        return true;
    return false;
}

std::string SizeText(uint32_t bytes)
{
    char text[32];
    snprintf(text, sizeof text, "%.1f MB", bytes / (1024.0 * 1024.0));
    return text;
}

void StartSending(const Info& pack)
{
    StopSending();
    WaitForSender();
    g_stop_sending = false;
    g_sent = 0;
    g_sender = CreateThread(nullptr, 0, SendThread, new SendJob{Folder() + pack.file, pack.size}, 0, nullptr);
    Log("packs: sending %s (%u bytes)", pack.file.c_str(), pack.size);
}

void StopSending() { g_stop_sending = true; }

void WaitForSender()
{
    if (g_sender) {
        WaitForSingleObject(g_sender, 2000);
        CloseHandle(g_sender);
        g_sender = nullptr;
    }
}

bool Sending() { return g_sender && WaitForSingleObject(g_sender, 0) == WAIT_TIMEOUT; }
uint32_t BytesSent() { return g_sent; }
void SetThrottle(int milliseconds) { g_throttle = milliseconds; }
void SetCorrupt(bool on) { g_corrupt = on; }

bool StartReceiving(const Info& pack)
{
    StopReceiving();
    g_incoming_pack = pack;
    g_incoming_path = Folder() + pack.file + ".part";
    g_incoming = CreateFileA(g_incoming_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    g_received = 0;
    g_received_crc = 0xFFFFFFFF;
    if (g_incoming == INVALID_HANDLE_VALUE) {
        Log("packs: could not write in %s (error %lu)", g_incoming_path.c_str(), GetLastError());
        return false;
    }
    return true;
}

bool Receiving() { return g_incoming != INVALID_HANDLE_VALUE; }
uint32_t BytesReceived() { return g_received; }

bool ReceivePiece(const uint8_t* message, size_t size)
{
    if (!Receiving() || size <= 4)
        return false;
    uint32_t offset;
    memcpy(&offset, message, 4);
    const uint8_t* bytes = message + 4;
    DWORD n = static_cast<DWORD>(size - 4), written = 0;
    if (offset != g_received || static_cast<uint64_t>(offset) + n > g_incoming_pack.size)
        return false;
    if (!WriteFile(g_incoming, bytes, n, &written, nullptr) || written != n) {
        Log("packs: could not write %s (error %lu)", g_incoming_path.c_str(), GetLastError());
        return false;
    }
    g_received_crc = Crc32(g_received_crc, bytes, n);
    g_received += n;
    return true;
}

bool FinishReceiving(uint32_t crc)
{
    if (!Receiving())
        return false;
    CloseHandle(g_incoming);
    g_incoming = INVALID_HANDLE_VALUE;
    std::string final_path = Folder() + g_incoming_pack.file;
    bool whole = g_received == g_incoming_pack.size && ~g_received_crc == crc;
    bool pack = whole && LooksLikeAPack(g_incoming_path, g_incoming_pack.file, g_received);
    if (pack && MoveFileA(g_incoming_path.c_str(), final_path.c_str())) {
        Log("packs: %s installed (%u bytes)", g_incoming_pack.file.c_str(), g_received);
        return true;
    }
    Log("packs: %s not installed (%u of %u bytes, whole %d, a pack %d)", g_incoming_pack.file.c_str(), g_received,
        g_incoming_pack.size, whole ? 1 : 0, pack ? 1 : 0);
    DeleteFileA(g_incoming_path.c_str());
    return false;
}

void StopReceiving()
{
    if (!Receiving())
        return;
    CloseHandle(g_incoming);
    g_incoming = INVALID_HANDLE_VALUE;
    DeleteFileA(g_incoming_path.c_str());
}

}  // namespace packs
