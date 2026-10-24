// The cheats (PC Settings, Cheats; cod.h Settings), run on the game's thread from the player's update
// (game/player_tests.cpp's hook of sub_000CE790), each frame:
// - God Mode: the player's damage held off (cod::SetGodMode, game/damage.cpp), turned on and off with the setting;
// - Infinite Ammo: every weapon the player carries has its reserve full (its object's +0x418, the definition's +0xCC),
//   and the thrown items (grenades, mines: the inventory's counts at player+0x21AC, one word an item) never go down;
// - No Reload: the weapon in hand's magazine full (its object's +0x414, the definition's +0xC8).
// The Free Camera's F11 is game_info.cpp's, its flight common/widescreen.cpp's.
#include <cstdint>
#include <cstdlib>

#include "common/cod.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {
constexpr uint32_t kWeaponDef = 0xD20D0;  // cdecl (id): the weapon's definition
constexpr uint32_t kHeld = 0x1AE4;        // the player's weapon in hand
constexpr uint32_t kObjects = 0x1A48;     // the player's weapon objects by id
constexpr int kWeapons = 0x27;
constexpr uint32_t kClip = 0x414, kReserve = 0x418;           // a weapon object's rounds
constexpr uint32_t kClipSize = 0xC8, kReserveSize = 0xCC;     // its definition's
constexpr uint32_t kInventory = 0x21AC;                       // the thrown items' counts
constexpr int kItems = 13;

uint32_t g_defs[kWeapons] = {};
uint32_t Def(X86Context& ctx, int id) {
    if (!g_defs[id]) g_defs[id] = CallGuest(ctx, kWeaponDef, {uint32_t(id)});
    return g_defs[id];
}

bool g_god = false;
uint32_t g_items_player = 0;
uint32_t g_items[kItems] = {};
}  // namespace

namespace cod {
void CheatsStep(X86Context& ctx, uint32_t player) {
    const Settings& s = Get();
    // tests (XBE_CHEATS_LOG): the weapon in hand's rounds and the thrown items twice a second
    static const bool log = std::getenv("XBE_CHEATS_LOG") != nullptr;
    static int frames = 0;
    if (log && ++frames % 30 == 0) {
        uint32_t id = R32(player + kHeld);
        uint32_t obj = id < uint32_t(kWeapons) ? R32(player + kObjects + id * 4) : 0;
        LOGR("cheats: weapon %02X clip %u reserve %u, items %u %u %u %u %u %u %u %u %u %u %u %u %u", id,
             obj ? R32(obj + kClip) : 0, obj ? R32(obj + kReserve) : 0, R32(player + kInventory), R32(player + kInventory + 4),
             R32(player + kInventory + 8), R32(player + kInventory + 12), R32(player + kInventory + 16),
             R32(player + kInventory + 20), R32(player + kInventory + 24), R32(player + kInventory + 28),
             R32(player + kInventory + 32), R32(player + kInventory + 36), R32(player + kInventory + 40),
             R32(player + kInventory + 44), R32(player + kInventory + 48));
    }
    if (s.god_mode != g_god) {
        g_god = s.god_mode;
        SetGodMode(g_god);
        LOGR("cheats: god mode %s", g_god ? "on" : "off");
    }
    if (s.infinite_ammo) {
        for (int id = 0; id < kWeapons; id++) {
            uint32_t obj = R32(player + kObjects + uint32_t(id) * 4);
            if (!obj) continue;
            uint32_t def = Def(ctx, id);
            if (!def) continue;
            uint32_t full = R32(def + kReserveSize);
            if (full && R32(obj + kReserve) < full) W32(obj + kReserve, full);
        }
        // the thrown items: a count that went down is put back (one picked up raises what is kept)
        const uint32_t inv = player + kInventory;
        if (g_items_player != player) {
            g_items_player = player;
            for (int i = 0; i < kItems; i++) g_items[i] = R32(inv + uint32_t(i) * 4);
        }
        for (int i = 0; i < kItems; i++) {
            uint32_t n = R32(inv + uint32_t(i) * 4);
            if (n > 99) continue;
            if (n < g_items[i]) W32(inv + uint32_t(i) * 4, g_items[i]);
            else g_items[i] = n;
        }
    } else {
        g_items_player = 0;  // (taken again when it is turned on)
    }
    if (s.no_reload) {
        uint32_t id = R32(player + kHeld);
        if (id < uint32_t(kWeapons)) {
            uint32_t obj = R32(player + kObjects + id * 4);
            uint32_t def = obj ? Def(ctx, int(id)) : 0;
            uint32_t full = def ? R32(def + kClipSize) : 0;
            if (full && R32(obj + kClip) < full) W32(obj + kClip, full);
        }
    }
}
}  // namespace cod
