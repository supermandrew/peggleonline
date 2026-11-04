#include "pad.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "game.h"
#include "log.h"
#include "padhid.h"

namespace pad {

namespace {

constexpr int kAppOffHwnd = 0x350;
constexpr float kLeftDeadZone = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE / 32767.0f;
constexpr float kRightDeadZone = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE / 32767.0f;
constexpr BYTE kTriggerDown = 128;  // a trigger counts as pressed past half way
constexpr DWORD kRescanMs = 2000;   // asking XInput about an empty slot is slow
constexpr int kSlots = XUSER_MAX_COUNT + padhid::kMax;  // XInput's four, then PlayStation controllers read directly
constexpr int kMouseSlack = 3;      // pixels the mouse may drift before it counts as moved
constexpr float kInUseStick = 0.3f;

const char* const kKeys[kButtonCount] = {"A",    "B",     "X",  "Y",  "LB", "RB",   "LT",   "RT",
                                         "BACK", "START", "LS", "RS", "UP", "DOWN", "LEFT", "RIGHT"};
const char* const kNames[kButtonCount] = {
    "A / CROSS",    "B / CIRCLE",      "X / SQUARE", "Y / TRIANGLE", "LB / L1",  "RB / R1",
    "LT / L2",      "RT / R2",         "BACK / SHARE", "START / OPTIONS", "LS / L3", "RS / R3",
    "D-PAD UP",     "D-PAD DOWN",      "D-PAD LEFT", "D-PAD RIGHT"};
const WORD kXInputBits[kButtonCount] = {
    XINPUT_GAMEPAD_A,          XINPUT_GAMEPAD_B,         XINPUT_GAMEPAD_X,         XINPUT_GAMEPAD_Y,
    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0,
    XINPUT_GAMEPAD_BACK,       XINPUT_GAMEPAD_START,     XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
    XINPUT_GAMEPAD_DPAD_UP,    XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT};

// Sounds that shake the controller (resource globals hold the sound ids; see re/NOTES.md).
struct SoundRumble {
    uintptr_t sound;
    const char* name;
    float low, high;  // the two motors: heavy and light
    int ms;
};
const SoundRumble kSoundRumbles[] = {
    {0x6CAA44, "CANNONSHOT", 0.30f, 0.45f, 110},  // the cannon fires
    {0x6CAA28, "PEGHIT", 0.00f, 0.20f, 35},       // a tick per peg
    {0x6CA710, "PEGHIT2", 0.00f, 0.20f, 35},
    {0x6CA780, "PEGHIT3", 0.00f, 0.20f, 35},
    {0x6CAB38, "PEGHIT_LOW", 0.00f, 0.15f, 35},
    {0x6CAB3C, "BUCKETHIT", 0.25f, 0.20f, 80},
    {0x6CA3D4, "FLIPPERBOUNCE", 0.20f, 0.25f, 60},
    {0x6CA6A4, "FLIPPERUP", 0.15f, 0.20f, 50},
    {0x6CA388, "FREEBALL", 0.45f, 0.35f, 220},    // into the bucket
    {0x6CA990, "EXTRABALL", 0.40f, 0.30f, 180},
    {0x6CA9D4, "EXTRABALL2", 0.40f, 0.30f, 180},
    {0x6CAA8C, "EXTRABALL3", 0.40f, 0.30f, 180},
    {0x6CA8B8, "POWERUP", 0.40f, 0.40f, 250},     // a master's power
    {0x6CA8A4, "MULTIBALL", 0.40f, 0.40f, 250},
    {0x6CAB8C, "FIREBALLSHOT", 0.60f, 0.50f, 250},
    {0x6CAB54, "LIGHTNING_HIT", 0.50f, 0.60f, 200},
    {0x6CA89C, "EXPLODE", 0.85f, 0.60f, 300},
    {0x6CA850, "SPACEBLAST", 0.85f, 0.60f, 350},
    {0x6CA810, "SLOWMO", 0.25f, 0.10f, 900},      // the last orange peg is in reach
    {0x6CA954, "FEVERHIT", 1.00f, 1.00f, 700},    // EXTREME FEVER
    {0x6CAADC, "CYMBAL", 0.60f, 0.50f, 400},
    {0x6CA8E0, "GONG", 0.60f, 0.50f, 400},
    {0x6CAAF4, "FIREWORKS1", 0.50f, 0.30f, 120},
    {0x6CAB14, "FIREWORKS2", 0.50f, 0.30f, 120},
    {0x6CA930, "LEVELWON", 0.50f, 0.40f, 400},
};

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
GetStateFn g_get_state;
SetStateFn g_set_state;
const char* g_dll;
bool g_load_tried;

struct Controller {
    bool connected;
    uint32_t down, prev;  // buttons this frame and the last
    Stick left, right;
};
Controller g_pads[kSlots];  // by slot (testing: the pretend ones)
DWORD g_last_scan[XUSER_MAX_COUNT];
int g_order[kSlots];  // the slots of the connected controllers, in order
int g_count;
int g_selected = kEveryone;
const Controller kNoController = {};
// Every controller together. Two controllers can be the same one seen twice (a remapper's virtual copy, a frame
// behind): a press counts once.
uint32_t g_all_down, g_all_prev;
Stick g_all_left, g_all_right;

bool g_in_use;
POINT g_mouse_at_use;  // where the pointer was when the controller took over

bool g_test;
Controller g_test_pads[kSlots];  // as the test pipe last set them

struct Effect {
    float low, high;
    DWORD start;
    int ms;
    uint32_t slots;  // the controllers it shakes
};
Effect g_effects[8];
WORD g_motors[kSlots][2];  // heavy and light motor, as each controller was last told
int g_sound_target = kEveryone;
std::string g_last_rumble = "none";
int g_last_rumble_target = kEveryone;
int g_rumbles;

void Load()
{
    if (g_load_tried)
        return;
    g_load_tried = true;
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
        HMODULE m = LoadLibraryA(dll);
        if (!m)
            continue;
        g_get_state = reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"));
        g_set_state = reinterpret_cast<SetStateFn>(GetProcAddress(m, "XInputSetState"));
        if (g_get_state) {
            g_dll = dll;
            // Steam Input shows PlayStation and other controllers to the game as an Xbox one through its overlay.
            Log("pad: using %s; Steam overlay %s", dll,
                GetModuleHandleA("gameoverlayrenderer.dll") ? "loaded" : "not loaded (no Steam Input in the game)");
            return;
        }
    }
    Log("pad: no XInput on this computer; only PlayStation controllers will work");
}

bool GameInFront()
{
    void* app = game::App();
    return app && GetForegroundWindow() == game::At<HWND>(app, kAppOffHwnd);
}

SHORT Axis(uint8_t v) { return static_cast<SHORT>((v - 127.5f) / 127.5f * 32767); }  // a HID report's 0..255

Stick DeadZone(SHORT x, SHORT y, float zone)
{
    float fx = x / 32767.0f, fy = y / 32767.0f;
    float mag = std::sqrt(fx * fx + fy * fy);
    if (mag <= zone)
        return {0, 0};
    float scaled = std::min(1.0f, (mag - zone) / (1 - zone));
    return {fx / mag * scaled, fy / mag * scaled};
}

float Length(Stick s) { return std::sqrt(s.x * s.x + s.y * s.y); }

void ReadControllers()
{
    Load();
    DWORD now = GetTickCount();
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        Controller& c = g_pads[i];
        c.down = 0;
        c.left = c.right = {0, 0};
        if (!g_get_state || (!c.connected && now - g_last_scan[i] < kRescanMs))
            continue;
        g_last_scan[i] = now;
        XINPUT_STATE st = {};
        bool was = c.connected;
        c.connected = g_get_state(i, &st) == ERROR_SUCCESS;
        if (c.connected != was)
            Log("pad: controller %lu %s", i + 1, c.connected ? "connected" : "disconnected");
        if (!c.connected) {
            g_motors[i][0] = g_motors[i][1] = 0;  // it starts out still when it comes back
            continue;
        }
        const XINPUT_GAMEPAD& gp = st.Gamepad;
        for (int b = 0; b < kButtonCount; ++b)
            if (kXInputBits[b] && (gp.wButtons & kXInputBits[b]))
                c.down |= 1u << b;
        if (gp.bLeftTrigger >= kTriggerDown)
            c.down |= 1u << kLT;
        if (gp.bRightTrigger >= kTriggerDown)
            c.down |= 1u << kRT;
        c.left = DeadZone(gp.sThumbLX, gp.sThumbLY, kLeftDeadZone);
        c.right = DeadZone(gp.sThumbRX, gp.sThumbRY, kRightDeadZone);
    }
    // PlayStation controllers, read directly, only while XInput has none: an Xbox controller there is usually one of
    // them seen twice (DSX, Steam Input and DS4Windows all make one), and one person would be two players.
    bool xinput = false;
    for (int i = 0; i < XUSER_MAX_COUNT; ++i)
        xinput |= g_pads[i].connected;
    padhid::Update(!xinput);
    for (int i = 0; i < padhid::kMax; ++i) {
        int slot = XUSER_MAX_COUNT + i;
        Controller& c = g_pads[slot];
        padhid::State st;
        bool was = c.connected;
        c.connected = !xinput && padhid::Get(i, st);
        c.down = 0;
        c.left = c.right = {0, 0};
        if (c.connected != was)
            Log("pad: PlayStation controller %d %s", i + 1, c.connected ? "connected" : "disconnected");
        if (!c.connected) {
            g_motors[slot][0] = g_motors[slot][1] = 0;
            continue;
        }
        c.down = st.buttons;
        if (st.l2 >= kTriggerDown)
            c.down |= 1u << kLT;
        if (st.r2 >= kTriggerDown)
            c.down |= 1u << kRT;
        c.left = DeadZone(Axis(st.lx), Axis(255 - st.ly), kLeftDeadZone);
        c.right = DeadZone(Axis(st.rx), Axis(255 - st.ry), kRightDeadZone);
    }
}

