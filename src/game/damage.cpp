// Being hurt. The game's pain effect (CausePlayerPainOnHUD, the HUD's sub_000AABA0, which starts sub_000DC420) blends
// the previous frame, HUD and all, over each new one at 80% while it lasts (sub_0012A800, RenderWare's motion blur):
// the HUD smeared and doubled, more so on a wide screen. In its place the 3D scene shakes a little and wavers as the
// pain fades (gpu::g_hurt, d3d12.cpp's scene composite); the HUD stays still.
#include <algorithm>
#include <atomic>
#include <cstring>

#include "core/log.h"
#include "gpu/backend.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {
constexpr uint32_t kPainLevel = 0x3CBE1C;  // int, the effect's level, falling to 0 over its time
constexpr uint32_t kPainTime = 0x3CC104;   // float, milliseconds left
constexpr uint32_t kPainSpan = 0x3CC11C;   // float, the time it started with
uint32_t g_peak = 0;                       // the level the current pain started at
}  // namespace

namespace cod::damage {
std::atomic<bool> g_test{false};  // control line "hurt": the pain effect as a hit starts it
}

// the motion blur's draw: not drawn
X86_FUNC(sub_0012A800) { Return(ctx, 0, 0); }

// the pain effect's frame (cdecl, the camera): its level falls; the scene's shake follows it
X86_EXTERN_FUNC(__imp__sub_000DC320);
X86_FUNC(sub_000DC320) {
    if (cod::damage::g_test.exchange(false)) {
        uint32_t hud = R32(0x3B5C20);
        uint32_t level = hud ? R32(hud + 0xF108) : 0;
        int32_t span = hud ? int32_t(R32(hud + 0xF10C)) : 0;
        LOGR("damage: test, the HUD's pain level %u for %d ms", level, span);
        float t = float(std::max(span, 1));
        W32(kPainLevel, level ? level : 255);
        std::memcpy(HostPtr(kPainTime), &t, 4);
        std::memcpy(HostPtr(kPainSpan), &t, 4);
    }
    uint32_t before = R32(kPainLevel);
    __imp__sub_000DC320(ctx, base);
    uint32_t level = R32(kPainLevel) & 0xFF;
    if (level > (before & 0xFF) || g_peak == 0) g_peak = level;
    if (!level) g_peak = 0;
    // a full hit shakes most; the level's own scale (0 to 255) sets how much
    float strength = std::min(1.0f, float(g_peak) / 160.0f);
    gpu::g_hurt = g_peak ? strength * float(level) / float(g_peak) : 0.0f;
    static int logged = 0;
    if (level && logged < 6 && level == g_peak) {
        logged++;
        float span;
        std::memcpy(&span, HostPtr(kPainSpan), 4);
        float dt;
        std::memcpy(&dt, HostPtr(0x32166C), 4);
        LOGR("damage: pain level %u over %.0f (a frame %.3f)", level, span, dt);
    }
}

// The level scripts' control of the player's damage, in the log: a script turning it off (a scripted scene) and not
// back on leaves the player unhurt (seen once in the second level, not since). Their argument: the script stack
// ([0x345588]) at the call's first parameter (the call record's [0x1C] byte less its [0x0C]).
namespace {
uint32_t ScriptArg(uint32_t call) {
    uint32_t stack = R32(0x345588);
    int32_t at = int32_t(*reinterpret_cast<const int8_t*>(HostPtr(R32(call + 0x1C)))) - int32_t(R32(call + 0xC));
    return R32(stack + uint32_t(at * 4));
}
void LogScript(const char* what, X86Context& ctx) {
    uint32_t v = ScriptArg(R32(ctx.esp + 8));
    float f;
    std::memcpy(&f, &v, 4);
    LOGR("damage: the level's script %s %u (%.2f)", what, v, f);
}
}  // namespace

// The player's entity: the game's own lookup (sub_0010F590(0), as DisablePlayerDamage uses it) kept as it answers.
// God mode (cod.h SetGodMode, tests): its damage off (DisablePlayerDamage's flag at 0x19FC) every time it is looked
// up, so no script turns it back on
namespace {
std::atomic<uint32_t> g_player{0};
std::atomic<bool> g_god{false};
}  // namespace

namespace cod {
uint32_t PlayerEntity() { return g_player; }
void SetGodMode(bool on) { g_god = on; }
bool GodMode() { return g_god; }
}  // namespace cod

X86_EXTERN_FUNC(__imp__sub_0010F590);
X86_FUNC(sub_0010F590) {
    uint32_t which = R32(ctx.esp + 4);
    __imp__sub_0010F590(ctx, base);
    if (which == 0 && ctx.eax) {
        g_player = ctx.eax;
        if (g_god) W32(ctx.eax + 0x19FC, 1);
    }
}

X86_EXTERN_FUNC(__imp__sub_000563F0);
X86_FUNC(sub_000563F0) {  // DisablePlayerDamage(on)
    LogScript("DisablePlayerDamage", ctx);
    __imp__sub_000563F0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_000563A0);
X86_FUNC(sub_000563A0) {  // SetPlayerHealthPercentage(percent)
    LogScript("SetPlayerHealthPercentage", ctx);
    __imp__sub_000563A0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00055760);
X86_FUNC(sub_00055760) {  // SetCanEnemiesDamage(entity, ...)
    LogScript("SetCanEnemiesDamage, entity", ctx);
    __imp__sub_00055760(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00055900);
X86_FUNC(sub_00055900) {  // SetCanBulletsDamage(entity, ...)
    LogScript("SetCanBulletsDamage, entity", ctx);
    __imp__sub_00055900(ctx, base);
}

// the player's health set outright (SetHealthPercent, +0x19DC): past the damage flag, as the game's own end of a fall
// too long (its time in the air, +0x518, past 0x322188: the player taken for fallen out of the world, its health set
// to 0) does it. In god mode its health is never set lower
X86_EXTERN_FUNC(__imp__sub_000C85C0);
X86_FUNC(sub_000C85C0) {  // SetHealthPercent(percent)
    if (g_god && ctx.ecx == g_player) {
        float now;
        uint32_t n = R32(ctx.ecx + 0x19DC);
        std::memcpy(&now, &n, 4);
        if (float(int32_t(R32(ctx.esp + 4))) * 0.01f < now) {
            LOGR("damage: god, the player's health kept (set to %d%% from %08X)", int32_t(R32(ctx.esp + 4)), R32(ctx.esp));
            ctx.esp += 8;  // (ret 4)
            return;
        }
    }
    __imp__sub_000C85C0(ctx, base);
}
