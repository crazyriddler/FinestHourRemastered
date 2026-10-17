// Keyboard and mouse in game: the mouse turns the view through the game's own look controls. The game asks each frame
// for its physical controls by number (GetControl); the right stick's axes are 18 and 19. Their value is a turn rate
// the game eases its turn towards (a spring: the turn takes a while to reach the stick's rate and drifts on after the
// stick lets go), a controller's feel. The mouse is raw instead: while it moves the view, each look spring's turn is
// the mouse's own motion for the frame, the same degrees per count on both axes, scaled with the zoom, at once, and
// it stops where the mouse stops. The controller's turn is the stick's at once too, unless Look Acceleration is on.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#include "common/cod.h"
#include "common/pc_input.h"
#include "common/widescreen.h"
#include "core/control.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kPads = 0x3EC530, kPadSize = 0x9C;  // the game's controller records, one per port
constexpr uint32_t kRightX = 18, kRightY = 19;
// turn rate per mouse count per second at 100% sensitivity: the game turns 12.5 degrees per second for each 1000 of
// rate (measured), this makes about 0.035 degrees per count
constexpr float kRate = 1.0f / 570.0f;
constexpr float kDegreesPerCount = 0.05f;  // the raw mouse at 100%: a PC shooter's usual at 800 counts per inch

float g_look[2] = {};
// the controller's turn scale per axis (sensitivity, the axis's own, Fast Turn): applied to the look springs' turn,
// not to the stick's value. Above about 1 the game's own fast turn takes over (its turn grows frame by frame)
float g_stick_mult[2] = {1, 1};
bool g_stick = false;
bool g_moved = false;
// the raw mouse: this frame's turn in degrees per axis, and whether the mouse is the one turning the view (it moved
// lately and the stick rests)
float g_mouse_deg[2] = {};
bool g_mouse_mode = false;
std::chrono::steady_clock::time_point g_mouse_t{};

int Port(uint32_t pad) { return pad >= kPads && pad < kPads + 4 * kPadSize ? int((pad - kPads) / kPadSize) : -1; }

// the view's zoom: the camera's view window now against the widest it had (a scope narrows it)
float Zoom() {
    static float widest = 0;
    float x;
    {
        std::lock_guard lock(gpu::g_material_frame_lock);
        x = gpu::g_material_frame.view_x;
    }
    widest = std::max(widest, x);
    return widest > 0 ? std::clamp(x / widest, 0.05f, 1.0f) : 1.0f;
}

}  // namespace

// the controller of a port read for this frame
X86_EXTERN_FUNC(__imp__sub_0012D010);
X86_FUNC(sub_0012D010) {
    uint32_t pad = R32(ctx.esp + 4);
    __imp__sub_0012D010(ctx, base);
    if (Port(pad) != 0) return;
    float x, y;
    int cx = 0, cy = 0;
    pc_input::TakeLook(x, y, &cx, &cy);
    if ((cx || cy) && control::Recording()) {
        char text[48];
        std::snprintf(text, sizeof(text), "%d %d", cx, cy);
        control::RecordLine("mouse", text);
    }
    const cod::Settings& set = cod::Get();
    // the Free Camera flying: the mouse turns it, the player's view stays
    if (cod::wide::FlyActive()) {
        cod::wide::FlyLook(cx, cy * (set.invert_mouse ? -1 : 1), float(set.mouse_sensitivity) / 100.0f);
        g_look[0] = g_look[1] = 0;
        g_moved = false;
        return;
    }
    float k = kRate * float(set.mouse_sensitivity) / 100.0f;
    g_look[0] = -x * k;  // the game turns right for a negative rate on this axis (measured)
    g_look[1] = -y * k * (set.invert_mouse ? -1.0f : 1.0f);  // the stick's up is positive
    g_moved = x != 0 || y != 0;
    float deg = kDegreesPerCount * float(set.mouse_sensitivity) / 100.0f * Zoom();
    g_mouse_deg[0] = std::fabs(float(cx)) * deg;
    g_mouse_deg[1] = std::fabs(float(cy)) * deg;
    auto now = std::chrono::steady_clock::now();
    if (cx || cy) g_mouse_t = now;
    // the right stick, in place of the game's dead zone (it drops all under 0.32 of the travel, then jumps to a
    // quarter of its turn rate): a radial dead zone of the setting, the rest a curve from 0 to full
    input::Pad p = input::Last(0);
    float sx = p.rx / 32767.0f, sy = p.ry / 32767.0f;
    float mag = std::sqrt(sx * sx + sy * sy), dz = std::min(set.stick_deadzone, 50u) / 100.0f;
    bool stick = mag > dz && mag > 1e-4f;
    if (stick) {
        float m = std::min(1.0f, (mag - dz) / std::max(0.98f - dz, 0.05f));
        // the outer part of the travel turns faster (Fast Turn at full tilt), the centre as it was: precise aim
        float t = std::clamp((m - 0.7f) / 0.3f, 0.0f, 1.0f);
        float boost = 1 + (std::clamp(set.stick_outer_boost, 100u, 400u) / 100.0f - 1) * t * t * (3 - 2 * t);
        float k2 = std::pow(m, 1.6f) / mag;
        g_look[0] += -sx * k2;  // the game's x is negative to the right
        g_look[1] += sy * k2 * (set.invert_stick_y ? -1.0f : 1.0f);
        g_moved = true;
        float sens = float(set.stick_sensitivity) / 100.0f * boost;
        g_stick_mult[0] = sens * float(std::clamp(set.stick_sens_x, 25u, 200u)) / 100.0f;
        g_stick_mult[1] = sens * float(std::clamp(set.stick_sens_y, 25u, 200u)) / 100.0f;
    }
    g_stick = stick;
    g_mouse_mode = !stick && now - g_mouse_t < std::chrono::milliseconds(300);
}

