// RenderWare worlds freed under the objects still in them (the game's own bug: it crashes the same way on the
// console's emulators). Each level section is an RpWorld; unloading one (sub_0010FE10) moves what it can find into
// the worlds that stay (sub_0010FD70: clumps and lights by the world's lists, atomics by the ties in its sectors),
// then frees it (RpWorldDestroy, sub_001F1450), which unties every atomic and light from its sectors but leaves
// each object's own world pointer as it was. An atomic it could not find (no tie in the sectors yet), and every
// object when a level's main world goes at its end, keeps pointing at the freed world; the next time its frame
// moves, RenderWare's frame sync (sub_001CE8E0) ties it again into that world (sub_001F3CC0 -> sub_001F3B70),
// walking a tree in freed memory: a crash where the memory was used again (Road to Remagen's tunnel by the large
// building, every time), writes into it where it was not (crashes later, anywhere).
// Here the worlds alive are kept (created, read, destroyed), and an object whose world is gone is taken out of it
// (its world pointer cleared, as the world's own removal does) before RenderWare touches that world: an atomic as
// its frame is synced, a clump or a light as the game removes it to place it elsewhere. The game then places it in
// the world where it is (sub_0010FBA0 and its kin), as it does any object with no world.
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <unordered_set>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {
constexpr uint32_t kAtomicWorld = 0x3F6DAC;  // the world plugin's offset in an atomic: {world, sync}
constexpr uint32_t kClumpWorld = 0x3F6DB0;   // in a clump: {world, ...}
constexpr uint32_t kLightWorld = 0x3F6DB4;   // in a light: {world, ...}
constexpr uint32_t kLightLink = 0x34;        // a light's link in its world's list

std::mutex g_lock;
std::unordered_set<uint32_t> g_worlds;

void Born(uint32_t world) {
    if (!world) return;
    std::lock_guard<std::mutex> guard(g_lock);
    g_worlds.insert(world);
}

bool Alive(uint32_t world) {
    static const bool off = std::getenv("XBE_WORLDS_AS_GAME") != nullptr;  // tests: the game's own bug, as it was
    if (off) return true;
    std::lock_guard<std::mutex> guard(g_lock);
    return g_worlds.count(world) != 0;
}

void Told(const char* what, uint32_t object, uint32_t world) {
    static int n = 0;
    if (n++ < 32) LOGR("worlds: %s %08X was still in the freed world %08X: taken out", what, object, world);
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_001F14E0);
X86_FUNC(sub_001F14E0) {  // RpWorldCreate
    __imp__sub_001F14E0(ctx, base);
    Born(ctx.eax);
}

X86_EXTERN_FUNC(__imp__sub_001F5700);
X86_FUNC(sub_001F5700) {  // RpWorldStreamRead
    __imp__sub_001F5700(ctx, base);
    Born(ctx.eax);
}

X86_EXTERN_FUNC(__imp__sub_001F1450);
X86_FUNC(sub_001F1450) {  // RpWorldDestroy(world)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_worlds.erase(R32(ctx.esp + 4));
    }
    __imp__sub_001F1450(ctx, base);
}

// an atomic's frame synced (cdecl, the atomic): tied again to its world's sectors
X86_EXTERN_FUNC(__imp__sub_001F3CC0);
X86_FUNC(sub_001F3CC0) {
    uint32_t atomic = R32(ctx.esp + 4);
    uint32_t at = atomic + R32(kAtomicWorld);
    if (uint32_t world = R32(at); world && !Alive(world)) {
        Told("an atomic", atomic, world);
        W32(at, 0);  // (its ties went with the world's sectors)
    }
    __imp__sub_001F3CC0(ctx, base);
}

// RpWorldRemoveClump (cdecl, world, clump): the world's clump list and count
X86_EXTERN_FUNC(__imp__sub_001F4960);
X86_FUNC(sub_001F4960) {
    uint32_t world = R32(ctx.esp + 4), clump = R32(ctx.esp + 8);
    if (world && !Alive(world)) {
        Told("a clump", clump, world);
        W32(clump + R32(kClumpWorld), 0);
        Return(ctx, world, 0);  // cdecl
        return;
    }
    __imp__sub_001F4960(ctx, base);
}

// RpWorldRemoveLight (cdecl, world, light): the world's light list
X86_EXTERN_FUNC(__imp__sub_001F4600);
X86_FUNC(sub_001F4600) {
    uint32_t world = R32(ctx.esp + 4), light = R32(ctx.esp + 8);
    if (world && !Alive(world)) {
        Told("a light", light, world);
        W32(light + R32(kLightWorld), 0);
        W32(light + kLightLink, light + kLightLink);  // its link on its own, as an unlinked one
        W32(light + kLightLink + 4, light + kLightLink);
        Return(ctx, world, 0);  // cdecl
        return;
    }
    __imp__sub_001F4600(ctx, base);
}

// RpCollisionWorldForAllIntersections (cdecl, world, intersection, callback, data): the game asks every loaded
// section's world, each loop checking the section's world is there (a section is marked loaded, sub_000A4040,
// before its world is read) but one (sub_000779A0, at 0x77D1B: the American sewers, restarting at a checkpoint,
// crashed in it). A section with no world yet has nothing to meet, as the others take it.
X86_EXTERN_FUNC(__imp__sub_001DDA70);
X86_FUNC(sub_001DDA70) {
    if (!R32(ctx.esp + 4)) {
        static int n = 0;
        if (n++ < 8) LOGR("worlds: intersections asked of a section with no world yet (from %08X)", R32(ctx.esp));
        Return(ctx, 0, 0);  // cdecl
        return;
    }
    __imp__sub_001DDA70(ctx, base);
}