const Controller* Chosen()  // null: every controller together
{
    if (g_selected == kEveryone)
        return nullptr;
    return g_selected >= 0 && g_selected < g_count ? &g_pads[g_order[g_selected]] : &kNoController;
}

uint32_t Slots(int controller)
{
    if (controller == kEveryone)
        return (1u << kSlots) - 1;
    return controller >= 0 && controller < g_count ? 1u << g_order[controller] : 0;
}

void SetMotors(int slot, float low, float high)
{
    WORD l = static_cast<WORD>(std::min(1.0f, low) * 65535), h = static_cast<WORD>(std::min(1.0f, high) * 65535);
    if (!g_pads[slot].connected || (l == g_motors[slot][0] && h == g_motors[slot][1]))
        return;
    g_motors[slot][0] = l;
    g_motors[slot][1] = h;
    if (g_test)
        return;
    if (slot >= XUSER_MAX_COUNT) {
        padhid::SetMotors(slot - XUSER_MAX_COUNT, static_cast<uint8_t>(l >> 8), static_cast<uint8_t>(h >> 8));
    } else if (g_set_state) {
        XINPUT_VIBRATION v = {l, h};
        g_set_state(slot, &v);
    }
}

void UpdateRumble()
{
    DWORD now = GetTickCount();
    float low[kSlots] = {}, high[kSlots] = {};
    for (Effect& e : g_effects) {
        if (e.ms <= 0)
            continue;
        int age = static_cast<int>(now - e.start);
        if (age >= e.ms) {
            e.ms = 0;
            continue;
        }
        float fade = std::min(1.0f, (e.ms - age) / (0.4f * e.ms));  // full strength, then the last 40% fades
        for (int i = 0; i < kSlots; ++i) {
            if (e.slots & (1u << i)) {
                low[i] = std::max(low[i], e.low * fade);
                high[i] = std::max(high[i], e.high * fade);
            }
        }
    }
    bool allowed = g_in_use && config::PadVibration() && (g_test || GameInFront());
    for (int i = 0; i < kSlots; ++i)
        SetMotors(i, allowed ? low[i] : 0, allowed ? high[i] : 0);
}

}  // namespace