// GetControl(pad, control, mode, negate): the value of a physical control, mode 1 as held, 2 its being held
X86_EXTERN_FUNC(__imp__sub_0012D190);
X86_FUNC(sub_0012D190) {
    uint32_t pad = R32(ctx.esp + 4), control = R32(ctx.esp + 8), mode = R32(ctx.esp + 12), negate = R32(ctx.esp + 16);
    __imp__sub_0012D190(ctx, base);
    if (!g_moved || (mode != 1 && mode != 2) || (control != kRightX && control != kRightY) || Port(pad) != 0) return;
    double v = g_look[control - kRightX];
    X87_ST(0) = negate ? -v : v;
}

namespace {

// the look springs (sub_000C7A20(&turn, target, &rate, ...), the turn in degrees for the frame): on foot and on a
// mounted gun, across and up and down, by the call's return
struct Spring {
    uint32_t ret;
    int axis;
    uint32_t turn, rate;  // the addresses it was last called with
};
Spring g_springs[] = {{0xCA362, 0, 0, 0}, {0xCA146, 0, 0, 0}, {0xCA8BE, 1, 0, 0}, {0xCA70E, 1, 0, 0}};

Spring* SpringAt(uint32_t ret) {
    for (Spring& s : g_springs)
        if (s.ret == ret) return &s;
    return nullptr;
}

// before the look updates, the mouse's springs stand still: where the mouse stops the view stops (the game lets a
// spring with no target run down slowly)
void StillSprings() {
    if (!g_mouse_mode) return;
    for (const Spring& s : g_springs)
        if (s.turn) W32(s.turn, 0), W32(s.rate, 0);
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_000C7A20);
X86_FUNC(sub_000C7A20) {
    uint32_t ret = R32(ctx.esp), turn = R32(ctx.esp + 4), t = R32(ctx.esp + 8), rate = R32(ctx.esp + 12);
    Spring* s = SpringAt(ret);
    if (s) s->turn = turn, s->rate = rate;
    // the controller's sensitivities on the target
    float target_f;
    std::memcpy(&target_f, &t, 4);
    if (s && !g_mouse_mode && g_stick) {
        target_f *= g_stick_mult[s->axis];
        std::memcpy(&t, &target_f, 4);
        W32(ctx.esp + 8, t);
    }
    // the controller without the game's easing: the turn is the stick's at once, and stops when it is let go
    if (s && !g_mouse_mode && !cod::Get().look_acceleration) {
        W32(turn, t);
        W32(rate, 0);
        Return(ctx, 0, 0);  // cdecl
        return;
    }
    if (!s || !g_mouse_mode) {
        __imp__sub_000C7A20(ctx, base);
        return;
    }
    // the mouse's turn, its way the game's (the target carries the axes' signs and the invert setting)
    float target;
    std::memcpy(&target, &t, 4);
    float v = target > 0 ? g_mouse_deg[s->axis] : target < 0 ? -g_mouse_deg[s->axis] : 0.0f;
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    W32(turn, bits);
    W32(rate, 0);
    Return(ctx, 0, 0);  // cdecl
}

// the player's look across (and a mounted gun's), then up and down (thiscall)
X86_EXTERN_FUNC(__imp__sub_000C9E30);
X86_FUNC(sub_000C9E30) {
    StillSprings();
    // the game's fast turn (the stick past a threshold: the turn grows each frame, [player+0x46C] on) is acceleration
    // too: off with it
    if (!cod::Get().look_acceleration) W32(ctx.ecx + 0x46C, 0);
    __imp__sub_000C9E30(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_000CA440);
X86_FUNC(sub_000CA440) {
    StillSprings();
    __imp__sub_000CA440(ctx, base);
}

// A controller found (the controllers' thread, cdecl, the port): the game opens it with its profile's settings
// ([0x3EA958], made on first use by sub_00129210). A controller there from the start can be found before the game
// has made the profile (a slow first start): the profile is waited for a moment, then made here.
X86_EXTERN_FUNC(__imp__sub_0012D4C0);
X86_EXTERN_FUNC(__imp__sub_00129210);
X86_FUNC(sub_0012D4C0) {
    for (int i = 0; i < 300 && !R32(0x3EA958); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!R32(0x3EA958)) {
        LOGI("controls: the profile made for port %u's controller", R32(ctx.esp + 4));
        ctx.esp -= 4;  // a return address of our own: the call's ret takes it
        W32(ctx.esp, 0);
        __imp__sub_00129210(ctx, base);
    }
    __imp__sub_0012D4C0(ctx, base);
}
