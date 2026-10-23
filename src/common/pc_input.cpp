// See pc_input.h.
#include "common/pc_input.h"
#include "common/prompts.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

#include <toml++/toml.hpp>

#include "common/cod.h"
#include "core/control.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace pc_input {

namespace {

constexpr int64_t kKeysVersion = 3;

struct ActionInfo {
    const char* name;
    const char* label;
    int key[2];
};
// defaults, in Action order
const ActionInfo kActions[kActionCount] = {
    {"move_forward", "Move Forward", {'W', 0}},
    {"move_back", "Move Back", {'S', 0}},
    {"strafe_left", "Strafe Left", {'A', 0}},
    {"strafe_right", "Strafe Right", {'D', 0}},
    {"look_up", "Look Up", {0, 0}},
    {"look_down", "Look Down", {0, 0}},
    {"look_left", "Look Left", {0, 0}},
    {"look_right", "Look Right", {0, 0}},
    {"fire", "Fire", {VK_LBUTTON, 0}},
    {"aim", "Aim Down Sights", {VK_RBUTTON, 0}},
    {"jump", "Stand Up / Jump", {VK_SPACE, 0}},
    {"crouch", "Crouch / Prone", {'C', VK_LCONTROL}},
    {"grenade", "Throw Grenade", {'G', 0}},
    {"melee", "Melee", {'V', VK_MBUTTON}},
    {"use", "Use / Action", {'F', 0}},
    {"reload", "Reload", {'R', 0}},
    {"next_weapon", "Next Weapon", {kWheelUp, kWheelDown}},
    {"weapon_1", "Primary Weapon", {'1', 0}},
    {"weapon_2", "Secondary Weapon", {'2', 0}},
    {"next_grenade", "Switch Grenade", {'T', 0}},
    {"lean_left", "Lean Left (Aiming)", {'Q', 0}},
    {"lean_right", "Lean Right (Aiming)", {'E', 0}},
    {"health", "Use Health Pack", {'H', 0}},
    {"alt_fire", "Alternate Fire (Tank)", {'X', 0}},
    {"center_turret", "Centre Turret", {'Z', 0}},
    {"objectives", "Objectives", {VK_TAB, 0}},
    {"pause", "Pause", {VK_ESCAPE, 0}},
    {"dpad_up", "Menu Up", {VK_UP, 0}},
    {"dpad_down", "Menu Down", {VK_DOWN, 0}},
    {"dpad_left", "Menu Left", {VK_LEFT, 0}},
    {"dpad_right", "Menu Right", {VK_RIGHT, 0}},
    {"menu_accept", "Menu Accept", {VK_RETURN, 0}},
    {"menu_back", "Menu Back", {VK_BACK, 0}},
};

Bindings g_keys;
std::once_flag g_keys_once;
HWND g_hwnd = nullptr;
std::atomic<int> g_dx{0}, g_dy{0};
std::atomic<int64_t> g_wheel_until[2] = {0, 0};  // up, down: held until this time (ms)
std::atomic<bool> g_in_game{false};
std::atomic<int> g_rebind{-1};
int g_rebind_slot = 0;
using Clock = std::chrono::steady_clock;
Clock::time_point g_last_look = Clock::now();

int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

void Defaults() {
    for (int a = 0; a < kActionCount; a++) {
        g_keys.key[a][0] = kActions[a].key[0];
        g_keys.key[a][1] = kActions[a].key[1];
    }
}

std::atomic<int> g_test_key{0};
std::atomic<int64_t> g_test_until{0};

bool Down(int key) {
    if (!key) return false;
    if (key == g_test_key && NowMs() < g_test_until) return true;
    if (key == kWheelUp || key == kWheelDown) return NowMs() < g_wheel_until[key - kWheelUp].load();
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

bool g_menu = false;  // the poll is for a menu: the mouse works the menu, not its bindings

bool MouseKey(int key) {
    return key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2 ||
           key == kWheelUp || key == kWheelDown;
}

bool Pressed(Action a) {
    auto down = [](int key) { return Down(key) && !(g_menu && MouseKey(key)); };
    return down(g_keys.key[a][0]) || down(g_keys.key[a][1]);
}

void Clip() {
    if (!g_hwnd) return;
    if (g_in_game && Active()) {
        RECT r;
        GetClientRect(g_hwnd, &r);
        MapWindowPoints(g_hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
        ClipCursor(&r);
    } else {
        ClipCursor(nullptr);
    }
}

}  // namespace

const char* ActionName(Action a) { return kActions[a].name; }
const char* ActionLabel(Action a) { return kActions[a].label; }

Bindings& Keys() {
    std::call_once(g_keys_once, Defaults);
    return g_keys;
}
void ResetKeys() {
    Keys();
    Defaults();
}

std::string KeyName(int key) {
    switch (key) {
        case 0: return "";
        case kWheelUp: return "WheelUp";
        case kWheelDown: return "WheelDown";
        case VK_LBUTTON: return "Mouse1";
        case VK_RBUTTON: return "Mouse2";
        case VK_MBUTTON: return "Mouse3";
        case VK_XBUTTON1: return "Mouse4";
        case VK_XBUTTON2: return "Mouse5";
        case VK_LSHIFT: return "LShift";
        case VK_RSHIFT: return "RShift";
        case VK_LCONTROL: return "LCtrl";
        case VK_RCONTROL: return "RCtrl";
        case VK_LMENU: return "LAlt";
        case VK_RMENU: return "RAlt";
    }
    UINT sc = MapVirtualKeyW(UINT(key), MAPVK_VK_TO_VSC);
    LONG lp = LONG(sc << 16);
    if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT ||
        key == VK_END || key == VK_HOME || key == VK_INSERT || key == VK_DELETE || key == VK_DIVIDE || key == VK_NUMLOCK)
        lp |= 1 << 24;
    // the keyboard's own name for it, in its language, as UTF-8 (the menus' text)
    wchar_t name[64] = {};
    if (sc && GetKeyNameTextW(lp, name, 64) > 0) {
        char utf8[192];
        if (WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr) > 0) return utf8;
    }
    char hex[16];
    std::snprintf(hex, sizeof(hex), "VK%02X", key);
    return hex;
}

