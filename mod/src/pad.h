#pragma once
// The controllers, through XInput: Xbox controllers, and anything Steam, DSX or DS4Windows presents as one.
// Read once per frame while the game's window is in front. Every connected controller counts as the same one,
// except where a local duel gives each player their own (Select).
// Tests drive pretend controllers through the debug pipe instead.
#include <cstdint>
#include <string>

namespace pad {

enum Button { kA, kB, kX, kY, kLB, kRB, kLT, kRT, kBack, kStart, kLS, kRS, kUp, kDown, kLeft, kRight, kButtonCount };
const char* ButtonKey(int button);        // "A", "LB", ... as written in netplay.ini
int ButtonByKey(const std::string& key);  // or -1
const char* ButtonName(int button);       // "A / CROSS": Xbox and PlayStation names

struct Stick {
    float x, y;  // -1..1 past the dead zone; y is up
};

void Update();  // once per frame, on the game thread
bool Connected();
int Count();  // controllers connected

// Which controller Down, Pressed, Left, ... read until changed: every one together (kEveryone), or the first or
// second connected (0, 1), in the order Windows numbers them.
constexpr int kEveryone = -1;
void Select(int controller);

bool Down(int button);
bool Pressed(int button);  // went down this frame
uint32_t PressedBits();    // every button that went down this frame
uint32_t DownBits();
Stick Left();
Stick Right();

// The player is using a controller rather than the mouse: from the first controller input until the mouse moves.
bool InUse();
bool MouseTookOver();  // checks the mouse now (also ends InUse when it moved)
void NoteMouseUsed();  // input arrived from the mouse some other way (the test pipe)

// Vibration: strongest effect wins, each fading out over its last part. Off in settings, or with the mouse in use.
void Rumble(float low, float high, int ms, int controller = kEveryone);
void OnSound(unsigned sound_id);              // the game starts a sound: some of them shake the controller ...
void SetSoundRumbleTarget(int controller);    // ... this one (kEveryone: all)

// Testing (debug pipe): pretend controllers replace the real ones until EndTest. Controller 0 is always there;
// another one is plugged in by giving it a state and unplugged with Unplug.
void SetTestState(int controller, uint32_t buttons, float lx, float ly, float rx, float ry);
void Unplug(int controller);
void EndTest();
std::string DebugState();

}  // namespace pad
