#include "padhid.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <vector>

#include "log.h"
#include "pad.h"

namespace padhid {

namespace {

constexpr USHORT kSony = 0x054C;
const USHORT kDualShock4s[] = {0x05C4, 0x09CC, 0x0BA0};  // the first one, the second, the wireless adapter
const USHORT kDualSenses[] = {0x0CE6, 0x0DF2};           // DualSense, DualSense Edge
constexpr size_t kUsbReport = 64;   // an input report's length over USB; longer over Bluetooth
constexpr size_t kMaxReport = 600;  // (a DualShock 4 over Bluetooth: 547)
constexpr size_t kBluetoothOut = 78;  // an output report over Bluetooth, its check at the end
constexpr DWORD kScanMs = 2000;
constexpr int kReportsPerFrame = 64;  // they come faster than the frames: the last one is the state

struct Device {
    std::wstring path;
    HANDLE file;
    bool dualsense, bluetooth;
    size_t in_length, out_length;  // out 0: it opened for reading only
    OVERLAPPED read, write;
    bool reading, writing;  // the buffer below is the system's until that ends
    uint8_t in[kMaxReport], out[kMaxReport];
    bool has_state;
    State state;
    uint8_t want[2], sent[2];  // heavy and light motor
    uint32_t reports, writes, write_fails;
};
Device* g_devices[kMax];  // the game thread's

// Between the threads.
std::mutex g_mutex;
std::vector<std::wstring> g_open;   // paths of the devices opened and not closed again
std::vector<Device*> g_incoming;    // opened, for the game thread to take
volatile LONG g_wanted;
HANDLE g_thread;  // not std::thread: see net.cpp

bool Listed(const USHORT* ids, size_t n, USHORT id)
{
    for (size_t i = 0; i < n; ++i)
        if (ids[i] == id)
            return true;
    return false;
}

void Consider(const wchar_t* path)
{
    std::wstring lower = path;
    for (wchar_t& c : lower)
        c = static_cast<wchar_t>(towlower(c));
    if (lower.find(L"054c") == std::wstring::npos)  // (Sony's number: USB "vid_054c", Bluetooth "vid&0002054c")
        return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const std::wstring& p : g_open)
            if (p == lower)
                return;
    }
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    HANDLE probe = CreateFileW(path, 0, share, nullptr, OPEN_EXISTING, 0, nullptr);
    if (probe == INVALID_HANDLE_VALUE)
        return;
    HIDD_ATTRIBUTES attr = {};
    attr.Size = sizeof attr;
    PHIDP_PREPARSED_DATA data = nullptr;
    HIDP_CAPS caps = {};
    bool known = HidD_GetAttributes(probe, &attr) && HidD_GetPreparsedData(probe, &data);
    if (data) {
        known = known && HidP_GetCaps(data, &caps) == HIDP_STATUS_SUCCESS;
        HidD_FreePreparsedData(data);
    }
    CloseHandle(probe);
    bool dualsense = Listed(kDualSenses, _countof(kDualSenses), attr.ProductID);
    bool dualshock = Listed(kDualShock4s, _countof(kDualShock4s), attr.ProductID);
    if (!known || attr.VendorID != kSony || (!dualsense && !dualshock))
        return;
    if (caps.UsagePage != HID_USAGE_PAGE_GENERIC ||
        (caps.Usage != HID_USAGE_GENERIC_GAMEPAD && caps.Usage != HID_USAGE_GENERIC_JOYSTICK))
        return;
    if (caps.InputReportByteLength < 10 || caps.InputReportByteLength > kMaxReport ||
        caps.OutputReportByteLength > kMaxReport)
        return;
    bool writable = true;
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, share, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        writable = false;
        file = CreateFileW(path, GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) {
        Log("pad: can't open a %s (error %lu)", dualsense ? "DualSense" : "DualShock 4", GetLastError());
        return;
    }
    Device* d = new Device();
    d->path = lower;
    d->file = file;
    d->dualsense = dualsense;
    d->bluetooth = caps.InputReportByteLength != kUsbReport;
    d->in_length = caps.InputReportByteLength;
    d->out_length = writable ? caps.OutputReportByteLength : 0;
    d->read.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    d->write.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Log("pad: %s found (%s, %u/%u byte reports%s)", dualsense ? "DualSense" : "DualShock 4",
        d->bluetooth ? "Bluetooth" : "USB", caps.InputReportByteLength, caps.OutputReportByteLength,
        writable ? "" : ", no vibration");
    std::lock_guard<std::mutex> lock(g_mutex);
    g_open.push_back(lower);
    g_incoming.push_back(d);
}