namespace {
// the settings file's names: the same whatever the keyboard's language (letters, digits, these, else VKxx)
struct Named {
    int vk;
    const char* name;
};
constexpr Named kFileNames[] = {
    {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_TAB, "Tab"}, {VK_ESCAPE, "Esc"}, {VK_BACK, "Backspace"},
    {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"}, {VK_INSERT, "Insert"},
    {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"}, {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"},
    {VK_CAPITAL, "CapsLock"}, {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"},
    {VK_NUMPAD0, "Num0"}, {VK_NUMPAD1, "Num1"}, {VK_NUMPAD2, "Num2"}, {VK_NUMPAD3, "Num3"}, {VK_NUMPAD4, "Num4"},
    {VK_NUMPAD5, "Num5"}, {VK_NUMPAD6, "Num6"}, {VK_NUMPAD7, "Num7"}, {VK_NUMPAD8, "Num8"}, {VK_NUMPAD9, "Num9"},
    {VK_MULTIPLY, "Num*"}, {VK_ADD, "Num+"}, {VK_SUBTRACT, "Num-"}, {VK_DECIMAL, "Num."}, {VK_DIVIDE, "Num/"},
};
}  // namespace

std::string FileKeyName(int key) {
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) return std::string(1, char(key));
    if (key >= VK_F1 && key <= VK_F24) return "F" + std::to_string(key - VK_F1 + 1);
    for (const Named& n : kFileNames)
        if (n.vk == key) return n.name;
    std::string fixed = KeyName(key);  // the mouse's, the sides' modifiers: fixed English names
    if (key == 0 || key >= kWheelUp || key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON ||
        key == VK_XBUTTON1 || key == VK_XBUTTON2 || (key >= VK_LSHIFT && key <= VK_RMENU))
        return fixed;
    char hex[16];
    std::snprintf(hex, sizeof(hex), "VK%02X", key);
    return hex;
}

int KeyFromName(const std::string& name) {
    if (name.empty()) return 0;
    if (name.size() > 2 && name.compare(0, 2, "VK") == 0) return int(std::strtol(name.c_str() + 2, nullptr, 16));
    for (int k = 1; k < kKeyCount; k++)
        if (_stricmp(FileKeyName(k).c_str(), name.c_str()) == 0) return k;
    // a file of older versions: the keyboard's own names (in its language)
    for (int k = 1; k < kKeyCount; k++)
        if (_stricmp(KeyName(k).c_str(), name.c_str()) == 0) return k;
    // older still: those names in the system's ANSI code page
    for (int k = 1; k < kKeyCount; k++) {
        UINT sc = MapVirtualKeyW(UINT(k), MAPVK_VK_TO_VSC);
        LONG lp = LONG(sc << 16);
        if (k == VK_LEFT || k == VK_RIGHT || k == VK_UP || k == VK_DOWN || k == VK_PRIOR || k == VK_NEXT ||
            k == VK_END || k == VK_HOME || k == VK_INSERT || k == VK_DELETE || k == VK_DIVIDE || k == VK_NUMLOCK)
            lp |= 1 << 24;
        char ansi[64] = {};
        if (sc && GetKeyNameTextA(lp, ansi, sizeof(ansi)) > 0 && _stricmp(ansi, name.c_str()) == 0) return k;
    }
    return 0;
}

void Load(const void* table) {
    Keys();
    const toml::table& t = *static_cast<const toml::table*>(table);
    const toml::table* keys = t["keys"].as_table();
    // version 3: a PC shooter's (lean on Q and E, the wheel switches weapons); earlier files bound other ones, defaults
    // instead
    if (!keys || t["keys_version"].value_or(int64_t(0)) < kKeysVersion) return;
    for (int a = 0; a < kActionCount; a++) {
        const toml::array* arr = (*keys)[kActions[a].name].as_array();
        if (!arr) continue;
        for (int s = 0; s < 2; s++) g_keys.key[a][s] = s < int(arr->size()) ? KeyFromName(arr->get(size_t(s))->value_or(std::string())) : 0;
    }
}

void Save(void* table) {
    Keys();
    toml::table keys;
    for (int a = 0; a < kActionCount; a++) {
        toml::array arr;
        for (int s = 0; s < 2; s++) arr.push_back(FileKeyName(g_keys.key[a][s]));
        keys.insert(kActions[a].name, arr);
    }
    static_cast<toml::table*>(table)->insert("keys", keys);
    static_cast<toml::table*>(table)->insert_or_assign("keys_version", kKeysVersion);
}

void Window(void* hwnd) {
    g_hwnd = static_cast<HWND>(hwnd);
    RAWINPUTDEVICE rid{0x01, 0x02, 0, g_hwnd};  // generic desktop, mouse
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) LOGW("input: raw mouse input not registered (%lu)", GetLastError());
}

bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    (void)hwnd;
    switch (msg) {
        case WM_INPUT: {
            RAWINPUT ri;
            UINT size = sizeof(ri);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && Active()) {
                g_dx += ri.data.mouse.lLastX;
                g_dy += ri.data.mouse.lLastY;
                if (std::abs(ri.data.mouse.lLastX) + std::abs(ri.data.mouse.lLastY) > 2) cod::prompts::KeyboardUsed();
            }
            return false;
        }
        case WM_MOUSEWHEEL: {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            if (g_rebind >= 0) {
                Keys().key[g_rebind][g_rebind_slot] = d > 0 ? kWheelUp : kWheelDown;
                g_rebind = -1;
            } else if (Active()) {
                g_wheel_until[d > 0 ? 0 : 1] = NowMs() + 90;  // long enough for a few frames to see it
            }
            *result = 0;
            return true;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN: {
            if (Active()) cod::prompts::KeyboardUsed();
            if (g_rebind < 0) break;
            int key = msg == WM_LBUTTONDOWN   ? VK_LBUTTON
                      : msg == WM_RBUTTONDOWN ? VK_RBUTTON
                      : msg == WM_MBUTTONDOWN ? VK_MBUTTON
                      : msg == WM_XBUTTONDOWN ? (GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2)
                                              : int(wp);
            // the left and right of shift, control and alt
            if (key == VK_SHIFT) key = MapVirtualKeyW(UINT((lp >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX);
            if (key == VK_CONTROL) key = (lp >> 24) & 1 ? VK_RCONTROL : VK_LCONTROL;
            if (key == VK_MENU) key = (lp >> 24) & 1 ? VK_RMENU : VK_LMENU;
            // escape cancels, delete clears
            if (key != VK_ESCAPE) Keys().key[g_rebind][g_rebind_slot] = key == VK_DELETE ? 0 : key;
            g_rebind = -1;
            *result = 0;
            return true;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && (Active() || (cod::OverlayOpen() && cod::Get().keyboard_mouse))) {
                SetCursor(nullptr);  // no arrow over the game: the menus draw their own pointer
                *result = TRUE;
                return true;
            }
            break;
        case WM_ACTIVATE:
        case WM_SIZE:
        case WM_MOVE:
            Clip();
            break;
    }
    return false;
}

bool Active() {
    bool focus = GetForegroundWindow() == g_hwnd || NowMs() < g_test_until;
    return cod::Get().keyboard_mouse && g_hwnd && focus && !cod::OverlayOpen() && g_rebind < 0;
}

void SetCapture(bool in_game) {
    if (g_in_game.exchange(in_game) != in_game) Clip();
}

int Rebinding() { return g_rebind; }
void StartRebind(int action, int slot) {
    g_rebind_slot = slot;
    g_rebind = action;
}

// Aiming (the game's left trigger: iron sights and scopes alike). Toggle Aim: a click of a mouse button bound to aim
// starts aiming and the next one stops, as do reloading and throwing a grenade; keys bound to it still aim while held.
// Not kept through the menus, the pause screen or a load.
bool Aiming() {
    static bool latched = false, was = false;
    bool mouse = false, keys = false;
    for (int s = 0; s < 2; s++) {
        int k = g_keys.key[kAim][s];
        if (!k || !Down(k) || (g_menu && MouseKey(k))) continue;
        (MouseKey(k) ? mouse : keys) = true;
    }
    if (!cod::Get().toggle_aim || g_menu || !g_in_game) {
        latched = false, was = mouse;
        return mouse || keys;
    }
    if (mouse && !was) latched = !latched;
    was = mouse;
    // as Call of Duty's toggle aim was up to Black Ops II: reloading or throwing a grenade lets go of it
    static bool reload_was = false, grenade_was = false;
    const bool reload = Pressed(kReload), grenade = Pressed(kGrenade);
    if ((reload && !reload_was) || (grenade && !grenade_was)) latched = false;
    reload_was = reload, grenade_was = grenade;
    return latched || keys;
}

// The weapon keys (1 and 2, as in later Call of Duty games): the game only switches to the other weapon (next weapon),
// so a key switches when the weapon held is not its own. The player's weapon held is its entity's + 0x1AE4 (the
// weapon's id); the primary the one held as a level starts, the secondary the other; a weapon picked up in place of
// the one held takes its place. A switch is a press of next weapon while the held one is not yet the one wanted
// (a press, then a wait for the game to change it)
bool WeaponKeys() {
    static uint32_t primary = 0, last = 0, player_seen = 0;
    static bool switching = false;
    static int64_t press_until = 0, wait_until = 0;
    const uint32_t player = cod::PlayerEntity();
    const int64_t now = NowMs();
    if (!player) return false;
    const uint32_t held = kernel::R32(player + 0x1AE4);
    if (player != player_seen) player_seen = player, primary = held, last = held, switching = false;  // a level
    if (held != last) {
        // changed by a switch (ours, the wheel's, the controller's) or a weapon picked up in place of the held one
        if (!switching && !Pressed(kNextWeapon) && last == primary) primary = held;
        last = held, switching = false;
    }
    if (now < press_until) return true;
    if (now < wait_until) return false;
    const bool want1 = Pressed(kWeapon1), want2 = Pressed(kWeapon2);
    if ((want1 && held != primary) || (want2 && held == primary)) {
        switching = true;
        press_until = now + 120, wait_until = now + 700;
        return true;
    }
    return false;
}

void Pad(int port, input::Pad& pad) {
    if (port != 0 || !cod::Get().keyboard_mouse) return;
    Keys();
    // the keyboard is always plugged in: no "reconnect the controller" without a pad
    pad.connected = true;
    static int64_t last_clip = 0;
    if (int64_t now = NowMs(); now - last_clip > 500) {
        last_clip = now;
        Clip();
    }
    if (!Active()) return;
    // the menus (the front end's and the pause screen) on the mouse: the wheel moves, right goes back
    g_menu = (!g_in_game && !cod::InMovie()) || cod::Paused();
    if (g_menu) {
        if (Down(kWheelUp)) pad.buttons |= 0x1;
        if (Down(kWheelDown)) pad.buttons |= 0x2;
        // the left button: menu_mouse.cpp, on the item pointed at
        if (Down(VK_RBUTTON)) pad.analog[1] = 255;
    }
    auto axis = [](bool neg, bool pos, int16_t& v) {
        if (neg != pos) v = pos ? 32767 : -32768;
    };
    axis(Pressed(kStrafeLeft), Pressed(kStrafeRight), pad.lx);
    axis(Pressed(kMoveBack), Pressed(kMoveForward), pad.ly);
    axis(Pressed(kLookLeft), Pressed(kLookRight), pad.rx);
    axis(Pressed(kLookDown), Pressed(kLookUp), pad.ry);
    auto analog = [&](Action a, int i) {
        if (Pressed(a)) pad.analog[i] = 255;
    };
    analog(kMelee, 0);
    analog(kMenuAccept, 0);
    analog(kCrouch, 1);
    analog(kMenuBack, 1);
    analog(kGrenade, 2);
    analog(kJump, 3);
    analog(kUse, 4);
    analog(kReload, 5);
    const bool aiming = Aiming();
    if (aiming) pad.analog[6] = 255;
    analog(kFire, 7);
    auto button = [&](Action a, uint16_t bit) {
        if (Pressed(a)) pad.buttons |= bit;
    };
    button(kDpadUp, 0x1);
    button(kHealth, 0x1);
    button(kDpadDown, 0x2);
    button(kDpadLeft, 0x4);
    button(kDpadRight, 0x8);
    // the game leans with left and right while aiming, and otherwise switches weapons and grenades with them: the
    // lean keys lean (aiming), the switches switch (not aiming)
    if (aiming) {
        button(kLeanLeft, 0x4);
        button(kLeanRight, 0x8);
    } else {
        button(kNextWeapon, 0x4);
        button(kNextGrenade, 0x8);
        if (WeaponKeys()) pad.buttons |= 0x4;
    }
    // Escape: START in the game (the pause screen, which it also closes) and to skip a movie, back in the menus
    if (g_in_game || cod::InMovie()) button(kPause, 0x10);
    else if (Pressed(kPause)) pad.analog[1] = 255;
    button(kObjectives, 0x20);
    button(kAltFire, 0x40);
    button(kCenterTurret, 0x80);
    // the front end's title screen takes only START: there the menus' accept key is START as well (START does
    // nothing in its other screens)
    if (!g_in_game) button(kMenuAccept, 0x10);
}

void HoldKey(int key, int ms) {
    g_test_key = key;
    g_test_until = NowMs() + ms;
    cod::prompts::KeyboardUsed();
}

std::atomic<int> g_test_dx{0}, g_test_dy{0};  // AddLook's, kept through a replay

void AddLook(int dx, int dy) {
    g_dx += dx;
    g_dy += dy;
    g_test_dx += dx;
    g_test_dy += dy;
}

void TakeLook(float& x, float& y, int* counts_x, int* counts_y) {
    auto now = Clock::now();
    double dt = std::chrono::duration<double>(now - g_last_look).count();
    g_last_look = now;
    int dx = g_dx.exchange(0), dy = g_dy.exchange(0);
    // a replay's motion in place of the mouse's; tests' look commands (AddLook) on top
    int lx = dx, ly = dy;
    if (control::ReplayMouse(dx, dy)) dx += g_test_dx.exchange(0), dy += g_test_dy.exchange(0);
    else dx = lx, dy = ly, g_test_dx = 0, g_test_dy = 0;
    if (counts_x) *counts_x = dx;
    if (counts_y) *counts_y = dy;
    if (dt <= 0 || dt > 0.25) dt = 0.25;  // the first poll after a pause
    x = float(dx / dt);
    y = float(dy / dt);
}

}  // namespace pc_input
