// The world's animated textures. Some of the levels' materials carry a texture animation made in the art tools
// (RenderWare's material effects, rpMATFXEFFECTUVTRANSFORM, driven by a UV animation): the sewers' light caustics on
// the walls (water_light_refraction), the falling water (water_flowing). The game runs those animations every frame,
// but a world sector draws its materials' effects only through the effects' own pipeline, which a sector takes when
// its stream has the effects' chunk (sub_002117B0, _rpMatFXWorldSectorStreamRead): no sector of the game's worlds has
// it, so their default pipeline drew those materials still, the animation's matrix never set.
//
// RpWorldSectorRender (sub_001F4C80): a sector about to be drawn whose materials have a UV transform takes the effects'
// pipeline, as RpMatFXWorldSectorEnableEffects gives it (the sector's flag, its pipeline from 0x3F7100); its draw
// then sets each animated material's matrix (sub_00212DA0) and the rest as before. Water off: the sector's own again.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_set>

#include "app/app.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kMatFxMaterialOffset = 0x3F6FC8;  // the effects' plugin offset into a material: its data
constexpr uint32_t kMatFxSectorOffset = 0x3F6FDC;    // into a world sector: its "effects on" flag
constexpr uint32_t kMatFxSectorPipeline = 0x3F7100;  // the effects' world sector pipeline
constexpr uint32_t kSectorMesh = 0x78;               // RpWorldSector: its RpMeshHeader
constexpr uint32_t kSectorPipeline = 0x7C;           // its pipeline (0: the world's)
constexpr uint32_t kEffectsAt = 0x60;                // the effects' data: RpMatFXMaterialFlags
constexpr uint32_t kUvTransform = 5, kDualUvTransform = 6;

bool Ok(uint32_t a, uint32_t n) { return a >= 0x10000 && a + n < 0x4000000; }

// the effects the sector's materials have, a bit each
uint32_t SectorEffects(uint32_t sector) {
    uint32_t header = R32(sector + kSectorMesh);
    if (!Ok(header, 16)) return 0;
    uint32_t meshes = R32(header + 4) & 0xFFFF, first = R32(header + 12);
    uint32_t plugin = R32(kMatFxMaterialOffset);
    uint32_t mesh = header + 16 + first, effects = 0;
    if (meshes > 256 || !Ok(mesh, meshes * 12)) return 0;
    for (uint32_t i = 0; i < meshes; i++) {
        uint32_t material = R32(mesh + i * 12 + 8);
        if (!Ok(material, plugin + 4)) continue;
        uint32_t data = R32(material + plugin);
        if (!Ok(data, kEffectsAt + 4)) continue;
        uint32_t e = R32(data + kEffectsAt);
        if (e < 32) effects |= 1u << e;
    }
    return effects;
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_001F4C80);
X86_FUNC(sub_001F4C80) {  // RpWorldSectorRender(sector)
    uint32_t sector = R32(ctx.esp + 4);
    uint32_t flags = R32(kMatFxSectorOffset), pipeline = R32(kMatFxSectorPipeline);
    if (Ok(sector, 0x80) && flags && pipeline) {
        const bool on = app::Get().water;
        uint32_t current = R32(sector + kSectorPipeline);
        if (on && current == 0) {
            uint32_t effects = SectorEffects(sector);
            static const bool log = std::getenv("XBE_MATFX_LOG") != nullptr;
            if (log && effects) {
                static std::unordered_set<uint32_t> seen;
                if (seen.insert(sector).second) {
                    std::string names;
                    uint32_t header = R32(sector + kSectorMesh), mesh = header + 16 + R32(header + 12);
                    for (uint32_t i = 0; i < (R32(header + 4) & 0xFFFF); i++) {
                        uint32_t material = R32(mesh + i * 12 + 8), data = R32(material + R32(kMatFxMaterialOffset));
                        uint32_t tex = R32(material);
                        if (Ok(data, kEffectsAt + 4) && R32(data + kEffectsAt) && Ok(tex, 0x30))
                            names += std::string(" ") + static_cast<const char*>(HostPtr(tex + 0x10));
                    }
                    LOGR("matfx: sector %08X effects %X:%s", sector, effects, names.c_str());
                }
            }
            // (only the animated: the other effects, bump and environment maps, the console never showed either)
            if (effects & (1u << kUvTransform | 1u << kDualUvTransform) &&
                !(effects & ~(1u << kUvTransform | 1u << kDualUvTransform | 1u))) {
                W32(sector + flags, 1);
                W32(sector + kSectorPipeline, pipeline);
            }
        } else if (!on && current == pipeline && R32(sector + flags)) {
            W32(sector + flags, 0);
            W32(sector + kSectorPipeline, 0);
        }
    }
    __imp__sub_001F4C80(ctx, base);
}
