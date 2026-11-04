#pragma once
// PlayStation controllers (DualShock 4, DualSense) read straight from their HID reports, for when nothing shows them
// to the game as an Xbox controller: DSX's PS4 and native modes, or no remapper at all. pad.cpp only asks for them
// while XInput has no controller: a remapper's Xbox controller is usually one of these seen twice.
// Finding them is slow, so a thread of its own does that; everything else here runs on the game thread.
#include <cstddef>
#include <cstdint>
#include <string>

namespace padhid {

constexpr int kMax = 4;

struct State {
    uint32_t buttons;        // bits by pad::Button, all but the triggers
    uint8_t lx, ly, rx, ry;  // 0..255, the middle about 128, y down
    uint8_t l2, r2;          // 0..255
};

// Once per frame. Not wanted: every controller closed, and no looking for more.
void Update(bool wanted);
bool Get(int pad, State& out);  // false: nothing there (or nothing heard from it yet)
void SetMotors(int pad, uint8_t heavy, uint8_t light);

// Testing: what a report says (dualsense or not, as the device's input report length is what tells USB from Bluetooth),
// and what's connected.
bool ParseReport(const uint8_t* report, size_t size, bool dualsense, size_t input_length, State& out);
std::string DebugState();

}  // namespace padhid