void Scan()
{
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return;
    SP_DEVICE_INTERFACE_DATA iface = {};
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &iface); ++i) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
            continue;
        std::vector<char> buffer(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof *detail;
        if (SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, nullptr))
            Consider(detail->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(set);
}

DWORD WINAPI ScanThread(void*)
{
    for (;;) {
        if (g_wanted)
            Scan();
        Sleep(kScanMs);
    }
}

uint32_t Crc32(uint32_t crc, const uint8_t* p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        for (int bit = 0; bit < 8; ++bit)
            crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return crc;
}

void SendMotors(Device& d)
{
    if (d.writing) {
        DWORD done = 0;
        if (!GetOverlappedResult(d.file, &d.write, &done, FALSE)) {
            if (GetLastError() == ERROR_IO_INCOMPLETE)
                return;
            ++d.write_fails;
        }
        d.writing = false;
    }
    if (!d.out_length || (d.want[0] == d.sent[0] && d.want[1] == d.sent[1]))
        return;
    d.sent[0] = d.want[0];
    d.sent[1] = d.want[1];
    uint8_t heavy = d.want[0], light = d.want[1];
    memset(d.out, 0, sizeof d.out);
    // Only the motors' bits are set: the light bar and the triggers stay as they are (DSX may be looking after them).
    if (d.dualsense) {
        size_t at = 1;
        if (d.bluetooth) {
            d.out[0] = 0x31;
            d.out[1] = 0x02;
            at = 2;
        } else {
            d.out[0] = 0x02;
        }
        d.out[at] = 0x03;  // motors as on the older controllers, not its audio haptics
        d.out[at + 2] = light;
        d.out[at + 3] = heavy;
    } else if (d.bluetooth) {
        d.out[0] = 0x11;
        d.out[1] = 0xC4;
        d.out[3] = 0x01;
        d.out[6] = light;
        d.out[7] = heavy;
    } else {
        d.out[0] = 0x05;
        d.out[1] = 0x01;
        d.out[4] = light;
        d.out[5] = heavy;
    }
    if (d.bluetooth) {
        if (d.out_length < kBluetoothOut)
            return;
        const uint8_t header = 0xA2;
        uint32_t crc = ~Crc32(Crc32(0xFFFFFFFFu, &header, 1), d.out, kBluetoothOut - 4);
        memcpy(d.out + kBluetoothOut - 4, &crc, 4);
    }
    ++d.writes;
    ResetEvent(d.write.hEvent);
    DWORD done = 0;
    if (WriteFile(d.file, d.out, static_cast<DWORD>(d.out_length), &done, &d.write))
        return;
    if (GetLastError() == ERROR_IO_PENDING)
        d.writing = true;
    else
        ++d.write_fails;
}

bool Read(Device& d)  // false: it's gone
{
    for (int i = 0; i < kReportsPerFrame; ++i) {
        DWORD got = 0;
        if (!d.reading) {
            ResetEvent(d.read.hEvent);
            if (!ReadFile(d.file, d.in, static_cast<DWORD>(d.in_length), &got, &d.read)) {
                if (GetLastError() != ERROR_IO_PENDING)
                    return false;
                d.reading = true;
            }
        }
        if (d.reading) {
            if (!GetOverlappedResult(d.file, &d.read, &got, FALSE))
                return GetLastError() == ERROR_IO_INCOMPLETE;  // (nothing new yet: it stays asked for)
            d.reading = false;
        }
        State st;
        if (ParseReport(d.in, got, d.dualsense, d.in_length, st)) {
            d.state = st;
            d.has_state = true;
            ++d.reports;
        }
    }
    return true;
}

void Close(Device* d)
{
    d->want[0] = d->want[1] = 0;
    SendMotors(*d);  // it mustn't go on shaking
    CancelIoEx(d->file, &d->read);
    DWORD done;
    if (d->reading)
        GetOverlappedResult(d->file, &d->read, &done, TRUE);
    if (d->writing)
        GetOverlappedResult(d->file, &d->write, &done, TRUE);
    CloseHandle(d->read.hEvent);
    CloseHandle(d->write.hEvent);
    CloseHandle(d->file);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (size_t i = 0; i < g_open.size(); ++i)
            if (g_open[i] == d->path) {
                g_open.erase(g_open.begin() + i);
                break;
            }
    }
    delete d;
}

}  // namespace