const char* ButtonKey(int button) { return button >= 0 && button < kButtonCount ? kKeys[button] : "?"; }
const char* ButtonName(int button) { return button >= 0 && button < kButtonCount ? kNames[button] : "?"; }

int ButtonByKey(const std::string& key)
{
    for (int b = 0; b < kButtonCount; ++b)
        if (_stricmp(key.c_str(), kKeys[b]) == 0)
            return b;
    return -1;
}

void Update()
{
    for (Controller& c : g_pads)
        c.prev = c.down;
    g_all_prev = g_all_down;
    if (g_test) {
        for (int i = 0; i < kSlots; ++i) {
            uint32_t prev = g_pads[i].prev;
            g_pads[i] = g_test_pads[i];
            g_pads[i].prev = prev;
        }
    } else {
        ReadControllers();
        if (!GameInFront()) {  // the game in the background ignores the controllers, like the keyboard
            for (Controller& c : g_pads) {
                c.down = 0;
                c.left = c.right = {0, 0};
            }
        }
    }
    g_count = 0;
    g_all_down = 0;
    g_all_left = g_all_right = {0, 0};
    bool input = false;
    for (int i = 0; i < kSlots; ++i) {
        const Controller& c = g_pads[i];
        if (!c.connected)
            continue;
        g_order[g_count++] = i;
        g_all_down |= c.down;
        if (Length(c.left) > Length(g_all_left))
            g_all_left = c.left;
        if (Length(c.right) > Length(g_all_right))
            g_all_right = c.right;
        input |= (c.down & ~c.prev) != 0 || Length(c.left) > kInUseStick || Length(c.right) > kInUseStick;
    }
    if (input && !g_in_use) {
        g_in_use = true;
        GetCursorPos(&g_mouse_at_use);
    }
    MouseTookOver();
    UpdateRumble();
}

