#pragma once
// Texture remastering: each 2D texture the title uploads gets a content id (a hash of its top level as the title keeps
// it), its pixels can be dumped (XBE_DUMP_TEXTURES=<folder>), and the pack tools/remaster makes from the game's
// texture dictionaries replaces it when it is drawn: a higher resolution colour map, and for surfaces normal,
// roughness, height, cavity and specular maps with the material's parameters (materials.txt).
#include <dxgiformat.h>

#include <cstdint>
#include <string>
#include <vector>

#include "gpu/texture.h"

namespace gpu::remaster {

// 0 when the texture is not one to remaster (cube maps, tiny ones). The id is made from the top level's bytes as the
// title keeps them (swizzled, palette indices) and, for P8, a signature of its first 16 colours that does not depend
// on their channel order: tools/remaster reads the same bytes from the game's texture dictionaries.
uint64_t Identify(const TextureDesc& d, const uint8_t* guest, const uint32_t* palette);
// writes <folder>/<id>.dds once per id when XBE_DUMP_TEXTURES is set, with the guest's own bytes of the top level and
// its palette beside it (<id>.raw: the level, then 256 colours)
void Dump(uint64_t id, const TextureDesc& d, Host host, const std::vector<uint8_t>& top, uint32_t pitch,
          const uint8_t* guest, const uint32_t* palette);

std::string IdName(uint64_t id);  // 16 hex digits

struct Material {
    bool pbr = false;    // normal and material maps besides the colour
    bool tiles = false;
    float normal = 1, pom = 0, spec = 0;
    int detail = -1;     // detail texture index, -1 none
    int grain = -1;      // its kind's detail colour grain (the details array's slice past the normal maps, Detail Textures), -1 none
    float refl = 0;      // how much its smooth parts reflect (by its class)
    float metal = 0;     // its class is metal: its bare parts (grey, smooth) reflect tinted, as metal does
    bool skin = false;
    bool cloth = false;   // its class is fabric (uniforms)    // its class is skin (hands, faces): no relief on the weapon in hand's
    bool water = false;   // its names say water (rivers, the Volga, sewers): ripples and reflections (Water)
    float sway = 0;      // foliage and cloth cut out by their alpha sway in the wind (Wind)
    bool snow = false;    // the snowy ground (the pack's snow.txt): footprints in it (Snow Tracks)
};

// a DDS as read: its format, size and mips, the levels one after another
struct Image {
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t w = 0, h = 0, mips = 0;
    std::vector<uint8_t> data;
    std::vector<size_t> offsets;  // per mip into data
    uint32_t RowBytes(uint32_t mip) const;
    uint32_t Rows(uint32_t mip) const;
};
bool ReadDds(const std::wstring& path, Image& out);
// the pack's detail normal maps (details/<n>.dds, the kinds of Material::detail), all of one format and size
std::vector<Image> ReadDetails();
// a button prompt of the pack (prompts/<name>.dds, RGBA), false when it has none
bool ReadPrompt(const std::string& name, Image& out);
std::wstring Dir();  // the pack's folder

struct Loaded {
    uint64_t id = 0;
    Image color, normal, mat;  // normal and mat empty without pbr
};

bool Active();                        // a pack is there and the setting is on
const Material* Find(uint64_t id);    // the pack's material for the texture, null for none
void Request(uint64_t id);            // reads its maps in the background, once
bool TakeLoaded(Loaded& out);         // render thread: one read that finished

}  // namespace gpu::remaster
