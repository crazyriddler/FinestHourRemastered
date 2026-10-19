#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gpu/state.h"

// HLSL for the title's shaders and for its fixed function state, per the XDK's definitions of both
namespace gpu {

enum TexKind : uint8_t { kTexNone, kTex2D, kTex3D, kTexCube };

struct TexInfo {
    uint8_t kind = kTexNone;
    uint8_t rect = 0;    // linear texture, coordinates in texels
    uint8_t shadow = 0;  // depth texture: 1 D24, 2 D16
    uint8_t flags = 0;   // kTexWide; bits 0 and 1 are a backend extension's
};
enum : uint8_t {
    kTexWide = 4,  // an upscaled target shrunk by the pass: each tap averaged over console texels
    kTexMaterial = 8,  // remastered: normal and material maps at t4 and t5, constants in mtl (one stage a draw)
};

struct PixelKey {
    uint32_t programmable = 0;
    // combiners, straight from D3DPIXELSHADERDEF
    uint32_t count = 0, rgb_in[8] = {}, rgb_out[8] = {}, a_in[8] = {}, a_out[8] = {}, final_abcd = 0, final_efg = 0;
    uint32_t tex_modes = 0, dot_mapping = 0, input_texture = 0, compare_mode = 0;
    // fixed function stages: colorop, arg1, arg2, arg0, alphaop, arg1, arg2, arg0, result
    uint32_t ff[4][9] = {};
    TexInfo tex[4];
    uint32_t alphakill = 0;  // bit per stage
    uint32_t flat = 0, two_sided = 0;
    uint32_t proj = 0;       // fixed function: bit per stage sampled at coordinates divided by w
    uint32_t quad = 0;       // a screen quad: QuadHlsl
    uint32_t lit = 0;        // per pixel lighting: the vertex program's PixelLightingId
    uint32_t soft = 0;       // a soft particle: faded where it meets the scene's depth (t6, layout2d.z, surf.w)
    uint32_t gbuf = 0;       // a material drawn to the scene: its normal, depth, roughness and reflectivity to SV_Target1
    bool operator==(const PixelKey& o) const;
};

uint64_t Hash(const void* p, size_t n, uint64_t h = 1469598103934665603ull);

PixelKey MakePixelKey(const State& s, const TexInfo tex[4]);

// vertex program microcode to HLSL, regs is the bit mask of v registers the draw supplies
// decode: VertexDecode per register, 4 bits each, for elements the input assembler hands over raw
std::string VertexProgramHlsl(const std::vector<uint32_t>& program, uint16_t regs, uint64_t decode = 0);
// a vertex program whose vertex colour is lit from a normal and constants alone: an id for PixelKey::lit (the pixel
// shader lights each pixel), 0 for one that is not
uint32_t PixelLightingId(const std::vector<uint32_t>& program, uint16_t regs);
std::string PixelLightingCode(uint32_t id);
std::string LightingReport(const std::vector<uint32_t>& program, uint16_t regs);  // tests: how a program lights
// A program lit by one directional light it is given in its model's space, and placed by a matrix it is given
// (a static model's): the constants, as hardware registers (D3D's plus 96). ok false for any other program.
struct ProgramLightInfo {
    bool ok = false;
    uint16_t dir = 0;      // towards the light, model space
    uint16_t color = 0;
    uint16_t ambient = 0;
    uint16_t pos[4] = {};  // the clip position's x y z w: dot products of the model's position with these
};
ProgramLightInfo ProgramLight(const std::vector<uint32_t>& program, uint16_t regs);
// a program's position: the constants whose dp4s with an input make the clip position's x y z w (any program, lit
// or not: the glowing particles' for their lights)
struct ProgramPositionInfo {
    bool ok = false;
    uint16_t rows[4] = {};
    uint16_t input = 0;  // the input register dotted with them
};
ProgramPositionInfo ProgramPosition(const std::vector<uint32_t>& program, uint16_t regs);
// A program decoded to run on the CPU, for its oPos alone (the glowing particles' places: their programs read them
// from the constants by a0, the vertices only number them)
struct CpuProgram;
// the weapon in hand's geometry shader on a wide screen: its triangles that show nothing of the 4:3 view are dropped
std::string WeaponCullGsHlsl();
// PN triangle tessellation of a lit program's triangles (TessRows: the rows its position takes, ok)
std::string TessHlsl();
struct TessInfo {
    bool ok = false;
    uint16_t rows[4] = {};
    uint16_t offset = 0;  // the constant added to h.xyz / h.w (the viewport's offset)
    bool skinned = false;  // its matrices read by a0 (a character's bones), not a rigid model's one placement
};
TessInfo TessRows(const std::vector<uint32_t>& program, uint16_t regs);
std::shared_ptr<const CpuProgram> DecodeCpuProgram(const std::vector<uint32_t>& program, uint16_t regs);
// one vertex: its inputs (v0 to v15), the constants in hardware order; oPos as the program writes it
void RunCpuProgram(const CpuProgram& p, const float in[16][4], const std::array<std::array<float, 4>, 192>& c, float opos[4]);
// Shadows (shadows.h): a vertex shader as the shadow casters' variant. Its constant buffer b0 runs past the
// program's 192 constants: the cascades' view to light clip matrices (shc), their atlas tiles (sht: clip xy scale,
// offset) and shv (1 / the projection's x and y scale, the first cascade drawn). Drawn instanced, one instance a
// cascade, to the atlas tile, clipped to it.
std::string ShadowVertexHlsl(std::string vs);
// the casters' pixel shader where the draw's alpha test cuts the shape out of stage 0's texture (proj: coordinates
// divided by w)
std::string ShadowAlphaHlsl(bool proj);
// The motion variant of a draw's vertex shader (d3d12.cpp MotionPass): its main run twice, with this frame's constants
// and the frame before's (b2 its vertex constants, b3 its draw constants: wvp at the float4 wvp_at), out both clip
// positions; entry "motion"
std::string MotionVertexHlsl(const std::string& vs, unsigned wvp_at);
std::string GuardVertexHlsl(std::string vs);
// The top view variant (d3d12.cpp WaterInteraction): the draw seen from above onto the wave field's square (b2:
// view to the square's clip, the world's height from view space, 1 / P00, 1 / P11), out the world's height
std::string TopVertexHlsl(std::string vs);
std::string PointSpriteGsHlsl();
std::string DrawHlslHead();  // the draws' shared HLSL head: VOut and the constants
// the guard band (d3d12.cpp DrawGuard): the world's depth drawn again with the view widened, past the screen's edges,
// for the screen space passes (scene_shaders.h kSceneAo): the screen's uv u is the band's (u - 0.5) * scale + 0.5
inline constexpr float kGuardScale = 0.6666667f;
// level 1's sky pieces (d3d12.cpp sky_phase_): the depth of each a step nearer than the one before, from the far plane
inline constexpr float kSkyLayerStep = 4.76837158e-7f;  // 2^-21: 8 steps of a 24 bit depth
inline constexpr uint32_t kShadowConstantsTail = 512;  // bytes the vertex constants' upload keeps past the 192
// fixed function vertex state a shader is built for
struct FixedKey {
    uint32_t light = 0;    // bit 0 lighting, 1 specular, 2 local viewer, 3 normalize, then D3DMCS 2 bits each from bit 8:
                           // diffuse, specular, ambient, emissive (material when the vertex lacks that colour)
    uint32_t types = 0;    // D3DLIGHTTYPE per light, 2 bits each, 0 off
    uint32_t tex[4] = {};  // per stage: coordinate set, texgen at bit 4, D3DTTFF count at bit 8, projected bit 11, set size at bit 12
};
FixedKey MakeFixedKey(const State& s);
// the same for lit fixed function vertices whose material colours are not the vertices'
uint32_t FixedLightingId(const FixedKey& f, uint16_t regs);
std::string FixedVertexHlsl(uint16_t regs, bool pretransformed, uint64_t decode = 0, const FixedKey& f = {});
std::string PixelHlsl(const PixelKey& k);
// a screen quad's vertex or pixel shader: texture coordinates as the quad was drawn, before its far edges were pulled out
std::string QuadHlsl(std::string s);

// HLSL a backend extension adds; the defaults write the console path
struct ShaderHooks {
    virtual ~ShaderHooks() = default;
    // the cbuffer line for DrawConstants::ext
    virtual std::string Constants() const;
    // float4 shadow(Texture2D<float> t, SamplerComparisonState s, float2 uv, float z, int fmt, float2 px, float2 size)
    virtual std::string Shadow() const;
    // what spreadtap scales a render target's colour by
    virtual std::string ReadScale(const TexInfo&) const { return "1"; }
    // a cube sampled along dir at stage i, empty for a plain sample
    virtual std::string Cube(const TexInfo&, int, const std::string&) const { return {}; }
    // after stage i's fetch
    virtual std::string Fetched(const TexInfo&, int) const { return {}; }
    // outc to the target, after the alpha test
    virtual std::string Output() const;
};
inline const ShaderHooks* g_shader_hooks = nullptr;

// the constant buffer layout both stages share, register b1
struct DrawConstants {
    float surf[4];      // target width, height, 1 / depth scale, 0
    float fog[4];       // start, end, density, mode
    float fog_color[4];
    float misc[4];      // alpha ref, alpha func, shadow func, specular
    float pc[18][4];    // combiner c0 c1 per stage, then final c0 c1
    float tfactor[4];
    float texsize[4][4];
    float bump[4][4];   // 00 01 10 11
    float bumpl[4][4];  // scale offset
    float wvp[16];      // fixed function
    float ext[4];       // a backend extension's, declared by ShaderHooks::Constants
    float spread[4][4]; // kTexWide: tap step in uv, taps per axis
    // fixed function, lit in view space
    float wv[16];
    float wvn[16];       // normals: inverse transpose of wv
    float tm[4][16];     // texture transforms
    float mat[5][4];     // material diffuse, ambient, specular, emissive, power
    float amb[4];        // D3DRS_AMBIENT
    float lt[8][7][4];   // per light: diffuse, specular, ambient, position range, direction falloff, attenuation, cos theta/2 phi/2
    float layout[4];     // 2D on a widened screen: x times layout.x plus layout.y, console pixels (g_layout_2d); z: the
                         // weapon in hand's half width of the 4:3 view in clip space, its triangles wholly outside it culled
    // remastered material: view window xy, normal map green sign, strength; light dir xyz, specular strength;
    // the texture's normal strength, parallax depth, specular, detail; unused
    float mtl[4][4];
    float mrefl[4];      // the material's reflectivity, metal (remaster::Material), 0, 0
    float tess[4];       // tessellation: the program's position rows (c[] indices) for x y z w
    float tess2[4];      // its offset constant's index
    float jitter[4];     // temporal anti-aliasing: the frame's sub pixel offset, clip space xy (the 3D's draws)
    float pts0[4];       // points (PointSpriteGsHlsl): D3DRS_POINTSIZE, its min, max, the scale on
    float pts1[4];       // the scale's A, B, C; point sprites on
    float flow[4];       // falling water (Water): the time (0 none), the texture's speed along v, the brightness, the opacity
    // Water Interaction (d3d12.cpp WaterInteraction): the world's x and z from view space (columns of the view's
    // inverse: xyz also the world's x and z axes in view space), the wave field's corner x z, 1 / its size, the
    // slope's scale (0: none)
    float wave[3][4];
    // Snow Tracks (d3d12.cpp SnowTracks): the trail's corner x z, 1 / its size, the strength (0: none); the world's
    // height from view space (wave[0], wave[1] its x and z)
    float trail[2][4];
    float dtl[4];        // Detail Textures: the grain's slice in the detail array (-1 none), its strength; Character Detail's
                         // tiling over the texture; the grain's tile in the texture's texels
    // the materials' highlight out of the sun's shadow (the last frame's mask, d3d12.cpp BindSunVisibility): the view
    // space now to the clip space of the frame before, and to this frame's (rows; its pixel moved by the difference)
    float sunrp[2][16];
};

}  // namespace gpu
