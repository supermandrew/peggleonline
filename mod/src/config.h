#pragma once
// Settings in mods\netplay.ini (last joined address, hosting port, controller, display).
#include <cstdint>
#include <string>

namespace config {

std::string LastAddress();
void SetLastAddress(const std::string& address);
uint16_t Port();

// Controller: which buttons do what (a set of pad::Button bits per action), vibration, aim speed, and which
// controller plays which player in a local duel (swapped: the first controller plays player 2).
enum PadAction { kPadShoot, kPadBack, kPadChat, kPadReplay, kPadFastForward, kPadMenu, kPadFlippers, kPadActionCount };
uint32_t PadBinding(int action);
uint32_t DefaultPadBinding(int action);
void SetPadBinding(int action, uint32_t buttons);
bool PadVibration();
void SetPadVibration(bool on);
enum AimSpeed { kAimSlow, kAimNormal, kAimFast, kAimSpeedCount };
int PadAimSpeed();
void SetPadAimSpeed(int speed);
bool PadPlayersSwapped();
void SetPadPlayersSwapped(bool swapped);

// Display: borderless full screen (a window without a frame across its whole monitor).
bool Borderless();
void SetBorderless(bool on);

// Duel: Chaos Mode and Sabotage Mode, the boxes on the Two Player Selection.
bool ChaosMode();
void SetChaosMode(bool on);
bool RedPegMode();
void SetRedPegMode(bool on);

}  // namespace config
