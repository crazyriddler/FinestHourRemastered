// Tests run on the game's thread from the player's update: a level ended as its script ends it (control line
// "endlevel <next>", cod::EndLevel: QuitAndLoadNextLevel's sub_000A42F0, the level ids of the game's table, L_NULL 0),
// and the player handed any weapon of the game's table (control line "giveweapon <id>", cod::GiveWeapon), so the
// remastered meshes' dump (gpu/mesh_swap.h) sees every first-person model, not only those a level starts with.
// The game's own way, as its scripts do it: the weapon's bank streamed into the player's free stream slot
// (RequestToStreamPlayerWeapon: the player's sub_000C2D20), waited for (IsPlayerWeaponStreamed: sub_000C2DE0), then
// equipped in place of the held weapon (SetPlayerWeapon: sub_000C7190(id, 1, 0)). Run from the player's own update
// (sub_000CE790), on the game's thread; the control line only leaves the request.
//
// Ids: 0x00-0x12 and 0x25 are the weapons held in hand (Bazooka, Panzerschreck, BAR, Bren, DP, M1919A6, MG42, MP40,
// PPSh, Sten, Thompson, Luger, Enfield, G43, Mosin Nagant, M1 Garand, Kar98, Nagant Sniper, Springfield; Grease Gun);
// the rest are thrown, mounted or a vehicle's.
#include <atomic>
#include <cstdint>

#include "common/cod.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {
constexpr uint32_t kWeaponDef = 0xD20D0;     // cdecl (id): the weapon's definition
constexpr uint32_t kModelOf = 0x127880;      // cdecl (definition): its first-person model, 0 before it is streamed
constexpr uint32_t kFreeSlot = 0xC2E30;      // thiscall: the stream slot neither carried weapon uses
constexpr uint32_t kStream = 0xC2D20;        // thiscall (id): its bank streamed into [player+0x2130]'s slot
constexpr uint32_t kStreamed = 0xC2DE0;      // thiscall (id): its bank in a slot
constexpr uint32_t kEquip = 0xC7190;         // thiscall (id, now, other slot): SetPlayerWeapon
constexpr uint32_t kSlotLocks = 0x3E1E68;    // the slots' locks (a load running, a mounted gun's)
constexpr uint32_t kHeld = 0x1AE4;           // the player's weapon in hand
constexpr uint32_t kStreamSlot = 0x2130, kMayStream = 0x2134;
constexpr uint32_t kObjects = 0x1A48;        // the player's weapon objects by id
constexpr int kWeapons = 0x27;
constexpr int kTimeoutFrames = 600;

constexpr uint32_t kInventory = 0x21AC;     // the player's thrown items
constexpr uint32_t kItemCount = 0xD06D0;     // thiscall (index, count)
constexpr uint32_t kItemSelect = 0xD06B0;    // thiscall (index)

constexpr uint32_t kEndLevel = 0xA42F0;     // cdecl (next level): QuitAndLoadNextLevel
constexpr uint32_t kQuitLevel = 0xA42B0;    // cdecl: QuitLevel, back to the main menu

std::atomic<int> g_request{-1};
std::atomic<int> g_end_level{-1};
std::atomic<int> g_item{-1}, g_item_count{0};

// GivePlayerInventoryItem's (0x38770) inventory index of a thrown item's id, -1 none
int ItemIndex(int id) {
    switch (id) {
        case 0x13: return 10;  // M8 Smoke
        case 0x14: return 8;   // Mark2 Frag
        case 0x15: return 2;   // RGD33
        case 0x16: return 9;   // Stielhandgranate
        case 0x1F: return 5;   // ST74
        case 0x21: return 7;   // Teller Mine
        case 0x22: return 11;  // KaiserBear Grenade
        case 0x26: return 12;  // StickyBear Grenade
        default: return -1;
    }
}
int g_want = -1, g_frames = 0;
bool g_asked = false;

uint32_t Call(X86Context& ctx, uint32_t self, uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t ecx = ctx.ecx;
    ctx.ecx = self;
    uint32_t r = CallGuest(ctx, fn, args);
    ctx.ecx = ecx;
    return r;
}

void Step(X86Context& ctx, uint32_t player) {
    if (int next = g_end_level.exchange(-1); next >= 0) {
        if (next == 0) {
            LOGR("endlevel: QuitLevel");
            CallGuest(ctx, kQuitLevel, {});
        } else {
            LOGR("endlevel: QuitAndLoadNextLevel(%d)", next);
            CallGuest(ctx, kEndLevel, {uint32_t(next)});
        }
    }
    if (int item = g_item.exchange(-1); item >= 0) {
        const uint32_t inv = player + kInventory;
        Call(ctx, inv, kItemCount, {uint32_t(ItemIndex(item)), uint32_t(g_item_count.load())});
        Call(ctx, inv, kItemSelect, {uint32_t(ItemIndex(item))});
        LOGR("giveitem: %02X x%d", item, g_item_count.load());
    }
    if (int r = g_request.exchange(-1); r >= 0) g_want = r, g_frames = 0, g_asked = false;
    if (g_want < 0) return;
    const uint32_t id = uint32_t(g_want);
    if (R32(player + kHeld) == id) {
        LOGR("giveweapon: %02X already in hand", id);
        g_want = -1;
        return;
    }
    if (++g_frames > kTimeoutFrames) {
        LOGR("giveweapon: %02X not streamed in time", id);
        g_want = -1;
        return;
    }
    if (!g_asked) {
        uint32_t slot = Call(ctx, player, kFreeSlot, {});
        if (slot > 2 || R32(kSlotLocks + slot * 4)) return;  // (a load running there: next frame)
        W32(player + kStreamSlot, slot);
        W32(player + kMayStream, 1);
        Call(ctx, player, kStream, {id});
        g_asked = true;
        return;
    }
    if (!Call(ctx, player, kStreamed, {id})) return;
    uint32_t def = CallGuest(ctx, kWeaponDef, {id});
    if (!def || !CallGuest(ctx, kModelOf, {def})) return;
    Call(ctx, player, kEquip, {id, 1, 0});
    // a full clip and reserve, so it can be fired and reloaded
    if (uint32_t obj = R32(player + kObjects + id * 4)) W32(obj + 0x414, R32(def + 0xC8)), W32(obj + 0x418, R32(def + 0xCC));
    LOGR("giveweapon: %02X in hand (held %02X, %d frames)", id, R32(player + kHeld), g_frames);
    g_want = -1;
}
}  // namespace

namespace cod {
void GiveWeapon(int n) {
    if (n < 0 || n >= kWeapons) return;
    g_request = n;
    LOGR("giveweapon: %02X asked", n);
}
void EndLevel(int next) { g_end_level = next; }

void GiveItem(int id, int count) {
    if (ItemIndex(id) < 0) return;
    g_item_count = count;
    g_item = id;
}
}  // namespace cod

// the player's update (thiscall, dt)
X86_EXTERN_FUNC(__imp__sub_000CE790);
X86_FUNC(sub_000CE790) {
    uint32_t self = ctx.ecx;
    __imp__sub_000CE790(ctx, base);
    if (self && self == cod::PlayerEntity() && (g_want >= 0 || g_request.load() >= 0 || g_item.load() >= 0 || g_end_level.load() >= 0)) {
        uint32_t eax = ctx.eax;
        Step(ctx, self);
        ctx.eax = eax;
    }
}
