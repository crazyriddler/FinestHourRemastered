// What the game draws glowing, by texture (the particles' dictionary of each level, and the fire meshes'): each sprite
// drawn with one lights the scene around it (gpu::g_emitter, d3d12.cpp CollectEmitter). A shot's flash lights for
// its few frames and fades; a machine gun's lights at each of its shots; tracers light along their flight; fire
// burns steadily and flickers; explosions light as far as their fireball is large.
#include <cstdint>

#include "gpu/backend.h"

namespace {

struct Entry {
    uint64_t tex;
    gpu::EmitterKind kind;
};

// colour (strength folded in), radius (centimetres, the world's units), the sprite size it is for, hold (seconds),
// flicker, how much brighter the sprite itself is drawn (its glow), whether it lights, its soft edge's depth, the
// shimmer of the air above it, the view's flash when it is close
constexpr gpu::EmitterKind kFlash{{1.5f, 1.0f, 0.5f}, 480, 0, 0.06f, false, 2.6f, true, 30, 0.2f, 0.0f, true};     // firesmoke: shots, fireballs
constexpr gpu::EmitterKind kBurst{{1.8f, 1.2f, 0.6f}, 650, 0, 0.09f, false, 3.0f, true, 50, 1.0f, 1.0f};     // fburst: a blast's flash
constexpr gpu::EmitterKind kFlame{{1.0f, 0.55f, 0.22f}, 520, 120, 0.25f, true, 1.8f, true, 50, 1.0f};  // fire sprites
constexpr gpu::EmitterKind kFireMesh{{1.0f, 0.55f, 0.22f}, 650, 0, 0.3f, true, 1.6f, true, 0, 0.8f};  // burning models' fire
// tracers (machine guns', tanks' and flak's rounds): a warm orange (times their own colour), a few metres about their flight
constexpr gpu::EmitterKind kTracer{{1.55f, 0.82f, 0.32f}, 520, 0, 0.04f, false, 3.0f, true, 0, 0, 0, false, true, 0, true};
// a tank's or a gun's shell in flight (its trail): warm, further
constexpr gpu::EmitterKind kShell{{1.6f, 1.2f, 0.8f}, 800, 0, 0.05f, false, 2.5f, true, 0, 0, 0, false, true, 0, true};
constexpr gpu::EmitterKind kSpark{{0.9f, 0.68f, 0.35f}, 170, 0, 0.04f, false, 2.5f, true, 0, 0, 0, false, true};
constexpr gpu::EmitterKind kBlueSpark{{0.4f, 0.55f, 1.0f}, 170, 0, 0.04f, false, 2.5f, true, 0, 0, 0, false, true};
constexpr gpu::EmitterKind kEmber{{0.8f, 0.32f, 0.12f}, 320, 0, 0.15f, true, 1.6f};    // smredcent
// smoke and dust: no light, soft where they meet the ground and walls
constexpr gpu::EmitterKind kSmoke{{0, 0, 0}, 0, 0, 0, false, 1, false, 60};
// where they land on water, a ring and a splash (Water Interaction): a bullet's dust, a shell's or a grenade's dirt,
// the game's own spray (level 1), a blast
constexpr gpu::EmitterKind Hit(gpu::EmitterKind k, float impact) {
    k.impact = impact;
    return k;
}
constexpr gpu::EmitterKind kBulletHit = Hit(kSmoke, 0.6f);
constexpr gpu::EmitterKind kGeyser = Hit(kSmoke, 2.0f);
constexpr gpu::EmitterKind kBlast = Hit(kBurst, 2.5f);
constexpr gpu::EmitterKind kSpray = Hit(gpu::EmitterKind{{0, 0, 0}, 0, 0, 0, false, 1, false}, 1.0f);

constexpr Entry kTable[] = {
    {0x59b1a9b9814675d8, kFlash},     // firesmoke
    {0x29cba8343fe083f3, kBlast},     // fburst
    {0x13f75c5c1b05d1c7, kFlame},     // flame
    {0xe35326b85a3f60e9, kFlame},     // flame2
    {0xc0937612b3842557, kFlame},     // flame3
    {0xe51f28ac26db6d4a, kFlame},     // flame4
    {0xf460aaaa72faa11f, kFlame},     // rollingflame
    {0x17f6101c7e9e08a0, kFlame},     // rollingflame2
    {0xf9791e32609f0e34, kFlame},     // rollingflame3
    {0xb860a3f69def7578, kFlame},     // rollingflame4
    {0x9347a1cc65c2c614, kFireMesh},  // Dark_Opt_firealpha
    {0x07aeed6001d3af4d, kFireMesh},  // 1_1_aFire
    {0x34d4d655e55e0eae, kTracer},    // tracerroundwhitetrans
    {0x093ae31e2da4082d, kTracer},    // tracerstreakwhitetrans
    {0x1564ef8b30c88ab1, kShell},     // tankwhoosh: a shell's trail
    {0x716232ce5371ec2f, kSpark},     // spark
    {0x914441d0a35fa26d, kSpark},     // spark4
    {0x8e36ce1937723fe2, kBlueSpark}, // bluespark
    {0x5e3266a7516f2fe6, kEmber},     // smredcent
    {0xb3a23299277edaed, kSmoke},     // ysmoke
    {0xd5f717cecea1f6f6, kSmoke},     // smoke
    {0xdfcaa69cacadf2b2, kSmoke},     // wsmoke
    {0xce22552515d50591, kSmoke},     // flitsmoke
    {0x83ef87ef9e4fdaed, kSmoke},     // smokepoof
    {0x7cd5a99f181c8b50, kSmoke},     // smokepoof2
    {0x02c0145a097a389e, kSmoke},     // smokepoof3
    {0xdc8d4d6370a009fa, kSmoke},     // smokeblastMH
    {0x7f7d9a4e9ca38755, kSmoke},     // ydust
    {0x12271e08cedc7a53, kSmoke},     // brickpiece's dark smoke
    {0xd671f0d093f9f6af, kGeyser},    // dirtgeyser1
    {0x3ef5763b805afcb0, kGeyser},    // dirtgeyser2
    {0x9d1e433f62fe9d40, kGeyser},    // dirtgeyser3
    {0x9d7bf44df07cccc3, kBulletHit}, // bulletgroundhit
    {0x4919c421ede75030, kSpray},     // 1_1_aWaterSplashA (level 1's shells and rounds in the Volga)
    {0xf778fa6ae940992f, kSmoke},     // stormcloud
};

bool Kind(uint64_t tex, gpu::EmitterKind& kind) {
    for (const Entry& e : kTable)
        if (e.tex == tex) {
            kind = e.kind;
            return true;
        }
    return false;
}

const bool registered = [] {
    gpu::g_emitter = Kind;
    return true;
}();

}  // namespace