bool Connected() { return g_count > 0; }
int Count() { return g_count; }
void Select(int controller) { g_selected = controller; }

uint32_t DownBits()
{
    const Controller* c = Chosen();
    return c ? c->down : g_all_down;
}

uint32_t PressedBits()
{
    const Controller* c = Chosen();
    return c ? c->down & ~c->prev : g_all_down & ~g_all_prev;
}

bool Down(int button) { return (DownBits() >> button) & 1; }
bool Pressed(int button) { return (PressedBits() >> button) & 1; }

Stick Left()
{
    const Controller* c = Chosen();
    return c ? c->left : g_all_left;
}

Stick Right()
{
    const Controller* c = Chosen();
    return c ? c->right : g_all_right;
}

bool InUse() { return g_in_use; }

bool MouseTookOver()
{
    if (!g_in_use || g_test)
        return false;
    POINT p;
    if (!GetCursorPos(&p) || (std::abs(p.x - g_mouse_at_use.x) <= kMouseSlack &&
                              std::abs(p.y - g_mouse_at_use.y) <= kMouseSlack))
        return false;
    g_in_use = false;
    return true;
}

void NoteMouseUsed() { g_in_use = false; }

void Rumble(float low, float high, int ms, int controller)
{
    uint32_t slots = Slots(controller);
    if (!slots)
        return;
    Effect* slot = nullptr;
    for (Effect& e : g_effects)
        if (!slot && e.ms <= 0)
            slot = &e;
    if (!slot) {  // all busy: replace the one ending first
        slot = &g_effects[0];
        for (Effect& e : g_effects)
            if (e.start + e.ms < slot->start + slot->ms)
                slot = &e;
    }
    *slot = {low, high, GetTickCount(), ms, slots};
}

void OnSound(unsigned sound_id)
{
    for (const SoundRumble& s : kSoundRumbles) {
        if (*reinterpret_cast<const int*>(s.sound) != static_cast<int>(sound_id))
            continue;
        if (g_in_use && config::PadVibration()) {
            Rumble(s.low, s.high, s.ms, g_sound_target);
            g_last_rumble = s.name;
            g_last_rumble_target = g_sound_target;
            ++g_rumbles;
        }
        return;
    }
}

void SetSoundRumbleTarget(int controller) { g_sound_target = controller; }

void SetTestState(int controller, uint32_t buttons, float lx, float ly, float rx, float ry)
{
    if (controller < 0 || controller >= XUSER_MAX_COUNT)
        return;
    if (!g_test) {
        Log("pad: test controllers on");
        for (int i = 0; i < kSlots; ++i) {
            if (i < XUSER_MAX_COUNT && g_pads[i].connected && g_set_state && (g_motors[i][0] || g_motors[i][1])) {
                XINPUT_VIBRATION still = {0, 0};  // a real one mustn't go on shaking
                g_set_state(i, &still);
            }
            g_test_pads[i] = {i == 0};  // the first one is always there
        }
        padhid::Update(false);
        memset(g_motors, 0, sizeof g_motors);
    }
    g_test = true;
    g_test_pads[controller] = {true, buttons, 0, {lx, ly}, {rx, ry}};
}

void Unplug(int controller)
{
    if (g_test && controller > 0 && controller < XUSER_MAX_COUNT)
        g_test_pads[controller] = {};
}

void EndTest()
{
    if (g_test)
        Log("pad: test controllers off");
    g_test = false;
    g_in_use = false;
    for (Controller& c : g_pads)
        c = {};
    for (DWORD& scan : g_last_scan)
        scan = 0;  // look for real ones again straight away
    memset(g_motors, 0, sizeof g_motors);
    g_all_down = g_all_prev = 0;
    g_all_left = g_all_right = {0, 0};
}

std::string DebugState()
{
    auto motor = [](int controller, int which) -> unsigned {
        return controller < g_count ? g_motors[g_order[controller]][which] : 0;
    };
    char buf[300];
    snprintf(buf, sizeof buf,
             "connected=%d count=%d inuse=%d test=%d down=%X motors=%u,%u motors2=%u,%u rumbles=%d lastrumble=%s "
             "rumbleto=%d",
             g_count > 0 ? 1 : 0, g_count, g_in_use ? 1 : 0, g_test ? 1 : 0, g_all_down, motor(0, 0), motor(0, 1),
             motor(1, 0), motor(1, 1), g_rumbles, g_last_rumble.c_str(), g_last_rumble_target);
    return buf + (" " + padhid::DebugState());
}

}  // namespace pad
