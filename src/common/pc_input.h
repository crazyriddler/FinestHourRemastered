#pragma once
// Keyboard and mouse: keys and mouse buttons bound to the controller's buttons and sticks, the mouse turning the view
// through the game's look controls (controls.cpp) at its own rate, not through the stick's dead zone.
#include <cstdint>
#include <string>

#include "core/input.h"

namespace pc_input {

// what a key can do: the game's actions, each the controller's button for it in the Standard layout
// (Options > Controls): Y jump, B crouch, X grenade, A melee, black use, white reload, d-pad up health pack, left and
// right next weapon and grenade (leaning while aiming), left stick click alternate fire, right stick click centre turret
enum Action : int {
    kMoveForward, kMoveBack, kStrafeLeft, kStrafeRight,
    kLookUp, kLookDown, kLookLeft, kLookRight,
    kFire, kAim, kJump, kCrouch, kGrenade, kMelee, kUse, kReload, kNextWeapon, kNextGrenade, kLeanLeft, kLeanRight,
    kHealth, kAltFire,
    kCenterTurret, kObjectives, kPause, kDpadUp, kDpadDown, kDpadLeft, kDpadRight, kMenuAccept, kMenuBack,
    kActionCount
};
const char* ActionName(Action a);  // settings file key
const char* ActionLabel(Action a);  // the settings overlay's

// virtual key codes, plus the wheel as two keys
constexpr int kWheelUp = 0x100, kWheelDown = 0x101, kKeyCount = 0x102;
std::string KeyName(int key);
std::string FileKeyName(int key);  // its English name (the settings file's)
int KeyFromName(const std::string& name);

struct Bindings {
    int key[kActionCount][2];  // two keys per action, 0 unbound
};
Bindings& Keys();
void ResetKeys();

// settings file: the [keys] table
void Load(const void* table);
void Save(void* table);

// window thread
void Window(void* hwnd);
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);

// a polled controller: the keys and mouse buttons merged into port 0
void Pad(int port, input::Pad& pad);

// the mouse's motion since the last call: counts per second, and the counts themselves
void TakeLook(float& x, float& y, int* counts_x = nullptr, int* counts_y = nullptr);
void AddLook(int dx, int dy);  // tests: motion as if the mouse moved
void HoldKey(int key, int ms);  // tests: a key held down for a while, the window as if it had the keyboard
bool Active();                // the game window has the keyboard and the overlay is closed
void SetCapture(bool in_game);  // in game the cursor is held in the window and hidden
int Rebinding();              // the action waiting for a key (overlay), -1 none
void StartRebind(int action, int slot);

}  // namespace pc_input
