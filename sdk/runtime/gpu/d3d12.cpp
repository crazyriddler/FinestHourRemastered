// The title's Direct3D 8 calls drawn with Direct3D 12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <fstream>
#include <functional>
#include <thread>
#include <iterator>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <unordered_set>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "audio/audio.h"
#include "core/control.h"
#include "core/diag.h"
#include "core/sampler.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/ext.h"
#include "gpu/post_shaders.h"
#include "gpu/remaster.h"
#include "gpu/scene_shaders.h"
#include "gpu/smaa_textures.h"
#include "gpu/motion_shaders.h"
#include "gpu/shaders.h"
#include "gpu/shadow_shaders.h"
#include "gpu/shadows.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr uint64_t kStaleFrames = 8;  // frames a target may go unused and still be read back
constexpr uint64_t kUploadSize = 64ull << 20;
// a draw starting with less than this left in the frame's ring waits for the GPU and starts the ring over
constexpr uint64_t kUploadReserve = 8ull << 20;
constexpr uint32_t kSrvCount = 3 * 32768;
constexpr uint32_t kSamplerCount = 2048;
constexpr uint32_t kRtvCount = 1024;
// views of the passes on the shown frame, after the back buffers': SMAA edges, weights and blend, sharpened; then
// the scene passes': its copy, two occlusion targets, four bloom levels
constexpr uint32_t kPostRtvs = 4, kSceneRtvs = 14;
constexpr uint32_t kSceneVel = 12;   // full size: the screen's velocity (MotionPass)
constexpr uint32_t kSceneHist = 13;  // full size, the scene's format: the temporal anti-aliasing's history
constexpr uint32_t kSceneFog = 10;  // half size: the volumetric haze (FogPass)
constexpr uint32_t kSceneGi = 11;   // half size: one bounce of light (kSceneAo's gi)
constexpr uint32_t kSceneBloomEnd = 7, kSceneShadow = 7;
constexpr uint32_t kSceneRays = 8, kSceneDof = 9;  // half size: the sun's shafts, the blurred scene for aiming
constexpr uint32_t kSkyConstantsAt = 86 * 4;  // floats: kSceneCommon's sky[6] after p, lcount, lpos, lcol, heat, heatz
constexpr uint32_t kGradeAt = kSkyConstantsAt + 24;  // floats: kSceneCommon's grade[4], after sky[6] (the 1536 bytes' end)
constexpr uint32_t kMsaaKey = 0x1000;         // PsoKey::topo: drawn into the scene's multisampled targets (MSAA), the
                                              // sample count at bits 16..19
constexpr uint32_t kPointSpriteKey = 0x100000;  // PsoKey::topo: points, each a square through PointSpriteGsHlsl
constexpr uint32_t kA2cKey = 0x2000;          // PsoKey::topo: cut out by its alpha, the cut's edge alpha to coverage
constexpr uint32_t kTessKey = 0x800;          // PsoKey::topo: PN triangle tessellation (TessHlsl)
constexpr uint32_t kGbufKey = 0x400;          // PsoKey::topo: a material's second target, the reflection buffer
constexpr uint32_t kWeaponCullKey = 0x200;    // PsoKey::topo: the weapon in hand's triangles through WeaponCullGsHlsl
constexpr uint32_t kVeilKey = 0x8000;         // ShadowKey::topo: a see-through draw's veil over the occlusion (DrawVeil)
constexpr uint32_t kGuardKey = 0x4000;        // ShadowKey::topo: the guard band's depth pipeline (DrawGuard)
constexpr uint32_t kTopKey = 0x10000;        // ShadowKey::topo: seen from above onto the wave field (WaterInteraction)
constexpr uint32_t kTopWaterKey = 0x20000;   // ShadowKey::topo: and it is the water (its height), not what crosses it
constexpr uint32_t kTrailSize = 2048;         // Snow Tracks: the trail's texels a side
constexpr float kTrailCell = 2.5f;            // and a texel's size in the world (cm)
constexpr uint32_t kWaveSize = 512;          // Water Interaction: the wave field's texels a side
constexpr float kWaveCell = 6;               // and a texel's size in the world (cm)
constexpr uint32_t kShadowScreenKey = 0x100;  // ShadowKey::topo: a character screen mask pipeline  // scene_: the bloom's levels end at 7, then the shadow mask
constexpr uint32_t kDsvCount = 256;
constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;

// perf diagnostic: where a frame's time goes, logged once a second with diag::Perf
using PerfClock = std::chrono::steady_clock;
double PerfMs(PerfClock::time_point t) {
    return std::chrono::duration<double, std::milli>(PerfClock::now() - t).count();
}
struct Perf {
    // milliseconds
    double draw = 0;
    double assemble = 0;
    double bind = 0;
    double sig = 0;
    double texup = 0;
    double compile = 0;
    double clear = 0;
    double copy = 0;
    double present = 0;
    double wait = 0;
    double swap = 0;
    double sync = 0;
    // counts and bytes
    uint64_t draws = 0;
    uint64_t verts = 0;
    uint64_t upload = 0;
    uint64_t overflow = 0;
    uint64_t sigs = 0;
    uint64_t sig_bytes = 0;
    uint64_t texups = 0;
    uint64_t texup_bytes = 0;
    uint64_t newtex = 0;
    uint64_t targets = 0;
    uint64_t rtups = 0;
    uint64_t shrinks = 0;
    uint64_t syncs = 0;
    uint64_t psos = 0;
    uint64_t shaders = 0;
    uint64_t raw = 0;
    uint64_t lates = 0;
    uint64_t srv_hits = 0;  // draws whose texture table was made already
};
Perf g_perf;
// the per draw timers (draw, assembly, binding, signatures, clears, copies) only for the perf log (XBE_PERF): two
// clock reads a timer, thousands a frame; the others (compiles, uploads, waits, syncs, presents) always, the frame
// times' report and the hitch log read them
const bool g_draw_timers = diag::Perf();
struct PerfScope {
    double& acc;
    bool on;
    PerfClock::time_point t;
    PerfScope(double& a, bool o = true) : acc(a), on(o), t(o ? PerfClock::now() : PerfClock::time_point{}) {}
    ~PerfScope() {
        if (on) acc += PerfMs(t);
    }
};

// F3 captures only with diag::CaptureDir set
bool CaptureOn() {
    if (diag::CaptureDir().empty()) LOG_FIRST(1, "capture: set XBE_CAPTURE_DIR");
    return !diag::CaptureDir().empty();
}

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";


const char* kTvHlsl = R"(
cbuffer B : register(b1) { float4 src_rect; float4 screen; float4 opts; float4 size; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
Texture2D frame : register(t2);
Texture2D ui : register(t3);  // the overlay's rows in the picture (premultiplied), over the 4:3 picture
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
// the rounded rectangle of the screen's opening: signed distance in the frame's height units
float Opening(float2 q) {
    float2 c = (screen.xy + screen.zw) * 0.5, h = (screen.zw - screen.xy) * 0.5;
    float2 d = abs(q - c) * float2(16.0 / 9.0, 1) - h * float2(16.0 / 9.0, 1) + opts.y;
    return length(max(d, 0)) + min(max(d.x, d.y), 0) - opts.y;
}
float3 Picture(float2 s) {
    // a slightly curved tube: the picture bulges towards the edges
    float2 c = s * 2 - 1;
    c *= 1 + opts.z * float2(c.y * c.y, c.x * c.x);
    // the corners the bulge pushes out of the picture hold its edge (the glass's round corners cover them)
    s = saturate(c * 0.5 + 0.5);
    float2 uv = src_rect.xy + s * src_rect.zw;
    // a soft tube: the picture blurred by a third of a source line, which hides small pictures' pixels
    float2 px = size.zw;  // half a console pixel
    float3 col = img.SampleLevel(ls, uv, 0).rgb * 0.4;
    col += img.SampleLevel(ls, uv + float2(px.x, 0), 0).rgb * 0.15;
    col += img.SampleLevel(ls, uv - float2(px.x, 0), 0).rgb * 0.15;
    col += img.SampleLevel(ls, uv + float2(0, px.y), 0).rgb * 0.15;
    col += img.SampleLevel(ls, uv - float2(0, px.y), 0).rgb * 0.15;
    // the overlay's rows drawn in the picture, as soft
    float2 up = float2(0.5 / 640, 0.5 / 480);
    float4 o = ui.SampleLevel(ls, s, 0) * 0.4 + (ui.SampleLevel(ls, s + float2(up.x, 0), 0) + ui.SampleLevel(ls, s - float2(up.x, 0), 0) +
               ui.SampleLevel(ls, s + float2(0, up.y), 0) + ui.SampleLevel(ls, s - float2(0, up.y), 0)) * 0.15;
    col = col * (1 - o.a) + o.rgb;
    if (opts.x > 0) col = float3(lut.Sample(ps, float2(col.r, 0.5)).r, lut.Sample(ps, float2(col.g, 0.5)).g, lut.Sample(ps, float2(col.b, 0.5)).b);
    // faint scan lines at 480, a little bloom of the bright lines over them
    float scan = 0.5 + 0.5 * cos(s.y * 480 * 6.2831853);
    float lum = dot(col, float3(0.3, 0.59, 0.11));
    col *= lerp(1, 0.9 + 0.1 * scan, opts.w * (1 - lum * 0.6));
    // the tube's corners darker
    float2 v = s * (1 - s);
    col *= pow(saturate(v.x * v.y * 18), 0.18);
    return col;
}
float4 psmain(O i) : SV_Target {
    float2 q = i.uv;
    float3 col = 0;
    // the picture in the screen's rectangle; the frame's glass (its alpha) gives the tube's shape
    float2 s = (q - screen.xy) / (screen.zw - screen.xy);
    if (all(s >= -0.01) && all(s <= 1.01)) col = Picture(saturate(s));
    float4 f = frame.SampleLevel(ls, q, 0);
    return float4(lerp(col, f.rgb, f.a), 1);
}
)";

const char* kDepthHlsl = R"(
Texture2D<float> img : register(t0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth { return img.SampleLevel(ps, i.uv, 0); }
)";

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

// D3DBLEND, the console uses GL values
constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

D3D12_BLEND Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return D3D12_BLEND_ZERO;
        case 1: return D3D12_BLEND_ONE;
        case 0x300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case 0x301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case 0x302: return D3D12_BLEND_SRC_ALPHA;
        case 0x303: return D3D12_BLEND_INV_SRC_ALPHA;
        case 0x304: return D3D12_BLEND_DEST_ALPHA;
        case 0x305: return D3D12_BLEND_INV_DEST_ALPHA;
        case kBlendDestColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case 0x307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case 0x308: return D3D12_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
        default: return D3D12_BLEND_ONE;
    }
}

// D3DBLENDOP, GL values
D3D12_BLEND_OP BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 0x8007: return D3D12_BLEND_OP_MIN;
        case 0x8008: return D3D12_BLEND_OP_MAX;
        default: return D3D12_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207, D3D12's order from 1
D3D12_COMPARISON_FUNC Cmp(uint32_t v) {
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return D3D12_COMPARISON_FUNC(n + 1);
}

// D3DSTENCILOP, GL values
D3D12_STENCIL_OP StencilOp(uint32_t v) {
    switch (v) {
        case 0: return D3D12_STENCIL_OP_ZERO;
        case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D12_STENCIL_OP_INVERT;
        case 0x8507: return D3D12_STENCIL_OP_INCR;
        case 0x8508: return D3D12_STENCIL_OP_DECR;
        default: return D3D12_STENCIL_OP_KEEP;
    }
}

// D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 clamp to edge
D3D12_TEXTURE_ADDRESS_MODE Address(uint32_t v) {
    switch (v) {
        case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

DXGI_FORMAT HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return DXGI_FORMAT_BC1_UNORM;
        case Host::BC2: return DXGI_FORMAT_BC2_UNORM;
        case Host::BC3: return DXGI_FORMAT_BC3_UNORM;
        case Host::R32F: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// rows of a texture upload start D3D12_TEXTURE_DATA_PITCH_ALIGNMENT apart
uint32_t UploadPitch(uint32_t row_bytes) { return (row_bytes + 255) & ~255u; }

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return bd;
}

// one level, one sample: callers set Flags where they differ
D3D12_RESOURCE_DESC Texture2DDesc(DXGI_FORMAT fmt, uint64_t w, uint32_t h) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    return td;
}

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    g_perf.sigs++;
    g_perf.sig_bytes += total <= kSigWholeMax ? total : kSigSamples * kSigSampleBytes;
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

struct Tex {
    ComPtr<ID3D12Resource> res;
    TextureDesc d;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
    uint8_t flags = 0;  // Extension::Decoded's
    uint64_t id = 0;    // remaster content id of the top level, 0 for none
};

// a remastered texture on the GPU: colour, and for a material its normal and material maps
class D3D12Backend;
D3D12Backend* g_ui_backend = nullptr;  // inside the overlay's frame
std::mutex g_override_lock;
std::vector<TextureOverride> g_override_set;
bool g_override_new = false;

struct RemasterTex {
    ComPtr<ID3D12Resource> color, normal, mat;
    DXGI_FORMAT color_fmt = DXGI_FORMAT_UNKNOWN, normal_fmt = DXGI_FORMAT_UNKNOWN, mat_fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t color_mips = 1, normal_mips = 1, mat_mips = 1;
    const remaster::Material* m = nullptr;
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
};

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;       // through the gamma lookup
    bool alpha = false;       // the source's alpha, else 1
    uint32_t taps = 1;        // n x n box filter per pixel
    float mul = 1;            // colour scale
    DXGI_FORMAT rtv = kBgra;  // the target's format
    bool over = false;        // premultiplied over the target, which keeps its alpha
};

// Shrink's settings past the source and the size
struct ShrinkOptions {
    uint32_t taps = 1;
    // into: a copy from an earlier call to draw over again, in state into_state
    ID3D12Resource* into = nullptr;
    D3D12_RESOURCE_STATES into_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    float mul = 1;
};

// a draw's four texture stages: BindStageTexturesAndTargets fills them, the constants, views and samplers read them
struct DrawTextures {
    TexInfo info[4];
    D3D12_SHADER_RESOURCE_VIEW_DESC sv[4];
    ID3D12Resource* res[4];
    bool rt[4] = {};            // stage samples a render target
    float k[4] = {1, 1, 1, 1};  // Extension::Sample's k
    float wide[4] = {};         // host texels per console texel of a target read into a smaller one, 0 otherwise
    const RemasterTex* mat = nullptr;  // the remastered material of one stage
    int mat_stage = -1;
    bool hd[4] = {};  // the stage's texture is a replacement at a higher resolution (the pack's, an override)
    uint64_t id[4] = {};  // the stage's texture's content id
    bool flat = false;  // a 2D draw
    bool weapon = false;  // the weapon in hand (after the world's depth was cleared): its remastered textures softer
};

// a draw's shaders: what they are made from, their cache keys, the hashes Compile returned and the compiled blobs
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    ID3DBlob* vs_code = nullptr;
    ID3DBlob* ps_code = nullptr;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

struct ShaderProgress;

// An open addressing table of 64 bit keys (hashes) to values, emptied by a stamp: no allocation once grown (the
// frame's bookkeeping, thousands of entries a frame). Get inserts a value-initialised entry when the key is new.
template <typename V>
struct StampTable {
    struct Slot {
        uint64_t key = 0;
        uint32_t stamp = 0;
        V v{};
    };
    std::vector<Slot> slots = std::vector<Slot>(8192);
    uint32_t stamp = 1;
    size_t used = 0;
    void Clear() {
        used = 0;
        if (++stamp == 0) {  // (wrapped: the old stamps could match again)
            for (Slot& sl : slots) sl.stamp = 0;
            stamp = 1;
        }
    }
    V* Find(uint64_t key) {
        size_t mask = slots.size() - 1;
        for (size_t i = size_t(key) & mask;; i = (i + 1) & mask) {
            Slot& sl = slots[i];
            if (sl.stamp != stamp) return nullptr;
            if (sl.key == key) return &sl.v;
        }
    }
    V& Get(uint64_t key) {
        if ((used + 1) * 2 > slots.size()) Grow();
        size_t mask = slots.size() - 1;
        for (size_t i = size_t(key) & mask;; i = (i + 1) & mask) {
            Slot& sl = slots[i];
            if (sl.stamp != stamp) {
                sl.key = key, sl.stamp = stamp, sl.v = V{};
                used++;
                return sl.v;
            }
            if (sl.key == key) return sl.v;
        }
    }
    void Grow() {
        std::vector<Slot> old(slots.size() * 2);
        old.swap(slots);
        size_t mask = slots.size() - 1;
        for (Slot& o : old) {
            if (o.stamp != stamp) continue;
            size_t i = size_t(o.key) & mask;
            while (slots[i].stamp == stamp) i = (i + 1) & mask;
            slots[i] = o;
        }
    }
};

class D3D12Backend final : public Backend, public Context {
public:
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;

    ID3D12Device* Device() override { return dev_.Get(); }
    ID3D12GraphicsCommandList* List() override { return cl_.Get(); }
    void Release(ComPtr<ID3D12Resource> r) override { CurrentFrame().garbage.push_back(std::move(r)); }
    D3D12_CPU_DESCRIPTOR_HANDLE View(const Target& t) override { return Cpu(rtv_heap_.Get(), t.view, rtv_inc_); }
    void Unbind() override { bound_rt_ = bound_ds_ = nullptr; }
    void Capture(Target& t, const char* tag, float mul) override;
    float Scale() const override { return scale_; }
    void GuestSize(uint32_t& w, uint32_t& h) const override {
        w = guest_w_;
        h = guest_h_;
    }
    uint64_t Presents() const override { return presents_; }

private:
    struct Frame {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12Resource> upload;
        uint8_t* map = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS va = 0;  // the ring's
        uint64_t used = 0;
        uint64_t fence = 0;
        std::vector<ComPtr<ID3D12Resource>> garbage;
        uint32_t srv = 0;
    };
    struct Alloc {
        ID3D12Resource* res;
        uint64_t off;
        uint8_t* cpu;
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
    };

    Alloc Upload(uint64_t size, uint64_t align = 256);
    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    uint32_t AllocSrvSlots(uint32_t n);
    // the heaps' starts, asked of the runtime once (thousands of times a frame otherwise)
    struct HeapStart {
        ID3D12DescriptorHeap* h = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE c{};
        D3D12_GPU_DESCRIPTOR_HANDLE g{};
        bool has_g = false;
    };
    HeapStart heap_starts_[8];
    HeapStart& StartOf(ID3D12DescriptorHeap* h) {
        for (HeapStart& s : heap_starts_) {
            if (s.h == h) return s;
            if (!s.h) {
                s.h = h, s.c = h->GetCPUDescriptorHandleForHeapStart();
                return s;
            }
        }
        static thread_local HeapStart spare;  // (more heaps than kept: asked each time)
        spare = {h, h->GetCPUDescriptorHandleForHeapStart()};
        return spare;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto c = StartOf(h).c;
        c.ptr += size_t(i) * inc;
        return c;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        HeapStart& s = StartOf(h);
        if (!s.has_g) s.g = h->GetGPUDescriptorHandleForHeapStart(), s.has_g = true;  // (shader visible heaps alone)
        auto g = s.g;
        g.ptr += uint64_t(i) * inc;
        return g;
    }
    // The command list's state as last set (the draws set the same pipeline, viewport, scissor, blend factor,
    // stencil reference, topology and sampler table again and again): set only when it changes. Forgotten at each
    // list's start (Begin), the samplers' table at a root signature's change, all after the UI (ImGui sets its own).
    struct CmdState {
        ID3D12PipelineState* pso = nullptr;
        bool pso_ok = false, vp_ok = false, sc_ok = false, bf_ok = false, sref_ok = false, topo_ok = false, smp_ok = false;
        D3D12_VIEWPORT vp{};
        D3D12_RECT sc{};
        float bf[4]{};
        UINT sref = 0;
        D3D_PRIMITIVE_TOPOLOGY topo{};
        UINT64 smp = 0;
    } cmd_;
    uint64_t cmd_issued_ = 0, cmd_skipped_ = 0;
    void SetPso(ID3D12PipelineState* p) {
        if (cmd_.pso_ok && p == cmd_.pso) return cmd_skipped_++, void();
        cl_->SetPipelineState(p), cmd_.pso = p, cmd_.pso_ok = true, cmd_issued_++;
    }
    void SetViewport(const D3D12_VIEWPORT& v) {
        if (cmd_.vp_ok && !std::memcmp(&v, &cmd_.vp, sizeof(v))) return;
        cl_->RSSetViewports(1, &v), cmd_.vp = v, cmd_.vp_ok = true;
    }
    void SetScissor(const D3D12_RECT& r) {
        if (cmd_.sc_ok && !std::memcmp(&r, &cmd_.sc, sizeof(r))) return;
        cl_->RSSetScissorRects(1, &r), cmd_.sc = r, cmd_.sc_ok = true;
    }
    void SetBlendFactor(const float f[4]) {
        if (cmd_.bf_ok && !std::memcmp(f, cmd_.bf, sizeof(cmd_.bf))) return;
        cl_->OMSetBlendFactor(f), std::memcpy(cmd_.bf, f, sizeof(cmd_.bf)), cmd_.bf_ok = true;
    }
    void SetStencilRef(UINT r) {
        if (cmd_.sref_ok && r == cmd_.sref) return;
        cl_->OMSetStencilRef(r), cmd_.sref = r, cmd_.sref_ok = true;
    }
    void SetTopology(D3D_PRIMITIVE_TOPOLOGY t) {
        if (cmd_.topo_ok && t == cmd_.topo) return;
        cl_->IASetPrimitiveTopology(t), cmd_.topo = t, cmd_.topo_ok = true;
    }
    void SetSamplerTable(D3D12_GPU_DESCRIPTOR_HANDLE h) {
        if (cmd_.smp_ok && h.ptr == cmd_.smp) return;
        cl_->SetGraphicsRootDescriptorTable(3, h), cmd_.smp = h.ptr, cmd_.smp_ok = true;
    }
    // every resource made here: an epoch (a released resource's address may come back as another's)
    std::atomic<uint64_t> res_epoch_{0};
    template <typename... A>
    HRESULT CreateResource(A&&... args) {
        res_epoch_++;
        return dev_->CreateCommittedResource(std::forward<A>(args)...);
    }
    // The draws' texture tables (SetTextureViewsAndSamplers): seven views a draw, the same set again for most draws
    // of a frame; a table made once in the frame's descriptor ring is used again while the ring and the resources
    // are as they were (ring_gen_, res_epoch_). The key is the views' bytes: alike, the table is the same.
    struct SrvTable {
        uint64_t key = 0, gen = 0;
        uint32_t slot = 0;
    };
    std::vector<SrvTable> srv_tables_ = std::vector<SrvTable>(4096);
    uint64_t srv_gen_ = 1, srv_ring_gen_ = ~0ull, srv_epoch_ = ~0ull;
    uint64_t srv_last_key_ = 0;
    uint32_t srv_last_slot_ = 0;
    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) override;
    void Begin();
    void Submit(bool wait);
    void WaitFence(uint64_t value);
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h);
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], bool gamma, bool alpha, uint32_t taps = 1,
              float mul = 1, DXGI_FORMAT rtv = kBgra) override {
        Blit(src, fmt, uvrect, BlitOptions{.gamma = gamma, .alpha = alpha, .taps = taps, .mul = mul, .rtv = rtv});
    }
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o);
    uint32_t EnsureBlitSamplers();
    void DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                        std::optional<uint32_t> sampler_slot = {});
    ID3D12Resource* Feedback(Target& t) override;
    void Resolve(Target& t) {
        if (t.range != 1 && ext_) ext_->Resolve(*this, t);
    }
    // Pass, then its step
    void Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w,
              uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3], const float p[8],
              float blend) override;
    ID3D12PipelineState* EnsurePassPipeline(const char* hlsl, const char* entry, DXGI_FORMAT rtv, float blend);
    uint32_t pass_floats_ = 8;  // the next Pass's constants (floats of p), then 8 again
    // Draw, in call order, each callee right after its caller
    bool DepthCopy(const State& s);
    ID3D12PipelineState* EnsureDepthCopyPipeline();
    void DrawDepthCopy(Target& src, Target& dst, ID3D12PipelineState* pso);
    bool AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                      Assembled& a);
    bool BindTargets(const State& s);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    ID3D12Resource* BindTexture(const State& s, int stage, DrawTextures& stages);
    ID3D12Resource* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                      DrawTextures& stages);
    ID3D12Resource* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                     DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours,
                       bool same);
    void UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    ID3D12PipelineState* EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps);
    void FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const;
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestExtent size, DrawConstants& c);
    void FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c);
    void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c);
    void CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                     const DrawTextures& stages);
    void CaptureTextures(const State& s, const DrawTextures& stages);
    void EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c);
    void CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c);
    void CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
    void CaptureShader(const char* kind, uint64_t key, const std::string& src);
    void SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages);
    uint32_t EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp);
    void SetViewportScissor(const State& s, uint32_t gw);
    void SetBlendFactorStencilRef(const State& s);
    void DrawGeometry(const Assembled& a);
    void UploadVertexBuffers(const Assembled& a);
    // shaders by source hash: Compile returns the bytecode and sets hash
    ID3DBlob* Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash);
    // Blit's pipeline
    ID3D12PipelineState* EnsureBlitPipeline(DXGI_FORMAT rtv, bool over = false);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv) const;
    // Init, in order, each callee right after its caller
    bool CreateDevice();
    bool CreateSwapChain(HWND hwnd);
    void CreateHeaps();
    uint32_t CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible, ComPtr<ID3D12DescriptorHeap>& out);
    void CreateBackBufferViews();
    bool CreateRootSignature();
    void CreateFrames();
    void CreateGammaLut();
    void LoadSettings();
    // Present, in call order, each callee right after its caller; DrawFrame runs the two after it
    void FinishCapture(const Surface& front);
    void StartCapture();
    void ResizeSwap();
    void UploadGammaRamp();
    void DrawFrame(Target* t, uint32_t bi);
    void BlitFrame(Target& t, ID3D12Resource* src, DXGI_FORMAT fmt);
    // the passes on the shown frame (SMAA, sharpening): what to show, readable; t's own resource without any
    ID3D12Resource* PostProcess(Target& t, DXGI_FORMAT& fmt);
    bool EnsurePostTargets(DXGI_FORMAT fmt, uint32_t w, uint32_t h);
    bool EnsureSmaaLookups();
    D3D12_CPU_DESCRIPTOR_HANDLE PostRtv(uint32_t i) { return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + i, rtv_inc_); }
    ComPtr<ID3D12Resource> post_[kPostRtvs];
    D3D12_RESOURCE_STATES post_state_[kPostRtvs] = {};
    uint32_t post_w_ = 0, post_h_ = 0;
    DXGI_FORMAT post_fmt_ = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12Resource> smaa_lut_[2];  // area, search
    // the remaster pack's textures by content id, made as their reads finish
    std::unordered_map<uint64_t, RemasterTex> remaster_;
    // the scene's passes before the 2D (ScenePost): the 3D drawn this frame to a screen target with its depth
    Target* scene_rt_ = nullptr;
    Target* scene_ds_ = nullptr;
    bool scene_done_ = false;
    float hurt_shown_ = 0;  // the pain the scene shakes by (g_hurt, let go of slowly)
    bool occlusion_done_ = false;
    bool hdr_ = false;  // the screen targets were made in 16 bit float
    ComPtr<ID3D12Resource> scene_[kSceneRtvs];
    D3D12_RESOURCE_STATES scene_state_[kSceneRtvs] = {};
    uint32_t scene_w_ = 0, scene_h_ = 0;
    DXGI_FORMAT scene_fmt_ = DXGI_FORMAT_UNKNOWN;
    D3D12_CPU_DESCRIPTOR_HANDLE SceneRtv(uint32_t i) { return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kPostRtvs + i, rtv_inc_); }
    bool EnsureSceneTargets(DXGI_FORMAT fmt, uint32_t w, uint32_t h);
    void ScenePass(const std::string& hlsl, const char* entry, uint32_t out, DXGI_FORMAT out_fmt, uint32_t w, uint32_t h,
                   std::initializer_list<std::pair<ID3D12Resource*, DXGI_FORMAT>> src, const float p[16],
                   float blend = -1);
    void ScenePost();
    void SceneOcclusion();
    void SceneComposite(Target& rt, const char* entry, ID3D12Resource* occ, bool bloom, const float p[16], ID3D12Resource* depth,
                        ID3D12Resource* shadow = nullptr, bool lights = false);
    void SceneParams(const Target& rt, float p[16]);
    void CopyScene(Target& rt);
    void PumpRemaster();
    ComPtr<ID3D12Resource> UploadImage(const remaster::Image& im);
    struct Streamer;
    std::unique_ptr<Streamer> streamer_;
    ComPtr<ID3D12Resource> UploadImages(const remaster::Image* const* list, uint32_t count);
    // the game's replacements by id (SetTextureOverrides)
    struct OverrideTex {
        ComPtr<ID3D12Resource> res;
        uint32_t mips = 1;
    };
    std::unordered_map<uint64_t, OverrideTex> overrides_;
    void PumpOverrides();
    bool draw_character_ = false;  // the draw's program is lit per pixel: a character
    // Shadows (shadows.h). The frame's casters: the 3D drawn to the screen with a depth write before the occlusion,
    // drawn again from the light, once a cascade (instances), into the atlas; their uploads stay in the frame's ring
    struct Caster {
        ID3D12PipelineState* pso;
        D3D12_VERTEX_BUFFER_VIEW vb[16];
        uint32_t vb_count;
        D3D12_INDEX_BUFFER_VIEW ib;
        bool indexed;
        bool tess;  // the screen mask drawn as the character was: tessellated patches
        uint32_t count;
        int32_t base;
        D3D_PRIMITIVE_TOPOLOGY topo;
        D3D12_GPU_VIRTUAL_ADDRESS vc, dc;
        uint8_t* tail;  // the vertex constants' tail: the cascades, written once they are fitted
        D3D12_GPU_DESCRIPTOR_HANDLE srv, smp;
        bool alpha;
        ID3D12PipelineState* screen;  // a character's: its screen mask pipeline
        ID3D12PipelineState* guard;   // the guard band's (DrawGuard)
        ID3D12PipelineState* top;     // seen from above, its cover of the water (WaterInteraction)
        float zmin, zmax;             // its viewport's depth range
    };
    std::vector<Caster> casters_;
    // The veil (DrawVeil): the world's see-through draws (smoke, dust: blended over, no depth write) again, before
    // the occlusion is laid on the scene: where they cover it the occlusion fades, the corners' shade is behind them
    struct VeilDraw {
        ID3D12PipelineState* pso;
        D3D12_VERTEX_BUFFER_VIEW vb[16];
        uint32_t vb_count;
        D3D12_INDEX_BUFFER_VIEW ib;
        bool indexed;
        uint32_t count;
        int32_t base;
        D3D_PRIMITIVE_TOPOLOGY topo;
        D3D12_GPU_VIRTUAL_ADDRESS vc, dc;
        D3D12_GPU_DESCRIPTOR_HANDLE srv, smp;
    };
    std::vector<VeilDraw> veil_;
    uint64_t veil_gen_ = 0;
    uint64_t veil_ps_ = 0;
    std::unordered_map<uint64_t, uint32_t> lit_ids_;
    uint64_t targets_version_ = 0;
    // the draw before's keys and what they found (EnsureStageSamplers, EnsurePipeline, the pixel key's hash)
    D3D12_SAMPLER_DESC last_sd_[4]{};
    uint32_t last_smp_base_ = 0;
    bool last_smp_ok_ = false;
    PsoKey last_pso_key_{};
    ID3D12PipelineState* last_pso_ = nullptr;
    bool last_pso_ok_ = false;
    PixelKey last_pk_{};
    uint64_t last_pkey_ = 0;
    bool last_pk_ok_ = false;  // targets_ made or dropped (a Target* kept from before may be gone)
    Perf rec_start_perf_;  // g_perf when the frame times' recording began
    // the game's camera and light for the materials (g_material_frame): copied once a frame, at its first use, not
    // under its lock at every draw
    MaterialFrame mf_;
    uint64_t mf_frame_ = ~0ull;
    const MaterialFrame& FrameMaterial() {
        if (mf_frame_ != frame_) {
            std::lock_guard lock(g_material_frame_lock);
            mf_ = g_material_frame;
            mf_frame_ = frame_;
        }
        return mf_;
    }  // a vertex shader's key: its per pixel lighting id
    bool draw_far_ = false;          // the draw is put at the far plane (jitter.w: the sky's pieces, level 1's far shells)
    bool draw_emitter_ = false;      // the draw's first texture is a sprite of the game's table (g_emitter)
    EmitterKind draw_emitter_kind_;  // and its kind
    void RecordVeil(const State& s, const Assembled& a, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages);
    void DrawVeil(Target& ds);
    // The guard band: the casters' depth again with the view widened (kGuardScale), what lies past the screen's edges
    // for the ambient occlusion and the contact shadows; R32, three quarters of the screen's size
    ComPtr<ID3D12Resource> guard_;
    D3D12_RESOURCE_STATES guard_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    uint32_t guard_w_ = 0, guard_h_ = 0, guard_dsv_ = ~0u;
    std::unordered_map<uint64_t, uint64_t> guard_vs_;
    ID3D12Resource* DrawGuard(uint32_t w, uint32_t h);
    // Water Interaction: the frame's water seen from above (its height, R32) and the casters crossing it (how far up
    // the slab about the water they reach, R16) on a square about the eye, the wave field stepped from them
    // (kWaveHlsl, RGBA16F twice), the water's draws reading its slope (wave constants, t8)
    std::vector<Caster> water_tops_;
    uint64_t water_gen_ = 0;
    std::unordered_map<uint64_t, uint64_t> top_vs_;
    uint64_t top_water_ps_ = 0, top_cover_ps_ = 0;
    ComPtr<ID3D12Resource> wave_state_[2], wave_water_, wave_cover_;
    D3D12_RESOURCE_STATES wave_state_st_[2] = {}, wave_water_st_ = {}, wave_cover_st_ = {};
    int wave_cur_ = 0;
    bool wave_ok_ = false, wave_placed_ = false;
    float wave_x0_ = 0, wave_z0_ = 0, wave_acc_ = 0, wave_age_ = 0;  // wave_age_: seconds since the field was placed anew
    uint64_t wave_inv_frame_ = ~0ull;
    float wave_inv_[16] = {};
    uint64_t TopVertexShader(const State& s, const Assembled& a, const DrawShaders& sh);
    void RecordWaterTop(const State& s, const Assembled& a, const DrawShaders& sh, const PsoKey& k);
    void WaterInteraction();
    // Snow Tracks: the trail (kTrailHlsl, R16G16F twice) about the eye, the steps pressed in, the snow reading it
    ComPtr<ID3D12Resource> trail_[2];
    D3D12_RESOURCE_STATES trail_st_[2] = {};
    int trail_cur_ = 0;
    bool trail_ok_ = false, trail_bound_ = false;
    float trail_x0_ = 0, trail_z0_ = 0;
    void SnowTracks();
    // tests (g_map_frames): the floors of the solid world drawn, a 3 m cell's heights (CollectMap)
    std::unordered_map<int64_t, std::vector<float>> map_cells_;
    bool map_on_ = false;
    void CollectMap(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk, const PsoKey& k);
    void MapFrame();
    // the hits' places this frame (CollectEmitter: EmitterKind::impact), the ones seen lately (a hit's sprite lasts),
    // this frame's new ones sent to the field (drops: texel x, z, height, strength)
    struct Impact {
        float pos[3];
        float strength, age;
    };
    std::vector<Impact> impacts_frame_, impacts_seen_;
    float drops_[8][4] = {};
    // the splashes to make, read back kFrames later (kWaveHlsl waveevents): per frame slot, its fence, the field's
    // corner and drops then
    ComPtr<ID3D12Resource> wave_events_, wave_events_rb_[kFrames];
    D3D12_RESOURCE_STATES wave_events_st_ = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT wave_events_fp_{};
    struct EventSlot {
        uint64_t fence = 0;
        float x0 = 0, z0 = 0;
        float drops[8][4] = {};
        float world[8][3] = {};
    } wave_slots_[kFrames];
    // the splashes: drops and mist, moved here, drawn over the scene (DrawSplashes)
    struct Splash {
        float p[3], v[3];
        float size, age, life, seed, water_y;
        int kind;  // 0 a drop, 1 mist
    };
    std::vector<Splash> splashes_;
    std::vector<std::array<float, 4>> splash_recent_;  // x, y, z, age: splashes made lately (none again there)
    void SpawnSplash(const float pos[3], float strength);
    void DrawSplashes(Target& rt);
    float wave_world_[8][3] = {};  // this frame's drops in the world
    float wave_water_y_ = -1e9f;   // a water height the field read back lately (tests)
    D3D12_CPU_DESCRIPTOR_HANDLE WaveRtv(uint32_t i) { return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kPostRtvs + kSceneRtvs + 6 + i, rtv_inc_); }
    // Motion (MotionPass): the frame's opaque draws again with their constants of the frame before, matched by what
    // they draw and their order; the screen's velocity, the per object motion blur, the temporal anti-aliasing
    struct MotionDraw {
        ID3D12PipelineState* pso;
        D3D12_VERTEX_BUFFER_VIEW vb[16];
        uint32_t vb_count;
        D3D12_INDEX_BUFFER_VIEW ib;
        bool indexed;
        uint32_t count;
        int32_t base;
        D3D_PRIMITIVE_TOPOLOGY topo;
        D3D12_GPU_VIRTUAL_ADDRESS vc, dc, pvc, pdc;
        bool weapon;  // the weapon in hand (after the world's depth was cleared): tested against the depth now
    };
    struct MotionSeen {
        D3D12_GPU_VIRTUAL_ADDRESS vc, dc;
        uint64_t gen;
    };
    std::vector<MotionDraw> motion_;
    uint64_t motion_gen_ = 0;
    StampTable<MotionSeen> motion_cur_, motion_prev_;
    StampTable<uint32_t> motion_order_, motion_counts_prev_;
    uint64_t motion_prev_gen_ = 0;  // ring_gen_ when motion_prev_ was this frame's: its entries of that generation count
    std::unordered_map<uint64_t, uint64_t> motion_vs_;
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> motion_psos_;
    ComPtr<ID3D12RootSignature> motion_root_;
    uint64_t motion_ps_ = 0, motion_ps_over_ = 0;
    bool motion_ok_ = false;  // this frame's velocity drawn
    float prev_view_[16] = {}, prev_proj_[16] = {};
    bool prev_cam_ok_ = false;
    float taa_jit_[2] = {};    // this frame's jitter, clip space
    uint32_t taa_index_ = 0;
    bool taa_hist_ok_ = false;
    int taa_level_ = -9;
    void RecordMotion(const State& s, const Assembled& a, const DrawShaders& sh, const PsoKey& k);
    void MotionPass(Target& rt);
    void MotionBlurPass(Target& rt);
    void TaaPass(Target& rt);
    void MotionParams(float out[28]);
    uint64_t ring_gen_ = 0;     // the frame's uploads so far went (Submit(true), a wrapped descriptor ring)
    uint64_t casters_gen_ = 0;  // ring_gen_ the casters were recorded in
    // the sun's reach on the screen for the materials' highlights: the last shadow mask (scene_[kSceneShadow], its
    // visibility in g), at t7 (root table 4) for the whole frame
    bool sun_vis_ok_ = false;     // a mask (or the white of no shadows) was made at the scene targets' size
    bool sun_vis_bound_ = false;  // and is at t7 for this list
    bool wave_bound_ = false;     // the wave field at t8 (Water Interaction)
    D3D12_GPU_DESCRIPTOR_HANDLE sun_table_{};
    void BindSunVisibility();
    // a caster pipeline: the variant vertex shader, the depth only or alpha test pixel shader, the input layout
    struct ShadowKey {
        uint64_t vs, ps;
        uint32_t regs, topo;
        uint32_t elements[16];
    };
    std::unordered_map<uint64_t, uint64_t> shadow_vs_;  // a vertex shader's key to its variant's hash, 0 none
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> shadow_psos_;
    std::unordered_set<uint64_t> known_shadow_;
    ID3D12PipelineState* EnsureShadowPipeline(const ShadowKey& k);
    bool ShadowDesc(const ShadowKey& k, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, D3D12_INPUT_ELEMENT_DESC el[16]) const;
    void RecordCaster(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk, const PsoKey& k,
                      const DrawTextures& stages);
    void GatherLights(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
    void ResolveLight();
    bool DrawShadowMap();
    ID3D12Resource* ShadowMask(Target& rt, Target& ds);
    bool CharacterMask(Target& rt, Target& ds);
    ComPtr<ID3D12Resource> shadow_atlas_;
    D3D12_RESOURCE_STATES shadow_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    uint32_t shadow_tile_ = 0;
    uint32_t shadow_dsv_ = ~0u;
    shadow::Cascades cascades_;
    shadow::Quality shadow_q_{};
    shadow::Light light_;
    bool light_resolved_ = false;
    bool shadows_drawn_ = false;  // this frame's atlas is ready
    // what the atlas was last drawn for (DrawShadowMap draws it again only when needed)
    bool atlas_valid_ = false;
    double atlas_time_ = 0;
    float atlas_eye_[3] = {}, atlas_ahead_[3] = {}, atlas_light_[3] = {};
    int atlas_level_ = -2;
    uint32_t atlas_quality_ = ~0u;
    // the camera the frame's 3D was drawn with (the fixed function transforms, which the game's own matrices
    // are made from)
    bool cam_ok_ = false;
    float cam_view_[16] = {}, cam_proj_[16] = {};
    float cam_min_z_ = 0, cam_max_z_ = 1;
    float inv_vp_[16] = {};
    float inv_vp_of_[32] = {};  // the view and projection inv_vp_ is of
    bool inv_vp_ok_ = false;
    std::unordered_map<uint64_t, ProgramLightInfo> prog_light_;  // by vertex shader key
    bool EnsureInvVp();  // inv_vp_ of the frame's camera
    ID3DBlob* weapon_gs_ = nullptr;
    ID3DBlob* point_gs_ = nullptr;  // PointSpriteGsHlsl, for the pipelines of kPointSpriteKey
    ID3DBlob *tess_hs_ = nullptr, *tess_ds_ = nullptr;  // TessHlsl, for kTessKey
    std::unordered_map<uint64_t, TessInfo> tess_rows_;  // by vertex shader key
    bool tess_draw_ = false;                            // this draw is tessellated: patches of three
    std::vector<uint32_t> tess_list_;                   // a strip's triangles as a list  // WeaponCullGsHlsl, for the pipelines of kWeaponCullKey
    // the lights of glowing sprites (g_emitter): the frame's, then the ones lighting, each fading once its sprite is
    // gone; the world's depth kept from before the weapon in hand clears it, to light the world after it is drawn
    struct DynLight {
        float pos[3];  // world
        float radius;
        float color[3];
        float strength;  // the sprite's at its last sighting
        float age;       // seconds since
        float hold;
        bool flicker;
        float heat, flash;
        bool shadows;
    };
    std::vector<DynLight> dyn_frame_, dyn_lights_;
    // the frame's effects for the composite: heat sources (screen xy, radius, strength; view depth), the flash
    struct SceneFx {
        float heat[8][4], heatz[8][4];
        uint32_t heats = 0;
        float flash = 0;
    };
    // the game's fog as the world is drawn with it (the frame's last world draw with fog): colour, start, end
    float fog_[5] = {0, 0, 0, 0, 0};
    bool fog_seen_ = false;
    float mask_params_[9 * 4 + 4 * 4 + 4 * 16] = {};  // the shadow mask's parameters, for the haze
    bool mask_params_ok_ = false;
    bool fog_done_ = false;
    float fog_tint_[3] = {};
    bool gi_ready_ = false;
    // the frame's dynamic lights for the smoke's sprites (Lit Smoke), view space
    uint64_t smoke_lights_frame_ = ~0ull;
    float barrel_heat_ = 0, muzzle_[2] = {0.6f, 0.75f};
    float sun_screen_[3] = {};  // the sun on the screen and its strength there (0: not in view), for the lens flare
    struct Shock {
        float x, y, size, t;  // on the screen, its full radius there, seconds since
        float power = 1;      // by its distance
    };
    std::vector<Shock> shocks_;  // Shockwaves
    float dirt_ = 0;             // dirt on the view from a close blast, 1 fading to 0
    float dirt_seed_ = 0;        // its pattern  // Barrel Heat: the player's gun's, its muzzle on the screen
    float smoke_lights_pr_[8 * 4] = {}, smoke_lights_cl_[8 * 4] = {};
    uint32_t smoke_lights_n_ = 0;  // this frame's bounce (kSceneGi) for the occlusion composite
    // the volumetric haze (kSceneFogHlsl) at half size, into scene_[kSceneRays]'s twin
    void FogPass(Target& rt, Target& ds);
    SceneFx fx_;
    float dof_ = 0;                // aiming's depth of field, 0 to 1
    void SceneFinal(Target& rt, const float p[16], bool bloom, bool rays, bool dof);
    // the sky's constants (kSceneCommon's sky[6]), for every scene pass of the frame
    float sky_cb_[24] = {};
    // the eye's exposure (Exposure): the scene's brightness measured into one pixel, read back a few frames on
    ComPtr<ID3D12Resource> meter_;
    D3D12_RESOURCE_STATES meter_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ComPtr<ID3D12Resource> meter_rb_[kFrames];
    uint64_t meter_fence_[kFrames] = {};
    float exposure_ev_ = 0;
    float grade_cb_[16] = {};
    void Exposure(Target& rt);
    void Grade();
    bool sky_done_ = false;  // the frame's sky drawn (before the weapon in hand)
    // Level 1's sky (the Volga: a dome and a shell of smoke before it, drawn first, the depth test off): its pieces
    // put at the far plane, each a layer of its own a few steps nearer (kSkyLayerStep), so the sky passes take them
    // all as sky and soften where one ends over another (kSceneSkyHlsl's SkyFeather)
    bool sky_phase_ = true;
    uint32_t sky_layer_ = 0;
    bool SkyConstants();
    void SkyPass(Target& rt, const float p[16]);
    std::unordered_map<uint64_t, std::shared_ptr<const CpuProgram>> cpu_prog_;  // by vertex shader key
    ComPtr<ID3D12Resource> world_depth_;
    D3D12_RESOURCE_STATES world_depth_state_ = D3D12_RESOURCE_STATE_COMMON;
    bool world_depth_saved_ = false;
    void CollectEmitter(const State& s, const Assembled& a, const DrawShaders& sh, const EmitterKind& kind);
    bool WeaponPieces(const State& s, const Assembled& a, const DrawShaders& sh);
    // the weapon in hand's triangles kept (WeaponPieces, in the draw's triangle order): DrawGeometry draws them alone
    std::vector<uint8_t> weapon_keep_;
    bool weapon_filter_ = false;
    void SaveWorldDepth(Target& ds);
    bool CopyDepth(Target& ds);
    bool soft_draw_ = false;   // this draw is a soft particle (world_depth_ at t6)
    float tess_now_[5] = {};   // the tessellated draw's position rows, its offset constant
    // the reflection buffer: what the materials drawn to the scene write beside their colour (kGbufKey), the screen's
    // size; cleared at the frame's first, read by the reflections' pass
    ComPtr<ID3D12Resource> gbuf_;
    D3D12_RESOURCE_STATES gbuf_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    uint32_t gbuf_w_ = 0, gbuf_h_ = 0;
    bool gbuf_used_ = false;   // written this frame
    bool gbuf_bound_ = false;  // bound beside bound_rt_
    D3D12_CPU_DESCRIPTOR_HANDLE GbufRtv() { return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kPostRtvs + kSceneRtvs, rtv_inc_); }
    D3D12_CPU_DESCRIPTOR_HANDLE MsaaRtv() { return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kPostRtvs + kSceneRtvs + 2, rtv_inc_); }
    D3D12_CPU_DESCRIPTOR_HANDLE MsaaGbufRtv() { return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kPostRtvs + kSceneRtvs + 3, rtv_inc_); }
    // 4x MSAA of the scene (Anti-Aliasing): the world drawn into these from the frame's first full clear of the scene's
    // targets until anything reads them (MsaaResolve), then resolved into them and the frame goes on single sampled
    ComPtr<ID3D12Resource> msaa_rt_, msaa_ds_, msaa_gbuf_;
    D3D12_RESOURCE_STATES msaa_rt_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET, msaa_ds_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE,
                          msaa_gbuf_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    uint32_t msaa_w_ = 0, msaa_h_ = 0, msaa_dsv_ = ~0u;
    DXGI_FORMAT msaa_fmt_ = DXGI_FORMAT_UNKNOWN;
    bool msaa_active_ = false, msaa_done_ = false, msaa_bound_ = false, msaa_gbuf_used_ = false;
    Target* msaa_for_rt_ = nullptr;
    Target* msaa_for_ds_ = nullptr;
    uint32_t msaa_samples_ = 0;
    bool EnsureMsaa(const Target& rt, uint32_t samples);
    void MsaaResolve();
    D3D12_CPU_DESCRIPTOR_HANDLE SkyTilesRtv(uint32_t i) { return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kPostRtvs + kSceneRtvs + 4 + i, rtv_inc_); }
    // level 1's sky tiles (kSkyTilesHlsl): found, then spread
    ComPtr<ID3D12Resource> sky_tiles_[2];
    D3D12_RESOURCE_STATES sky_tiles_state_[2] = {};
    uint32_t sky_tiles_w_ = 0, sky_tiles_h_ = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE MeterRtv() { return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kPostRtvs + kSceneRtvs + 1, rtv_inc_); }
    bool BindGbuf(bool on);
    void ReflectionPass(Target& rt, const float p[16]);
    bool soft_saved_ = false;  // world_depth_ holds the frame's depth for its soft particles
    void UpdateDynLights();
    uint32_t DynLightConstants(float* pos_radius, float* color, uint32_t max);
    // the last draw's uploads and views
    D3D12_VERTEX_BUFFER_VIEW last_vbv_[16] = {};
    D3D12_INDEX_BUFFER_VIEW last_ibv_{};
    D3D12_GPU_VIRTUAL_ADDRESS last_vc_ = 0, last_dc_ = 0;
    uint8_t* last_tail_ = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE last_srv_{}, last_smp_{};
    // the shadows' GPU time (XBE_SHADOW_STATS): timestamps around the atlas and the mask, per frame slot
    ComPtr<ID3D12QueryHeap> shadow_queries_;
    ComPtr<ID3D12Resource> shadow_readback_;
    bool shadow_timed_[kFrames] = {};
    double shadow_ms_[2] = {}, shadow_ms_n_ = 0;
    uint64_t shadow_casters_n_ = 0;
    void ShadowTimestamp(uint32_t i);
    void ReadShadowTimes();
    float Daylight() const;
    // tests (gpu::g_frame_stats recording): the GPU's time between named points of the frame (GpuMark: the time since
    // the mark before is the named step's), averaged in the frame times' report
    static constexpr uint32_t kMarks = 64;
    ComPtr<ID3D12QueryHeap> marks_;
    ComPtr<ID3D12Resource> marks_rb_;
    const char* mark_names_[kFrames][kMarks] = {};
    uint32_t mark_n_[kFrames] = {};
    bool marks_resolved_[kFrames] = {};
    void GpuMark(const char* name);
    void ResolveGpuMarks();
    void ReadGpuMarks();
    // the television around the 4:3 screens (g_tv_frame)
    ComPtr<ID3D12Resource> tv_frame_;
    DXGI_FORMAT tv_fmt_ = DXGI_FORMAT_UNKNOWN;
    uint32_t tv_mips_ = 1;
    float tv_screen_[4] = {};  // the screen's rectangle in the frame, 0..1 (ui/tv_frame.txt)
    bool tv_tried_ = false;
    bool TvFrame(Target& t, ID3D12Resource* src, DXGI_FORMAT fmt);
    // the overlay's rows in the picture (g_tv_ui), drawn into their layer before the television shows it
    void TvUi();
    ComPtr<ID3D12Resource> tv_ui_;
    D3D12_RESOURCE_STATES tv_ui_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_CPU_DESCRIPTOR_HANDLE TvUiRtv() { return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + kPostRtvs + kSceneRtvs + 13, rtv_inc_); }

  public:
    bool tv_on_ = false;
    float tv_rect_[4] = {};

  private:
    uint64_t draw_tex0_ = 0;       // the draw's stage 0 texture's content id
    uint64_t game_moon_frame_ = ~0ull;  // the last frame the game drew its own moon sprite (SkyConstants)
    int game_moon_level_ = -1000;        // the level that has shown its own: none of ours there, wherever one looks
    bool OwnMoonLevel() const { return game_moon_level_ == g_level.load() || g_level.load() == 2; }
    bool recolor_ = false;         // g_recolor_2d asked for the draw: its colour element's new RGB
    uint32_t recolor_argb_ = 0;
    VertexElement recolor_elem_{};
    uint32_t brightness_ = 0;      // the brightness the gamma ramp was made with
    uint64_t cpu_frame_ = ~0ull >> 1;
    bool scene_drawn_ = false;  // the frame being presented has the 3D world
    bool tv_was_on_ = false;  // the last present whose frame the CPU wrote whole (a movie)
    ComPtr<ID3D12Resource> details_;  // the pack's detail normal maps, one slice each
    DXGI_FORMAT details_fmt_ = DXGI_FORMAT_UNKNOWN;
    uint32_t details_mips_ = 0, details_count_ = 0;
    bool details_tried_ = false;
    // a screen space draw to a widened screen placed by g_layout_2d: c.layout
    void Layout2D(const State& s, const Assembled& a, bool prog, GuestExtent size, DrawConstants& c);
    // video recording (control "record <file.mp4>" / "record stop", XBE_RECORD): each shown frame, at most
    // XBE_RECORD_FPS (60) a second, copied into one of two readback buffers; a buffer is read once its copy is done
    // (a frame later, nobody waiting) and its pixels go to ffmpeg (XBE_FFMPEG, else on the PATH) on a writer thread,
    // encoded with NVENC (HEVC, high quality; a fragmented MP4: a killed run still plays). The video keeps the
    // clock's time: a frame is written as many times as the rate's frames it stands for (a slow frame, a stall), so
    // the game's sound, written beside it (<file>.wav, the mix as it goes out), stays in step.
    struct RecSlot {
        ComPtr<ID3D12Resource> rb;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        uint64_t fence = 0;
        bool pending = false;
        int64_t at = 0;  // the rate's frame it was taken at, from the start
    };
    RecSlot rec_slots_[2];
    int rec_next_ = 0;
    FILE* rec_pipe_ = nullptr;
    uint32_t rec_w_ = 0, rec_h_ = 0;
    std::wstring rec_path_;
    std::chrono::steady_clock::time_point rec_last_{};
    std::thread rec_thread_;
    std::mutex rec_mu_;
    std::condition_variable rec_cv_;
    std::deque<std::pair<std::vector<uint8_t>, int64_t>> rec_queue_;  // a frame, how many times it is written
    bool rec_stop_ = false;
    std::chrono::steady_clock::time_point rec_start_{};
    int64_t rec_due_ = 0;     // the next of the rate's frames to take
    int64_t rec_frames_ = 0;  // the frames given to the writer
    FILE* rec_wav_ = nullptr;
    uint64_t rec_wav_bytes_ = 0;
    std::mutex rec_wav_mu_;
    bool rec_copied_ = false;
    void RecordFrame(uint32_t bi);   // before Submit: the copy of a due frame
    void RecordAfterSubmit();         // after Submit: the slot's fence
    void RecordFlush(RecSlot& s);     // its pixels to the writer
    void RecordStop();
    // control channel screenshots (core/control.h): the shown back buffer into shot_rb_, then to a BMP
    bool CopyBackBuffer(uint32_t bi);
    void SaveBackBufferCopy(const std::wstring& path);
    ComPtr<ID3D12Resource> shot_rb_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT shot_fp_ = {};
    void RenderUi();
    void ReleaseOldTextures();
    void ResetFrame();
    void CountFps();
    void ApplySettings();
    ComPtr<ID3D12Resource> Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt, uint32_t w,
                                  uint32_t h, const ShrinkOptions& o = {});
    bool ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void CapStep(const Surface& rt);
    // a resource CapSave shrinks to w x h and writes, in state *state, colour times mul
    struct CapImage {
        ID3D12Resource* res;
        D3D12_RESOURCE_STATES* state;
        DXGI_FORMAT fmt;
        uint32_t w;
        uint32_t h;
        float mul = 1;
    };
    void CapSave(const CapImage& img, const char* name);
    void CapSave(Target& t, const char* name);
    // a surface's place in targets_: its key, console size and entry, end when it has none
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;  // the entry is there at the surface's size
    };
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    // EnsureTarget's steps, in call order
    TargetSlot FindTargetSlot(const Surface& s, bool depth, uint64_t alt);
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    bool TargetStale(uint32_t data) const;
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool CreateTargetResource(Target& t, const Surface& s);
    bool CreateTargetView(Target& t);
    // UploadTarget, then its step
    void UploadTarget(Target& t, const Surface& s);
    ComPtr<ID3D12Resource> CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    // Clear's arguments past the state
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs);
    // Copy's guest lists: count src_rects, dst_points 0 or one per rect
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
    };
    void CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                    const CopyList& l);
    void CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                      const CopyList& l);
    // the target as a guest sized BGRA copy source, and the shrunk copy that holds it when it is not one already
    struct CopySource {
        ID3D12Resource* res;
        ComPtr<ID3D12Resource> down;
    };
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    // RecordLate's steps, under late_lock_
    struct Late;
    void ResetLate(Late& l, const Target& t);
    ID3D12Resource* PrepareLateSource(Target& t, Late& l);
    void RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src);

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_;
    ComPtr<ID3D12Resource> bb_[kFrames];
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t fence_value_ = 0;
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    uint64_t colour_frame_ = ~0ull;  // the frame whose scene colour was cleared (Clear)
    ComPtr<ID3D12GraphicsCommandList> cl_;
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    ComPtr<ID3D12DescriptorHeap> smp_heap_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12DescriptorHeap> ui_heap_;
    static constexpr uint32_t kUiDescriptors = 128;  // ImGui's font first, then the overlay's pictures
    struct UiTex {
        ComPtr<ID3D12Resource> res;
        uint64_t id = 0;
    };
    std::unordered_map<std::string, UiTex> ui_tex_;
    uint32_t ui_used_ = 1;

  public:
    uint64_t UiImage(const std::string& key, const remaster::Image* im);

  private:
    uint32_t srv_inc_ = 0;
    uint32_t smp_inc_ = 0;
    uint32_t rtv_inc_ = 0;
    uint32_t dsv_inc_ = 0;
    std::vector<uint32_t> free_rtv_;
    std::vector<uint32_t> free_dsv_;
    ComPtr<ID3D12RootSignature> root_;
    std::unordered_map<uint64_t, ComPtr<ID3DBlob>> blobs_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel program keys to source hash
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> psos_;
    // the shader cache (LoadShaderCache)
    ComPtr<ID3D12PipelineLibrary> library_;
    std::vector<uint8_t> library_data_;  // kept alive: the library reads from it
    bool library_dirty_ = false;
    std::unordered_set<uint64_t> known_psos_;
    std::chrono::steady_clock::time_point library_saved_ = std::chrono::steady_clock::now();
    void LoadShaderCache();
    void SaveShaderCache();
    void RememberBlob(uint64_t h, ID3DBlob* b);
    void RememberPipeline(const PsoKey& k, uint64_t h, ID3D12PipelineState* pso);
    // recipes (recipes.bin): what each shader the draws used is made from, its vertex program or fixed function key
    // and its pixel key, so that a new build makes them all again at start; the pipelines name their shaders by the
    // recipe's id (stable across builds), not by the bytecode's hash
    std::unordered_set<uint64_t> recipes_known_;
    std::unordered_map<uint64_t, uint64_t> stable_of_;  // a blob's hash: the recipe's id it was made from
    std::unordered_map<uint64_t, uint64_t> blob_of_;    // a recipe's id: its blob in this build
    void RememberVertexRecipe(const State& s, const Assembled& a, const DrawShaders& sh, uint8_t variant, uint64_t h);
    void RememberPixelRecipe(const PixelKey& pk, uint64_t pkey, uint64_t h);
    uint64_t StableOf(uint64_t h) const {
        auto it = stable_of_.find(h);
        return it == stable_of_.end() ? h : it->second;
    }
    uint64_t FixedShader(uint64_t id, const std::string& src, const char* entry, const char* profile);
    uint64_t shadow_alpha_ps_[2] = {}, mask_ps_ = 0;
    std::unordered_set<uint64_t> passes_known_;  // passes.bin's
    uint64_t root_hash_ = 0;  // the root signature's, serialized
    void PassDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv, float blend, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const;
    std::unordered_map<uint64_t, uint32_t> samplers_;
    uint32_t smp_used_ = 0;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;  // target address to the frame it was last used
    std::unordered_map<uint64_t, Tex> textures_;  // by data address, a P8 texture's with its palette's above
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> blit_psos_;
    Extension* ext_ = nullptr;
    // one frame capture: every draw in a text file, each target as the frame leaves it, the textures it read
    FILE* cap_ = nullptr;
    std::filesystem::path cap_dir_;
    uint32_t cap_i_ = 0;
    uint32_t cap_rt_ = 0;
    std::unordered_set<uint32_t> cap_seen_;
    std::unordered_set<uint64_t> cap_sh_;
    struct CapTex {
        ComPtr<ID3D12Resource> res;
        DXGI_FORMAT fmt;
        uint32_t data;
        uint32_t w;
        uint32_t h;
    };
    std::vector<CapTex> cap_tex_;
    ComPtr<ID3D12Resource> lut_;
    D3D12_RESOURCE_STATES lut_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    float scale_ = 1.0f;
    float widen_ = 1.0f;  // g_screen_widen when the targets were made
    float layout_[2] = {1, 0};  // the draw's 2D placement (Layout2D), its scissor goes with it
    // surfaces locked without a wait: copies in flight, the newest finished one goes to guest memory
    struct Late {
        ComPtr<ID3D12Resource> rb[kFrames];
        ComPtr<ID3D12Resource> down;
        D3D12_RESOURCE_STATES down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t row_pitch = 0;
        uint32_t next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                      // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};    // presents_ for DeliverLate
    Assembled asm_;  // kept between draws for its buffers
    bool tearing_ = false;
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

bool D3D12Backend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!CreateDevice()) return false;
    if (!CreateSwapChain(hwnd)) return false;
    CreateHeaps();
    CreateBackBufferViews();
    if (!CreateRootSignature()) return false;
    CreateFrames();
    CreateGammaLut();
    // (twice the frames: the overlay draws twice a frame, the television's layer and the window's)
    ImGui_ImplDX12_Init(dev_.Get(), kFrames * 2, DXGI_FORMAT_B8G8R8A8_UNORM, ui_heap_.Get(),
                        ui_heap_->GetCPUDescriptorHandleForHeapStart(), ui_heap_->GetGPUDescriptorHandleForHeapStart());
    LoadSettings();
    LoadShaderCache();
    Begin();
    LOGI("d3d12: %ux%u guest, %ux%u window, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    return true;
}

bool D3D12Backend::CreateDevice() {
    if (GetEnvironmentVariableW(L"XBE_D3D12_DEBUG", nullptr, 0)) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) return false;
    // dedicated GPU first on hybrid laptops
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(factory_.As(&f6))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_))) &&
        FAILED(D3D12CreateDevice((adapter = nullptr).Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_)))) {
        LOGE("d3d12: no device");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGR("D3D12 adapter: %ls", adapter ? ad.Description : L"default");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    ComPtr<IDXGIFactory5> f5;
    BOOL tear = FALSE;
    if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
    tearing_ = tear;
    return true;
}

bool D3D12Backend::CreateSwapChain(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    bb_w_ = std::max<LONG>(rc.right - rc.left, 1);
    bb_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bb_w_;
    sd.Height = bb_h_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
        LOGE("d3d12: no swap chain");
        return false;
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&swap_);
    return true;
}

uint32_t D3D12Backend::CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible,
                                  ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = t;
    hd.NumDescriptors = n;
    hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out));
    return dev_->GetDescriptorHandleIncrementSize(t);
}

void D3D12Backend::CreateBackBufferViews() {
    for (int i = 0; i < kFrames; i++) {
        swap_->GetBuffer(i, IID_PPV_ARGS(&bb_[i]));
        dev_->CreateRenderTargetView(bb_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kRtvCount + i, rtv_inc_));
    }
}

void D3D12Backend::CreateHeaps() {
    srv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount, true, srv_heap_);
    smp_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCount, true, smp_heap_);
    rtv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvCount + kFrames + kPostRtvs + kSceneRtvs + 14, false, rtv_heap_);
    dsv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false, dsv_heap_);
    CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kUiDescriptors, true, ui_heap_);
    for (uint32_t i = kRtvCount; i-- > 0;) free_rtv_.push_back(i);
    for (uint32_t i = kDsvCount; i-- > 0;) free_dsv_.push_back(i);
}

bool D3D12Backend::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 7;  // the stages, then a remastered material's normal, material and detail maps
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[1].NumDescriptors = 4;
    D3D12_ROOT_PARAMETER params[6] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    for (int i = 0; i < 2; i++) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // t7: the sun's reach on the screen (BindSunVisibility)
    D3D12_DESCRIPTOR_RANGE sun{};
    sun.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    sun.NumDescriptors = 3;  // and t8: the wave field (Water Interaction), t9: the snow's trail (Snow Tracks)
    sun.BaseShaderRegister = 7;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges = &sun;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // b2: the top view's constants (WaterInteraction)
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[5].Descriptor.ShaderRegister = 2;
    // s4: the wave field's, linear and clamped
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderRegister = 4;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 6;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &ss;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rs;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err))) return false;
    dev_->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&root_));
    root_hash_ = Hash(rs->GetBufferPointer(), rs->GetBufferSize());
    return true;
}

void D3D12Backend::CreateFrames() {
    for (auto& f : frames_) {
        dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd = BufferDesc(kUploadSize);
        CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&f.upload));
        f.upload->Map(0, nullptr, reinterpret_cast<void**>(&f.map));
        f.va = f.upload->GetGPUVirtualAddress();
    }
    dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&cl_));
}

// gamma ramp as a 256 x 1 lookup
void D3D12Backend::CreateGammaLut() {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1);
    CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&lut_));
}

void D3D12Backend::LoadSettings() {
    scale_ = app::RenderScale(guest_h_);
    widen_ = std::max(1.0f, g_screen_widen.load());
    ext_ = g_extension.get();
    if (ext_) {
        ext_->Settings();
        ext_->Reset(*this);
    }
}

void D3D12Backend::Begin() {
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    bound_rt_ = bound_ds_ = nullptr;
    cmd_ = CmdState{};
    BindSunVisibility();
}

// t7 for the list: the last shadow mask while it is readable at the scene targets' size, else a null view (the
// highlights then left as they are: UploadConstants asks for the mask only when bound)
void D3D12Backend::BindSunVisibility() {
    uint32_t slot = AllocSrvSlots(3);
    constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* r = sun_vis_ok_ && scene_[kSceneShadow] && scene_state_[kSceneShadow] == kRead ? scene_[kSceneShadow].Get() : nullptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(r, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    sun_table_ = Gpu(srv_heap_.Get(), slot, srv_inc_);
    sun_vis_bound_ = r != nullptr;
    // t8: the wave field as last stepped (Water Interaction)
    ID3D12Resource* wf = wave_ok_ && wave_state_[wave_cur_] && wave_state_st_[wave_cur_] == kRead ? wave_state_[wave_cur_].Get() : nullptr;
    sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dev_->CreateShaderResourceView(wf, &sv, Cpu(srv_heap_.Get(), slot + 1, srv_inc_));
    wave_bound_ = wf != nullptr;
    // t9: the snow's trail (Snow Tracks)
    ID3D12Resource* tr = trail_ok_ && trail_[trail_cur_] && trail_st_[trail_cur_] == kRead ? trail_[trail_cur_].Get() : nullptr;
    sv.Format = DXGI_FORMAT_R16G16_FLOAT;
    dev_->CreateShaderResourceView(tr, &sv, Cpu(srv_heap_.Get(), slot + 2, srv_inc_));
    trail_bound_ = tr != nullptr;
    cl_->SetGraphicsRootDescriptorTable(4, sun_table_);
}

void D3D12Backend::Submit(bool wait) {
    cl_->Close();
    ID3D12CommandList* lists[] = {cl_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), ++fence_value_);
    Frame& f = CurrentFrame();
    f.fence = fence_value_;
    if (wait) {
        ring_gen_++;
        WaitFence(fence_value_);
        f.alloc->Reset();
        f.garbage.clear();
        f.used = 0;
        f.srv = 0;
        cl_->Reset(f.alloc.Get(), nullptr);
        Begin();
    }
}

void D3D12Backend::Flush() { Submit(true); }

void D3D12Backend::WaitFence(uint64_t value) {
    fence_->SetEventOnCompletion(value, event_);
    WaitForSingleObject(event_, INFINITE);
}

void D3D12Backend::SetFullViewport(uint32_t w, uint32_t h) {
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    SetViewport(vp);
    SetScissor(scissor);
}

// one w x h colour target for a Blit, the draw's bound targets forgotten
void D3D12Backend::SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h) {
    cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

D3D12Backend::Alloc D3D12Backend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    g_perf.upload += size;
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload.Get(), o, f.map + o, f.va + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    g_perf.overflow++;
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    constexpr uint64_t kOwnAlign = 0x10000;  // a committed buffer's size granularity
    D3D12_RESOURCE_DESC bd = BufferDesc((size + kOwnAlign - 1) & ~(kOwnAlign - 1));
    CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                  IID_PPV_ARGS(&r));
    uint8_t* p = nullptr;
    r->Map(0, nullptr, reinterpret_cast<void**>(&p));
    f.garbage.push_back(r);
    return {r.Get(), 0, p, r->GetGPUVirtualAddress()};
}

uint32_t D3D12Backend::AllocSrvSlots(uint32_t n) {
    Frame& f = CurrentFrame();
    const uint32_t per = kSrvCount / kFrames;
    if (f.srv + n > per) {
        LOG_FIRST(4, "d3d12: descriptor ring wrapped within a frame");
        f.srv = 0;
        ring_gen_++;
    }
    uint32_t i = uint32_t(frame_ % kFrames) * per + f.srv;
    f.srv += n;
    return i;
}

void D3D12Backend::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) {
    if (cur == want) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter = want;
    cl_->ResourceBarrier(1, &b);
    cur = want;
}

// a resized target gives back its view and its resources
void D3D12Backend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    (it->second.depth ? free_dsv_ : free_rtv_).push_back(it->second.view);
    CurrentFrame().garbage.push_back(it->second.res);
    if (it->second.fb) CurrentFrame().garbage.push_back(it->second.fb);
    targets_.erase(it);
    targets_version_++;
}

// the host texture of a new target, at render scale or the extension's
bool D3D12Backend::CreateTargetResource(Target& t, const Surface& s) {
    float sc = scale_;
    if (ext_) ext_->MakeTarget(s, t.depth, t.gw, t.gh, sc, t.fmt);
    // the screen and its depth widened (g_screen_widen), any other target as the console has it
    float sx = sc * (t.gw == guest_w_ && t.gh == guest_h_ ? widen_ : 1.0f);
    // HDR: the screen's colour in 16 bit float, light added past white keeps its brightness until ScenePost
    if (hdr_ && !t.depth && t.gw == guest_w_ && t.gh == guest_h_ && t.fmt == kBgra) t.fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    t.w = std::max(1u, uint32_t(std::lround(t.gw * sx)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * sc)));
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_UNKNOWN, t.w, t.h);
    D3D12_CLEAR_VALUE cv{};
    if (t.depth) {
        td.Format = DXGI_FORMAT_R24G8_TYPELESS;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        cv.DepthStencil.Depth = 1;
        t.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else {
        td.Format = t.fmt;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        cv.Format = td.Format;
        t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    return SUCCEEDED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.state, &cv,
                                                   IID_PPV_ARGS(&t.res)));
}

// a view from the free pool, cleared; false when the pool is empty
bool D3D12Backend::CreateTargetView(Target& t) {
    auto& pool = t.depth ? free_dsv_ : free_rtv_;
    if (pool.empty()) return false;
    t.view = pool.back();
    pool.pop_back();
    if (t.depth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(t.res.Get(), &dv, Cpu(dsv_heap_.Get(), t.view, dsv_inc_));
        float one = 1;
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_),
                                   D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, one, 0, 0, nullptr);
    } else {
        dev_->CreateRenderTargetView(t.res.Get(), nullptr, Cpu(rtv_heap_.Get(), t.view, rtv_inc_));
        float black[4] = {0, 0, 0, 0};
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), black, 0, nullptr);
    }
    return true;
}

// targets_ key of a surface's colour or depth target
uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// a smaller depth surface's console size, above the address bits
constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

// a smaller depth surface laid on the depth buffer's memory is a target of its own
uint64_t D3D12Backend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

// alt: the extension's bits of the key
D3D12Backend::TargetSlot D3D12Backend::FindTargetSlot(const Surface& s, bool depth, uint64_t alt) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    SurfaceSize(s, slot.gw, slot.gh);
    slot.key = SizedTargetKey(slot.data, depth, slot.gw, slot.gh) | alt;
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

// the surface's target at its size, null when there is none
Target* D3D12Backend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    if (!slot.sized) return nullptr;
    target_used_[slot.data] = frame_;
    return &slot.it->second;
}

// the surface's target, made again at its size when that changed
Target* D3D12Backend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    target_used_[slot.data] = frame_;
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    Target t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t, s)) return nullptr;
    g_perf.targets++;
    // an empty pool drops the new resource with t
    if (!CreateTargetView(t)) return nullptr;
    t.data = slot.data;
    t.bytes = GuestPitch(slot.gw) * slot.gh;
    if (depth) t.bytes = slot.gw * slot.gh * 4;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    auto& ref = targets_[slot.key] = std::move(t);
    targets_version_++;
    return &ref;
}

bool D3D12Backend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    // the scene's pair while the multisampled targets stand for them; anything else uses the scene's own: resolved
    if (msaa_active_) {
        bool pair = rt == msaa_for_rt_ && (ds == msaa_for_ds_ || !ds);
        if (!pair && (rt == msaa_for_rt_ || ds == msaa_for_ds_)) MsaaResolve();
        else if (pair) {
            if (rt != bound_rt_ || ds != bound_ds_ || !msaa_bound_) {
                D3D12_CPU_DESCRIPTOR_HANDLE rh = MsaaRtv();
                D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), msaa_dsv_, dsv_inc_);
                cl_->OMSetRenderTargets(1, &rh, FALSE, ds ? &dh : nullptr);
                bound_rt_ = rt;
                bound_ds_ = ds;
                gbuf_bound_ = false;
                msaa_bound_ = true;
            }
            rt->drawn = true;
            if (ds) ds->drawn = true;
            return true;
        }
    }
    if (msaa_bound_) msaa_bound_ = false, bound_rt_ = bound_ds_ = nullptr;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rt->drawn = true;
    }
    if (ds) {
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ds->drawn = true;
    }
    if (rt != bound_rt_ || ds != bound_ds_) {
        D3D12_CPU_DESCRIPTOR_HANDLE rh = rt ? Cpu(rtv_heap_.Get(), rt->view, rtv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        D3D12_CPU_DESCRIPTOR_HANDLE dh = ds ? Cpu(dsv_heap_.Get(), ds->view, dsv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        cl_->OMSetRenderTargets(rt ? 1 : 0, rt ? &rh : nullptr, FALSE, ds ? &dh : nullptr);
        bound_rt_ = rt;
        bound_ds_ = ds;
        gbuf_bound_ = false;
    }
    return true;
}

// the reflection buffer beside the scene's target (on), or the target alone again
bool D3D12Backend::BindGbuf(bool on) {
    if (on == gbuf_bound_ || !bound_rt_) return on == gbuf_bound_;
    if (on) {
        uint32_t w = bound_rt_->w, h = bound_rt_->h;
        if (!gbuf_ || gbuf_w_ != w || gbuf_h_ != h) {
            if (gbuf_) Release(std::move(gbuf_));
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h);
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{};
            cv.Format = td.Format;
            gbuf_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, gbuf_state_, &cv, IID_PPV_ARGS(&gbuf_)))) {
                gbuf_.Reset();
                return false;
            }
            dev_->CreateRenderTargetView(gbuf_.Get(), nullptr, GbufRtv());
            gbuf_w_ = w, gbuf_h_ = h;
            gbuf_used_ = false;
        }
        Transition(gbuf_.Get(), gbuf_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (!gbuf_used_) {
            const float zero[4] = {0, 0, 0, 0};
            cl_->ClearRenderTargetView(GbufRtv(), zero, 0, nullptr);
            gbuf_used_ = true;
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rh[2] = {Cpu(rtv_heap_.Get(), bound_rt_->view, rtv_inc_), GbufRtv()};
    D3D12_CPU_DESCRIPTOR_HANDLE dh = bound_ds_ ? Cpu(dsv_heap_.Get(), bound_ds_->view, dsv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
    if (msaa_bound_) {
        rh[0] = MsaaRtv(), rh[1] = MsaaGbufRtv();
        dh = Cpu(dsv_heap_.Get(), msaa_dsv_, dsv_inc_);
        if (on && !msaa_gbuf_used_) {
            const float zero[4] = {0, 0, 0, 0};
            Transition(msaa_gbuf_.Get(), msaa_gbuf_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
            cl_->ClearRenderTargetView(MsaaGbufRtv(), zero, 0, nullptr);
            msaa_gbuf_used_ = true;
        }
    }
    cl_->OMSetRenderTargets(on ? 2 : 1, rh, FALSE, bound_ds_ ? &dh : nullptr);
    gbuf_bound_ = on;
    return true;
}

bool t_range_ok(const Target* t) { return t && t->range != 1; }
// tests: XBE_SHADOW_DEBUG, read once
float ShadowDebug() {
    static const float v = [] {
        const char* e = std::getenv("XBE_SHADOW_DEBUG");
        return e ? float(std::atoi(e)) : 0.0f;
    }();
    return v;
}
bool SmaaMode();

// the multisampled colour, depth (and reflection buffer) for a scene's target of this size and format
bool D3D12Backend::EnsureMsaa(const Target& rt, uint32_t samples) {
    // the count the card takes for every target (8 down to 4, 2)
    auto supported = [&](DXGI_FORMAT f, uint32_t n) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
        q.Format = f, q.SampleCount = n;
        return SUCCEEDED(dev_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) && q.NumQualityLevels > 0;
    };
    while (samples > 2 && !(supported(rt.fmt, samples) && supported(DXGI_FORMAT_D24_UNORM_S8_UINT, samples) &&
                            supported(DXGI_FORMAT_R16G16B16A16_FLOAT, samples)))
        samples /= 2;
    if (msaa_rt_ && msaa_w_ == rt.w && msaa_h_ == rt.h && msaa_fmt_ == rt.fmt && msaa_samples_ == samples) return true;
    if (msaa_rt_) Release(std::move(msaa_rt_));
    if (msaa_ds_) Release(std::move(msaa_ds_));
    if (msaa_gbuf_) Release(std::move(msaa_gbuf_));
    if (msaa_dsv_ == ~0u) {
        if (free_dsv_.empty()) return false;
        msaa_dsv_ = free_dsv_.back();
        free_dsv_.pop_back();
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(rt.fmt, rt.w, rt.h);
    td.SampleDesc.Count = samples;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = rt.fmt;
    msaa_rt_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, msaa_rt_state_, &cv, IID_PPV_ARGS(&msaa_rt_)))) return false;
    dev_->CreateRenderTargetView(msaa_rt_.Get(), nullptr, MsaaRtv());
    D3D12_RESOURCE_DESC dd = Texture2DDesc(DXGI_FORMAT_R24G8_TYPELESS, rt.w, rt.h);
    dd.SampleDesc.Count = samples;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dv{};
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.DepthStencil.Depth = 1;
    msaa_ds_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, msaa_ds_state_, &dv, IID_PPV_ARGS(&msaa_ds_)))) {
        msaa_rt_.Reset();
        return false;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    dev_->CreateDepthStencilView(msaa_ds_.Get(), &dsv, Cpu(dsv_heap_.Get(), msaa_dsv_, dsv_inc_));
    D3D12_RESOURCE_DESC gd = Texture2DDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, rt.w, rt.h);
    gd.SampleDesc.Count = samples;
    gd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE gv{};
    gv.Format = gd.Format;
    msaa_gbuf_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &gd, msaa_gbuf_state_, &gv, IID_PPV_ARGS(&msaa_gbuf_)))) {
        msaa_rt_.Reset(), msaa_ds_.Reset();
        return false;
    }
    dev_->CreateRenderTargetView(msaa_gbuf_.Get(), nullptr, MsaaGbufRtv());
    msaa_w_ = rt.w, msaa_h_ = rt.h, msaa_fmt_ = rt.fmt, msaa_samples_ = samples;
    LOGR("d3d12: %ux MSAA targets %ux%u", samples, rt.w, rt.h);
    return true;
}

// The multisampled scene into the scene's own targets: the colour (and reflection buffer) resolved, the depth the
// nearest of its samples (a full screen pass writing depth); then the frame goes on single sampled
void D3D12Backend::MsaaResolve() {
    if (!msaa_active_) return;
    msaa_active_ = false;
    Target* rt = msaa_for_rt_;
    Target* ds = msaa_for_ds_;
    msaa_bound_ = false;
    bound_rt_ = bound_ds_ = nullptr;
    gbuf_bound_ = false;
    if (rt) {
        Transition(msaa_rt_.Get(), msaa_rt_state_, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        cl_->ResolveSubresource(rt->res.Get(), 0, msaa_rt_.Get(), 0, rt->fmt);
        Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (msaa_gbuf_used_ && gbuf_ && gbuf_w_ == msaa_w_ && gbuf_h_ == msaa_h_) {
        Transition(msaa_gbuf_.Get(), msaa_gbuf_state_, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        Transition(gbuf_.Get(), gbuf_state_, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        cl_->ResolveSubresource(gbuf_.Get(), 0, msaa_gbuf_.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
        Transition(gbuf_.Get(), gbuf_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        gbuf_used_ = true;
    }
    if (ds) {
        auto& pso = blit_psos_[~0ull - 1];  // (beside DepthCopy's, kDepthCopyKey)
        if (!pso) {
            static const char* kHlsl = R"(
Texture2DMS<float> msd : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps(float4 pos : SV_Position) : SV_Depth {
    int2 p = int2(pos.xy);
    uint w, h, n;
    msd.GetDimensions(w, h, n);
    float z = msd.Load(p, 0);
    [loop] for (uint s = 1; s < n; s++) z = min(z, msd.Load(p, s));
    return z;
}
)";
            uint64_t h;
            ID3DBlob* vs = Compile(kHlsl, "vs", "vs_5_0", h);
            ID3DBlob* ps = Compile(kHlsl, "ps", "ps_5_0", h);
            if (vs && ps) {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
                p.pRootSignature = root_.Get();
                p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
                p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
                p.SampleMask = UINT_MAX;
                p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                p.RasterizerState.DepthClipEnable = FALSE;
                p.DepthStencilState.DepthEnable = TRUE;
                p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                p.NumRenderTargets = 0;
                p.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
                p.SampleDesc.Count = 1;
                if (FAILED(dev_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pso)))) LOGE("d3d12: MSAA depth resolve pipeline failed");
            }
        }
        if (pso) {
            Transition(msaa_ds_.Get(), msaa_ds_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            uint32_t slot = AllocSrvSlots(7);
            for (uint32_t i = 0; i < 7; i++) {
                D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if (i == 0) {
                    sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                } else {
                    sv.Format = kBgra;
                    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    sv.Texture2D.MipLevels = 1;
                }
                dev_->CreateShaderResourceView(i == 0 ? msaa_ds_.Get() : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
            }
            D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), ds->view, dsv_inc_);
            cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
            SetFullViewport(ds->w, ds->h);
            Alloc cb = Upload(256, 256);
            DrawFullScreen(pso.Get(), cb.gpu, slot);
            Transition(msaa_ds_.Get(), msaa_ds_state_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }
    msaa_for_rt_ = msaa_for_ds_ = nullptr;
}

constexpr uint64_t kDepthCopyKey = ~0ull;  // DepthCopy's pipeline in blit_psos_

// DepthCopy's pipeline, kept in blit_psos_: writes depth only, always passing; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsureDepthCopyPipeline() {
    auto& pso = blit_psos_[kDepthCopyKey];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kDepthHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kDepthHlsl, "psmain", "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root_.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = TRUE;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    p.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

// src's depth drawn into dst's depth buffer
void D3D12Backend::DrawDepthCopy(Target& src, Target& dst, ID3D12PipelineState* pso) {
    if (cap_) fprintf(cap_, "%04u depth copy %08X to %08X\n", cap_i_++, src.data, dst.data);
    Transition(src.res.Get(), src.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst.res.Get(), dst.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), dst.view, dsv_inc_);
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
    SetFullViewport(dst.w, dst.h);
    uint32_t slot = AllocSrvSlots(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src.res.Get(), &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    Alloc cb = Upload(256, 256);
    DrawFullScreen(pso, cb.gpu, slot);
    dst.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool D3D12Backend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1) {
        return false;
    }
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(s.rt, gw, gh);
    auto di = targets_.find(SizedTargetKey(mem::VirtualFromPhysical(s.rt.data), true, gw, gh));
    auto si = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    MsaaResolve();
    Target& dst = di->second;
    Target& src = si->second;
    ID3D12PipelineState* pso = EnsureDepthCopyPipeline();
    if (!pso) return false;
    DrawDepthCopy(src, dst, pso);
    return true;
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// BGRA pixels into a new w x h texture, left as a shader resource
ComPtr<ID3D12Resource> D3D12Backend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w,
                                                        uint32_t h) {
    ComPtr<ID3D12Resource> tex;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&tex));
    uint32_t row = UploadPitch(w * 4);
    Alloc a = Upload(uint64_t(row) * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * row, &px[size_t(y) * pitch], w * 4);
    D3D12_TEXTURE_COPY_LOCATION dst{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint.Offset = a.off;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, row};
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tex.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return tex;
}

// CPU pixels of a color surface into its host target, scaled
void D3D12Backend::UploadTarget(Target& t, const Surface& s) {
    g_perf.rtups++;
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "d3d12: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ComPtr<ID3D12Resource> tex = CreatePixelTexture(px, pitch, d.width, d.height);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    // a whole frame the CPU made (a movie) on a widened screen keeps its 4:3 in the middle, black at the sides
    if (widen_ > 1.0f && t.gw == guest_w_ && t.gh == guest_h_) {
        cpu_frame_ = presents_;  // a movie: the television shows it too
        float black[4] = {0, 0, 0, 1};
        if (!over) cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), black, 0, nullptr);
        float vw = float(t.w) / widen_;
        D3D12_VIEWPORT vp{(float(t.w) - vw) / 2, 0, vw, float(t.h), 0, 1};
        SetViewport(vp);
    }
    float uv[4] = {0, 0, 1, 1};
    float mul = 1 / t.range;
    Blit(tex.Get(), kBgra, uv, {.alpha = true, .mul = mul, .rtv = t.fmt, .over = over});
    CurrentFrame().garbage.push_back(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// samplers_ key of the full screen passes' samplers, apart from the stage samplers' hashed keys
constexpr uint64_t kBlitSamplerKey = 0xB117;

// four clamp samplers: linear at slot 0, point at slots 1 to 3
uint32_t D3D12Backend::EnsureBlitSamplers() {
    uint64_t key = kBlitSamplerKey;
    auto it = samplers_.find(key);
    uint32_t si;
    if (it == samplers_.end()) {
        if (smp_used_ + 4 > kSamplerCount) {
            samplers_.clear();
            last_smp_ok_ = false;
            smp_used_ = 0;
        }
        si = smp_used_;
        smp_used_ += 4;
        for (int i = 0; i < 4; i++) {
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            dev_->CreateSampler(&sd, Cpu(smp_heap_.Get(), si + i, smp_inc_));
        }
        samplers_[key] = si;
    } else {
        si = it->second;
    }
    return si;
}

// the full screen triangle: cb at b0 and b1, views at srv_slot; without a sampler slot EnsureBlitSamplers gives it
void D3D12Backend::DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                                  std::optional<uint32_t> sampler_slot) {
    SetPso(pso);
    cl_->SetGraphicsRootConstantBufferView(0, cb);
    cl_->SetGraphicsRootConstantBufferView(1, cb);
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), srv_slot, srv_inc_));
    // without a slot the passes take the blit samplers, looked up at this point as they always were
    uint32_t si = sampler_slot ? *sampler_slot : EnsureBlitSamplers();
    SetSamplerTable(Gpu(smp_heap_.Get(), si, smp_inc_));
    SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl_->DrawInstanced(3, 1, 0, 0);
}

void D3D12Backend::Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o) {
    uint32_t slot = AllocSrvSlots(2);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    sv.Format = kBgra;
    dev_->CreateShaderResourceView(lut_.Get(), &sv, Cpu(srv_heap_.Get(), slot + 1, srv_inc_));
    uint32_t si = EnsureBlitSamplers();
    Alloc cb = Upload(256, 256);
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3],
                  o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, float(o.taps), o.mul};
    std::memcpy(cb.cpu, c, sizeof(c));
    DrawFullScreen(EnsureBlitPipeline(o.rtv, o.over), cb.gpu, slot, si);
}


uint32_t FillInputLayout(const PsoKey& k, D3D12_INPUT_ELEMENT_DESC el[16]);

// --- the shader cache ---------------------------------------------------------------------------------------------
// Compiled shaders and the pipelines the game used, kept beside the exe (shadercache/):
//   blobs.bin      the bytecode: hash, size, bytes
//   recipes.bin    what each draw's shaders are made from (the vertex program or fixed function key, the pixel key):
//                  a new build makes them all again at its first start
//   psos2.bin      the draws' pipelines: a PsoKey and its shaders' recipe ids (psos.bin before: the key alone)
//   shadows2.bin   the casters' and the veil's: a ShadowKey, the same (shadows.bin before)
//   passes.bin     the full screen passes' sources and targets, this build's alone
//   pipelines.bin  the driver's own (an ID3D12PipelineLibrary)
// The pack can carry the lists made by playing every level (remaster/shadercache/). At start every shader and
// pipeline in them is made, on every core, showing how far it is (app::Progress) when that is more than reading the
// caches: none is made while the game plays.

std::filesystem::path CacheDir() { return app::ExeDir() / "shadercache"; }

template <typename F>
void ReadRecords(const std::filesystem::path& p, F f) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    std::vector<char> all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    f(reinterpret_cast<const uint8_t*>(all.data()), all.size());
}

// tagged records (recipes.bin, passes.bin): the tag, the size, the record
template <typename F>
void ReadTagged(const std::filesystem::path& p, F f) {
    ReadRecords(p, [&](const uint8_t* b, size_t n) {
        for (size_t at = 0; at + 8 <= n;) {
            uint32_t tag, size;
            std::memcpy(&tag, b + at, 4);
            std::memcpy(&size, b + at + 4, 4);
            at += 8;
            if (at + size > n) break;
            f(tag, b + at, size_t(size));
            at += size;
        }
    });
}

void WriteTagged(std::ofstream& out, uint32_t tag, const void* a, size_t na, const void* b = nullptr, size_t nb = 0) {
    uint32_t size = uint32_t(na + nb);
    out.write(reinterpret_cast<const char*>(&tag), 4);
    out.write(reinterpret_cast<const char*>(&size), 4);
    out.write(static_cast<const char*>(a), std::streamsize(na));
    if (nb) out.write(static_cast<const char*>(b), std::streamsize(nb));
}

// a file written whole: to its side first, then over it
template <typename F>
void Rewrite(const std::filesystem::path& p, F f) {
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        f(out);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
}

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;
// a vertex recipe's variants: the draw's own shader, the shadow caster's, the guard band's
enum : uint8_t { kVariantMain, kVariantShadow, kVariantGuard, kVariantTop };
uint64_t VertexRecipeId(uint64_t vkey, uint8_t variant) { return variant ? Hash(&variant, 1, vkey) : vkey; }
// the fixed shaders' ids
constexpr uint64_t kShadowAlphaId[2] = {0x5AD0A1FA00000000ull, 0x5AD0A1FA00000001ull};
constexpr uint64_t kMaskId = 0x5AD0A1FA00000002ull, kVeilId = 0x5AD0A1FA00000003ull;
constexpr uint64_t kTopWaterId = 0x5AD0A1FA00000004ull, kTopCoverId = 0x5AD0A1FA00000005ull;

constexpr uint32_t kVertexTag = 'V', kPixelTag = 'P', kBuildTag = 'B', kPassTag = 'S';
struct VertexRecipe {
    uint64_t vkey, decode;
    FixedKey fk;
    uint16_t regs;
    uint8_t variant, prog, quad, pretransformed, pad[2];
    uint32_t words;  // the program's, after the record
};

struct Recipe {
    uint32_t tag;
    uint8_t variant;
    uint64_t id, key;  // the stable id; the vertex or pixel key
    std::vector<uint8_t> data;
};

bool ParseRecipe(uint32_t tag, const uint8_t* b, size_t n, Recipe& r) {
    r.tag = tag, r.variant = 0;
    if (tag == kPixelTag) {
        if (n != sizeof(PixelKey)) return false;
        r.key = Hash(b, n), r.id = r.key ^ kPixelKeySalt;
    } else {
        VertexRecipe v;
        if (tag != kVertexTag || n < sizeof(v)) return false;
        std::memcpy(&v, b, sizeof(v));
        if (n != sizeof(v) + size_t(v.words) * 4 || v.variant > kVariantTop) return false;
        r.key = v.vkey, r.variant = v.variant, r.id = VertexRecipeId(v.vkey, v.variant);
    }
    r.data.assign(b, b + n);
    return true;
}

// the recipe's source in this build ("" none)
std::string RecipeSource(const Recipe& r) {
    if (r.tag == kPixelTag) {
        PixelKey pk;
        std::memcpy(&pk, r.data.data(), sizeof(pk));
        return PixelHlsl(pk);
    }
    VertexRecipe v;
    std::memcpy(&v, r.data.data(), sizeof(v));
    std::string base;
    if (v.prog) {
        std::vector<uint32_t> program(v.words);
        if (v.words) std::memcpy(program.data(), r.data.data() + sizeof(v), size_t(v.words) * 4);
        base = VertexProgramHlsl(program, v.regs, v.decode);
    } else {
        base = FixedVertexHlsl(v.regs, v.pretransformed != 0, v.decode, v.fk);
    }
    if (v.variant == kVariantShadow) return ShadowVertexHlsl(std::move(base));
    if (v.variant == kVariantGuard) return GuardVertexHlsl(std::move(base));
    if (v.variant == kVariantTop) return TopVertexHlsl(std::move(base));
    return v.quad ? QuadHlsl(std::move(base)) : base;
}

// the veil's pixel shader (RecordVeil): the draw's coverage, its texture's alpha by its vertex alpha
std::string VeilPixelHlsl() {
    return DrawHlslHead() + R"(
Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);
float4 main(VOut i) : SV_Target {
    float2 uv = i.tc0.xy / (i.tc0.w == 0 ? 1 : i.tc0.w);
    // (wherever any smoke is seen, nothing of the corners' shade behind it shows; only its quad's empty edge not)
    return smoothstep(0.004, 0.035, tex0.Sample(smp0, uv).a * saturate(i.d0.a)).xxxx;
}
)";
}

// one source can hold several entry points
uint64_t SourceHash(const std::string& src, const char* entry, const char* profile) {
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    return Hash(src.data(), src.size(), entry_seed);
}

// any thread
ComPtr<ID3DBlob> CompileBlob(const std::string& src, const char* entry, const char* profile, std::string& error) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> err;
    // (hull shaders unoptimised: the optimiser splits the patch constants into phases its own validator rejects)
    UINT flags = std::strncmp(profile, "hs_", 3) == 0 ? D3DCOMPILE_SKIP_OPTIMIZATION : D3DCOMPILE_OPTIMIZATION_LEVEL1;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, profile, flags, 0, &code, &err);
    if (FAILED(hr)) {
        error = err ? static_cast<const char*>(err->GetBufferPointer()) : "?";
        return nullptr;
    }
    return code;
}

// this exe: the passes' sources are its own
uint64_t BuildId() {
    // the executable's link time and size from its own header: the same wherever it is copied or installed (a file's
    // date is the copy's)
    static const uint64_t id = [] {
        auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const uint32_t stamp[2] = {nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage};
        return Hash(stamp, sizeof(stamp), 0);
    }();
    return id;
}

struct PassHead {
    uint32_t rtv;
    float blend;
    uint32_t entry;  // the entry point's length, then it and the source
};

uint64_t PassKey(const char* hlsl, size_t len, const char* entry, DXGI_FORMAT rtv, float blend) {
    uint64_t seed = uint64_t(rtv) << 1 | (blend >= 0 ? 1 : 0);
    uint64_t source_seed = Hash(hlsl, len, seed);
    return Hash(entry, std::strlen(entry), source_seed);
}

const wchar_t* CompilingText() {
    switch (app::Get().language) {
        case 3: return L"Shader werden kompiliert";
        case 4: return L"Compilation des shaders";
        case 5: return L"Compilando shaders";
        case 6: return L"Compilazione degli shader";
        default: return L"Compiling shaders";
    }
}


// the start's work, shown once any of it is more than reading the caches
struct ShaderProgress {
    std::atomic<size_t> done{0};
    size_t total = 1;
    std::atomic<bool> busy{false};
    bool shown = false;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    void Show() {
        // (a few made quickly: not worth a window)
        if (!busy || (!shown && std::chrono::steady_clock::now() - since < std::chrono::milliseconds(250))) return;
        shown = true;
        double f = std::min(1.0, double(done) / double(std::max<size_t>(total, 1)));
        wchar_t line[96];
        swprintf(line, 96, L"%ls...  %d%%", CompilingText(), int(f * 100));
        app::Progress(line, float(f));
    }
    void Close() {
        if (shown) app::Progress(L"", -1);
    }
};


// f(i) for i below n on every core, this thread showing how far they are
template <typename F>
void ParallelFor(size_t n, ShaderProgress& progress, F f) {
    if (!n) return;
    std::atomic<size_t> next{0};
    unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    std::atomic<unsigned> left{threads};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; t++)
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < n;) {
                f(i);
                progress.done++;
            }
            left--;
        });
    while (left.load()) {
        progress.Show();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    for (auto& t : pool) t.join();
    progress.Show();
}


void D3D12Backend::LoadShaderCache() {
    std::error_code ec;
    std::filesystem::create_directories(CacheDir(), ec);
    auto t0 = std::chrono::steady_clock::now();
    auto load_blobs = [&](const std::filesystem::path& p) {
        ReadRecords(p, [&](const uint8_t* b, size_t n) {
            size_t at = 0;
            while (at + 12 <= n) {
                uint64_t h;
                uint32_t size;
                std::memcpy(&h, b + at, 8);
                std::memcpy(&size, b + at + 8, 4);
                at += 12;
                if (at + size > n) break;
                if (!blobs_.count(h)) {
                    ComPtr<ID3DBlob> blob;
                    if (SUCCEEDED(D3DCreateBlob(size, &blob))) {
                        std::memcpy(blob->GetBufferPointer(), b + at, size);
                        blobs_[h] = blob;
                    }
                }
                at += size;
            }
        });
    };
    std::filesystem::path seed = std::filesystem::path(remaster::Dir()) / "shadercache";
    load_blobs(seed / "blobs.bin");
    load_blobs(CacheDir() / "blobs.bin");
    size_t blobs_read = blobs_.size();
    std::vector<uint64_t> fixed_blobs;
    {
        uint64_t h;
        weapon_gs_ = Compile(WeaponCullGsHlsl(), "gs", "gs_5_0", h);  // before the pipelines that take it
        fixed_blobs.push_back(h);
        point_gs_ = Compile(PointSpriteGsHlsl(), "gs", "gs_5_0", h);
        fixed_blobs.push_back(h);
        std::string tess = TessHlsl();
        tess_hs_ = Compile(tess, "hs", "hs_5_0", h);
        fixed_blobs.push_back(h);
        tess_ds_ = Compile(tess, "ds", "ds_5_0", h);
        fixed_blobs.push_back(h);
    }
    // the casters' and the veil's own
    shadow_alpha_ps_[0] = FixedShader(kShadowAlphaId[0], ShadowAlphaHlsl(false), "main", "ps_5_0");
    shadow_alpha_ps_[1] = FixedShader(kShadowAlphaId[1], ShadowAlphaHlsl(true), "main", "ps_5_0");
    mask_ps_ = FixedShader(kMaskId, "float4 main(float4 pos : SV_Position) : SV_Target { return 1; }", "main", "ps_5_0");
    veil_ps_ = FixedShader(kVeilId, VeilPixelHlsl(), "main", "ps_5_0");
    // the top view's (Water Interaction): the water's height; what crosses it, how far up the slab about the water's
    // height (tdp.x below, tdp.y above) it reaches
    top_water_ps_ = FixedShader(kTopWaterId, "float4 main(float4 pos : SV_Position, float wy : TEXCOORD0) : SV_Target { return wy; }",
                                "main", "ps_5_0");
    top_cover_ps_ = FixedShader(kTopCoverId,
                                "Texture2D<float> water : register(t0);\n"
                                "cbuffer TD : register(b2) { row_major float4x4 tdm; float4 tdy; float4 tdv; float4 tdp; };\n"
                                "float4 main(float4 pos : SV_Position, float wy : TEXCOORD0) : SV_Target {\n"
                                "    float w = water.Load(int3(pos.xy, 0));\n"
                                "    if (w < -1e8 || wy < w - tdp.x || wy > w + tdp.y) discard;\n"
                                "    return wy - (w - tdp.x);\n"
                                "}\n",
                                "main", "ps_5_0");
    // the pipeline library of this device and driver, or a new one. Its pipelines are named by their keys, which leave
    // out what all share (the root signature, the geometry and tessellation shaders): a library made with others is
    // dropped, as its pipelines would never load again nor could be replaced
    uint64_t build = root_hash_;
    for (uint64_t h : fixed_blobs) build = Hash(&h, 8, build);
    {
        uint64_t had = 0;
        ReadRecords(CacheDir() / "pipelines.id", [&](const uint8_t* b, size_t n) {
            if (n == 8) std::memcpy(&had, b, 8);
        });
        if (had != build) {
            std::filesystem::remove(CacheDir() / "pipelines.bin", ec);
            Rewrite(CacheDir() / "pipelines.id", [&](std::ofstream& out) { out.write(reinterpret_cast<const char*>(&build), 8); });
            if (had) LOGR("d3d12: shader cache: the pipeline library was another build's, made again");
        }
    }
    ComPtr<ID3D12Device1> dev1;
    if (SUCCEEDED(dev_.As(&dev1))) {
        ReadRecords(CacheDir() / "pipelines.bin", [&](const uint8_t* b, size_t n) { library_data_.assign(b, b + n); });
        if (library_data_.empty() ||
            FAILED(dev1->CreatePipelineLibrary(library_data_.data(), library_data_.size(), IID_PPV_ARGS(&library_)))) {
            library_data_.clear();
            dev1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library_));
        }
    }

    // the lists
    std::vector<Recipe> recipes;
    auto load_recipes = [&](const std::filesystem::path& p) {
        ReadTagged(p, [&](uint32_t tag, const uint8_t* b, size_t n) {
            Recipe r;
            if (ParseRecipe(tag, b, n, r) && recipes_known_.insert(r.id).second) recipes.push_back(std::move(r));
        });
    };
    load_recipes(seed / "recipes.bin");
    load_recipes(CacheDir() / "recipes.bin");
    struct PsoRecord {
        PsoKey k;
        uint64_t vs, ps;  // the shaders' recipe ids
    };
    struct ShadowRecord {
        ShadowKey k;
        uint64_t vs, ps;
    };
    std::vector<PsoRecord> pso_records;
    std::vector<ShadowRecord> shadow_records;
    bool legacy = false;
    auto load_fixed = [&](const std::filesystem::path& p, auto& out, bool with_ids) {
        using Rec = typename std::decay_t<decltype(out)>::value_type;
        constexpr size_t key_size = sizeof(Rec::k);
        size_t size = with_ids ? key_size + 16 : key_size;
        ReadRecords(p, [&](const uint8_t* b, size_t n) {
            for (size_t at = 0; at + size <= n; at += size) {
                Rec r{};
                std::memcpy(&r.k, b + at, key_size);
                if (with_ids) {
                    std::memcpy(&r.vs, b + at + key_size, 8);
                    std::memcpy(&r.ps, b + at + key_size + 8, 8);
                } else {
                    r.vs = r.k.vs, r.ps = r.k.ps;
                    legacy = true;
                }
                out.push_back(r);
            }
        });
    };
    for (const auto& dir : {seed, CacheDir()}) {
        load_fixed(dir / "psos.bin", pso_records, false);
        load_fixed(dir / "psos2.bin", pso_records, true);
        load_fixed(dir / "shadows.bin", shadow_records, false);
        load_fixed(dir / "shadows2.bin", shadow_records, true);
    }
    struct PassRecord {
        PassHead head;
        std::string entry, text;
        uint64_t key, vs = 0, ps = 0;
    };
    std::vector<PassRecord> passes;
    {
        bool ours = false;
        ReadTagged(CacheDir() / "passes.bin", [&](uint32_t tag, const uint8_t* b, size_t n) {
            if (tag == kBuildTag && n == 8) {
                uint64_t id;
                std::memcpy(&id, b, 8);
                ours = id == BuildId();
            } else if (tag == kPassTag && ours && n >= sizeof(PassHead)) {
                PassRecord p;
                std::memcpy(&p.head, b, sizeof(PassHead));
                if (sizeof(PassHead) + p.head.entry > n) return;
                p.entry.assign(reinterpret_cast<const char*>(b) + sizeof(PassHead), p.head.entry);
                p.text.assign(reinterpret_cast<const char*>(b) + sizeof(PassHead) + p.head.entry, n - sizeof(PassHead) - p.head.entry);
                p.key = PassKey(p.text.data(), p.text.size(), p.entry.c_str(), DXGI_FORMAT(p.head.rtv), p.head.blend);
                if (passes_known_.insert(p.key).second) passes.push_back(std::move(p));
            }
        });
        if (!ours) {
            // another build's: this one's from here
            Rewrite(CacheDir() / "passes.bin", [&](std::ofstream& out) {
                uint64_t id = BuildId();
                WriteTagged(out, kBuildTag, &id, 8);
            });
        }
    }
    // the per pixel lighting code the lit pixel shaders take (PixelLightingCode by PixelKey::lit) is made from the
    // vertex programs: made again from their recipes before the pixel shaders
    for (const Recipe& r : recipes) {
        if (r.tag != kVertexTag) continue;
        VertexRecipe v;
        std::memcpy(&v, r.data.data(), sizeof(v));
        if (v.variant != kVariantMain || v.pretransformed) continue;
        if (v.prog) {
            std::vector<uint32_t> program(v.words);
            if (v.words) std::memcpy(program.data(), r.data.data() + sizeof(v), size_t(v.words) * 4);
            PixelLightingId(program, v.regs);
        } else {
            FixedLightingId(v.fk, v.regs);
        }
    }
    ShaderProgress progress;
    progress.total = recipes.size() + passes.size() * 2 + pso_records.size() + shadow_records.size() + passes.size();

    // the shaders: each recipe's source in this build, compiled where its bytecode is not kept
    struct Made {
        uint64_t h = 0;
        ComPtr<ID3DBlob> blob;
        std::string error;
    };
    size_t shader_jobs = recipes.size() + passes.size() * 2;
    std::vector<Made> made_shaders(shader_jobs);
    std::atomic<size_t> compiled{0};
    ParallelFor(shader_jobs, progress, [&](size_t i) {
        std::string src;
        const char* entry = "main";
        const char* profile = "vs_5_0";
        if (i < recipes.size()) {
            src = RecipeSource(recipes[i]);
            if (recipes[i].tag == kPixelTag) profile = "ps_5_0";
        } else {
            const PassRecord& p = passes[(i - recipes.size()) / 2];
            bool ps = (i - recipes.size()) & 1;
            src = p.text;
            entry = ps ? p.entry.c_str() : "vs";
            profile = ps ? "ps_5_0" : "vs_5_0";
        }
        if (src.empty()) return;
        Made& m = made_shaders[i];
        uint64_t h = SourceHash(src, entry, profile);
        auto it = blobs_.find(h);
        if (it != blobs_.end()) {
            if (it->second) m.h = h;
            return;
        }
        progress.busy = true;
        m.blob = CompileBlob(src, entry, profile, m.error);
        if (m.blob) m.h = h, compiled++;
    });
    {
        std::ofstream out(CacheDir() / "blobs.bin", std::ios::binary | std::ios::app);
        for (size_t i = 0; i < shader_jobs; i++) {
            Made& m = made_shaders[i];
            if (!m.error.empty()) LOGE("d3d12: shader cache: a recipe failed to compile: %s", m.error.c_str());
            if (!m.h) continue;
            if (m.blob && !blobs_.count(m.h)) {
                blobs_[m.h] = m.blob;
                uint32_t size = uint32_t(m.blob->GetBufferSize());
                out.write(reinterpret_cast<const char*>(&m.h), 8);
                out.write(reinterpret_cast<const char*>(&size), 4);
                out.write(static_cast<const char*>(m.blob->GetBufferPointer()), size);
            }
            if (i >= recipes.size()) {
                PassRecord& p = passes[(i - recipes.size()) / 2];
                ((i - recipes.size()) & 1 ? p.ps : p.vs) = m.h;
                continue;
            }
            const Recipe& r = recipes[i];
            stable_of_[m.h] = r.id, blob_of_[r.id] = m.h;
            // the draws find them made: no source generated while the game plays
            if (r.tag == kPixelTag) shader_src_[r.key ^ kPixelKeySalt] = m.h;
            else if (r.variant == kVariantShadow) shadow_vs_[r.key] = m.h;
            else if (r.variant == kVariantGuard) guard_vs_[r.key] = m.h;
            else if (r.variant == kVariantTop) top_vs_[r.key] = m.h;
            else shader_src_[r.key] = m.h;
        }
    }
    auto resolve = [&](uint64_t id) {
        auto it = blob_of_.find(id);
        return it == blob_of_.end() ? id : it->second;
    };
    auto have = [&](uint64_t h) {
        auto it = blobs_.find(h);
        return it != blobs_.end() && it->second;
    };

    // the draws' pipelines: from the library when it has them, else made
    std::vector<PsoKey> keys;
    std::unordered_set<uint64_t> seen;
    size_t dropped = 0, upgraded = 0;  // upgraded: records by bytecode whose shaders have recipes now
    for (const PsoRecord& r : pso_records) {
        PsoKey k = r.k;
        k.vs = resolve(r.vs), k.ps = resolve(r.ps);
        if (!have(k.vs) || !have(k.ps)) {
            dropped++, progress.done++;
            continue;
        }
        if (StableOf(k.vs) != r.vs || StableOf(k.ps) != r.ps) upgraded++;
        if (seen.insert(Hash(&k, sizeof(k))).second) keys.push_back(k);
        else progress.done++;
    }
    known_psos_ = seen;
    std::vector<std::pair<uint64_t, ComPtr<ID3D12PipelineState>>> made(keys.size());
    std::atomic<size_t> from_library{0};
    std::vector<uint8_t> fresh(keys.size(), 0);
    ParallelFor(keys.size(), progress, [&](size_t i) {
        const PsoKey& k = keys[i];
        D3D12_INPUT_ELEMENT_DESC el[16];
        uint32_t ne = FillInputLayout(k, el);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.InputLayout = {el, ne};
        FillPipelineDesc(k, blobs_.at(k.vs).Get(), blobs_.at(k.ps).Get(), d);
        uint64_t h = Hash(&k, sizeof(k));
        wchar_t name[24];
        swprintf(name, 24, L"%016llx", (unsigned long long)h);
        ComPtr<ID3D12PipelineState> pso;
        // (one the weapon's geometry shader was left out of is kept without it)
        auto load = [&](const D3D12_GRAPHICS_PIPELINE_STATE_DESC& x) {
            return library_ && SUCCEEDED(library_->LoadGraphicsPipeline(name, &x, IID_PPV_ARGS(&pso)));
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC whole = d;
        whole.GS = {};
        if (load(d) || (d.GS.pShaderBytecode && load(whole))) {
            from_library++;
        } else {
            progress.busy = true;
            if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso))) && d.GS.pShaderBytecode) {
                d.GS = {};  // as EnsurePipeline: drawn whole
                dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
            }
            if (pso) fresh[i] = 1;
        }
        made[i] = {h, pso};
    });
    size_t count = 0, new_ones = 0;
    for (size_t i = 0; i < made.size(); i++) {
        if (!made[i].second) continue;
        psos_[made[i].first] = made[i].second;
        count++;
        if (fresh[i] && library_) {
            wchar_t name[24];
            swprintf(name, 24, L"%016llx", (unsigned long long)made[i].first);
            if (SUCCEEDED(library_->StorePipeline(name, made[i].second.Get()))) new_ones++, library_dirty_ = true;
        }
    }

    // the casters' and the veil's
    std::vector<ShadowKey> skeys;
    std::unordered_set<uint64_t> sseen;
    for (const ShadowRecord& r : shadow_records) {
        ShadowKey k = r.k;
        k.vs = resolve(r.vs), k.ps = r.ps ? resolve(r.ps) : 0;
        if (!have(k.vs) || (k.ps && !have(k.ps))) {
            dropped++, progress.done++;
            continue;
        }
        if (StableOf(k.vs) != r.vs || (k.ps && StableOf(k.ps) != r.ps)) upgraded++;
        if (sseen.insert(Hash(&k, sizeof(k))).second) skeys.push_back(k);
        else progress.done++;
    }
    std::vector<ComPtr<ID3D12PipelineState>> smade(skeys.size());
    std::vector<uint8_t> sfresh(skeys.size(), 0);
    ParallelFor(skeys.size(), progress, [&](size_t i) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        D3D12_INPUT_ELEMENT_DESC el[16];
        if (!ShadowDesc(skeys[i], d, el)) return;
        uint64_t h = Hash(&skeys[i], sizeof(ShadowKey));
        wchar_t name[24];
        swprintf(name, 24, L"s%015llx", (unsigned long long)(h >> 4));
        if (library_ && SUCCEEDED(library_->LoadGraphicsPipeline(name, &d, IID_PPV_ARGS(&smade[i])))) return;
        progress.busy = true;
        if (SUCCEEDED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&smade[i])))) sfresh[i] = 1;
    });
    size_t shadow_count = 0, shadow_made = 0, passes_made = 0;
    for (size_t i = 0; i < skeys.size(); i++) {
        if (!smade[i]) continue;
        shadow_made += sfresh[i];
        uint64_t h = Hash(&skeys[i], sizeof(ShadowKey));
        shadow_psos_[h] = smade[i];
        known_shadow_.insert(h);
        shadow_count++;
        if (sfresh[i] && library_) {
            wchar_t name[24];
            swprintf(name, 24, L"s%015llx", (unsigned long long)(h >> 4));
            if (SUCCEEDED(library_->StorePipeline(name, smade[i].Get()))) library_dirty_ = true;
        }
    }

    // the passes'
    std::vector<ComPtr<ID3D12PipelineState>> pmade(passes.size());
    std::vector<uint8_t> pfresh(passes.size(), 0);
    ParallelFor(passes.size(), progress, [&](size_t i) {
        const PassRecord& p = passes[i];
        if (!have(p.vs) || !have(p.ps)) return;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
        PassDesc(blobs_.at(p.vs).Get(), blobs_.at(p.ps).Get(), DXGI_FORMAT(p.head.rtv), p.head.blend, d);
        wchar_t name[24];
        swprintf(name, 24, L"p%015llx", (unsigned long long)(p.key >> 4));
        if (library_ && SUCCEEDED(library_->LoadGraphicsPipeline(name, &d, IID_PPV_ARGS(&pmade[i])))) return;
        progress.busy = true;
        if (SUCCEEDED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pmade[i])))) pfresh[i] = 1;
    });
    for (size_t i = 0; i < passes.size(); i++) {
        if (!pmade[i]) continue;
        blit_psos_[passes[i].key] = pmade[i];
        passes_made += pfresh[i];
        if (pfresh[i] && library_) {
            wchar_t name[24];
            swprintf(name, 24, L"p%015llx", (unsigned long long)(passes[i].key >> 4));
            if (SUCCEEDED(library_->StorePipeline(name, pmade[i].Get()))) library_dirty_ = true;
        }
    }
    progress.Close();

    // the lists again without what no longer builds, by recipe ids (the old files' keys carried bytecode hashes)
    if (legacy || dropped || upgraded) {
        Rewrite(CacheDir() / "psos2.bin", [&](std::ofstream& out) {
            for (const PsoKey& k : keys) {
                uint64_t ids[2] = {StableOf(k.vs), StableOf(k.ps)};
                out.write(reinterpret_cast<const char*>(&k), sizeof(k));
                out.write(reinterpret_cast<const char*>(ids), 16);
            }
        });
        Rewrite(CacheDir() / "shadows2.bin", [&](std::ofstream& out) {
            for (const ShadowKey& k : skeys) {
                uint64_t ids[2] = {StableOf(k.vs), k.ps ? StableOf(k.ps) : 0};
                out.write(reinterpret_cast<const char*>(&k), sizeof(k));
                out.write(reinterpret_cast<const char*>(ids), 16);
            }
        });
        std::filesystem::remove(CacheDir() / "psos.bin", ec);
        std::filesystem::remove(CacheDir() / "shadows.bin", ec);
    }
    // the bytecode of older builds goes once it is most of the file
    {
        std::unordered_set<uint64_t> needed(fixed_blobs.begin(), fixed_blobs.end());
        for (auto& [id, h] : blob_of_) needed.insert(h);
        for (const PsoKey& k : keys) needed.insert(k.vs), needed.insert(k.ps);
        for (const ShadowKey& k : skeys) needed.insert(k.vs), needed.insert(k.ps);
        for (const PassRecord& p : passes) needed.insert(p.vs), needed.insert(p.ps);
        if (blobs_read > needed.size() * 3 / 2 + 200) {
            size_t kept = 0;
            Rewrite(CacheDir() / "blobs.bin", [&](std::ofstream& out) {
                for (uint64_t h : needed) {
                    auto it = blobs_.find(h);
                    if (it == blobs_.end() || !it->second) continue;
                    uint32_t size = uint32_t(it->second->GetBufferSize());
                    out.write(reinterpret_cast<const char*>(&h), 8);
                    out.write(reinterpret_cast<const char*>(&size), 4);
                    out.write(static_cast<const char*>(it->second->GetBufferPointer()), size);
                    kept++;
                }
            });
            LOGR("d3d12: shader cache: %zu old shaders dropped, %zu kept", blobs_read - std::min(blobs_read, kept), kept);
        }
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOGR("d3d12: shader cache: %zu recipes (%zu compiled), %zu passes, %zu pipelines ready (%zu from the driver's cache, "
         "%zu made), %zu shadow pipelines (%zu made), %zu passes made, %zu dropped, %zu named by recipe now, in %.0f ms%s",
         recipes.size(), size_t(compiled), passes.size(), count, size_t(from_library), new_ones, shadow_count, shadow_made,
         passes_made, dropped, upgraded, ms,
         progress.shown ? " (progress shown)" : "");
    SaveShaderCache();
}

void D3D12Backend::SaveShaderCache() {
    if (!library_ || !library_dirty_) return;
    library_dirty_ = false;
    size_t n = library_->GetSerializedSize();
    std::vector<uint8_t> data(n);
    if (FAILED(library_->Serialize(data.data(), n))) return;
    Rewrite(CacheDir() / "pipelines.bin",
            [&](std::ofstream& out) { out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(n)); });
}

void D3D12Backend::RememberBlob(uint64_t h, ID3DBlob* b) {
    std::ofstream out(CacheDir() / "blobs.bin", std::ios::binary | std::ios::app);
    uint32_t size = uint32_t(b->GetBufferSize());
    out.write(reinterpret_cast<const char*>(&h), 8);
    out.write(reinterpret_cast<const char*>(&size), 4);
    out.write(static_cast<const char*>(b->GetBufferPointer()), size);
}

void D3D12Backend::RememberPipeline(const PsoKey& k, uint64_t h, ID3D12PipelineState* pso) {
    if (!known_psos_.insert(h).second) return;
    std::ofstream out(CacheDir() / "psos2.bin", std::ios::binary | std::ios::app);
    uint64_t ids[2] = {StableOf(k.vs), StableOf(k.ps)};
    out.write(reinterpret_cast<const char*>(&k), sizeof(k));
    out.write(reinterpret_cast<const char*>(ids), 16);
    if (library_ && pso) {
        wchar_t name[24];
        swprintf(name, 24, L"%016llx", (unsigned long long)h);
        if (SUCCEEDED(library_->StorePipeline(name, pso))) library_dirty_ = true;
    }
}

void D3D12Backend::RememberVertexRecipe(const State& s, const Assembled& a, const DrawShaders& sh, uint8_t variant,
                                        uint64_t h) {
    uint64_t id = VertexRecipeId(sh.vkey, variant);
    stable_of_[h] = id, blob_of_[id] = h;
    if (!recipes_known_.insert(id).second) return;
    VertexRecipe r;
    std::memset(&r, 0, sizeof(r));
    r.vkey = sh.vkey, r.decode = a.decode, r.fk = sh.fk, r.regs = a.regs;
    r.variant = variant, r.prog = sh.prog, r.quad = sh.quad, r.pretransformed = a.pretransformed;
    const std::vector<uint32_t>* program = sh.prog ? &s.vs->program : nullptr;
    r.words = program ? uint32_t(program->size()) : 0;
    std::ofstream out(CacheDir() / "recipes.bin", std::ios::binary | std::ios::app);
    WriteTagged(out, kVertexTag, &r, sizeof(r), program ? program->data() : nullptr, size_t(r.words) * 4);
}

void D3D12Backend::RememberPixelRecipe(const PixelKey& pk, uint64_t pkey, uint64_t h) {
    uint64_t id = pkey ^ kPixelKeySalt;
    stable_of_[h] = id, blob_of_[id] = h;
    if (!recipes_known_.insert(id).second) return;
    std::ofstream out(CacheDir() / "recipes.bin", std::ios::binary | std::ios::app);
    WriteTagged(out, kPixelTag, &pk, sizeof(pk));
}

uint64_t D3D12Backend::FixedShader(uint64_t id, const std::string& src, const char* entry, const char* profile) {
    uint64_t h = 0;
    if (!Compile(src, entry, profile, h)) return 0;
    stable_of_[h] = id, blob_of_[id] = h;
    return h;
}

ID3DBlob* D3D12Backend::Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash) {
    hash = SourceHash(src, entry, profile);
    auto it = blobs_.find(hash);
    if (it != blobs_.end()) return it->second.Get();
    PerfScope perf_compile{g_perf.compile};
    g_perf.shaders++;
    std::string error;
    ComPtr<ID3DBlob> code = CompileBlob(src, entry, profile, error);
    if (!code) {
        LOGE("d3d12: %s compile failed: %s", profile, error.c_str());
        LOGE("%s", src.c_str());
        blobs_[hash] = nullptr;
        return nullptr;
    }
    blobs_[hash] = code;
    RememberBlob(hash, code.Get());
    return code.Get();
}

// a full screen pass's pipeline: solid, no culling, one target, writing all channels; callers set the blend
D3D12_GRAPHICS_PIPELINE_STATE_DESC D3D12Backend::FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps,
                                                                        DXGI_FORMAT rtv) const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rtv;
    d.SampleDesc.Count = 1;
    return d;
}

ID3D12PipelineState* D3D12Backend::EnsureBlitPipeline(DXGI_FORMAT rtv, bool over) {
    auto& pso = blit_psos_[uint64_t(rtv) | uint64_t(over) << 32];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kBlitHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kBlitHlsl, "psmain", "ps_5_0", h);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    if (over) {
        auto& blend = d.BlendState.RenderTarget[0];
        // premultiplied colour, the target keeps its alpha
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    return pso.Get();
}

// Pass's pipeline for the shader, kept in blit_psos_; null when it can't be made
void D3D12Backend::PassDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv, float blend,
                            D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const {
    d = FullScreenPipelineDesc(vs, ps, rtv);
    if (blend < -1.5f) {
        // -2: premultiplied alpha over what is there
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_ONE;
        b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (blend >= 0) {
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
        b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
}

ID3D12PipelineState* D3D12Backend::EnsurePassPipeline(const char* hlsl, const char* entry, DXGI_FORMAT rtv,
                                                      float blend) {
    size_t len = std::strlen(hlsl);
    uint64_t key = PassKey(hlsl, len, entry, rtv, blend);
    auto& pso = blit_psos_[key];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(hlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(hlsl, entry, "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
    PassDesc(vs, ps, rtv, blend, d);
    wchar_t name[24];
    swprintf(name, 24, L"p%015llx", (unsigned long long)(key >> 4));
    if (!library_ || FAILED(library_->LoadGraphicsPipeline(name, &d, IID_PPV_ARGS(&pso)))) {
        g_perf.psos++;
        if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
        if (library_ && SUCCEEDED(library_->StorePipeline(name, pso.Get()))) library_dirty_ = true;
    }
    // remembered for the next start (passes.bin)
    if (passes_known_.insert(key).second) {
        PassHead head{uint32_t(rtv), blend, uint32_t(std::strlen(entry))};
        std::string rec(reinterpret_cast<const char*>(&head), sizeof(head));
        rec += entry;
        rec.append(hlsl, len);
        std::ofstream out(CacheDir() / "passes.bin", std::ios::binary | std::ios::app);
        WriteTagged(out, kPassTag, rec.data(), rec.size());
    }
    return pso.Get();
}

void D3D12Backend::Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh,
                        uint32_t w, uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3],
                        const float p[8], float blend) {
    ID3D12PipelineState* pso = EnsurePassPipeline(hlsl, entry, rtv, blend);
    if (!pso) return;
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 3 && src[i] ? fmt[i] : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 3 ? src[i] : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Alloc cb = Upload(std::max<uint64_t>(256, (pass_floats_ * 4 + 255) & ~255ull), 256);
    std::memcpy(cb.cpu, p, pass_floats_ * sizeof(float));
    pass_floats_ = 8;
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(w, h);
    if (blend >= 0) {
        float f[4] = {blend, blend, blend, blend};
        SetBlendFactor(f);
    }
    DrawFullScreen(pso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
}

// a copy of the target to read while drawing to it
ID3D12Resource* D3D12Backend::Feedback(Target& t) {
    MsaaResolve();
    if (!t.fb) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = t.res->GetDesc();
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        t.fb_state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.fb_state, nullptr,
                                                 IID_PPV_ARGS(&t.fb)))) {
            return nullptr;
        }
    }
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_COPY_DEST);
    cl_->CopyResource(t.fb.Get(), t.res.Get());
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return t.fb.Get();
}

// the key's vertex attributes, ATTR0 to ATTR15 in register order; returns how many
uint32_t FillInputLayout(const PsoKey& k, D3D12_INPUT_ELEMENT_DESC el[16]) {
    uint32_t n = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    }
    return n;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(uint32_t topo) {
    switch (topo) {
        case 0: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case 1: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW

void FillDepthStencilDesc(const PsoKey& k, D3D12_DEPTH_STENCIL_DESC& ds) {
    ds.DepthEnable = k.zen;
    ds.DepthWriteMask = k.zwrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = Cmp(k.zfunc);
    ds.StencilEnable = k.sten;
    ds.StencilReadMask = UINT8(k.smask);
    ds.StencilWriteMask = UINT8(k.swmask);
    D3D12_DEPTH_STENCILOP_DESC so{StencilOp(k.sfail), StencilOp(k.szfail), StencilOp(k.spass), Cmp(k.sfunc)};
    ds.FrontFace = ds.BackFace = so;
}

// everything of the pipeline but its input layout
void D3D12Backend::FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps,
                                    D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const {
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& b = d.BlendState.RenderTarget[0];
    b.BlendEnable = k.blend;
    b.SrcBlend = Blend(k.src, false);
    b.DestBlend = Blend(k.dst, false);
    b.BlendOp = BlendOp(k.op);
    b.SrcBlendAlpha = Blend(k.src, true);
    b.DestBlendAlpha = Blend(k.dst, true);
    b.BlendOpAlpha = BlendOp(k.op);
    b.RenderTargetWriteMask = UINT8(k.wmask);
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = k.fill == kFillWireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = k.cull ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = k.ccw;
    d.RasterizerState.DepthBias = k.zbias;
    d.RasterizerState.SlopeScaledDepthBias = k.zslope;
    d.RasterizerState.DepthClipEnable = TRUE;
    FillDepthStencilDesc(k, d.DepthStencilState);
    d.PrimitiveTopologyType = TopologyType(k.topo & 0xFF);
    if ((k.topo & kPointSpriteKey) && point_gs_) d.GS = {point_gs_->GetBufferPointer(), point_gs_->GetBufferSize()};
    else if ((k.topo & kWeaponCullKey) && weapon_gs_) d.GS = {weapon_gs_->GetBufferPointer(), weapon_gs_->GetBufferSize()};
    if ((k.topo & kTessKey) && tess_hs_ && tess_ds_) {
        d.HS = {tess_hs_->GetBufferPointer(), tess_hs_->GetBufferSize()};
        d.DS = {tess_ds_->GetBufferPointer(), tess_ds_->GetBufferSize()};
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    }
    if ((k.topo & kGbufKey) && k.has_rt) {
        d.BlendState.IndependentBlendEnable = TRUE;
        d.BlendState.RenderTarget[1].BlendEnable = FALSE;
        d.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    d.NumRenderTargets = k.has_rt ? ((k.topo & kGbufKey) ? 2 : 1) : 0;
    d.RTVFormats[0] = k.has_rt ? DXGI_FORMAT(k.rtfmt) : DXGI_FORMAT_UNKNOWN;
    d.DSVFormat = k.has_ds ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = (k.topo & kMsaaKey) ? (k.topo >> 16) & 0xF : 1;
    // the cut out's soft edge (the pixel shader's alpha past the reference, alphatest) as samples covered
    d.BlendState.AlphaToCoverageEnable = (k.topo & kMsaaKey) && (k.topo & kA2cKey);
}

ID3D12PipelineState* D3D12Backend::EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps) {
    // (most draws take the pipeline of the draw before; psos_ keeps every pipeline it made)
    if (last_pso_ok_ && !std::memcmp(&k, &last_pso_key_, sizeof(k))) return last_pso_;
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) {
        last_pso_key_ = k, last_pso_ = it->second.Get(), last_pso_ok_ = true;
        return it->second.Get();
    }
    D3D12_INPUT_ELEMENT_DESC el[16];
    uint32_t n = FillInputLayout(k, el);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.InputLayout = {el, n};
    FillPipelineDesc(k, vs, ps, d);
    ComPtr<ID3D12PipelineState> pso;
    g_perf.psos++;
    HRESULT hr = dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    if (FAILED(hr) && d.GS.pShaderBytecode) {
        // a vertex shader whose outputs the weapon's geometry shader does not take: drawn whole
        static int told = 0;
        if (told++ < 4) LOGI("d3d12: the weapon's geometry shader left out of a pipeline (vs %016llx)", (unsigned long long)k.vs);
        d.GS = {};
        hr = dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    }
    if (FAILED(hr)) {
        LOGE("d3d12: pipeline creation failed");
        // with the debug layer (XBE_D3D12_DEBUG), why
        ComPtr<ID3D12InfoQueue> q;
        if (SUCCEEDED(dev_.As(&q))) {
            for (UINT64 i = 0, n = q->GetNumStoredMessages(); i < n; i++) {
                SIZE_T len = 0;
                q->GetMessage(i, nullptr, &len);
                std::vector<uint8_t> m(len);
                auto* msg = reinterpret_cast<D3D12_MESSAGE*>(m.data());
                if (SUCCEEDED(q->GetMessage(i, msg, &len))) LOGE("d3d12: %s", msg->pDescription);
            }
            q->ClearStoredMessages();
        }
    }
    psos_[h] = pso;
    RememberPipeline(k, h, pso.Get());
    return pso.Get();
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

ID3D12Resource* D3D12Backend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                                DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    if (msaa_active_ && (&t == msaa_for_rt_ || &t == msaa_for_ds_)) MsaaResolve();
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.h) / (t.gh * stages.k[stage]);
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (ext_) ext_->Sampled(d, &t, 0, info);
    sv.Format = t.depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : t.fmt;
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (feedback) return Feedback(t);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    info.kind = kTex2D;
    return t.res.Get();
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

// false when the texture could not be made, its entry dropped
bool D3D12Backend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    g_perf.newtex++;
    if (tx.res) CurrentFrame().garbage.push_back(tx.res);
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    bool bc = fi.compressed;
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = d.depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = UINT16(d.cube ? 6 : d.depth);
    td.MipLevels = UINT16(tx.levels);
    td.Format = tx.fmt;
    td.SampleDesc.Count = 1;
    if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&tx.res)))) {
        textures_.erase(key);
        return false;
    }
    return true;
}

void CopyLevelRows(uint8_t* out, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT rows, UINT64 row_size,
                   const std::vector<uint8_t>& px, uint32_t pitch) {
    uint32_t slices = fp.Footprint.Depth;
    size_t src_rows = pitch ? px.size() / pitch / std::max(slices, 1u) : 0;
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = out + (size_t(z) * rows + y) * fp.Footprint.RowPitch;
            if (y < src_rows) {
                std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row_size));
            } else {
                std::memset(dst, 0, size_t(row_size));
            }
        }
    }
}

void D3D12Backend::UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px,
                                      uint32_t pitch) {
    D3D12_RESOURCE_DESC rd = tx.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    uint32_t sub = f * tx.levels + l;
    dev_->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, &row_size, &bytes);
    Alloc a = Upload(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    CopyLevelRows(a.cpu, fp, rows, row_size, px, pitch);
    fp.Offset = a.off;
    D3D12_TEXTURE_COPY_LOCATION dl{tx.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dl.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint = fp;
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12Backend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                                 const uint32_t* colours, bool same) {
    D3D12_RESOURCE_STATES st = same ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_COPY_DEST);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            if (f == 0 && l == 0) {
                tx.id = remaster::Identify(d, p, colours);
                remaster::Dump(tx.id, d, fi.host, px, pitch, p, colours);
                // the remastered maps read as soon as the game loads the texture (its level's loading screen), not
                // when a draw first shows it
                if (tx.id && remaster::Active() && remaster::Find(tx.id) && !remaster_.count(tx.id)) remaster::Request(tx.id);
            }
            if (ext_) ext_->Decoded(d, fi, f, l, px, pitch, tx.flags);
            UploadTextureLevel(tx, f, l, px, pitch);
        }
    }
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void FillTextureView(const Tex& tx, const TextureDesc& d, D3D12_SHADER_RESOURCE_VIEW_DESC& sv) {
    sv.Format = tx.fmt;
    if (d.cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = tx.levels;
    } else if (d.depth > 1) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = tx.levels;
    } else {
        sv.Texture2D.MipLevels = tx.levels;
    }
}

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    const uint32_t* colours = nullptr;  // 256 (fewer by the size code, the rest 0)
    uint32_t va = 0;
    uint64_t hash = 0;
};
// the palettes as last read, by their address: the same bytes again keep their hash (a P8 texture's palette is read
// at every bind; hashing it each time was a share of the draws' cost)
struct PaletteCache {
    uint32_t va = 0;
    size_t bytes = 0;
    uint32_t colours[256] = {};
    uint64_t hash = 0;
};
PaletteCache g_palettes[16];

// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;          // D3DFMT_P8
    constexpr uint32_t kPaletteCommon = 0;     // D3DPalette: Common, its top two bits the size code
    constexpr uint32_t kPaletteData = 4;       // D3DPalette: Data, physical address of the colours
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(g_palettes[0].colours) >> size_code) * sizeof(uint32_t);
    const void* src = kernel::HostPtr(palette.va);
    PaletteCache& e = g_palettes[(palette.va >> 4) & 15];
    if (e.va != palette.va || e.bytes != bytes || std::memcmp(e.colours, src, bytes)) {
        std::memset(e.colours, 0, sizeof(e.colours));
        std::memcpy(e.colours, src, bytes);
        e.va = palette.va, e.bytes = bytes;
        e.hash = Hash(e.colours, sizeof(e.colours));
    }
    palette.colours = e.colours;
    palette.hash = e.hash;
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
ID3D12Resource* D3D12Backend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                               DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        PerfClock::time_point perf_t = g_draw_timers ? PerfClock::now() : PerfClock::time_point{};
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ palette.hash;
        if (g_draw_timers) g_perf.sig += PerfMs(perf_t);
        if (sig != tx.sig || !same) {
            PerfScope perf_up{g_perf.texup};
            g_perf.texups++;
            g_perf.texup_bytes += total;
            tx.sig = sig;
            UploadTexture(tx, d, fi, p, palette.va ? palette.colours : nullptr, same);
        }
    }
    FillTextureView(tx, d, stages.sv[stage]);
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    if (ext_) ext_->Sampled(d, nullptr, tx.flags, info);
    if (tx.id && tx.id == g_watch_texture.load(std::memory_order_relaxed)) g_watch_hits++;
    stages.id[stage] = tx.id;
    if (tx.id && !overrides_.empty()) {
        auto it = overrides_.find(tx.id);
        if (it != overrides_.end()) {
            D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
            sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D = {};
            sv.Texture2D.MipLevels = it->second.mips;
            stages.hd[stage] = true;
            return it->second.res.Get();
        }
    }
    if (tx.id && remaster::Active()) {
        if (const remaster::Material* m = remaster::Find(tx.id)) {
            auto it = remaster_.find(tx.id);
            if (it == remaster_.end()) {
                remaster::Request(tx.id);
            } else if (it->second.color) {
                RemasterTex& rt = it->second;
                D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
                sv.Format = rt.color_fmt;
                sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Texture2D = {};
                sv.Texture2D.MipLevels = rt.color_mips;
                if (m->pbr && rt.normal && rt.mat && stages.mat_stage < 0) {
                    stages.mat = &rt;
                    stages.mat_stage = stage;
                    info.flags |= kTexMaterial;
                }
                stages.hd[stage] = true;
                return rt.color.Get();
            }
        }
    }
    return tx.res.Get();
}

ID3D12Resource* D3D12Backend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    info = {};
    sv = {};
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.Texture2D.MipLevels = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;

    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    uint64_t alt = 0;
    bool box = true;
    if (ext_) ext_->Sample(d.data, fi.depth, stage, alt, stages.k[stage], box);
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height) | alt);
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, box, stages);
    }
    return BindGuestTexture(s, stage, d, fi, stages);
}

// D3DTEXF: 1 point, 2 linear, 3 anisotropic
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
// D3DTADDRESS values the samplers test
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

D3D12_SAMPLER_DESC StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso) {
    D3D12_SAMPLER_DESC sd = {};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    D3D12_FILTER_TYPE fmin = lin(min) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmag = lin(mag) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmip = mip == kTexfLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_FILTER(fmin << 4 | fmag << 2 | fmip);
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    if (aniso && lin(min) && mip && stages.sv[i].Texture2D.MipLevels > 1 && !stages.info[i].shadow) an = aniso;
    if (an > 1) {
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::min<uint32_t>(an, 16);
    }
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
    sd.AddressU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    sd.AddressV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    sd.AddressW = Address(t[kTssAddressW]);
    sd.MipLODBias = Bits(t[kTssMipLodBias]);
    sd.MinLOD = float(t[kTssMaxMipLevel]);
    sd.MaxLOD = mip ? D3D12_FLOAT32_MAX : sd.MinLOD;
    // a replacement at a higher resolution is filtered smoothly through all its mips whatever the title asked for
    // its small original (point filtered HUD pieces and text): lines and letters stay smooth as they scale and turn;
    // in 2D a little sharper
    if (stages.hd[i] && !stages.info[i].shadow) {
        sd.Filter = an > 1 ? D3D12_FILTER_ANISOTROPIC : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        if (stages.flat) sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR, sd.MaxAnisotropy = 0;
        sd.MinLOD = 0;
        sd.MaxLOD = D3D12_FLOAT32_MAX;
        // the world's, far: a little sharper (Sharper Distance), the anisotropic filter keeps it from shimmering
        sd.MipLODBias = stages.flat ? -0.5f : app::Get().sharp_distance && an > 1 ? sd.MipLODBias - 0.4f : sd.MipLODBias;
    }
    Color(t[kTssBorderColor], sd.BorderColor);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, D3D12 tests r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        sd.Filter = lin(min) || lin(mag) ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                                         : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = D3D12_COMPARISON_FUNC(swap[Cmp(s.rs[kRsShadowFunc]) - 1] + 1);
        sd.MaxAnisotropy = 0;
    }
    return sd;
}

uint32_t D3D12Backend::EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp) {
    D3D12_SAMPLER_DESC sd[4];
    uint32_t aniso = app::Get().aniso;
    for (int i = 0; i < 4; i++) sd[i] = StageSampler(s, i, stages, clamp, aniso);
    // (most draws take the samplers of the draw before)
    if (last_smp_ok_ && !std::memcmp(sd, last_sd_, sizeof(sd))) return last_smp_base_;
    uint64_t key = Hash(sd, sizeof(sd));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) {
        std::memcpy(last_sd_, sd, sizeof(sd)), last_smp_base_ = it->second, last_smp_ok_ = true;
        return it->second;
    }
    if (smp_used_ + 4 > kSamplerCount) {
        samplers_.clear();
        last_smp_ok_ = false;
        smp_used_ = 0;
    }
    uint32_t base = smp_used_;
    smp_used_ += 4;
    for (int i = 0; i < 4; i++) dev_->CreateSampler(&sd[i], Cpu(smp_heap_.Get(), base + i, smp_inc_));
    samplers_[key] = base;
    std::memcpy(last_sd_, sd, sizeof(sd)), last_smp_base_ = base, last_smp_ok_ = true;
    return base;
}

uint32_t SimpleState(const State& s, uint32_t method) {
    return s.simple[method >> 2];
}

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    // a shader made from a declaration alone (no program) is the fixed function pipeline on those inputs
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    // up to a quad with no depth test: it may be a pass over the whole target, see QuadHlsl
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable];
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

// vertex shader cache key salts
constexpr uint64_t kDeclarationSalt = 0x10000;    // fixed function seed, above the 16 bit register mask
constexpr uint64_t kPretransformedSalt = 0x5A5A;  // screen space fixed function vertices, apart from untransformed
constexpr uint64_t kQuadSalt = 0xC3C30000;        // the QuadHlsl variant, apart from the plain shader

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgramHlsl(s.vs->program, a.regs, a.decode);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

// sets sh.vh and sh.vs_code, compiling the shader on a cache miss
void D3D12Backend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) {
        sh.vs_code = blobs_[vit->second].Get();
        sh.vh = vit->second;
        return;
    }
    std::string src = VertexHlsl(s, a, sh);
    sh.vs_code = Compile(sh.quad ? QuadHlsl(std::move(src)) : src, "main", "vs_5_0", sh.vh);
    shader_src_[sh.vkey] = sh.vh;
    if (sh.vs_code) RememberVertexRecipe(s, a, sh, kVariantMain, sh.vh);
}

bool D3D12Backend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    PerfScope perf_bind{g_perf.bind, g_draw_timers};
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    uint64_t version = targets_version_;
    for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    // textures may have recorded copies that unbound the targets; when nothing moved (the same targets bound, as they
    // were, none made or dropped) the bind just done stands
    if (version == targets_version_ && bound_rt_ == draw_rt_ && bound_ds_ == draw_ds_ && (bound_rt_ || bound_ds_) &&
        (msaa_bound_ || ((!bound_rt_ || bound_rt_->state == D3D12_RESOURCE_STATE_RENDER_TARGET) &&
                         (!bound_ds_ || bound_ds_->state == D3D12_RESOURCE_STATE_DEPTH_WRITE))))
        return true;
    return BindTargets(s);
}

// sets sh.ph and sh.ps_code, compiling the shader on a cache miss
void D3D12Backend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) {
        sh.ps_code = blobs_[pit->second].Get();
        sh.ph = pit->second;
        return;
    }
    sh.ps_code = Compile(PixelHlsl(pk), "main", "ps_5_0", sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
    if (sh.ps_code) RememberPixelRecipe(pk, sh.pkey, sh.ph);
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to D3D12 RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

constexpr float kDepth24Max = 16777215.0f;  // 2^24 minus 1: a 24 bit depth buffer's units per 1.0

void D3D12Backend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    // polygon offset: bias in depth buffer units, the target is always 24 bit; the slope is per console pixel,
    // and an upscaled target's pixels are smaller
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * (bound_ds_ ? float(bound_ds_->w) / bound_ds_->gw : 1.0f);
    }
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

void D3D12Backend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? bound_rt_->fmt : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

// alpha test off: compare always, in Cmp() minus 1 terms like the enabled test
constexpr float kAlphaTestAlways = float(D3D12_COMPARISON_FUNC_ALWAYS - 1);

// D3DRS_FOGTABLEMODE values
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;

// table mode none under a vertex program: oFog is the blend factor, passed on as linear fog from 1 to 0
void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(Cmp(SimpleState(s, kMAlphaFunc)) - 1) : kAlphaTestAlways;
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]) - 1);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
    if (c.layout[0] == 0) c.layout[0] = 1;  // Layout2D's, else as drawn
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;  // console texels across the box each wide tap averages

void D3D12Backend::FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    // the console samples pixels at integer positions, D3D12 half a pixel on: targets are read half a texel back
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    c.texsize[i][2] = -0.5f / (float(d.width) * k);
    c.texsize[i][3] = -0.5f / (float(d.height) * k);
    // a 3 console texel box per tap keeps the console chain's glow width and brightness
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width) * k);
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void D3D12Backend::FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    // lights to view space
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

void D3D12Backend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                   GuestExtent size, DrawConstants& c) {
    Alloc vc = Upload(sizeof(s.vsc) + kShadowConstantsTail, 256);
    std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
    last_vc_ = vc.gpu, last_tail_ = vc.cpu + sizeof(s.vsc);
    Alloc dc = Upload(sizeof(DrawConstants), 256);
    FillSurfaceConstants(s, size, c);
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    if (ext_) ext_->Constants(reinterpret_cast<float(*)[4]>(vc.cpu), c.ext, bound_rt_, k.blend ? k.src : 1);
    FillTextureConstants(s, stages, c);
    if (stages.mat) {
        const MaterialFrame& mf = FrameMaterial();
        const app::Settings& set = app::Get();
        const remaster::Material& m = *stages.mat->m;
        float c0[4] = {mf.view_x, mf.view_y, set.normal_green, set.materials ? 1.0f : 0.0f};
        float c1[4] = {mf.light[0], mf.light[1], mf.light[2], set.specular / 100.0f};
        // the light in the draws' own view space (the camera the frame is drawn with): the game's camera frame's axes
        // (RenderWare's right handed: its right the view's left) turned it with the camera, a wall's relief and sheen
        // brighter or darker as the view turned
        if (cam_ok_) {
            float v[3];
            for (int j = 0; j < 3; j++) v[j] = mf.sun[0] * cam_view_[j] + mf.sun[1] * cam_view_[4 + j] + mf.sun[2] * cam_view_[8 + j];
            float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (n > 1e-4f) c1[0] = v[0] / n, c1[1] = v[1] / n, c1[2] = v[2] / n;
        }
        float c2[4] = {m.normal, set.parallax ? m.pom : 0.0f, m.spec, float(m.detail)};
        // the snowy ground (snow.txt): the generated relief read the snow's faint marks as deep cuts, dark scratches
        // all over it; snow lies soft and white: a gentle relief, little depth, no ground detail map
        // (its generated cavity map, the cracks it read in the snow, darkened them to black scratches: faint too,
        // with the relief's turn of the light, mtl[0].w)
        if (m.snow) c2[0] = m.normal * 0.3f, c2[1] *= 0.3f, c2[3] = -1, c0[3] *= 0.25f;
        // characters (lit per pixel by their program): the remastered colour alone. The generated relief of faces, hair
        // and cloth reads their painted shading as shape and doubles it, and they need no highlights
        if (draw_character_) c0[3] = 0, c1[3] = 0, c2[1] = 0;
        // the sewers' water (level 4, under the streets): no sun's highlight. The sun is above the street; its shadow
        // misses the thin water along the walls' foot, which lit up as a flat grey band (its ripples and reflection
        // stay)
        constexpr uint64_t kSewerWater = 0x24d88cb7fa7d9e46ull;  // water_sewer
        for (int st = 0; st < 4; st++)
            if (stages.id[st] == kSewerWater) c1[3] = 0;
        // the weapon in hand (drawn after the world's depth is cleared), its wood and metal (not the hands' skin): a
        // soft relief and a metal sheen where its texture says metal (Weapon Materials)
        if (draw_character_ && occlusion_done_ && set.weapon_materials && m.pbr && !m.skin) {
            // gentle: its relief the generated map's broad shapes only (the fine grain sparkled, the weapon close and
            // large on the screen), its sheen soft
            c0[3] = set.materials ? 0.3f : 0.0f;
            c1[3] = set.specular / 100.0f * 0.8f;
            c2[0] = m.normal * 0.25f;
        }
        // the highlight is the sun's: kept out of its shadow (the last frame's mask at t7, asked for by a negative
        // strength) on the screen's world, where the mask's pixels are the draw's
        if (c1[3] > 0 && !draw_character_ && sun_vis_bound_ && bound_rt_ && bound_rt_ == scene_rt_ && bound_rt_->w == scene_w_ &&
            bound_rt_->h == scene_h_)
            c1[3] = -c1[3];
        std::memcpy(c.mtl[0], c0, 16);
        std::memcpy(c.mtl[1], c1, 16);
        std::memcpy(c.mtl[2], c2, 16);
        static const float debug = [] {
            const char* e = std::getenv("XBE_MATERIAL_DEBUG");
            return e ? float(std::atoi(e)) : 0.0f;
        }();
        c.mtl[3][0] = debug;
        c.mtl[3][1] = set.detail && details_ && m.detail < int(details_count_) && !draw_character_ ? 0.45f : 0.0f;
        // a character close up (Character Detail): its skin's pores or its uniform's weave, fine (the detail maps' skin,
        // 6, or fabric's, 2), as a negative strength
        if (draw_character_ && set.character_detail && details_ && !(occlusion_done_ && !m.skin)) {
            // (a uniform the classifier took for skin: its weave, by its grain)
            int d = m.grain == 7 + 5 ? 2 : m.skin || m.grain == 7 + 11 ? 6 : m.detail == 2 || m.cloth ? 2 : -1;
            if (d >= 0 && d < int(details_count_)) c.mtl[2][3] = float(d), c.mtl[3][1] = d == 6 ? (occlusion_done_ ? -0.9f : -1.7f) : -0.8f;  // (the hands in view softer: close up, strong pores read as dirt)
        }
        c.mtl[3][2] = mf.units;
        // Detail Textures: its kind's grain, everything (the world, the soldiers, the weapon in hand)
        c.dtl[0] = -1;
        // the sizes: a character's pores and weave large (tiled 3 times: low resolution textures), the hands and the
        // weapon in hand, close and large on the screen, finer (10); the world's grain a tile per 160 texels, the
        // characters' and the weapon's larger (400)
        c.dtl[2] = draw_character_ && occlusion_done_ ? 10.0f : (m.skin || m.grain == 7 + 11) && m.grain != 7 + 5 ? 4.0f : 3.0f;  // (pores a little finer than weave)
        c.dtl[3] = draw_character_ ? 400.0f : 160.0f;
        // (not on skin: its own pores, Character Detail; clothes and the weapon in hand stronger, their textures
        // spread over large parts)
        if (set.detail && details_ && m.grain >= 0 && m.grain < int(details_count_) && m.grain != 7 + 11 && !g_texview.load()) {
            c.dtl[0] = float(m.grain);
            c.dtl[1] = draw_character_ ? 1.0f : m.snow ? 0.3f : 0.65f;  // (the shader takes over 0.9 for a character's: its skin left bare; snow's soft)
        }
        // source alpha blending (D3DBLEND_SRCALPHA): the height mask (cut-outs have no height: pom 0)
        constexpr uint32_t kSrcAlpha = 0x302;
        bool layered = k.blend && k.src == kSrcAlpha;
        c.mtl[3][3] = set.height_blend && layered && m.pbr && m.pom > 0 ? 1.0f : 0.0f;
        c.mrefl[0] = m.refl;
        c.mrefl[1] = m.metal;
        // water (Water): smooth and reflective, its ripples moving (mrefl.z, negative, the time)
        // the snowy ground (Snow Tracks): where the trail lies in the world
        static const bool snow_log = std::getenv("XBE_SNOW_LOG") != nullptr;
        if (snow_log && m.snow && presents_ % 240 == 3)
            LOGR("snow: ground draw tex %016llx ok %d bound %d cam %d character %d", (unsigned long long)draw_tex0_, int(trail_ok_),
                 int(trail_bound_), int(cam_ok_), int(draw_character_));
        if (m.snow && set.snow_tracks && trail_ok_ && trail_bound_ && cam_ok_ && !draw_character_) {
            if (wave_inv_frame_ != frame_ && shadow::Invert(cam_view_, wave_inv_)) wave_inv_frame_ = frame_;
            if (wave_inv_frame_ == frame_) {
                const float* I = wave_inv_;
                float w0[4] = {I[0], I[4], I[8], I[12]}, w1[4] = {I[2], I[6], I[10], I[14]}, wy[4] = {I[1], I[5], I[9], I[13]};
                std::memcpy(c.wave[0], w0, 16), std::memcpy(c.wave[1], w1, 16), std::memcpy(c.trail[1], wy, 16);
                c.trail[0][0] = trail_x0_, c.trail[0][1] = trail_z0_, c.trail[0][2] = 1.0f / (kTrailSize * kTrailCell);
                static const bool snow_debug = std::getenv("XBE_SNOW_DEBUG") != nullptr;
                c.trail[0][3] = snow_debug ? -1.8f : 1.8f;
            }
        }
        if (m.water && set.water) {
            static const auto t0 = std::chrono::steady_clock::now();
            float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
            c.mrefl[0] = 0.8f;
            c.mrefl[2] = -(0.1f + std::fmod(t, 600.0f));
            // the world's up in view space (trail[1]: the shader's flat water)
            if (cam_ok_) {
                if (wave_inv_frame_ != frame_ && shadow::Invert(cam_view_, wave_inv_)) wave_inv_frame_ = frame_;
                if (wave_inv_frame_ == frame_) c.trail[1][0] = wave_inv_[1], c.trail[1][1] = wave_inv_[5], c.trail[1][2] = wave_inv_[9], c.trail[1][3] = wave_inv_[13];
            }
            c.mtl[2][0] = std::max(c.mtl[2][0], 0.6f);
            // what crosses it stirs it (Water Interaction): where the wave field lies in the world
            static const bool wlog = std::getenv("XBE_WAVE_DEBUG") != nullptr;
            if (wlog && presents_ % 240 == 7) LOGR("waves: water draw ok %d bound %d cam %d", int(wave_ok_), int(wave_bound_), int(cam_ok_));
            if (set.water_interaction && wave_ok_ && wave_bound_ && cam_ok_) {
                if (wave_inv_frame_ != frame_ && shadow::Invert(cam_view_, wave_inv_)) wave_inv_frame_ = frame_;
                if (wave_inv_frame_ == frame_) {
                    const float* I = wave_inv_;
                    float w0[4] = {I[0], I[4], I[8], I[12]}, w1[4] = {I[2], I[6], I[10], I[14]};
                    std::memcpy(c.wave[0], w0, 16), std::memcpy(c.wave[1], w1, 16);
                    c.wave[2][0] = wave_x0_, c.wave[2][1] = wave_z0_, c.wave[2][2] = 1.0f / (kWaveSize * kWaveCell);
                    static const bool wave_debug = std::getenv("XBE_WAVE_DEBUG") != nullptr;
                    c.wave[2][3] = (wave_debug ? -3.0f : 3.0f) / (2 * kWaveCell);
                }
            }
        }
    }
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    std::memcpy(dc.cpu, &c, sizeof(c));
    last_dc_ = dc.gpu;
    cl_->SetGraphicsRootConstantBufferView(0, vc.gpu);
    cl_->SetGraphicsRootConstantBufferView(1, dc.gpu);
}

void D3D12Backend::CaptureTextures(const State& s, const DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        ID3D12Resource* res = stages.res[i];
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        bool tg = targets_.count(SizedTargetKey(d.data, Format(d.format).depth, d.width, d.height));
        fprintf(cap_, " | t%d %08X fmt %X %ux%u%s%s", i, d.data, d.format, d.width, d.height, tg ? " target" : "",
                res ? "" : " unbound");
        if (res && !tg && stages.info[i].kind == kTex2D && cap_seen_.insert(d.data).second) {
            cap_tex_.push_back({res, stages.sv[i].Format, d.data, d.width, d.height});
        }
    }
}

// s.vsc holds the 192 vertex shader constants in hardware order: D3D register + 96
constexpr int kVsConstants = 192;
constexpr int kVsConstantBias = 96;

void D3D12Backend::CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c) {
    fprintf(cap_, "\n     verts");
    uint32_t n = 0;
    for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
    for (size_t v = 0; v < std::min<size_t>(4, n ? a.vertices.size() / (n * 4) : 0); v++) {
        fprintf(cap_, " [");
        for (uint32_t k = 0; k < n * 4; k++) fprintf(cap_, " %g", a.vertices[v * n * 4 + k]);
        fprintf(cap_, " ]");
    }
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (v[0] || v[1] || v[2] || v[3]) {
            fprintf(cap_, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
        }
    }
    fprintf(cap_, "\n     pc");
    for (size_t r = 0; r < std::size(c.pc); r++) {
        fprintf(cap_, " (%.3f %.3f %.3f %.3f)", c.pc[r][0], c.pc[r][1], c.pc[r][2], c.pc[r][3]);
    }
}

// the draw's line: its state, then its textures
void D3D12Backend::CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                               const DrawTextures& stages) {
    fprintf(cap_,
            "%04u draw rt %08X ds %08X prim %u count %u vs %08X ps %08X blend %u %X %X zen %u %X zwrite %u "
            "zbias %d %.2f cull %u cw %X c0 %.3f %.3f %.3f %.3f",
            cap_i_++, s.rt.data, bound_ds_ ? s.zs.data : 0, prim, count, s.vs_handle, s.ps ? s.ps->handle : 0, k.blend,
            k.src, k.dst, k.zen, k.zfunc, k.zwrite, k.zbias, k.zslope, k.cull, k.wmask, c.pc[0][0], c.pc[0][1],
            c.pc[0][2], c.pc[0][3]);
    CaptureTextures(s, stages);
}

// small draws: their inputs and constants; then ends the draw's line
void D3D12Backend::EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c) {
    CaptureInputs(s, a, c);  // (every draw: its constants; the converted path alone has vertices to show)
    fputc('\n', cap_);
}

// each shader once, as translated
void D3D12Backend::CaptureShader(const char* kind, uint64_t key, const std::string& src) {
    if (!cap_sh_.insert(key).second) return;
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (FILE* f = _wfopen((cap_dir_ / name).c_str(), L"w")) {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
    fprintf(cap_, "     %s\n", name);
}

void D3D12Backend::CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    std::string src = VertexHlsl(s, a, sh);
    CaptureShader("vs", sh.vkey, sh.quad ? QuadHlsl(std::move(src)) : src);
    CaptureShader("ps", sh.pkey, PixelHlsl(pk));
}

D3D12_SRV_DIMENSION NullViewDimension(uint8_t kind) {
    if (kind == kTexCube) return D3D12_SRV_DIMENSION_TEXTURECUBE;
    if (kind == kTex3D) return D3D12_SRV_DIMENSION_TEXTURE3D;
    return D3D12_SRV_DIMENSION_TEXTURE2D;
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes. The console's
// pixel centres sample such a quad from its first texel to half a texel short of its far edge; upscaled, the far
// edge's last half texel is drawn too, and wrapped it blends in the opposite edge (thin dark lines on HUD sprites).
// Clamped, it takes the edge texel instead, which a wrapped texture used once over 0..1 has there anyway.
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;  // most vertices a small draw has
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

// a stage without a texture gets a null view of its kind; the samplers read the views after
void D3D12Backend::SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages) {
    struct View {
        ID3D12Resource* r;
        D3D12_SHADER_RESOURCE_VIEW_DESC d;
    } views[7];
    std::memset(views, 0, sizeof(views));
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        bool has = stages.mat && details_;
        sv.Format = has ? details_fmt_ : DXGI_FORMAT_B8G8R8A8_UNORM;
        sv.Texture2DArray.MipLevels = has ? details_mips_ : 1;
        sv.Texture2DArray.ArraySize = has ? details_count_ : 1;
        if (soft_draw_ && world_depth_) {
            // a soft particle's: the scene's depth before it
            sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D = {0, 1, 0, 0};
            views[6] = {world_depth_.Get(), sv};
        } else {
            views[6] = {has ? details_.Get() : nullptr, sv};
        }
    }
    for (int i = 0; i < 2; i++) {
        const RemasterTex* m = stages.mat;
        ID3D12Resource* r = m ? (i ? m->mat.Get() : m->normal.Get()) : nullptr;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Format = r ? (i ? m->mat_fmt : m->normal_fmt) : DXGI_FORMAT_B8G8R8A8_UNORM;
        sv.Texture2D.MipLevels = r ? (i ? m->mat_mips : m->normal_mips) : 1;
        views[4 + i] = {r, sv};
    }
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[i];
        if (!stages.res[i]) {
            sv.ViewDimension = NullViewDimension(stages.info[i].kind);
            sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        views[i] = {stages.res[i], sv};
    }
    // the table: made already this frame, or made now
    uint64_t key = Hash(views, sizeof(views)) | 1;
    auto sync = [&] {
        uint64_t epoch = res_epoch_.load(std::memory_order_relaxed);
        if (srv_ring_gen_ != ring_gen_ || srv_epoch_ != epoch) srv_gen_++, srv_ring_gen_ = ring_gen_, srv_epoch_ = epoch, srv_last_key_ = 0;
    };
    sync();
    uint32_t slot = 0;
    bool found = false;
    const size_t mask = srv_tables_.size() - 1;
    if (key == srv_last_key_) {
        slot = srv_last_slot_, found = true;
    } else {
        for (size_t i = key & mask, n = 0; n < 8; i = (i + 1) & mask, n++) {
            const SrvTable& t = srv_tables_[i];
            if (t.gen != srv_gen_) break;
            if (t.key == key) {
                slot = t.slot, found = true;
                break;
            }
        }
    }
    g_perf.srv_hits += found;
    if (!found) {
        slot = AllocSrvSlots(7);
        sync();  // (a wrapped ring is another generation)
        for (uint32_t i = 0; i < 7; i++) dev_->CreateShaderResourceView(views[i].r, &views[i].d, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
        size_t at = key & mask;
        for (size_t n = 0; n < 8; n++) {  // a free place near the key's, else its own
            size_t i = (size_t(key) + n) & mask;
            if (srv_tables_[i].gen != srv_gen_) {
                at = i;
                break;
            }
        }
        srv_tables_[at] = {key, srv_gen_, slot};
    }
    srv_last_key_ = key, srv_last_slot_ = slot;
    last_srv_ = Gpu(srv_heap_.Get(), slot, srv_inc_);
    cl_->SetGraphicsRootDescriptorTable(2, last_srv_);
    uint32_t samplers = EnsureStageSamplers(s, stages, UnitTexcoords(a));
    last_smp_ = Gpu(smp_heap_.Get(), samplers, smp_inc_);
    SetSamplerTable(last_smp_);
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void D3D12Backend::SetViewportScissor(const State& s, uint32_t gw) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    D3D12_VIEWPORT vp{0, 0, W, H, 0, 1};
    SetViewport(vp);
    const Target* bt = bound_rt_ ? bound_rt_ : bound_ds_;
    float sx = W / gw, sy = bt->gh ? H / bt->gh : sx;
    // a 2D draw moved by its layout is clipped where it went
    LONG x0 = LONG((s.viewport.x * layout_[0] + layout_[1]) * sx);
    LONG y0 = LONG(s.viewport.y * sy);
    LONG x1 = LONG(((s.viewport.x + s.viewport.w) * layout_[0] + layout_[1]) * sx);
    LONG y1 = LONG((s.viewport.y + s.viewport.h) * sy);
    if (s.scissor_count) {
        float lx0 = s.scissors[0] * layout_[0] + layout_[1], lx1 = s.scissors[2] * layout_[0] + layout_[1];
        x0 = std::max(x0, LONG(lx0 * sx));
        y0 = std::max(y0, LONG(s.scissors[1] * sy));
        x1 = std::min(x1, LONG(lx1 * sx));
        y1 = std::min(y1, LONG(s.scissors[3] * sy));
    }
    D3D12_RECT rect{std::clamp<LONG>(x0, 0, LONG(W)), std::clamp<LONG>(y0, 0, LONG(H)),
                    std::clamp<LONG>(x1, 0, LONG(W)), std::clamp<LONG>(y1, 0, LONG(H))};
    SetScissor(rect);
}

void D3D12Backend::SetBlendFactorStencilRef(const State& s) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    SetBlendFactor(bf);
    SetStencilRef(SimpleState(s, kMStencilRef) & 0xFF);
}

D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    if (a.topology == Topology::Lines) {
        return a.strip ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    }
    return a.strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

void D3D12Backend::UploadVertexBuffers(const Assembled& a) {
    D3D12_VERTEX_BUFFER_VIEW vbv[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(b.bytes, 16);
        std::memcpy(vb.cpu, b.src, b.bytes);
        vbv[i] = {vb.gpu, b.bytes, b.stride};
        last_vbv_[i] = vbv[i];
        // g_recolor_2d: the copy's colours, alpha kept
        if (recolor_ && recolor_elem_.buffer == i && b.stride)
            for (uint32_t at = recolor_elem_.offset; at + 4 <= b.bytes; at += b.stride) {
                uint32_t v;
                std::memcpy(&v, vb.cpu + at, 4);
                v = (v & 0xFF000000u) | (recolor_argb_ & 0xFFFFFFu);
                std::memcpy(vb.cpu + at, &v, 4);
            }
    }
    cl_->IASetVertexBuffers(0, a.buffer_count, vbv);
}

void D3D12Backend::DrawGeometry(const Assembled& a) {
    UploadVertexBuffers(a);
    // the weapon in hand's pieces left out (WeaponPieces): its kept triangles as a list (a strip's winding kept)
    if (weapon_filter_ && !tess_draw_) {
        weapon_filter_ = false;
        uint32_t n = a.index_count ? a.index_count : a.draw_count;
        auto index = [&](uint32_t i) -> uint32_t {
            if (!a.index_count) return i;
            return a.index16 ? static_cast<const uint16_t*>(a.index_src)[i] : static_cast<const uint32_t*>(a.index_src)[i];
        };
        tess_list_.clear();
        size_t t = 0;
        if (a.strip) {
            for (uint32_t i = 2; i < n; i++, t++) {
                uint32_t i0 = index(i - 2), i1 = index(i - 1), i2 = index(i);
                if (i0 == i1 || i1 == i2 || i0 == i2 || (t < weapon_keep_.size() && !weapon_keep_[t])) continue;
                if (i & 1) std::swap(i0, i1);
                tess_list_.insert(tess_list_.end(), {i0, i1, i2});
            }
        } else {
            for (uint32_t i = 0; i + 2 < n; i += 3, t++)
                if (t >= weapon_keep_.size() || weapon_keep_[t]) tess_list_.insert(tess_list_.end(), {index(i), index(i + 1), index(i + 2)});
        }
        if (tess_list_.empty()) return;
        SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        uint32_t ibytes = uint32_t(tess_list_.size() * 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, tess_list_.data(), ibytes);
        D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, ibytes, DXGI_FORMAT_R32_UINT};
        last_ibv_ = ibv;
        cl_->IASetIndexBuffer(&ibv);
        cl_->DrawIndexedInstanced(uint32_t(tess_list_.size()), 1, 0, a.index_count ? a.base_vertex : 0, 0);
        return;
    }
    if (tess_draw_) {
        // patches of three: a list's triangles as they are, a strip's taken apart (its alternate winding kept,
        // its joining triangles of a repeated corner left out)
        SetTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        uint32_t n = a.index_count ? a.index_count : a.draw_count;
        auto index = [&](uint32_t i) -> uint32_t {
            if (!a.index_count) return i;
            return a.index16 ? static_cast<const uint16_t*>(a.index_src)[i] : static_cast<const uint32_t*>(a.index_src)[i];
        };
        tess_list_.clear();
        const bool keep = weapon_filter_;  // (the weapon in hand's pieces left out: WeaponPieces)
        weapon_filter_ = false;
        size_t t = 0;
        if (a.strip) {
            for (uint32_t i = 2; i < n; i++, t++) {
                uint32_t i0 = index(i - 2), i1 = index(i - 1), i2 = index(i);
                if (i0 == i1 || i1 == i2 || i0 == i2 || (keep && t < weapon_keep_.size() && !weapon_keep_[t])) continue;
                if (i & 1) std::swap(i0, i1);
                tess_list_.insert(tess_list_.end(), {i0, i1, i2});
            }
        } else {
            for (uint32_t i = 0; i + 2 < n; i += 3, t++)
                if (!keep || t >= weapon_keep_.size() || weapon_keep_[t]) tess_list_.insert(tess_list_.end(), {index(i), index(i + 1), index(i + 2)});
        }
        if (tess_list_.empty()) return;
        uint32_t ibytes = uint32_t(tess_list_.size() * 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, tess_list_.data(), ibytes);
        D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, ibytes, DXGI_FORMAT_R32_UINT};
        last_ibv_ = ibv;
        cl_->IASetIndexBuffer(&ibv);
        cl_->DrawIndexedInstanced(uint32_t(tess_list_.size()), 1, 0, a.index_count ? a.base_vertex : 0, 0);
        return;
    }
    SetTopology(PrimitiveTopology(a));
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, ibytes, a.index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        last_ibv_ = ibv;
        cl_->IASetIndexBuffer(&ibv);
        cl_->DrawIndexedInstanced(a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        cl_->DrawInstanced(a.draw_count, 1, 0, 0);
    }
}

// calls the extension's EndDraw on every return from Draw once made
struct DrawEnd {
    Extension* e;
    ~DrawEnd() {
        if (e) e->EndDraw();
    }
};

// Assemble, timed and counted
bool D3D12Backend::AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count,
                                const uint8_t* indices, Assembled& a) {
    PerfClock::time_point perf_t = g_draw_timers ? PerfClock::now() : PerfClock::time_point{};
    bool assembled = Assemble(s, prim, start, count, indices, a);
    if (g_draw_timers) g_perf.assemble += PerfMs(perf_t);
    g_perf.verts += a.vertex_count;
    g_perf.raw += a.vertices.empty();
    return assembled;
}

void D3D12Backend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    PerfScope perf_draw{g_perf.draw, g_draw_timers};
    g_perf.draws++;
    if (cap_) CapStep(s.rt);
    if (DepthCopy(s)) return;
    DrawEnd draw_end{ext_};
    if (ext_) ext_->BeginDraw(s);
    Assembled& a = asm_;
    if (!AssembleDraw(s, prim, start, count, indices, a)) return;
    // past the ring every upload is a buffer of its own: seconds lost on a frame that draws a whole level once
    if (CurrentFrame().used > kUploadSize - kUploadReserve) Submit(true);
    // the first 2D after the 3D: the scene's passes run on what is drawn so far
    bool flat = s.layout2d || a.pretransformed;
    if (flat && scene_rt_ && !scene_done_) ScenePost();
    if (g_hide_hud.load(std::memory_order_relaxed) && (s.layout2d & 0xFF) == 1) return;
    if (!BindTargets(s)) return;
    if (ext_ && ext_->Bound(*this, s, bound_rt_, bound_ds_, a.pretransformed) && !BindTargets(s)) return;
    // some levels never clear the screen's colour, only its depth: their sky, drawn see-through, showed what the last
    // frame left under it (a ghost of the world while the view turned; hidden when our clouds cover the sky). The
    // frame's first 3D draw on the screen-sized target clears it first (black)
    if (colour_frame_ != frame_ && !flat && bound_rt_ && !bound_rt_->depth && bound_rt_->gw == guest_w_ &&
        bound_rt_->gh == guest_h_ && !msaa_bound_) {
        colour_frame_ = frame_;
        Transition(bound_rt_->res.Get(), bound_rt_->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float black[4] = {0, 0, 0, 1};
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), bound_rt_->view, rtv_inc_), black, 0, nullptr);
    }
    if (!flat && bound_rt_ && bound_ds_ && bound_rt_->gw == guest_w_ && bound_rt_->gh == guest_h_ && !occlusion_done_) {
        scene_rt_ = bound_rt_;
        scene_ds_ = bound_ds_;
    }

    // vertex shader
    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    // stage textures, then the pixel shader made for them
    DrawTextures stages;
    if (!BindStageTexturesAndTargets(s, stages)) return;
    // 2D keeps the remastered colour alone: menus and the HUD have no surfaces to light
    stages.flat = s.layout2d || a.pretransformed;
    stages.weapon = !stages.flat && occlusion_done_;
    // nor do the effects' sprites (smoke, sparks, the dirt bullets throw up: the game's table, g_emitter), whatever
    // their texture was taken for: lit as a wall they turn into dark, hard-edged blots
    // (looked up once a draw: draw_emitter_, draw_emitter_kind_, for the uses below and RecordCaster)
    {
        auto sprite_of = g_emitter.load();
        draw_emitter_kind_ = EmitterKind{};
        draw_emitter_ = stages.id[0] && sprite_of && sprite_of(stages.id[0], draw_emitter_kind_);
    }
    const bool sprite = draw_emitter_;
    if (stages.mat && (s.layout2d || a.pretransformed || sprite)) {
        stages.info[stages.mat_stage].flags &= uint8_t(~kTexMaterial);
        stages.mat = nullptr;
        stages.mat_stage = -1;
    }
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    draw_tex0_ = stages.id[0];
    // the level's own moon (a sprite of the night skies of levels 14 and 15) or sun: Sun and Moon draws none beside it
    {
        constexpr uint64_t kMoonSprite = 0x33469bfa695c087dull;  // 2_1moon1_png
        // and a sky box with its sun painted away from the level's light (level 17's)
        constexpr uint64_t kPaintedSun = 0x5a9f421798167e0aull;  // TsarTsarsine_UP
        if (draw_tex0_ == kMoonSprite || draw_tex0_ == kPaintedSun) {
            game_moon_frame_ = frame_;
            // the game draws its own only while it is in view: ours would show (elsewhere, at the level's light)
            // whenever it is out of it
            if (game_moon_level_ != g_level.load()) game_moon_level_ = g_level.load(), LOGI("sky: level %d has its own moon or sun", game_moon_level_);
        }
    }
    // (any stage's texture: a material can be on a stage after the first)
    if (g_hidden_tex[0].load())
        for (const auto& hidden : g_hidden_tex)
            for (int st = 0; st < 4; st++)
                if (stages.id[st] && hidden.load() == stages.id[st]) return;
    for (const auto& hidden : g_game_hidden_tex) {
        uint64_t id = hidden.load(std::memory_order_relaxed);
        if (!id) break;
        if (id == stages.id[0]) return;
    }
    // tests (XBE_HIDE_BLENDED): the blended draws alone of a hidden texture's id
    {
        static const uint64_t blended = [] {
            const char* e = std::getenv("XBE_HIDE_BLENDED");
            return e ? std::strtoull(e, nullptr, 16) : 0ull;
        }();
        if (blended && draw_tex0_ == blended && SimpleState(s, kMBlendEnable)) return;
    }
    // a glowing or smoking sprite of the game's table (g_emitter): its light, glow and soft edges
    const EmitterKind& glow = draw_emitter_kind_;
    const bool emitter = !flat && draw_emitter_ && app::Get().dynamic_lights;
    // soft where it meets the world: a blended sprite over the world's depth, not writing it
    static const bool no_soft = std::getenv("XBE_NO_SOFT") != nullptr;  // tests: side by side
    soft_draw_ = !no_soft && app::Get().soft_particles && emitter && glow.soft > 0 && !stages.mat && !occlusion_done_ && bound_ds_ && bound_ds_ == scene_ds_ &&
                 SimpleState(s, kMBlendEnable) && SimpleState(s, kMSrcBlend) == 0x302 && !SimpleState(s, kMZWriteEnable) &&
                 (soft_saved_ || (soft_saved_ = CopyDepth(*bound_ds_)));
    pk.soft = soft_draw_ ? 1 : 0;
    if (app::Get().pixel_lighting && !s.layout2d && !a.pretransformed) {
        // (the vertex shader's key holds what the id is made from: the program or fixed function key and the
        // registers; looked up once a shader)
        auto it = lit_ids_.find(sh.vkey);
        if (it == lit_ids_.end())
            it = lit_ids_.emplace(sh.vkey, sh.prog ? PixelLightingId(s.vs->program, a.regs) : FixedLightingId(sh.fk, a.regs)).first;
        pk.lit = it->second;
    }
    // a material's surface for the reflections: writing the depth (the world, opaque or cut out; RenderWare blends
    // it all the same) to the scene's target, not a character's
    {
        bool character = (sh.prog && pk.lit) || (sh.fixed_transform && (sh.fk.light & 1));
        pk.gbuf = app::Get().reflections && stages.mat && stages.mat->m && stages.mat->m->refl > 0 && !character && !sh.quad &&
                  SimpleState(s, kMZWriteEnable) && s.rs[kRsZEnable] && bound_rt_ && bound_rt_ == scene_rt_ && bound_rt_->gw == guest_w_ &&
                  bound_rt_->gh == guest_h_ ? 1 : 0;
    }
    // XBE_SKIN_DUMP: each character program (lit by itself) once, its HLSL to logs/skin_<key>.hlsl
    static const bool skin_dump = std::getenv("XBE_SKIN_DUMP") != nullptr;
    if (skin_dump && sh.prog && pk.lit && !flat) {
        static std::unordered_set<uint64_t> dumped;
        if (dumped.insert(sh.vkey).second && dumped.size() < 12) {
            char name[64];
            std::snprintf(name, sizeof(name), "skin_%016llx.hlsl", (unsigned long long)sh.vkey);
            std::ofstream f(app::ExeDir() / "logs" / name);
            f << "// regs " << a.regs << " count " << a.vertex_count << "\n" << VertexProgramHlsl(s.vs->program, a.regs);
            LOGR("skin: dumped %s (%u vertices, regs %04X)", name, a.vertex_count, a.regs);
        }
    }
    // XBE_LIGHTING_AUDIT: each vertex shader's lighting, once
    static const bool audit = std::getenv("XBE_LIGHTING_AUDIT") != nullptr;
    if (audit && !s.layout2d && !a.pretransformed) {
        static std::unordered_set<uint64_t> seen;
        if (seen.insert(sh.vkey).second) {
            std::string r = sh.prog ? LightingReport(s.vs->program, a.regs)
                            : !(sh.fk.light & 1) ? "fixed function, unlit (vertex colour)"
                            : pk.lit ? "fixed function, per pixel" : "fixed function, PER VERTEX (material from vertex colour)";
            LOGR("lighting audit: vs %08X %s", s.vs_handle, r.c_str());
        }
    }
    GatherLights(s, a, sh, pk);
    if (!flat && s.rs[kRsFogEnable] && !occlusion_done_ && bound_rt_ && bound_rt_ == scene_rt_) {
        float fc[4];
        Color(s.rs[kRsFogColor], fc);
        fog_[0] = fc[0], fog_[1] = fc[1], fog_[2] = fc[2];
        fog_[3] = Bits(s.rs[kRsFogStart]), fog_[4] = Bits(s.rs[kRsFogEnd]);
        fog_seen_ = true;
        static const bool far_log = std::getenv("XBE_FAR_LOG") != nullptr;
        static int n = 0;
        if (far_log && n++ % 3000 == 0) LOGR("fog: start %.0f end %.0f mode %u", fog_[3], fog_[4], s.rs[kRsFogTableMode]);
    }
    // (the pixel key of the draw before, its hash kept)
    if (last_pk_ok_ && !std::memcmp(&pk, &last_pk_, sizeof(pk))) {
        sh.pkey = last_pkey_;
    } else {
        sh.pkey = Hash(&pk, sizeof(pk));
        last_pk_ = pk, last_pkey_ = sh.pkey, last_pk_ok_ = true;
    }
    EnsurePixelShader(pk, sh);
    if (!sh.vs_code || !sh.ps_code) return;

    // pipeline
    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    // XBE_TSS_LOG=<texture id>: that texture's draws, their texture stages' states, the fixed function's texture
    // set up and the texture matrices (each draw shape once)
    {
        static const uint64_t tss_of = [] {
            const char* e = std::getenv("XBE_TSS_LOG");
            return e ? std::strtoull(e, nullptr, 16) : 0ull;
        }();
        static std::unordered_set<uint64_t> told_tss;
        if (tss_of && draw_tex0_ == tss_of &&
            told_tss.insert(uint64_t(presents_ / 60) << 40 | uint64_t(a.index_count ? a.index_count : a.draw_count) << 8 | sh.fk.tex[0]).second &&
            told_tss.size() < 300) {
            LOGR("tss: tex %016llx count %u prog %d fixed %d regs %04X fk.tex %X %X %X %X", (unsigned long long)draw_tex0_,
                 a.index_count ? a.index_count : a.draw_count, int(sh.prog), int(sh.fixed_transform), a.regs, sh.fk.tex[0], sh.fk.tex[1],
                 sh.fk.tex[2], sh.fk.tex[3]);
            for (int t = 0; t < 4; t++) {
                const auto& ts = s.tss[size_t(t)];
                std::string line;
                for (int i = 0; i < kTssCount; i++) line += " " + std::to_string(ts[size_t(i)]);
                LOGR("tss:   stage %d id %016llx:%s", t, (unsigned long long)stages.id[t], line.c_str());
                const auto& m = s.transforms[kTsTexture0 + size_t(t)];
                LOGR("tss:   matrix %d: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", t, m[0], m[1], m[2], m[3], m[4], m[5], m[6],
                     m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            }
            // the first vertices: each register's floats (position, colour as bytes, texture coordinates)
            for (uint32_t vi = 0; vi < std::min<uint32_t>(a.vertex_count, 8); vi++) {
                std::string line;
                for (int r = 0; r < 16; r++) {
                    if (!((a.regs >> r) & 1)) continue;
                    const VertexElement& e = a.elements[size_t(r)];
                    const VertexBuffer& vb = a.buffers[e.buffer];
                    if (!vb.src) continue;
                    const uint8_t* at = vb.src + size_t(vi) * vb.stride + e.offset;
                    char text[96];
                    if (e.format == DXGI_FORMAT_R32G32B32_FLOAT || e.format == DXGI_FORMAT_R32G32_FLOAT || e.format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                        const float* f = reinterpret_cast<const float*>(at);
                        int n = e.format == DXGI_FORMAT_R32G32_FLOAT ? 2 : e.format == DXGI_FORMAT_R32G32B32_FLOAT ? 3 : 4;
                        std::snprintf(text, sizeof(text), " v%d(%g %g %g %g)", r, f[0], f[1], n > 2 ? f[2] : 0.0f, n > 3 ? f[3] : 0.0f);
                    } else {
                        uint32_t w;
                        std::memcpy(&w, at, 4);
                        std::snprintf(text, sizeof(text), " v%d(fmt %u: %08X)", r, unsigned(e.format), w);
                    }
                    line += text;
                }
                LOGR("tss:   vertex %u%s", vi, line.c_str());
            }
        }
    }
    bool far_water = false;
    // points: squares of their size (PointSpriteGsHlsl), as the console drew them
    const bool points = a.topology == Topology::Points;
    if (points) k.topo |= kPointSpriteKey;
    // XBE_WATER_LOG: level 1's water draws' first vertices (position, texture coordinates), once each
    {
        static const bool water_log = std::getenv("XBE_WATER_LOG") != nullptr;
        static std::unordered_set<uint64_t> told_water;
        if (water_log && stages.mat && stages.mat->m && stages.mat->m->water && told_water.insert(uint64_t(a.vertex_count) << 32 | k.blend).second) {
            LOGR("water: count %u verts %u regs %04X blend %d prog %d", a.index_count ? a.index_count : a.draw_count, a.vertex_count, a.regs, k.blend, int(sh.prog));
            for (int r = 0; r < 16; r++)
                if ((a.regs >> r) & 1)
                    LOGR("water:   v%d fmt %u buf %u off %u stride %u", r, a.elements[r].format, a.elements[r].buffer, a.elements[r].offset,
                         a.buffers[a.elements[r].buffer].stride);
            for (uint32_t vi = 0; vi < std::min<uint32_t>(a.vertex_count, 12); vi++) {
                std::string line;
                for (int r = 0; r < 16; r++) {
                    if (!((a.regs >> r) & 1)) continue;
                    const VertexElement& e = a.elements[r];
                    const VertexBuffer& vb = a.buffers[e.buffer];
                    int nf = e.format == 2 ? 4 : e.format == 6 ? 3 : e.format == 16 ? 2 : 0;
                    if (!vb.src || !nf) continue;
                    const float* f = reinterpret_cast<const float*>(vb.src + size_t(vi + a.base_vertex) * vb.stride + e.offset);
                    line += " v" + std::to_string(r) + "(";
                    for (int j = 0; j < nf; j++) line += std::to_string(f[j]) + (j + 1 < nf ? " " : ")");
                }
                LOGR("water:   %u%s", vi, line.c_str());
            }
        }
    }
    // level 1's far water (the Volga's ring out to the horizon, blended over the sky, no depth of its own): its depth
    // written, or the passes took it for sky and laid the clouds and the sky's haze over it (lighter, smooth)
    if (g_level.load() == 1 && !flat && stages.mat && stages.mat->m && stages.mat->m->water && k.blend && k.zen && !k.zwrite) {
        static int told = 0;
        if (told++ < 3) LOGR("water: level 1's far water drawn solid (tex %016llx)", (unsigned long long)draw_tex0_);
        k.zwrite = true, k.blend = false;
        far_water = !sh.prog;
    }
    if (g_log_draws && k.blend && !flat) {
        uint32_t col = 0;
        const VertexElement& ce = a.elements[3];
        if ((a.regs & 8) && ce.buffer < a.buffer_count && a.buffers[ce.buffer].src) std::memcpy(&col, a.buffers[ce.buffer].src + ce.offset, 4);
        LOGR("draw:   blend tex %016llx src %X dst %X op %X zwrite %u zfunc %X col %08X atest %u aref %u prog %d",
             (unsigned long long)draw_tex0_, k.src, k.dst, k.op, k.zwrite, k.zfunc, col, SimpleState(s, kMAlphaTestEnable),
             SimpleState(s, kMAlphaRef), int(sh.prog));
        LOGR("draw:   tfactor %08X", s.rs[kRsTextureFactor]);
    }
    // light alone (tracers, sparks: EmitterKind pure) is added to what is behind it, never laid over it: a burst's
    // tracers, met where the rounds land and shaded dark by the night, piled into a black blot
    {
        const EmitterKind& light_kind = draw_emitter_kind_;
        bool sprite = k.blend && !flat && draw_emitter_;
        if (sprite && light_kind.pure) {
            constexpr uint32_t kSrcAlpha = 0x302, kOne = 0x1, kAdd = 0x8006;
            k.src = kSrcAlpha, k.dst = kOne, k.op = kAdd;
        }
        // and no effect's sprite writes the depth: its see-through quad cut what is drawn behind it after (the
        // sparks, the flash, lit smoke), a dark hole the tracer's shape where a burst's rounds land
        if (sprite) k.zwrite = false;
    }
    // a character's triangles (a program lit by itself, drawn with the world: not the weapon in hand, its hard edges
    // would swell), smoothed by tessellation
    tess_draw_ = false;
    if (app::Get().smooth_models && sh.prog && pk.lit && !flat && !sh.quad && !occlusion_done_ && a.topology == Topology::Triangles && tess_hs_ &&
        tess_ds_ && bound_rt_ && bound_rt_->gw == guest_w_ && bound_rt_->gh == guest_h_) {
        auto it = tess_rows_.find(sh.vkey);
        if (it == tess_rows_.end()) it = tess_rows_.emplace(sh.vkey, TessRows(s.vs->program, a.regs)).first;
        // characters alone (skinned): a rigid lit model (a window's frame, a crate) is boxy by design, and its
        // patches, flattened or not as the camera turns, switched its light on and off
        if (it->second.ok && it->second.skinned) {
            tess_draw_ = true;
            k.topo |= kTessKey;
            for (int j = 0; j < 4; j++) tess_now_[j] = float(it->second.rows[j]);
            tess_now_[4] = float(it->second.offset);
        }
    }
    if (pk.gbuf && BindGbuf(true)) k.topo |= kGbufKey;
    else BindGbuf(false);
    // multisampled: the sample count, and what is cut out by its alpha (foliage, fences, wires; drawn solid, the
    // alpha past a reference) with its edge as coverage: smooth, not stair stepped
    bool a2c = false;
    if (msaa_bound_) {
        k.topo |= kMsaaKey | (msaa_samples_ << 16);
        uint32_t f = Cmp(SimpleState(s, kMAlphaFunc));
        if (SimpleState(s, kMAlphaTestEnable) && !k.blend && k.has_rt &&
            (f == D3D12_COMPARISON_FUNC_GREATER || f == D3D12_COMPARISON_FUNC_GREATER_EQUAL))
            k.topo |= kA2cKey, a2c = true;
    }
    static const bool ssr_log = std::getenv("XBE_SSR_LOG") != nullptr;
    if (ssr_log) {
        static int n = 0;
        if (stages.mat && !flat && n++ % 500 == 0)
            LOGR("ssr: material draw refl %.2f gbuf %u bound %d scene %d blend %u character %d", stages.mat->m ? stages.mat->m->refl : -1.0f,
                 pk.gbuf, int(gbuf_bound_), int(bound_rt_ == scene_rt_), SimpleState(s, kMBlendEnable), int(draw_character_));
    }
    // the weapon in hand on a wide screen (drawn after the world's depth is cleared): see WeaponCullGsHlsl
    if (!flat && !sh.quad && occlusion_done_ && widen_ > 1.0f && a.topology == Topology::Triangles && bound_rt_ &&
        bound_rt_->gw == guest_w_ && bound_rt_->gh == guest_h_)
        k.topo |= kWeaponCullKey;
    static const bool emit_log = std::getenv("XBE_EMIT_LOG") != nullptr;
    if (emit_log && !flat && k.blend && bound_rt_ && bound_rt_->gw == guest_w_ && bound_rt_->gh == guest_h_) {
        static std::unordered_map<uint64_t, int> seen;
        int& n = seen[draw_tex0_ ^ (uint64_t(k.dst) << 56) ^ (uint64_t(sh.prog) << 60)];
        if (n++ % 200 == 0) {
            const VertexElement& e = a.elements[0];
            float p[3] = {};
            const VertexBuffer* vb = e.buffer < a.buffer_count ? &a.buffers[e.buffer] : nullptr;
            if (vb && vb->src) std::memcpy(p, vb->src + e.offset, 12);
            const float* W = s.transforms[kTsWorld].data();
            float wx = p[0] * W[0] + p[1] * W[4] + p[2] * W[8] + W[12], wy = p[0] * W[1] + p[1] * W[5] + p[2] * W[9] + W[13],
                  wz = p[0] * W[2] + p[1] * W[6] + p[2] * W[10] + W[14];
            uint32_t col = 0;
            const VertexElement& ce = a.elements[3];
            if ((a.regs & 8) && ce.buffer < a.buffer_count && a.buffers[ce.buffer].src) std::memcpy(&col, a.buffers[ce.buffer].src + ce.offset, 4);
            LOGR("emit: tex %016llx prog %d src %u dst %u zw %u topo %u verts %u regs %04X col %08X tf %08X world %.0f %.0f %.0f (#%d)",
                 (unsigned long long)draw_tex0_, int(sh.prog), k.src, k.dst, k.zwrite, k.topo, a.vertex_count, a.regs, col,
                 s.rs[kRsTextureFactor], wx, wy, wz, n);
        }
    }
    ID3D12PipelineState* pso = EnsurePipeline(k, sh.vs_code, sh.ps_code);
    if (!pso) return;
    SetPso(pso);

    // constants
    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    Layout2D(s, a, sh.prog, size, c);
    // the weapon in hand on a wide screen: a piece of it the 4:3 view shows is drawn whole (an arm going on to the
    // screen's edge); only a piece wholly outside it (an arm's cut end the console never showed) is left out
    weapon_filter_ = false;
    if (c.layout[2] > 0 && c.layout[2] < 1 && sh.prog && a.topology == Topology::Triangles && WeaponPieces(s, a, sh))
        c.layout[2] = 0;
    if (!flat && bound_rt_ && bound_rt_ == scene_rt_) c.jitter[0] = taa_jit_[0], c.jitter[1] = taa_jit_[1];
    c.jitter[2] = a2c ? 1.0f : 0.0f;  // alphatest: the edge as coverage
    if (points) {
        auto f = [&](int rs) { return std::bit_cast<float>(s.rs[rs]); };
        c.pts0[0] = f(kRsPointSize), c.pts0[1] = f(kRsPointSizeMin), c.pts0[2] = f(kRsPointSizeMax);
        c.pts0[3] = s.rs[kRsPointScaleEnable] ? 1.0f : 0.0f;
        c.pts1[0] = f(kRsPointScaleA), c.pts1[1] = f(kRsPointScaleB), c.pts1[2] = f(kRsPointScaleC);
        c.pts1[3] = s.rs[kRsPointSpriteEnable] ? 1.0f : 0.0f;
        static std::unordered_set<uint64_t> told_points;
        if (told_points.insert(draw_tex0_).second && told_points.size() < 64)
            LOGR("points: tex %016llx size %g min %g max %g scale %d (%g %g %g) sprite %d prog %d count %u", (unsigned long long)draw_tex0_,
                 c.pts0[0], c.pts0[1], c.pts0[2], int(c.pts0[3]), c.pts1[0], c.pts1[1], c.pts1[2], int(c.pts1[3]), int(sh.prog),
                 a.index_count ? a.index_count : a.draw_count);
    }
    // level 1's far water: its texture laid flat at the near water's density (FixedVertexHlsl, -jitter.z)
    if (far_water) {
        static const float density = [] {
            const char* e = std::getenv("XBE_RING_UV");
            return e ? float(std::atof(e)) : 1.0f / 3000;  // the near water: a tile about 3000 units
        }();
        c.jitter[2] = -density;
    }
    // the sky's pieces (sky_phase_: the frame's first draws, with depth written whatever is there): at the far plane,
    // as the sky is (some levels' sky boxes are drawn about the camera at a few thousand units: the clouds, the sun and
    // the passes took their nearer parts for the world, a hard edge across the sky where they met). Level 1: one layer
    // each, its pieces told apart (scene_shaders.h SkyLayer)
    if (g_level.load() != 1 && !flat && bound_rt_ && bound_rt_ == scene_rt_ && !occlusion_done_) {
        constexpr uint32_t kAlways = 0x207;  // D3DCMP_ALWAYS
        if (sky_phase_ && k.zen && k.zwrite && k.zfunc == kAlways) c.jitter[3] = 1.0f;
        else sky_phase_ = false;
    }
    if (g_level.load() == 1 && !flat && bound_rt_ && bound_rt_ == scene_rt_ && !occlusion_done_) {
        constexpr uint32_t kAlways = 0x207;  // D3DCMP_ALWAYS
        // the column of oil smoke over the city (aOilCloud: shells drawn with the world, their outline the polygons')
        // with the sky too, a layer a texture past the sky's own (16 on: veiled, scene_shaders.h Veiled)
        static const uint64_t kOilCloud[] = {0x097c440f7e2d3a5cull, 0xca3f4e0fb7c8bad2ull, 0xf148c5deea5fbe3aull};
        if (sky_phase_ && k.zen && k.zwrite && k.zfunc == kAlways && sky_layer_ < 16) {
            c.jitter[3] = 1.0f - float(sky_layer_++) * kSkyLayerStep;
        } else {
            sky_phase_ = false;
            for (uint32_t j = 0; j < 3; j++)
                if (draw_tex0_ == kOilCloud[j] && k.zen) c.jitter[3] = 1.0f - float(16 + j) * kSkyLayerStep;
            // the far water (far_water: drawn first of the river, before all that floats on it) at layer 20:
            // the sky pass leaves it as it is (scene_shaders.h skyfx)
            if (far_water) c.jitter[3] = 1.0f - 20.0f * kSkyLayerStep;
        }
    }
    // lit by the game itself (its programs, or fixed function lights: what characters hold and wear): no relief or highlights of the materials
    draw_character_ = (sh.prog && pk.lit) || (sh.fixed_transform && (sh.fk.light & 1));
    // (not the water: level 1's Volga is a skinned mesh its bones heave, lit by its program, but water all the same)
    if (stages.mat && stages.mat->m && stages.mat->m->water) draw_character_ = false;
    // wind (Wind): foliage and flags (the remaster's classes), cut out by their alpha, sway; their strength in pixels
    // at depth 1 (the program's position in pixels before the divide: about 6 cm at 1 m), the time
    if (app::Get().wind && !flat && !tess_draw_ && draw_tex0_ && SimpleState(s, kMAlphaTestEnable) && occlusion_done_ == false) {
        if (const remaster::Material* m = remaster::Find(draw_tex0_)) {
            if (m->sway > 0) {
                static const auto t0 = std::chrono::steady_clock::now();
                float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
                c.mrefl[3] = m->sway * float(size.h) * 0.04f;
                c.tess2[1] = std::fmod(t, 1000.0f);
            }
        }
    }
    // a character's light (Character Lighting)
    if (app::Get().character_light && draw_character_ && sh.prog && !flat && !g_texview.load()) c.mrefl[2] = 1;
    // a glowing sprite: past white in the HDR scene (its bloom), and its light
    if (emitter && hdr_ && glow.glow > 1 && app::Get().bloom) c.layout[3] = glow.glow;
    // a flame's sprite: the time for its inner flow (Animated Fire), its flicker kind (fire, not a shot's flash)
    if (emitter && glow.flicker && app::Get().animated_fire && !tess_draw_) {
        static const auto t0 = std::chrono::steady_clock::now();
        c.tess2[0] = 0.1f + std::fmod(std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count(), 500.0f);
    }
    // falling water (the pipes' and the sewer's waterfall sheets, water_flowing: nearly black, their texture's streaks
    // lit only by the dark prelit colours): the sheet's texture runs down it and the water catches the light (Water)
    if (app::Get().water && !flat) {
        static const uint64_t kFlowing[] = {0x3d399e0f1b26898bull, 0xdf5acfa8682d9ea1ull, 0xe15b8faaa2324069ull};
        for (uint64_t id : kFlowing)
            if (draw_tex0_ == id) {
                static const auto t0 = std::chrono::steady_clock::now();
                static const float speed = [] {
                    const char* e = std::getenv("XBE_FLOW_SPEED");
                    return e ? float(std::atof(e)) : 0.9f;
                }();
                c.flow[0] = 0.1f + std::fmod(std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count(), 600.0f);
                // the game's own flow when its sector draws the material's animation (src/game/matfx.cpp: the
                // texture transform set), else ours along u
                c.flow[1] = s.tss[0][kTssTextureTransformFlags] ? 0.0f : speed, c.flow[2] = 2.6f, c.flow[3] = 1.15f;
                // a falling sheet, not a pool: no flat water's ripples and up facing normal, little reflection and
                // sheen (on its upright face they came out white)
                c.mrefl[0] = 0.12f, c.mrefl[2] = 0;
                c.mtl[1][3] *= 0.25f;
            }
    }
    // aircraft propellers (VT_Aircraft_propeller): spun into a soft disc (shaders.cpp, flow.x < 0: a turn, the opacity)
    if (!flat) {
        static const uint64_t kProps[] = {0x0e83fc4858f212cfull, 0x1f00add88e7ed459ull, 0xd63f67e622630665ull};
        for (uint64_t id : kProps)
            if (draw_tex0_ == id) {
                static const auto t0 = std::chrono::steady_clock::now();
                float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
                c.flow[0] = -1, c.flow[1] = std::fmod(t * 40.0f, 6.2831853f), c.flow[2] = 0.85f;
                c.flow[3] = std::fmod(t * 9.0f, 6.2831853f);  // the blades' faint sweep (slower than the spin: its strobe)
            }
    }
    // XBE_FLAT_LOG: each texture of the 2D draws, once (blend, layout or pretransformed, count)
    {
        static const bool flat_log = std::getenv("XBE_FLAT_LOG") != nullptr;
        static std::unordered_set<uint64_t> told_flat;
        if (flat_log && flat && told_flat.insert(draw_tex0_ ^ (uint64_t(k.dst) << 56)).second)
            LOGR("flat: tex %016llx blend %d %X %X layout2d %d pretransformed %d count %u rt %d", (unsigned long long)draw_tex0_, k.blend,
                 k.src, k.dst, s.layout2d ? 1 : 0, a.pretransformed ? 1 : 0, a.index_count ? a.index_count : a.draw_count,
                 bound_rt_ == scene_rt_ ? 1 : 0);
    }
    // XBE_DRAW_LOG: a frame's 3D draws now and then, in order (texture, viewport depth, depth write and test, blend)
    {
        static const bool draw_log = std::getenv("XBE_DRAW_LOG") != nullptr;
        if (draw_log && !flat && presents_ % 400 == 200)
            LOGR("draw: tex %016llx vz %.4f..%.4f zen %d zw %d zf %X blend %d %X %X at %d count %u rt %d occ %d", (unsigned long long)draw_tex0_,
                 s.viewport.min_z, s.viewport.max_z, k.zen, k.zwrite, k.zfunc, k.blend, k.src, k.dst, SimpleState(s, kMAlphaTestEnable) ? 1 : 0,
                 a.index_count ? a.index_count : a.draw_count, bound_rt_ == scene_rt_ ? 1 : 0, occlusion_done_ ? 1 : 0);
    }
    {
        static const bool uv_debug = std::getenv("XBE_UV_DEBUG") != nullptr;  // the texture coordinates' fraction
        if (uv_debug && !flat && !k.blend) c.mrefl[3] = -2;
    }
    if (g_texview.load() && !flat) {
        // tests: the texture's own colour, from its id; each colour logged once
        uint64_t h = draw_tex0_ * 0x9E3779B97F4A7C15ull;
        uint8_t rgb[3] = {uint8_t(40 + (h >> 40) % 200), uint8_t(40 + (h >> 48) % 200), uint8_t(40 + (h >> 56) % 200)};
        c.mrefl[3] = -1;  // (> 0 is the wind's)
        for (int j = 0; j < 3; j++) c.tess2[1 + j] = rgb[j] / 255.0f;
        static std::unordered_set<uint64_t> told;
        if (told.insert(draw_tex0_).second) LOGR("texview: %016llx %d %d %d", (unsigned long long)draw_tex0_, rgb[0], rgb[1], rgb[2]);
        // the later stages' textures too (listed for the searches: a material can be on any stage)
        for (int st = 1; st < 4; st++)
            if (stages.id[st] && told.insert(stages.id[st]).second)
                LOGR("texview: %016llx 0 0 0 (stage %d, with %016llx)", (unsigned long long)stages.id[st], st, (unsigned long long)draw_tex0_);
    }
    // smoke and dust (the table's sprites that do not light): the game shades them by the level's light, by night
    // nearly black, a dark blot where a burst's rounds land; they fade as dark as they are drawn (layout2d.w < 0)
    {
        const EmitterKind& smoke = draw_emitter_kind_;
        if (!flat && draw_emitter_) {
            if (!smoke.light) {
                c.layout[3] = -1;
                // the lights near it (Lit Smoke): the frame's dynamic lights, view space, in lt (a program's draw
                // reads none of its fixed function lights)
                if (sh.prog && app::Get().lit_smoke && app::Get().dynamic_lights) {
                    // once a frame (DynLightConstants also sets the frame's heat and flash, as the composite will)
                    if (smoke_lights_frame_ != frame_) {
                        smoke_lights_frame_ = frame_;
                        SceneFx keep = fx_;
                        smoke_lights_n_ = DynLightConstants(smoke_lights_pr_, smoke_lights_cl_, 8);
                        fx_ = keep;
                    }
                    const float* pr = smoke_lights_pr_;
                    const float* cl = smoke_lights_cl_;
                    uint32_t n = smoke_lights_n_;
                    const MaterialFrame& mf = FrameMaterial();
                    for (uint32_t j = 0; j < n; j++) {
                        std::memcpy(c.lt[j][0], pr + j * 4, 16);
                        std::memcpy(c.lt[j][1], cl + j * 4, 16);
                    }
                    c.lt[0][6][0] = float(n), c.lt[0][6][1] = mf.view_x, c.lt[0][6][2] = mf.view_y;
                }
            }
            // light alone (tracers, sparks): 50 past its glow, its dark see-through (a burst's tracers, all met
            // where the rounds land and shaded by the night, piled into a black blot)
            if (smoke.pure) c.layout[3] = 50 + (c.layout[3] > 0 ? c.layout[3] : 1);
        }
    }
    if (soft_draw_) {
        static const bool show = std::getenv("XBE_SOFT_DEBUG") != nullptr;
        c.layout[2] = show ? -glow.soft : glow.soft;
        const MaterialFrame& mf = FrameMaterial();
        c.surf[3] = mf.far_plane > mf.near_plane && mf.near_plane > 0 ? 1 - mf.near_plane / mf.far_plane : 1;
    }
    if (tess_draw_) std::memcpy(c.tess, tess_now_, sizeof(c.tess)), c.tess2[0] = tess_now_[4];
    // what a character holds or wears, carried with the bone the flinch moved (g_carry): its world matrix for this
    // draw only
    struct WorldBack {
        State* s = nullptr;
        std::array<float, 16> w{};
        ~WorldBack() {
            if (s) s->transforms[kTsWorld] = w;
        }
    } world_back;
    auto carry = g_carry.load();
    if (carry && sh.fixed_transform && (sh.fk.light & 1) && !flat && !occlusion_done_) {
        State& ms = const_cast<State&>(s);
        std::array<float, 16> w = ms.transforms[kTsWorld];
        if (carry(ms.transforms[kTsWorld].data())) world_back.s = &ms, world_back.w = w;
    }
    UploadConstants(s, sh, k, stages, size, c);
    if (emitter && (glow.light || glow.impact > 0)) CollectEmitter(s, a, sh, glow);

    if (cap_) {
        CaptureDraw(s, prim, count, k, c, stages);
        EndDrawCapture(s, a, count, c);
        CaptureShaders(s, a, sh, pk);
    }

    // texture views and samplers, viewport and scissor, blend factor and stencil reference
    SetTextureViewsAndSamplers(s, a, stages);
    SetViewportScissor(s, size.w);
    SetBlendFactorStencilRef(s);

    // geometry
    DrawGeometry(a);
    // (the sky's pieces, level 1's oil smoke shells and far water: at the far plane, no caster, no guard band)
    draw_far_ = c.jitter[3] > 0;
    RecordCaster(s, a, sh, pk, k, stages);
    if (stages.mat && stages.mat->m && stages.mat->m->water && app::Get().water && app::Get().water_interaction)
        RecordWaterTop(s, a, sh, k);
    RecordVeil(s, a, sh, k, stages);
    RecordMotion(s, a, sh, k);
    if (map_on_) CollectMap(s, a, sh, pk, k);
}

void D3D12Backend::Layout2D(const State& s, const Assembled& a, bool prog, GuestExtent size, DrawConstants& c) {
    c.layout[0] = 1;
    layout_[0] = 1, layout_[1] = 0;
    recolor_ = false;
    auto layout = g_layout_2d.load();
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    bool screen = t && t->gw == guest_w_ && t->gh == guest_h_;
    // the weapon in hand (drawn after the world's depth is cleared): its pieces wholly outside the console's 4:3 view,
    // never shown by the game, are left out (a sleeve's cut end by the left edge of a wide screen)
    c.layout[2] = !(a.pretransformed || s.layout2d) && screen && occlusion_done_ && widen_ > 1.0f ? 1.0f / widen_ : 0.0f;
    if (int ww = g_weapon_whole.load(std::memory_order_relaxed); ww == 1) c.layout[2] = 0;  // tests: as the game draws it
    const VertexElement& e = a.elements[0];
    if (g_log_draws && screen) {
        float p[4] = {};
        if ((a.regs & 1) && e.buffer < a.buffer_count && a.buffers[e.buffer].src) std::memcpy(p, a.buffers[e.buffer].src + e.offset, 16);
        LOGR("draw: tex %016llx blend %u tag %u prog %d pre %d regs %04X fmt %u count %u cull %.2f v0 %.1f %.1f %.3f %.3f",
             (unsigned long long)draw_tex0_, SimpleState(s, kMBlendEnable), s.layout2d, prog, a.pretransformed, a.regs, e.format, a.vertex_count, c.layout[2],
             p[0], p[1], p[2], p[3]);
    }
    auto seen = g_seen_2d.load();
    if (!(a.pretransformed || s.layout2d) || !screen) return;
    if ((!layout || widen_ <= 1.0f) && !seen) return;
    Draw2D d{1e9f, 1e9f, -1e9f, -1e9f, size.w, size.h, widen_, a.pretransformed ? 0 : s.layout2d, false, presents_, 0xFFFFFFFF};
    // the bounds in console pixels: pretransformed positions as they are, fixed function ones through world, view,
    // projection and the viewport; a vertex program's are not known
    bool floats = e.format == DXGI_FORMAT_R32G32B32A32_FLOAT || e.format == DXGI_FORMAT_R32G32B32_FLOAT ||
                  (a.pretransformed && e.format == DXGI_FORMAT_R32G32_FLOAT);
    const VertexBuffer* b = (a.regs & 1) && e.buffer < a.buffer_count ? &a.buffers[e.buffer] : nullptr;
    // tags with bit 8 set: a vertex program's positions are already screen pixels
    bool pixels = a.pretransformed || (s.layout2d & 0x100);
    if (floats && b && b->src && b->stride && (pixels || !prog)) {
        float wv[16], wvp[16];
        if (!pixels) {
            MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
            MatMul(wv, s.transforms[kTsProjection].data(), wvp);
        }
        const auto& v = s.viewport;
        uint32_t n = std::min<uint32_t>(a.vertex_count ? a.vertex_count : b->bytes / b->stride, b->bytes / b->stride);
        for (uint32_t i = 0; i < n; i++) {
            float p[3] = {};
            std::memcpy(p, b->src + size_t(i) * b->stride + e.offset, e.format == DXGI_FORMAT_R32G32_FLOAT ? 8 : 12);
            float x = p[0], y = p[1];
            if (!pixels) {
                float cx = p[0] * wvp[0] + p[1] * wvp[4] + p[2] * wvp[8] + wvp[12];
                float cy = p[0] * wvp[1] + p[1] * wvp[5] + p[2] * wvp[9] + wvp[13];
                float cw = p[0] * wvp[3] + p[1] * wvp[7] + p[2] * wvp[11] + wvp[15];
                if (cw <= 1e-6f) continue;
                x = v.x + (cx / cw * 0.5f + 0.5f) * v.w;
                y = v.y + (0.5f - cy / cw * 0.5f) * v.h;
            }
            d.x0 = std::min(d.x0, x), d.x1 = std::max(d.x1, x);
            d.y0 = std::min(d.y0, y), d.y1 = std::max(d.y1, y);
            d.bounded = true;
        }
    }
    if (!d.bounded) d.x0 = 0, d.y0 = 0, d.x1 = float(size.w), d.y1 = float(size.h);
    {
        uint32_t color = 0xFFFFFFFF;
        const VertexElement& ce = a.elements[3];
        if ((a.regs & 8) && ce.buffer < a.buffer_count && a.buffers[ce.buffer].src && ce.format == DXGI_FORMAT_B8G8R8A8_UNORM)
            std::memcpy(&color, a.buffers[ce.buffer].src + ce.offset, 4);
        d.color = color;
        if (seen && d.bounded) seen({d.x0, d.y0, d.x1, d.y1, draw_tex0_, color, s.layout2d, presents_});
        uint32_t argb;
        auto recolor = g_recolor_2d.load();
        if (recolor && d.bounded && (a.regs & 8) && ce.format == DXGI_FORMAT_B8G8R8A8_UNORM &&
            recolor({d.x0, d.y0, d.x1, d.y1, draw_tex0_, color, s.layout2d, presents_}, argb))
            recolor_ = true, recolor_argb_ = argb, recolor_elem_ = ce;
    }
    if (!layout || widen_ <= 1.0f) return;
    float scale = 1, offset = 0;
    if (layout(d, scale, offset)) c.layout[0] = scale, c.layout[1] = offset;
    layout_[0] = c.layout[0], layout_[1] = c.layout[1];
    if (g_log_draws)
        LOGR("draw:   bounds %d %.0f..%.0f x %.0f..%.0f -> x * %.3f + %.1f", d.bounded, d.x0, d.x1, d.y0, d.y1, c.layout[0], c.layout[1]);
}

// a guest rect scaled to the target and clamped to it
D3D12_RECT ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sx = float(t.w) / t.gw, sy = float(t.h) / t.gh;
    return {std::clamp<LONG>(LONG(x0 * sx), 0, t.w), std::clamp<LONG>(LONG(y0 * sy), 0, t.h),
            std::clamp<LONG>(LONG(x1 * sx), 0, t.w), std::clamp<LONG>(LONG(y1 * sy), 0, t.h)};
}

// rs: the rects buffer, reused across targets
void D3D12Backend::ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs) {
    rs.clear();
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            rs.push_back(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        rs.push_back(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    if (t.depth) {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CLEAR_FLAGS cf = D3D12_CLEAR_FLAGS(((c.flags & kClearZ) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                 ((c.flags & kClearStencil) ? D3D12_CLEAR_FLAG_STENCIL : 0));
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_), cf, std::clamp(c.z, 0.0f, 1.0f),
                                   UINT8(c.stencil), UINT(rs.size()), rs.data());
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float col[4];
        Color(c.color, col);
        for (int i = 0; t.range != 1 && i < 3; i++) col[i] /= t.range;
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), col, UINT(rs.size()), rs.data());
    }
    t.drawn = true;
}

void D3D12Backend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                         uint32_t stencil) {
    if (g_log_draws) LOGR("draw: clear %u rects flags %X color %08X", count, flags, color);
    PerfScope perf_clear{g_perf.clear, g_draw_timers};
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f\n", cap_i_++, s.rt.data, s.zs.data, flags,
                color, z);
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    if (rt && !rt->depth && rt->gw == guest_w_ && rt->gh == guest_h_ && count == 0) colour_frame_ = frame_;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    // the frame's first full clear of the scene's colour and depth: the multisampled pair from now on (Anti-Aliasing)
    if (msaa_active_) MsaaResolve();
    if (!msaa_done_ && app::Get().taa && !SmaaMode() && rt && ds && count == 0 && (flags & kClearTarget) && (flags & kClearZ) &&
        !rt->depth && rt->gw == guest_w_ && rt->gh == guest_h_ && rt->w == ds->w && rt->h == ds->h && EnsureMsaa(*rt, 4)) {
        float col[4];
        Color(color, col);
        for (int i = 0; t_range_ok(rt) && i < 3; i++) col[i] /= rt->range;
        Transition(msaa_rt_.Get(), msaa_rt_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Transition(msaa_ds_.Get(), msaa_ds_state_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cl_->ClearRenderTargetView(MsaaRtv(), col, 0, nullptr);
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), msaa_dsv_, dsv_inc_), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                   std::clamp(z, 0.0f, 1.0f), UINT8(stencil), 0, nullptr);
        msaa_active_ = true, msaa_done_ = true, msaa_gbuf_used_ = false;
        msaa_for_rt_ = rt, msaa_for_ds_ = ds;
        bound_rt_ = bound_ds_ = nullptr, msaa_bound_ = false;
    }
    // the world's depth is about to go (the weapon in hand clears it): its occlusion first
    if (ds && ds == scene_ds_ && (flags & kClearZ) && !occlusion_done_) {
        SceneOcclusion();
        SaveWorldDepth(*ds);
        GpuMark("world depth copy");
        // the sky's clouds, sun and moon on the world now: the weapon in hand drawn over them (its see-through parts
        // show them too)
        if (scene_rt_ && EnsureSceneTargets(scene_rt_->fmt, scene_rt_->w, scene_rt_->h) && SkyConstants()) {
            float p[16];
            SceneParams(*scene_rt_, p);
            CopyScene(*scene_rt_);
            SkyPass(*scene_rt_, p);
            GpuMark("sky");
            sky_done_ = true;
            BindTargets(s);
        }
    }
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(rt ? s.rt : s.zs, gw, gh);
    std::vector<D3D12_RECT> rs;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (Target* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, c, rs);
    }
}

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kRectLeft = 0;
constexpr uint32_t kRectTop = 4;
constexpr uint32_t kRectRight = 8;
constexpr uint32_t kRectBottom = 12;
constexpr uint32_t kGuestPointBytes = 8;
constexpr uint32_t kPointX = 0;
constexpr uint32_t kPointY = 4;

struct GuestRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct GuestPoint {
    int32_t x;
    int32_t y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    GuestRect g;
    g.left = int32_t(kernel::R32(r + kRectLeft));
    g.top = int32_t(kernel::R32(r + kRectTop));
    g.right = int32_t(kernel::R32(r + kRectRight));
    g.bottom = int32_t(kernel::R32(r + kRectBottom));
    return g;
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    GuestPoint g;
    g.x = int32_t(kernel::R32(p + kPointX));
    g.y = int32_t(kernel::R32(p + kPointY));
    return g;
}

// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float dx = 0;
    float dy = 0;
};

// without dst_points the rect lands where it is
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r;
    r.x0 = float(g.left);
    r.y0 = float(g.top);
    r.x1 = float(g.right);
    r.y1 = float(g.bottom);
    r.dx = r.x0;
    r.dy = r.y0;
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

// a guest coordinate read as unsigned, times the host scale
UINT ScaleUnsigned(int32_t v, float sc) { return UINT(uint32_t(v) * sc); }

// a drawn copy's taps round down a ratio a hair over a whole number
constexpr float kTapsSlack = 1e-3f;

// one Blit per rect: for formats that differ
void D3D12Backend::CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                              Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), dst_t.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    float src_scale = float(src_t.h) / src_t.gh;
    float dst_scale = float(dst_t.h) / dst_t.gh;
    float dst_scale_x = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = float(std::min(src_t.gw, dst_t.gw));
        float whole_h = float(std::min(src_t.gh, dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        D3D12_VIEWPORT vp{r.dx * dst_scale_x, r.dy * dst_scale, (r.x1 - r.x0) * dst_scale_x, (r.y1 - r.y0) * dst_scale,
                          0, 1};
        LONG right = std::min(LONG(vp.TopLeftX + vp.Width), LONG(dst_t.w));
        LONG bottom = std::min(LONG(vp.TopLeftY + vp.Height), LONG(dst_t.h));
        D3D12_RECT scissor{LONG(vp.TopLeftX), LONG(vp.TopLeftY), right, bottom};
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
        SetViewport(vp);
        SetScissor(scissor);
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        float mul = 1 / dst_t.range;
        BlitOptions o{.alpha = true, .taps = taps, .mul = mul, .rtv = dst_t.fmt};
        Blit(src_res, src_t.fmt, uv, o);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// one CopyTextureRegion per rect, clamped to both targets
void D3D12Backend::CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                                Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{dst_t.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{src_res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    float dst_scale = float(dst_t.w) / dst_t.gw, dst_scale_y = float(dst_t.h) / dst_t.gh;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        D3D12_BOX b{0, 0, 0, std::min(src_t.w, dst_t.w), std::min(src_t.h, dst_t.h), 1};
        uint32_t dx = 0;
        uint32_t dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            b.left = ScaleUnsigned(r.left, dst_scale);
            b.top = ScaleUnsigned(r.top, dst_scale_y);
            b.right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            b.bottom = std::min(ScaleUnsigned(r.bottom, dst_scale_y), src_t.h);
            dx = b.left;
            dy = b.top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale_y);
            }
        }
        if (b.right <= b.left || b.bottom <= b.top || dx >= dst_t.w || dy >= dst_t.h) continue;
        b.right = std::min<UINT>(b.right, b.left + dst_t.w - dx);
        b.bottom = std::min<UINT>(b.bottom, b.top + dst_t.h - dy);
        cl_->CopyTextureRegion(&dl, dx, dy, 0, &sl, &b);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                        uint32_t dst_points) {
    PerfScope perf_copy{g_perf.copy, g_draw_timers};
    MsaaResolve();
    Target* src_t = FindTarget(src, false);
    if (g_log_draws) LOGR("draw: copy %08X (%s) to %08X, %u rects", src.data, src_t ? "target" : "memory", dst.data, count);
    if (cap_) {
        const char* from = "memory";
        if (src_t) from = src_t->drawn ? "target" : "idle target";
        fprintf(cap_, "%04u copy %08X %s to %08X\n", cap_i_++, src.data, from, dst.data);
    }
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "d3d12: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Resolve(*src_t);
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points};
    if (src_t->fmt != dst_t->fmt || src_t->w * dst_t->gw != dst_t->w * src_t->gw ||
        src_t->h * dst_t->gh != dst_t->h * src_t->gh) {
        // formats or scales differ (a widened screen into a texture): drawn across
        CopyByDraw(*src_t, src_t->res.Get(), src_t->state, *dst_t, l);
        return;
    }
    CopyByRegion(*src_t, src_t->res.Get(), src_t->state, *dst_t, l);
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_i_, dst_t->data);
        CapSave(*dst_t, name);
    }
}

void D3D12Backend::ResizeSwap() {
    HWND hwnd = static_cast<HWND>(app::Window());
    RECT rc;
    GetClientRect(hwnd, &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
    if (w == bb_w_ && h == bb_h_) return;
    Flush();
    for (auto& f : frames_) {
        if (f.fence) WaitFence(f.fence);
    }
    for (auto& b : bb_) b.Reset();
    swap_->ResizeBuffers(kFrames, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    CreateBackBufferViews();
    bb_w_ = w;
    bb_h_ = h;
}

void D3D12Backend::FinishCapture(const Surface& front) {
    CapStep(Surface{});
    if (Target* f = FindTarget(front, false)) CapSave(*f, "final");
    for (auto& t : cap_tex_) {
        char name[32];
        snprintf(name, sizeof(name), "tex_%08X", t.data);
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        CapSave({.res = t.res.Get(), .state = &st, .fmt = t.fmt, .w = t.w, .h = t.h}, name);
    }
    fclose(cap_);
    cap_ = nullptr;
    cap_tex_.clear();
    LOGI("d3d12: frame captured to %s", cap_dir_.string().c_str());
}

void D3D12Backend::StartCapture() {
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02u%02u%02u", tm.wHour, tm.wMinute, tm.wSecond);
    std::error_code ec;
    cap_dir_ = std::filesystem::weakly_canonical(app::ExeDir() / diag::CaptureDir() / stamp, ec);
    std::filesystem::create_directories(cap_dir_, ec);
    cap_ = _wfopen((cap_dir_ / "frame.txt").c_str(), L"w");
    cap_i_ = cap_rt_ = 0;
    cap_seen_.clear();
    cap_sh_.clear();
}

// ramp_ holds the red, green and blue ramps, 256 entries each
constexpr int kRampRed = 0;
constexpr int kRampGreen = 256;
constexpr int kRampBlue = 512;

void D3D12Backend::UploadGammaRamp() {
    constexpr uint32_t kRampBytes = 256 * 4;  // one BGRA texel per ramp entry
    ramp_dirty_ = false;
    Alloc a = Upload(kRampBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    bool identity = true;
    // the game's ramp, then the player's brightness: a gamma over it
    brightness_ = app::Get().brightness;
    const double g = 100.0 / std::max(brightness_, 1u);
    auto curve = [&](uint8_t v) { return uint8_t(std::lround(255.0 * std::pow(v / 255.0, g))); };
    // the title's ramp, the identity until it sets one
    bool set = false;
    for (uint8_t v : ramp_) set |= v != 0;
    if (!set)
        for (int i = 0; i < 256; i++) ramp_[kRampRed + i] = ramp_[kRampGreen + i] = ramp_[kRampBlue + i] = uint8_t(i);
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = curve(ramp_[kRampBlue + i]);
        p[1] = curve(ramp_[kRampGreen + i]);
        p[2] = curve(ramp_[kRampRed + i]);
        p[3] = 255;
        identity &= p[2] == i && p[1] == i && p[0] == i;
    }
    gamma_ = !identity;
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{lut_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1, 1, kRampBytes};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

bool D3D12Backend::CopyBackBuffer(uint32_t bi) {
    ID3D12Resource* bb = bb_[bi].Get();
    D3D12_RESOURCE_DESC d = bb->GetDesc();
    UINT64 bytes = 0;
    dev_->GetCopyableFootprints(&d, 0, 1, 0, &shot_fp_, nullptr, nullptr, &bytes);
    if (!shot_rb_ || shot_rb_->GetDesc().Width < bytes) {
        shot_rb_.Reset();
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&shot_rb_))))
            return false;
    }
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PRESENT;
    Transition(bb, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{shot_rb_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = shot_fp_;
    D3D12_TEXTURE_COPY_LOCATION src{bb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(bb, st, D3D12_RESOURCE_STATE_PRESENT);
    return true;
}

// --- video recording --------------------------------------------------------------------------------------------------

void D3D12Backend::RecordStop() {
    for (RecSlot& s : rec_slots_)
        if (s.pending) RecordFlush(s);
    if (rec_thread_.joinable()) {
        {
            std::lock_guard l(rec_mu_);
            rec_stop_ = true;
        }
        rec_cv_.notify_all();
        rec_thread_.join();
    }
    if (rec_pipe_) _pclose(rec_pipe_), LOGR("record: %ls closed, %lld frames", rec_path_.c_str(), (long long)rec_frames_);
    rec_pipe_ = nullptr;
    audio::SetTap(nullptr);
    {
        std::lock_guard l(rec_wav_mu_);
        if (rec_wav_) {
            // the RIFF and data sizes
            uint32_t riff = uint32_t(std::min<uint64_t>(rec_wav_bytes_ + 36, 0xFFFFFFFFu));
            uint32_t data = uint32_t(std::min<uint64_t>(rec_wav_bytes_, 0xFFFFFFFFu - 36));
            std::fseek(rec_wav_, 4, SEEK_SET), std::fwrite(&riff, 4, 1, rec_wav_);
            std::fseek(rec_wav_, 40, SEEK_SET), std::fwrite(&data, 4, 1, rec_wav_);
            std::fclose(rec_wav_);
            rec_wav_ = nullptr;
        }
    }
    rec_queue_.clear();
    rec_stop_ = false;
    rec_w_ = rec_h_ = 0;
}

void D3D12Backend::RecordFrame(uint32_t bi) {
    rec_copied_ = false;
    std::wstring path;
    if (control::TakeRecord(path)) {
        RecordStop();
        rec_path_ = path;
    }
    if (rec_path_.empty()) return;
    // at most so many frames a second (dropped frames lower the rate only)
    static const double fps = [] {
        const char* e = std::getenv("XBE_RECORD_FPS");
        return e ? std::atof(e) : 60.0;
    }();
    auto now = std::chrono::steady_clock::now();
    int64_t at = 0;
    if (rec_pipe_) {
        at = int64_t(std::chrono::duration<double>(now - rec_start_).count() * fps);
        if (at < rec_due_) return;  // (this frame of the rate taken already)
    }
    rec_last_ = now;
    ID3D12Resource* bb = bb_[bi].Get();
    D3D12_RESOURCE_DESC d = bb->GetDesc();
    if (!rec_pipe_) {
        // ffmpeg reads raw BGRA frames from its input
        const char* ff = std::getenv("XBE_FFMPEG");
        std::string exe = ff && *ff ? ff : "ffmpeg";
        int n = WideCharToMultiByte(CP_UTF8, 0, rec_path_.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string out(size_t(std::max(n, 1)) - 1, ' ');
        WideCharToMultiByte(CP_UTF8, 0, rec_path_.c_str(), -1, out.data(), n, nullptr, nullptr);
        char cmd[2048];
        std::snprintf(cmd, sizeof(cmd),
                      "\"\"%s\" -hide_banner -loglevel error -y -f rawvideo -pix_fmt bgra -s %llux%u -framerate %g -i - "
                      "-c:v hevc_nvenc -preset p6 -tune hq -rc vbr -cq 16 -b:v 0 -pix_fmt yuv420p "
                      "-movflags frag_keyframe+empty_moov \"%s\"\"",
                      exe.c_str(), (unsigned long long)d.Width, d.Height, fps, out.c_str());
        rec_pipe_ = _popen(cmd, "wb");
        if (!rec_pipe_) {
            LOGE("record: ffmpeg did not start (%s)", exe.c_str());
            rec_path_.clear();
            return;
        }
        rec_w_ = uint32_t(d.Width), rec_h_ = d.Height;
        rec_start_ = now, rec_due_ = 0, rec_frames_ = 0;
        {
            std::filesystem::path wav(rec_path_);
            wav.replace_extension(L".wav");
            std::lock_guard l(rec_wav_mu_);
            rec_wav_ = _wfopen(wav.c_str(), L"wb");
            rec_wav_bytes_ = 0;
            if (rec_wav_) {
                // RIFF WAVE, IEEE float stereo at the mixer's rate
                uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 3, 0, 2, 0};
                auto put = [&](int o, uint32_t v) { std::memcpy(h + o, &v, 4); };
                put(24, audio::kRate), put(28, audio::kRate * 8);
                h[32] = 8, h[34] = 32;
                std::memcpy(h + 36, "data", 4);
                std::fwrite(h, 1, 44, rec_wav_);
            }
        }
        audio::SetTap([this](const float* mix, uint32_t frames) {
            std::lock_guard l(rec_wav_mu_);
            if (rec_wav_) rec_wav_bytes_ += std::fwrite(mix, 8, frames, rec_wav_) * 8;
        });
        rec_thread_ = std::thread([this] {
            for (;;) {
                std::pair<std::vector<uint8_t>, int64_t> f;
                {
                    std::unique_lock l(rec_mu_);
                    rec_cv_.wait(l, [this] { return rec_stop_ || !rec_queue_.empty(); });
                    if (rec_queue_.empty()) return;
                    f = std::move(rec_queue_.front());
                    rec_queue_.pop_front();
                }
                for (int64_t i = 0; i < f.second; i++) std::fwrite(f.first.data(), 1, f.first.size(), rec_pipe_);
            }
        });
        LOGR("record: %ls, %ux%u at %g fps", rec_path_.c_str(), rec_w_, rec_h_, fps);
    }
    if (uint32_t(d.Width) != rec_w_ || d.Height != rec_h_) return;  // (resized: frames of another size skipped)
    RecSlot& s = rec_slots_[rec_next_];
    if (s.pending) RecordFlush(s);
    s.at = at;
    rec_due_ = at + 1;
    UINT64 bytes = 0;
    dev_->GetCopyableFootprints(&d, 0, 1, 0, &s.fp, nullptr, nullptr, &bytes);
    if (!s.rb || s.rb->GetDesc().Width < bytes) {
        s.rb.Reset();
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.rb))))
            return;
    }
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PRESENT;
    Transition(bb, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{s.rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = s.fp;
    D3D12_TEXTURE_COPY_LOCATION src{bb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(bb, st, D3D12_RESOURCE_STATE_PRESENT);
    rec_copied_ = true;
}

void D3D12Backend::RecordAfterSubmit() {
    if (!rec_copied_) return;
    RecSlot& s = rec_slots_[rec_next_];
    s.fence = fence_value_;
    s.pending = true;
    rec_next_ ^= 1;
    // the other slot's copy, from the frame before, is usually done by now
    RecSlot& o = rec_slots_[rec_next_];
    if (o.pending && fence_->GetCompletedValue() >= o.fence) RecordFlush(o);
}

void D3D12Backend::RecordFlush(RecSlot& s) {
    s.pending = false;
    if (fence_->GetCompletedValue() < s.fence) WaitFence(s.fence);
    uint32_t w = s.fp.Footprint.Width, h = s.fp.Footprint.Height, pitch = s.fp.Footprint.RowPitch;
    uint8_t* p = nullptr;
    D3D12_RANGE range{0, SIZE_T(s.fp.Offset) + SIZE_T(pitch) * h};
    if (FAILED(s.rb->Map(0, &range, reinterpret_cast<void**>(&p)))) return;
    std::vector<uint8_t> f(size_t(w) * h * 4);
    const bool rgba = s.fp.Footprint.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* src = p + s.fp.Offset + size_t(y) * pitch;
        uint8_t* dst = f.data() + size_t(y) * w * 4;
        if (!rgba) {
            std::memcpy(dst, src, size_t(w) * 4);
        } else {
            for (uint32_t x = 0; x < w; x++)
                dst[x * 4] = src[x * 4 + 2], dst[x * 4 + 1] = src[x * 4 + 1], dst[x * 4 + 2] = src[x * 4], dst[x * 4 + 3] = 255;
        }
    }
    D3D12_RANGE none{0, 0};
    s.rb->Unmap(0, &none);
    {
        // written for every frame of the rate up to its own (those it stands for); ffmpeg behind: the last one
        // queued stands for them as well
        std::lock_guard l(rec_mu_);
        int64_t times = s.at + 1 - rec_frames_;
        if (times > 0) {
            if (rec_queue_.size() < 8) rec_queue_.push_back({std::move(f), times});
            else rec_queue_.back().second += times;
            rec_frames_ += times;
        }
    }
    rec_cv_.notify_one();
}

// 24-bit BMP of the copied back buffer (BGRA rows), after the copy has finished on the GPU
void D3D12Backend::SaveBackBufferCopy(const std::wstring& path) {
    WaitFence(fence_value_);
    uint32_t w = shot_fp_.Footprint.Width, h = shot_fp_.Footprint.Height, pitch = shot_fp_.Footprint.RowPitch;
    bool rgba = shot_fp_.Footprint.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
    uint8_t* p = nullptr;
    D3D12_RANGE range{0, SIZE_T(pitch) * h};
    if (FAILED(shot_rb_->Map(0, &range, reinterpret_cast<void**>(&p)))) return;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f) {
        uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
        uint8_t hdr[54] = {'B', 'M'};
        auto put = [&](int at, uint32_t v) { std::memcpy(hdr + at, &v, 4); };
        put(2, size), put(10, 54), put(14, 40), put(18, w), put(22, h), put(34, row * h);
        hdr[26] = 1, hdr[28] = 24;
        std::fwrite(hdr, 1, 54, f);
        std::vector<uint8_t> line(row, 0);
        for (uint32_t y = h; y-- > 0;) {
            const uint8_t* src = p + shot_fp_.Offset + size_t(y) * pitch;
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t* s = src + x * 4;
                uint8_t* o = &line[x * 3];
                if (rgba) o[0] = s[2], o[1] = s[1], o[2] = s[0];
                else o[0] = s[0], o[1] = s[1], o[2] = s[2];
            }
            std::fwrite(line.data(), 1, row, f);
        }
        std::fclose(f);
    }
    D3D12_RANGE none{0, 0};
    shot_rb_->Unmap(0, &none);
}


// --- shadows (shadows.h) ---------------------------------------------------------------------------------------------

bool D3D12Backend::ShadowDesc(const ShadowKey& k, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d,
                              D3D12_INPUT_ELEMENT_DESC el[16]) const {
    auto vs = blobs_.find(k.vs);
    auto ps = k.ps ? blobs_.find(k.ps) : blobs_.end();
    if (vs == blobs_.end() || !vs->second || (k.ps && (ps == blobs_.end() || !ps->second))) return false;
    uint32_t n = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    }
    d = {};
    d.pRootSignature = root_.Get();
    d.InputLayout = {el, n};
    d.VS = {vs->second->GetBufferPointer(), vs->second->GetBufferSize()};
    if (k.ps) d.PS = {ps->second->GetBufferPointer(), ps->second->GetBufferSize()};
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // both faces: thin walls and open models cast from either side; the receivers keep off their own surfaces
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.SlopeScaledDepthBias = 1.2f;
    d.RasterizerState.DepthBiasClamp = 0.002f;
    // casters between the light and a cascade's near end are flattened onto it
    d.RasterizerState.DepthClipEnable = FALSE;
    d.DepthStencilState.DepthEnable = TRUE;
    d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    d.PrimitiveTopologyType = TopologyType(k.topo & 0xFF);
    d.NumRenderTargets = 0;
    d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    d.SampleDesc.Count = 1;
    if ((k.topo & kShadowScreenKey) && (k.topo & kTessKey) && tess_hs_ && tess_ds_) {
        d.HS = {tess_hs_->GetBufferPointer(), tess_hs_->GetBufferSize()};
        d.DS = {tess_ds_->GetBufferPointer(), tess_ds_->GetBufferSize()};
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    }
    if (k.topo & kVeilKey) {
        // the veil: the draw's coverage over the occlusion (R8), lifting it towards 1 by as much (the source's
        // value times what is left: o + a (1 - o)), through the scene's depth, not written
        d.RasterizerState.SlopeScaledDepthBias = 0;
        d.RasterizerState.DepthBiasClamp = 0;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_INV_DEST_COLOR, b.DestBlend = D3D12_BLEND_ONE, b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE, b.DestBlendAlpha = D3D12_BLEND_ZERO, b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
        d.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    }
    if (k.topo & kGuardKey) {
        // the guard band: the screen's depth, as drawn (no offset, clipped at the near plane)
        d.RasterizerState.SlopeScaledDepthBias = 0;
        d.RasterizerState.DepthBiasClamp = 0;
        d.RasterizerState.DepthClipEnable = TRUE;
    }
    if (k.topo & kTopKey) {
        // seen from above onto the wave field: the highest height of each texel kept (the water's, or what crosses
        // it), no depth
        d.RasterizerState.SlopeScaledDepthBias = 0;
        d.RasterizerState.DepthBiasClamp = 0;
        d.RasterizerState.DepthClipEnable = FALSE;
        d.DepthStencilState.DepthEnable = FALSE;
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DSVFormat = DXGI_FORMAT_UNKNOWN;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = k.topo & kTopWaterKey ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16_FLOAT;
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_ONE, b.DestBlend = D3D12_BLEND_ONE, b.BlendOp = D3D12_BLEND_OP_MAX;
        b.SrcBlendAlpha = D3D12_BLEND_ONE, b.DestBlendAlpha = D3D12_BLEND_ONE, b.BlendOpAlpha = D3D12_BLEND_OP_MAX;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
    }
    if (k.topo & kShadowScreenKey) {
        // the characters' screen mask: the draw's own shader where its pixels stayed in the scene's depth
        d.RasterizerState.SlopeScaledDepthBias = 0;
        d.RasterizerState.DepthBiasClamp = 0;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    }
    return true;
}

ID3D12PipelineState* D3D12Backend::EnsureShadowPipeline(const ShadowKey& k) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = shadow_psos_.find(h);
    if (it != shadow_psos_.end()) return it->second.Get();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
    D3D12_INPUT_ELEMENT_DESC el[16];
    if (!ShadowDesc(k, d, el)) {
        shadow_psos_[h] = nullptr;
        return nullptr;
    }
    ComPtr<ID3D12PipelineState> pso;
    wchar_t name[24];
    swprintf(name, 24, L"s%015llx", (unsigned long long)(h >> 4));
    bool loaded = library_ && SUCCEEDED(library_->LoadGraphicsPipeline(name, &d, IID_PPV_ARGS(&pso)));
    if (!loaded) {
        g_perf.psos++;
        if (HRESULT hr = dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)); FAILED(hr))
            LOGE("d3d12: shadow pipeline creation failed (%08X, topo %X, ps %d)", unsigned(hr), k.topo, k.ps ? 1 : 0);
        else if (library_ && SUCCEEDED(library_->StorePipeline(name, pso.Get()))) library_dirty_ = true;
    }
    shadow_psos_[h] = pso;
    if (pso && known_shadow_.insert(h).second) {
        std::ofstream out(CacheDir() / "shadows2.bin", std::ios::binary | std::ios::app);
        uint64_t ids[2] = {StableOf(k.vs), k.ps ? StableOf(k.ps) : 0};
        out.write(reinterpret_cast<const char*>(&k), sizeof(k));
        out.write(reinterpret_cast<const char*>(ids), 16);
    }
    return pso.Get();
}

// the light the game gave the draw, if it is one the shadows can take: a static model's, carried to world space by
// its world matrix (its world-view-projection times the camera's inverse), or a fixed function draw's D3D lights
void D3D12Backend::GatherLights(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    if (s.layout2d || a.pretransformed || !bound_rt_ || !bound_ds_ || bound_rt_->gw != guest_w_ || bound_rt_->gh != guest_h_ ||
        occlusion_done_)
        return;
    // the camera: the transforms the game keeps for its fixed function draws, a perspective one's
    const float* V = s.transforms[kTsView].data();
    const float* Pm = s.transforms[kTsProjection].data();
    if (Pm[11] > 0.99f && Pm[11] < 1.01f && std::fabs(Pm[15]) < 1e-6f && Pm[0] != 0 && Pm[5] != 0) {
        std::memcpy(cam_view_, V, sizeof(cam_view_));
        std::memcpy(cam_proj_, Pm, sizeof(cam_proj_));
        cam_min_z_ = s.viewport.min_z, cam_max_z_ = s.viewport.max_z;
        cam_ok_ = true;
    }
    auto lum = [](const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
    if (sh.prog && pk.lit) {
        auto it = prog_light_.find(sh.vkey);
        if (it == prog_light_.end()) it = prog_light_.emplace(sh.vkey, ProgramLight(s.vs->program, a.regs)).first;
        const ProgramLightInfo& pl = it->second;
        static const uint64_t vsc_of = [] {
            const char* e = std::getenv("XBE_VSC_DUMP");
            return e ? std::strtoull(e, nullptr, 16) : 0ull;
        }();
        if (g_log_draws && vsc_of && draw_tex0_ == vsc_of) {
            for (int r = 0; r < 192; r++) {
                const auto& v = s.vsc[size_t(r)];
                if (v[0] != 0 || v[1] != 0 || v[2] != 0 || v[3] != 0) LOGR("draw:   vsc c%d %.4f %.4f %.4f %.4f", r, v[0], v[1], v[2], v[3]);
            }
        }
        if (g_log_draws && pl.ok) {
            const auto &col = s.vsc[pl.color], &amb = s.vsc[pl.ambient], &dir = s.vsc[pl.dir];
            LOGR("draw:   light tex %016llx colour %.2f %.2f %.2f ambient %.2f %.2f %.2f dir %.2f %.2f %.2f", (unsigned long long)draw_tex0_,
                 col[0], col[1], col[2], amb[0], amb[1], amb[2], dir[0], dir[1], dir[2]);
        }
        if (!pl.ok || !cam_ok_) return;
        if (!inv_vp_ok_ || std::memcmp(inv_vp_of_, V, 64) || std::memcmp(inv_vp_of_ + 16, Pm, 64)) {
            float vp[16];
            shadow::Mul(V, Pm, vp);
            inv_vp_ok_ = shadow::Invert(vp, inv_vp_);
            std::memcpy(inv_vp_of_, V, 64), std::memcpy(inv_vp_of_ + 16, Pm, 64);
            if (!inv_vp_ok_) return;
        }
        // the model's world matrix: its clip matrix (rows of dp4s, so columns here) times the camera's inverse
        float A[16], W[16];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) A[i * 4 + j] = s.vsc[pl.pos[j]][size_t(i)];
        shadow::Mul(A, inv_vp_, W);
        const auto& L = s.vsc[pl.dir];
        shadow::Candidate c;
        for (int j = 0; j < 3; j++) c.dir[j] = L[0] * W[j] + L[1] * W[4 + j] + L[2] * W[8 + j];
        // a rigid placement: anything else is not a model's world matrix
        float sx = W[0] * W[0] + W[1] * W[1] + W[2] * W[2], sy = W[4] * W[4] + W[5] * W[5] + W[6] * W[6];
        if (!(sx > 1e-4f) || std::fabs(sx / sy - 1) > 0.1f || std::fabs(W[3]) + std::fabs(W[7]) + std::fabs(W[11]) > 1e-2f) return;
        c.color = lum(s.vsc[pl.color].data());
        c.ambient = lum(s.vsc[pl.ambient].data());
        c.source = shadow::kStatic;
        shadow::Lights().Add(c);
    } else if (sh.fixed_transform && (sh.fk.light & 1)) {
        float amb[4];
        Color(s.rs[kRsAmbient], amb);
        for (int i = 0; i < 8; i++) {
            if (((sh.fk.types >> (i * 2)) & 3) != 3) continue;  // D3DLIGHT_DIRECTIONAL
            float f[kLightFloats];
            std::memcpy(f, s.lights[size_t(i)].data(), sizeof(f));
            shadow::Candidate c;
            for (int j = 0; j < 3; j++) c.dir[j] = -f[kLightDirection + j];
            c.color = lum(f + kLightDiffuse);
            c.ambient = lum(amb);
            c.source = shadow::kFixed;
            shadow::Lights().Add(c);
        }
    }
}

// once a frame: the light, and the shadows' settings for the frame
void D3D12Backend::ResolveLight() {
    if (light_resolved_) return;
    light_resolved_ = true;
    static const auto t0 = std::chrono::steady_clock::now();
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    light_ = shadow::Lights().Resolve(now, g_level.load());
    MaterialFrame mf;
    {
        std::lock_guard lock(g_material_frame_lock);
        mf = g_material_frame;
    }
    shadow_q_ = shadow::QualityFor(app::Get().shadow_quality, mf.units);
}

// A draw for the screen's velocity (MotionPass): the 3D's opaque and cut out draws before the weapon in hand, matched
// to the frame before's by what they draw (their shader, vertices, texture) and their order among the alike
void D3D12Backend::RecordMotion(const State& s, const Assembled& a, const DrawShaders& sh, const PsoKey& k) {
    const app::Settings& set = app::Get();
    if (!(set.taa || set.motion_blur) || s.layout2d || a.pretransformed || sh.quad) return;
    if (!bound_rt_ || bound_rt_ != scene_rt_ || bound_ds_ != scene_ds_ || a.topology != Topology::Triangles) return;
    // what is drawn into the depth (solid, cut out), and what is seen through (smoke, sparks, fire, tracers: over
    // what is behind them, flagged)
    const bool over = !(k.zwrite && (!k.blend || SimpleState(s, kMAlphaTestEnable)));
    if (!k.zen && !over) return;
    uint64_t ident[6] = {sh.vkey, uint64_t(reinterpret_cast<uintptr_t>(a.buffer_count ? a.buffers[0].src : nullptr)),
                         uint64_t(a.vertex_count) << 32 | a.index_count, uint64_t(uint32_t(a.base_vertex)) << 32 | a.draw_count,
                         draw_tex0_, 0};
    uint64_t base = Hash(ident, sizeof(ident));
    uint32_t order = motion_order_.Get(base)++;
    uint64_t key = Hash(&base, 8, order);
    motion_cur_.Get(key) = {last_vc_, last_dc_, ring_gen_};
    const MotionSeen* prev = motion_prev_.Find(key);
    // new this frame (or its constants rewritten by a flush since): the camera's motion alone
    if (!prev || prev->gen != motion_prev_gen_) return;
    // one of many alike (shell casings, debris): their order changes between frames, matched by it they swap
    // motions and smear; the camera's motion alone
    const uint32_t* many = motion_counts_prev_.Find(base);
    if (order > 0 || (many && *many > 1)) return;
    if (motion_.empty()) motion_gen_ = ring_gen_;
    if (motion_gen_ != ring_gen_ || motion_.size() >= 8192) return;
    // its motion vertex shader and pipeline
    auto vit = motion_vs_.find(sh.vkey);
    if (vit == motion_vs_.end()) {
        std::string src = MotionVertexHlsl(VertexHlsl(s, a, sh), unsigned(offsetof(DrawConstants, wvp) / 16));
        uint64_t h = 0;
        if (src.empty() || !Compile(src, "motion", "vs_5_0", h)) h = 0;
        vit = motion_vs_.emplace(sh.vkey, h).first;
    }
    if (!vit->second) return;
    if (!motion_root_) {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER params[6] = {};
        for (int i = 0; i < 5; i++) params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV, params[i].Descriptor.ShaderRegister = UINT(i);
        params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[5].DescriptorTable.NumDescriptorRanges = 1;
        params[5].DescriptorTable.pDescriptorRanges = &range;
        params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 6;
        rsd.pParameters = params;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> rs, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err)) ||
            FAILED(dev_->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&motion_root_))))
            return;
        std::string ps = std::string("cbuffer MV : register(b4) {") + kMotionFns + kMotionPsMain;
        if (!Compile(ps, "main", "ps_5_0", motion_ps_)) motion_ps_ = 0;
        if (!Compile("#define OVER 1\n" + ps, "main", "ps_5_0", motion_ps_over_)) motion_ps_over_ = 0;
    }
    if (!motion_ps_ || (over && !motion_ps_over_)) return;
    struct {
        uint64_t vs;
        uint32_t regs, topo;
        uint32_t elements[16];
    } mk{};
    mk.vs = vit->second, mk.regs = k.regs, mk.topo = (k.topo & 0xFF) | (over ? 0x10000u : 0u);
    std::memcpy(mk.elements, k.elements, sizeof(mk.elements));
    uint64_t ph = Hash(&mk, sizeof(mk));
    auto pit = motion_psos_.find(ph);
    if (pit == motion_psos_.end()) {
        ComPtr<ID3D12PipelineState> pso;
        auto vs = blobs_.find(mk.vs);
        auto ps = blobs_.find(over ? motion_ps_over_ : motion_ps_);
        if (vs != blobs_.end() && vs->second && ps != blobs_.end() && ps->second) {
            D3D12_INPUT_ELEMENT_DESC el[16];
            uint32_t n = 0;
            for (uint32_t r = 0; r < 16; r++) {
                if (!((mk.regs >> r) & 1)) continue;
                uint32_t e = mk.elements[r];
                el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
            }
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
            d.pRootSignature = motion_root_.Get();
            d.InputLayout = {el, n};
            d.VS = {vs->second->GetBufferPointer(), vs->second->GetBufferSize()};
            d.PS = {ps->second->GetBufferPointer(), ps->second->GetBufferSize()};
            d.SampleMask = UINT_MAX;
            d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            d.RasterizerState.DepthClipEnable = TRUE;
            d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            d.PrimitiveTopologyType = TopologyType(mk.topo & 0xFF);
            d.NumRenderTargets = 1;
            d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.SampleDesc.Count = 1;
            g_perf.psos++;
            if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) LOGE("d3d12: motion pipeline creation failed");
        }
        pit = motion_psos_.emplace(ph, pso).first;
    }
    if (!pit->second) return;
    MotionDraw m{};
    m.pso = pit->second.Get();
    m.vb_count = a.buffer_count;
    std::memcpy(m.vb, last_vbv_, sizeof(D3D12_VERTEX_BUFFER_VIEW) * a.buffer_count);
    m.indexed = a.index_count != 0;
    m.ib = last_ibv_;
    m.count = a.index_count ? a.index_count : a.draw_count;
    m.base = a.base_vertex;
    m.topo = PrimitiveTopology(a);
    if (tess_draw_) {
        m.indexed = true;
        m.count = uint32_t(tess_list_.size());
        m.base = a.index_count ? a.base_vertex : 0;
        m.topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
    m.vc = last_vc_, m.dc = last_dc_, m.pvc = prev->vc, m.pdc = prev->dc;
    m.weapon = occlusion_done_;
    motion_.push_back(m);
}

// the velocity passes' parameters (kMotionFns): the projection, the viewport's depth and the size, the view now to the
// clip space of the frame before, the jitter
void D3D12Backend::MotionParams(float out[28]) {
    float mv0[4] = {1 / cam_proj_[0], 1 / cam_proj_[5], cam_proj_[10], cam_proj_[14]};
    float mv1[4] = {cam_min_z_, cam_max_z_ - cam_min_z_, float(scene_w_), float(scene_h_)};
    const float* pv = prev_cam_ok_ ? prev_view_ : cam_view_;
    const float* pp = prev_cam_ok_ ? prev_proj_ : cam_proj_;
    float inv[16], vp[16], cam[16];
    if (!shadow::Invert(cam_view_, inv)) std::memcpy(inv, cam_view_, sizeof(inv));
    shadow::Mul(pv, pp, vp);
    shadow::Mul(inv, vp, cam);
    float mv2[4] = {taa_jit_[0], taa_jit_[1], 0, 0};
    std::memcpy(out, mv0, 16), std::memcpy(out + 4, mv1, 16), std::memcpy(out + 8, cam, 64), std::memcpy(out + 24, mv2, 16);
}

// The screen's velocity into scene_[kSceneVel]: the camera's motion everywhere from the world's depth, then the
// recorded draws' own (this frame's constants and the frame before's) where the world's depth has them
void D3D12Backend::MotionPass(Target& rt) {
    if (!scene_ds_ || !scene_[kSceneVel]) return;
    Target& ds = *scene_ds_;
    constexpr DXGI_FORMAT kF16 = DXGI_FORMAT_R16G16B16A16_FLOAT, kDepth = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    ID3D12Resource* depth = world_depth_saved_ ? world_depth_.Get() : ds.res.Get();
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (world_depth_saved_) Transition(world_depth_.Get(), world_depth_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    float mp[28];
    MotionParams(mp);
    // the camera's
    std::string text = std::string(kPassVs) + "cbuffer MV : register(b1) {" + kMotionFns + kCamVelMain;
    ID3D12PipelineState* pso = EnsurePassPipeline(text.c_str(), "camvel", kF16, -1);
    if (!pso) {
        Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        return;
    }
    uint32_t slot = AllocSrvSlots(7);
    for (uint32_t i = 0; i < 7; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i == 0 ? kDepth : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i == 0 ? depth : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Transition(scene_[kSceneVel].Get(), scene_state_[kSceneVel], D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = SceneRtv(kSceneVel);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(rt.w, rt.h);
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, mp, sizeof(mp));
    DrawFullScreen(pso, cb.gpu, slot);
    // the objects' own: the world's against its depth, the weapon in hand's against the depth now (its own)
    if (!motion_.empty() && motion_gen_ == ring_gen_ && motion_root_) {
        uint32_t wslot = AllocSrvSlots(1);
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = kDepth;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            dev_->CreateShaderResourceView(ds.res.Get(), &sv, Cpu(srv_heap_.Get(), wslot, srv_inc_));
        }
        cl_->SetGraphicsRootSignature(motion_root_.Get());
        cmd_.smp_ok = false;  // (the root's arguments go with its signature)
        cl_->SetGraphicsRootConstantBufferView(4, cb.gpu);
        ID3D12PipelineState* bound = nullptr;
        int table = -1;
        for (const MotionDraw& m : motion_) {
            int want = m.weapon ? 1 : 0;
            if (want != table) {
                table = want;
                cl_->SetGraphicsRootDescriptorTable(5, Gpu(srv_heap_.Get(), want ? wslot : slot, srv_inc_));
            }
            if (m.pso != bound) SetPso(bound = m.pso);
            cl_->SetGraphicsRootConstantBufferView(0, m.vc);
            cl_->SetGraphicsRootConstantBufferView(1, m.dc);
            cl_->SetGraphicsRootConstantBufferView(2, m.pvc);
            cl_->SetGraphicsRootConstantBufferView(3, m.pdc);
            cl_->IASetVertexBuffers(0, m.vb_count, m.vb);
            SetTopology(m.topo);
            if (m.indexed) {
                cl_->IASetIndexBuffer(&m.ib);
                cl_->DrawIndexedInstanced(m.count, 1, 0, m.base, 0);
            } else {
                cl_->DrawInstanced(m.count, 1, 0, 0);
            }
        }
        cl_->SetGraphicsRootSignature(root_.Get());
        cl_->SetGraphicsRootDescriptorTable(4, sun_table_);
        cmd_.smp_ok = false;
    }
    Transition(scene_[kSceneVel].Get(), scene_state_[kSceneVel], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    bound_rt_ = bound_ds_ = nullptr;
    motion_ok_ = true;
}

// Per object motion blur on the scene's target (kMotionBlurHlsl)
void D3D12Backend::MotionBlurPass(Target& rt) {
    if (!scene_ds_) return;
    std::string text = std::string(kPassVs) + kMotionBlurHlsl;
    ID3D12PipelineState* pso = EnsurePassPipeline(text.c_str(), "mblur", rt.fmt, -1);
    if (!pso) return;
    Target& ds = *scene_ds_;
    CopyScene(rt);
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    std::pair<ID3D12Resource*, DXGI_FORMAT> src[3] = {{scene_[0].Get(), rt.fmt},
                                                      {scene_[kSceneVel].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
                                                      {ds.res.Get(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS}};
    uint32_t slot = AllocSrvSlots(7);
    for (uint32_t i = 0; i < 7; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 3 ? src[i].second : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 3 ? src[i].first : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), rt.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(rt.w, rt.h);
    Alloc cb = Upload(256, 256);
    static const bool vel_debug = std::getenv("XBE_VELOCITY_DEBUG") != nullptr;
    float mb[4] = {float(rt.w), float(rt.h), vel_debug ? 2.0f : 1.0f, world_depth_saved_ ? 1.0f : 0.0f};
    std::memcpy(cb.cpu, mb, sizeof(mb));
    DrawFullScreen(pso, cb.gpu, slot);
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    bound_rt_ = bound_ds_ = nullptr;
}

// Temporal anti-aliasing on the scene's target (kTaaHlsl), before the 2D; its result kept as the next frame's history
void D3D12Backend::TaaPass(Target& rt) {
    if (!scene_ds_ || !scene_[kSceneHist] || !motion_ok_) {
        taa_hist_ok_ = false;
        return;
    }
    std::string text = std::string(kPassVs) + kTaaHlsl;
    ID3D12PipelineState* pso = EnsurePassPipeline(text.c_str(), "taa", rt.fmt, -1);
    if (!pso) return;
    // a cut (the camera far from where it was, or turned away), a new level: the history starts over
    bool reset = !taa_hist_ok_ || !prev_cam_ok_ || g_level.load() != taa_level_;
    taa_level_ = g_level.load();
    if (!reset) {
        float a[16], b[16];
        if (shadow::Invert(cam_view_, a) && shadow::Invert(prev_view_, b)) {
            float d[3] = {a[12] - b[12], a[13] - b[13], a[14] - b[14]};
            float fwd = a[8] * b[8] + a[9] * b[9] + a[10] * b[10];
            reset = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 300.0f * 300.0f || fwd < 0.9f;
        }
    }
    Target& ds = *scene_ds_;
    CopyScene(rt);
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(scene_[kSceneHist].Get(), scene_state_[kSceneHist], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    std::pair<ID3D12Resource*, DXGI_FORMAT> src[4] = {{scene_[0].Get(), rt.fmt},
                                                      {scene_[kSceneHist].Get(), rt.fmt},
                                                      {scene_[kSceneVel].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
                                                      {ds.res.Get(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS}};
    uint32_t slot = AllocSrvSlots(7);
    for (uint32_t i = 0; i < 7; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 4 ? src[i].second : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 4 ? src[i].first : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), rt.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(rt.w, rt.h);
    Alloc cb = Upload(256, 256);
    float tp[8] = {float(rt.w), float(rt.h), 1.0f / rt.w, 1.0f / rt.h, reset ? 1.0f : 0.0f, world_depth_saved_ ? 1.0f : 0.0f, 0, 0};
    std::memcpy(cb.cpu, tp, sizeof(tp));
    DrawFullScreen(pso, cb.gpu, slot);
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    // the history: this frame's result
    Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(scene_[kSceneHist].Get(), scene_state_[kSceneHist], D3D12_RESOURCE_STATE_COPY_DEST);
    cl_->CopyResource(scene_[kSceneHist].Get(), rt.res.Get());
    Transition(scene_[kSceneHist].Get(), scene_state_[kSceneHist], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    taa_hist_ok_ = true;
    bound_rt_ = bound_ds_ = nullptr;
    // the film grain, past the history
    if (app::Get().film_grain) {
        std::string gt = std::string(kPassVs) + kGrainHlsl;
        ID3D12PipelineState* gp = EnsurePassPipeline(gt.c_str(), "grain", rt.fmt, -1);
        if (!gp) return;
        CopyScene(rt);
        uint32_t gs = AllocSrvSlots(7);
        for (uint32_t i = 0; i < 7; i++) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = i == 0 ? rt.fmt : kBgra;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            dev_->CreateShaderResourceView(i == 0 ? scene_[0].Get() : nullptr, &sv, Cpu(srv_heap_.Get(), gs + i, srv_inc_));
        }
        Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE grh = Cpu(rtv_heap_.Get(), rt.view, rtv_inc_);
        cl_->OMSetRenderTargets(1, &grh, FALSE, nullptr);
        SetFullViewport(rt.w, rt.h);
        static const auto t0 = std::chrono::steady_clock::now();
        float g[4] = {0.012f, std::fmod(std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count(), 1000.0f), 0, 0};
        Alloc gcb = Upload(256, 256);
        std::memcpy(gcb.cpu, g, sizeof(g));
        DrawFullScreen(gp, gcb.gpu, gs);
        bound_rt_ = bound_ds_ = nullptr;
    }
}

void D3D12Backend::RecordCaster(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk, const PsoKey& k,
                                const DrawTextures& stages) {
    // the 3D of the screen with a depth write, solid or cut out by its alpha test, before the occlusion (the weapon
    // in hand comes after); not decals (a depth offset), not screen passes
    if (app::Get().shadow_quality == 0 || occlusion_done_ || s.layout2d || a.pretransformed || sh.quad) return;
    if (!bound_rt_ || !bound_ds_ || bound_rt_->gw != guest_w_ || bound_rt_->gh != guest_h_) return;
    if (!k.zen || !k.zwrite || a.topology != Topology::Triangles || k.zbias != 0 || k.fill == kFillWireframe) return;
    const auto& v = s.viewport;
    GuestExtent size;
    SurfaceSize(s.rt, size.w, size.h);
    if (v.x != 0 || v.y != 0 || v.w != size.w || v.h != size.h) return;
    bool alpha_test = SimpleState(s, kMAlphaTestEnable) != 0 || (pk.alphakill & 1);
    if (k.blend && !alpha_test) return;
    // nor the effects' sprites (the dust, smoke and sparks of a shot's impact, tracers, flashes: the game's table,
    // g_emitter) nor what is added as light: cut out by their alpha test they threw dark, blocky blots on the ground
    {
        constexpr uint32_t kOne = 0x1;
        if (draw_emitter_) return;
        if (k.blend && k.dst == kOne) return;
    }
    bool alpha = alpha_test && stages.info[0].kind == kTex2D;
    if (alpha_test && !alpha) return;
    if (casters_.empty()) casters_gen_ = ring_gen_;
    if (casters_gen_ != ring_gen_ || casters_.size() >= 8192) return;
    // the caster variant of the draw's vertex shader
    auto vit = shadow_vs_.find(sh.vkey);
    if (vit == shadow_vs_.end()) {
        std::string src = ShadowVertexHlsl(VertexHlsl(s, a, sh));
        uint64_t h = 0;
        if (src.empty() || !Compile(src, "main", "vs_5_0", h)) h = 0;
        if (h) RememberVertexRecipe(s, a, sh, kVariantShadow, h);
        vit = shadow_vs_.emplace(sh.vkey, h).first;
    }
    if (!vit->second) return;
    ShadowKey key{};
    key.vs = vit->second;
    if (alpha) {
        bool proj = (pk.proj & 1) != 0;
        key.ps = shadow_alpha_ps_[proj];
        if (!key.ps) return;
    }
    key.regs = k.regs, key.topo = k.topo & 0xFF;  // the primitive type alone (not the draw's tessellation or targets)
    std::memcpy(key.elements, k.elements, sizeof(key.elements));
    ID3D12PipelineState* pso = EnsureShadowPipeline(key);
    if (!pso) return;
    Caster c{};
    c.pso = pso;
    c.vb_count = a.buffer_count;
    std::memcpy(c.vb, last_vbv_, sizeof(D3D12_VERTEX_BUFFER_VIEW) * a.buffer_count);
    c.indexed = a.index_count != 0;
    c.ib = last_ibv_;
    c.count = a.index_count ? a.index_count : a.draw_count;
    c.base = a.base_vertex;
    c.topo = PrimitiveTopology(a);
    c.tess = false;
    if (tess_draw_) {
        // drawn as patches from the list its strip became (DrawGeometry): the shadow from that list's triangles, the
        // screen mask tessellated as the character was
        c.indexed = true;
        c.count = uint32_t(tess_list_.size());
        c.base = a.index_count ? a.base_vertex : 0;
        c.topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        c.tess = true;
    }
    c.vc = last_vc_, c.dc = last_dc_, c.tail = last_tail_;
    c.srv = last_srv_, c.smp = last_smp_;
    c.alpha = alpha;
    // characters (skinned, lit by their own lights) and what they hold (lit fixed function): their pixels are marked
    // on the screen, where shadows leave out the model's own coarse facets
    bool character = (sh.prog && pk.lit && !(prog_light_.count(sh.vkey) && prog_light_[sh.vkey].ok)) ||
                     (sh.fixed_transform && (sh.fk.light & 1));
    if (character && !alpha) {
        ShadowKey mk = key;
        mk.vs = sh.vh, mk.ps = mask_ps_, mk.topo |= kShadowScreenKey;
        if (tess_draw_) mk.topo |= kTessKey;
        c.screen = EnsureShadowPipeline(mk);
    }
    // the guard band's variant
    {
        auto git = guard_vs_.find(sh.vkey);
        if (git == guard_vs_.end()) {
            std::string src = GuardVertexHlsl(VertexHlsl(s, a, sh));
            uint64_t h = 0;
            if (src.empty() || !Compile(src, "main", "vs_5_0", h)) h = 0;
            if (h) RememberVertexRecipe(s, a, sh, kVariantGuard, h);
            git = guard_vs_.emplace(sh.vkey, h).first;
        }
        if (git->second) {
            ShadowKey gk = key;
            gk.vs = git->second, gk.topo |= kGuardKey;
            c.guard = EnsureShadowPipeline(gk);
        }
    }
    // seen from above (Water Interaction): its cover of the water
    c.top = nullptr;
    bool water = stages.mat && stages.mat->m && stages.mat->m->water;  // (the water itself, where solid, crosses nothing)
    if (app::Get().water && app::Get().water_interaction && top_cover_ps_ && !water) {
        if (uint64_t tv = TopVertexShader(s, a, sh)) {
            ShadowKey tk = key;
            tk.vs = tv, tk.ps = top_cover_ps_, tk.topo |= kTopKey;
            c.top = EnsureShadowPipeline(tk);
        }
    }
    c.zmin = v.min_z, c.zmax = v.max_z;
    casters_.push_back(c);
    static const bool list = std::getenv("XBE_SHADOW_CASTERS") != nullptr;
    if (list && presents_ % 600 == 300)
        LOGR("shadows: caster %zu vs %08X prog %d count %u z %.3f..%.3f zfunc %X alpha %d blend %d tex0 %016llx", casters_.size(), s.vs_handle,
             sh.prog, c.count, v.min_z, v.max_z, k.zfunc, alpha, k.blend, (unsigned long long)stages.id[0]);
}

// the cascades fitted to the frame's camera and light, the casters drawn into their tiles
bool D3D12Backend::DrawShadowMap() {
    if (casters_.empty() || casters_gen_ != ring_gen_) return false;
    const shadow::Quality& q = shadow_q_;
    // the atlas: 2 x 2 tiles
    if (!shadow_atlas_ || shadow_tile_ != q.tile) {
        if (shadow_atlas_) Release(std::move(shadow_atlas_));
        if (shadow_dsv_ == ~0u) {
            if (free_dsv_.empty()) return false;
            shadow_dsv_ = free_dsv_.back();
            free_dsv_.pop_back();
        }
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_R32_TYPELESS, q.tile * 2, q.tile * 2);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1;
        shadow_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, shadow_state_, &cv, IID_PPV_ARGS(&shadow_atlas_))))
            return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(shadow_atlas_.Get(), &dv, Cpu(dsv_heap_.Get(), shadow_dsv_, dsv_inc_));
        shadow_tile_ = q.tile;
        atlas_valid_ = false;
        LOGR("shadows: atlas %u x %u, %d cascades", q.tile * 2, q.tile * 2, q.cascades);
    }
    // the camera and the cascades
    shadow::Camera cam{};
    if (!shadow::Invert(cam_view_, cam.view_to_world)) return false;
    cam.p00 = cam_proj_[0], cam.p11 = cam_proj_[5], cam.p22 = cam_proj_[10], cam.p32 = cam_proj_[14];
    cam.near_z = std::fabs(cam.p22) > 1e-6f ? -cam.p32 / cam.p22 : 1.0f;
    // Drawn again only when needed: at 144 times a second at most (a moving model's shadow is a frame late at
    // worst), at once when the view turned or moved, the light turned or the level or quality changed. Between,
    // the atlas stays with the cascades it was drawn for, which the mask reads from the camera of the moment.
    {
        static const auto t0 = std::chrono::steady_clock::now();
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const float* m = cam.view_to_world;
        auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        float moved[3] = {m[12] - atlas_eye_[0], m[13] - atlas_eye_[1], m[14] - atlas_eye_[2]};
        MaterialFrame mf;
        {
            std::lock_guard lock(g_material_frame_lock);
            mf = g_material_frame;
        }
        bool stale = !atlas_valid_ || now - atlas_time_ >= 1.0 / 144 || dot3(m + 8, atlas_ahead_) < 0.9945f ||
                     dot3(moved, moved) > (0.5f * mf.units) * (0.5f * mf.units) || dot3(light_.dir, atlas_light_) < 0.99996f ||
                     atlas_level_ != g_level.load() || atlas_quality_ != app::Get().shadow_quality;
        if (!stale) {
            ShadowTimestamp(0), ShadowTimestamp(1);  // nothing drawn: the averages count it
            shadows_drawn_ = true;
            return true;
        }
        atlas_valid_ = true, atlas_time_ = now, atlas_level_ = g_level.load(), atlas_quality_ = app::Get().shadow_quality;
        std::memcpy(atlas_eye_, m + 12, 12), std::memcpy(atlas_ahead_, m + 8, 12), std::memcpy(atlas_light_, light_.dir, 12);
    }
    shadow::Fit(cam, light_.dir, q, cascades_);
    static const bool shadow_stats = std::getenv("XBE_SHADOW_STATS") != nullptr;
    if (shadow_stats && presents_ % 600 == 0) {
        LOGR("shadows: camera P %.3f %.3f %.3f %.3f near %.1f depth %.3f..%.3f eye %.0f %.0f %.0f ahead %.2f %.2f %.2f", cam.p00, cam.p11,
             cam.p22, cam.p32, cam.near_z, cam_min_z_, cam_max_z_, cam.view_to_world[12], cam.view_to_world[13], cam.view_to_world[14],
             cam.view_to_world[8], cam.view_to_world[9], cam.view_to_world[10]);
        for (int c = 0; c < cascades_.count; c++)
            LOGR("shadows: cascade %d to %.0f texel %.2f depth %.0f", c, cascades_.far_z[c], cascades_.texel[c], cascades_.depth_range[c]);
    }
    // the casters' constants: view to light clip per cascade, the tiles, 1 / P00, 1 / P11
    struct Tail {
        float shc[4][16];
        float sht[4][4];
        float shv[4];
    } tail{};
    for (int c = 0; c < cascades_.count; c++) {
        shadow::Mul(cam.view_to_world, cascades_.world_to_light[c], tail.shc[c]);
        float ix = float(c & 1), iy = float(c >> 1);
        tail.sht[c][0] = 0.5f, tail.sht[c][1] = 0.5f, tail.sht[c][2] = ix - 0.5f, tail.sht[c][3] = 0.5f - iy;
    }
    tail.shv[0] = 1 / cam.p00, tail.shv[1] = 1 / cam.p11;
    static_assert(sizeof(Tail) <= kShadowConstantsTail);
    for (const Caster& c : casters_) std::memcpy(c.tail, &tail, sizeof(tail));
    ShadowTimestamp(0);
    Transition(shadow_atlas_.Get(), shadow_state_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = Cpu(dsv_heap_.Get(), shadow_dsv_, dsv_inc_);
    cl_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    SetFullViewport(q.tile * 2, q.tile * 2);
    ID3D12PipelineState* bound = nullptr;
    static const int exp = std::getenv("XBE_SHADOW_EXP") ? std::atoi(std::getenv("XBE_SHADOW_EXP")) : 0;
    uint32_t instances = exp == 1 ? 1 : uint32_t(cascades_.count);
    for (const Caster& c : casters_) {
        if (exp == 2) break;
        if (c.pso != bound) SetPso(bound = c.pso);
        cl_->SetGraphicsRootConstantBufferView(0, c.vc);
        cl_->SetGraphicsRootConstantBufferView(1, c.dc);
        if (c.alpha) {
            cl_->SetGraphicsRootDescriptorTable(2, c.srv);
            SetSamplerTable(c.smp);
        }
        cl_->IASetVertexBuffers(0, c.vb_count, c.vb);
        SetTopology(c.topo);
        if (c.indexed) {
            cl_->IASetIndexBuffer(&c.ib);
            cl_->DrawIndexedInstanced(c.count, instances, 0, c.base, 0);
        } else {
            cl_->DrawInstanced(c.count, instances, 0, 0);
        }
    }
    shadow_casters_n_ += casters_.size();
    Transition(shadow_atlas_.Get(), shadow_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ShadowTimestamp(1);
    bound_rt_ = bound_ds_ = nullptr;
    shadows_drawn_ = true;
    return true;
}

// a see-through draw of the world (blended over what is behind, no depth write: smoke, dust, mist) kept for DrawVeil
void D3D12Backend::RecordVeil(const State& s, const Assembled& a, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages) {
    constexpr uint32_t kSrcAlpha = 0x302, kInvSrcAlpha = 0x303;
    static const bool vlog = std::getenv("XBE_VEIL_LOG") != nullptr;
    if (vlog && k.blend && !k.zwrite && !s.layout2d && !a.pretransformed) {
        static std::unordered_set<uint64_t> seen_veil;
        if (seen_veil.insert(draw_tex0_ ^ uint64_t(occlusion_done_) << 63).second)
            LOGR("veil: candidate tex %016llx src %X dst %X occ %d rt %d topo %d quad %d tess %d kind %d prog %d", (unsigned long long)draw_tex0_,
                 k.src, k.dst, int(occlusion_done_), bound_rt_ == scene_rt_ ? 1 : 0, int(a.topology), int(sh.quad), int(tess_draw_),
                 int(stages.info[0].kind), int(sh.prog));
    }
    if (!(app::Get().ao || app::Get().contact_shadows) || occlusion_done_ || s.layout2d || a.pretransformed || sh.quad || tess_draw_) return;
    if (!bound_rt_ || bound_rt_ != scene_rt_ || a.topology != Topology::Triangles) return;
    // (blended by its alpha, or premultiplied by it)
    if (!k.blend || k.zwrite || (k.src != kSrcAlpha && k.src != 1) || k.dst != kInvSrcAlpha || stages.info[0].kind != kTex2D) return;
    static const bool vlog2 = std::getenv("XBE_VEIL_LOG") != nullptr;
    if (vlog2 && (veil_gen_ != ring_gen_ && !veil_.empty())) {
        static std::atomic<int> n{0};
        if (n++ < 5) LOGR("veil: dropped (ring %llu, kept from %llu)", (unsigned long long)ring_gen_, (unsigned long long)veil_gen_);
    }
    if (veil_.empty()) veil_gen_ = ring_gen_;
    if (veil_gen_ != ring_gen_ || veil_.size() >= 4096) return;
    static std::atomic<int> told_veil{0};
    if (told_veil++ < 3) LOGR("veil: recorded tex %016llx count %u", (unsigned long long)draw_tex0_, a.index_count ? a.index_count : a.draw_count);
    if (!veil_ps_) return;
    ShadowKey key{};
    key.vs = sh.vh, key.ps = veil_ps_, key.regs = k.regs, key.topo = (k.topo & 0xFF) | kVeilKey;
    std::memcpy(key.elements, k.elements, sizeof(key.elements));
    ID3D12PipelineState* pso = EnsureShadowPipeline(key);
    if (!pso) {
        static std::unordered_set<uint64_t> told;
        if (told.insert(sh.vh).second) LOGR("veil: no pipeline for tex %016llx (vs %016llx regs %X)", (unsigned long long)draw_tex0_, (unsigned long long)sh.vh, k.regs);
        return;
    }
    VeilDraw v{};
    v.pso = pso;
    v.vb_count = a.buffer_count;
    std::memcpy(v.vb, last_vbv_, sizeof(D3D12_VERTEX_BUFFER_VIEW) * a.buffer_count);
    v.indexed = a.index_count != 0;
    v.ib = last_ibv_;
    v.count = a.index_count ? a.index_count : a.draw_count;
    v.base = a.base_vertex;
    v.topo = PrimitiveTopology(a);
    v.vc = last_vc_, v.dc = last_dc_, v.srv = last_srv_, v.smp = last_smp_;
    veil_.push_back(v);
}

// the veil over the occlusion (scene_[1]): the see-through draws again through the scene's depth (DrawVeil's kVeilKey)
void D3D12Backend::DrawVeil(Target& ds) {
    {
        static const bool vlog = std::getenv("XBE_VEIL_LOG") != nullptr;
        static std::atomic<int> n{0};
        if (vlog && n++ % 200 == 0) LOGR("veil: called, %zu kept, gen %llu ring %llu", veil_.size(), (unsigned long long)veil_gen_, (unsigned long long)ring_gen_);
    }
    if (veil_.empty() || veil_gen_ != ring_gen_) {
        veil_.clear();
        return;
    }
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);  // (its view is not read only; not written)
    Transition(scene_[1].Get(), scene_state_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = SceneRtv(1), dh = Cpu(dsv_heap_.Get(), ds.view, dsv_inc_);
    static const bool vlog = std::getenv("XBE_VEIL_LOG") != nullptr;
    if (vlog && presents_ % 120 == 0) LOGR("veil: %zu draws over the occlusion (%ux%u, scene_[1] %p)", veil_.size(), ds.w, ds.h, (void*)scene_[1].Get());
    cl_->OMSetRenderTargets(1, &rh, FALSE, &dh);
    SetFullViewport(ds.w, ds.h);
    ID3D12PipelineState* bound = nullptr;
    for (const VeilDraw& v : veil_) {
        if (v.pso != bound) SetPso(bound = v.pso);
        cl_->SetGraphicsRootConstantBufferView(0, v.vc);
        cl_->SetGraphicsRootConstantBufferView(1, v.dc);
        cl_->SetGraphicsRootDescriptorTable(2, v.srv);
        SetSamplerTable(v.smp);
        cl_->IASetVertexBuffers(0, v.vb_count, v.vb);
        SetTopology(v.topo);
        if (v.indexed) {
            cl_->IASetIndexBuffer(&v.ib);
            cl_->DrawIndexedInstanced(v.count, 1, 0, v.base, 0);
        } else {
            cl_->DrawInstanced(v.count, 1, 0, 0);
        }
    }
    veil_.clear();
    Transition(scene_[1].Get(), scene_state_[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    bound_rt_ = bound_ds_ = nullptr;
}

// a draw's top view vertex shader (Water Interaction), made once per vertex shader; 0 none
uint64_t D3D12Backend::TopVertexShader(const State& s, const Assembled& a, const DrawShaders& sh) {
    auto it = top_vs_.find(sh.vkey);
    if (it == top_vs_.end()) {
        std::string src = TopVertexHlsl(VertexHlsl(s, a, sh));
        uint64_t h = 0;
        if (src.empty() || !Compile(src, "main", "vs_5_0", h)) h = 0;
        if (h) RememberVertexRecipe(s, a, sh, kVariantTop, h);
        it = top_vs_.emplace(sh.vkey, h).first;
    }
    return it->second;
}

// a water draw kept to be seen again from above (WaterInteraction): its height on the wave field's square
void D3D12Backend::RecordWaterTop(const State& s, const Assembled& a, const DrawShaders& sh, const PsoKey& k) {
    if (occlusion_done_ || s.layout2d || a.pretransformed || sh.quad || a.topology != Topology::Triangles || !top_water_ps_) return;
    if (!bound_rt_ || bound_rt_->gw != guest_w_ || bound_rt_->gh != guest_h_) return;
    if (water_tops_.empty()) water_gen_ = ring_gen_;
    if (water_gen_ != ring_gen_ || water_tops_.size() >= 256) return;
    uint64_t tv = TopVertexShader(s, a, sh);
    if (!tv) return;
    ShadowKey tk{};
    tk.vs = tv, tk.ps = top_water_ps_, tk.regs = k.regs, tk.topo = (k.topo & 0xFF) | kTopKey | kTopWaterKey;
    std::memcpy(tk.elements, k.elements, sizeof(tk.elements));
    ID3D12PipelineState* pso = EnsureShadowPipeline(tk);
    if (!pso) return;
    Caster c{};
    c.pso = pso;
    c.vb_count = a.buffer_count;
    std::memcpy(c.vb, last_vbv_, sizeof(D3D12_VERTEX_BUFFER_VIEW) * a.buffer_count);
    c.indexed = a.index_count != 0;
    c.ib = last_ibv_;
    c.count = a.index_count ? a.index_count : a.draw_count;
    c.base = a.base_vertex;
    c.topo = PrimitiveTopology(a);
    if (tess_draw_) c.indexed = true, c.count = uint32_t(tess_list_.size()), c.base = a.index_count ? a.base_vertex : 0,
                    c.topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    c.vc = last_vc_, c.dc = last_dc_;
    water_tops_.push_back(c);
}

// Water Interaction: what crosses the water stirs it. The frame's water drawn again from above onto a square about
// the eye (kWaveSize texels of kWaveCell): its height, the highest kept; then the casters (the world's solid draws:
// soldiers, boats, oars, bodies) from above, only where they lie within a slab about that height (1.5 m under to
// 25 cm over), how far up it they reach. The wave field steps at 60 a second from the change of that cover since its
// last step: a boat pushes a wake, an oar or a falling body rings. The water's draws read the field's slope the frame
// after (t8, their wave constants).
void D3D12Backend::WaterInteraction() {
    const app::Settings& set = app::Get();
    bool on = set.water && set.water_interaction && !water_tops_.empty() && water_gen_ == ring_gen_ && cam_ok_ &&
              top_water_ps_ && top_cover_ps_;
    // the hits: new ones (no other within 80 cm in the last half second) land this frame
    {
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
        last = now;
        wave_age_ += dt;
        for (Impact& m : impacts_seen_) m.age += dt;
        std::erase_if(impacts_seen_, [](const Impact& m) { return m.age > 0.5f; });
        std::memset(drops_, 0, sizeof(drops_));
        int nd = 0;
        for (const Impact& f : impacts_frame_) {
            bool seen = false;
            for (const Impact& m : impacts_seen_) {
                float d2 = 0;
                for (int k = 0; k < 3; k++) d2 += (m.pos[k] - f.pos[k]) * (m.pos[k] - f.pos[k]);
                seen |= d2 < 80 * 80;
            }
            if (seen) continue;
            if (impacts_seen_.size() < 256) impacts_seen_.push_back(f);
            if (nd < 8) {
                std::memcpy(wave_world_[nd], f.pos, 12);
                drops_[nd][2] = f.pos[1], drops_[nd][3] = f.strength;  // x and z once the field is placed
                nd++;
            }
        }
        impacts_frame_.clear();
        // the player wading (not drawn: the eye 1.2 m over the feet's water, give or take 40 cm), walking: a small
        // drop each frame at the feet (the field and its splashes take it only on water)
        float pi[16];
        if (cam_ok_ && nd < 8 && dt > 0 && shadow::Invert(cam_view_, pi)) {
            static float prev[3] = {}, speed = 0;
            float d[2] = {pi[12] - prev[0], pi[14] - prev[2]};
            float v = std::sqrt(d[0] * d[0] + d[1] * d[1]) / dt;
            speed += (std::min(v, 2000.0f) - speed) * std::min(1.0f, dt * 8);
            prev[0] = pi[12], prev[1] = pi[13], prev[2] = pi[14];
            if (speed > 60 && speed < 900) {
                wave_world_[nd][0] = pi[12], wave_world_[nd][1] = pi[13] - 120, wave_world_[nd][2] = pi[14];
                drops_[nd][2] = pi[13] - 120, drops_[nd][3] = std::clamp(speed / 300, 0.0f, 1.0f) * 0.15f;
                nd++;
            }
        }
        // tests (XBE_SPLASH_TEST): a hit every second and a half 5 m before the eye, on the water seen lately
        static const bool test = std::getenv("XBE_SPLASH_TEST") != nullptr;
        static float since = 0;
        float ti[16];
        if (test && (since += dt) > 1.5f && nd < 8 && wave_water_y_ > -1e8f && cam_ok_ && shadow::Invert(cam_view_, ti)) {
            since = 0;
            float f[2] = {ti[8], ti[10]}, l = std::max(1e-3f, std::sqrt(f[0] * f[0] + f[1] * f[1]));
            wave_world_[nd][0] = ti[12] + f[0] / l * 500, wave_world_[nd][1] = wave_water_y_, wave_world_[nd][2] = ti[14] + f[1] / l * 500;
            drops_[nd][2] = wave_water_y_, drops_[nd][3] = 1.2f;
            nd++;
        }
    }
    float inv[16];
    if (!on || !shadow::Invert(cam_view_, inv)) {
        water_tops_.clear();
        wave_ok_ = false;
        return;
    }
    constexpr uint32_t n = kWaveSize;
    constexpr float size = kWaveSize * kWaveCell;
    // the targets
    if (!wave_cover_) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        auto make = [&](ComPtr<ID3D12Resource>& r, D3D12_RESOURCE_STATES& st, DXGI_FORMAT f, uint32_t rtv) {
            D3D12_RESOURCE_DESC td = Texture2DDesc(f, n, n);
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{};
            cv.Format = f;
            st = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, &cv, IID_PPV_ARGS(&r)))) return false;
            dev_->CreateRenderTargetView(r.Get(), nullptr, WaveRtv(rtv));
            const float zero[4] = {};
            cl_->ClearRenderTargetView(WaveRtv(rtv), zero, 0, nullptr);
            return true;
        };
        bool ok = make(wave_state_[0], wave_state_st_[0], DXGI_FORMAT_R16G16B16A16_FLOAT, 0) &&
                  make(wave_state_[1], wave_state_st_[1], DXGI_FORMAT_R16G16B16A16_FLOAT, 1) &&
                  make(wave_water_, wave_water_st_, DXGI_FORMAT_R32_FLOAT, 2) && make(wave_cover_, wave_cover_st_, DXGI_FORMAT_R16_FLOAT, 3);
        if (!ok) {
            wave_state_[0].Reset(), wave_state_[1].Reset(), wave_water_.Reset(), wave_cover_.Reset();
            water_tops_.clear();
            wave_ok_ = false;
            return;
        }
        wave_placed_ = false;
    }
    // the square follows the eye a sixteenth of its size at a time, by whole texels
    float ex = inv[12], ez = inv[14];
    int shift[2] = {0, 0};
    {
        float cx = wave_x0_ + size / 2, cz = wave_z0_ + size / 2;
        if (!wave_placed_ || std::fabs(ex - cx) > size / 16 || std::fabs(ez - cz) > size / 16) {
            float nx = std::floor((ex - size / 2) / kWaveCell) * kWaveCell, nz = std::floor((ez - size / 2) / kWaveCell) * kWaveCell;
            if (wave_placed_) {
                shift[0] = int(std::lround((nx - wave_x0_) / kWaveCell)), shift[1] = int(std::lround((nz - wave_z0_) / kWaveCell));
                if (std::abs(shift[0]) >= int(n) || std::abs(shift[1]) >= int(n)) shift[0] = shift[1] = int(n);  // all new
            } else {
                shift[0] = shift[1] = int(n);
            }
            if (shift[0] == int(n)) wave_age_ = 0;
            wave_x0_ = nx, wave_z0_ = nz, wave_placed_ = true;
        }
    }
    // the drops in the field's texels
    for (int d = 0; d < 8; d++)
        if (drops_[d][3] > 0) drops_[d][0] = (wave_world_[d][0] - wave_x0_) / kWaveCell, drops_[d][1] = (wave_world_[d][2] - wave_z0_) / kWaveCell;
    // the splashes the field showed kFrames ago (waveevents, read back)
    if (wave_events_) {
        EventSlot& es = wave_slots_[frame_ % kFrames];
        if (es.fence && fence_->GetCompletedValue() >= es.fence) {
            const uint8_t* m = nullptr;
            if (SUCCEEDED(wave_events_rb_[frame_ % kFrames]->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint8_t**>(&m)))) && m) {
                const uint32_t pitch = wave_events_fp_.Footprint.RowPitch;
                for (int by = 0; by < 64; by++) {
                    const float* row = reinterpret_cast<const float*>(m + size_t(by) * pitch);
                    for (int bx = 0; bx < 64; bx++) {
                        float gain = row[bx * 2], wy = row[bx * 2 + 1];
                        if (wy > -1e8f) wave_water_y_ = wy;
                        // (a hull moving on covers a thin strip of a block a step: a fall, an oar's dip covers it
                        // at once)
                        if (gain < 12 || wy < -1e8f || wave_age_ < 1.5f) continue;
                        float pos[3] = {es.x0 + (bx * 8 + 4) * kWaveCell, wy, es.z0 + (by * 8 + 4) * kWaveCell};
                        SpawnSplash(pos, std::clamp(gain / 30, 0.3f, 1.6f));
                    }
                }
                const float* row = reinterpret_cast<const float*>(m + size_t(64) * pitch);
                for (int d = 0; d < 8; d++)
                    if (es.drops[d][3] > 0 && row[d * 2] > 0.5f) {
                        float pos[3] = {es.world[d][0], row[d * 2 + 1], es.world[d][2]};
                        SpawnSplash(pos, es.drops[d][3]);
                    }
                D3D12_RANGE none{0, 0};
                wave_events_rb_[frame_ % kFrames]->Unmap(0, &none);
            }
            es.fence = 0;
        }
    }
    // the steps due: 60 a second, at once when the field moved or a hit landed; a frame without one draws nothing
    // here (the field and its cover change only with them)
    int steps;
    {
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        wave_acc_ += std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
        last = now;
        steps = std::min(int(wave_acc_ * 60), 4);
        wave_acc_ = std::max(0.0f, wave_acc_ - steps / 60.0f);
        if (wave_acc_ > 1 / 60.0f) wave_acc_ = 0;
        bool drops = false;
        for (int d = 0; d < 8; d++) drops |= drops_[d][3] > 0;
        if (steps == 0 && (shift[0] || shift[1] || drops)) steps = 1;
    }
    if (steps == 0) {
        water_tops_.clear();
        wave_ok_ = true;
        return;
    }
    // the top view: view space to the square's clip (x east, z north up the texture), the world's height
    struct Td {
        float m[16];
        float y[4];
        float v[4];
        float p[4];
    } td{};
    {
        float W[16] = {};
        W[0] = 2 / size;                   // x
        W[9] = -2 / size;                  // z: clip y down the rows
        W[12] = -wave_x0_ * 2 / size - 1;  // translation
        W[13] = 1 + wave_z0_ * 2 / size;
        W[14] = 0.5f, W[15] = 1;
        shadow::Mul(inv, W, td.m);
        td.y[0] = inv[1], td.y[1] = inv[5], td.y[2] = inv[9], td.y[3] = inv[13];
        td.v[0] = 1 / cam_proj_[0], td.v[1] = 1 / cam_proj_[5];
        td.p[0] = 150, td.p[1] = 25;
    }
    static const bool wave_log = std::getenv("XBE_WAVE_DEBUG") != nullptr;
    if (wave_log && presents_ % 240 == 0) {
        size_t tops = 0;
        for (const Caster& c : casters_) tops += c.top != nullptr;
        LOGR("waves: %zu water draws, %zu of %zu casters from above (gen %d), eye %.0f %.0f %.0f, field at %.0f %.0f", water_tops_.size(), tops,
             casters_.size(), int(casters_gen_ == ring_gen_), ex, inv[13], ez, wave_x0_, wave_z0_);
    }
    // the water and its cover from above at 30 a second (every other step frame; the field moved, at once): the
    // casters drawn again cost as much as the scene's own
    static bool cover_now = true;
    cover_now = !cover_now || shift[0] || shift[1] || !wave_events_;
    if (cover_now) {
    Alloc tcb = Upload(sizeof(Td), 256);
    std::memcpy(tcb.cpu, &td, sizeof(td));
    cl_->SetGraphicsRootConstantBufferView(5, tcb.gpu);
    // the water's height
    Transition(wave_water_.Get(), wave_water_st_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        const float none[4] = {-1e9f, -1e9f, -1e9f, -1e9f};
        D3D12_CPU_DESCRIPTOR_HANDLE rh = WaveRtv(2);
        cl_->ClearRenderTargetView(rh, none, 0, nullptr);
        cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
        SetFullViewport(n, n);
        ID3D12PipelineState* bound = nullptr;
        for (const Caster& c : water_tops_) {
            if (c.pso != bound) SetPso(bound = c.pso);
            cl_->SetGraphicsRootConstantBufferView(0, c.vc);
            cl_->SetGraphicsRootConstantBufferView(1, c.dc);
            cl_->IASetVertexBuffers(0, c.vb_count, c.vb);
            SetTopology(c.topo);
            if (c.indexed) {
                cl_->IASetIndexBuffer(&c.ib);
                cl_->DrawIndexedInstanced(c.count, 1, 0, c.base, 0);
            } else {
                cl_->DrawInstanced(c.count, 1, 0, 0);
            }
        }
    }
    Transition(wave_water_.Get(), wave_water_st_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // what crosses it
    Transition(wave_cover_.Get(), wave_cover_st_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        const float zero[4] = {};
        D3D12_CPU_DESCRIPTOR_HANDLE rh = WaveRtv(3);
        cl_->ClearRenderTargetView(rh, zero, 0, nullptr);
        cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
        SetFullViewport(n, n);
        uint32_t slot = AllocSrvSlots(7);
        for (uint32_t i = 0; i < 7; i++) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = i == 0 ? DXGI_FORMAT_R32_FLOAT : kBgra;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            dev_->CreateShaderResourceView(i == 0 ? wave_water_.Get() : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
        }
        cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), slot, srv_inc_));
        ID3D12PipelineState* bound = nullptr;
        if (casters_gen_ == ring_gen_) {
            for (const Caster& c : casters_) {
                if (!c.top) continue;
                if (c.top != bound) SetPso(bound = c.top);
                cl_->SetGraphicsRootConstantBufferView(0, c.vc);
                cl_->SetGraphicsRootConstantBufferView(1, c.dc);
                cl_->IASetVertexBuffers(0, c.vb_count, c.vb);
                SetTopology(c.topo);
                if (c.indexed) {
                    cl_->IASetIndexBuffer(&c.ib);
                    cl_->DrawIndexedInstanced(c.count, 1, 0, c.base, 0);
                } else {
                    cl_->DrawInstanced(c.count, 1, 0, 0);
                }
            }
        }
    }
    Transition(wave_cover_.Get(), wave_cover_st_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    // the steps: 60 a second, the field moved on the first
    {
        // what to splash, before the steps: how far the cover rose since the last, where the drops landed (read
        // back kFrames later)
        if (steps > 0 && cover_now) {
            if (!wave_events_) {
                D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
                D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_R32G32_FLOAT, 64, 65);
                td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                D3D12_CLEAR_VALUE cv{};
                cv.Format = td.Format;
                wave_events_st_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bool ok = SUCCEEDED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, wave_events_st_, &cv, IID_PPV_ARGS(&wave_events_)));
                UINT64 bytes = 0;
                if (ok) dev_->GetCopyableFootprints(&td, 0, 1, 0, &wave_events_fp_, nullptr, nullptr, &bytes);
                D3D12_HEAP_PROPERTIES rp{D3D12_HEAP_TYPE_READBACK};
                D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
                for (auto& b : wave_events_rb_)
                    ok = ok && SUCCEEDED(CreateResource(&rp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&b)));
                if (ok) dev_->CreateRenderTargetView(wave_events_.Get(), nullptr, WaveRtv(4));
                else wave_events_.Reset();
            }
            EventSlot& es = wave_slots_[frame_ % kFrames];
            if (wave_events_ && !es.fence) {
                Transition(wave_state_[wave_cur_].Get(), wave_state_st_[wave_cur_], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                Transition(wave_events_.Get(), wave_events_st_, D3D12_RESOURCE_STATE_RENDER_TARGET);
                float q[40] = {float(shift[0]), float(shift[1]), 0, 0, 0, float(n), 0, 160};
                std::memcpy(q + 8, drops_, sizeof(drops_));
                ID3D12Resource* srcs[3] = {wave_state_[wave_cur_].Get(), wave_water_.Get(), wave_cover_.Get()};
                DXGI_FORMAT fmts[3] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_FLOAT};
                pass_floats_ = 40;
                Pass(kWaveHlsl, "waveevents", DXGI_FORMAT_R32G32_FLOAT, WaveRtv(4), 64, 65, srcs, fmts, q, -1);
                Transition(wave_events_.Get(), wave_events_st_, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dl{wave_events_rb_[frame_ % kFrames].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                dl.PlacedFootprint = wave_events_fp_;
                D3D12_TEXTURE_COPY_LOCATION sl{wave_events_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
                es.fence = fence_value_ + 1;  // this frame's (Submit signals the next value)
                es.x0 = wave_x0_, es.z0 = wave_z0_;
                std::memcpy(es.drops, drops_, sizeof(drops_));
                std::memcpy(es.world, wave_world_, sizeof(wave_world_));
            }
        }
        for (int i = 0; i < steps; i++) {
            int src = wave_cur_, dst = 1 - wave_cur_;
            Transition(wave_state_[src].Get(), wave_state_st_[src], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(wave_state_[dst].Get(), wave_state_st_[dst], D3D12_RESOURCE_STATE_RENDER_TARGET);
            float q[40] = {float(i ? 0 : shift[0]), float(i ? 0 : shift[1]), 0.22f, 0.988f, 0.02f, float(n), 1.0f, 160};
            if (i == 0) std::memcpy(q + 8, drops_, sizeof(drops_));
            pass_floats_ = 40;
            ID3D12Resource* srcs[3] = {wave_state_[src].Get(), wave_water_.Get(), wave_cover_.Get()};
            DXGI_FORMAT fmts[3] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_FLOAT};
            Pass(kWaveHlsl, "wavestep", DXGI_FORMAT_R16G16B16A16_FLOAT, WaveRtv(uint32_t(dst)), n, n, srcs, fmts, q, -1);
            wave_cur_ = dst;
        }
        Transition(wave_state_[wave_cur_].Get(), wave_state_st_[wave_cur_], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    // tests (XBE_WAVE_DUMP): the water's height, the cover and the field read back now and then, logs/wave_*.f32
    static const bool dump = std::getenv("XBE_WAVE_DUMP") != nullptr;
    if (dump && presents_ % 300 == 150) {
        struct Out {
            ID3D12Resource* r;
            D3D12_RESOURCE_STATES* st;
            const char* name;
        } outs[3] = {{wave_water_.Get(), &wave_water_st_, "water"}, {wave_cover_.Get(), &wave_cover_st_, "cover"},
                     {wave_state_[wave_cur_].Get(), &wave_state_st_[wave_cur_], "state"}};
        ComPtr<ID3D12Resource> rb[3];
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[3];
        for (int i = 0; i < 3; i++) {
            D3D12_RESOURCE_DESC rd = outs[i].r->GetDesc();
            UINT64 bytes = 0;
            dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp[i], nullptr, nullptr, &bytes);
            D3D12_HEAP_PROPERTIES rp{D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
            if (FAILED(CreateResource(&rp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb[i])))) return;
            Transition(outs[i].r, *outs[i].st, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dl{rb[i].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dl.PlacedFootprint = fp[i];
            D3D12_TEXTURE_COPY_LOCATION sl{outs[i].r, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
            Transition(outs[i].r, *outs[i].st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        Submit(true);
        for (int i = 0; i < 3; i++) {
            uint8_t* p = nullptr;
            if (FAILED(rb[i]->Map(0, nullptr, reinterpret_cast<void**>(&p)))) continue;
            char path[64];
            std::snprintf(path, sizeof(path), "logs/wave_%s.f32", outs[i].name);
            if (FILE* f = std::fopen(path, "wb")) {
                uint32_t head[3] = {n, n, fp[i].Footprint.RowPitch};
                std::fwrite(head, 4, 3, f);
                std::fwrite(p, 1, size_t(fp[i].Footprint.RowPitch) * n, f);
                std::fclose(f);
            }
            rb[i]->Unmap(0, nullptr);
        }
        LOGR("waves: dumped (field at %.0f %.0f)", wave_x0_, wave_z0_);
    }
    water_tops_.clear();
    wave_ok_ = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// a splash where something fell in, an oar dipped, a hit landed (strength about 1): drops thrown up and out, a puff
// of mist; none again within 60 cm for a fifth of a second
void D3D12Backend::SpawnSplash(const float pos[3], float strength) {
    for (const auto& r : splash_recent_) {
        float d2 = (r[0] - pos[0]) * (r[0] - pos[0]) + (r[2] - pos[2]) * (r[2] - pos[2]);
        if (d2 < 100 * 100 && r[3] < 0.5f) return;
    }
    if (splash_recent_.size() < 256) splash_recent_.push_back({pos[0], pos[1], pos[2], 0});
    static const bool slog = std::getenv("XBE_WAVE_DEBUG") != nullptr;
    if (slog) LOGR("waves: splash at %.0f %.0f %.0f strength %.2f (%zu alive)", pos[0], pos[1], pos[2], strength, splashes_.size());
    static uint32_t seed = 12345;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) / 16777216.0f;
    };
    int drops = int(10 + 45 * strength), mist = strength > 0.6f ? int(1 + 3 * strength) : 0;
    for (int i = 0; i < drops + mist && splashes_.size() < 1900; i++) {
        Splash s{};
        bool m = i >= drops;
        float a = rnd() * 6.2831853f, r = rnd();
        float out = (m ? 40 : 60 + 160 * r) * std::sqrt(strength), upv = m ? 30 + 40 * rnd() : (180 + 320 * rnd()) * std::sqrt(strength);
        s.p[0] = pos[0] + std::cos(a) * r * 12 * strength, s.p[1] = pos[1] + 2, s.p[2] = pos[2] + std::sin(a) * r * 12 * strength;
        s.v[0] = std::cos(a) * out, s.v[1] = upv, s.v[2] = std::sin(a) * out;
        s.size = m ? (40 + 35 * rnd()) * std::sqrt(strength) : 1.0f + 1.6f * rnd();
        s.life = m ? 0.9f + 0.6f * rnd() : 2.0f;
        s.seed = rnd();
        s.water_y = pos[1];
        s.kind = m ? 1 : 0;
        splashes_.push_back(s);
    }
}

// the splashes moved (gravity and air for the drops, the mist rising and spreading slowly) and drawn over the
// finished scene (kSplashHlsl), behind the weapon in hand
void D3D12Backend::DrawSplashes(Target& rt) {
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.05f);
    last = now;
    for (auto& r : splash_recent_) r[3] += dt;
    std::erase_if(splash_recent_, [](const std::array<float, 4>& r) { return r[3] > 0.6f; });
    for (Splash& s : splashes_) {
        s.age += dt;
        if (s.kind == 0) {
            s.v[1] -= 981 * dt;
            float drag = std::max(0.0f, 1 - 0.8f * dt);
            s.v[0] *= drag, s.v[2] *= drag;
        } else {
            float drag = std::max(0.0f, 1 - 2.5f * dt);
            for (float& v : s.v) v *= drag;
            s.size += 25 * dt;
        }
        for (int k = 0; k < 3; k++) s.p[k] += s.v[k] * dt;
    }
    std::erase_if(splashes_, [](const Splash& s) { return s.age > s.life || (s.kind == 0 && s.v[1] < 0 && s.p[1] < s.water_y - 3); });
    static const bool slog = std::getenv("XBE_WAVE_DEBUG") != nullptr;
    if (slog && presents_ % 120 == 0) LOGR("waves: %zu splashes alive, ds %d cam %d", splashes_.size(), int(scene_ds_ != nullptr), int(cam_ok_));
    if (splashes_.empty() || !scene_ds_ || !cam_ok_ || !app::Get().water_interaction) {
        if (!app::Get().water_interaction) splashes_.clear();
        return;
    }
    ID3D12PipelineState* pso = EnsurePassPipeline(kSplashHlsl, "splashps", rt.fmt, -2);
    if (!pso) {
        if (slog) LOGR("waves: no splash pipeline");
        return;
    }
    Target& ds = *scene_ds_;
    float inv[16];
    if (!shadow::Invert(cam_view_, inv)) return;
    // the constants: the camera's view projection, its right and up in the world (with the depth range), the
    // depth's projection terms and the light the spray catches, then two rows a splash
    const size_t count = std::min<size_t>(splashes_.size(), 1990);
    std::vector<float> cb(size_t(4 * 7) + count * 8);
    shadow::Mul(cam_view_, cam_proj_, cb.data());
    cb[16] = inv[0], cb[17] = inv[1], cb[18] = inv[2], cb[19] = cam_min_z_;
    cb[20] = inv[4], cb[21] = inv[5], cb[22] = inv[6], cb[23] = cam_max_z_;
    float glow = std::clamp(0.05f + light_.ambient * 0.15f + light_.direct * 0.08f, 0.05f, 0.16f);
    cb[24] = cam_proj_[10], cb[25] = cam_proj_[14], cb[26] = glow, cb[27] = world_depth_saved_ ? 1.0f : 0.0f;
    if (std::getenv("XBE_SPLASH_RED")) cb[27] = 10;  // tests: the squares red
    for (size_t i = 0; i < count; i++) {
        const Splash& s = splashes_[i];
        float* o = cb.data() + 28 + i * 8;
        float u = s.age / s.life;
        static const float big = std::getenv("XBE_SPLASH_BIG") ? 5.0f : 1.0f;  // tests
        o[0] = s.p[0], o[1] = s.p[1], o[2] = s.p[2], o[3] = s.size * big;
        o[4] = s.kind ? (1 - u) * std::min(1.0f, s.age * 8) : 1 - u * u;
        o[5] = float(s.kind);
        o[6] = s.kind ? 0 : std::clamp(std::fabs(s.v[1]) / 120, 0.0f, 3.5f);  // a drop stretched along its fall
        o[7] = s.seed;
    }
    Alloc c = Upload((cb.size() * 4 + 255) & ~size_t(255), 256);
    std::memcpy(c.cpu, cb.data(), cb.size() * 4);
    // the world's depth (before the weapon), the depth with the weapon, the scene's copy for their light
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (world_depth_saved_) Transition(world_depth_.Get(), world_depth_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* depth = world_depth_saved_ ? world_depth_.Get() : ds.res.Get();
    uint32_t slot = AllocSrvSlots(7);
    std::pair<ID3D12Resource*, DXGI_FORMAT> src[3] = {{depth, DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
                                                      {ds.res.Get(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
                                                      {scene_[0].Get(), rt.fmt}};
    for (uint32_t i = 0; i < 7; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 3 ? src[i].second : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 3 ? src[i].first : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), rt.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(rt.w, rt.h);
    SetPso(pso);
    cl_->SetGraphicsRootConstantBufferView(0, c.gpu);
    cl_->SetGraphicsRootConstantBufferView(1, c.gpu);
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), slot, srv_inc_));
    SetSamplerTable(Gpu(smp_heap_.Get(), EnsureBlitSamplers(), smp_inc_));
    SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl_->DrawInstanced(6, uint32_t(count), 0, 0);
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    bound_rt_ = bound_ds_ = nullptr;
}

// Snow Tracks: the soldiers' steps (gpu::g_steps) and the player's (not drawn: a step each 70 cm walked, the feet
// 1.6 m under the eye, left and right in turn) pressed into the trail, a square about the eye that follows it by
// whole texels; the snowy ground reads it the frame after (t9, its trail constants). Only a frame with steps or a
// move runs the pass.
void D3D12Backend::SnowTracks() {
    std::vector<Step> steps;
    {
        std::lock_guard l(g_steps.m);
        steps.swap(g_steps.list);
    }
    const app::Settings& set = app::Get();
    float inv[16];
    if (!set.snow_tracks || !cam_ok_ || !shadow::Invert(cam_view_, inv)) {
        trail_ok_ = false;
        return;
    }
    // the player
    {
        static auto last = std::chrono::steady_clock::now();
        static float prev[3] = {}, walked = 0;
        static int side = 0;
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        float d[2] = {inv[12] - prev[0], inv[14] - prev[2]};
        float len = std::sqrt(d[0] * d[0] + d[1] * d[1]);
        float speed = dt > 0 ? len / dt : 0;
        if (speed > 50 && speed < 700 && std::fabs(inv[13] - prev[1]) < 30) {
            walked += len;
            if (walked > 70) {
                walked = 0;
                float fx = d[0] / len, fz = d[1] / len, off = side ? 11.0f : -11.0f;
                steps.push_back({inv[12] - fz * off, inv[13] - 160, inv[14] + fx * off, fx, fz, side});
                side ^= 1;
            }
        }
        prev[0] = inv[12], prev[1] = inv[13], prev[2] = inv[14];
    }
    // tests (XBE_SNOW_TEST): steps round a circle 4 m before the eye, at the height of the soldiers' last step
    {
        static float last_y = -1e9f;
        for (const Step& st : steps) last_y = st.y;
        static const bool test = std::getenv("XBE_SNOW_TEST") != nullptr;
        static const bool at_eye = test && std::string(std::getenv("XBE_SNOW_TEST")) == "eye";
        static const float below = at_eye ? 0.0f : float(std::atof(std::getenv("XBE_SNOW_TEST") ? std::getenv("XBE_SNOW_TEST") : "0"));
        if (at_eye) last_y = inv[13] - 160;
        else if (test && below > 0) last_y = inv[13] - below;  // a number: the ground that far under the eye
        static int k = 0;
        static auto t0 = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (test && last_y > -1e8f && std::chrono::duration<float>(now - t0).count() > 0.25f) {
            t0 = now;
            float f[2] = {inv[8], inv[10]}, l = std::max(1e-3f, std::sqrt(f[0] * f[0] + f[1] * f[1]));
            float cx = inv[12] + f[0] / l * 100, cz = inv[14] + f[1] / l * 100, a = k * 0.35f;
            float px = cx + std::cos(a) * 150, pz = cz + std::sin(a) * 150, dx = -std::sin(a), dz = std::cos(a);
            float off = (k & 1) ? 11.0f : -11.0f;
            steps.push_back({px - dz * off, last_y, pz + dx * off, dx, dz, k & 1});
            k++;
        }
    }
    constexpr uint32_t n = kTrailSize;
    constexpr float size = kTrailSize * kTrailCell;
    if (!trail_[1]) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_R16G16_FLOAT, n, n);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = td.Format;
        bool ok = true;
        for (int i = 0; i < 2 && ok; i++) {
            trail_st_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            ok = SUCCEEDED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, trail_st_[i], &cv, IID_PPV_ARGS(&trail_[i])));
            if (ok) {
                dev_->CreateRenderTargetView(trail_[i].Get(), nullptr, WaveRtv(5 + uint32_t(i)));
                const float zero[4] = {};
                cl_->ClearRenderTargetView(WaveRtv(5 + uint32_t(i)), zero, 0, nullptr);
                Transition(trail_[i].Get(), trail_st_[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
        if (!ok) {
            trail_[0].Reset(), trail_[1].Reset();
            trail_ok_ = false;
            return;
        }
        trail_x0_ = std::floor((inv[12] - size / 2) / kTrailCell) * kTrailCell;
        trail_z0_ = std::floor((inv[14] - size / 2) / kTrailCell) * kTrailCell;
    }
    // the square follows the eye an eighth of its size at a time
    int shift[2] = {0, 0};
    {
        float cx = trail_x0_ + size / 2, cz = trail_z0_ + size / 2;
        if (std::fabs(inv[12] - cx) > size / 8 || std::fabs(inv[14] - cz) > size / 8) {
            float nx = std::floor((inv[12] - size / 2) / kTrailCell) * kTrailCell, nz = std::floor((inv[14] - size / 2) / kTrailCell) * kTrailCell;
            shift[0] = int(std::lround((nx - trail_x0_) / kTrailCell)), shift[1] = int(std::lround((nz - trail_z0_) / kTrailCell));
            if (std::abs(shift[0]) >= int(n) || std::abs(shift[1]) >= int(n)) shift[0] = shift[1] = int(n);
            trail_x0_ = nx, trail_z0_ = nz;
        }
    }
    trail_ok_ = true;
    if (steps.empty() && !shift[0] && !shift[1]) return;
    // the pass: 32 steps at a time
    for (size_t first = 0; first < std::max<size_t>(steps.size(), 1); first += 32) {
        float q[8 + 64 * 4] = {float(first ? 0 : shift[0]), float(first ? 0 : shift[1]), float(n), 0, 0, 0, 0, 0};
        uint32_t count = 0;
        for (size_t i = first; i < steps.size() && count < 32; i++) {
            const Step& st = steps[i];
            float* a = q + 8 + count * 8;
            float l = std::max(1e-3f, std::sqrt(st.dx * st.dx + st.dz * st.dz));
            a[0] = (st.x - trail_x0_) / kTrailCell, a[1] = (st.z - trail_z0_) / kTrailCell, a[2] = st.y, a[3] = float(st.side);
            a[4] = st.dx / l, a[5] = st.dz / l;
            count++;
        }
        q[3] = float(count);
        int src = trail_cur_, dst = 1 - trail_cur_;
        Transition(trail_[src].Get(), trail_st_[src], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(trail_[dst].Get(), trail_st_[dst], D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12Resource* srcs[3] = {trail_[src].Get(), nullptr, nullptr};
        DXGI_FORMAT fmts[3] = {DXGI_FORMAT_R16G16_FLOAT, kBgra, kBgra};
        pass_floats_ = 8 + 64 * 4;
        Pass(kTrailHlsl, "trailstep", DXGI_FORMAT_R16G16_FLOAT, WaveRtv(5 + uint32_t(dst)), n, n, srcs, fmts, q, -1);
        Transition(trail_[dst].Get(), trail_st_[dst], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        trail_cur_ = dst;
    }
    static const bool slog = std::getenv("XBE_SNOW_LOG") != nullptr;
    if (slog && !steps.empty()) LOGR("snow: %zu steps pressed, first at %.0f %.0f %.0f", steps.size(), steps[0].x, steps[0].y, steps[0].z);
    bound_rt_ = bound_ds_ = nullptr;
}

// tests: the floors drawn (a solid draw of the world's: depth written, not blended, not a character, not the sky):
// its triangles in the world (the draw's world matrix, or its program's position rows and the camera), those facing up or down (a floor, a roof's top) sampled
// every metre or so into 3 m cells, each cell's heights kept apart by more than a metre
void D3D12Backend::CollectMap(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk, const PsoKey& k) {
    if (s.layout2d || a.pretransformed || occlusion_done_ || a.topology != Topology::Triangles || !k.zwrite || k.blend || draw_far_ ||
        !bound_rt_ || bound_rt_->gw != guest_w_ || bound_rt_->gh != guest_h_)
        return;
    auto pl = sh.prog ? prog_light_.find(sh.vkey) : prog_light_.end();
    bool character = (sh.prog && pk.lit && !(pl != prog_light_.end() && pl->second.ok)) || (sh.fixed_transform && (sh.fk.light & 1));
    if (character) return;
    float W[16];
    if (sh.prog) {
        if (pl == prog_light_.end() || !pl->second.ok || !EnsureInvVp()) return;
        float A[16];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) A[i * 4 + j] = s.vsc[pl->second.pos[j]][size_t(i)];
        shadow::Mul(A, inv_vp_, W);
    } else {
        std::memcpy(W, s.transforms[kTsWorld].data(), sizeof(W));
    }
    const VertexElement& e = a.elements[0];
    if (!(a.regs & 1) || e.buffer >= a.buffer_count) return;
    const VertexBuffer& b = a.buffers[e.buffer];
    bool f3 = e.format == DXGI_FORMAT_R32G32B32_FLOAT || e.format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (!b.src || !b.stride || !f3) return;
    uint32_t verts = b.bytes / b.stride;
    auto world = [&](uint32_t v, float* out) {
        if (v >= verts) return false;
        float p[3];
        std::memcpy(p, b.src + size_t(v) * b.stride + e.offset, 12);
        float w = p[0] * W[3] + p[1] * W[7] + p[2] * W[11] + W[15];
        if (std::fabs(w) < 1e-6f) return false;
        for (int j = 0; j < 3; j++) out[j] = (p[0] * W[j] + p[1] * W[4 + j] + p[2] * W[8 + j] + W[12 + j]) / w;
        return std::isfinite(out[0] + out[1] + out[2]) && std::fabs(out[0]) < 1e6f && std::fabs(out[2]) < 1e6f;
    };
    uint32_t n = a.index_count ? a.index_count : a.draw_count;
    auto index = [&](uint32_t i) -> uint32_t {
        if (!a.index_count) return i;
        uint32_t v = a.index16 ? static_cast<const uint16_t*>(a.index_src)[i] : static_cast<const uint32_t*>(a.index_src)[i];
        return uint32_t(int32_t(v) + a.base_vertex);
    };
    constexpr float kCell = 300;
    auto put = [&](float x, float y, float z) {
        int64_t key = (int64_t(std::floor(x / kCell)) << 32) ^ (int64_t(std::floor(z / kCell)) & 0xFFFFFFFF);
        auto& ys = map_cells_[key];
        for (float& h : ys)
            if (std::fabs(h - y) < 120) {
                h = std::max(h, y);
                return;
            }
        if (ys.size() < 8) ys.push_back(y);
    };
    auto add = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
        float p[3][3];
        if (!world(i0, p[0]) || !world(i1, p[1]) || !world(i2, p[2])) return;
        float u[3], v[3], c[3];
        for (int j = 0; j < 3; j++) u[j] = p[1][j] - p[0][j], v[j] = p[2][j] - p[0][j];
        c[0] = u[1] * v[2] - u[2] * v[1], c[1] = u[2] * v[0] - u[0] * v[2], c[2] = u[0] * v[1] - u[1] * v[0];
        float len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        if (len < 100 || std::fabs(c[1]) < 0.7f * len) return;  // a square decimetre at least, facing up or down
        float span = std::max({std::fabs(u[0]), std::fabs(u[2]), std::fabs(v[0]), std::fabs(v[2])});
        int steps = std::clamp(int(span / 100), 1, 24);
        for (int a1 = 0; a1 <= steps; a1++)
            for (int b1 = 0; a1 + b1 <= steps; b1++) {
                float s1 = float(a1) / steps, t1 = float(b1) / steps;
                put(p[0][0] + u[0] * s1 + v[0] * t1, p[0][1] + u[1] * s1 + v[1] * t1, p[0][2] + u[2] * s1 + v[2] * t1);
            }
    };
    if (a.strip) {
        for (uint32_t i = 2; i < n; i++) {
            uint32_t i0 = index(i - 2), i1 = index(i - 1), i2 = index(i);
            if (i0 == i1 || i1 == i2 || i0 == i2) continue;
            add(i0, i1, i2);
        }
    } else {
        for (uint32_t i = 0; i + 2 < n; i += 3) add(index(i), index(i + 1), index(i + 2));
    }
}

// tests: the map's frames counted down (g_map_frames); at the end the cells written to logs/map.txt
void D3D12Backend::MapFrame() {
    int left = g_map_frames.load();
    if (left > 0) {
        if (!map_on_) map_cells_.clear();
        map_on_ = true;
        g_map_frames = left - 1;
        return;
    }
    if (!map_on_) return;
    map_on_ = false;
    std::filesystem::create_directories(app::ExeDir() / "logs");
    std::ofstream out(app::ExeDir() / "logs" / "map.txt");
    for (const auto& [key, ys] : map_cells_) {
        int32_t cx = int32_t(key >> 32), cz = int32_t(uint32_t(key & 0xFFFFFFFF));
        out << (cx * 300 + 150) << ' ' << (cz * 300 + 150);
        for (float y : ys) out << ' ' << int(y);
        out << '\n';
    }
    LOGR("map: %zu cells written", map_cells_.size());
}

// The guard band (guard_): the frame's casters drawn again from the camera with its view widened by 1 / kGuardScale,
// depth alone; null when the frame has none
ID3D12Resource* D3D12Backend::DrawGuard(uint32_t w, uint32_t h) {
    if (casters_.empty() || casters_gen_ != ring_gen_) return nullptr;
    uint32_t gw = std::max(1u, w * 3 / 4), gh = std::max(1u, h * 3 / 4);
    if (!guard_ || guard_w_ != gw || guard_h_ != gh) {
        if (guard_) Release(std::move(guard_));
        if (guard_dsv_ == ~0u) {
            if (free_dsv_.empty()) return nullptr;
            guard_dsv_ = free_dsv_.back();
            free_dsv_.pop_back();
        }
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_R32_TYPELESS, gw, gh);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1;
        guard_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, guard_state_, &cv, IID_PPV_ARGS(&guard_)))) {
            guard_.Reset();
            return nullptr;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(guard_.Get(), &dv, Cpu(dsv_heap_.Get(), guard_dsv_, dsv_inc_));
        guard_w_ = gw, guard_h_ = gh;
        LOGR("d3d12: guard band %ux%u", gw, gh);
    }
    Transition(guard_.Get(), guard_state_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = Cpu(dsv_heap_.Get(), guard_dsv_, dsv_inc_);
    cl_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    SetFullViewport(gw, gh);
    ID3D12PipelineState* bound = nullptr;
    float zr[2] = {-1, -1};
    for (const Caster& c : casters_) {
        if (!c.guard) continue;
        if (c.zmin != zr[0] || c.zmax != zr[1]) {
            zr[0] = c.zmin, zr[1] = c.zmax;
            D3D12_VIEWPORT vp{0, 0, float(gw), float(gh), c.zmin, c.zmax};
            SetViewport(vp);
        }
        if (c.guard != bound) SetPso(bound = c.guard);
        cl_->SetGraphicsRootConstantBufferView(0, c.vc);
        cl_->SetGraphicsRootConstantBufferView(1, c.dc);
        if (c.alpha) {
            cl_->SetGraphicsRootDescriptorTable(2, c.srv);
            SetSamplerTable(c.smp);
        }
        cl_->IASetVertexBuffers(0, c.vb_count, c.vb);
        SetTopology(c.topo);
        if (c.indexed) {
            cl_->IASetIndexBuffer(&c.ib);
            cl_->DrawIndexedInstanced(c.count, 1, 0, c.base, 0);
        } else {
            cl_->DrawInstanced(c.count, 1, 0, 0);
        }
    }
    Transition(guard_.Get(), guard_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    bound_rt_ = bound_ds_ = nullptr;
    return guard_.Get();
}

// the characters' pixels marked in scene_[2] (free until the occlusion's blur): their draws again, through the
// scene's depth; false when the frame has none
bool D3D12Backend::CharacterMask(Target& rt, Target& ds) {
    bool any = false;
    for (const Caster& c : casters_) any |= c.screen != nullptr;
    if (!any) return false;
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    Transition(scene_[2].Get(), scene_state_[2], D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = SceneRtv(2), dh = Cpu(dsv_heap_.Get(), ds.view, dsv_inc_);
    const float zero[4] = {};
    cl_->ClearRenderTargetView(rh, zero, 0, nullptr);
    cl_->OMSetRenderTargets(1, &rh, FALSE, &dh);
    SetFullViewport(rt.w, rt.h);
    ID3D12PipelineState* bound = nullptr;
    for (const Caster& c : casters_) {
        if (!c.screen) continue;
        if (c.screen != bound) SetPso(bound = c.screen);
        cl_->SetGraphicsRootConstantBufferView(0, c.vc);
        cl_->SetGraphicsRootConstantBufferView(1, c.dc);
        cl_->IASetVertexBuffers(0, c.vb_count, c.vb);
        SetTopology(c.tess ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST : c.topo);
        if (c.indexed) {
            cl_->IASetIndexBuffer(&c.ib);
            cl_->DrawIndexedInstanced(c.count, 1, 0, c.base, 0);
        } else {
            cl_->DrawInstanced(c.count, 1, 0, 0);
        }
    }
    Transition(scene_[2].Get(), scene_state_[2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    bound_rt_ = bound_ds_ = nullptr;
    return true;
}

// the screen's shadow mask (shadow_shaders.h) into scene_[kSceneShadow]; the depth is left readable
ID3D12Resource* D3D12Backend::ShadowMask(Target& rt, Target& ds) {
    std::string text = kShadowMask;
    ID3D12PipelineState* pso = EnsurePassPipeline(text.c_str(), "mask", DXGI_FORMAT_R8G8B8A8_UNORM, -1);
    if (!pso) return nullptr;
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    uint32_t slot = AllocSrvSlots(7);
    bool chars = CharacterMask(rt, ds);
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    std::pair<ID3D12Resource*, DXGI_FORMAT> src[4] = {
        {ds.res.Get(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS}, {shadow_atlas_.Get(), DXGI_FORMAT_R32_FLOAT}, {scene_[0].Get(), rt.fmt},
        {chars ? scene_[2].Get() : nullptr, DXGI_FORMAT_R8_UNORM}};
    for (uint32_t i = 0; i < 7; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 4 ? src[i].second : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 4 ? src[i].first : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    // the parameters (kShadowMask's cbuffer)
    struct Params {
        float p[9][4];
        float tile[4][4];
        float cas[4][16];
    } P{};
    const float* V = cam_view_;
    float L[3];
    for (int j = 0; j < 3; j++) L[j] = light_.dir[0] * V[j] + light_.dir[1] * V[4 + j] + light_.dir[2] * V[8 + j];
    float n = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    const shadow::Quality& q = shadow_q_;
    float vw[16];
    if (!shadow::Invert(cam_view_, vw)) return nullptr;
    float p[9][4] = {
        {1 / cam_proj_[0], 1 / cam_proj_[5], cam_proj_[10], cam_proj_[14]},
        {L[0] / n, L[1] / n, L[2] / n, light_.weight * light_.strength},
        {float(rt.w), float(rt.h), 1.0f / rt.w, 1.0f / rt.h},
        {light_.direct, light_.ambient, q.distance * 0.82f, q.distance},
        {cascades_.far_z[0], cascades_.far_z[1], cascades_.far_z[2], cascades_.far_z[3]},
        {float(cascades_.count), float(q.tile * 2), q.softness, float(q.taps)},
        {cascades_.texel[0], cascades_.texel[1], cascades_.texel[2], cascades_.texel[3]},
        {},
        {cam_min_z_, cam_max_z_ - cam_min_z_, ShadowDebug(),
         app::Get().soft_shadows ? 1.0f : 0.0f}};
    for (int c = 0; c < 4; c++) p[7][c] = cascades_.depth_range[c] > 0 ? 1 / cascades_.depth_range[c] : 0;
    std::memcpy(P.p, p, sizeof(p));
    for (int c = 0; c < cascades_.count; c++) {
        P.tile[c][0] = 0.5f, P.tile[c][1] = 0.5f, P.tile[c][2] = float(c & 1) * 0.5f, P.tile[c][3] = float(c >> 1) * 0.5f;
        shadow::Mul(vw, cascades_.world_to_light[c], P.cas[c]);
    }
    Transition(scene_[kSceneShadow].Get(), scene_state_[kSceneShadow], D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = SceneRtv(kSceneShadow);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(rt.w, rt.h);
    Alloc cb = Upload(sizeof(P), 256);
    std::memcpy(cb.cpu, &P, sizeof(P));
    std::memcpy(mask_params_, &P, sizeof(P));
    mask_params_ok_ = true;
    DrawFullScreen(pso, cb.gpu, slot);
    Transition(scene_[kSceneShadow].Get(), scene_state_[kSceneShadow], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ShadowTimestamp(2);
    bound_rt_ = bound_ds_ = nullptr;
    return scene_[kSceneShadow].Get();
}

// The volumetric haze (kShadowMask's fog, half size, into scene_[kSceneFog]): the shadow mask's parameters, the haze's
// own: a density from the game's fog (its end: thicker where the level fogs near) or a light one, the ground's height
// (the eye 1.6 m over it), the sun's light by its strength. Added in the composite.
void D3D12Backend::FogPass(Target& rt, Target& ds) {
    if (!mask_params_ok_ || !scene_[kSceneFog]) return;
    std::string text = kShadowMask;
    ID3D12PipelineState* pso = EnsurePassPipeline(text.c_str(), "fog", DXGI_FORMAT_R16G16B16A16_FLOAT, -1);
    if (!pso) return;
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    uint32_t slot = AllocSrvSlots(7);
    std::pair<ID3D12Resource*, DXGI_FORMAT> src[2] = {{ds.res.Get(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
                                                      {shadow_atlas_.Get(), DXGI_FORMAT_R32_FLOAT}};
    for (uint32_t i = 0; i < 7; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 2 ? src[i].second : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 2 ? src[i].first : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    constexpr size_t kMaskFloats = sizeof(mask_params_) / sizeof(float);
    float P[kMaskFloats + 8];
    std::memcpy(P, mask_params_, sizeof(mask_params_));
    // the haze's density: about a tenth hidden at 60 m, more where the game's own fog closes in
    float density = 0.0000175f;
    if (fog_seen_ && fog_[4] > fog_[3] && fog_[4] > 100) density = std::clamp(1.5f / fog_[4], 0.000008f, 0.00005f);
    // the world's up in view space, the ground 1.6 m under the eye
    float up[3] = {cam_view_[4], cam_view_[5], cam_view_[6]};
    float sunlight = std::clamp(light_.direct * 1.4f, 0.0f, 1.2f);
    float p9[4] = {density, -160, sunlight, 0}, p10[4] = {up[0], up[1], up[2], 0};
    std::memcpy(P + kMaskFloats, p9, sizeof(p9));
    std::memcpy(P + kMaskFloats + 4, p10, sizeof(p10));
    // the half size target's size (the parameters' p2 are the full one's: the march only needs uv)
    uint32_t w = std::max(1u, rt.w / 2), h = std::max(1u, rt.h / 2);
    Transition(scene_[kSceneFog].Get(), scene_state_[kSceneFog], D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = SceneRtv(kSceneFog);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(w, h);
    Alloc cb = Upload(sizeof(P), 256);
    std::memcpy(cb.cpu, P, sizeof(P));
    DrawFullScreen(pso, cb.gpu, slot);
    Transition(scene_[kSceneFog].Get(), scene_state_[kSceneFog], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    bound_rt_ = bound_ds_ = nullptr;
    // the haze's colour for the composite: the game's fog's, else a cool grey of the sky's
    fog_tint_[0] = fog_seen_ ? fog_[0] : 0.55f, fog_tint_[1] = fog_seen_ ? fog_[1] : 0.6f, fog_tint_[2] = fog_seen_ ? fog_[2] : 0.68f;
    fog_done_ = true;
}

// XBE_SHADOW_STATS: the GPU's time on the atlas and on the mask, averaged and logged every few seconds
void D3D12Backend::ShadowTimestamp(uint32_t i) {
    static const bool on = std::getenv("XBE_SHADOW_STATS") != nullptr;
    if (!on) return;
    if (!shadow_queries_) {
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4 * kFrames, 0};
        if (FAILED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&shadow_queries_)))) return;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(8 * 4 * kFrames);
        if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&shadow_readback_))))
            return;
    }
    uint32_t slot = uint32_t(frame_ % kFrames);
    cl_->EndQuery(shadow_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 4 + i);
    if (i == 2) {
        cl_->ResolveQueryData(shadow_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 4, 3, shadow_readback_.Get(), slot * 32);
        shadow_timed_[slot] = true;
    }
}

void D3D12Backend::GpuMark(const char* name) {
    if (gpu::g_frame_stats.load() != 1) return;
    if (!marks_) {
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kMarks * kFrames, 0};
        if (FAILED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&marks_)))) return;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(8 * kMarks * kFrames);
        if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&marks_rb_)))) {
            marks_ = nullptr;
            return;
        }
    }
    uint32_t slot = uint32_t(frame_ % kFrames);
    if (mark_n_[slot] >= kMarks) return;
    cl_->EndQuery(marks_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kMarks + mark_n_[slot]);
    mark_names_[slot][mark_n_[slot]++] = name;
}

void D3D12Backend::ResolveGpuMarks() {
    uint32_t slot = uint32_t(frame_ % kFrames);
    if (!marks_ || !mark_n_[slot]) return;
    cl_->ResolveQueryData(marks_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kMarks, mark_n_[slot], marks_rb_.Get(),
                          uint64_t(slot) * kMarks * 8);
    marks_resolved_[slot] = true;
}

// the frame's steps, kept for the report
struct GpuSteps {
    std::vector<std::pair<const char*, float>> steps;
    float total = 0;
};
std::vector<GpuSteps> g_gpu_frames;

void D3D12Backend::ReadGpuMarks() {
    uint32_t slot = uint32_t(frame_ % kFrames);
    uint32_t n = mark_n_[slot];
    mark_n_[slot] = 0;
    if (!marks_resolved_[slot]) return;
    marks_resolved_[slot] = false;
    uint64_t* t = nullptr;
    D3D12_RANGE range{size_t(slot) * kMarks * 8, (size_t(slot) * kMarks + n) * 8};
    if (FAILED(marks_rb_->Map(0, &range, reinterpret_cast<void**>(&t)))) return;
    uint64_t freq = 0;
    queue_->GetTimestampFrequency(&freq);
    const uint64_t* q = t + size_t(slot) * kMarks;
    GpuSteps f;
    for (uint32_t i = 1; i < n && freq; i++) {
        float ms = q[i] >= q[i - 1] ? float(double(q[i] - q[i - 1]) * 1000 / double(freq)) : 0.0f;
        f.steps.push_back({mark_names_[slot][i], ms});
        f.total += ms;
    }
    D3D12_RANGE none{0, 0};
    marks_rb_->Unmap(0, &none);
    if (gpu::g_frame_stats.load() == 1 && g_gpu_frames.size() < 200000) g_gpu_frames.push_back(std::move(f));
}

void D3D12Backend::ReadShadowTimes() {
    uint32_t slot = uint32_t(frame_ % kFrames);
    if (!shadow_readback_ || !shadow_timed_[slot]) return;
    shadow_timed_[slot] = false;
    uint64_t* t = nullptr;
    D3D12_RANGE range{slot * 32, slot * 32 + 24};
    if (FAILED(shadow_readback_->Map(0, &range, reinterpret_cast<void**>(&t)))) return;
    uint64_t freq = 0;
    queue_->GetTimestampFrequency(&freq);
    const uint64_t* q = t + slot * 4;
    if (freq && q[1] >= q[0] && q[2] >= q[1]) {
        shadow_ms_[0] += double(q[1] - q[0]) * 1000 / freq;
        shadow_ms_[1] += double(q[2] - q[1]) * 1000 / freq;
        shadow_ms_n_++;
    }
    D3D12_RANGE none{0, 0};
    shadow_readback_->Unmap(0, &none);
    if (shadow_ms_n_ >= 600) {
        LOGR("shadows: GPU %.3f ms atlas (%.0f casters a frame), %.3f ms mask, quality %u, light %.2f %.2f %.2f (%s)",
             shadow_ms_[0] / shadow_ms_n_, double(shadow_casters_n_) / shadow_ms_n_, shadow_ms_[1] / shadow_ms_n_,
             app::Get().shadow_quality, light_.dir[0], light_.dir[1], light_.dir[2], shadow::SourceName(light_.source));
        shadow_ms_[0] = shadow_ms_[1] = shadow_ms_n_ = 0;
        shadow_casters_n_ = 0;
    }
}

// the scene passes' targets: the scene's copy in its format, the occlusion twice, the bloom at half to a 16th
bool D3D12Backend::EnsureSceneTargets(DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
    if (scene_[kSceneRtvs - 1] && scene_w_ == w && scene_h_ == h && scene_fmt_ == fmt) return true;
    sun_vis_ok_ = false;
    for (auto& r : scene_)
        if (r) Release(std::move(r));
    scene_w_ = scene_h_ = 0;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    for (uint32_t i = 0; i < kSceneRtvs; i++) {
        DXGI_FORMAT f = i == 0 || i == kSceneHist ? fmt
                        : i < 3                   ? DXGI_FORMAT_R8_UNORM
                        : i == kSceneShadow       ? DXGI_FORMAT_R8G8B8A8_UNORM
                                                  : DXGI_FORMAT_R16G16B16A16_FLOAT;
        uint32_t div = i < 3 || i == kSceneShadow || i >= kSceneVel ? 1 : i >= kSceneRays ? 2 : 2u << (i - 3);
        D3D12_RESOURCE_DESC td = Texture2DDesc(f, std::max(1u, w / div), std::max(1u, h / div));
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = f;
        scene_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, scene_state_[i], &cv, IID_PPV_ARGS(&scene_[i])))) {
            for (auto& r : scene_) r.Reset();
            return false;
        }
        dev_->CreateRenderTargetView(scene_[i].Get(), nullptr, SceneRtv(i));
    }
    scene_w_ = w, scene_h_ = h, scene_fmt_ = fmt;
    taa_hist_ok_ = false;
    return true;
}

// a full screen pass of kSceneCommon and hlsl into scene_[out], the sources at t0 on, 16 parameters at b1
void D3D12Backend::ScenePass(const std::string& hlsl, const char* entry, uint32_t out, DXGI_FORMAT out_fmt, uint32_t w,
                             uint32_t h, std::initializer_list<std::pair<ID3D12Resource*, DXGI_FORMAT>> src, const float p[16],
                             float blend) {
    std::string text = std::string(kSceneCommon) + hlsl;
    ID3D12PipelineState* pso = EnsurePassPipeline(text.c_str(), entry, out_fmt, blend);
    if (!pso) return;
    uint32_t slot = AllocSrvSlots(7);
    uint32_t i = 0;
    for (auto [r, f] : src) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = f;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(r, &sv, Cpu(srv_heap_.Get(), slot + i++, srv_inc_));
    }
    for (; i < 6; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Transition(scene_[out].Get(), scene_state_[out], D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = SceneRtv(out);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(w, h);
    Alloc cb = Upload(1536, 256);
    std::memcpy(cb.cpu, p, 16 * sizeof(float));
    std::memcpy(cb.cpu + kSkyConstantsAt * sizeof(float), sky_cb_, sizeof(sky_cb_));
    if (blend >= 0) {
        const float f[4] = {blend, blend, blend, blend};
        SetBlendFactor(f);
    }
    DrawFullScreen(pso, cb.gpu, slot);
    Transition(scene_[out].Get(), scene_state_[out], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    bound_rt_ = bound_ds_ = nullptr;
    GpuMark(entry);
}

// a pass of kSceneComposite into the scene's target itself, reading scene_[0] (its copy), the occlusion and the bloom
void D3D12Backend::SceneComposite(Target& rt, const char* entry, ID3D12Resource* occ, bool bloom, const float p[16], ID3D12Resource* depth,
                                  ID3D12Resource* shadow, bool lights) {
    std::string text = std::string(kSceneCommon) + kSceneComposite;
    ID3D12PipelineState* pso = EnsurePassPipeline(text.c_str(), entry, rt.fmt, -1);
    if (!pso) return;
    uint32_t slot = AllocSrvSlots(7);
    {
        // the shadow mask at t6, without one it reads 1 (the atlas debug view: the atlas)
        bool atlas = shadow && shadow_atlas_ && std::strcmp(entry, "shadow_atlas") == 0;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = atlas ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = shadow ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING : D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(5, 5, 5, 5);
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(atlas ? shadow_atlas_.Get() : shadow, &sv, Cpu(srv_heap_.Get(), slot + 6, srv_inc_));
    }
    constexpr DXGI_FORMAT kF16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    std::pair<ID3D12Resource*, DXGI_FORMAT> src[6] = {{scene_[0].Get(), rt.fmt}, {occ, DXGI_FORMAT_R8_UNORM},
                                                      {scene_[3].Get(), kF16},   {scene_[4].Get(), kF16},
                                                      {scene_[5].Get(), kF16},   {scene_[6].Get(), kF16}};
    for (uint32_t i = 0; i < 6; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        bool none = !src[i].first || (i >= 2 && !bloom);
        sv.Format = none ? (i == 1 ? DXGI_FORMAT_R8_UNORM : kBgra) : src[i].second;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        // without occlusion the view reads 1 everywhere
        sv.Shader4ComponentMapping = i == 1 && !occ ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(5, 5, 5, 5)
                                                     : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(none ? nullptr : src[i].first, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    // the bounce at t5 (no bloom here)
    if (!bloom && gi_ready_) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(scene_[kSceneGi].Get(), &sv, Cpu(srv_heap_.Get(), slot + 5, srv_inc_));
    }
    // the world's depth at t2 for the clouds' shadows (no bloom here)
    if (depth && !bloom) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(depth, &sv, Cpu(srv_heap_.Get(), slot + 2, srv_inc_));
    }
    // the glowing sprites' lights: the world's depth at t1 (kept before the weapon's clear), the depth now at t6
    constexpr uint32_t kMaxLights = 32;
    float lpos[kMaxLights * 4], lcol[kMaxLights * 4];
    uint32_t nlights = lights && scene_ds_ ? DynLightConstants(lpos, lcol, kMaxLights) : 0;
    Target* lit_ds = nlights ? scene_ds_ : nullptr;
    if (lit_ds) {
        Transition(lit_ds->res.Get(), lit_ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (world_depth_saved_) Transition(world_depth_.Get(), world_depth_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        for (uint32_t i : {1u, 6u}) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            ID3D12Resource* r = i == 1 && world_depth_saved_ ? world_depth_.Get() : lit_ds->res.Get();
            dev_->CreateShaderResourceView(r, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
        }
    }
    Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), rt.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(rt.w, rt.h);
    Alloc cb = Upload(1536, 256);
    std::memset(cb.cpu, 0, 1536);
    std::memcpy(cb.cpu, p, 16 * sizeof(float));
    {
        static const float cloud_debug = std::getenv("XBE_CLOUD_DEBUG") ? 1.0f : 0.0f;
        float clouds[4] = {0, depth && !bloom ? 1.0f : 0.0f, cloud_debug, 0};
        std::memcpy(cb.cpu + (kGradeAt + 12) * sizeof(float), clouds, sizeof(clouds));
        float bounce = !bloom && gi_ready_ ? 1.2f : 0.0f;
        std::memcpy(cb.cpu + kGradeAt * sizeof(float), &bounce, sizeof(bounce));
        // the camera's place in the world (the view's inverse's translation) at grade[1]
        float inv[16];
        if (cam_ok_ && shadow::Invert(cam_view_, inv)) {
            float eye[4] = {inv[12], inv[13], inv[14], 0};
            std::memcpy(cb.cpu + (kGradeAt + 4) * sizeof(float), eye, sizeof(eye));
        }
        // the sky's (the frame before's: the occlusion runs before this frame's are made)
        std::memcpy(cb.cpu + kSkyConstantsAt * sizeof(float), sky_cb_, sizeof(sky_cb_));
    }
    {
        float count[4] = {float(nlights), 0, 0, 0};
        std::memcpy(cb.cpu + 20 * sizeof(float), count, sizeof(count));
        std::memcpy(cb.cpu + 24 * sizeof(float), lpos, nlights * 4 * sizeof(float));
        std::memcpy(cb.cpu + (24 + kMaxLights * 4) * sizeof(float), lcol, nlights * 4 * sizeof(float));
    }
    // the player's pain and the time, for the composite's shake
    // (the game's own pain lasts a third of a second: the shake lets go over half a second more)
    static const auto t0 = std::chrono::steady_clock::now();
    static float last = 0;
    float& shown = hurt_shown_;
    float now = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    shown = std::max(std::clamp(g_hurt.load(), 0.0f, 1.0f), shown - std::clamp(now - last, 0.0f, 0.1f) / 0.5f);
    last = now;
    float hurt[4] = {std::max(shown, 0.0f), std::fmod(now, 1000.0f), 0, 0};
    std::memcpy(cb.cpu + 16 * sizeof(float), hurt, sizeof(hurt));
    DrawFullScreen(pso, cb.gpu, slot);
    if (lit_ds) Transition(lit_ds->res.Get(), lit_ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    bound_rt_ = bound_ds_ = nullptr;
    GpuMark(entry);
}

bool D3D12Backend::EnsureInvVp() {
    if (!cam_ok_) return false;
    if (!inv_vp_ok_ || std::memcmp(inv_vp_of_, cam_view_, 64) || std::memcmp(inv_vp_of_ + 16, cam_proj_, 64)) {
        float vp[16];
        shadow::Mul(cam_view_, cam_proj_, vp);
        inv_vp_ok_ = shadow::Invert(vp, inv_vp_);
        std::memcpy(inv_vp_of_, cam_view_, 64), std::memcpy(inv_vp_of_ + 16, cam_proj_, 64);
    }
    return inv_vp_ok_;
}

// The weapon in hand on a wide screen. Its model was made for the console's 4:3 view: past it an arm goes on to the
// screen's edge, or ends (its cut end, or a piece the console never showed, alone in a corner). Its triangles joined
// into the pieces they make on the screen (shared corners, and corners at the same place), a piece any of whose
// triangles the 4:3 view shows is drawn whole, a piece wholly outside it is left out. The program runs here for each
// corner (as for the glowing particles). False: not worked out (the geometry shader's triangle test instead).
bool D3D12Backend::WeaponPieces(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (!s.vs || !bound_rt_) return false;
    const auto started = std::chrono::steady_clock::now();
    auto it = cpu_prog_.find(sh.vkey);
    if (it == cpu_prog_.end()) it = cpu_prog_.emplace(sh.vkey, DecodeCpuProgram(s.vs->program, a.regs)).first;
    const CpuProgram* prog = it->second.get();
    if (!prog) return false;
    auto input = [&](uint32_t r, uint32_t i, float v[4]) {
        v[0] = v[1] = v[2] = 0, v[3] = 1;
        const VertexElement& e = a.elements[r];
        if (!(a.regs & (1u << r)) || e.buffer >= a.buffer_count) return;
        const VertexBuffer& b = a.buffers[e.buffer];
        if (!b.src || !b.stride || size_t(i) * b.stride + e.offset + 4 > b.bytes) return;
        const uint8_t* at = b.src + size_t(i) * b.stride + e.offset;
        switch (e.format) {
            case DXGI_FORMAT_R32_FLOAT: std::memcpy(v, at, 4); break;
            case DXGI_FORMAT_R32G32_FLOAT: std::memcpy(v, at, 8); break;
            case DXGI_FORMAT_R32G32B32_FLOAT: std::memcpy(v, at, 12); break;
            case DXGI_FORMAT_R32G32B32A32_FLOAT: std::memcpy(v, at, 16); break;
            case DXGI_FORMAT_B8G8R8A8_UNORM: v[0] = at[2] / 255.0f, v[1] = at[1] / 255.0f, v[2] = at[0] / 255.0f, v[3] = at[3] / 255.0f; break;
            case DXGI_FORMAT_R8G8B8A8_UNORM: for (int k = 0; k < 4; k++) v[k] = at[k] / 255.0f; break;
            case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16B16A16_SNORM: {
                int16_t q[4] = {};
                const int m = e.format == DXGI_FORMAT_R16G16_SNORM ? 2 : 4;
                std::memcpy(q, at, size_t(m) * 2);
                for (int k = 0; k < m; k++) v[k] = std::max(q[k] / 32767.0f, -1.0f);
                break;
            }
            default: break;
        }
    };
    uint32_t vcount = a.vertex_count;
    if (!vcount) {
        const VertexElement& e = a.elements[0];
        if (e.buffer < a.buffer_count && a.buffers[e.buffer].stride) vcount = a.buffers[e.buffer].bytes / a.buffers[e.buffer].stride;
    }
    if (!vcount || vcount > 16384) return false;
    // the triangles, in DrawGeometry's order (a strip's degenerate joins counted, kept)
    const uint32_t n = a.index_count ? a.index_count : a.draw_count;
    auto index = [&](uint32_t i) -> int64_t {
        int64_t v = !a.index_count ? int64_t(i)
                                   : int64_t(a.index16 ? static_cast<const uint16_t*>(a.index_src)[i] : static_cast<const uint32_t*>(a.index_src)[i]);
        return v + (a.index_count ? a.base_vertex : 0);
    };
    struct Tri {
        uint32_t v[3];
        bool ok;
    };
    std::vector<Tri> tris;
    if (a.strip) {
        for (uint32_t i = 2; i < n; i++) tris.push_back({{uint32_t(index(i - 2)), uint32_t(index(i - 1)), uint32_t(index(i))}, true});
    } else {
        for (uint32_t i = 0; i + 2 < n; i += 3) tris.push_back({{uint32_t(index(i)), uint32_t(index(i + 1)), uint32_t(index(i + 2))}, true});
    }
    // each corner's place on the screen (the console's pixels, the wide view's; w the view depth), worked out once
    const float gw = float(bound_rt_->gw), gh = float(bound_rt_->gh), half = 1.0f / widen_;
    std::vector<std::array<float, 3>> scr(vcount);
    std::vector<uint8_t> done(vcount, 0);
    std::vector<uint32_t> parent(vcount);
    for (uint32_t i = 0; i < vcount; i++) parent[i] = i;
    auto find = [&](uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    auto join = [&](uint32_t x, uint32_t y) { parent[find(x)] = find(y); };
    std::unordered_map<uint64_t, uint32_t> at_place;  // corners at the same place (a model's seams) as one
    auto corner = [&](uint32_t v) -> const std::array<float, 3>& {
        if (!done[v]) {
            float in[16][4], o[4];
            for (uint32_t r = 0; r < 16; r++) input(r, v, in[r]);
            RunCpuProgram(*prog, in, s.vsc, o);
            bool ok = o[3] > 0 && std::isfinite(o[0]) && std::isfinite(o[1]);
            scr[v] = {ok ? o[0] * 2 / gw - 1 : 0.0f, ok ? 1 - o[1] * 2 / gh : 0.0f, ok ? 1.0f : 0.0f};
            done[v] = 1;
            uint64_t key = (uint64_t(uint32_t(std::lround(in[0][0] * 64))) << 42) ^ (uint64_t(uint32_t(std::lround(in[0][1] * 64))) << 21) ^
                           uint64_t(uint32_t(std::lround(in[0][2] * 64)));
            auto [p, fresh] = at_place.emplace(key, v);
            if (!fresh) join(v, p->second);
        }
        return scr[v];
    };
    // on the screen: joined into pieces; a piece the 4:3 view shows is kept
    std::vector<uint8_t> on(tris.size(), 0), in43(tris.size(), 0);
    for (size_t t = 0; t < tris.size(); t++) {
        Tri& tr = tris[t];
        if (tr.v[0] >= vcount || tr.v[1] >= vcount || tr.v[2] >= vcount) return false;
        if (tr.v[0] == tr.v[1] || tr.v[1] == tr.v[2] || tr.v[0] == tr.v[2]) continue;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        bool behind = false;
        for (uint32_t v : tr.v) {
            const auto& q = corner(v);
            if (q[2] == 0) behind = true;
            x0 = std::min(x0, q[0]), x1 = std::max(x1, q[0]), y0 = std::min(y0, q[1]), y1 = std::max(y1, q[1]);
        }
        if (behind) continue;  // (through the eye: not on the screen as a triangle; kept)
        // the triangle itself against a rectangle (its corners clipped by the rectangle's sides; something left: it
        // shows there), not its bounding box: a long thin one by a corner has a box reaching in, and none of it does
        auto overlaps = [&](float rx0, float rx1, float ry0, float ry1) {
            if (x1 < rx0 || x0 > rx1 || y1 < ry0 || y0 > ry1) return false;
            float poly[2][12][2];
            int n = 3, cur = 0;
            for (int k = 0; k < 3; k++) poly[0][k][0] = scr[tr.v[k]][0], poly[0][k][1] = scr[tr.v[k]][1];
            for (int side = 0; side < 4 && n > 0; side++) {
                const int axis = side / 2;
                const float lim = side == 0 ? rx0 : side == 1 ? rx1 : side == 2 ? ry0 : ry1;
                const bool keep_greater = side == 0 || side == 2;
                int m = 0;
                for (int i = 0; i < n && m < 10; i++) {
                    const float* a0 = poly[cur][i];
                    const float* b0 = poly[cur][(i + 1) % n];
                    float da = keep_greater ? a0[axis] - lim : lim - a0[axis];
                    float db = keep_greater ? b0[axis] - lim : lim - b0[axis];
                    if (da >= 0) poly[1 - cur][m][0] = a0[0], poly[1 - cur][m][1] = a0[1], m++;
                    if ((da >= 0) != (db >= 0) && m < 10) {
                        float f = da / (da - db);
                        poly[1 - cur][m][0] = a0[0] + (b0[0] - a0[0]) * f, poly[1 - cur][m][1] = a0[1] + (b0[1] - a0[1]) * f, m++;
                    }
                }
                n = m, cur = 1 - cur;
            }
            return n >= 3;
        };
        on[t] = overlaps(-1, 1, -1, 1);
        in43[t] = on[t] && overlaps(-half, half, -1, 1);
        if (on[t]) join(tr.v[0], tr.v[1]), join(tr.v[1], tr.v[2]);
    }
    std::unordered_set<uint32_t> shown;
    for (size_t t = 0; t < tris.size(); t++)
        if (in43[t]) shown.insert(find(tris[t].v[0]));
    weapon_keep_.assign(tris.size(), 1);
    size_t dropped = 0;
    for (size_t t = 0; t < tris.size(); t++)
        if (on[t] && !shown.count(find(tris[t].v[0]))) weapon_keep_[t] = 0, dropped++;
    // tests (cmd weaponwhole 3 / 4): the triangles wholly outside the 4:3 view wound one way (a strip's alternate
    // triangles counted as the strip draws them) left out, the rest drawn
    if (int ww = g_weapon_whole.load(std::memory_order_relaxed); ww == 3 || ww == 4) {
        weapon_keep_.assign(tris.size(), 1), dropped = 0;
        for (size_t t = 0; t < tris.size(); t++) {
            if (!on[t] || in43[t]) continue;
            const auto &p0 = scr[tris[t].v[0]], &p1 = scr[tris[t].v[1]], &p2 = scr[tris[t].v[2]];
            float area = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
            if (a.strip && (t & 1)) area = -area;
            if ((ww == 3) == (area > 0)) weapon_keep_[t] = 0, dropped++;
        }
    }
    weapon_filter_ = dropped > 0;
    static const bool log = std::getenv("XBE_WEAPON_LOG") != nullptr;
    if (log && presents_ % 120 == 0) {
        LOGR("weapon: tex %016llx %zu triangles, %zu left out, %.3f ms", (unsigned long long)draw_tex0_, tris.size(), dropped,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        // each piece on the screen: its triangles, its box, shown by the 4:3 view
        struct Box { float x0 = 9, x1 = -9, y0 = 9, y1 = -9; int n = 0; };
        std::unordered_map<uint32_t, Box> boxes;
        for (size_t t = 0; t < tris.size(); t++) {
            if (!on[t]) continue;
            Box& b = boxes[find(tris[t].v[0])];
            b.n++;
            for (uint32_t v : tris[t].v) b.x0 = std::min(b.x0, scr[v][0]), b.x1 = std::max(b.x1, scr[v][0]), b.y0 = std::min(b.y0, scr[v][1]), b.y1 = std::max(b.y1, scr[v][1]);
        }
        for (auto& [root, b] : boxes)
            LOGR("weapon:   piece %u: %d on screen, x %.2f..%.2f y %.2f..%.2f%s", root, b.n, b.x0, b.x1, b.y0, b.y1, shown.count(root) ? " shown" : "");
    }
    return true;
}

// A glowing sprite's light (g_emitter by its texture): a light at each sprite of the draw (its corners, four or three
// vertices), in the world: a fixed function draw's through its world matrix; a program's run here for each vertex
// (the particles' programs read their places from the constants by a0), its screen position back through the
// camera. Sprites close together make one light.
void D3D12Backend::CollectEmitter(const State& s, const Assembled& a, const DrawShaders& sh, const EmitterKind& kind) {
    if (s.layout2d || a.pretransformed || !bound_rt_ || bound_rt_->gw != guest_w_ || bound_rt_->gh != guest_h_) return;
    const CpuProgram* prog = nullptr;
    float inv_view[16];
    if (sh.prog) {
        auto it = cpu_prog_.find(sh.vkey);
        if (it == cpu_prog_.end()) it = cpu_prog_.emplace(sh.vkey, DecodeCpuProgram(s.vs->program, a.regs)).first;
        prog = it->second.get();
        if (!cam_ok_ || cam_proj_[0] == 0 || cam_proj_[5] == 0 || !shadow::Invert(cam_view_, inv_view)) return;
    }
    // a register's value at a vertex, as the input assembler gives it
    auto input = [&](uint32_t r, uint32_t i, float v[4]) {
        v[0] = v[1] = v[2] = 0, v[3] = 1;
        const VertexElement& e = a.elements[r];
        if (!(a.regs & (1u << r)) || e.buffer >= a.buffer_count) return false;
        const VertexBuffer& b = a.buffers[e.buffer];
        if (!b.src || !b.stride || size_t(i) * b.stride + e.offset + 4 > b.bytes) return false;
        const uint8_t* at = b.src + size_t(i) * b.stride + e.offset;
        switch (e.format) {
            case DXGI_FORMAT_R32_FLOAT: std::memcpy(v, at, 4); return true;
            case DXGI_FORMAT_R32G32_FLOAT: std::memcpy(v, at, 8); return true;
            case DXGI_FORMAT_R32G32B32_FLOAT: std::memcpy(v, at, 12); return true;
            case DXGI_FORMAT_R32G32B32A32_FLOAT: std::memcpy(v, at, 16); return true;
            case DXGI_FORMAT_B8G8R8A8_UNORM: v[0] = at[2] / 255.0f, v[1] = at[1] / 255.0f, v[2] = at[0] / 255.0f, v[3] = at[3] / 255.0f; return true;
            case DXGI_FORMAT_R8G8B8A8_UNORM: for (int k = 0; k < 4; k++) v[k] = at[k] / 255.0f; return true;
            default: return false;
        }
    };
    // a vertex's place in the world
    const float* W = s.transforms[kTsWorld].data();
    const float gw = float(bound_rt_->gw), gh = float(bound_rt_->gh);
    auto place = [&](uint32_t i, float q[3]) {
        if (!prog) {
            float p[4];
            if (!input(0, i, p)) return false;
            for (int k = 0; k < 3; k++) q[k] = p[0] * W[k] + p[1] * W[4 + k] + p[2] * W[8 + k] + W[12 + k];
            return true;
        }
        float in[16][4], o[4];
        for (uint32_t r = 0; r < 16; r++) input(r, i, in[r]);
        RunCpuProgram(*prog, in, s.vsc, o);
        // oPos: the console's screen pixels, w the view depth
        float z = o[3];
        if (!(z > 1.0f)) return false;
        float nx = o[0] * 2 / gw - 1, ny = 1 - o[1] * 2 / gh;
        float v[3] = {nx * z / cam_proj_[0], ny * z / cam_proj_[5], z};
        for (int k = 0; k < 3; k++) q[k] = v[0] * inv_view[k] + v[1] * inv_view[4 + k] + v[2] * inv_view[8 + k] + inv_view[12 + k];
        return true;
    };
    uint32_t n = a.vertex_count;
    if (!n) {
        const VertexElement& e = a.elements[0];
        if (e.buffer < a.buffer_count && a.buffers[e.buffer].stride) n = a.buffers[e.buffer].bytes / a.buffers[e.buffer].stride;
    }
    n = std::min<uint32_t>(n, 4096);
    // the vertex colour fades a particle
    const bool col = (a.regs & 8) && a.elements[3].format == DXGI_FORMAT_B8G8R8A8_UNORM;
    uint32_t chunk = n % 4 == 0 ? 4 : n % 3 == 0 ? 3 : n;
    if (!chunk) return;
    // a fire of hundreds of sprites makes a few lights: two dozen of its sprites at most, a program's at one corner
    // each (it runs here, a sprite's corner is as good as its middle for a light this size)
    uint32_t sprites = n / chunk, step = std::max(1u, sprites / 24u), corners = prog ? 1 : chunk;
    static const bool log = std::getenv("XBE_EMIT_LOG") != nullptr;
    for (uint32_t first = 0; first + chunk <= n; first += chunk * step) {
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, sum[3] = {}, glow = 0, tint[3] = {};
        uint32_t got = 0;
        for (uint32_t i = first; i < first + corners; i++) {
            float q[3];
            if (!place(i, q)) continue;
            for (int k = 0; k < 3; k++) sum[k] += q[k], lo[k] = std::min(lo[k], q[k]), hi[k] = std::max(hi[k], q[k]);
            float c[4];
            if (col && input(3, i, c)) {
                glow += c[3] * std::max({c[0], c[1], c[2]});
                for (int k = 0; k < 3; k++) tint[k] += c[k];
            } else {
                glow += 1;
                for (int k = 0; k < 3; k++) tint[k] += 1;
            }
            got++;
        }
        if (!got) continue;
        // a hit's sprite (Water Interaction): its place, for the water it may land on
        if (kind.impact > 0 && impacts_frame_.size() < 64)
            impacts_frame_.push_back({{sum[0] / got, sum[1] / got, sum[2] / got}, kind.impact, 0});
        if (!kind.light) continue;
        float strength = glow / float(got);
        if (strength < 0.03f) continue;
        float size = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
        if (size > 2500) continue;  // a backdrop painted with it (a city burning far away), not a flame
        // a tinted one: the sprite's own colour (its hue at full brightness, times the kind's strength)
        float lc[3] = {kind.color[0], kind.color[1], kind.color[2]};
        if (kind.tint) {
            float m = std::max({tint[0], tint[1], tint[2], 1e-6f});
            for (int k = 0; k < 3; k++) lc[k] = kind.color[k] * tint[k] / m;
        }
        DynLight l{{sum[0] / got, sum[1] / got, sum[2] / got}, kind.radius, {lc[0], lc[1], lc[2]},
                   strength, 0, kind.hold, kind.flicker, kind.heat, kind.flash, kind.shadows};
        if (kind.size > 0 && got > 1) l.radius *= std::clamp(size / kind.size, 0.5f, 3.0f);
        if (log) {
            static int logged = 0;
            if (logged++ % 25 == 0)
                LOGR("emit: light %016llx prog %d at %.0f %.0f %.0f size %.0f radius %.0f strength %.2f (%u vertices)",
                     (unsigned long long)draw_tex0_, int(sh.prog), l.pos[0], l.pos[1], l.pos[2], size, l.radius, strength, n);
        }
        bool merged = false;
        for (DynLight& m : dyn_frame_) {
            float d2 = 0;
            for (int k = 0; k < 3; k++) d2 += (m.pos[k] - l.pos[k]) * (m.pos[k] - l.pos[k]);
            float r = 0.5f * std::max(m.radius, l.radius);
            if (d2 < r * r && m.hold == l.hold && m.color[0] == l.color[0]) {
                float w = l.strength / (m.strength + l.strength);
                for (int k = 0; k < 3; k++) m.pos[k] += (l.pos[k] - m.pos[k]) * w;
                m.strength = std::min(m.strength + l.strength * 0.25f, 1.4f);
                m.radius = std::max(m.radius, l.radius);
                merged = true;
                break;
            }
        }
        if (!merged && dyn_frame_.size() < 256) dyn_frame_.push_back(l);
    }
}

// the world's depth before the weapon in hand clears it: its lights are added once the weapon is drawn
void D3D12Backend::SaveWorldDepth(Target& ds) {
    world_depth_saved_ = CopyDepth(ds);
}

// the depth buffer now into world_depth_ (the soft particles', then the world's for the lights), readable
bool D3D12Backend::CopyDepth(Target& ds) {
    MsaaResolve();
    D3D12_RESOURCE_DESC d = ds.res->GetDesc();
    if (world_depth_) {
        D3D12_RESOURCE_DESC have = world_depth_->GetDesc();
        if (have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) Release(std::move(world_depth_));
    }
    if (!world_depth_) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        world_depth_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &d, world_depth_state_, nullptr, IID_PPV_ARGS(&world_depth_)))) {
            world_depth_.Reset();
            return false;
        }
    }
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(world_depth_.Get(), world_depth_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    cl_->CopyResource(world_depth_.Get(), ds.res.Get());
    Transition(world_depth_.Get(), world_depth_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    return true;
}

// the frame's sprites into the lights shown: a light seen again carries on from where its sprite is now, a new one
// starts; each fades over its hold once its sprite is gone
void D3D12Backend::UpdateDynLights() {
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
    last = now;
    for (DynLight& l : dyn_lights_) l.age += dt;
    if (float r = g_test_light[3].load(); r > 0) {
        DynLight t{};
        for (int k = 0; k < 3; k++) t.pos[k] = g_test_light[k].load();
        t.radius = r, t.color[0] = 1.5f, t.color[1] = 1.0f, t.color[2] = 0.5f, t.strength = 1, t.hold = 0.06f, t.shadows = true;
        dyn_frame_.push_back(t);
    }
    for (const DynLight& f : dyn_frame_) {
        DynLight* best = nullptr;
        float best_d2 = 0;
        for (DynLight& l : dyn_lights_) {
            if (l.age <= 0 || l.hold != f.hold || l.color[0] != f.color[0]) continue;
            float d2 = 0;
            for (int k = 0; k < 3; k++) d2 += (l.pos[k] - f.pos[k]) * (l.pos[k] - f.pos[k]);
            float r = 0.6f * std::max(l.radius, f.radius);
            if (d2 < r * r && (!best || d2 < best_d2)) best = &l, best_d2 = d2;
        }
        if (best) *best = f;
        else if (dyn_lights_.size() < 256) {
            dyn_lights_.push_back(f);
            // a new blast, seen for the first time (Shockwaves): a ring from its place on the screen, only a near
            // one (within about 25 m), weaker further; very near, dirt on the view
            if (f.flash > 0 && cam_ok_) {
                const float* V = cam_view_;
                float v[3];
                for (int j = 0; j < 3; j++) v[j] = f.pos[0] * V[j] + f.pos[1] * V[4 + j] + f.pos[2] * V[8 + j] + V[12 + j];
                float dist = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (v[2] > 30 && dist < 2500) {
                    float nx = v[0] / v[2] * cam_proj_[0], ny = v[1] / v[2] * cam_proj_[5];
                    float size = f.radius * 2.0f / v[2] * cam_proj_[5] * 0.5f;
                    if (std::fabs(nx) < 1.5f && std::fabs(ny) < 1.5f) {
                        shocks_.push_back({nx * 0.5f + 0.5f, 0.5f - ny * 0.5f, std::clamp(size, 0.05f, 0.8f), 0,
                                           std::clamp(1.2f - dist / 2500, 0.2f, 1.0f)});
                        if (shocks_.size() > 2) shocks_.erase(shocks_.begin());
                    }
                }
                if (dist < f.radius * 3) {
                    float d = std::clamp(1.5f - dist / (f.radius * 2), 0.0f, 1.0f);
                    if (d > dirt_ + 0.2f) dirt_seed_ = float(frame_ % 97);
                    dirt_ = std::max(dirt_, d);
                }
            }
        }
    }
    // the barrel's heat (Barrel Heat): each of the player's shots adds, it cools by a fifth a second; the muzzle on the
    // screen at the last one
    {
        bool fired = false;
        for (const DynLight& f : dyn_frame_) {
            if (!f.shadows || !cam_ok_) continue;
            const float* V = cam_view_;
            float v[3];
            for (int j = 0; j < 3; j++) v[j] = f.pos[0] * V[j] + f.pos[1] * V[4 + j] + f.pos[2] * V[8 + j] + V[12 + j];
            if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > 250 * 250 || v[2] < 5) continue;
            fired = true;
            muzzle_[0] = v[0] / v[2] * cam_proj_[0] * 0.5f + 0.5f, muzzle_[1] = 0.5f - v[1] / v[2] * cam_proj_[5] * 0.5f;
        }
        if (fired) barrel_heat_ = std::min(barrel_heat_ + 0.06f, 1.0f);
        static const bool hot = std::getenv("XBE_BARREL_HOT") != nullptr;  // tests: always hot
        if (hot) barrel_heat_ = 1.2f;
        barrel_heat_ = std::max(0.0f, barrel_heat_ - dt * 0.2f);
    }
    dyn_frame_.clear();
    std::erase_if(dyn_lights_, [](const DynLight& l) { return l.age > l.hold * 5 + 0.02f; });
}

// the lights shown, the strongest first, in the camera's view space: position and radius, colour times strength
uint32_t D3D12Backend::DynLightConstants(float* pos_radius, float* color, uint32_t max) {
    if (dyn_lights_.empty() || !cam_ok_) return 0;
    static auto t0 = std::chrono::steady_clock::now();
    float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    struct Shown {
        float v[3], radius, c[3], rank;
        float dist;
        bool shadows;
        bool fire;  // a steady, flickering light (fire): the ambient motes' embers
        bool strong;
    };
    std::vector<Shown> shown;
    for (const DynLight& l : dyn_lights_) {
        float k = l.strength * std::exp(-l.age / std::max(l.hold, 0.005f));
        if (l.flicker) {
            float ph = l.pos[0] * 0.013f + l.pos[2] * 0.017f;
            k *= 0.82f + 0.1f * std::sin(t * 13.0f + ph) + 0.08f * std::sin(t * 23.0f + ph * 1.7f);
        }
        if (k < 0.02f) continue;
        Shown sw;
        const float* V = cam_view_;
        for (int j = 0; j < 3; j++) sw.v[j] = l.pos[0] * V[j] + l.pos[1] * V[4 + j] + l.pos[2] * V[8 + j] + V[12 + j];
        // behind the camera further than its reach: nothing it lights is seen
        if (sw.v[2] < -l.radius) continue;
        sw.radius = l.radius;
        for (int j = 0; j < 3; j++) sw.c[j] = l.color[j] * k;
        float dist = std::sqrt(sw.v[0] * sw.v[0] + sw.v[1] * sw.v[1] + sw.v[2] * sw.v[2]);
        sw.rank = k * l.radius / std::max(dist, l.radius * 0.5f);
        sw.dist = dist;
        sw.shadows = l.shadows;
        sw.fire = l.flicker;
        sw.strong = l.radius >= 400 && l.strength >= 0.8f;  // a big bright light (a fire, a burning wreck, a lamp)
        shown.push_back(sw);
    }
    // the hot ones' air shimmers (on the screen: where, how far, how strongly), the close explosions flash the view
    fx_.heats = 0;
    fx_.flash = 0;
    {
        struct Hot {
            float h[4], z;
        };
        std::vector<Hot> hot;
        for (const DynLight& l : dyn_lights_) {
            float k = l.strength * std::exp(-l.age / std::max(l.hold, 0.005f));
            const float* V = cam_view_;
            float v[3];
            for (int j = 0; j < 3; j++) v[j] = l.pos[0] * V[j] + l.pos[1] * V[4 + j] + l.pos[2] * V[8 + j] + V[12 + j];
            float dist = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (l.flash > 0) fx_.flash = std::max(fx_.flash, 0.5f * l.flash * std::min(k, 1.0f) * std::clamp(1 - dist / (l.radius * 2.5f), 0.0f, 1.0f));
            // a shimmer lasts as its fire: a burst's for a moment more than its flash
            float hk = l.heat * std::min(std::exp(-l.age / std::max(l.hold * 3, 0.3f)) * l.strength, 1.0f);
            if (hk < 0.05f || v[2] < 40) continue;
            float nx = v[0] / v[2] * cam_proj_[0], ny = v[1] / v[2] * cam_proj_[5];
            float r = l.radius * 0.35f / v[2] * cam_proj_[5] * 0.5f;
            if (std::fabs(nx) > 1 + 2 * r || std::fabs(ny) > 1 + 2 * r) continue;
            hot.push_back({{nx * 0.5f + 0.5f, 0.5f - ny * 0.5f, r, hk * std::clamp((v[2] - 40) / 150, 0.0f, 1.0f)}, v[2]});
        }
        std::sort(hot.begin(), hot.end(), [](const Hot& x, const Hot& y) { return x.h[3] * x.h[2] > y.h[3] * y.h[2]; });
        for (const Hot& o : hot) {
            if (fx_.heats >= 8) break;
            std::memcpy(fx_.heat[fx_.heats], o.h, sizeof(o.h));
            float z[4] = {o.z, 0, 0, 0};
            std::memcpy(fx_.heatz[fx_.heats], z, sizeof(z));
            fx_.heats++;
        }
        fx_.flash = std::min(fx_.flash, 0.5f);
    }
    // the shots' flashes, the fires and the big lights first (they cast shadows): the others (tracers, sparks, embers)
    // left out first when there are more than the scene takes
    auto caster = [](const Shown& x) { return x.shadows || x.fire || x.strong; };
    std::sort(shown.begin(), shown.end(), [&](const Shown& x, const Shown& y) {
        return caster(x) != caster(y) ? caster(x) : x.rank > y.rank;
    });
    uint32_t n = std::min<uint32_t>(uint32_t(shown.size()), max);
    // shadows: eight of the shots' flashes and eight of the fires and big lights, sixteen at most. Of each kind the
    // four nearest the eye (the player's own flash first), then four spread over the rest by distance, so a fight at
    // a middle distance keeps its shadows beside one close by
    std::vector<uint32_t> casting;
    auto pick = [&](auto&& wanted) {
        std::vector<uint32_t> kind;
        for (uint32_t i = 0; i < n; i++)
            if (wanted(shown[i]) && std::find(casting.begin(), casting.end(), i) == casting.end()) kind.push_back(i);
        std::sort(kind.begin(), kind.end(), [&](uint32_t x, uint32_t y) { return shown[x].dist < shown[y].dist; });
        constexpr size_t kNear = 4, kKind = 8;
        if (kind.size() <= kKind) {
            casting.insert(casting.end(), kind.begin(), kind.end());
            return;
        }
        casting.insert(casting.end(), kind.begin(), kind.begin() + kNear);
        size_t rest = kind.size() - kNear, take = kKind - kNear;
        for (size_t k = 0; k < take; k++) casting.push_back(kind[kNear + (k * 2 + 1) * rest / (take * 2)]);
    };
    if (app::Get().flash_shadows) {
        pick([](const Shown& x) { return x.shadows; });
        pick([](const Shown& x) { return x.fire || x.strong; });
    }
    for (uint32_t i = 0; i < n; i++) {
        const Shown& sw = shown[i];
        bool cast = std::find(casting.begin(), casting.end(), i) != casting.end();
        float pr[4] = {sw.v[0], sw.v[1], sw.v[2], sw.radius}, c[4] = {sw.c[0], sw.c[1], sw.c[2], cast ? (sw.fire ? 0.875f : 1.0f) : sw.fire ? 0.5f : 0.0f};  // (0.875: a fire that casts)
        std::memcpy(pos_radius + i * 4, pr, sizeof(pr));
        std::memcpy(color + i * 4, c, sizeof(c));
    }
    return n;
}

// the effects' parameters for the scene's target (see kSceneCommon)
void D3D12Backend::SceneParams(const Target& rt, float p[16]) {
    const app::Settings& set = app::Get();
    MaterialFrame mf;
    {
        std::lock_guard lock(g_material_frame_lock);
        mf = g_material_frame;
    }
    float u = mf.units;
    // the light in this view (as UploadConstants: the game's camera frame mirrors its x)
    float lv[3] = {mf.light[0], mf.light[1], mf.light[2]};
    if (cam_ok_) {
        float t[3];
        for (int j = 0; j < 3; j++) t[j] = mf.sun[0] * cam_view_[j] + mf.sun[1] * cam_view_[4 + j] + mf.sun[2] * cam_view_[8 + j];
        float n = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        if (n > 1e-4f) lv[0] = t[0] / n, lv[1] = t[1] / n, lv[2] = t[2] / n;
    }
    float v[16] = {mf.view_x, mf.view_y, mf.near_plane, mf.far_plane,
                   lv[0], lv[1], lv[2], 0.35f * u,
                   float(rt.w), float(rt.h), 1.0f / rt.w, 1.0f / rt.h,
                   0.6f * u, set.ao ? 1.4f : 0.0f, set.contact_shadows ? 0.55f : 0.0f, set.bloom ? 0.6f : 0.0f};
    std::memcpy(p, v, sizeof(v));
}

// the copy of the scene's target the passes read while they draw over it
void D3D12Backend::CopyScene(Target& rt) {
    MsaaResolve();
    Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(scene_[0].Get(), scene_state_[0], D3D12_RESOURCE_STATE_COPY_DEST);
    cl_->CopyResource(scene_[0].Get(), rt.res.Get());
    Transition(scene_[0].Get(), scene_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// Ambient occlusion and contact shadows on the world, from its depth, before anything is drawn over it with a depth
// of its own (the weapon in hand clears the depth first)
void D3D12Backend::SceneOcclusion() {
    GpuMark("scene draws");
    MsaaResolve();
    GpuMark("msaa resolve");
    occlusion_done_ = true;
    const app::Settings& set = app::Get();
    ResolveLight();
    bool shadows = scene_rt_ && scene_ds_ && light_.on && shadow_q_.cascades > 0 && cam_ok_;
    if (!scene_rt_ || !scene_ds_ || (!set.ao && !set.contact_shadows && !shadows)) {
        casters_.clear();
        return;
    }
    Target& rt = *scene_rt_;
    Target& ds = *scene_ds_;
    if (!EnsureSceneTargets(rt.fmt, rt.w, rt.h)) return;
    float p[16];
    SceneParams(rt, p);
    uint32_t w = rt.w, h = rt.h;
    CopyScene(rt);
    GpuMark("scene copy");
    bool shadow_map = shadows && DrawShadowMap();
    GpuMark("shadow map");
    ID3D12Resource* mask = shadow_map ? ShadowMask(rt, ds) : nullptr;
    GpuMark("shadow mask");
    fog_done_ = false;
    if (mask && set.volumetric_fog) FogPass(rt, ds);
    GpuMark("volumetric fog");
    // past the screen's edges: the guard band, from the casters (a null view reads 0: none)
    ID3D12Resource* guard = set.ao || set.contact_shadows ? DrawGuard(w, h) : nullptr;
    GpuMark("guard band");
    WaterInteraction();
    GpuMark("water");
    MapFrame();
    SnowTracks();
    GpuMark("snow");
    casters_.clear();
    const bool real_mask = mask != nullptr;
    if (!mask) {
        // no shadows: the mask white (a null view reads 0 whatever its mapping)
        Transition(scene_[kSceneShadow].Get(), scene_state_[kSceneShadow], D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float white[4] = {1, 1, 1, 1};
        cl_->ClearRenderTargetView(SceneRtv(kSceneShadow), white, 0, nullptr);
        Transition(scene_[kSceneShadow].Get(), scene_state_[kSceneShadow], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        mask = scene_[kSceneShadow].Get();
    }
    sun_vis_ok_ = w == scene_w_ && h == scene_h_;  // the next frame's highlights read it (BindSunVisibility)
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    constexpr DXGI_FORMAT kDepth = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    static const int dbg = [] {
        const char* e = std::getenv("XBE_SCENE_DEBUG");
        return e ? std::atoi(e) : 0;
    }();
    static const int sdbg = [] {
        const char* e = std::getenv("XBE_SHADOW_DEBUG");
        return e ? std::atoi(e) : 0;
    }();
    // one bounce of light, from the scene as drawn (its copy, scene_[0])
    bool gi = set.bounce_light && scene_[kSceneGi];
    if (gi) {
        ScenePass(kSceneAo, "gi", kSceneGi, DXGI_FORMAT_R16G16B16A16_FLOAT, std::max(1u, w / 2), std::max(1u, h / 2),
                  {{ds.res.Get(), kDepth}, {scene_[0].Get(), rt.fmt}}, p);
    }
    gi_ready_ = gi;
    // the clouds' shadows need the world's depth and the shadows' mask
    ID3D12Resource* cloud_depth = set.cloud_shadows && set.clouds && real_mask ? ds.res.Get() : nullptr;
    if (!set.ao && !set.contact_shadows) {
        // no occlusion: white (a null view reads 0, the whole world black)
        Transition(scene_[1].Get(), scene_state_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float white[4] = {1, 1, 1, 1};
        cl_->ClearRenderTargetView(SceneRtv(1), white, 0, nullptr);
        Transition(scene_[1].Get(), scene_state_[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        SceneComposite(rt, "occlude", scene_[1].Get(), false, p, cloud_depth, mask);
        Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        return;
    }
    ScenePass(kSceneAo, dbg == 2 ? "ao_depth" : dbg == 3 ? "ao_viewz" : dbg == 4 ? "ao_guard" : "ao", 1, DXGI_FORMAT_R8_UNORM, w, h,
              {{ds.res.Get(), kDepth}, {nullptr, kBgra}, {guard, DXGI_FORMAT_R32_FLOAT}}, p);
    float ph[16];
    std::memcpy(ph, p, sizeof(ph));
    ph[4] = 1, ph[5] = 0;
    ScenePass(kSceneBlur, "blur", 2, DXGI_FORMAT_R8_UNORM, w, h, {{scene_[1].Get(), DXGI_FORMAT_R8_UNORM}, {ds.res.Get(), kDepth}}, ph);
    ph[4] = 0, ph[5] = 1;
    ScenePass(kSceneBlur, "blur", 1, DXGI_FORMAT_R8_UNORM, w, h, {{scene_[2].Get(), DXGI_FORMAT_R8_UNORM}, {ds.res.Get(), kDepth}}, ph);
    // the smoke and dust before the corners: the occlusion lifted where they cover it
    static const bool no_veil = std::getenv("XBE_NO_VEIL") != nullptr;  // tests
    if (dbg <= 1 && !no_veil) DrawVeil(ds);
    else veil_.clear();
    GpuMark("veil");
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const char* views[] = {"occlude", "shadow_mask", "shadow_cascades", "shadow_raw", "shadow_atlas", "shadow_share", "shadow_rgb"};
    const char* entry = dbg ? "composite_debug" : mask && sdbg > 0 && sdbg < 7 ? views[sdbg] : "occlude";
    SceneComposite(rt, entry, scene_[1].Get(), false, p, cloud_depth, mask);
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

// Bloom and the highlights' roll off on the 3D, before the 2D is drawn over it
void D3D12Backend::ScenePost() {
    MsaaResolve();
    scene_done_ = true;
    if (!occlusion_done_) SceneOcclusion();
    const app::Settings& set = app::Get();
    UpdateDynLights();
    if (!scene_rt_) return;
    Target& rt = *scene_rt_;
    if (!EnsureSceneTargets(rt.fmt, rt.w, rt.h)) return;
    float p[16];
    SceneParams(rt, p);
    uint32_t w = rt.w, h = rt.h;
    motion_ok_ = false;
    GpuMark("weapon and late draws");
    if ((set.taa || set.motion_blur) && cam_ok_) MotionPass(rt);
    GpuMark("motion");
    CopyScene(rt);
    if (set.auto_exposure) Exposure(rt);
    GpuMark("exposure");
    // the sky: before the weapon in hand when it cleared the depth, else here
    if (!sky_done_ && SkyConstants()) {
        GpuMark("post start");
        SkyPass(rt, p);
        GpuMark("sky");
        CopyScene(rt);
    } else if (sky_done_) {
        SkyConstants();  // the constants the passes below read
    }
    static const bool ssr_log = std::getenv("XBE_SSR_LOG") != nullptr;
    if (ssr_log) {
        static int n = 0;
        if (n++ % 300 == 0) LOGR("ssr: pass used %d gbuf %dx%d rt %ux%u", int(gbuf_used_), gbuf_w_, gbuf_h_, rt.w, rt.h);
    }
    if (set.reflections && gbuf_used_ && gbuf_ && gbuf_w_ == rt.w && gbuf_h_ == rt.h) {
        ReflectionPass(rt, p);
        GpuMark("reflections");
        CopyScene(rt);
    }
    if (set.bloom) {
        constexpr DXGI_FORMAT kF16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ScenePass(kSceneBright, "bright", 3, kF16, std::max(1u, w / 2), std::max(1u, h / 2), {{scene_[0].Get(), rt.fmt}}, p);
        for (uint32_t i = 4; i < kSceneBloomEnd; i++) {
            uint32_t sw = std::max(1u, w / (2u << (i - 4))), sh = std::max(1u, h / (2u << (i - 4)));
            float pd[16];
            std::memcpy(pd, p, sizeof(pd));
            pd[10] = 1.0f / sw, pd[11] = 1.0f / sh;
            ScenePass(kSceneDown, "down", i, kF16, std::max(1u, sw / 2), std::max(1u, sh / 2), {{scene_[i - 1].Get(), kF16}}, pd);
        }
        // back up the levels, each a tent filtered upsample of the smaller blended over it: one smooth, wide glow
        for (uint32_t i = kSceneBloomEnd - 1; i > 3; i--) {
            uint32_t sw = std::max(1u, w / (1u << (i - 2))), sh = std::max(1u, h / (1u << (i - 2)));
            float pu[16];
            std::memcpy(pu, p, sizeof(pu));
            pu[10] = 1.0f / sw, pu[11] = 1.0f / sh;
            ScenePass(kSceneUp, "up", i - 1, kF16, std::max(1u, w / (1u << (i - 3))), std::max(1u, h / (1u << (i - 3))),
                      {{scene_[i].Get(), kF16}}, pu, 0.6f);
        }
    }
    // the sun's shafts: the bright sky blurred towards the sun, where the sun is in view or near it
    bool rays = false;
    sun_screen_[2] = 0;
    float pr[16];
    std::memcpy(pr, p, sizeof(pr));
    if (cam_ok_ && light_.direct > 0 && scene_ds_) {
        const float* V = cam_view_;
        float v[3];
        for (int j = 0; j < 3; j++) v[j] = light_.dir[0] * V[j] + light_.dir[1] * V[4 + j] + light_.dir[2] * V[8 + j];
        if (v[2] > 0.05f) {
            float nx = v[0] / v[2] * cam_proj_[0], ny = v[1] / v[2] * cam_proj_[5];
            float edge = std::max(std::fabs(nx), std::fabs(ny));
            float strength = 0.55f * std::clamp(1.6f - edge, 0.0f, 1.0f) * std::clamp(v[2] * 3, 0.0f, 1.0f) *
                             std::min(light_.direct * 2, 1.0f);
            // (no flare nor shafts from where no moon is drawn: the level's own moon stands elsewhere in its sky)
            if (OwnMoonLevel() || (game_moon_frame_ != ~0ull && frame_ - game_moon_frame_ <= 1)) strength = 0;
            pr[4] = nx * 0.5f + 0.5f, pr[5] = 0.5f - ny * 0.5f, pr[6] = strength, pr[7] = float(w) / float(h);
            rays = strength > 0.01f && set.sun_shafts;
            sun_screen_[0] = pr[4], sun_screen_[1] = pr[5], sun_screen_[2] = strength;
        }
        // tests: a sun forced at the top middle of the screen
        static const bool rays_test = std::getenv("XBE_RAYS_TEST") != nullptr;
        if (rays_test) pr[4] = 0.5f, pr[5] = 0.12f, pr[6] = 0.6f, pr[7] = float(w) / float(h), rays = true;
        static const bool log = std::getenv("XBE_RAYS_LOG") != nullptr;
        static int n = 0;
        if (log && n++ % 120 == 0) LOGR("rays: sun in view %.2f %.2f %.2f, on screen %.2f %.2f, strength %.2f", v[0], v[1], v[2], pr[4], pr[5], pr[6]);
    }
    // aiming down the sights (the game's aim held), with every weapon: a scope's overlay hides the weapon, nothing of
    // it to blur
    {
        float target = g_aiming.load() && set.aim_dof ? 1.0f : 0.0f;
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
        last = now;
        dof_ += std::clamp(target - dof_, -dt / 0.15f, dt / 0.15f);
    }
    bool dof = dof_ > 0.01f;
    if (rays || dof) {
        Target& ds = *scene_ds_;
        constexpr DXGI_FORMAT kF16 = DXGI_FORMAT_R16G16B16A16_FLOAT, kDepth = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12Resource* depth = world_depth_saved_ ? world_depth_.Get() : ds.res.Get();
        if (world_depth_saved_) Transition(world_depth_.Get(), world_depth_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (rays) ScenePass(kSceneRaysHlsl, "rays", kSceneRays, kF16, std::max(1u, w / 2), std::max(1u, h / 2),
                            {{scene_[0].Get(), rt.fmt}, {depth, kDepth}}, pr);
        if (dof) ScenePass(kSceneDofHlsl, "dofblur", kSceneDof, kF16, std::max(1u, w / 2), std::max(1u, h / 2),
                           {{scene_[0].Get(), rt.fmt}}, p);
        Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    SceneFinal(rt, p, set.bloom, rays, dof);
    GpuMark("final");
    DrawSplashes(rt);
    GpuMark("splashes");
    if (set.motion_blur && motion_ok_) MotionBlurPass(rt);
    if (set.taa) TaaPass(rt);
    else taa_hist_ok_ = false;
    GpuMark("taa");
    world_depth_saved_ = false;
}

// The sky's constants: the camera's rows in the world, the level's light as the sun (or the moon), the time, the
// switches. False when there is no sky to draw on (no camera, both off).
// how much the level's light is the day's, 0 to 1, from its strength (the night levels' moon is about 0.2, an
// overcast day's 0.5, a desert's 1): the sky's pixels alone took a night sky's brighter pieces for day, and the clouds
// over them lit as by day showed their hard edges. 1 when the level's light is not known (the pixels decide).
float D3D12Backend::Daylight() const {
    if (light_.source == shadow::kNone || light_.direct <= 0) return 1;
    float t = std::clamp((light_.direct - 0.25f) / 0.2f, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

bool D3D12Backend::SkyConstants() {
    const app::Settings& set = app::Get();
    std::memset(sky_cb_, 0, sizeof(sky_cb_));
    static const bool lights_debug = std::getenv("XBE_LIGHTS_DEBUG") != nullptr;
    sky_cb_[22] = lights_debug ? 1.0f : 0.0f;
    static const float ssr_debug = [] {
        const char* e = std::getenv("XBE_SSR_DEBUG");
        return e ? float(std::atoi(e)) : 0.0f;
    }();
    sky_cb_[23] = ssr_debug;
    static const bool sky_layers_debug = std::getenv("XBE_SKY_LAYERS") != nullptr;  // level 1's sky pieces in colours
    if (!cam_ok_ || !scene_ds_ || (!set.sun_moon && !set.clouds)) return false;
    float inv[16];
    if (!shadow::Invert(cam_view_, inv)) return false;
    static const auto t0 = std::chrono::steady_clock::now();
    float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    float n = std::sqrt(light_.dir[0] * light_.dir[0] + light_.dir[1] * light_.dir[1] + light_.dir[2] * light_.dir[2]);
    // (none where the level's sky has its own: the moon sprite drawn since the level began, or a level known to
    // have one, level 2's, which it draws only while its own camera looks at it)
    bool own = OwnMoonLevel() || (game_moon_frame_ != ~0ull && frame_ - game_moon_frame_ <= 1);
    bool sun = set.sun_moon && n > 0.5f && light_.direct > 0 && !own;
    float sw[3] = {light_.dir[0] / std::max(n, 1e-6f), light_.dir[1] / std::max(n, 1e-6f), light_.dir[2] / std::max(n, 1e-6f)};
    // a light from about the horizon or under it (some levels' models are lit nearly level): the sun just over
    // the horizon at its heading
    if (sw[1] < 0.2f) {
        float hz = std::sqrt(sw[0] * sw[0] + sw[2] * sw[2]), k = std::sqrt(1 - 0.04f) / std::max(hz, 1e-6f);
        sw[0] *= k, sw[2] *= k, sw[1] = 0.2f;
    }
    float sv[3];
    for (int j = 0; j < 3; j++) sv[j] = sw[0] * cam_view_[j] + sw[1] * cam_view_[4 + j] + sw[2] * cam_view_[8 + j];
    float rows[24] = {inv[0], inv[1], inv[2], g_level.load() == 1 ? (sky_layers_debug ? 3.0f : 2.0f) : 1.0f,  inv[4], inv[5], inv[6], std::fmod(t, 20000.0f),
                      inv[8], inv[9], inv[10], 0.5f,
                      sv[0], sv[1], sv[2], sun ? 1.0f : 0.0f,
                      sw[0], sw[1], sw[2], std::clamp(light_.direct * 2.0f, 0.4f, 1.5f),
                      set.clouds ? 1.0f : 0.0f, sun ? 1.0f : 0.0f, Daylight() + (lights_debug ? 2.0f : 0.0f), ssr_debug};
    std::memcpy(sky_cb_, rows, sizeof(rows));
    return true;
}

// the clouds, the sun and the moon on the sky's pixels of the scene (from its copy, scene_[0]), before the bloom
void D3D12Backend::SkyPass(Target& rt, const float p[16]) {
    std::string text = std::string(kSceneCommon) + kSceneSkyHlsl;
    ID3D12PipelineState* pso = EnsurePassPipeline(text.c_str(), "skyfx", rt.fmt, -1);
    if (!pso || !scene_ds_) return;
    Target& ds = *scene_ds_;
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (world_depth_saved_) Transition(world_depth_.Get(), world_depth_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* depth = world_depth_saved_ ? world_depth_.Get() : ds.res.Get();
    // level 1: where its smoke and far water are near (skyfx leaves the costly parts out elsewhere)
    ID3D12Resource* tiles = nullptr;
    if (sky_cb_[3] > 1.5f) {
        uint32_t tw = (rt.w + 15) / 16, th = (rt.h + 15) / 16;
        if (!sky_tiles_[1] || sky_tiles_w_ != tw || sky_tiles_h_ != th) {
            for (auto& r : sky_tiles_)
                if (r) Release(std::move(r));
            sky_tiles_w_ = sky_tiles_h_ = 0;
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_R8G8_UNORM, tw, th);
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{};
            cv.Format = td.Format;
            bool ok = true;
            for (uint32_t i = 0; i < 2 && ok; i++) {
                sky_tiles_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                ok = SUCCEEDED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, sky_tiles_state_[i], &cv, IID_PPV_ARGS(&sky_tiles_[i])));
                if (ok) dev_->CreateRenderTargetView(sky_tiles_[i].Get(), nullptr, SkyTilesRtv(i));
            }
            if (ok) sky_tiles_w_ = tw, sky_tiles_h_ = th;
            else for (auto& r : sky_tiles_) r.Reset();
        }
        if (sky_tiles_[1]) {
            // the reaches: SkyFeather's disc (0.17 of the height), HorizonSoft's band and disc (0.03 and 0.014 of it,
            // the sum), in tiles, a tile more for where in its tile a pixel is
            float reach_veil = std::ceil(0.17f * float(rt.h) / 16) + 1, reach_water = std::ceil((0.03f + 0.014f) * float(rt.h) / 16) + 1;
            float q[8] = {float(rt.w), float(rt.h), float(tw), float(th), reach_veil, reach_water, 0, 0};
            Transition(sky_tiles_[0].Get(), sky_tiles_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
            ID3D12Resource* s0[3] = {depth, nullptr, nullptr};
            DXGI_FORMAT f0[3] = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, kBgra, kBgra};
            Pass(kSkyTilesHlsl, "skytiles", DXGI_FORMAT_R8G8_UNORM, SkyTilesRtv(0), tw, th, s0, f0, q, -1);
            Transition(sky_tiles_[0].Get(), sky_tiles_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(sky_tiles_[1].Get(), sky_tiles_state_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
            ID3D12Resource* s1[3] = {sky_tiles_[0].Get(), nullptr, nullptr};
            DXGI_FORMAT f1[3] = {DXGI_FORMAT_R8G8_UNORM, kBgra, kBgra};
            Pass(kSkyTilesHlsl, "skydilate", DXGI_FORMAT_R8G8_UNORM, SkyTilesRtv(1), tw, th, s1, f1, q, -1);
            Transition(sky_tiles_[1].Get(), sky_tiles_state_[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            tiles = sky_tiles_[1].Get();
        }
    }
    uint32_t slot = AllocSrvSlots(7);
    std::pair<ID3D12Resource*, DXGI_FORMAT> src[3] = {{scene_[0].Get(), rt.fmt},
                                                      {depth, DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
                                                      {tiles, DXGI_FORMAT_R8G8_UNORM}};
    for (uint32_t i = 0; i < 7; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 3 && src[i].first ? src[i].second : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 3 ? src[i].first : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), rt.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(rt.w, rt.h);
    Alloc cb = Upload(1536, 256);
    std::memset(cb.cpu, 0, 1536);
    std::memcpy(cb.cpu, p, 16 * sizeof(float));
    std::memcpy(cb.cpu + kSkyConstantsAt * sizeof(float), sky_cb_, sizeof(sky_cb_));
    DrawFullScreen(pso, cb.gpu, slot);
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    bound_rt_ = bound_ds_ = nullptr;
}

// The reflections (kSceneSsrHlsl) on the scene's target, from its copy (scene_[0]): t1 the depth now, t2 the reflection
// buffer, t3 the world's depth (kept before the weapon in hand cleared it; the depth now without it)
void D3D12Backend::ReflectionPass(Target& rt, const float p[16]) {
    std::string text = std::string(kSceneCommon) + kSceneSsrHlsl;
    ID3D12PipelineState* pso = EnsurePassPipeline(text.c_str(), "ssr", rt.fmt, -1);
    if (!pso || !scene_ds_) return;
    Target& ds = *scene_ds_;
    if (bound_rt_ == &rt) BindGbuf(false);
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(gbuf_.Get(), gbuf_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (world_depth_saved_) Transition(world_depth_.Get(), world_depth_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    constexpr DXGI_FORMAT kDepth = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    // t4 the occlusion (the AO's, scene_[1]: corners and creases reflect less); none: 1
    const app::Settings& set = app::Get();
    bool ao = set.ao || set.contact_shadows;
    std::pair<ID3D12Resource*, DXGI_FORMAT> src[5] = {{scene_[0].Get(), rt.fmt},
                                                      {ds.res.Get(), kDepth},
                                                      {gbuf_.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
                                                      {world_depth_saved_ ? world_depth_.Get() : ds.res.Get(), kDepth},
                                                      {ao ? scene_[1].Get() : nullptr, DXGI_FORMAT_R8_UNORM}};
    uint32_t slot = AllocSrvSlots(7);
    for (uint32_t i = 0; i < 7; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 5 && src[i].first ? src[i].second : i == 4 ? DXGI_FORMAT_R8_UNORM : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = i == 4 && !ao ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(5, 5, 5, 5)
                                                   : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 5 ? src[i].first : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), rt.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(rt.w, rt.h);
    Alloc cb = Upload(1536, 256);
    std::memset(cb.cpu, 0, 1536);
    std::memcpy(cb.cpu, p, 16 * sizeof(float));
    std::memcpy(cb.cpu + kSkyConstantsAt * sizeof(float), sky_cb_, sizeof(sky_cb_));
    DrawFullScreen(pso, cb.gpu, slot);
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    bound_rt_ = bound_ds_ = nullptr;
    gbuf_bound_ = false;
}

// The 3D's last pass before the 2D: the scene (shaken and wavering in pain, shimmering over fires), blurred behind
// what is aimed at, the bloom and the sun's shafts added, the glowing sprites' lights, an explosion's flash, the
// highlights rolled off. t0 the scene, t1 the world's depth (kept before the weapon's clear), t2 the bloom, t3 the
// shafts, t4 the blurred scene, t6 the depth now (the weapon in hand's).
void D3D12Backend::SceneFinal(Target& rt, const float p[16], bool bloom, bool rays, bool dof) {
    std::string text = std::string(kSceneCommon) + kSceneComposite;
    ID3D12PipelineState* pso = EnsurePassPipeline(text.c_str(), "composite", rt.fmt, -1);
    if (!pso || !scene_ds_) return;
    Target& ds = *scene_ds_;
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (world_depth_saved_) Transition(world_depth_.Get(), world_depth_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    constexpr DXGI_FORMAT kF16 = DXGI_FORMAT_R16G16B16A16_FLOAT, kDepth = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    std::pair<ID3D12Resource*, DXGI_FORMAT> src[7] = {
        {scene_[0].Get(), rt.fmt},
        {world_depth_saved_ ? world_depth_.Get() : ds.res.Get(), kDepth},
        {bloom ? scene_[3].Get() : nullptr, kF16},
        {rays ? scene_[kSceneRays].Get() : nullptr, kF16},
        {dof ? scene_[kSceneDof].Get() : nullptr, kF16},
        {fog_done_ ? scene_[kSceneFog].Get() : nullptr, kF16},
        {ds.res.Get(), kDepth}};
    uint32_t slot = AllocSrvSlots(7);
    for (uint32_t i = 0; i < 7; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = src[i].first ? src[i].second : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(src[i].first, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    constexpr uint32_t kMaxLights = 32;
    float lpos[kMaxLights * 4], lcol[kMaxLights * 4];
    uint32_t nlights = app::Get().dynamic_lights ? DynLightConstants(lpos, lcol, kMaxLights) : 0;
    const app::Settings& set = app::Get();
    if (!nlights || !set.heat_haze) fx_.heats = 0;
    if (!nlights || !set.explosion_flash) fx_.flash = 0;
    Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), rt.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(rt.w, rt.h);
    // kSceneCommon's cbuffer: p[5], lcount, lpos[32], lcol[32], heat[8], heatz[8]
    Alloc cb = Upload(1536, 256);
    std::memset(cb.cpu, 0, 1536);
    std::memcpy(cb.cpu, p, 16 * sizeof(float));
    // the player's pain (the game's own lasts a third of a second: the shake lets go over half a second more), the
    // time, the flash, the aiming's blur
    static const auto t0 = std::chrono::steady_clock::now();
    static float last = 0;
    float& shown = hurt_shown_;
    float now = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    shown = std::max(set.damage_shake ? std::clamp(g_hurt.load(), 0.0f, 1.0f) : 0.0f, shown - std::clamp(now - last, 0.0f, 0.1f) / 0.5f);
    last = now;
    float p4[4] = {std::max(shown, 0.0f), std::fmod(now, 1000.0f), fx_.flash, dof ? dof_ : 0.0f};
    float p5[4] = {float(nlights), float(fx_.heats), rays ? 1.0f : 0.0f, set.light_halos ? 1.0f : 0.0f};
    std::memcpy(cb.cpu + 16 * sizeof(float), p4, sizeof(p4));
    std::memcpy(cb.cpu + 20 * sizeof(float), p5, sizeof(p5));
    std::memcpy(cb.cpu + 24 * sizeof(float), lpos, nlights * 4 * sizeof(float));
    std::memcpy(cb.cpu + (24 + kMaxLights * 4) * sizeof(float), lcol, nlights * 4 * sizeof(float));
    std::memcpy(cb.cpu + (24 + kMaxLights * 8) * sizeof(float), fx_.heat, sizeof(fx_.heat));
    std::memcpy(cb.cpu + (24 + kMaxLights * 8 + 32) * sizeof(float), fx_.heatz, sizeof(fx_.heatz));
    // the blasts' rings at heatz[3..4].yzw (x, y, radius now; strength in its sign's place: radius * strength), the
    // dirt at heatz[2].y and its seed at heatz[2].z (Shockwaves)
    {
        static auto last = std::chrono::steady_clock::now();
        auto tnow = std::chrono::steady_clock::now();
        float dt = std::clamp(std::chrono::duration<float>(tnow - last).count(), 0.0f, 0.1f);
        last = tnow;
        for (Shock& sk : shocks_) sk.t += dt;
        // tests: a blast in the middle every two seconds, close
        static const bool test = std::getenv("XBE_SHOCK_TEST") != nullptr;
        static float since = 0;
        if (test && (since += dt) > 2) since = 0, shocks_.push_back({0.5f, 0.55f, 0.35f, 0, 1}), dirt_ = 1, dirt_seed_ = float(frame_ % 97);
        std::erase_if(shocks_, [](const Shock& sk) { return sk.t > 0.6f; });
        dirt_ = std::max(0.0f, dirt_ - dt / 2.5f);
        if (app::Get().shockwaves) {
            for (size_t k = 0; k < shocks_.size() && k < 2; k++) {
                const Shock& sk = shocks_[k];
                float u = sk.t / 0.6f;
                float ring[3] = {sk.x, sk.y, sk.size * (0.15f + u * 1.2f) + 1e-3f};
                float strength = (1 - u) * (1 - u) * sk.power;
                ring[2] = ring[2] + std::floor(strength * 100) * 10;  // the strength in the tens and up
                std::memcpy(cb.cpu + (24 + kMaxLights * 8 + 32 + (3 + k) * 4 + 1) * sizeof(float), ring, sizeof(ring));
            }
            float d[2] = {dirt_, dirt_seed_};
            std::memcpy(cb.cpu + (24 + kMaxLights * 8 + 32 + 2 * 4 + 1) * sizeof(float), d, sizeof(d));
        }
    }
    // the sun on the screen for the lens flare at heatz[1].yzw (Lens Flare); the lens dirt's switch in heatz[0].y
    if (app::Get().lens_flare) {
        float sn[3] = {sun_screen_[0], sun_screen_[1], sun_screen_[2]};
        std::memcpy(cb.cpu + (24 + kMaxLights * 8 + 32 + 1 * 4 + 1) * sizeof(float), sn, sizeof(sn));
        float on = 1;
        std::memcpy(cb.cpu + (24 + kMaxLights * 8 + 32 + 0 * 4 + 1) * sizeof(float), &on, sizeof(on));
    }
    // the barrel's heat at heatz[5].yzw: the muzzle on the screen, the heat (Barrel Heat)
    if (app::Get().barrel_heat && barrel_heat_ > 0.01f) {
        float bh[3] = {muzzle_[0], muzzle_[1], barrel_heat_};
        std::memcpy(cb.cpu + (24 + kMaxLights * 8 + 32 + 5 * 4 + 1) * sizeof(float), bh, sizeof(bh));
    }
    // the motes (Ambient Particles): their switch at heatz[6].y, the eye's place in the world at heatz[7].yzw (the
    // heat sources use heatz's x alone)
    {
        float inv[16];
        if (app::Get().ambient_particles && cam_ok_ && shadow::Invert(cam_view_, inv)) {
            static const bool motes_debug = std::getenv("XBE_MOTES_DEBUG") != nullptr;
            float on = motes_debug ? 2.0f : 1.0f, eye[3] = {inv[12], inv[13], inv[14]};
            std::memcpy(cb.cpu + (24 + kMaxLights * 8 + 32 + 6 * 4 + 1) * sizeof(float), &on, sizeof(on));
            std::memcpy(cb.cpu + (24 + kMaxLights * 8 + 32 + 7 * 4 + 1) * sizeof(float), eye, sizeof(eye));
        }
    }
    std::memcpy(cb.cpu + kSkyConstantsAt * sizeof(float), sky_cb_, sizeof(sky_cb_));
    Grade();
    // the haze's colour and switch in grade[2].w and grade[3].yzw (the composite's)
    grade_cb_[13] = fog_done_ ? fog_tint_[0] : 0, grade_cb_[14] = fog_tint_[1], grade_cb_[15] = fog_tint_[2];
    grade_cb_[11] = fog_done_ ? 1.0f : 0.0f;
    std::memcpy(cb.cpu + kGradeAt * sizeof(float), grade_cb_, sizeof(grade_cb_));
    DrawFullScreen(pso, cb.gpu, slot);
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    bound_rt_ = bound_ds_ = nullptr;
}

// The eye's exposure: the scene's log average luminance (kMeterHlsl, one pixel) copied to a read back buffer, the one
// copied kFrames ago read. It lifts dark scenes and tempers bright ones a little, never far from the game's own light:
// half the difference from a reference, within -0.2 and +0.45 of a stop, adapting over a second or so.
void D3D12Backend::Exposure(Target& rt) {
    if (!meter_) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = td.Format;
        meter_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, meter_state_, &cv, IID_PPV_ARGS(&meter_)))) return;
        dev_->CreateRenderTargetView(meter_.Get(), nullptr, MeterRtv());
        D3D12_HEAP_PROPERTIES rp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(256);
        for (auto& b : meter_rb_)
            if (FAILED(CreateResource(&rp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&b))))
                return;
    }
    // what was measured kFrames ago
    uint32_t slot = uint32_t(frame_ % kFrames);
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
    last = now;
    if (meter_fence_[slot] && fence_->GetCompletedValue() >= meter_fence_[slot]) {
        float* v = nullptr;
        D3D12_RANGE r{0, 16};
        if (SUCCEEDED(meter_rb_[slot]->Map(0, &r, reinterpret_cast<void**>(&v))) && v) {
            float lum = v[0];
            D3D12_RANGE none{0, 0};
            meter_rb_[slot]->Unmap(0, &none);
            constexpr float kReference = -3.3f;  // log2 of the luminance the levels' daylight mostly averages
            float target = std::clamp(-0.5f * (lum - kReference), -0.2f, 0.45f);
            float tau = target > exposure_ev_ ? 1.2f : 0.5f;  // darker eyes adapt slower
            exposure_ev_ += (target - exposure_ev_) * (1 - std::exp(-dt / tau));
            static const bool log = std::getenv("XBE_EXPOSURE_LOG") != nullptr;
            static int n = 0;
            if (log && n++ % 60 == 0) LOGR("exposure: scene %.2f stops, target %+.2f, now %+.2f", lum, target, exposure_ev_);
        }
    }
    // this frame's measure
    ID3D12Resource* src[3] = {scene_[0].Get(), nullptr, nullptr};
    DXGI_FORMAT fmt[3] = {rt.fmt, rt.fmt, rt.fmt};
    float p[8] = {0, 0, 0, 0, 0, 0, float(rt.w), float(rt.h)};
    Transition(meter_.Get(), meter_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Pass(kMeterHlsl, "meter", DXGI_FORMAT_R32G32B32A32_FLOAT, MeterRtv(), 1, 1, src, fmt, p, -1);
    Transition(meter_.Get(), meter_state_, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, from{};
    dst.pResource = meter_rb_[slot].Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1, 256};
    from.pResource = meter_.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0;
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
    meter_fence_[slot] = fence_value_ + 1;  // this frame's (Submit signals the next value)
}

// The composite's grade (kSceneCommon grade[4]): the exposure, the campaign's tones by level (the Soviet campaign's
// winter cool and a little grey, North Africa warm, Europe near neutral), the grain
void D3D12Backend::Grade() {
    const app::Settings& set = app::Get();
    float exposure = set.auto_exposure ? std::exp2(exposure_ev_) : 1.0f;
    float sat = 1, gain[3] = {1, 1, 1}, lift[3] = {0, 0, 0};
    if (set.color_grade) {
        int level = g_level.load();
        if (level >= 0 && level <= 12) {
            sat = 0.9f, gain[0] = 0.97f, gain[1] = 1.0f, gain[2] = 1.04f, lift[0] = 0.002f, lift[1] = 0.004f, lift[2] = 0.009f;
        } else if (level >= 13 && level <= 18) {
            sat = 1.06f, gain[0] = 1.04f, gain[1] = 1.0f, gain[2] = 0.93f, lift[0] = 0.008f, lift[1] = 0.005f, lift[2] = 0.0f;
        } else if (level > 18) {
            sat = 0.95f, gain[0] = 1.0f, gain[1] = 1.01f, gain[2] = 0.97f, lift[0] = 0.003f, lift[1] = 0.005f, lift[2] = 0.003f;
        }
    }
    bool on = set.auto_exposure || set.color_grade;
    float g[16] = {exposure, sat, 1, on ? 1.0f : 0.0f, gain[0], gain[1], gain[2], 0, lift[0], lift[1], lift[2], 0,
                   set.film_grain && !set.taa ? 0.012f : 0.0f, 0, 0, 0};
    std::memcpy(grade_cb_, g, sizeof(g));
}

// a DDS of the pack into a texture with its mips, readable by the pixel shaders
ComPtr<ID3D12Resource> D3D12Backend::UploadImage(const remaster::Image& im) {
    ComPtr<ID3D12Resource> r;
    if (!im.w || !im.h || im.data.empty()) return r;
    const remaster::Image* one = &im;
    return UploadImages(&one, 1);
}

// DDS files of one format and size into a texture array of as many slices
ComPtr<ID3D12Resource> D3D12Backend::UploadImages(const remaster::Image* const* list, uint32_t count) {
    ComPtr<ID3D12Resource> r;
    const remaster::Image& first = *list[0];
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(first.fmt, first.w, first.h);
    td.MipLevels = UINT16(first.mips);
    td.DepthOrArraySize = UINT16(count);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    for (uint32_t slice = 0; slice < count; slice++) {
    const remaster::Image& im = *list[slice];
    for (uint32_t m = 0; m < im.mips; m++) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
        UINT rows;
        UINT64 row_size, bytes;
        dev_->GetCopyableFootprints(&td, slice * im.mips + m, 1, 0, &fp, &rows, &row_size, &bytes);
        Alloc a = Upload(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        uint32_t rb = im.RowBytes(m), nr = im.Rows(m);
        for (uint32_t y = 0; y < nr && y < rows; y++)
            std::memcpy(a.cpu + size_t(y) * fp.Footprint.RowPitch, im.data.data() + im.offsets[m] + size_t(y) * rb, rb);
        D3D12_TEXTURE_COPY_LOCATION dl{r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dl.SubresourceIndex = slice * im.mips + m;
        D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        sl.PlacedFootprint = fp;
        sl.PlacedFootprint.Offset = a.off;
        cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    }
    }
    Transition(r.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return r;
}


// --- remastered textures streamed in the background -----------------------------------------------------------------
// A thread of its own takes the pack's reads as they finish, makes their textures and copies them on a copy queue;
// the render thread only takes the ones whose copies the GPU finished (a fence), so a burst of new textures (a scene
// turning into view) costs the frames nothing. The textures are left in the common state, which the direct queue's
// reads promote from.
struct D3D12Backend::Streamer {
    D3D12Backend& b;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc[2];
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    uint64_t fence_value = 0;
    HANDLE event = nullptr;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::mutex lock;
    struct Done {
        uint64_t id;
        RemasterTex tex;
        uint64_t fence;
    };
    std::deque<Done> done;
    std::vector<ComPtr<ID3D12Resource>> staging[2];  // upload buffers of the batch in flight on each allocator
    uint64_t alloc_fence[2] = {0, 0};

    explicit Streamer(D3D12Backend& backend) : b(backend) {
        D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_COPY};
        if (FAILED(b.dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return;
        for (auto& a : alloc) b.dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&a));
        b.dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, alloc[0].Get(), nullptr, IID_PPV_ARGS(&list));
        list->Close();
        b.dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        thread = std::thread([this] { Run(); });
    }
    ~Streamer() {
        stop = true;
        if (thread.joinable()) thread.join();
        if (event) CloseHandle(event);
    }

    ComPtr<ID3D12Resource> Texture(const remaster::Image& im, std::vector<ComPtr<ID3D12Resource>>& up) {
        ComPtr<ID3D12Resource> r;
        if (!im.w || !im.h || im.data.empty()) return r;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(im.fmt, im.w, im.h);
        td.MipLevels = UINT16(im.mips);
        if (FAILED(b.CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&r))))
            return nullptr;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(im.mips);
        std::vector<UINT> rows(im.mips);
        std::vector<UINT64> row_size(im.mips);
        UINT64 total = 0;
        b.dev_->GetCopyableFootprints(&td, 0, im.mips, 0, fp.data(), rows.data(), row_size.data(), &total);
        D3D12_HEAP_PROPERTIES uhp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buf;
        if (FAILED(b.CreateResource(&uhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&buf))))
            return nullptr;
        uint8_t* p = nullptr;
        buf->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (uint32_t m = 0; m < im.mips; m++) {
            uint32_t rb = im.RowBytes(m), nr = im.Rows(m);
            for (uint32_t y = 0; y < nr && y < rows[m]; y++)
                std::memcpy(p + fp[m].Offset + size_t(y) * fp[m].Footprint.RowPitch, im.data.data() + im.offsets[m] + size_t(y) * rb, rb);
            D3D12_TEXTURE_COPY_LOCATION dl{r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            dl.SubresourceIndex = m;
            D3D12_TEXTURE_COPY_LOCATION sl{buf.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            sl.PlacedFootprint = fp[m];
            list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        }
        buf->Unmap(0, nullptr);
        up.push_back(buf);
        return r;
    }

    void Run() {
        int slot = 0;
        while (!stop) {
            remaster::Loaded ld;
            std::vector<remaster::Loaded> batch;
            while (batch.size() < 16 && remaster::TakeLoaded(ld)) batch.push_back(std::move(ld));
            if (batch.empty()) {
                Sleep(2);
                continue;
            }
            // the allocator of two batches ago is free once its fence passed
            if (fence->GetCompletedValue() < alloc_fence[slot]) {
                fence->SetEventOnCompletion(alloc_fence[slot], event);
                WaitForSingleObject(event, INFINITE);
            }
            staging[slot].clear();
            alloc[slot]->Reset();
            list->Reset(alloc[slot].Get(), nullptr);
            std::vector<Done> made;
            for (remaster::Loaded& l : batch) {
                Done d{l.id, {}, 0};
                d.tex.m = remaster::Find(l.id);
                d.tex.color = Texture(l.color, staging[slot]);
                d.tex.color_fmt = l.color.fmt;
                d.tex.color_mips = l.color.mips;
                if (!l.normal.data.empty()) {
                    d.tex.normal = Texture(l.normal, staging[slot]);
                    d.tex.mat = Texture(l.mat, staging[slot]);
                    d.tex.normal_fmt = l.normal.fmt;
                    d.tex.mat_fmt = l.mat.fmt;
                    d.tex.normal_mips = l.normal.mips;
                    d.tex.mat_mips = l.mat.mips;
                }
                made.push_back(std::move(d));
            }
            list->Close();
            ID3D12CommandList* lists[] = {list.Get()};
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(fence.Get(), ++fence_value);
            alloc_fence[slot] = fence_value;
            {
                std::lock_guard l(lock);
                for (Done& d : made) {
                    d.fence = fence_value;
                    done.push_back(std::move(d));
                }
            }
            slot ^= 1;
        }
    }

    // render thread: the textures whose copies finished
    template <typename F>
    void Take(F f) {
        uint64_t completed = fence ? fence->GetCompletedValue() : 0;
        std::lock_guard l(lock);
        while (!done.empty() && done.front().fence <= completed) {
            f(done.front().id, std::move(done.front().tex));
            done.pop_front();
        }
    }
};

// the game's replacement textures when it changed them: each with its mips, a box filter down
void D3D12Backend::PumpOverrides() {
    std::vector<TextureOverride> set;
    {
        std::lock_guard l(g_override_lock);
        if (!g_override_new) return;
        g_override_new = false;
        set = std::move(g_override_set);
    }
    for (auto& [id, o] : overrides_) Release(std::move(o.res));
    overrides_.clear();
    for (const TextureOverride& o : set) {
        if (!o.w || !o.h || o.rgba.size() < size_t(o.w) * o.h * 4) continue;
        remaster::Image im;
        im.fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        im.w = o.w, im.h = o.h;
        std::vector<uint8_t> level(o.rgba.begin(), o.rgba.begin() + std::ptrdiff_t(size_t(o.w) * o.h * 4));
        uint32_t w = o.w, h = o.h;
        for (;;) {
            im.offsets.push_back(im.data.size());
            im.data.insert(im.data.end(), level.begin(), level.end());
            im.mips++;
            if (w == 1 && h == 1) break;
            uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
            std::vector<uint8_t> next(size_t(nw) * nh * 4);
            for (uint32_t y = 0; y < nh; y++)
                for (uint32_t x = 0; x < nw; x++)
                    for (uint32_t c = 0; c < 4; c++) {
                        uint32_t x1 = std::min(w - 1, x * 2 + 1), y1 = std::min(h - 1, y * 2 + 1);
                        uint32_t sum = level[(size_t(y * 2) * w + x * 2) * 4 + c] + level[(size_t(y * 2) * w + x1) * 4 + c] +
                                       level[(size_t(y1) * w + x * 2) * 4 + c] + level[(size_t(y1) * w + x1) * 4 + c];
                        next[(size_t(y) * nw + x) * 4 + c] = uint8_t((sum + 2) / 4);
                    }
            level.swap(next);
            w = nw, h = nh;
        }
        OverrideTex t;
        t.res = UploadImage(im);
        t.mips = im.mips;
        if (t.res) overrides_[o.id] = std::move(t);
    }
    LOGR("d3d12: %zu replaced textures", overrides_.size());
}

// the pack's reads that finished: on the GPU, a few a frame
void D3D12Backend::PumpRemaster() {
    if (!details_tried_ && remaster::Active()) {
        details_tried_ = true;
        std::vector<remaster::Image> det = remaster::ReadDetails();
        if (!det.empty()) {
            std::vector<const remaster::Image*> p;
            for (const remaster::Image& im : det) p.push_back(&im);
            details_ = UploadImages(p.data(), uint32_t(p.size()));
            details_fmt_ = det[0].fmt;
            details_mips_ = det[0].mips;
            details_count_ = uint32_t(det.size());
        }
        LOGR("remaster: %zu detail maps", det.size());
    }
    if (!streamer_) streamer_ = std::make_unique<Streamer>(*this);
    streamer_->Take([&](uint64_t id, RemasterTex&& t) { remaster_[id] = std::move(t); });
}

// SMAA's targets in its formats, then the frame's format twice (SMAA's blend, the sharpened frame)
bool D3D12Backend::EnsurePostTargets(DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
    if (post_[kPostRtvs - 1] && post_w_ == w && post_h_ == h && post_fmt_ == fmt) return true;
    for (auto& r : post_)
        if (r) Release(std::move(r));
    post_w_ = post_h_ = 0;
    const DXGI_FORMAT f[kPostRtvs] = {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, fmt, fmt};
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    for (uint32_t i = 0; i < kPostRtvs; i++) {
        D3D12_RESOURCE_DESC td = Texture2DDesc(f[i], w, h);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = f[i];
        post_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, post_state_[i], &cv, IID_PPV_ARGS(&post_[i])))) {
            for (auto& r : post_) r.Reset();
            return false;
        }
        dev_->CreateRenderTargetView(post_[i].Get(), nullptr, PostRtv(i));
    }
    post_w_ = w, post_h_ = h, post_fmt_ = fmt;
    LOGI("d3d12: post targets %ux%u", w, h);
    return true;
}

// SMAA's area (160 x 560, two channels) and search (64 x 16, one) lookups, readable
bool D3D12Backend::EnsureSmaaLookups() {
    if (smaa_lut_[1]) return true;
    struct L {
        DXGI_FORMAT fmt;
        uint32_t w, h, bpp;
        const uint8_t* data;
    } const ls[2] = {{DXGI_FORMAT_R8G8_UNORM, 160, 560, 2, kSmaaArea}, {DXGI_FORMAT_R8_UNORM, 64, 16, 1, kSmaaSearch}};
    static_assert(sizeof(kSmaaArea) == 160 * 560 * 2 && sizeof(kSmaaSearch) == 64 * 16);
    for (int i = 0; i < 2; i++) {
        const L& l = ls[i];
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(l.fmt, l.w, l.h);
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(&smaa_lut_[i])))) {
            smaa_lut_[0].Reset();
            smaa_lut_[1].Reset();
            return false;
        }
        uint32_t row = l.w * l.bpp, pitch = UploadPitch(row);
        Alloc a = Upload(uint64_t(pitch) * l.h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        for (uint32_t y = 0; y < l.h; y++) std::memcpy(a.cpu + size_t(y) * pitch, l.data + size_t(y) * row, row);
        D3D12_TEXTURE_COPY_LOCATION dl{smaa_lut_[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        sl.PlacedFootprint.Offset = a.off;
        sl.PlacedFootprint.Footprint = {l.fmt, l.w, l.h, 1, pitch};
        cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        Transition(smaa_lut_[i].Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    return true;
}

// tests (XBE_AA=smaa): Anti-Aliasing as SMAA and TAA, not 4x MSAA and TAA
bool SmaaMode() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_AA");
        return e && std::strcmp(e, "smaa") == 0;
    }();
    return on;
}

ID3D12Resource* D3D12Backend::PostProcess(Target& t, DXGI_FORMAT& fmt) {
    const app::Settings& set = app::Get();
    ID3D12Resource* src = t.res.Get();
    fmt = t.fmt;
    const bool smaa = set.taa && SmaaMode();
    if (!smaa && !set.sharpening) return src;
    if (!EnsurePostTargets(t.fmt, t.w, t.h)) return src;
    uint32_t w = t.w, h = t.h;
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto into = [&](uint32_t i) { Transition(post_[i].Get(), post_state_[i], D3D12_RESOURCE_STATE_RENDER_TARGET); };
    auto done = [&](uint32_t i) { Transition(post_[i].Get(), post_state_[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); };
    if (smaa && EnsureSmaaLookups()) {
        float p[8] = {1.0f / w, 1.0f / h, float(w), float(h)};
        const float clear[4] = {};
        // edge detection discards where there is no edge: the edges start clear
        into(0);
        cl_->ClearRenderTargetView(PostRtv(0), clear, 0, nullptr);
        ID3D12Resource* e_src[3] = {src, nullptr, nullptr};
        DXGI_FORMAT e_fmt[3] = {fmt, fmt, fmt};
        Pass(SmaaPass(0).c_str(), "smaa_edges", DXGI_FORMAT_R8G8_UNORM, PostRtv(0), w, h, e_src, e_fmt, p, -1);
        done(0);
        into(1);
        cl_->ClearRenderTargetView(PostRtv(1), clear, 0, nullptr);
        ID3D12Resource* w_src[3] = {post_[0].Get(), smaa_lut_[0].Get(), smaa_lut_[1].Get()};
        DXGI_FORMAT w_fmt[3] = {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8_UNORM};
        Pass(SmaaPass(1).c_str(), "smaa_weights", DXGI_FORMAT_R8G8B8A8_UNORM, PostRtv(1), w, h, w_src, w_fmt, p, -1);
        done(1);
        into(2);
        ID3D12Resource* b_src[3] = {src, post_[1].Get(), nullptr};
        DXGI_FORMAT b_fmt[3] = {fmt, DXGI_FORMAT_R8G8B8A8_UNORM, fmt};
        Pass(SmaaPass(2).c_str(), "smaa_blend", fmt, PostRtv(2), w, h, b_src, b_fmt, p, -1);
        done(2);
        src = post_[2].Get();
    }
    if (set.sharpening) {
        float p[8] = {std::min(set.sharpening, 10u) / 10.0f, 0, 0, 0, 0, 0, float(w), float(h)};
        into(3);
        ID3D12Resource* c_src[3] = {src, nullptr, nullptr};
        DXGI_FORMAT c_fmt[3] = {fmt, fmt, fmt};
        Pass(kCasHlsl, "cas", fmt, PostRtv(3), w, h, c_src, c_fmt, p, -1);
        done(3);
        src = post_[3].Get();
    }
    return src;
}


// The 4:3 picture in the middle of the widened frame, in the television: the largest 16:9 area of the window (black
// beyond on wider ones), the picture through the tube, the frame over it
bool D3D12Backend::TvFrame(Target& t, ID3D12Resource* src, DXGI_FORMAT fmt) {
    if (!tv_tried_) {
        tv_tried_ = true;
        remaster::Image im;
        std::filesystem::path p = std::filesystem::path(remaster::Dir()) / "ui" / "tv_frame.dds";
        std::ifstream rect(std::filesystem::path(remaster::Dir()) / "ui" / "tv_frame.txt");
        bool has_rect = bool(rect >> tv_screen_[0] >> tv_screen_[1] >> tv_screen_[2] >> tv_screen_[3]);
        if (has_rect && std::filesystem::exists(p) && remaster::ReadDds(p.wstring(), im)) {
            tv_frame_ = UploadImage(im);
            tv_fmt_ = im.fmt;
            tv_mips_ = im.mips;
        }
        LOGR("d3d12: television frame %s", tv_frame_ ? "loaded" : "not in the pack");
    }
    if (!tv_frame_ || !tv_ui_) return false;
    float w = float(bb_w_), h = float(bb_h_);
    float vw = std::min(w, h * 16.0f / 9.0f), vh = vw * 9.0f / 16.0f;
    float vx = (w - vw) / 2, vy = (h - vh) / 2;
    D3D12_VIEWPORT vp{vx, vy, vw, vh, 0, 1};
    D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    SetViewport(vp);
    SetScissor(scissor);
    if (src == t.res.Get()) Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    uint32_t slot = AllocSrvSlots(4);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = kBgra;
    dev_->CreateShaderResourceView(tv_ui_.Get(), &sv, Cpu(srv_heap_.Get(), slot + 3, srv_inc_));
    sv.Format = fmt;
    dev_->CreateShaderResourceView(src, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    sv.Format = kBgra;
    dev_->CreateShaderResourceView(lut_.Get(), &sv, Cpu(srv_heap_.Get(), slot + 1, srv_inc_));
    sv.Format = tv_fmt_;
    sv.Texture2D.MipLevels = tv_mips_;
    dev_->CreateShaderResourceView(tv_frame_.Get(), &sv, Cpu(srv_heap_.Get(), slot + 2, srv_inc_));
    // the 4:3 part of the widened frame
    float part = 1.0f / widen_;
    Alloc cb = Upload(256, 256);
    float c[16] = {(1 - part) / 2, 0, part, 1,
                   tv_screen_[0], tv_screen_[1], tv_screen_[2], tv_screen_[3],
                   gamma_ ? 1.0f : 0.0f, 0, 0.03f, 1.0f,
                   float(t.w), float(t.h), 0.5f / (float(t.gw) * widen_), 0.5f / float(t.gh)};
    std::memcpy(cb.cpu, c, sizeof(c));
    ID3D12PipelineState* pso = EnsurePassPipeline(kTvHlsl, "psmain", DXGI_FORMAT_B8G8R8A8_UNORM, -1);
    if (!pso) return false;
    DrawFullScreen(pso, cb.gpu, slot, EnsureBlitSamplers());
    tv_on_ = true;
    tv_rect_[0] = vx + tv_screen_[0] * vw, tv_rect_[1] = vy + tv_screen_[1] * vh;
    tv_rect_[2] = vx + tv_screen_[2] * vw, tv_rect_[3] = vy + tv_screen_[3] * vh;
    return true;
}

// the overlay's rows that belong to the picture (g_tv_ui, filled by the overlay last frame) into their layer, clear
// where there are none; the passes' heaps and root again after
void D3D12Backend::TvUi() {
    if (!tv_ui_) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(kBgra, kTvUiW, kTvUiH);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = kBgra;
        if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&tv_ui_)))) return;
        tv_ui_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
        dev_->CreateRenderTargetView(tv_ui_.Get(), nullptr, TvUiRtv());
    }
    Transition(tv_ui_.Get(), tv_ui_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float zero[4] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE rh = TvUiRtv();
    cl_->ClearRenderTargetView(rh, zero, 0, nullptr);
    {
        std::lock_guard lock(app::UiLock());
        auto* list = static_cast<ImDrawList*>(g_tv_ui.load());
        if (list && list->VtxBuffer.Size > 0) {
            cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
            ImDrawData dd;
            dd.Valid = true;
            dd.DisplayPos = ImVec2(0, 0);
            dd.DisplaySize = ImVec2(float(kTvUiW), float(kTvUiH));
            dd.FramebufferScale = ImVec2(1, 1);
            dd.AddDrawList(list);
            ID3D12DescriptorHeap* uh[] = {ui_heap_.Get()};
            cl_->SetDescriptorHeaps(1, uh);
            ImGui_ImplDX12_RenderDrawData(&dd, cl_.Get());
            Begin();
        }
    }
    Transition(tv_ui_.Get(), tv_ui_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void D3D12Backend::BlitFrame(Target& t, ID3D12Resource* src, DXGI_FORMAT fmt) {
    // 16:9 when the console reports a widescreen display, else 4:3; a widened screen at its own aspect
    bool wide = app::Widescreen();
    float aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    if (widen_ > 1.0f) aspect = float(guest_w_) / guest_h_ * widen_;
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    float uv[4] = {0, 0, 1, 1};
    D3D12_VIEWPORT vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
    D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    SetViewport(vp);
    SetScissor(scissor);
    if (src == t.res.Get()) Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // a frame larger than the window is averaged down (supersampling), not point picked
    uint32_t taps = std::clamp(uint32_t(std::ceil(std::max(t.w / vw, t.h / vh) - 0.01f)), 1u, 4u);
    Blit(src, fmt, uv, {.gamma = gamma_, .taps = taps});
}

// a picture for the overlay in the next slot of ImGui's heap, once per key
uint64_t D3D12Backend::UiImage(const std::string& key, const remaster::Image* im) {
    auto it = ui_tex_.find(key);
    if (it != ui_tex_.end()) return it->second.id;
    UiTex t;
    if (im && ui_used_ < kUiDescriptors && (t.res = UploadImage(*im))) {
        uint32_t slot = ui_used_++;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = im->fmt;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = im->mips;
        dev_->CreateShaderResourceView(t.res.Get(), &sv, Cpu(ui_heap_.Get(), slot, srv_inc_));
        t.id = ui_heap_->GetGPUDescriptorHandleForHeapStart().ptr + uint64_t(slot) * srv_inc_;
    }
    uint64_t id = t.id;
    ui_tex_[key] = std::move(t);
    return id;
}

void D3D12Backend::RenderUi() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    g_ui_backend = this;
    if (app::kGame.ui) app::kGame.ui(fps_);
    g_ui_backend = nullptr;
    ImGui::Render();
    ID3D12DescriptorHeap* uh[] = {ui_heap_.Get()};
    cl_->SetDescriptorHeaps(1, uh);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl_.Get());
    cmd_ = CmdState{};  // (ImGui set its own pipeline, viewport, scissor, blend factor, topology, root)
}

void D3D12Backend::DrawFrame(Target* t, uint32_t bi) {
    // the passes on the frame draw to their own targets, before the back buffer is bound
    ID3D12Resource* src = nullptr;
    DXGI_FORMAT fmt = kBgra;
    if (t) src = PostProcess(*t, fmt);
    // the game's 4:3 screens, and a movie however the game plays it (whole frames from the CPU, this frame or the last)
    bool movie = g_tv_movies && presents_ - cpu_frame_ <= 1;
    // once in the television, the picture stays there until the game draws its 3D world (between screens, at the end
    // of a loading screen, the game's own flags drop for a moment)
    bool keep = tv_was_on_ && g_tv_movies && !scene_drawn_;
    bool tv = (g_tv_frame || movie || keep) && widen_ > 1.0f;
    if (t && tv) TvUi();
    D3D12_RESOURCE_STATES bs = D3D12_RESOURCE_STATE_PRESENT;
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), kRtvCount + bi, rtv_inc_);
    float black[4] = {0, 0, 0, 1};
    cl_->ClearRenderTargetView(rh, black, 0, nullptr);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    tv_on_ = false;
    if (t && !(tv && TvFrame(*t, src, fmt))) BlitFrame(*t, src, fmt);
    tv_was_on_ = tv_on_;
    RenderUi();
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_PRESENT);
}

void D3D12Backend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            CurrentFrame().garbage.push_back(it->second.res);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void D3D12Backend::ResetFrame() {
    Frame& f = CurrentFrame();
    if (f.fence && fence_->GetCompletedValue() < f.fence) {
        auto perf_wait = PerfClock::now();
        WaitFence(f.fence);
        g_perf.wait += PerfMs(perf_wait);
    }
    ReadShadowTimes();
    ReadGpuMarks();
    f.alloc->Reset();
    f.garbage.clear();
    f.used = 0;
    f.srv = 0;
    ring_gen_++;
    cl_->Reset(f.alloc.Get(), nullptr);
    Begin();
    GpuMark("start");
}

void D3D12Backend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    // tests (g_frame_stats): every frame's time and what it did, then the report
    {
        struct FrameStat {
            float ms, compile, texup, wait, sync, present;
            uint32_t shaders, psos, texups, newtex, draws;
            double t;
            std::chrono::steady_clock::time_point end;
        };
        static const bool sample = std::getenv("XBE_PERF_SAMPLE") != nullptr;
        static std::vector<FrameStat> stats;
        static auto last = now, start = now;
        static Perf before = g_perf;
        static bool recording = false;
        double ms = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        int mode = gpu::g_frame_stats.load();
        if (mode == 1 && !recording) {
            stats.clear();
            recording = true;
            start = now;
            LOGR("perf: recording");
            rec_start_perf_ = g_perf;
            g_gpu_frames.clear();
            if (sample) sampler::Start();
        } else if (mode == 1) {
            stats.push_back({float(ms), float(g_perf.compile - before.compile), float(g_perf.texup - before.texup),
                             float(g_perf.wait - before.wait), float(g_perf.sync - before.sync),
                             float(g_perf.present - before.present), uint32_t(g_perf.shaders - before.shaders),
                             uint32_t(g_perf.psos - before.psos), uint32_t(g_perf.texups - before.texups),
                             uint32_t(g_perf.newtex - before.newtex), uint32_t(g_perf.draws - before.draws),
                             std::chrono::duration<double>(now - start).count(), now});
        } else if (mode == 2) {
            gpu::g_frame_stats = 0;
            recording = false;
            {
                uint64_t draws = g_perf.draws - rec_start_perf_.draws, hits = g_perf.srv_hits - rec_start_perf_.srv_hits;
                LOGR("perf [%s]: %llu draws, texture tables made already for %.1f%%", gpu::g_frame_stats_label,
                     (unsigned long long)draws, draws ? 100.0 * double(hits) / double(draws) : 0.0);
            }
            if (sample) {
                sampler::Stop();
                // where each thread spent the whole recording (XBE_PERF_SAMPLE_TOP lines, 40 by default)
                static const int top_n = [] {
                    const char* e = std::getenv("XBE_PERF_SAMPLE_TOP");
                    return e ? std::atoi(e) : 40;
                }();
                std::string top = sampler::Profile(start, now, top_n);
                for (size_t at = 0, nl; (nl = top.find('\n', at)) != std::string::npos; at = nl + 1)
                    LOGR("perf [%s]: whole run:%s", gpu::g_frame_stats_label, top.substr(at, nl - at).c_str());
                // XBE_PERF_SAMPLE_STACKS: the whole run's stacks too (innermost first), the most taken
                static const bool stacks = std::getenv("XBE_PERF_SAMPLE_STACKS") != nullptr;
                if (stacks) {
                    std::string st = sampler::Top(start, now, 150);
                    for (size_t at = 0, nl; (nl = st.find('\n', at)) != std::string::npos; at = nl + 1)
                        LOGR("perf [%s]: stack:%s", gpu::g_frame_stats_label, st.substr(at, nl - at).c_str());
                }
            }
            // the GPU's steps: on average, and in its slowest 1% of frames
            if (!g_gpu_frames.empty()) {
                std::vector<size_t> by(g_gpu_frames.size());
                for (size_t i = 0; i < by.size(); i++) by[i] = i;
                std::sort(by.begin(), by.end(), [](size_t a, size_t b) { return g_gpu_frames[a].total > g_gpu_frames[b].total; });
                auto report = [&](size_t count, const char* what) {
                    std::vector<std::pair<std::string, double>> sums;
                    double total = 0;
                    for (size_t k = 0; k < count; k++) {
                        const GpuSteps& f = g_gpu_frames[by[k]];
                        total += f.total;
                        for (auto& [name, ms] : f.steps) {
                            auto it = std::find_if(sums.begin(), sums.end(), [&](auto& e) { return e.first == name; });
                            if (it == sums.end()) sums.push_back({name, ms});
                            else it->second += ms;
                        }
                    }
                    std::sort(sums.begin(), sums.end(), [](auto& a, auto& b) { return a.second > b.second; });
                    std::string line;
                    for (auto& [name, sum] : sums) {
                        char t[64];
                        std::snprintf(t, sizeof(t), " %s %.2f,", name.c_str(), sum / double(count));
                        line += t;
                    }
                    LOGR("perf [%s]: GPU %s: %.2f ms:%s", gpu::g_frame_stats_label, what, total / double(count), line.c_str());
                };
                report(by.size(), "average");
                report(std::max<size_t>(1, by.size() / 100), "slowest 1%");
                g_gpu_frames.clear();
            }
            if (!stats.empty()) {
                std::vector<float> sorted;
                double total = 0;
                for (const FrameStat& f : stats) sorted.push_back(f.ms), total += f.ms;
                std::sort(sorted.begin(), sorted.end(), std::greater<float>());
                size_t n = sorted.size();
                auto worst_avg = [&](double share) {
                    size_t k = std::max<size_t>(1, size_t(double(n) * share));
                    double s = 0;
                    for (size_t i = 0; i < k; i++) s += sorted[i];
                    return 1000.0 / (s / double(k));
                };
                float median = sorted[n / 2];
                size_t spikes = 0;
                for (float m : sorted) spikes += m > median * 2 && m > median + 4;
                LOGR("perf [%s]: %zu frames in %.1f s: average %.1f fps, min %.1f fps (%.2f ms), max %.1f fps (%.2f ms), "
                     "1%% low %.1f fps, 0.1%% low %.1f fps, median %.2f ms, p99 %.2f ms, p99.9 %.2f ms, spikes %zu",
                     gpu::g_frame_stats_label, n, total / 1000, 1000.0 * double(n) / total, 1000.0 / sorted[0], sorted[0],
                     1000.0 / sorted[n - 1], sorted[n - 1], worst_avg(0.01), worst_avg(0.001), median,
                     sorted[std::min(n - 1, n / 100)], sorted[std::min(n - 1, n / 1000)], spikes);
                // when the spikes came: a count for each second
                {
                    std::string when;
                    int second = -1, count = 0;
                    auto flush = [&] {
                        if (count) when += " " + std::to_string(second) + "s:" + std::to_string(count);
                    };
                    for (const FrameStat& f : stats) {
                        if (!(f.ms > median * 2 && f.ms > median + 4)) continue;
                        if (int(f.t) != second) flush(), second = int(f.t), count = 0;
                        count++;
                    }
                    flush();
                    if (!when.empty()) LOGR("perf [%s]: spikes by second:%s", gpu::g_frame_stats_label, when.c_str());
                }
                std::vector<size_t> order(stats.size());
                for (size_t i = 0; i < order.size(); i++) order[i] = i;
                std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return stats[a].ms > stats[b].ms; });
                for (size_t i = 0; i < std::min<size_t>(8, order.size()); i++) {
                    const FrameStat& f = stats[order[i]];
                    LOGR("perf [%s]:   %.2f ms at %.2f s: compile %.1f ms (%u shaders, %u pipelines), %u uploads %.1f ms, %u new "
                         "textures, %u draws, gpu wait %.1f ms, sync %.1f ms, present %.1f ms",
                         gpu::g_frame_stats_label, f.ms, f.t, f.compile, f.shaders, f.psos, f.texups, f.texup, f.newtex,
                         f.draws, f.wait, f.sync, f.present);
                    if (sample && i < 4) {
                        std::string top = sampler::Top(f.end - std::chrono::microseconds(int64_t(f.ms * 1000)), f.end, 10);
                        for (size_t at = 0, nl; (nl = top.find('\n', at)) != std::string::npos; at = nl + 1)
                            LOGR("%s", top.substr(at, nl - at).c_str());
                    }
                }
            }
        }
        before = g_perf;
    }
    // XBE_HITCHES: frames much longer than their neighbours, with what they did
    static const bool hitches = std::getenv("XBE_HITCHES") != nullptr;
    if (hitches) {
        static auto last = now;
        static double avg = 16;
        static Perf before = g_perf;
        double ms = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        {
            static double t0 = 0;
            t0 += ms;
            static float lo = -1, hi = -1;
            if (lo < 0) {
                lo = hi = 0;
                if (const char* e = std::getenv("XBE_FRAMETIMES")) std::sscanf(e, "%f-%f", &lo, &hi);
            }
            if (t0 / 1000 >= lo && t0 / 1000 <= hi) LOGR("frametime %.2f", ms);
        }
        if (ms > avg * 1.25 && ms > 19)
            LOGR("hitch: %.1f ms (usual %.1f): compile %.1f ms %llu shaders %llu pipelines, %llu texture uploads %.1f ms, "
                 "%llu new textures, wait %.1f ms sync %.1f ms, remaster %zu loaded",
                 ms, avg, g_perf.compile - before.compile, (unsigned long long)(g_perf.shaders - before.shaders),
                 (unsigned long long)(g_perf.psos - before.psos), (unsigned long long)(g_perf.texups - before.texups),
                 g_perf.texup - before.texup, (unsigned long long)(g_perf.newtex - before.newtex), g_perf.wait - before.wait,
                 g_perf.sync - before.sync, remaster_.size());
        avg = avg * 0.95 + std::min(ms, avg * 3) * 0.05;
        before = g_perf;
    }
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void LogPerf(Perf& rec_start_perf_) {
    static PerfClock::time_point perf_start = PerfClock::now();
    static uint64_t perf_frames = 0;
    perf_frames++;
    double el = PerfMs(perf_start);
    if (el >= 1000.0) {
        Perf& p = g_perf;
        double n = double(perf_frames);
        double host = p.draw + p.clear + p.copy + p.present + p.sync;
        LOGI("perf: %.1f fps %.2f ms/frame | host %.2f (draw %.2f: assemble %.2f bind %.2f [check %.2f, upload %.2f] "
             "compile %.2f; clear %.2f copy %.2f sync %.2f present %.2f [swap %.2f wait %.2f]) other %.2f",
             n * 1000.0 / el, el / n, host / n, p.draw / n, p.assemble / n, p.bind / n, p.sig / n, p.texup / n,
             p.compile / n, p.clear / n, p.copy / n, p.sync / n, p.present / n, p.swap / n, p.wait / n,
             (el - host) / n);
        LOGI("perf: title thread per frame: recording %.2f ms, waiting %.2f ms (%.1f waits: read-back %.2f "
             "resource %.2f fence %.2f flush %.2f copy %.2f, next frame %.2f) | read-backs waited for: %.1f lock, "
             "%.1f NOOVERWRITE",
             g_perf_capture_ns.exchange(0) / n / 1e6, g_perf_wait_ns.exchange(0) / n / 1e6,
             g_perf_waits.exchange(0) / n, g_perf_wait_kind_ns[0].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[1].exchange(0) / n / 1e6, g_perf_wait_kind_ns[2].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[3].exchange(0) / n / 1e6, g_perf_wait_kind_ns[4].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[5].exchange(0) / n / 1e6, g_perf_sync_block.exchange(0) / n,
             g_perf_sync_late.exchange(0) / n);
        LOGI("perf: per frame %.0f draws (%.0f raw) %.0fk verts %.1f MB upload %.1f overflow | %.0f checks %.2f MB "
             "hashed | %.1f tex uploads %.2f MB, %.1f new tex, %.1f new targets, %.1f target uploads, %.1f shrinks, "
             "%.1f read-backs %.1f late | %llu shaders %llu psos this second",
             p.draws / n, p.raw / n, p.verts / n / 1000.0, p.upload / n / 1048576.0, p.overflow / n, p.sigs / n,
             p.sig_bytes / n / 1048576.0, p.texups / n, p.texup_bytes / n / 1048576.0, p.newtex / n,
             p.targets / n, p.rtups / n, p.shrinks / n, p.syncs / n, p.lates / n,
             (unsigned long long)p.shaders, (unsigned long long)p.psos);
        // (a recording's start moved back by what is dropped: its counts stay whole across the reset)
        rec_start_perf_.draws -= g_perf.draws, rec_start_perf_.srv_hits -= g_perf.srv_hits;
        g_perf = Perf{};
        perf_start = PerfClock::now();
        perf_frames = 0;
    }
}

void D3D12Backend::Present(const Surface& front) {
    MsaaResolve();
    msaa_done_ = false;
    sky_phase_ = true, sky_layer_ = 0;
    PerfScope perf_present{g_perf.present};
    if (cap_) {
        FinishCapture(front);
    } else if (g_capture.exchange(false) && CaptureOn()) {
        StartCapture();
    }
    ResizeSwap();
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t) Resolve(*t);
    if (ramp_dirty_ || app::Get().brightness != brightness_) UploadGammaRamp();
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    uint32_t bi = swap_->GetCurrentBackBufferIndex();
    if (scene_rt_ && !scene_done_) ScenePost();
    ResolveLight();
    light_resolved_ = false;
    casters_.clear();
    veil_.clear();
    // the frame's draws' constants for the next frame's motion (those still in place: none rewritten by a flush)
    std::swap(motion_prev_, motion_cur_);
    motion_prev_gen_ = ring_gen_;
    motion_cur_.Clear();
    std::swap(motion_counts_prev_, motion_order_);
    motion_order_.Clear();
    motion_.clear();
    prev_cam_ok_ = cam_ok_;
    if (cam_ok_) std::memcpy(prev_view_, cam_view_, sizeof(prev_view_)), std::memcpy(prev_proj_, cam_proj_, sizeof(prev_proj_));
    // the next frame's jitter: Halton (2, 3) over 8 frames, within a pixel
    if (app::Get().taa && scene_w_ > 0 && scene_h_ > 0) {
        auto halton = [](uint32_t i, uint32_t b) {
            float f = 1, r = 0;
            for (; i > 0; i /= b) f /= float(b), r += f * float(i % b);
            return r;
        };
        uint32_t i = taa_index_++ % 8 + 1;
        taa_jit_[0] = (2 * halton(i, 2) - 1) / float(scene_w_);
        taa_jit_[1] = (2 * halton(i, 3) - 1) / float(scene_h_);
    } else {
        taa_jit_[0] = taa_jit_[1] = 0;
    }
    cam_ok_ = false;
    shadows_drawn_ = false;
    scene_drawn_ = scene_rt_ != nullptr;
    scene_rt_ = scene_ds_ = nullptr;
    scene_done_ = false;
    occlusion_done_ = false;
    soft_saved_ = false;
    gbuf_used_ = false;
    sky_done_ = false;
    PumpRemaster();
    PumpOverrides();
    DrawFrame(t, bi);
    GpuMark("2d, hud, output");
    std::wstring shot;
    bool shooting = control::TakeShot(shot) && CopyBackBuffer(bi);
    RecordFrame(bi);
    ResolveGpuMarks();
    Submit(false);
    RecordAfterSubmit();
    if (shooting) SaveBackBufferCopy(shot);
    bool vsync = app::Get().vsync;
    auto perf_swap = PerfClock::now();
    swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    g_perf.swap += PerfMs(perf_swap);

    presents_++;
    presents_seen_.store(presents_);
    g_presented.fetch_add(1, std::memory_order_relaxed);
    if (g_log_draws > 0 && --g_log_draws == 0) LOGR("draw: end of frame");
    frame_++;
    ReleaseOldTextures();
    ResetFrame();
    CountFps();
    if (library_dirty_ && std::chrono::steady_clock::now() - library_saved_ > std::chrono::seconds(10)) {
        library_saved_ = std::chrono::steady_clock::now();
        SaveShaderCache();
    }
    if (diag::Perf()) LogPerf(rec_start_perf_);
    ApplySettings();
}

void D3D12Backend::ApplySettings() {
    float want = app::RenderScale(guest_h_);
    float widen = std::max(1.0f, g_screen_widen.load());
    bool remake = ext_ && ext_->Settings();
    if (app::Get().hdr != hdr_) {
        hdr_ = app::Get().hdr;
        remake = true;
    }
    if (std::fabs(want - scale_) > 1e-4f || std::fabs(widen - widen_) > 1e-4f || remake) {
        widen_ = widen;
        Flush();
        for (auto& [key, tg] : targets_) (tg.depth ? free_dsv_ : free_rtv_).push_back(tg.view);
        targets_.clear();
        targets_version_++;
        scale_ = want;
        if (ext_) ext_->Reset(*this);
        bound_rt_ = bound_ds_ = nullptr;
    }
}

// the target resolved, then shrunk with o when it is not guest sized BGRA; res null on failure
D3D12Backend::CopySource D3D12Backend::PrepareCopySource(Target& t, const ShrinkOptions& o) {
    Resolve(t);
    CopySource out{t.res.Get(), nullptr};
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) {
        out.down = Shrink(t.res.Get(), t.state, t.fmt, t.gw, t.gh, o);
        out.res = out.down.Get();
    } else {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    return out;
}

// long unused: the title freed it and other data is at this address now
bool D3D12Backend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return frame_ - used > kStaleFrames;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void D3D12Backend::Sync(uint32_t data, uint32_t) {
    MsaaResolve();
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data)) return;
    PerfScope perf_sync{g_perf.sync};
    g_perf.syncs++;
    if (diag::Perf()) {
        LOG_FIRST(16, "perf: read-back of %08X (%ux%u guest, %ux%u host)", data, it->second.gw, it->second.gh,
                  it->second.w, it->second.h);
    }
    Target& t = it->second;
    // src.down, when there is one, stays alive until ReadBack is done
    CopySource src = PrepareCopySource(t, {});
    if (!src.res) return;
    std::vector<uint8_t> px;
    if (!ReadBack(src.res, t.gw, t.gh, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so a surface locked every frame gets the newest
// finished copy (a frame or two old) and the CPU never waits on the GPU; one locked now and then still waits in Sync
void D3D12Backend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool D3D12Backend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = fence_->GetCompletedValue();
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.fence[i] > l.fence[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) {
        if (diag::Perf()) {
            LOG_FIRST(16, "perf: late %08X nothing finished: done %llu now %llu, slots %llu/%llu %llu/%llu %llu/%llu",
                      data, (unsigned long long)done, (unsigned long long)now, (unsigned long long)l.fence[0],
                      (unsigned long long)l.frame[0], (unsigned long long)l.fence[1], (unsigned long long)l.frame[1],
                      (unsigned long long)l.fence[2], (unsigned long long)l.frame[2]);
        }
        return false;
    }
    if (l.fence[best] > l.delivered) {
        uint8_t* p = nullptr;
        // the last row is not padded to the pitch: a range to the end of the pitch fails to map
        D3D12_RANGE rr{0, size_t(l.row_pitch) * (l.gh - 1) + l.gw * 4};
        if (FAILED(l.rb[best]->Map(0, &rr, reinterpret_cast<void**>(&p)))) return false;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.row_pitch, l.gw * 4);
        D3D12_RANGE none{0, 0};
        l.rb[best]->Unmap(0, &none);
        l.delivered = l.fence[best];
    }
    return true;
}

// a slot set for another size: its buffers go with the frame
void D3D12Backend::ResetLate(Late& l, const Target& t) {
    auto& garbage = CurrentFrame().garbage;
    for (auto& r : l.rb) {
        if (r) garbage.push_back(r);
    }
    if (l.down) garbage.push_back(l.down);
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

// the target as a guest sized BGRA copy source, shrunk into l.down when it is not one already
ID3D12Resource* D3D12Backend::PrepareLateSource(Target& t, Late& l) {
    ShrinkOptions reuse{.into = l.down.Get(), .into_state = l.down_state};
    CopySource src = PrepareCopySource(t, reuse);
    if (src.down) {
        l.down = src.down;
        l.down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    return src.res;
}

// src into the readback buffer of the slot, made on first use
void D3D12Backend::RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src) {
    D3D12_RESOURCE_DESC rd = src->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    if (!l.rb[slot]) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
        if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&l.rb[slot])))) {
            return;
        }
    }
    D3D12_TEXTURE_COPY_LOCATION dl{l.rb[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    l.row_pitch = fp.Footprint.RowPitch;
    l.fence[slot] = fence_value_ + 1;  // the next Submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

void D3D12Backend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || TargetStale(data)) return;
    Target& t = it->second;
    PerfScope perf_sync{g_perf.sync};
    g_perf.lates++;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        if (diag::Perf()) LOG_FIRST(16, "perf: late %08X ring reset, %ux%u -> %ux%u", data, l.gw, l.gh, t.gw, t.gh);
        ResetLate(l, t);
    }
    // a copy for a later lock, unless the GPU is a whole ring behind
    uint32_t slot = l.next;
    if (l.fence[slot] > fence_->GetCompletedValue()) return;
    ID3D12Resource* src = PrepareLateSource(t, l);
    if (!src) return;
    RecordLateCopy(l, slot, src);
}

// filters src into a new w x h target, left as a copy source
ComPtr<ID3D12Resource> D3D12Backend::Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt,
                                            uint32_t w, uint32_t h, const ShrinkOptions& o) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = td.Format;
    ComPtr<ID3D12Resource> down = o.into;
    D3D12_RESOURCE_STATES down_state = o.into ? o.into_state : D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_perf.shrinks++;
    if (free_rtv_.empty()) return nullptr;
    if (down) {
        Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&down)))) {
        return nullptr;
    }
    // borrows the next free view without taking it, only for this recording
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), free_rtv_.back(), rtv_inc_);
    dev_->CreateRenderTargetView(down.Get(), nullptr, rh);
    Transition(src, st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SetBlitTarget(rh, w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(src, fmt, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return down;
}

// a BGRA copy source to the CPU, rows packed; waits for the GPU
bool D3D12Backend::ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    ComPtr<ID3D12Resource> rb;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
    if (FAILED(CreateResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&rb)))) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION dl{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Flush();
    uint8_t* p = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
    out.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++) std::memcpy(&out[size_t(y) * w * 4], p + size_t(y) * fp.Footprint.RowPitch, w * 4);
    rb->Unmap(0, nullptr);
    return true;
}

// BMP file and info header: byte offsets of the fields CapSave writes
constexpr int kBmpHeaderBytes = 54;
constexpr int kBmpFileSize = 2;
constexpr int kBmpPixelOffset = 10;
constexpr int kBmpInfoSize = 14;
constexpr int kBmpWidth = 18;
constexpr int kBmpHeight = 22;  // negative: rows top down
constexpr int kBmpPlanes = 26;
constexpr int kBmpBitCount = 28;
constexpr int kBmpImageSize = 34;
constexpr uint32_t kBmpInfoBytes = 40;

void D3D12Backend::CapSave(const CapImage& img, const char* name) {
    ComPtr<ID3D12Resource> down = Shrink(img.res, *img.state, img.fmt, img.w, img.h, {.mul = img.mul});
    std::vector<uint8_t> px;
    if (!down || !ReadBack(down.Get(), img.w, img.h, px)) return;
    FILE* f = _wfopen((cap_dir_ / (std::string(name) + ".bmp")).c_str(), L"wb");
    if (!f) return;
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};  // BITMAPFILEHEADER and BITMAPINFOHEADER, top down 32 bit
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put(kBmpFileSize, kBmpHeaderBytes + uint32_t(px.size()));
    put(kBmpPixelOffset, kBmpHeaderBytes);
    put(kBmpInfoSize, kBmpInfoBytes);
    put(kBmpWidth, img.w);
    put(kBmpHeight, uint32_t(-int32_t(img.h)));
    hd[kBmpPlanes] = 1;
    hd[kBmpBitCount] = 32;
    put(kBmpImageSize, uint32_t(px.size()));
    fwrite(hd, 1, kBmpHeaderBytes, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

void D3D12Backend::CapSave(Target& t, const char* name) {
    DXGI_FORMAT fmt = t.depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : t.fmt;
    float mul = t.depth ? 1 : t.range;
    CapSave({.res = t.res.Get(), .state = &t.state, .fmt = fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

// the frame moves to another target: save the one it leaves
void D3D12Backend::CapStep(const Surface& rt) {
    uint32_t d = rt.header ? mem::VirtualFromPhysical(rt.data) : 0;
    if (d == cap_rt_) return;
    auto it = targets_.find(TargetMapKey(cap_rt_, false));
    if (cap_rt_ && it != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X", cap_i_, cap_rt_);
        CapSave(it->second, name);
    }
    auto z = targets_.find(TargetMapKey(cap_rt_, true));
    if (cap_rt_ && z != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X_z", cap_i_, cap_rt_);
        CapSave(z->second, name);
    }
    cap_rt_ = d;
}

void D3D12Backend::Capture(Target& t, const char* tag, float mul) {
    if (!cap_) return;
    char name[32];
    snprintf(name, sizeof(name), "%04u_%s_%08X", cap_i_, tag, t.data);
    CapSave({.res = t.res.Get(), .state = &t.state, .fmt = t.fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

void D3D12Backend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    ramp_dirty_ = true;
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<D3D12Backend>(); }

bool TvScreen(float rect[4]) {
    if (!g_ui_backend || !g_ui_backend->tv_on_) return false;
    std::memcpy(rect, g_ui_backend->tv_rect_, sizeof(float) * 4);
    return true;
}

uint64_t UiTexture(const std::string& pack_path) {
    if (!g_ui_backend) return 0;
    std::filesystem::path p = std::filesystem::path(remaster::Dir()) / pack_path;
    remaster::Image im;
    bool ok = std::filesystem::exists(p) && remaster::ReadDds(p.wstring(), im);
    return g_ui_backend->UiImage("pack:" + pack_path, ok ? &im : nullptr);
}

uint64_t UiImage(const std::string& name, uint32_t w, uint32_t h, const uint8_t* rgba) {
    if (!g_ui_backend) return 0;
    remaster::Image im;
    im.fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    im.w = w, im.h = h, im.mips = 1;
    im.offsets = {0};
    im.data.assign(rgba, rgba + size_t(w) * h * 4);
    return g_ui_backend->UiImage("img:" + name, &im);
}

void SetTextureOverrides(std::vector<TextureOverride> set) {
    std::lock_guard l(g_override_lock);
    g_override_set = std::move(set);
    g_override_new = true;
}

}  // namespace gpu
