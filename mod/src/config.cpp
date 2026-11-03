#include "config.h"

#include <windows.h>

#include "log.h"
#include "net.h"
#include "pad.h"

namespace config {

namespace {

std::string IniPath()
{
    CreateDirectoryA((ModDir() + "mods").c_str(), nullptr);
    return ModDir() + "mods\\netplay.ini";
}

// Controller settings are read every frame, so they are kept in memory and written when they change.
const char* const kActionKeys[kPadActionCount] = {"shoot", "back", "chat", "replay", "speed_up", "menu", "flippers"};
constexpr uint32_t Bit(int button) { return 1u << button; }
const uint32_t kDefaults[kPadActionCount] = {
    Bit(pad::kA), Bit(pad::kB), Bit(pad::kY), Bit(pad::kX), Bit(pad::kRT), Bit(pad::kStart),
    Bit(pad::kLB) | Bit(pad::kRB),
};

bool g_pad_loaded;
uint32_t g_bindings[kPadActionCount];
bool g_vibration;
int g_aim_speed;
bool g_players_swapped;
int g_borderless = -1;  // not read yet
int g_chaos = -1;
int g_red_peg = -1;

// "LB+RB" <-> bits
uint32_t ParseButtons(const std::string& text)
{
    uint32_t bits = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('+', start);
        if (end == std::string::npos)
            end = text.size();
        int b = pad::ButtonByKey(text.substr(start, end - start));
        if (b >= 0)
            bits |= Bit(b);
        start = end + 1;
    }
    return bits;
}

std::string FormatButtons(uint32_t bits)
{
    std::string text;
    for (int b = 0; b < pad::kButtonCount; ++b) {
        if (!(bits & Bit(b)))
            continue;
        if (!text.empty())
            text += "+";
        text += pad::ButtonKey(b);
    }
    return text;
}

void LoadPad()
{
    if (g_pad_loaded)
        return;
    g_pad_loaded = true;
    std::string ini = IniPath();
    for (int a = 0; a < kPadActionCount; ++a) {
        char buf[64];
        GetPrivateProfileStringA("controller", kActionKeys[a], "", buf, sizeof buf, ini.c_str());
        uint32_t bits = ParseButtons(buf);
        g_bindings[a] = bits ? bits : kDefaults[a];
    }
    g_vibration = GetPrivateProfileIntA("controller", "vibration", 1, ini.c_str()) != 0;
    int speed = GetPrivateProfileIntA("controller", "aim_speed", kAimNormal, ini.c_str());
    g_aim_speed = speed >= 0 && speed < kAimSpeedCount ? speed : kAimNormal;
    g_players_swapped = GetPrivateProfileIntA("controller", "swap_players", 0, ini.c_str()) != 0;
}

void WritePad(const char* key, const std::string& value)
{
    WritePrivateProfileStringA("controller", key, value.c_str(), IniPath().c_str());
}

}  // namespace

std::string LastAddress()
{
    char buf[256];
    GetPrivateProfileStringA("netplay", "last_address", "", buf, sizeof buf, IniPath().c_str());
    return buf;
}

void SetLastAddress(const std::string& address)
{
    WritePrivateProfileStringA("netplay", "last_address", address.c_str(), IniPath().c_str());
}

uint16_t Port()
{
    UINT port = GetPrivateProfileIntA("netplay", "port", net::kDefaultPort, IniPath().c_str());
    return port > 0 && port < 65536 ? static_cast<uint16_t>(port) : net::kDefaultPort;
}

uint32_t PadBinding(int action)
{
    LoadPad();
    return g_bindings[action];
}

uint32_t DefaultPadBinding(int action) { return kDefaults[action]; }

void SetPadBinding(int action, uint32_t buttons)
{
    LoadPad();
    g_bindings[action] = buttons;
    WritePad(kActionKeys[action], FormatButtons(buttons));
}

bool PadVibration()
{
    LoadPad();
    return g_vibration;
}

void SetPadVibration(bool on)
{
    LoadPad();
    g_vibration = on;
    WritePad("vibration", on ? "1" : "0");
}

int PadAimSpeed()
{
    LoadPad();
    return g_aim_speed;
}

void SetPadAimSpeed(int speed)
{
    LoadPad();
    g_aim_speed = speed;
    WritePad("aim_speed", std::to_string(speed));
}

bool Borderless()
{
    if (g_borderless < 0)
        g_borderless = GetPrivateProfileIntA("display", "borderless", 0, IniPath().c_str()) != 0;
    return g_borderless != 0;
}

void SetBorderless(bool on)
{
    if (Borderless() == on)
        return;
    g_borderless = on;
    WritePrivateProfileStringA("display", "borderless", on ? "1" : "0", IniPath().c_str());
}

bool ChaosMode()
{
    if (g_chaos < 0)
        g_chaos = GetPrivateProfileIntA("duel", "chaos", 0, IniPath().c_str()) != 0;
    return g_chaos != 0;
}

void SetChaosMode(bool on)
{
    if (ChaosMode() == on)
        return;
    g_chaos = on;
    WritePrivateProfileStringA("duel", "chaos", on ? "1" : "0", IniPath().c_str());
}

bool RedPegMode()
{
    if (g_red_peg < 0)
        g_red_peg = GetPrivateProfileIntA("duel", "red", 0, IniPath().c_str()) != 0;
    return g_red_peg != 0;
}

void SetRedPegMode(bool on)
{
    if (RedPegMode() == on)
        return;
    g_red_peg = on;
    WritePrivateProfileStringA("duel", "red", on ? "1" : "0", IniPath().c_str());
}

bool PadPlayersSwapped()
{
    LoadPad();
    return g_players_swapped;
}

void SetPadPlayersSwapped(bool swapped)
{
    LoadPad();
    g_players_swapped = swapped;
    WritePad("swap_players", swapped ? "1" : "0");
}

}  // namespace config