void Update(bool wanted)
{
    InterlockedExchange(&g_wanted, wanted ? 1 : 0);
    if (wanted && !g_thread)
        g_thread = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
    std::vector<Device*> incoming;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        incoming.swap(g_incoming);
    }
    for (Device* d : incoming) {
        int free_slot = -1;
        for (int i = kMax - 1; i >= 0; --i)
            if (!g_devices[i])
                free_slot = i;
        if (wanted && free_slot >= 0)
            g_devices[free_slot] = d;
        else
            Close(d);
    }
    for (Device*& d : g_devices) {
        if (!d)
            continue;
        if (!wanted || !Read(*d)) {
            Close(d);
            d = nullptr;
            continue;
        }
        SendMotors(*d);
    }
}

bool Get(int pad, State& out)
{
    const Device* d = pad >= 0 && pad < kMax ? g_devices[pad] : nullptr;
    if (!d || !d->has_state)
        return false;
    out = d->state;
    return true;
}

void SetMotors(int pad, uint8_t heavy, uint8_t light)
{
    if (Device* d = pad >= 0 && pad < kMax ? g_devices[pad] : nullptr) {
        d->want[0] = heavy;
        d->want[1] = light;
    }
}

// The reports. Sticks, then a byte with the d-pad and the face buttons, one with the rest, and the triggers, either
// after those (a DualShock 4; a DualSense over Bluetooth until something asks for more) or between the sticks and the
// buttons, with a counter (a DualSense's full report). Over Bluetooth the fuller ones have an id of their own and a
// few bytes in front. input_length is the device's: only that tells a DualSense's two reports numbered 1 apart.
bool ParseReport(const uint8_t* r, size_t size, bool dualsense, size_t input_length, State& out)
{
    if (size < 1)
        return false;
    size_t at;
    bool triggers_first = false;
    if (r[0] == 0x01) {
        at = 1;
        triggers_first = dualsense && input_length == kUsbReport;
    } else if (r[0] == 0x11 && !dualsense && size > 1 && (r[1] & 0x80)) {
        at = 3;
    } else if (r[0] == 0x31 && dualsense) {
        at = 2;
        triggers_first = true;
    } else {
        return false;
    }
    if (size < at + 9)
        return false;
    const uint8_t* d = r + at;
    if (!d[0] && !d[1] && !d[2] && !d[3])
        return false;  // (a wireless adapter with no controller)
    const uint8_t* b = d + (triggers_first ? 7 : 4);
    static const uint32_t kHat[8] = {
        1u << pad::kUp,                      (1u << pad::kUp) | (1u << pad::kRight),
        1u << pad::kRight,                   (1u << pad::kDown) | (1u << pad::kRight),
        1u << pad::kDown,                    (1u << pad::kDown) | (1u << pad::kLeft),
        1u << pad::kLeft,                    (1u << pad::kUp) | (1u << pad::kLeft)};
    static const struct {
        int byte;
        uint8_t mask;
        int button;
    } kButtons[] = {
        {0, 0x10, pad::kX},  // square
        {0, 0x20, pad::kA},  // cross
        {0, 0x40, pad::kB},  // circle
        {0, 0x80, pad::kY},  // triangle
        {1, 0x01, pad::kLB},
        {1, 0x02, pad::kRB},
        {1, 0x10, pad::kBack},   // share / create
        {1, 0x20, pad::kStart},  // options
        {1, 0x40, pad::kLS},
        {1, 0x80, pad::kRS},
    };
    out.buttons = (b[0] & 0x0F) < 8 ? kHat[b[0] & 0x0F] : 0;
    for (const auto& k : kButtons)
        if (b[k.byte] & k.mask)
            out.buttons |= 1u << k.button;
    out.lx = d[0];
    out.ly = d[1];
    out.rx = d[2];
    out.ry = d[3];
    out.l2 = d[triggers_first ? 4 : 7];
    out.r2 = d[triggers_first ? 5 : 8];
    return true;
}

std::string DebugState()
{
    std::string out = "hid:";
    bool any = false;
    char buf[120];
    for (int i = 0; i < kMax; ++i) {
        const Device* d = g_devices[i];
        if (!d)
            continue;
        any = true;
        snprintf(buf, sizeof buf, " %d=%s,%s,in%u,out%u,reports=%u,writes=%u,fails=%u", i,
                 d->dualsense ? "dualsense" : "ds4", d->bluetooth ? "bt" : "usb", static_cast<unsigned>(d->in_length),
                 static_cast<unsigned>(d->out_length), d->reports, d->writes, d->write_fails);
        out += buf;
    }
    return any ? out : out + " none";
}

}  // namespace padhid
