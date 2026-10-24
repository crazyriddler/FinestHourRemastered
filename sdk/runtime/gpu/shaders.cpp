#include "gpu/shaders.h"
#include "gpu/keymap.h"
#include "core/log.h"
#include "gpu/vertex.h"

#include <algorithm>
#include <cctype>
#include <bit>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gpu {

namespace {

// the sampler the material's own maps (normal, material, detail) are read through: s6, anisotropic at 4 (tests:
// XBE_TEST_MAT_TRI, s5's trilinear; XBE_TEST_MAT_STAGE, the stage's as the colour)
std::string MatSampler(int i) {
    static const bool tri = std::getenv("XBE_TEST_MAT_TRI") != nullptr, stage = std::getenv("XBE_TEST_MAT_STAGE") != nullptr;
    return tri ? std::string("mtri") : stage ? "smp" + std::to_string(i) : std::string("mmatsmp");
}

std::string F(const char* fmt, ...) {
    // (as long as it comes out: a block past a fixed buffer was cut short, a shader that no longer compiled)
    va_list ap, again;
    va_start(ap, fmt);
    va_copy(again, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string out(size_t(std::max(n, 0)), '\0');
    vsnprintf(out.data(), out.size() + 1, fmt, again);
    va_end(again);
    return out;
}

const char* kCommonHead = R"(
struct VOut {
    float4 pos : SV_Position;
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float4 b0 : COLOR2;
    float4 b1 : COLOR3;
    float4 tc0 : TEXCOORD0;
    float4 tc1 : TEXCOORD1;
    float4 tc2 : TEXCOORD2;
    float4 tc3 : TEXCOORD3;
    float fogamt : TEXCOORD4;
    float4 cpos : TEXCOORD6;  // clip position, for the remastered materials' view space
    float4 pn : TEXCOORD7;    // the normal, for per pixel lighting (PixelLightingId, FixedLightingId)
    float4 pv : TEXCOORD8;    // w 1: view space position (fixed function), w 2: a program's own position
    float psize : TEXCOORD9;  // a point's size in console pixels (PointSpriteGsHlsl)
};
cbuffer VC : register(b0) { float4 c[192]; };
cbuffer DC : register(b1) {
    float4 surf;
    float4 fogp;
    float4 fogcolor;
    float4 misc;
    float4 pc[18];
    float4 tfactor;
    float4 texsize[4];
    float4 bumpmat[4];
    float4 bumplum[4];
    float4x4 wvp;
)";
const char* kCommonTail = R"(    float4 spread[4];
    float4x4 wv;
    float4x4 wvn;
    float4x4 tm[4];
    float4 mat[5];
    float4 amb;
    float4 lt[56];
    float4 layout2d;
    float4 mtl[4];
    float4 mrefl;
    float4 tess;
    float4 tess2;
    float4 jitter;
    float4 pts0;  // points: D3DRS_POINTSIZE, its min, max, scale on (d3d12.cpp PointConstants)
    float4 pts1;  // the scale's A, B, C; point sprites on
    float4 flow;  // falling water: the time (0 none), the speed along v, the brightness, the opacity (d3d12.cpp)
    float4 wave[3];  // Water Interaction: world x, world z from view space; the field's corner, 1 / size, slope scale
    float4 trail[2];  // Snow Tracks: the trail's corner, 1 / size, strength; world y from view space
    float4 dtl;  // Detail Textures: the grain's slice in t6 (-1 none), its strength, Character Detail's tiling, the grain's texels per tile
    row_major float4x4 sunprev;  // the view space now to the frame before's clip space, and to this frame's (sunvis)
    row_major float4x4 sunnow;
    float4 layout2y;  // 2D placed by the game: y * layout2y.x + layout2y.y, console pixels (layout2d.xy for x)
};
)";

const ShaderHooks& Hooks() {
    static const ShaderHooks plain;
    return g_shader_hooks ? *g_shader_hooks : plain;
}

std::string Common() { return kCommonHead + Hooks().Constants() + kCommonTail; }

// The weapon in hand on a wide screen: a triangle that shows nothing of the console's 4:3 view (layout2d.z its half
// width in clip space) is not drawn. Its homogeneous corners are clipped by the near plane, the 4:3 view's sides, its
// top and bottom; nothing left, nothing drawn (the cut end of an arm, which the game never showed, by the screen's
// left edge).
const char* kWeaponCullGs = R"(
[maxvertexcount(3)]
void gs(triangle VOut v[3], inout TriangleStream<VOut> s) {
    float hw = layout2d.z;
    if (hw > 0) {
        float4 poly[8], next[8];
        [unroll] for (int z = 0; z < 8; z++) poly[z] = 0, next[z] = 0;
        uint n = 3;
        poly[0] = v[0].pos, poly[1] = v[1].pos, poly[2] = v[2].pos;
        const float4 planes[5] = {float4(0, 0, 1, 0), float4(1, 0, 0, hw), float4(-1, 0, 0, hw),
                                  float4(0, 1, 0, 1), float4(0, -1, 0, 1)};
        [unroll] for (int k = 0; k < 5; k++) {
            uint m = 0;
            [unroll] for (uint i = 0; i < 8; i++) {
                if (i >= n) break;
                float4 a = poly[i], b = poly[(i + 1) % n];
                float da = dot(a, planes[k]), db = dot(b, planes[k]);
                if (da >= 0 && m < 8) next[m++] = a;
                if ((da >= 0) != (db >= 0) && m < 8) next[m++] = lerp(a, b, da / (da - db));
            }
            n = m;
            [unroll] for (uint j = 0; j < 8; j++) poly[j] = next[j];
            if (n == 0) return;
        }
    }
    s.Append(v[0]);
    s.Append(v[1]);
    s.Append(v[2]);
}
)";

// vertex programs

const char* kVsHelpers = R"(
float4 mac_mul(float4 a, float4 b) { return (a == 0 || b == 0) ? (float4)0 : a * b; }
int mac_arl(float v) { return (int)floor(v + 1e-3); }
// magnitude clamped between 1 / 2^64 and 2^64, sign kept
float magclamp(float v) { float m = clamp(abs(v), 5.42101e-20, 1.84467e+19); return v < 0 ? -m : m; }
float4 ilu_rcp(float v) { return (1.0 / v).xxxx; }
float4 ilu_rcc(float v) { return magclamp(1.0 / v).xxxx; }
float4 ilu_rsq(float v) { return v == 0 ? (1.#INF).xxxx : (rsqrt(abs(v))).xxxx; }
// 2^floor(v), fraction, 2^v
float4 ilu_exp(float v) {
    float whole = floor(v);
    return float4(exp2(whole), v - whole, exp2(v), 1);
}
// exponent, mantissa, log2 of |v|; zero logs to negative infinity
float4 ilu_log(float v) {
    float mag = abs(v), l2 = log2(mag), ex = floor(l2);
    return mag == 0 ? float4(-1.#INF, 1, -1.#INF, 1) : float4(ex, mag / exp2(ex), l2, 1);
}
// power clamped to 128 less 1/256
float4 ilu_lit(float4 s) {
    const float cap = 127.99609375;
    float diffuse = max(s.x, 0);
    float spec = exp2(clamp(s.w, -cap, cap) * log2(max(s.y, 0)));
    return float4(1, diffuse, diffuse > 0 ? spec : 0, 1);
}
float fogfactor(float d) {
    if (fogp.w == 3) return (fogp.y - d) / (fogp.y - fogp.x);
    if (fogp.w == 1) return exp(-d * fogp.z);
    if (fogp.w == 2) return exp(-(d * fogp.z) * (d * fogp.z));
    return 1;
}
)";

struct Tok {
    const uint32_t* t;
    uint32_t f(int sub, int bit, int len) const { return (t[sub] >> bit) & ((1u << len) - 1); }
};

const char* kMask[16] = {"", "w", "z", "zw", "y", "yw", "yz", "yzw", "x", "xw", "xz", "xzw", "xy", "xyw", "xyz", "xyzw"};
const char* kLanes[5] = {"", "x", "xy", "xyz", "xyzw"};  // oFog takes one lane per mask bit, from x
const char* kOut[16] = {"oPos", "", "", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1", "oT0", "oT1", "oT2", "oT3", "", "", ""};

// operand A 0, B 1, C 2
std::string Src(const Tok& k, int which, uint16_t regs) {
    uint32_t neg, sw[4], reg, mux;
    if (which == 0) {
        neg = k.f(1, 8, 1);
        sw[0] = k.f(1, 6, 2), sw[1] = k.f(1, 4, 2), sw[2] = k.f(1, 2, 2), sw[3] = k.f(1, 0, 2);
        reg = k.f(2, 28, 4), mux = k.f(2, 26, 2);
    } else if (which == 1) {
        neg = k.f(2, 25, 1);
        sw[0] = k.f(2, 23, 2), sw[1] = k.f(2, 21, 2), sw[2] = k.f(2, 19, 2), sw[3] = k.f(2, 17, 2);
        reg = k.f(2, 13, 4), mux = k.f(2, 11, 2);
    } else {
        neg = k.f(2, 10, 1);
        sw[0] = k.f(2, 8, 2), sw[1] = k.f(2, 6, 2), sw[2] = k.f(2, 4, 2), sw[3] = k.f(2, 2, 2);
        reg = k.f(2, 0, 2) << 2 | k.f(3, 30, 2), mux = k.f(3, 28, 2);
        uint32_t ilu = k.f(1, 25, 3);
        if (ilu >= 2 && ilu <= 6) sw[1] = sw[2] = sw[3] = sw[0];  // scalar ops read x
    }
    // mux: 1 temporary (R12 reads oPos), 2 input, 3 constant
    std::string base;
    if (mux == 1) {
        base = reg == 12 ? "oPos" : F("R%u", reg);
    } else if (mux == 2) {
        uint32_t v = k.f(1, 9, 4);
        base = (regs >> v) & 1 ? F("i.v%u", v) : (v == 3 ? "float4(1,1,1,1)" : "float4(0,0,0,1)");
    } else {
        uint32_t cr = k.f(1, 13, 8);
        base = k.f(3, 1, 1) ? F("c[clamp(a0 + %u, 0, 191)]", cr) : F("c[%u]", cr);
    }
    const char* xyzw = "xyzw";
    std::string s = F("(%s(%s).%c%c%c%c)", neg ? "-" : "", base.c_str(), xyzw[sw[0]], xyzw[sw[1]], xyzw[sw[2]], xyzw[sw[3]]);
    return s;
}

// MAC opcodes from 1: mov mul add mad dp3 dph dp4 dst min max slt sge arl
std::string MacExpr(uint32_t op, const std::string& a, const std::string& b, const std::string& c) {
    switch (op) {
        case 1: return a;
        case 2: return "mac_mul(" + a + ", " + b + ")";
        case 3: return "(" + a + " + " + c + ")";
        case 4: return "(mac_mul(" + a + ", " + b + ") + " + c + ")";
        case 5: return "dot(" + a + ".xyz, " + b + ".xyz).xxxx";
        case 6: return "dot(float4(" + a + ".xyz, 1), " + b + ").xxxx";
        case 7: return "dot(" + a + ", " + b + ").xxxx";
        case 8: return "float4(1, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)";
        case 9: return "min(" + a + ", " + b + ")";
        case 10: return "max(" + a + ", " + b + ")";
        case 11: return "(float4)(" + a + " < " + b + ")";
        case 12: return "(float4)(" + a + " >= " + b + ")";
        case 13: return a;
        default: return "(float4)0";
    }
}

// ILU opcodes from 1: mov rcp rcc rsq exp log lit
std::string IluExpr(uint32_t op, const std::string& c) {
    switch (op) {
        case 1: return c;
        case 2: return "ilu_rcp(" + c + ".x)";
        case 3: return "ilu_rcc(" + c + ".x)";
        case 4: return "ilu_rsq(" + c + ".x)";
        case 5: return "ilu_exp(" + c + ".x)";
        case 6: return "ilu_log(" + c + ".x)";
        case 7: return "ilu_lit(" + c + ")";
        default: return "(float4)0";
    }
}

std::string RegDest(uint32_t r) { return r == 12 ? "oPos" : F("R%u", r); }

std::string VsInputStruct(uint16_t regs) {
    std::string s = "struct VIn {\n";
    for (int r = 0; r < 16; r++)
        if ((regs >> r) & 1) s += F("    float4 v%d : ATTR%d;\n", r, r);
    if (!regs) s += "    uint id : SV_VertexID;\n";
    return s + "};\n";
}

// the input struct and the head of main: elements read raw are finished into float4 i.vN first
std::string VsInputs(uint16_t regs, uint64_t decode) {
    if (!decode) return VsInputStruct(regs) + "VOut main(VIn i) {\n";
    std::string raw = "struct VRaw {\n", in = "struct VIn {\n", body;
    bool packed = false;
    for (int r = 0; r < 16; r++) {
        if (!((regs >> r) & 1)) continue;
        uint32_t d = uint32_t(decode >> (r * 4)) & 15;
        const char* type = d == kDecodeInt || d == kDecodeIntW1 ? "int4" : d == kDecodePacked ? "uint" : "float4";
        raw += F("    %s v%d : ATTR%d;\n", type, r, r);
        in += F("    float4 v%d;\n", r);
        packed |= d == kDecodePacked;
        switch (d) {
            case kDecodeW1: body += F("    i.v%d = float4(ri.v%d.xyz, 1);\n", r, r); break;
            case kDecodeInt: body += F("    i.v%d = float4(ri.v%d);\n", r, r); break;
            case kDecodeIntW1: body += F("    i.v%d = float4(float3(ri.v%d.xyz), 1);\n", r, r); break;
            case kDecodePacked: body += F("    i.v%d = UnpackNp3(ri.v%d);\n", r, r); break;
            case kDecodeFloat2H: body += F("    i.v%d = float4(ri.v%d.xy, 0, ri.v%d.z);\n", r, r, r); break;
            default: body += F("    i.v%d = ri.v%d;\n", r, r); break;
        }
    }
    std::string s = raw + "};\n" + in + "};\n";
    if (packed)
        s += "float4 UnpackNp3(uint u) {\n"
             "    int3 v = int3(int(u << 21) >> 21, int(u << 10) >> 21, int(u) >> 22);\n"
             "    return float4(max(float3(v) / float3(1023, 1023, 511), -1), 1);\n"
             "}\n";
    return s + "VOut main(VRaw ri) {\n    VIn i;\n" + body;
}

const char* kVsEpilogue = R"(
    VOut o = (VOut)0;
    precise float4 p = oPos;
    p.w = magclamp(p.w);
    precise float4 q;
    // full screen passes drawn half a pixel up-left for the console's integer pixel centers stop half a pixel short
    // of the far edges on D3D12's, which upscaled leaves the last host columns and rows undrawn
    if (abs(p.x - (surf.x - 0.5)) < 1e-3) p.x = surf.x;
    if (abs(p.y - (surf.y - 0.5)) < 1e-3) p.y = surf.y;
    if (mrefl.w > 0) {
        float tip = saturate(1.2 - oT0.y);
        float ph = tess2.y + p.x * 0.004 + p.y * 0.003;
        float gust = sin(ph * 1.3) * 0.6 + sin(ph * 2.9 + 1.7) * 0.3 + sin(ph * 6.1) * 0.1;
        p.x += gust * tip * mrefl.w / max(p.w, 1);
        p.y += sin(ph * 2.3 + 0.5) * tip * mrefl.w * 0.25 / max(p.w, 1);
    }
    p.x = p.x * layout2d.x + layout2d.y;
    p.y = p.y * layout2y.x + layout2y.y;
    q.x = p.x * 2 / surf.x - 1;
    q.y = 1 - p.y * 2 / surf.y;
    q.z = p.z * surf.z;
    q.xyz *= p.w;
    q.w = p.w;
    o.pos = q;
    o.cpos = q;
    o.pos.xy += jitter.xy * q.w;
    if (jitter.w > 0) o.pos.z = o.pos.w * jitter.w;  // a sky piece's own depth (d3d12.cpp sky_phase_)
    o.d0 = saturate(oD0);
    o.d1 = saturate(oD1);
    o.b0 = saturate(oB0);
    o.b1 = saturate(oB1);
    o.tc0 = oT0;
    o.tc1 = oT1;
    o.tc2 = oT2;
    o.tc3 = oT3;
    o.fogamt = fogfactor(oFog.x);
    // a point's size: the program's own (oPts), else the render state's
    o.psize = oPts.x >= 0 ? oPts.x : pts0.x;
    return o;
}
)";

const char* kVsPrologue = R"(
    int a0 = 0;
    // precise: EQUAL depth passes need bit exact positions
    precise float4 R0 = 0, R1 = 0, R2 = 0, R3 = 0, R4 = 0, R5 = 0, R6 = 0, R7 = 0, R8 = 0, R9 = 0, R10 = 0, R11 = 0;
    precise float4 oPos = 0;
    float4 oD0 = float4(0, 0, 0, 1), oD1 = 0, oB0 = float4(0, 0, 0, 1), oB1 = 0, oFog = 0, oPts = -1;
    float4 oT0 = float4(0, 0, 0, 1), oT1 = float4(0, 0, 0, 1), oT2 = float4(0, 0, 0, 1), oT3 = float4(0, 0, 0, 1);
)";

// combiners

struct In {
    uint32_t reg, chan, mod;
};
In ParseIn(uint32_t v) { return {v & 0xF, v & 0x10, v & 0xE0}; }

struct Ps {
    const PixelKey& k;
    int stage = 0;
    std::string e, f;  // final combiner E F for EF_PROD
    bool clamp_sum = false, inv_v1 = false, inv_r0 = false;
    std::string code;

    std::string Var(uint32_t reg) {
        uint32_t flags = k.count >> 8;  // PS_COMBINERCOUNT flags: 0x10 unique C0 per stage, 0x100 unique C1
        switch (reg) {
            case 0: return "float4(0,0,0,0)";
            case 1: return F("pc[%d]", stage == 8 ? 16 : ((flags & 0x10) ? stage * 2 : 0));
            case 2: return F("pc[%d]", stage == 8 ? 17 : ((flags & 0x100) ? stage * 2 + 1 : 1));
            case 3: return "fogin";
            case 4: return "v0";
            case 5: return "v1";
            case 8: return "t0";
            case 9: return "t1";
            case 10: return "t2";
            case 11: return "t3";
            case 12: return "r0";
            case 13: return "r1";
            case 14:
                return F("%s(float4(%s.rgb + %s.rgb, 0))", clamp_sum ? "saturate" : "", inv_v1 ? "(1 - v1)" : "v1",
                         inv_r0 ? "(1 - r0)" : "r0");
            case 15: return "float4(" + e + " * " + f + ", 0)";
            default: return "float4(0,0,0,0)";
        }
    }

    std::string Input(In in, bool alpha) {
        std::string r = Var(in.reg);
        if (!alpha) r += in.chan ? ".aaa" : ".rgb";
        else r += in.chan ? ".a" : ".b";
        switch (in.mod) {
            // PS_INPUTMAPPING
            case 0x00: return "max(" + r + ", 0)";
            case 0x20: return "(1 - saturate(" + r + "))";
            case 0x40: return "(2 * max(" + r + ", 0) - 1)";
            case 0x60: return "(-2 * max(" + r + ", 0) + 1)";
            case 0x80: return "(max(" + r + ", 0) - 0.5)";
            case 0xA0: return "(-max(" + r + ", 0) + 0.5)";
            case 0xC0: return r;
            default: return "-" + r;
        }
    }

    static std::string Map(const std::string& v, uint32_t m) {
        switch (m) {
            // PS_COMBINEROUTPUT bias and shift
            case 0x08: return "(" + v + " - 0.5)";
            case 0x10: return "(" + v + " * 2)";
            case 0x18: return "((" + v + " - 0.5) * 2)";
            case 0x20: return "(" + v + " * 4)";
            case 0x30: return "(" + v + " / 2)";
            default: return v;
        }
    }

    static const char* Dest(uint32_t reg) {
        switch (reg) {
            case 4: return "v0";
            case 5: return "v1";
            case 8: return "t0";
            case 9: return "t1";
            case 10: return "t2";
            case 11: return "t3";
            case 12: return "r0";
            case 13: return "r1";
            default: return nullptr;
        }
    }

    // rgb and alpha halves read the registers before either writes, so the writes come back to the caller
    std::string Stage(uint32_t inputs, uint32_t output, bool alpha) {
        In d = ParseIn(inputs & 0xFF), c = ParseIn((inputs >> 8) & 0xFF), b = ParseIn((inputs >> 16) & 0xFF),
           a = ParseIn(inputs >> 24);
        // fl: 1 cd dot, 2 ab dot, 4 mux, 0x38 mapping, 0x40 and 0x80 blue to alpha
        uint32_t cd_dst = output & 0xF, ab_dst = (output >> 4) & 0xF, sum_dst = (output >> 8) & 0xF, fl = output >> 12;
        const char* m = alpha ? "a" : "rgb";
        const char* T = alpha ? "float" : "float3";
        const char* n = alpha ? "a" : "c";
        std::string A = Input(a, alpha), B = Input(b, alpha), C = Input(c, alpha), D = Input(d, alpha);
        std::string ab = (fl & 2) ? "dot(" + A + ", " + B + ")" : "(" + A + " * " + B + ")";
        std::string cd = (fl & 1) ? "dot(" + C + ", " + D + ")" : "(" + C + " * " + D + ")";
        uint32_t map = fl & 0x38;
        code += F("%s ab%s = clamp((%s)%s, -1, 1);\n", T, n, T, Map(ab, map).c_str());
        code += F("%s cd%s = clamp((%s)%s, -1, 1);\n", T, n, T, Map(cd, map).c_str());
        std::string sum = (fl & 4) ? F("((%s) ? (%s)%s : (%s)%s)",
                                       ((k.count >> 8) & 1) ? "r0.a >= 0.5" : "(uint(r0.a * 255) & 1) == 1", T,
                                       cd.c_str(), T, ab.c_str())
                                   : "(" + ab + " + " + cd + ")";
        code += F("%s ms%s = clamp((%s)%s, -1, 1);\n", T, n, T, Map(sum, map).c_str());
        std::string post;
        if (const char* r = Dest(ab_dst)) {
            post += F("%s.%s = ab%s;\n", r, m, n);
            if (!alpha && (fl & 0x80)) post += F("%s.a = abc.b;\n", r);
        }
        if (const char* r = Dest(cd_dst)) {
            post += F("%s.%s = cd%s;\n", r, m, n);
            if (!alpha && (fl & 0x40)) post += F("%s.a = cdc.b;\n", r);
        }
        if (const char* r = Dest(sum_dst)) post += F("%s.%s = ms%s;\n", r, m, n);
        return post;
    }
};

// a dot stage's operand: the source stage's rgb, raw or read back as signed bytes
std::string DotInput(uint32_t map, uint32_t src) {
    switch (map) {
        case 1: return F("sbyte(t%u.rgb, 128, 128, 127)", src);        // offset by 128
        case 2: return F("sbyte(t%u.rgb, -0.5, 255.5, 127.5)", src);  // two's complement, (2b + 1) / 255
        case 3: return F("sbyte(t%u.rgb, 0, 256, 127)", src);         // two's complement
        default: return F("t%u.rgb", src);
    }
}

// texture shader stage i, per its mode
std::string TexFetch(const PixelKey& k, int i) {
    uint32_t mode = (k.tex_modes >> (i * 5)) & 0x1F;
    const TexInfo& t = k.tex[i];
    uint32_t in = i == 1 ? 0 : i == 2 ? (k.input_texture >> 16) & 0xF : (k.input_texture >> 20) & 0xF;
    uint32_t dm = i == 0 ? 0 : (k.dot_mapping >> ((i - 1) * 4)) & 0xF;
    const std::string tc = F("tc%d", i);
    const char* q = tc.c_str();
    std::string norm = (t.rect ? F(" / texsize[%d].xy", i) : "") + F(" + texsize[%d].zw", i);
    auto put = [&](const std::string& v) { return F("float4 t%d = %s;\n", i, v.c_str()); };
    // dpN, or dpNn for stage N + 1's product taken early
    auto dp = [&](int n, int coord, uint32_t map, uint32_t src) {
        return F("float dp%d%s = dot(tc%d.xyz, %s);\n", n, coord == n ? "" : "n", coord, DotInput(map, src).c_str());
    };
    auto tex2d = [&](const std::string& uv) {
        if (t.flags & kTexWide) return F("spreadtap(tex%d, smp%d, %s, spread[%d], %s)", i, i, uv.c_str(), i, Hooks().ReadScale(t).c_str());
        return F("tex%d.Sample(smp%d, %s)", i, i, uv.c_str());
    };
    auto cube = [&](const std::string& dir) {
        std::string c = Hooks().Cube(t, i, dir);
        return c.empty() ? F("tex%d.Sample(smp%d, %s)", i, i, dir.c_str()) : c;
    };
    auto sample2 = [&](const std::string& uv) {
        if (t.kind == kTex2D) return tex2d(F("(%s)%s", uv.c_str(), norm.c_str()));
        if (t.kind == kTexCube) return cube("float3(" + uv + ", 1)");
        if (t.kind == kTex3D) return F("tex%d.Sample(smp%d, float3(%s, 0))", i, i, uv.c_str());
        return std::string("float4(0,0,0,1)");
    };
    auto sample3 = [&](const std::string& uvw) {
        if (t.kind == kTex2D) return tex2d(F("(%s).xy%s", uvw.c_str(), norm.c_str()));
        if (t.kind == kTexCube) return cube(uvw);
        if (t.kind == kTex3D) return F("tex%d.Sample(smp%d, %s)", i, i, uvw.c_str());
        return std::string("float4(0,0,0,1)");
    };
    auto depth = [&](const std::string& ref) {
        return put(F("shadow(tex%d, smp%d, (%s.xy / %s.w)%s, %s, %d, i.pos.xy, texsize[%d].xy)", i, i, q, q, norm.c_str(), ref.c_str(),
                     t.shadow, i));
    };
    std::string s;
    switch (mode) {
        // PS_TEXTUREMODES: 1 project2d, 2 project3d, 3 cubemap, 4 passthru, 5 clipplane, 6 and 7 bumpenvmap, 9 dot_st, 0xA dot_zw, 0xD dot_str_3d, 0xE dot_str_cube, 0xF dpndnt_ar, 0x10 dpndnt_gb, 0x11 dotproduct
        case 0: return put("float4(0,0,0,1)");
        case 1:
            if (t.shadow) return depth("0");
            // a flame's sprite (layout2d.w in 1..50: a glowing sprite, not light alone; tess2.x the time) moves inside:
            // its texture drawn through a rising, wavering flow, as a flame's own frames would
            if (i == 0)
                return F("float2 fuv%d = %s.xy / %s.w;\n"
                         "if (flow.x > 0) fuv%d = flowuv(fuv%d);\n"
                         "if (layout2d.w > 1 && layout2d.w < 50 && tess2.x > 0) {\n"
                         "    float2 fp = fuv%d * 6 + float2(0, tess2.x * 1.6);\n"
                         "    fuv%d += float2(sin(fp.y * 2.1 + fp.x * 0.7), sin(fp.x * 1.7 - tess2.x * 3.1)) * 0.025;\n"
                         "}\n", i, q, q, i, i, i, i) +
                       put(sample2(F("fuv%d", i)));
            return put(sample2(F("%s.xy / %s.w", q, q)));
        case 2: return t.shadow ? depth(F("%s.z / %s.w", q, q)) : put(sample3(F("%s.xyz / %s.w", q, q)));
        case 3: return put(sample3(tc + ".xyz"));
        case 4: return put(tc);
        case 5:  // clip plane: a set compare_mode bit kills at >= 0, a clear one below 0
            s = put("0");
            for (int j = 0; j < 4; j++) s += F("if (%s.%c %s 0) discard;\n", q, "xyzw"[j], ((k.compare_mode >> (i * 4 + j)) & 1) ? ">=" : "<");
            return s;
        case 6:
        case 7:  // bump env map: signed du dv through the 2x2 matrix, 7 scales by luminance
            s = F("float2 duv%d = sbyte(t%u.bgr, 0, 256, 127).xy;\n", i, in);
            s += put(sample2(F("%s.xy + float2(dot(bumpmat[%d].xz, duv%d), dot(bumpmat[%d].yw, duv%d))", q, i, i, i, i)));
            if (mode == 7) s += F("t%d *= bumplum[%d].x * t%u.r + bumplum[%d].y;\n", i, i, in, i);
            return s;
        case 9: return dp(i, i, dm, in) + put(sample2(F("float2(dp%d, dp%d)", i - 1, i)));
        case 0xA:
        case 0x11: return dp(i, i, dm, in) + put("0");
        case 0xB:  // reflect diffuse at stage 2: stage 3's product early for the normal
            s = dp(i, i, dm, in) + dp(i, i + 1, (k.dot_mapping >> 8) & 0xF, (k.input_texture >> 20) & 0xF);
            return s + put(sample3(F("float3(dp%d, dp%d, dp%dn)", i - 1, i, i)));
        case 0xC:  // reflect specular at stage 3: eye from the three w
            s = dp(i, i, dm, in);
            s += F("float3 nrm%d = float3(dp%d, dp%d, dp%d), eye%d = float3(tc%d.w, tc%d.w, tc%d.w);\n", i, i - 2, i - 1, i, i, i - 2,
                   i - 1, i);
            s += F("float3 refl%d = 2 * nrm%d * dot(nrm%d, eye%d) / dot(nrm%d, nrm%d) - eye%d;\n", i, i, i, i, i, i, i);
            return s + put(sample3(F("refl%d", i)));
        case 0xD:
        case 0xE: return dp(i, i, dm, in) + put(sample3(F("float3(dp%d, dp%d, dp%d)", i - 2, i - 1, i)));
        case 0xF: return put(sample2(F("t%u.ar", in)));
        case 0x10: return put(sample2(F("t%u.gb", in)));
        default: return put("0");
    }
}

const char* kPsSbyte = R"(
// unorm to byte b, less lo under 128 or hi from 128, over d
float3 sbyte(float3 u, float lo, float hi, float d) { float3 b = u * 255; return (b - (b >= 128 ? hi : lo)) / d; }
)";
const char* kPsTail = R"(float4 spreadtap(Texture2D t, SamplerState s, float2 uv, float4 w, float m) {
    float4 c = 0;
    int n = (int)w.z;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float4 v = t.SampleLevel(s, uv + w.xy * (float2(x, y) - (n - 1) * 0.5), 0);
            c += float4(saturate(v.rgb * m), v.a);
        }
    return c / (n * n);
}
// falling water (flow.x the time, d3d12.cpp): its texture running along u (water_flowing's streaks lie along u, and
// its sheets fall along it: down is the falling u), swaying a little across
float2 flowuv(float2 u) {
    if (flow.x > 0) u += float2(flow.x * flow.y, sin(u.x * 9 + flow.x * 2.3) * 0.015);
    return u;
}
void alphatest(inout float a) {
    int f = (int)misc.y;
    // multisampled (jitter.z, d3d12.cpp kA2cKey): past the reference by under a pixel's change, partly covered; the
    // alpha out is the coverage (alpha to coverage), a cut out's edge smooth
    if (jitter.z > 0 && (f == 4 || f == 6)) {
        float d = a * 255 - misc.x;
        float cv = saturate(d / max(fwidth(d), 1e-3) + 0.5);
        if (cv <= 0) discard;
        a = cv;
        return;
    }
    int v = (int)round(a * 255), r = (int)misc.x;
    bool ok = f == 0 ? false : f == 1 ? v < r : f == 2 ? v == r : f == 3 ? v <= r : f == 4 ? v > r : f == 5 ? v != r : f == 6 ? v >= r : true;
    if (!ok) discard;
}
)";

std::string PsHelpers() { return kPsSbyte + Hooks().Shadow() + kPsTail; }

// fixed function texture stage arguments, D3DTA
std::string FfArg(uint32_t a, int stage, bool alpha) {
    std::string v;
    switch (a & 0xF) {
        case 0: v = "v0"; break;
        case 1: v = "cur"; break;
        case 2: v = F("t%d", stage); break;
        case 3: v = "tfactor"; break;
        case 4: v = "v1"; break;
        case 5: v = "tmp"; break;
        default: v = "cur"; break;
    }
    if (a & 0x20) v = "(" + v + ").aaaa";
    if (a & 0x10) v = "(1 - " + v + ")";
    return "(" + v + ")" + (alpha ? ".a" : ".rgb");
}

// D3DTOP
std::string FfOp(uint32_t op, const std::string& a1, const std::string& a2, const std::string& a0, int stage, bool alpha) {
    switch (op) {
        case 2: return a1;
        case 3: return a2;
        case 4: return a1 + " * " + a2;
        case 5: return a1 + " * " + a2 + " * 2";
        case 6: return a1 + " * " + a2 + " * 4";
        case 7: return a1 + " + " + a2;
        case 8: return a1 + " + " + a2 + " - 0.5";
        case 9: return "(" + a1 + " + " + a2 + " - 0.5) * 2";
        case 10: return a1 + " - " + a2;
        case 11: return a1 + " + " + a2 + " * (1 - " + a1 + ")";
        case 12: return "lerp(" + a2 + ", " + a1 + ", v0.a)";
        case 13: return "lerp(" + a2 + ", " + a1 + ", cur.a)";
        case 14: return F("lerp(%s, %s, t%d.a)", a2.c_str(), a1.c_str(), stage);
        case 15: return "lerp(" + a2 + ", " + a1 + ", tfactor.a)";
        case 16: return F("%s + %s * (1 - t%d.a)", a1.c_str(), a2.c_str(), stage);
        case 18: return alpha ? a1 : a1 + " + " + a2 + " * " + a1.substr(0, a1.size() - 4) + ".a";
        case 19: return alpha ? a1 : a1 + " * " + a2 + " + " + a1.substr(0, a1.size() - 4) + ".a";
        case 20: return alpha ? a1 : a1 + " + " + a2 + " * (1 - " + a1.substr(0, a1.size() - 4) + ".a)";
        case 21: return alpha ? a1 : "(1 - " + a1 + ") * " + a2 + " + " + a1.substr(0, a1.size() - 4) + ".a";
        case 22: return F("saturate(dot((%s - 0.5) * 2, (%s - 0.5) * 2))", alpha ? ("float3(" + a1 + ",0,0)").c_str() : a1.c_str(),
                          alpha ? ("float3(" + a2 + ",0,0)").c_str() : a2.c_str());
        case 23: return a0 + " + " + a1 + " * " + a2;
        case 24: return "lerp(" + a2 + ", " + a1 + ", " + a0 + ")";
        default: return alpha ? "cur.a" : "cur.rgb";
    }
}

}  // namespace

bool PixelKey::operator==(const PixelKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }

std::string ShaderHooks::Constants() const { return "    float4 ext;\n"; }

std::string ShaderHooks::Shadow() const {
    return R"(// the sampler compares, a linear filter blends the four results like the console does
float4 shadow(Texture2D<float> t, SamplerComparisonState s, float2 uv, float z, int fmt, float2 px, float2 size) {
    float mx = fmt == 2 ? 65535.0 : 16777215.0;
    return t.SampleCmpLevelZero(s, uv, clamp(z, 0, mx) / mx).xxxx;
}
)";
}

std::string ShaderHooks::Output() const { return "outc.rgb = saturate(outc.rgb);\n"; }

uint64_t Hash(const void* p, size_t n, uint64_t h) {
    // 16 bytes a step through a 64 x 64 -> 128 bit multiply: it runs over textures every frame and keys every draw
    constexpr uint64_t k0 = 0x9E3779B97F4A7C15ull, k1 = 0xE7037ED1A0B428DBull;
    auto mix = [](uint64_t a, uint64_t b) {
        unsigned __int128 r = static_cast<unsigned __int128>(a) * b;
        return uint64_t(r) ^ uint64_t(r >> 64);
    };
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t seed = h ^ k0;
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        uint64_t x, y;
        std::memcpy(&x, b + i, 8);
        std::memcpy(&y, b + i + 8, 8);
        seed = mix(x ^ k1, y ^ seed);
    }
    if (size_t r = n - i) {
        uint64_t x = 0, y = 0;
        if (r >= 8) {
            std::memcpy(&x, b + i, 8);
            std::memcpy(&y, b + n - 8, 8);
        } else {
            std::memcpy(&x, b + i, r);
        }
        seed = mix(x ^ k1, y ^ seed ^ r);
    }
    return mix(seed ^ k0, uint64_t(n) ^ k1);
}

uint64_t HashBulk(const void* p, size_t n, uint64_t h) {
    // four lanes of Hash's step side by side (64 bytes a round, the multiplies independent): for bulk bytes hashed
    // every frame, whose values are never kept past the run
    constexpr uint64_t k0 = 0x9E3779B97F4A7C15ull, k1 = 0xE7037ED1A0B428DBull;
    auto mix = [](uint64_t a, uint64_t b) {
        unsigned __int128 r = static_cast<unsigned __int128>(a) * b;
        return uint64_t(r) ^ uint64_t(r >> 64);
    };
    if (n < 64) return Hash(p, n, h);
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t s[4] = {h ^ k0, h ^ k1, ~h ^ k0, ~h ^ k1};
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        uint64_t w[8];
        std::memcpy(w, b + i, 64);
        for (int l = 0; l < 4; l++) s[l] = mix(w[l * 2] ^ k1, w[l * 2 + 1] ^ s[l]);
    }
    uint64_t r = mix(s[0] ^ s[2], s[1] ^ s[3] ^ k0);
    return i < n ? Hash(b + i, n - i, r ^ n) : mix(r ^ k1, uint64_t(n));
}

PixelKey MakePixelKey(const State& s, const TexInfo tex[4]) {
    PixelKey k;
    std::memset(&k, 0, sizeof(k));
    for (int i = 0; i < 4; i++) {
        k.tex[i] = tex[i];
        if (s.tss[size_t(i)][kTssAlphaKill] == 4) k.alphakill |= 1u << i;  // D3DTALPHAKILL_ENABLE
    }
    k.flat = s.simple[kMShadeMode >> 2] == 0x1D00;  // D3DSHADE_FLAT
    k.two_sided = s.rs[kRsTwoSidedLighting] != 0;
    if (s.ps) {
        const uint32_t* d = s.ps->def;
        k.programmable = 1;
        // D3DPIXELSHADERDEF dwords: alpha inputs 0, final 8 and 9, alpha outputs 26, rgb inputs 34, compare mode 42, rgb outputs 45, count 53, dot mapping 55, input texture 56
        k.count = d[53];
        for (int i = 0; i < 8; i++) {
            k.a_in[i] = d[i];
            k.a_out[i] = d[26 + i];
            k.rgb_in[i] = d[34 + i];
            k.rgb_out[i] = d[45 + i];
        }
        k.final_abcd = d[8];
        k.final_efg = d[9];
        // the stage modes follow the bound textures, as D3D's lazy shader stage setup does
        uint32_t modes = s.rs[kRsPsTextureModes];
        for (int i = 0; i < 4; i++) {
            uint32_t m = (modes >> (i * 5)) & 0x1F;
            const TexInfo& t = tex[i];
            if (!t.kind && m != 4 && m != 5 && m != 0xA && m != 0x11) m = 0;
            else if (m >= 1 && m <= 3) m = t.kind == kTexCube ? 3 : (t.kind == kTex3D || t.shadow) ? 2 : 1;
            else if (m == 0xD || m == 0xE) m = t.kind == kTexCube ? 0xE : 0xD;
            k.tex_modes |= m << (i * 5);
        }
        k.dot_mapping = d[55];
        k.input_texture = d[56];
        k.compare_mode = d[42];
        return k;
    }
    // the console's stages divide by q; a vertex program's q is its own
    bool program = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    for (int i = 0; i < 4; i++) {
        const auto& t = s.tss[size_t(i)];
        uint32_t v[9] = {t[kTssColorOp], t[kTssColorArg1], t[kTssColorArg2], t[kTssColorArg0], t[kTssAlphaOp],
                         t[kTssAlphaArg1],  t[kTssAlphaArg2], t[kTssAlphaArg0], t[kTssResultArg]};
        std::memcpy(k.ff[i], v, sizeof(v));
        if (program || (t[kTssTextureTransformFlags] & 0x100)) k.proj |= 1u << i;  // D3DTTFF_PROJECTED
        if (t[kTssColorOp] <= 1) break;
    }
    return k;
}

// One instruction of a vertex program, decoded: its operands and where its MAC and ILU results go
struct VsOperand {
    uint32_t kind = 0;  // 1 temporary (reg 12 oPos), 2 input, 3 constant
    uint32_t reg = 0;
    bool rel = false;   // constant indexed by a0
    uint32_t sw[4] = {};
    bool neg = false;
};
struct VsInst {
    uint32_t mac = 0, ilu = 0;
    VsOperand op[3];
    std::string a, b, c;
    uint32_t mac_reg = 0, mac_mask = 0;  // temporary written by the MAC (mask 0 none)
    uint32_t ilu_reg = 0, ilu_mask = 0;
    uint32_t out = 0, out_mask = 0;      // output register written (kOut index, mask 0 none)
    bool out_ilu = false;
};

VsOperand Operand(const Tok& k, int which) {
    VsOperand o;
    uint32_t mux;
    if (which == 0) {
        o.sw[0] = k.f(1, 6, 2), o.sw[1] = k.f(1, 4, 2), o.sw[2] = k.f(1, 2, 2), o.sw[3] = k.f(1, 0, 2);
        o.reg = k.f(2, 28, 4), mux = k.f(2, 26, 2);
    } else if (which == 1) {
        o.sw[0] = k.f(2, 23, 2), o.sw[1] = k.f(2, 21, 2), o.sw[2] = k.f(2, 19, 2), o.sw[3] = k.f(2, 17, 2);
        o.reg = k.f(2, 13, 4), mux = k.f(2, 11, 2);
    } else {
        o.sw[0] = k.f(2, 8, 2), o.sw[1] = k.f(2, 6, 2), o.sw[2] = k.f(2, 4, 2), o.sw[3] = k.f(2, 2, 2);
        o.reg = k.f(2, 0, 2) << 2 | k.f(3, 30, 2), mux = k.f(3, 28, 2);
        uint32_t ilu = k.f(1, 25, 3);
        if (ilu >= 2 && ilu <= 6) o.sw[1] = o.sw[2] = o.sw[3] = o.sw[0];
    }
    o.neg = (which == 0 ? k.f(1, 8, 1) : which == 1 ? k.f(2, 25, 1) : k.f(2, 10, 1)) != 0;
    o.kind = mux == 1 ? 1 : mux == 2 ? 2 : 3;
    if (o.kind == 2) o.reg = k.f(1, 9, 4);
    if (o.kind == 3) o.reg = k.f(1, 13, 8), o.rel = k.f(3, 1, 1) != 0;
    return o;
}

std::vector<VsInst> DecodeProgram(const std::vector<uint32_t>& prog, uint16_t regs) {
    std::vector<VsInst> list;
    for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
        Tok k{&prog[n]};
        VsInst v;
        v.mac = k.f(1, 21, 4), v.ilu = k.f(1, 25, 3);
        if (v.mac || v.ilu) {
            for (int i = 0; i < 3; i++) v.op[i] = Operand(k, i);
            v.a = Src(k, 0, regs), v.b = Src(k, 1, regs), v.c = Src(k, 2, regs);
            uint32_t r = k.f(3, 20, 4), omask = k.f(3, 12, 4);
            if (v.mac && v.mac != 13) {
                uint32_t m = k.f(3, 24, 4);
                if (v.ilu && r == 1) m = 0;
                if (m && r <= 12) v.mac_reg = r, v.mac_mask = m;
            }
            if (v.ilu) {
                uint32_t m = k.f(3, 16, 4), ir = v.mac ? 1 : r;
                if (m && ir <= 12) v.ilu_reg = ir, v.ilu_mask = m;
            }
            bool use_ilu = k.f(3, 2, 1) == 1;
            if (omask && k.f(3, 11, 1) && ((use_ilu && v.ilu) || (!use_ilu && v.mac && v.mac != 13))) {
                uint32_t o = k.f(3, 3, 8) & 0xF;
                if (kOut[o][0]) v.out = o, v.out_mask = omask, v.out_ilu = use_ilu;
            }
            list.push_back(std::move(v));
        }
        if (k.f(3, 0, 1)) break;  // last instruction
    }
    return list;
}

// soft: slt and sge become ramps over 0.16 around the edge (a light's cut across a low detail model, per pixel)
std::string EmitInst(const VsInst& v, bool soft = false) {
    std::string s = "    {\n";
    if (soft && v.mac == 11) s += "        float4 mr = saturate((" + v.b + " - " + v.a + ") / 0.16 + 0.5);\n";
    else if (soft && v.mac == 12) s += "        float4 mr = saturate((" + v.a + " - " + v.b + ") / 0.16 + 0.5);\n";
    else if (v.mac) s += "        precise float4 mr = " + MacExpr(v.mac, v.a, v.b, v.c) + ";\n";
    if (v.ilu) s += "        precise float4 ir = " + IluExpr(v.ilu, v.c) + ";\n";
    if (v.mac == 13) s += "        a0 = mac_arl(mr.x);\n";
    if (v.mac_mask) s += F("        %s.%s = mr.%s;\n", RegDest(v.mac_reg).c_str(), kMask[v.mac_mask], kMask[v.mac_mask]);
    if (v.ilu_mask) s += F("        %s.%s = ir.%s;\n", RegDest(v.ilu_reg).c_str(), kMask[v.ilu_mask], kMask[v.ilu_mask]);
    if (v.out_mask) {
        const char* msk = v.out == 5 ? kLanes[std::popcount(v.out_mask)] : kMask[v.out_mask];
        s += F("        %s.%s = %s.%s;\n", kOut[v.out], msk, v.out_ilu ? "ir" : "mr", msk);
    }
    return s + "    }\n";
}

// Per pixel lighting. A program lights its vertices with dot products of a normal and the lights' constants, then
// clamps, steps or raises the results before they sum into oD0: done at the vertices, that is the polygons' facets on
// low detail models. The normal as the program has it (a temporary or an input) goes to the pixel shader, which runs
// again the part of the program that makes oD0 from it and from constants alone.
struct PixelLighting {
    bool ok = false;
    int at = -1;            // the normal is final after this instruction (-1: an input, from the start)
    VsOperand normal;       // its register
    std::vector<bool> keep; // the instructions oD0 needs
    std::vector<bool> soft; // their comparisons of the normal's light (slt sge): ramps at each pixel
};

// which of a's lanes an op reads per result lane: tag of each result lane from its operands' lanes
enum : uint8_t { kUniform = 0, kNormal = 1, kOther = 2 };

PixelLighting AnalyzeLighting(const std::vector<VsInst>& list) {
    PixelLighting pl;
    const size_t n = list.size();
    // the lanes of an operand the MAC or the ILU reads (after its swizzle)
    auto lanes = [](const VsInst& v, int op, uint32_t result_lanes) {
        uint32_t m = 0;
        for (int l = 0; l < 4; l++)
            if ((result_lanes >> l) & 1) m |= 1u << v.op[op].sw[l];
        return m;
    };
    auto reads_mac = [&](const VsInst& v, int op) -> uint32_t {
        switch (v.mac) {
            case 0: return 0;
            case 5: return op < 2 ? lanes(v, op, 7) : 0;
            case 6: return op == 0 ? lanes(v, op, 7) : op == 1 ? lanes(v, op, 15) : 0;
            case 1: case 13: return op == 0 ? lanes(v, op, 15) : 0;
            case 3: return op == 1 ? 0 : lanes(v, op, 15);
            case 4: return lanes(v, op, 15);
            default: return op < 2 ? lanes(v, op, 15) : 0;  // mul dp4 dst min max slt sge
        }
    };
    auto reads_ilu = [&](const VsInst& v, int op) -> uint32_t {
        if (!v.ilu || op != 2) return 0;
        return lanes(v, op, v.ilu == 1 ? 15 : v.ilu == 7 ? 0xB : 1);
    };
    auto lane_mask = [](uint32_t mask) {  // kMask bits are wzyx from bit 0: to xyzw lanes from bit 0
        return ((mask >> 3) & 1) | ((mask >> 2) & 1) << 1 | ((mask >> 1) & 1) << 2 | (mask & 1) << 3;
    };
    // what oD0 needs, backwards: lanes of temporaries and of a0
    std::vector<bool> keep(n, false);
    uint32_t need[13] = {}, need_od0 = 15;
    bool need_a0 = false;
    for (size_t i = n; i-- > 0;) {
        const VsInst& v = list[i];
        bool used = false;
        uint32_t mr_used = 0, ir_used = 0;  // result lanes of mr and ir someone needs
        if (v.out_mask && v.out == 3) {
            uint32_t lanes = lane_mask(v.out_mask) & need_od0;
            if (lanes) used = true, (v.out_ilu ? ir_used : mr_used) |= lanes;
            need_od0 &= ~lane_mask(v.out_mask);
        }
        if (v.mac_mask) {
            uint32_t lanes = lane_mask(v.mac_mask) & need[v.mac_reg];
            if (lanes) used = true, mr_used |= lanes;
            need[v.mac_reg] &= ~lane_mask(v.mac_mask);
        }
        if (v.ilu_mask) {
            uint32_t lanes = lane_mask(v.ilu_mask) & need[v.ilu_reg];
            if (lanes) used = true, ir_used |= lanes;
            need[v.ilu_reg] &= ~lane_mask(v.ilu_mask);
        }
        if (v.mac == 13 && need_a0) used = true, mr_used |= 1, need_a0 = false;
        if (!used) continue;
        keep[i] = true;
        for (int op = 0; op < 3; op++) {
            const VsOperand& o = v.op[op];
            uint32_t r = (mr_used ? reads_mac(v, op) : 0) | (ir_used ? reads_ilu(v, op) : 0);
            if (!r) continue;
            if (o.kind == 1) need[o.reg] |= r;
            if (o.kind == 3 && o.rel) need_a0 = true;
        }
    }
    // the normal: the first lit dot product oD0 needs, a register against a constant
    int first = -1;
    for (size_t i = 0; i < n && first < 0; i++) {
        const VsInst& v = list[i];
        if (!keep[i] || v.mac != 5) continue;
        for (int op = 0; op < 2; op++) {
            const VsOperand &x = v.op[op], &l = v.op[1 - op];
            if (l.kind == 3 && !l.rel && (x.kind == 1 || x.kind == 2) && !(x.kind == 1 && x.reg == 12)) {
                pl.normal = x;
                first = int(i);
                break;
            }
        }
    }
    if (first < 0) return pl;
    if (pl.normal.kind == 1)
        for (int i = first - 1; i >= 0; i--)
            if ((list[i].mac_mask && list[i].mac_reg == pl.normal.reg) || (list[i].ilu_mask && list[i].ilu_reg == pl.normal.reg)) {
                pl.at = i;
                break;
            }
    // tags forward: oD0 must come from constants and the normal alone
    uint8_t tag[13][4] = {}, a0 = kUniform, od0[4] = {kUniform, kUniform, kUniform, kUniform};
    std::vector<bool> soft(n, false);
    auto set_normal = [&] {
        for (int l = 0; l < 3; l++) tag[pl.normal.reg][l] = kNormal;
    };
    if (pl.normal.kind == 1 && pl.at < 0) set_normal();
    for (size_t i = 0; i < n; i++) {
        const VsInst& v = list[i];
        auto op_tag = [&](int op, int lane) -> uint8_t {
            const VsOperand& o = v.op[op];
            uint32_t src = o.sw[lane];
            if (o.kind == 1) return tag[o.reg][src];
            if (o.kind == 2) return pl.normal.kind == 2 && o.reg == pl.normal.reg && src < 3 ? kNormal : kOther;
            return o.rel ? a0 : kUniform;
        };
        auto max_over = [&](int op, uint32_t lanes) {
            uint8_t t = kUniform;
            for (int l = 0; l < 4; l++)
                if ((lanes >> l) & 1) t = std::max(t, op_tag(op, l));
            return t;
        };
        uint8_t mr[4] = {}, ir[4] = {};
        for (int l = 0; l < 4; l++) {
            switch (v.mac) {
                case 5: mr[l] = std::max(max_over(0, 7), max_over(1, 7)); break;
                case 6: mr[l] = std::max(max_over(0, 7), max_over(1, 15)); break;
                case 7: case 8: mr[l] = std::max(max_over(0, 15), max_over(1, 15)); break;
                case 1: case 13: mr[l] = op_tag(0, l); break;
                case 3: mr[l] = std::max(op_tag(0, l), op_tag(2, l)); break;
                case 4: mr[l] = std::max({op_tag(0, l), op_tag(1, l), op_tag(2, l)}); break;
                default: mr[l] = std::max(op_tag(0, l), op_tag(1, l)); break;
            }
            ir[l] = v.ilu == 1 ? op_tag(2, l) : v.ilu == 7 ? max_over(2, 0xB) : op_tag(2, 0);
        }
        if (v.mac == 13) a0 = mr[0];
        soft[i] = (v.mac == 11 || v.mac == 12) && mr[0] == kNormal && mr[1] <= kNormal && mr[2] <= kNormal && mr[3] <= kNormal;
        for (int l = 0; l < 4; l++) {
            if ((lane_mask(v.mac_mask) >> l) & 1) tag[v.mac_reg][l] = mr[l];
            if ((lane_mask(v.ilu_mask) >> l) & 1) tag[v.ilu_reg][l] = ir[l];
            if (v.out == 3 && ((lane_mask(v.out_mask) >> l) & 1)) od0[l] = v.out_ilu ? ir[l] : mr[l];
        }
        if (int(i) == pl.at) set_normal();
    }
    bool lit = false;
    for (int l = 0; l < 4; l++) {
        if (od0[l] == kOther) return pl;
        lit |= od0[l] == kNormal;
    }
    pl.ok = lit;
    pl.keep = std::move(keep);
    pl.soft = std::move(soft);
    return pl;
}

std::string NormalName(const VsOperand& o) { return o.kind == 2 ? F("i.v%u", o.reg) : RegDest(o.reg); }

struct LightingCache {
    std::mutex lock;
    KeyMap<uint64_t, uint32_t> ids;      // program hash and inputs: lighting id, 0 none
    KeyMap<uint32_t, std::string> code;  // lighting id: PixelLit
};
LightingCache& Lighting() {
    static LightingCache c;
    return c;
}

std::string VertexProgramHlsl(const std::vector<uint32_t>& prog, uint16_t regs, uint64_t decode) {
    std::string s = Common();
    s += kVsHelpers;
    s += VsInputs(regs, decode);
    s += kVsPrologue;
    s += "    float4 pN = 0, pP = 0;\n";
    std::vector<VsInst> list = DecodeProgram(prog, regs);
    PixelLighting pl = AnalyzeLighting(list);
    if (pl.ok && pl.at < 0) s += "    pN = " + NormalName(pl.normal) + ";\n";
    // the position: what the first instruction to write oPos takes a dot product of with the constants
    bool have_pos = false;
    for (size_t i = 0; i < list.size(); i++) {
        const VsInst& v = list[i];
        if (pl.ok && !have_pos && v.out == 0 && v.out_mask && (v.mac == 6 || v.mac == 7)) {
            const VsOperand& o = v.op[0].kind == 3 ? v.op[1] : v.op[0];
            if (o.kind == 1 || o.kind == 2) {
                s += "    pP = float4(" + NormalName(o) + ".xyz, 2);\n";
                have_pos = true;
            }
        }
        s += EmitInst(v);
        if (pl.ok && int(i) == pl.at) s += "    pN = " + NormalName(pl.normal) + ";\n";
    }
    std::string tail = kVsEpilogue;
    tail.replace(tail.rfind("    return o;"), 0, "    o.pn = pN;\n    o.pv = pP;\n");
    return s + tail;
}

uint32_t PixelLightingId(const std::vector<uint32_t>& prog, uint16_t regs) {
    LightingCache& lc = Lighting();
    uint64_t key = Hash(prog.data(), prog.size() * 4, regs);
    std::lock_guard l(lc.lock);
    auto it = lc.ids.find(key);
    if (it != lc.ids.end()) return it->second;
    std::vector<VsInst> list = DecodeProgram(prog, regs);
    PixelLighting pl = AnalyzeLighting(list);
    uint32_t id = 0;
    if (pl.ok) {
        // the program again, the instructions oD0 needs, the normal the interpolated one
        std::string f = "struct LitIn { float4 v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15; };\n"
                        "float4 PixelLit(float3 N, float3 P) {\n    LitIn i = (LitIn)0;\n";
        f += kVsPrologue;
        if (pl.normal.kind == 2) f += F("    i.v%u.xyz = N;\n", pl.normal.reg);
        else if (pl.at < 0) f += "    " + NormalName(pl.normal) + ".xyz = N;\n";
        for (size_t i = 0; i < list.size(); i++) {
            if (pl.keep[i]) f += EmitInst(list[i], pl.soft[i]);
            if (int(i) == pl.at) f += "    " + NormalName(pl.normal) + ".xyz = N;\n";
        }
        f += "    return oD0;\n}\n";
        id = uint32_t(Hash(f.data(), f.size())) | 1;
        lc.code[id] = f;
        LOGR("shaders: per pixel lighting for a program of %zu instructions, normal %s after %d", list.size(), NormalName(pl.normal).c_str(), pl.at);
        if (std::getenv("XBE_LIT_DUMP")) LOGR("shaders: lighting %08X: %s", id, f.c_str());
    }
    lc.ids[key] = id;
    return id;
}

std::string LightingReport(const std::vector<uint32_t>& prog, uint16_t regs) {
    std::vector<VsInst> list = DecodeProgram(prog, regs);
    PixelLighting pl = AnalyzeLighting(list);
    if (pl.ok) return F("per pixel (normal %s)", NormalName(pl.normal).c_str());
    // what oD0 comes from: lighting ops on varying values, or a colour passed through
    bool lighting = false, writes = false;
    for (const VsInst& v : list) {
        writes |= v.out == 3 && v.out_mask;
        lighting |= v.mac == 5 || v.mac == 6 || v.ilu == 4 || v.ilu == 7;
    }
    if (!writes) return "no colour";
    std::string what;
    for (const VsInst& v : list)
        if (v.out == 3 && v.out_mask) what += (what.empty() ? "" : " ") + MacExpr(v.mac, v.a, v.b, v.c).substr(0, 60);
    return std::string(lighting ? "PER VERTEX? " : "unlit: ") + what;
}

struct CpuProgram {
    std::vector<VsInst> list;
    uint16_t regs = 0;
};

std::shared_ptr<const CpuProgram> DecodeCpuProgram(const std::vector<uint32_t>& prog, uint16_t regs) {
    auto p = std::make_shared<CpuProgram>();
    p->list = DecodeProgram(prog, regs);
    p->regs = regs;
    return p;
}

// as the HLSL the programs become (MacExpr, IluExpr, EmitInst): the MAC and the ILU read the registers as they were
// before the instruction, then write
void RunCpuProgram(const CpuProgram& p, const float in[16][4], const std::array<std::array<float, 4>, 192>& c, float opos[4]) {
    float R[13][4] = {};  // R12 is oPos
    int a0 = 0;
    auto read = [&](const VsOperand& o, float v[4]) {
        const float* base;
        static const float ones[4] = {1, 1, 1, 1}, unit[4] = {0, 0, 0, 1};
        if (o.kind == 1) base = R[std::min<uint32_t>(o.reg, 12)];
        else if (o.kind == 2) base = (p.regs >> o.reg) & 1 ? in[o.reg] : o.reg == 3 ? ones : unit;
        else base = c[size_t(std::clamp(int(o.reg) + (o.rel ? a0 : 0), 0, 191))].data();
        for (int l = 0; l < 4; l++) v[l] = o.neg ? -base[o.sw[l]] : base[o.sw[l]];
    };
    auto mul = [](float a, float b) { return a == 0 || b == 0 ? 0.0f : a * b; };
    for (const VsInst& v : p.list) {
        float a[4], b[4], cc[4], mr[4] = {}, ir[4] = {};
        read(v.op[0], a), read(v.op[1], b), read(v.op[2], cc);
        switch (v.mac) {
            case 1: case 13: std::copy(a, a + 4, mr); break;
            case 2: for (int l = 0; l < 4; l++) mr[l] = mul(a[l], b[l]); break;
            case 3: for (int l = 0; l < 4; l++) mr[l] = a[l] + cc[l]; break;
            case 4: for (int l = 0; l < 4; l++) mr[l] = mul(a[l], b[l]) + cc[l]; break;
            case 5: mr[0] = mr[1] = mr[2] = mr[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; break;
            case 6: mr[0] = mr[1] = mr[2] = mr[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3]; break;
            case 7: mr[0] = mr[1] = mr[2] = mr[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; break;
            case 8: mr[0] = 1, mr[1] = a[1] * b[1], mr[2] = a[2], mr[3] = b[3]; break;
            case 9: for (int l = 0; l < 4; l++) mr[l] = std::min(a[l], b[l]); break;
            case 10: for (int l = 0; l < 4; l++) mr[l] = std::max(a[l], b[l]); break;
            case 11: for (int l = 0; l < 4; l++) mr[l] = a[l] < b[l] ? 1.0f : 0.0f; break;
            case 12: for (int l = 0; l < 4; l++) mr[l] = a[l] >= b[l] ? 1.0f : 0.0f; break;
            default: break;
        }
        float x = cc[0];
        switch (v.ilu) {
            case 1: std::copy(cc, cc + 4, ir); break;
            case 2: ir[0] = ir[1] = ir[2] = ir[3] = 1.0f / x; break;
            case 3: {
                float r = 1.0f / x, m = std::clamp(std::fabs(r), 5.42101e-20f, 1.84467e+19f);
                ir[0] = ir[1] = ir[2] = ir[3] = r < 0 ? -m : m;
                break;
            }
            case 4: ir[0] = ir[1] = ir[2] = ir[3] = x == 0 ? INFINITY : 1.0f / std::sqrt(std::fabs(x)); break;
            case 5: ir[0] = std::exp2(std::floor(x)), ir[1] = x - std::floor(x), ir[2] = std::exp2(x), ir[3] = 1; break;
            case 6: {
                float m = std::fabs(x), l2 = std::log2(m), ex = std::floor(l2);
                if (m == 0) ir[0] = ir[2] = -INFINITY, ir[1] = ir[3] = 1;
                else ir[0] = ex, ir[1] = m / std::exp2(ex), ir[2] = l2, ir[3] = 1;
                break;
            }
            case 7: {
                const float cap = 127.99609375f;
                float d = std::max(cc[0], 0.0f);
                ir[0] = 1, ir[1] = d, ir[2] = d > 0 ? std::exp2(std::clamp(cc[3], -cap, cap) * std::log2(std::max(cc[1], 0.0f))) : 0, ir[3] = 1;
                break;
            }
            default: break;
        }
        if (v.mac == 13) a0 = int(std::floor(mr[0] + 1e-3f));
        for (int l = 0; l < 4; l++) {
            if ((v.mac_mask >> (3 - l)) & 1) R[v.mac_reg][l] = mr[l];
            if ((v.ilu_mask >> (3 - l)) & 1) R[v.ilu_reg][l] = ir[l];
            if (v.out == 0 && ((v.out_mask >> (3 - l)) & 1)) R[12][l] = v.out_ilu ? ir[l] : mr[l];
        }
    }
    std::copy(R[12], R[12] + 4, opos);
}

std::string WeaponCullGsHlsl() { return Common() + kWeaponCullGs; }

// Points (d3d12.cpp kPointSpriteKey): each a square of its size (VOut.psize, console pixels) about it, facing the
// screen; point sprites (pts1.w) take texture coordinates 0 to 1 across it, top left first, on every stage. D3D12
// draws a point as a single pixel: rain, drips and sparks drawn as point sprites were all but invisible
const char* kPointSpriteGs = R"(
[maxvertexcount(4)]
void gs(point VOut v[1], inout TriangleStream<VOut> s) {
    VOut o = v[0];
    float2 h = max(v[0].psize, 1) / surf.xy * v[0].pos.w;
    const float2 corner[4] = {float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, -1)};
    [unroll] for (int k = 0; k < 4; k++) {
        o.pos = v[0].pos + float4(corner[k] * h, 0, 0);
        if (pts1.w > 0) {
            float4 t = float4(corner[k].x * 0.5 + 0.5, 0.5 - corner[k].y * 0.5, 0, 1);
            o.tc0 = t, o.tc1 = t, o.tc2 = t, o.tc3 = t;
        }
        s.Append(o);
    }
}
)";
std::string PointSpriteGsHlsl() { return Common() + kPointSpriteGs; }
std::string DrawHlslHead() { return Common(); }

TessInfo TessRows(const std::vector<uint32_t>& prog, uint16_t regs) {
    TessInfo info;
    bool rows[4] = {}, offset = false;
    for (const VsInst& v : DecodeProgram(prog, regs)) {
        // oPos.xyz = oPos.xyz * (1 / oPos.w) + c[offset]: the perspective divide and the viewport's offset
        if (v.mac == 4 && ((v.out == 0 && !v.out_ilu && (v.out_mask & 0xE) == 0xE) || (v.mac_reg == 12 && (v.mac_mask & 0xE) == 0xE)) &&
            v.op[0].kind == 1 && v.op[0].reg == 12 && v.op[2].kind == 3 && !v.op[2].rel)
            info.offset = uint16_t(v.op[2].reg), offset = true;
        for (const VsOperand& o : v.op)
            if (o.kind == 3 && o.rel) info.skinned = true;
        if ((v.mac != 6 && v.mac != 7) || v.out != 0 || v.out_ilu || !v.out_mask) continue;
        for (int lane = 0; lane < 4; lane++) {
            if (v.out_mask != (8u >> lane) || rows[lane]) continue;
            for (int op = 0; op < 2; op++) {
                const VsOperand& c = v.op[op];
                if (c.kind == 3 && !c.rel) info.rows[lane] = uint16_t(c.reg), rows[lane] = true;
            }
        }
    }
    info.ok = rows[0] && rows[1] && rows[2] && rows[3] && offset;
    return info;
}

// PN triangles (Vlachos et al.): each triangle a cubic patch through its corners, its edges bending to the corners'
// normals, in the program's own space (VOut.pv, w 2: the skinned world position, VOut.pn its normal). A new point's
// screen position: the program's rows (tess) give its homogeneous h, the screen position is linear in h
// (q = a h + b h.w per axis, the viewport's scale and offset); a and b solved from two corners, checked on the third,
// else the patch stays flat. Factors by the edges' length on the screen.
const char* kTessHlsl = R"(
struct PnPatch {
    float edge[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
    float3 b210 : POSITION3; float3 b120 : POSITION4; float3 b021 : POSITION5; float3 b012 : POSITION6;
    float3 b102 : POSITION7; float3 b201 : POSITION8; float3 b111 : POSITION9;
    float usable : TEXCOORD10;
    float reach : TEXCOORD11;  // the farthest the surface may bulge from the flat triangle
};
float4 Homog(float3 P) {
    float4 X = float4(P, 1);
    return float4(dot(X, c[(int)tess.x]), dot(X, c[(int)tess.y]), dot(X, c[(int)tess.z]), dot(X, c[(int)tess.w]));
}
float3 Proj(float3 P, float3 N, float3 Q) { return Q - dot(Q - P, N) * N; }
// the screen position as the program and kVsEpilogue make it: h.xyz / h.w plus the offset, the viewport's pixels to
// clip space
float4 Screen(float3 P) {
    float4 h = Homog(P);
    float w = magclamp(h.w);
    float3 p = h.xyz * (1.0 / w) + c[(int)tess2.x].xyz;
    p.x = p.x * layout2d.x + layout2d.y;
    p.y = p.y * layout2y.x + layout2y.y;
    float4 q = float4(p.x * 2 / surf.x - 1, 1 - p.y * 2 / surf.y, p.z * surf.z, 1);
    q.xy += jitter.xy;
    q.xyz *= w;
    q.w = w;
    return q;
}
PnPatch pnconst(InputPatch<VOut, 3> v) {
    PnPatch o;
    float3 P[3], N[3];
    float2 s[3];
    bool ok = true;
    [unroll] for (int k = 0; k < 3; k++) {
        P[k] = v[k].pv.xyz;
        N[k] = normalize(v[k].pn.xyz + 1e-9);
        ok = ok && v[k].pv.w == 2 && v[k].pos.w > 1e-3;
        s[k] = v[k].pos.xy / max(v[k].pos.w, 1e-3) * surf.xy * 0.5;
    }
    // the program's position as we make it again: where it is not the one it drew (another program's form), flat
    [unroll] for (int j = 0; j < 3; j++) {
        float4 q = Screen(P[j]);
        ok = ok && all(abs(q.xy / q.w - v[j].pos.xy / v[j].pos.w) < 0.002) && dot(v[j].pn.xyz, v[j].pn.xyz) > 1e-6;
    }
    // a degenerate triangle (a strip's joint), or one whose corners' normals turn from its face: flat
    float3 fn = cross(P[1] - P[0], P[2] - P[0]);
    float area = length(fn);
    float longest = max(max(length(P[1] - P[0]), length(P[2] - P[1])), length(P[0] - P[2]));
    ok = ok && area > longest * longest * 0.01;
    fn /= max(area, 1e-12);
    [unroll] for (int q = 0; q < 3; q++) ok = ok && abs(dot(N[q], fn)) > 0.2;
    o.usable = ok ? 1 : 0;
    // (by its width as well: a long thin triangle, a gun barrel's along its length, bulged by its length grew the
    // barrel into a cigar)
    o.reach = min(longest * 0.12, area / max(longest, 1e-12) * 0.35);
    // the factors: an edge's length in console pixels over 5, up to 7; flat where it cannot be placed
    // (a NaN length 1, as Direct3D's clamp makes it: SPIR-V's leaves it NaN, and the tessellator hung on it)
    float l0 = length(s[1] - s[2]) / 5, l1 = length(s[2] - s[0]) / 5, l2 = length(s[0] - s[1]) / 5;
    float e0 = ok && l0 > 1 ? min(l0, 7) : 1;
    float e1 = ok && l1 > 1 ? min(l1, 7) : 1;
    float e2 = ok && l2 > 1 ? min(l2, 7) : 1;
    o.edge[0] = e0, o.edge[1] = e1, o.edge[2] = e2;
    o.inside = (e0 + e1 + e2) / 3;
    // the control points, each third of an edge pulled onto its corner's tangent plane
    o.b210 = Proj(P[0], N[0], (2 * P[0] + P[1]) / 3);
    o.b120 = Proj(P[1], N[1], (2 * P[1] + P[0]) / 3);
    o.b021 = Proj(P[1], N[1], (2 * P[1] + P[2]) / 3);
    o.b012 = Proj(P[2], N[2], (2 * P[2] + P[1]) / 3);
    o.b102 = Proj(P[2], N[2], (2 * P[2] + P[0]) / 3);
    o.b201 = Proj(P[0], N[0], (2 * P[0] + P[2]) / 3);
    float3 E = (o.b210 + o.b120 + o.b021 + o.b012 + o.b102 + o.b201) / 6, V = (P[0] + P[1] + P[2]) / 3;
    o.b111 = E + (E - V) / 2;
    return o;
}
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_ccw")]
[outputcontrolpoints(3)]
[patchconstantfunc("pnconst")]
VOut hs(InputPatch<VOut, 3> v, uint id : SV_OutputControlPointID) { return v[id]; }

[domain("tri")]
VOut ds(PnPatch pt, float3 uvw : SV_DomainLocation, const OutputPatch<VOut, 3> v) {
    float u = uvw.x, w0 = uvw.y, w1 = uvw.z;  // weights of corners 0, 1, 2
    VOut o;
#define LERP3(f) (v[0].f * u + v[1].f * w0 + v[2].f * w1)
    o.pos = LERP3(pos); o.d0 = LERP3(d0); o.d1 = LERP3(d1); o.b0 = LERP3(b0); o.b1 = LERP3(b1);
    o.tc0 = LERP3(tc0); o.tc1 = LERP3(tc1); o.tc2 = LERP3(tc2); o.tc3 = LERP3(tc3);
    o.fogamt = LERP3(fogamt); o.cpos = LERP3(cpos); o.pn = LERP3(pn); o.pv = LERP3(pv); o.psize = v[0].psize;
    if (pt.usable > 0) {
        float u2 = u * u, v2 = w0 * w0, w2 = w1 * w1;
        float3 P = v[0].pv.xyz * u2 * u + v[1].pv.xyz * v2 * w0 + v[2].pv.xyz * w2 * w1 +
                   pt.b210 * 3 * u2 * w0 + pt.b120 * 3 * u * v2 + pt.b201 * 3 * u2 * w1 + pt.b021 * 3 * v2 * w1 +
                   pt.b102 * 3 * u * w2 + pt.b012 * 3 * w0 * w2 + pt.b111 * 6 * u * w0 * w1;
        float3 L = v[0].pv.xyz * u + v[1].pv.xyz * w0 + v[2].pv.xyz * w1;
        float3 d = P - L;
        float dl = length(d);
        if (dl > pt.reach) P = L + d * (pt.reach / dl);
        float4 q = Screen(P);
        o.pos = q;
        o.cpos = q;
        o.cpos.xy -= jitter.xy * q.w;
        o.pv = float4(P, 2);
    }
    return o;
}
)";

std::string TessHlsl() { return Common() + kVsHelpers + kTessHlsl; }

ProgramPositionInfo ProgramPosition(const std::vector<uint32_t>& prog, uint16_t regs) {
    ProgramPositionInfo info;
    bool rows[4] = {};
    int input = -1;
    for (const VsInst& v : DecodeProgram(prog, regs)) {
        if (v.mac != 7) continue;  // dp4
        // to oPos itself, or to its temporary (r12)
        uint32_t mask = v.out == 0 && !v.out_ilu && v.out_mask ? v.out_mask : v.mac_reg == 12 ? v.mac_mask : 0;
        for (int lane = 0; lane < 4; lane++) {
            if (mask != (8u >> lane)) continue;
            for (int op = 0; op < 2; op++) {
                const VsOperand &c = v.op[op], &in = v.op[1 - op];
                if (c.kind != 3 || c.rel || in.kind != 2) continue;
                if (input >= 0 && input != int(in.reg)) return info;
                input = int(in.reg);
                info.rows[lane] = uint16_t(c.reg), rows[lane] = true;
            }
        }
    }
    info.ok = rows[0] && rows[1] && rows[2] && rows[3];
    info.input = uint16_t(std::max(input, 0));
    return info;
}

ProgramLightInfo ProgramLight(const std::vector<uint32_t>& prog, uint16_t regs) {
    static std::mutex lock;
    static KeyMap<uint64_t, ProgramLightInfo> known;
    uint64_t key = Hash(prog.data(), prog.size() * 4, regs ^ 0x5157);
    std::lock_guard l(lock);
    auto it = known.find(key);
    if (it != known.end()) return it->second;
    ProgramLightInfo info;
    std::vector<VsInst> list = DecodeProgram(prog, regs);
    PixelLighting pl = AnalyzeLighting(list);
    if (pl.ok) {
        bool rows[4] = {};
        int dir = -1, color = -1, ambient = -1;
        int from[13];  // a temporary moved from a constant, -1 none
        std::fill(std::begin(from), std::end(from), -1);
        auto constant = [&](const VsOperand& o) { return o.kind == 3 && !o.rel ? int(o.reg) : o.kind == 1 && o.reg < 13 ? from[o.reg] : -1; };
        for (size_t i = 0; i < list.size(); i++) {
            const VsInst& v = list[i];
            // the position's rows: dp4 straight to one lane of oPos
            if (v.out == 0 && v.mac == 7 && !v.out_ilu)
                for (int lane = 0; lane < 4; lane++)
                    if (v.out_mask == (8u >> lane))
                        for (int op = 0; op < 2; op++)
                            if (v.op[op].kind == 3 && !v.op[op].rel) info.pos[lane] = uint16_t(v.op[op].reg), rows[lane] = true;
            if (!pl.keep[i]) continue;
            if (dir < 0 && v.mac == 5)
                for (int op = 0; op < 2; op++) {
                    const VsOperand &n = v.op[op], &c = v.op[1 - op];
                    if (c.kind == 3 && !c.rel && n.kind == pl.normal.kind && n.reg == pl.normal.reg) dir = int(c.reg);
                }
            if (v.mac == 1 && v.mac_mask == 15 && v.mac_reg < 13) from[v.mac_reg] = constant(v.op[0]);
            else if (v.mac_mask && v.mac_reg < 13) from[v.mac_reg] = -1;
            // the colour: a light's term times the light's colour plus the ambient, to oD0
            if (v.mac == 4 && v.out == 3 && !v.out_ilu) {
                int a = constant(v.op[0]), b = constant(v.op[1]);
                color = a >= 0 ? a : b;
                ambient = constant(v.op[2]);
            }
        }
        if (rows[0] && rows[1] && rows[2] && rows[3] && dir >= 0 && color >= 0 && ambient >= 0) {
            info.ok = true;
            info.dir = uint16_t(dir), info.color = uint16_t(color), info.ambient = uint16_t(ambient);
            LOGR("shaders: a program lit by c%d (colour c%d, ambient c%d), placed by c%d..c%d", dir - 96, color - 96, ambient - 96,
                 info.pos[0] - 96, info.pos[3] - 96);
        }
    }
    known[key] = info;
    return info;
}

std::string ShadowVertexHlsl(std::string s) {
    auto sub = [&](const char* a, const std::string& b) {
        size_t p = s.find(a);
        if (p == std::string::npos) return false;
        s.replace(p, std::strlen(a), b);
        return true;
    };
    sub("cbuffer VC : register(b0) { float4 c[192]; };",
        "cbuffer VC : register(b0) { float4 c[192]; row_major float4x4 shc[4]; float4 sht[4]; float4 shv; };");
    std::string param, arg;
    if (sub("VOut main(VIn i) {", "VOut vmain(VIn i) {")) param = "VIn i", arg = "i";
    else if (sub("VOut main(VRaw ri) {", "VOut vmain(VRaw ri) {")) param = "VRaw ri", arg = "ri";
    else return {};
    s += "struct SOut { float4 pos : SV_Position; float4 tc0 : TEXCOORD0; float4 clip : SV_ClipDistance0; float back : SV_ClipDistance1; };\n";
    s += "SOut main(" + param + ", uint sid : SV_InstanceID) {\n    VOut o = vmain(" + arg + ");\n";
    s += R"(    uint k = sid + (uint)shv.z;
    // the camera's clip position is linear in its view space: x = view x * P00, y = view y * P11, w = view depth
    float3 v = float3(o.cpos.x * shv.x, o.cpos.y * shv.y, o.cpos.w);
    float4 l = mul(float4(v, 1), shc[k]);
    SOut r;
    r.pos = float4(l.xy * sht[k].xy + sht[k].zw, l.z, 1);
    r.clip = float4(1 + l.x, 1 - l.x, 1 + l.y, 1 - l.y);
    // what lies further towards the light than the cascade reaches (the sky's dome, the distant backdrop) is left
    // out; what lies just past it is flattened onto its near end
    r.back = l.z + 0.15;
    r.tc0 = o.tc0;
    return r;
}
)";
    return s;
}

// The top view variant (d3d12.cpp WaterInteraction): the draw's own position, from the camera's clip position back to
// its view space, then onto the wave field's square seen from above; its world height out for the pixel shader
std::string TopVertexHlsl(std::string s) {
    auto sub = [&](const char* a, const std::string& b) {
        size_t p = s.find(a);
        if (p == std::string::npos) return false;
        s.replace(p, std::strlen(a), b);
        return true;
    };
    sub("cbuffer VC : register(b0) { float4 c[192]; };",
        "cbuffer VC : register(b0) { float4 c[192]; };\ncbuffer TD : register(b2) { row_major float4x4 tdm; float4 tdy; float4 tdv; float4 tdp; };");
    std::string param, arg;
    if (sub("VOut main(VIn i) {", "VOut vmain(VIn i) {")) param = "VIn i", arg = "i";
    else if (sub("VOut main(VRaw ri) {", "VOut vmain(VRaw ri) {")) param = "VRaw ri", arg = "ri";
    else return {};
    s += "struct TOut { float4 pos : SV_Position; float wy : TEXCOORD0; float2 uv : TEXCOORD1; };\n";
    s += "TOut main(" + param + ") {\n    VOut o = vmain(" + arg + ");\n";
    s += R"(    float3 v = float3(o.cpos.x * tdv.x, o.cpos.y * tdv.y, o.cpos.w);
    float4 l = mul(float4(v, 1), tdm);
    TOut r;
    r.pos = float4(l.xy, 0.5, 1);
    r.wy = dot(float4(v, 1), tdy);
    r.uv = o.tc0.xy;
    return r;
}
)";
    return s;
}

// The guard band's variant (d3d12.cpp DrawGuard): the draw's own clip position with the view widened by
// 1 / kGuardScale, its depth as the screen's; the shadow caster's outputs, so its alpha test pixel shader serves
std::string GuardVertexHlsl(std::string s) {
    auto sub = [&](const char* a, const std::string& b) {
        size_t p = s.find(a);
        if (p == std::string::npos) return false;
        s.replace(p, std::strlen(a), b);
        return true;
    };
    std::string param, arg;
    if (sub("VOut main(VIn i) {", "VOut vmain(VIn i) {")) param = "VIn i", arg = "i";
    else if (sub("VOut main(VRaw ri) {", "VOut vmain(VRaw ri) {")) param = "VRaw ri", arg = "ri";
    else return {};
    s += "struct SOut { float4 pos : SV_Position; float4 tc0 : TEXCOORD0; float4 clip : SV_ClipDistance0; float back : SV_ClipDistance1; };\n";
    s += "SOut main(" + param + ") {\n    VOut o = vmain(" + arg + ");\n";
    s += F("    SOut r;\n    r.pos = float4(o.cpos.xy * %.6f, o.cpos.zw);\n", kGuardScale);
    s += "    r.clip = 1;\n    r.back = 1;\n    r.tc0 = o.tc0;\n    return r;\n}\n";
    return s;
}

std::string MotionVertexHlsl(const std::string& s, unsigned wvp_at) {
    size_t at = s.find("VOut main(VIn i) {");
    std::string param = "VIn i", arg = "i";
    if (at == std::string::npos) at = s.find("VOut main(VRaw ri) {"), param = "VRaw ri", arg = "ri";
    if (at == std::string::npos) return {};
    std::string fn = s.substr(at);
    // the frame before's: its constants (c[ to cp[), its fixed function transform (wvp to wvpp)
    auto word = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; };
    std::string prev;
    prev.reserve(fn.size() + 64);
    for (size_t k = 0; k < fn.size();) {
        bool start = k == 0 || !word(fn[k - 1]);
        if (start && fn.compare(k, 2, "c[") == 0) {
            prev += "cp[", k += 2;
            continue;
        }
        if (start && fn.compare(k, 3, "wvp") == 0 && (k + 3 >= fn.size() || !word(fn[k + 3]))) {
            prev += "wvpp", k += 3;
            continue;
        }
        prev += fn[k++];
    }
    prev.replace(prev.find("VOut main("), 10, "VOut mprev(");
    std::string out = s.substr(0, at);
    out += F("cbuffer VCP : register(b2) { float4 cp[192]; };\ncbuffer DCP : register(b3) { float4 dcp_pad[%u]; float4x4 wvpp; };\n",
             wvp_at);
    out += fn + prev;
    out += "struct MOut { float4 pos : SV_Position; float4 cur : TEXCOORD0; float4 prev : TEXCOORD1; };\n";
    out += "MOut motion(" + param + ") {\n    VOut a = main(" + arg + ");\n    VOut b = mprev(" + arg +
           ");\n    MOut o;\n    o.pos = a.pos;\n    o.cur = a.pos;\n    o.prev = b.pos;\n    return o;\n}\n";
    return out;
}

std::string ShadowAlphaHlsl(bool proj) {
    std::string s = R"(cbuffer DC : register(b1) { float4 surf; float4 fogp; float4 fogcolor; float4 misc; };
Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);
struct SOut { float4 pos : SV_Position; float4 tc0 : TEXCOORD0; float4 clip : SV_ClipDistance0; float back : SV_ClipDistance1; };
void main(SOut i) {
)";
    s += proj ? "    float2 uv = i.tc0.xy / (i.tc0.w == 0 ? 1 : i.tc0.w);\n" : "    float2 uv = i.tc0.xy;\n";
    s += R"(    float a = tex0.Sample(smp0, uv).a;
    int f = (int)misc.y;
    int v = (int)round(a * 255), r = (int)misc.x;
    bool ok = f == 0 ? false : f == 1 ? v < r : f == 2 ? v == r : f == 3 ? v <= r : f == 4 ? v > r : f == 5 ? v != r : f == 6 ? v >= r : true;
    if (!ok || a == 0) discard;
}
)";
    return s;
}

std::string PixelLightingCode(uint32_t id) {
    LightingCache& lc = Lighting();
    std::lock_guard l(lc.lock);
    auto it = lc.code.find(id);
    return it == lc.code.end() ? std::string() : it->second;
}

FixedKey MakeFixedKey(const State& s) {
    FixedKey f;
    if (s.rs[kRsLighting]) {
        f.light = 1 | (s.rs[kRsSpecularEnable] ? 2 : 0) | (s.rs[kRsLocalViewer] ? 4 : 0) | (s.rs[kRsNormalizeNormals] ? 8 : 0);
        if (s.rs[kRsColorVertex])
            f.light |= (s.rs[kRsDiffuseMaterialSource] & 3) << 8 | (s.rs[kRsSpecularMaterialSource] & 3) << 10 |
                       (s.rs[kRsAmbientMaterialSource] & 3) << 12 | (s.rs[kRsEmissiveMaterialSource] & 3) << 14;
        for (int i = 0; i < 8; i++)
            if ((s.light_enable >> i) & 1) f.types |= (s.lights[size_t(i)][0] & 3) << (i * 2);
    }
    for (int t = 0; t < 4; t++) {
        uint32_t tci = s.tss[size_t(t)][kTssTexCoordIndex], ttf = s.tss[size_t(t)][kTssTextureTransformFlags];
        uint32_t set = tci & 3, size = 2;
        if (s.vs && (s.vs_handle & 1)) {
            uint32_t ty = s.vs->inputs[9 + set].type;
            size = ty == 0x16 ? 3 : std::min<uint32_t>(ty >> 4, 4);  // 0x16 NORMPACKED3
            if (ty >> 4 > 4) size = 3;
            if (!size) size = 2;
        } else {
            uint32_t code = (s.vs_handle >> (16 + set * 2)) & 3;
            size = code == 0 ? 2 : code == 1 ? 3 : code == 2 ? 4 : 1;
        }
        f.tex[t] = set | ((tci >> 16) & 7) << 4 | (ttf & 7) << 8 | (ttf & 0x100 ? 0x800 : 0) | size << 12;
    }
    return f;
}

// D3D's fixed function vertex pipeline: transform, lighting in view space, texture coordinate generation and transforms
// The fixed function lighting from the view space normal vn and position vp into oD0 and oD1. pixel: for a pixel
// shader, which has no vertex colours: empty when a material colour comes from them.
std::string FixedLightingBody(const FixedKey& f, uint16_t regs, bool pixel) {
    std::string s;
    // D3DMCS: 1 the vertex diffuse, 2 its specular, else the material
    bool from_vertex = false;
    auto src = [&](int field, const char* mat) {
        uint32_t m = (f.light >> (8 + field * 2)) & 3;
        if (m == 1 && ((regs >> 3) & 1)) return from_vertex = true, std::string("i.v3");
        if (m == 2 && ((regs >> 4) & 1)) return from_vertex = true, std::string("i.v4");
        return std::string(mat);
    };
    s += "    float4 md = " + src(0, "mat[0]") + ", ms = " + src(1, "mat[2]") + ", ma = " + src(2, "mat[1]") +
         ", me = " + src(3, "mat[3]") + ";\n";
    if (pixel && from_vertex) return {};
    s += "    float3 la = 0, ld = 0, ls = 0;\n";
    s += (f.light & 4) ? "    float3 eye = -normalize(vp);\n" : "    float3 eye = float3(0, 0, 1);\n";
    for (int n = 0; n < 8; n++) {
        uint32_t type = (f.types >> (n * 2)) & 3;  // D3DLIGHTTYPE: 1 point, 2 spot, 3 directional
        if (!type) continue;
        int b = n * 7;
        s += "    {\n        float3 l;\n        float att = 1;\n";
        if (type == 3) {
            s += F("        l = -normalize(lt[%d].xyz);\n", b + 4);
        } else {
            s += F("        float3 d = lt[%d].xyz - vp;\n        float dist = length(d);\n        l = d / max(dist, 1e-6);\n", b + 3);
            s += F("        att = dist > lt[%d].w ? 0 : 1 / max(lt[%d].x + lt[%d].y * dist + lt[%d].z * dist * dist, 1e-6);\n", b + 3, b + 5,
                   b + 5, b + 5);
            if (type == 2) {
                s += F("        float rho = dot(-l, normalize(lt[%d].xyz));\n", b + 4);
                s += F("        att *= rho > lt[%d].x ? 1 : rho <= lt[%d].y ? 0 : pow(saturate((rho - lt[%d].y) / (lt[%d].x - lt[%d].y)), lt[%d].w);\n",
                       b + 6, b + 6, b + 6, b + 6, b + 6, b + 4);
            }
        }
        s += F("        la += att * lt[%d].rgb;\n", b + 2);
        s += "        float nl = dot(vn, l);\n";
        s += F("        ld += att * lt[%d].rgb * max(nl, 0);\n", b);
        if (f.light & 2) s += F("        if (nl > 0) ls += att * lt[%d].rgb * pow(max(dot(vn, normalize(l + eye)), 0), mat[4].x);\n", b + 1);
        s += "    }\n";
    }
    s += "    oD0 = float4(me.rgb + ma.rgb * (amb.rgb + la) + md.rgb * ld, md.a);\n";
    s += (f.light & 2) ? "    oD1 = float4(ms.rgb * ls, ms.a);\n" : "    oD1 = float4(0,0,0,0);\n";
    return s;
}

uint32_t FixedLightingId(const FixedKey& f, uint16_t regs) {
    if (!(f.light & 1) || !f.types) return 0;
    std::string body = FixedLightingBody(f, regs, true);
    if (body.empty()) return 0;
    std::string fn = "float4 PixelLit(float3 vn, float3 vp) {\n    float4 oD0 = 0, oD1 = 0;\n" + body + "    return oD0;\n}\n";
    uint32_t id = uint32_t(Hash(fn.data(), fn.size())) | 1;
    LightingCache& lc = Lighting();
    std::lock_guard l(lc.lock);
    lc.code[id] = fn;
    return id;
}

std::string FixedVertexHlsl(uint16_t regs, bool pretransformed, uint64_t decode, const FixedKey& f) {
    std::string s = Common();
    s += kVsHelpers;
    s += VsInputs(regs, decode);
    s += kVsPrologue;
    auto v = [&](int r, const char* def) { return (regs >> r) & 1 ? F("i.v%d", r) : std::string(def); };
    bool lit = !pretransformed && (f.light & 1);
    if (pretransformed) {
        s += "    float4 rhw = " + v(0, "float4(0,0,0,1)") + ";\n";
        s += "    float w = rhw.w == 0 ? 1 : 1 / rhw.w;\n";
        s += "    oPos = float4(rhw.xy, rhw.z / surf.z, w);\n";
    } else {
        s += "    float4 ip = float4(" + v(0, "float4(0,0,0,1)") + ".xyz, 1);\n";
        s += "    float4 cp = mul(ip, wvp);\n";
        s += "    float3 vp = mul(ip, wv).xyz;\n";
        s += "    float3 vn = mul(float4(" + v(2, "float4(0,0,0,0)") + ".xyz, 0), wvn).xyz;\n";
        if (f.light & 8) s += "    vn = dot(vn, vn) > 0 ? normalize(vn) : vn;\n";
    }
    if (lit) {
        s += FixedLightingBody(f, regs, false);
    } else {
        s += "    oD0 = " + v(3, "float4(1,1,1,1)") + ";\n";
        s += "    oD1 = " + v(4, "float4(0,0,0,0)") + ";\n";
    }
    s += "    oB0 = oD0;\n    oB1 = oD1;\n";
    for (int t = 0; t < 4; t++) {
        uint32_t u = f.tex[t], set = u & 3, gen = (u >> 4) & 7, count = (u >> 8) & 7, size = (u >> 12) & 7;
        std::string in = v(9 + int(set), "float4(0,0,0,1)");
        if (pretransformed || (!gen && !count)) {
            s += F("    oT%d = %s;\n", t, in.c_str());
            continue;
        }
        // generated coordinates are view space; a transform takes the set with 1 after its last element
        std::string c = gen == 1 ? "float4(vn, 1)" : gen == 2 ? "float4(vp, 1)" : gen == 3 ? "float4(reflect(normalize(vp), vn), 1)"
                        : size == 1 ? F("float4(%s.x, 1, 0, 0)", in.c_str()) : size == 2 ? F("float4(%s.xy, 1, 0)", in.c_str())
                        : size == 3 ? F("float4(%s.xyz, 1)", in.c_str()) : in;
        if (!count) {
            s += F("    oT%d = %s;\n", t, c.c_str());
            continue;
        }
        s += F("    {\n        float4 tc = mul(%s, tm[%d]);\n", c.c_str(), t);
        if (u & 0x800) s += F("        oT%d = float4(tc.xy, tc.z, tc[%u]);\n", t, std::min<uint32_t>(count, 4) - 1);
        else s += F("        oT%d = float4(tc.xy, %s, 1);\n", t, count >= 3 ? "tc.z" : "0");
        s += "    }\n";
    }
    s += "    oFog = " + v(5, "float4(0,0,0,0)") + ";\n";
    if (pretransformed) return s + kVsEpilogue;
    s += R"(
    VOut o = (VOut)0;
    // like the pretransformed ones, target sized passes end half a console pixel short of the far edges
    float2 sp = (float2(cp.x, -cp.y) / cp.w + 1) * 0.5 * surf.xy;
    if (abs(sp.x - (surf.x - 0.5)) < 1e-3) cp.x = cp.w;
    if (abs(sp.y - (surf.y - 0.5)) < 1e-3) cp.y = -cp.w;
    o.pos = cp;
    o.cpos = cp;
    // 2D placed by the game, x * layout2d.x + layout2d.y and y * layout2y.x + layout2y.y in console pixels, here in
    // clip space (y down the screen); after the quad variant took its positions, which its texture coordinates follow
    o.pos.x = (cp.x + cp.w) * layout2d.x - cp.w + 2 * layout2d.y / surf.x * cp.w;
    o.pos.y = cp.w - (cp.w - cp.y) * layout2y.x - 2 * layout2y.y / surf.y * cp.w;
    if (jitter.w > 0) o.pos.z = o.pos.w * jitter.w;  // a sky piece's own depth (d3d12.cpp sky_phase_)
    o.d0 = saturate(oD0);
    o.d1 = saturate(oD1);
    o.b0 = o.d0;
    o.b1 = o.d1;
    // a point's size: the render state's, scaled by the distance when the scale is on (D3D: the viewport's height
    // times the size over the square root of A + B d + C d squared), within its min and max (a max of 0: none)
    {
        float pd = length(vp);
        float ps = pts0.w > 0 ? surf.y * pts0.x * rsqrt(max(pts1.x + pts1.y * pd + pts1.z * pd * pd, 1e-6)) : pts0.x;
        o.psize = max(pts0.z > 0 ? min(ps, pts0.z) : ps, pts0.y);
    }
    o.tc0 = oT0;
    // level 1's far water (d3d12.cpp): its texture laid flat across the river at the near water's density (-jitter.z)
    if (jitter.z < 0) o.tc0 = float4(ip.xz * -jitter.z, 0, 1);
    o.tc1 = oT1;
    o.tc2 = oT2;
    o.tc3 = oT3;
    o.fogamt = fogfactor(abs(cp.z));
    o.pn = float4(vn, 0);
    o.pv = float4(vp, 1);
    return o;
}
)";
    return s;
}

// Remastered materials (kTexMaterial): the stage's colour comes from the remaster pack, its normal map at t4 and its
// material map at t5 (roughness, height, cavity, specular). The view space position is the clip position through the
// camera's view window; the tangent frame comes from its screen derivatives and the texture coordinates'.
std::string MaterialSetup(int i, const std::string& uv) {
    std::string s;
    s += F("float2 mUv = %s;\n", uv.c_str());
    s += "float3 mP = float3(i.cpos.xy / i.cpos.w * mtl[0].xy, 1) * i.cpos.w;\n";
    s += "float3 mDx = ddx(mP), mDy = ddy(mP);\n";
    s += "float3 mN0 = normalize(cross(mDy, mDx) + 1e-12);\n";
    s += "if (dot(mN0, mP) > 0) mN0 = -mN0;\n";
    // the smooth normal, from the vertices: the fixed function's in view space; a program's in its own space, carried
    // to view space by the map from its position's screen derivatives to the view position's (exact for a rigid model)
    s += "float3 mNs = mN0;\n";
    s += "{\n"
         "    float3 pa = ddx(i.pv.xyz), pb = ddy(i.pv.xyz), pc = cross(pb, pa), vc = cross(mDy, mDx);\n"
         "    pc *= rsqrt(max(length(pc), 1e-30)); vc *= rsqrt(max(length(vc), 1e-30));\n"
         "    float det = dot(pa, cross(pb, pc));\n"
         "    float3 n = i.pn.xyz * rsqrt(max(dot(i.pn.xyz, i.pn.xyz), 1e-30));\n"
         "    float3 k = float3(dot(cross(pb, pc), n), dot(cross(pc, pa), n), dot(cross(pa, pb), n)) / (abs(det) > 1e-30 ? det : 1);\n"
         "    float3 m = i.pv.w == 1 ? n : k.x * mDx + k.y * mDy + k.z * vc;\n"
         "    bool use = dot(i.pn.xyz, i.pn.xyz) > 1e-12 && dot(m, m) > 1e-30 && (i.pv.w == 1 || (i.pv.w == 2 && abs(det) > 1e-30));\n"
         "    if (use) mNs = normalize(m) * (dot(m, mN0) < 0 ? -1 : 1);\n"
         "}\n";
    // water lying flat (mrefl.z < 0: Water; trail[1].xyz the world's up in view space): the up, not its facets'
    // normals (a stream bed of flat shaded triangles shone facet by facet)
    s += "if (mrefl.z < 0 && dot(trail[1].xyz, trail[1].xyz) > 0.5) {\n"
         "    float3 up = normalize(trail[1].xyz);\n"
         "    float f = smoothstep(0.6, 0.85, dot(mN0, up));\n"
         "    mNs = normalize(lerp(mNs, up, f)); mN0 = normalize(lerp(mN0, up, f));\n"
         "}\n";
    s += "float2 mU1 = ddx(mUv), mU2 = ddy(mUv);\n";
    s += "float3 mDyP = cross(mDy, mN0), mDxP = cross(mN0, mDx);\n";
    s += "float3 mT = mDyP * mU1.x + mDxP * mU2.x, mB = mDyP * mU1.y + mDxP * mU2.y;\n";
    s += "float mInv = rsqrt(max(max(dot(mT, mT), dot(mB, mB)), 1e-30));\n";
    s += "mT *= mInv; mB *= mInv;\n";
    s += "float3 mV = normalize(-mP);\n";
    // parallax occlusion: the view walks into the height field, nearer surfaces only
    // (from 14 m on the walk moves nothing: skipped). Its taps (and the soft height below) through s5, trilinear:
    // anisotropy over tens of taps a pixel costs much and shows nothing in where the walk stops
    s += "float mFade = saturate(1 - mP.z * 0.0007);\n";
    s += F("if (mtl[2].y > 0 && mtl[0].w > 0 && mFade > 0) {\n"
           "    float3 vt = float3(dot(mV, mT), dot(mV, mB), dot(mV, mN0));\n"
           "    float fade = mFade;\n"
           "    float steps = lerp(10, 40, 1 - saturate(vt.z));\n"
           "    float uvw = sqrt((dot(mU1, mU1) + dot(mU2, mU2)) / max(dot(mDx, mDx) + dot(mDy, mDy), 1e-12));\n"
           "    float depth = min(mtl[2].y * mtl[3].z * uvw, mtl[2].y);\n"
           "    float2 walk = -vt.xy / max(vt.z, 0.45) * depth * fade * lerp(0.35, 1, saturate(vt.z * 1.6));\n"
           // (the whole walk's length on the screen in pixels: under half a pixel it moves nothing, and a step a pixel
           // and a third or so is all a shorter one needs; the steps by the angle stand where it is long)
           "    float jd = mU1.x * mU2.y - mU2.x * mU1.y;\n"
           "    float wpx = length(float2(mU2.y * walk.x - mU2.x * walk.y, mU1.x * walk.y - mU1.y * walk.x)) / max(abs(jd), 1e-20);\n"
           "    steps = wpx < 0.5 ? 0 : min(steps, max(6, ceil(wpx * 0.75)));\n"
           "    float2 duv = walk / max(steps, 1);\n"
           "    if (steps > 0) {\n"
           "    float2 hU1 = mU1 * 3, hU2 = mU2 * 3;\n"
           "    float layer = 1 / steps, d = 0, pd = 0;\n"
           "    float2 cur = mUv, prev = mUv;\n"
           "    float hd = 1 - mmat.SampleGrad(mtri, cur, hU1, hU2).g, phd = hd;\n"
           "    [loop] for (int k = 0; k < 40; k++) {\n"
           "        if (d >= hd || k >= steps) break;\n"
           "        prev = cur; phd = hd; pd = d;\n"
           "        cur += duv; d += layer;\n"
           "        hd = 1 - mmat.SampleGrad(mtri, cur, hU1, hU2).g;\n"
           "    }\n"
           "    float a = hd - d, b = phd - pd;\n"
           "    mUv = lerp(cur, prev, saturate(a / (a - b + 1e-5)));\n"
           "    }\n"
           "}\n");
    s += F("float4 t%d = tex%d.SampleGrad(smp%d, mUv, mU1, mU2);\n", i, i, i);
    s += F("float3 mBase = t%d.rgb;\nfloat mAlpha = t%d.a;\n", i, i);
    s += "float4 mMat = mmat.SampleGrad(" + MatSampler(i) + ", mUv, mU1, mU2);\n";
    // the height and the texture's cover the blend follows (Height Blend): two levels softer, their shapes without the
    // fine grain (sharpened by the blend, the grain broke the edge of a blended layer into specks)
    s += "float mHs = mmat.SampleGrad(mtri, mUv, mU1 * 4, mU2 * 4).g;\n";
    s += F("float mAs = tex%d.SampleGrad(smp%d, mUv, mU1 * 4, mU2 * 4).a;\n", i, i);
    s += "float2 mNt = mnrm.SampleGrad(" + MatSampler(i) + ", mUv, mU1, mU2).rg * 2 - 1;\n";
    return s;
}

// after the stage's combining: the relief and the specular, by the light the original shading gave the surface. The
// material's detail normal map (mtl[2].w, an index into t6) tiles twice over the texture and fades out by 4 m.
std::string MaterialShade(int i) {
    return
        // a character close up (mtl[3].y < 0, the detail's index in mtl[2].w): skin's pores and cloth's weave, large
        // (tiled 3 times: the textures are of a low resolution) and faint, as a touch of light and shade across the surface and a soft sheen on the skin; no
        // coarse relief (the game paints their shading)
        "if (mtl[0].w == 0 && mtl[3].y < 0 && mtl[2].w >= 0) {\n"
        "    float fd = saturate(1.6 - mP.z / (200 * mtl[3].z)) * -mtl[3].y;\n"
        // (a uniform's weave not over the skin painted in it, the hands: their colour skin's, red over blue)
        "    if (mtl[2].w != 6) { float3 sb = mBase.rgb; fd *= 1 - smoothstep(0.08, 0.2, sb.r - sb.b) * smoothstep(0.0, 0.06, sb.g - sb.b) * smoothstep(0.2, 0.35, sb.r) * step(sb.g, sb.r); }\n"
        "    if (fd > 0) {\n"
        "        float2 dn = mdet.SampleGrad(" + MatSampler(i) + ", float3(frac(mUv * dtl.z), mtl[2].w), mU1 * dtl.z, mU2 * dtl.z).rg * 2 - 1;\n"
        "        float3 sT = normalize(mT - mNs * dot(mT, mNs) + 1e-12), sB = normalize(mB - mNs * dot(mB, mNs) + 1e-12);\n"
        "        float3 dN = normalize(mNs + (sT * dn.x + sB * dn.y) * 0.6);\n"
        "        float3 L = mtl[1].xyz;\n"
        "        float shade = 1 + (dot(dN, L) - dot(mNs, L)) * 0.35 * fd;\n"
        "        float3 H = normalize(L + mV);\n"
        "        float sheen = pow(saturate(dot(dN, H)), 24) * 0.06 * fd * mtl[2].z;\n"
        "        float lit = smoothstep(0.03, 0.4, dot(v0.rgb, 0.3333));\n"
        "        outc.rgb = outc.rgb * lerp(1, shade, lit) + sheen * lit;\n"
        "    }\n"
        "}\n"
        "if (mtl[0].w > 0) {\n"
        "    float3 nt = float3(mNt * float2(1, mtl[0].z) * mtl[2].x, 0);\n"
        "    if (mtl[2].w >= 0 && mtl[3].y > 0) {\n"
        "        float fd = saturate(1.6 - mP.z / (250 * mtl[3].z)) * mtl[3].y;\n"
        "        if (fd > 0) nt.xy += (mdet.SampleGrad(" + MatSampler(i) + ", float3(frac(mUv * 2), mtl[2].w), mU1 * 2, mU2 * 2).rg * 2 - 1)"
        " * float2(1, mtl[0].z) * fd;\n"
        "    }\n"
        "    if (mrefl.z < 0) {\n"
        "        float wt = -mrefl.z;\n"
        "        float2 q = mUv * 6;\n"
        "        float2 w1 = float2(sin(q.x * 3.1 + wt * 1.7 + sin(q.y * 1.3)), cos(q.y * 2.7 + wt * 1.3 + sin(q.x * 0.9)));\n"
        "        float2 w2 = float2(sin(q.y * 7.3 - wt * 2.3 + q.x * 2), cos(q.x * 6.1 - wt * 2.9 + q.y * 1.5));\n"
        "        nt.xy = nt.xy * 0.3 + (w1 * 0.18 + w2 * 0.08);\n"
        "    }\n"
        "    nt.xy = nt.xy / max(1, length(nt.xy));\n"
        "    nt.z = sqrt(saturate(1 - dot(nt.xy, nt.xy)));\n"
        "    float3 sT = normalize(mT - mNs * dot(mT, mNs) + 1e-12), sB = normalize(mB - mNs * dot(mB, mNs) + 1e-12);\n"
        "    float3 mN = normalize(sT * nt.x + sB * nt.y + mNs * nt.z);\n"
        // what crosses the water stirs it (Water Interaction): the wave field's slope at this place in the world,
        // fading out at the field's edges
        "    if (wave[2].w != 0) {\n"
        "        float4 vp = float4(mP, 1);\n"
        "        float2 wu = (float2(dot(vp, wave[0]), dot(vp, wave[1])) - wave[2].xy) * wave[2].z;\n"
        "        const float e = 1.0 / 512;\n"
        "        float hx = wavef.SampleLevel(wavelin, wu + float2(e, 0), 0).r - wavef.SampleLevel(wavelin, wu - float2(e, 0), 0).r;\n"
        "        float hz = wavef.SampleLevel(wavelin, wu + float2(0, e), 0).r - wavef.SampleLevel(wavelin, wu - float2(0, e), 0).r;\n"
        "        float2 m = abs(wu - 0.5);\n"
        "        float fade = saturate((0.5 - max(m.x, m.y)) * 12);\n"
        "        float2 g = float2(hx, hz) * wave[2].w * fade;\n"
        "        mN = normalize(mN - (wave[0].xyz * g.x + wave[1].xyz * g.y));\n"
        // the crests break white where the water is stirred hard (a wake, an oar's stroke, a fall), as lit as
        // the water around
        "        float hc = wavef.SampleLevel(wavelin, wu, 0).r;\n"
        "        float foam = saturate(abs(hc) * 0.3 - 0.1) * saturate(length(g) * 4) * fade;\n"
        "        outc.rgb = lerp(outc.rgb, outc.rgb * 2.4 + 0.04, foam * 0.75);\n"
        // XBE_WAVE_DEBUG: the field's height red up, blue down; the cover green
        "        if (wave[2].w < 0) { float4 f = wavef.SampleLevel(wavelin, wu, 0); outc.rgb = float3(saturate(f.r * 0.3), saturate(f.b / 175), saturate(-f.r * 0.3)) * fade + float3(0.4, 0, 0.4) * (1 - fade); }\n"
        "    }\n"
        // the footprints in the snow (Snow Tracks): pressed in (r over 0, its rim pushed up under 0), where their
        // height (g) is this surface's: the relief turned by their slope, the trodden snow a little darker
        "    if (trail[0].w != 0) {\n"
        "        float4 vp = float4(mP, 1);\n"
        "        float2 tu = (float2(dot(vp, wave[0]), dot(vp, wave[1])) - trail[0].xy) * trail[0].z;\n"
        "        float2 tm = abs(tu - 0.5);\n"
        "        float tf = saturate((0.5 - max(tm.x, tm.y)) * 16);\n"
        "        float4 t = trailf.SampleLevel(wavelin, tu, 0);\n"
        "        float near = saturate(2 - abs(dot(vp, trail[1]) - t.g) / 40) * tf;\n"
        "        if (near > 0 && t.r != 0) {\n"
        "            const float te = 1.0 / 2048;\n"
        "            float dx = trailf.SampleLevel(wavelin, tu + float2(te, 0), 0).r - trailf.SampleLevel(wavelin, tu - float2(te, 0), 0).r;\n"
        "            float dz = trailf.SampleLevel(wavelin, tu + float2(0, te), 0).r - trailf.SampleLevel(wavelin, tu - float2(0, te), 0).r;\n"
        "            mN = normalize(mN + (wave[0].xyz * dx + wave[1].xyz * dz) * abs(trail[0].w) * near);\n"
        // its walls lit or shaded by the light's side, the pressed snow darker, the rim a touch lighter
        "            float3 sl = (wave[0].xyz * dx + wave[1].xyz * dz) * near;\n"
        "            float side = clamp(1 + dot(sl, mtl[1].xyz) * 2.5, 0.55, 1.4);\n"
        "            outc.rgb *= side * (1 - (0.32 * saturate(t.r) - 0.1 * saturate(-t.r)) * near);\n"
        "        }\n"
        // XBE_SNOW_DEBUG: pressed red, raised blue, where the height agrees green
        "        if (trail[0].w < 0) outc.rgb = float3(saturate(t.r), near * 0.5, saturate(-t.r) * 2) + 0.02 * tf;\n"
        "    }\n"
        "    float3 L = mtl[1].xyz;\n"
        // the relief: the light's turn across it, never more than half again or half less (the light in the frame's own
        // view: a dark room no longer brightens as the camera turns, no gate on the game's light needed)
        "    float lf = (dot(mN, L) * 0.5 + 0.5) / max(dot(mNs, L) * 0.5 + 0.5, 0.25);\n"
        "    lf = clamp(lf, 0.6, 1.4);\n"
        // (the cavity's weight with the relief's, mtl[0].w: the snowy ground's faint, d3d12.cpp)
        "    float cav = lerp(1, mMat.b, 0.65 * min(mtl[0].w, 1));\n"
        // the highlight: broad and soft. Its normal is the relief two mips coarser, leaning on the smooth surface (a
        // pixel's normal map detail made it a scatter of sparkles); the gloss stays low and widens where that
        // normal still turns fast; the original light (v0, at the vertices) gates it through a gentle ramp
        "    float lit = smoothstep(0.02, 0.75, dot(v0.rgb, 0.3333));\n"
        "    float2 sNt = mnrm.SampleGrad(" + MatSampler(i) + ", mUv, mU1 * 4, mU2 * 4).rg * 2 - 1;\n"
        "    float3 snt = float3(sNt * float2(1, mtl[0].z) * mtl[2].x * 0.5, 0);\n"
        "    snt.xy = snt.xy / max(1, length(snt.xy));\n"
        "    snt.z = sqrt(saturate(1 - dot(snt.xy, snt.xy)));\n"
        "    float3 sN = normalize(sT * snt.x + sB * snt.y + mNs * snt.z);\n"
        "    float3 H = normalize(L + mV);\n"
        "    float gl = exp2(1 + 6 * (1 - mMat.r));\n"
        "    float3 dnx = ddx(sN), dny = ddy(sN);\n"
        "    float ra = 2 / (gl + 2) + min(12 * (dot(dnx, dnx) + dot(dny, dny)), 0.7);\n"
        "    gl = 2 / ra - 2;\n"
        // the sun's: none in its shadow (the last frame's mask, d3d12.cpp BindSunVisibility, asked for by a negative
        // strength)
        "    float sun = 1;\n"
        "    if (mtl[1].w < 0) {\n"
        // where this point was on the screen when the mask was made, moved by the camera since (read where the pixel
        // is now, a stale mask lit and put out the highlights about a shadow's edge as the view turned)
        "        float4 qa = mul(float4(mP, 1), sunprev), qb = mul(float4(mP, 1), sunnow);\n"
        "        float2 mv = (qa.xy / max(qa.w, 1e-3) - qb.xy / max(qb.w, 1e-3)) * float2(0.5, -0.5);\n"
        "        float2 ssz;\n"
        "        sunvis.GetDimensions(ssz.x, ssz.y);\n"
        "        sun = sunvis.Load(int3(clamp(i.pos.xy + mv * ssz, 0, ssz - 1), 0)).g;\n"
        "    }\n"
        "    float sp = min(mMat.a * pow(saturate(dot(sN, H)), gl) * (gl + 8) / 25.1, 0.6) * abs(mtl[1].w) * sun;\n"
        "    float3 shaded = outc.rgb * lerp(1, lf, mtl[0].w) * cav + sp * lit * saturate(dot(mNs, L) * 2 + 0.3);\n"
        "    outc.rgb = lerp(outc.rgb, shaded, saturate(fogin.a));\n"
        "    if (mtl[3].w > 0) {\n"
        // (the layer's cover as drawn, its texture's part taken softer: the vertices' fade times the soft cover; the
        // blend eased in over the texture's cover from 60% to 85%, not switched on at 95%: blotches with hard edges)
        "        float sa = saturate(outc.a / max(mAlpha, 0.004) * mAs);\n"
        "        float ha = saturate((sa * 2 - 1 + mHs - 0.5) / 0.9 + 0.5);\n"
        "        if (mtl[3].x == 4) outc.rgb = float3(1 - outc.a, abs(ha - outc.a) * 4 * saturate(mAs * 4 - 2.4), 0);\n"
        "        outc.a = lerp(outc.a, ha, mtl[3].w * saturate(fogin.a) * saturate(mAs * 4 - 2.4));\n"
        "    }\n"
        // the reflection buffer (d3d12.cpp's kSceneSsr): the normal (octahedral), the view depth, reflectivity and
        // roughness (whole and fraction), metal by its sign. Metal is its bare parts: grey and smooth
        "    {\n"
        "        float rough = saturate(mMat.r);\n"
        "        float3 bc = mBase.rgb;\n"
        "        float sat = (max(max(bc.r, bc.g), bc.b) - min(min(bc.r, bc.g), bc.b)) / max(max(max(bc.r, bc.g), bc.b), 1e-3);\n"
        "        float metal = mrefl.y * saturate(1 - sat * 5) * smoothstep(0.5, 0.32, rough);\n"
        "        float refl = mrefl.x * (1 - rough * 0.8) * mtl[0].w;\n"
        "        float3 n = mN / (abs(mN.x) + abs(mN.y) + abs(mN.z));\n"
        "        float2 oct = n.z >= 0 ? n.xy : (1 - abs(n.yx)) * (n.xy >= 0 ? 1 : -1);\n"
        "        float code = floor(saturate(refl) * 63) + rough * 0.95;\n"
        "        g_gbuf = float4(oct, mP.z, metal > 0.5 ? -code : code);\n"
        "    }\n"
        "    if (mtl[3].x == 1) outc.rgb = mN * 0.5 + 0.5;\n"
        "    if (mtl[3].x == 2) outc.rgb = lf * 0.5 * cav;\n"
        "    if (mtl[3].x == 3) outc.rgb = mNs * 0.5 + 0.5;\n"
        "}\n"
        // Detail Textures: the material's grain (a grey map, mean 0.5) in light and shade over the colour, tiled finer
        // than the texture's own pixels (a coarse grain about 1.3 detail pixels to one of the original's, a finer one 2.5
        // times smaller); its mips grey out to nothing far off and it fades with the distance, the finer first
        "if (dtl.x >= 0 && dtl.y > 0) {\n"
        // (none over skin painted in a uniform's texture, the soldiers' hands: it read as dirt)
        "    float3 sb = mBase.rgb;\n"
        "    float skin = dtl.y > 0.9 ? smoothstep(0.08, 0.2, sb.r - sb.b) * smoothstep(0.0, 0.06, sb.g - sb.b) * smoothstep(0.2, 0.35, sb.r) * step(sb.g, sb.r) : 0;\n"
        "    float dm = mP.z / (100 * mtl[3].z);\n"
        "    float rep = max(texsize[" + std::to_string(i) + "].x, texsize[" + std::to_string(i) + "].y) / dtl.w;\n"
        "    float f1 = saturate(1.4 - dm / 14), f2 = saturate(1.5 - dm / 4);\n"
        "    float g = 0;\n"
        "    if (f1 > 0) g += (mdet.SampleGrad(" + MatSampler(i) + ", float3(mUv * rep, dtl.x), mU1 * rep, mU2 * rep).r - 0.5) * 2 * f1;\n"
        "    if (f2 > 0) g += (mdet.SampleGrad(" + MatSampler(i) + ", float3(mUv * rep * 2.5, dtl.x), mU1 * rep * 2.5, mU2 * rep * 2.5).g - 0.5) * 1.2 * f2;\n"
        "    outc.rgb *= lerp(1, 1 + 0.3 * tanh(g * dtl.y * 1.2), fogin.a * (1 - skin));  // (soft, at most a third either way: a deep trough went black)\n"
        "}\n";
}

int MaterialStage(const PixelKey& k) {
    for (int i = 0; i < 4; i++)
        if (k.tex[i].kind == kTex2D && (k.tex[i].flags & kTexMaterial)) return i;
    return -1;
}

std::string PixelHlsl(const PixelKey& k) {
    std::string s = Common();
    s += PsHelpers();
    for (int i = 0; i < 4; i++) {
        const TexInfo& t = k.tex[i];
        const char* type = t.shadow ? "Texture2D<float>" : t.kind == kTex3D ? "Texture3D" : t.kind == kTexCube ? "TextureCube" : "Texture2D";
        s += F("%s tex%d : register(t%d);\n%s smp%d : register(s%d);\n", type, i, i, t.shadow ? "SamplerComparisonState" : "SamplerState", i, i);
    }
    const int mat = MaterialStage(k);
    if (mat >= 0) s += "Texture2D mnrm : register(t4);\nTexture2D mmat : register(t5);\nTexture2DArray mdet : register(t6);\nTexture2D sunvis : register(t7);\nTexture2D wavef : register(t8);\nTexture2D trailf : register(t9);\nSamplerState wavelin : register(s4);\nSamplerState mtri : register(s5);\nSamplerState mmatsmp : register(s6);\n";
    else if (k.soft) s += "Texture2D<float> sdepth : register(t6);\n";
    std::string lit = k.lit ? PixelLightingCode(k.lit) : std::string();
    if (!lit.empty()) s += kVsHelpers + lit;
    const bool gbuf = k.gbuf && mat >= 0 && !k.quad;
    if (mat >= 0) s += "static float4 g_gbuf = 0;\n";  // the reflection buffer's, written by MaterialShade
    if (gbuf) s += "float4 shade_main(VOut i, bool front) {\n";
    else s += F("float4 main(VOut i, bool front : SV_IsFrontFace) : SV_Target {\n");
    s += "    float4 tc0 = i.tc0, tc1 = i.tc1, tc2 = i.tc2, tc3 = i.tc3;\n";
    s += k.two_sided ? "    float4 v0 = front ? i.d0 : i.b0, v1 = front ? i.d1 : i.b1;\n" : "    float4 v0 = i.d0, v1 = i.d1;\n";
    s += "    if (misc.w == 0) v1 = float4(0, 0, 0, 1);\n";
    if (!lit.empty())
        // (the alpha kept as the vertices gave it, and what is see-through by it lit by them: a propeller's disc went
        // white and opaque)
        // (where the vertices' normals turn fast across the screen or stand far off the face's own, a flat wall's noisy normals that the vertex
        // lighting averaged away, striped and chevroned per pixel: the vertex lighting kept there)
        s += F("    if (%sdot(i.pn.xyz, i.pn.xyz) > 1e-12) {\n"
               "        float3 pnn = normalize(i.pn.xyz);\n"
               "        float3 geo = cross(ddx(i.pv.xyz), ddy(i.pv.xyz));\n"
               "        float agree = dot(geo, geo) > 1e-20 ? abs(dot(pnn, normalize(geo))) : 1;\n"
               "        float turn = length(fwidth(pnn));\n"
               "        float keep = max(max(smoothstep(0.03, 0.09, turn), smoothstep(0.7, 0.4, agree)), smoothstep(0.97, 0.85, v0.a));\n"
               "        v0.rgb = lerp(saturate(PixelLit(pnn, i.pv.xyz)).rgb, v0.rgb, keep);\n"
               "    }\n",
               k.two_sided ? "front && " : "");
    // a character (skinned, mrefl.z on): its shade a little warmer (the skin's and cloth's scattering), in proportion
    // to the light it has (an amount added in the dark made a night's faint ripples of light red stripes on faces;
    // a rim from the program's normal and the eye striped heads the same way, gone)
    if (!lit.empty())
        s += "    if (mrefl.z > 0 && i.pv.w == 2 && dot(i.pn.xyz, i.pn.xyz) > 1e-12) {\n"
             "        float l = dot(v0.rgb, float3(0.2126, 0.7152, 0.0722));\n"
             "        float wrap = 1 - smoothstep(0.05, 0.5, l);\n"
             "        v0.rgb *= 1 + float3(0.22, 0.07, 0.02) * wrap;\n"
             "        v0.rgb = saturate(v0.rgb);\n"
             "    }\n";
    s += "    float fa = saturate(i.fogamt);\n";
    // level 1's far water (-jitter.z, d3d12.cpp): its fog at each pixel, not across its huge triangles from their corners
    s += "    if (jitter.z < 0) { float fd = abs(i.cpos.z); fa = saturate(fogp.w == 3 ? (fogp.y - fd) / (fogp.y - fogp.x) : fogp.w == 1 ? exp(-fd * fogp.z) : fogp.w == 2 ? exp(-(fd * fogp.z) * (fd * fogp.z)) : 1); }\n";
    s += "    float4 fogin = float4(fogcolor.rgb, fa);\n";
    std::string body;
    if (k.programmable) {
        for (int i = 0; i < 4; i++) {
            if (i == mat && (k.tex_modes >> (i * 5) & 0x1F) == 1) body += MaterialSetup(i, F("tc%d.xy / tc%d.w + texsize[%d].zw", i, i, i));
            else body += TexFetch(k, i);
            body += Hooks().Fetched(k.tex[i], i);
        }
        for (int i = 0; i < 4; i++) {
            uint32_t m = (k.tex_modes >> (i * 5)) & 0x1F;
            bool fetch = m && m != 4 && m != 5 && m != 0xA && m != 0x11;
            if (fetch && ((k.alphakill >> i) & 1)) body += F("if (t%d.a == 0) discard;\n", i);
        }
        uint32_t mode0 = k.tex_modes & 0x1F;
        body += F("float4 r0 = float4(0, 0, 0, %s), r1 = 0;\n", mode0 ? "t0.a" : "1");
        Ps ps{k};
        uint32_t n = std::min<uint32_t>(k.count & 0xFF, 8);
        for (uint32_t st = 0; st < n; st++) {
            ps.stage = int(st);
            ps.code += "{\n";
            std::string wc = ps.Stage(k.rgb_in[st], k.rgb_out[st], false);
            std::string wa = ps.Stage(k.a_in[st], k.a_out[st], true);
            ps.code += wc + wa + "}\n";
        }
        body += ps.code;
        if (k.final_abcd || k.final_efg) {
            ps.stage = 8;
            uint32_t fl = k.final_efg & 0xFF;
            ps.clamp_sum = fl & 0x80;
            ps.inv_v1 = fl & 0x40;
            ps.inv_r0 = fl & 0x20;
            ps.e = ps.Input(ParseIn(k.final_efg >> 24), false);
            ps.f = ps.Input(ParseIn((k.final_efg >> 16) & 0xFF), false);
            std::string A = ps.Input(ParseIn(k.final_abcd >> 24), false), B = ps.Input(ParseIn((k.final_abcd >> 16) & 0xFF), false),
                        C = ps.Input(ParseIn((k.final_abcd >> 8) & 0xFF), false), D = ps.Input(ParseIn(k.final_abcd & 0xFF), false),
                        G = ps.Input(ParseIn((k.final_efg >> 8) & 0xFF), true);
            body += "float4 outc;\n";
            body += "outc.rgb = " + D + " + lerp(" + C + ", " + B + ", " + A + ");\n";
            body += "outc.a = " + G + ";\n";
        } else {
            body += "float4 outc = float4(lerp(fogin.rgb, r0.rgb, fogin.a), r0.a);\n";
        }
    } else {
        body += "float4 cur = v0, tmp = 0;\n";
        for (int i = 0; i < 4; i++) {
            const uint32_t* f = k.ff[i];
            if (f[0] <= 1) break;
            const TexInfo& t = k.tex[i];
            std::string norm = (t.rect ? F(" / texsize[%d].xy", i) : "") + F(" + texsize[%d].zw", i);
            std::string uv = (k.proj >> i) & 1 ? F("(tc%d.xy / tc%d.w)%s", i, i, norm.c_str()) : F("tc%d.xy%s", i, norm.c_str());
            if (i == 0) uv = "flowuv(" + uv + ")";  // falling water's flow (flow.x)
            std::string cube;
            if (t.shadow)
                body += F("float4 t%d = shadow(tex%d, smp%d, (tc%d.xy / tc%d.w)%s, tc%d.z / tc%d.w, %d, i.pos.xy, texsize[%d].xy);\n", i, i,
                          i, i, i, norm.c_str(), i, i, t.shadow, i);
            else if (t.kind == kTex2D && (t.flags & kTexWide))
                body += F("float4 t%d = spreadtap(tex%d, smp%d, %s, spread[%d], %s);\n", i, i, i, uv.c_str(), i, Hooks().ReadScale(t).c_str());
            else if (t.kind == kTex2D && i == mat) body += MaterialSetup(i, uv);
            else if (t.kind == kTex2D && i == 0)
                // a spinning propeller (flow.x < 0, d3d12.cpp): its texture turned about its tile's centre (the coordinates
                // run over 1..2 too) and averaged, the
                // blades a soft disc, fainter as they smear
                body += F("float4 t0;\n"
                          "if (flow.x < 0) {\n"
                          "    float2 pc = floor(%s), pu = %s - pc - 0.5; t0 = 0;\n"
                          "    [unroll] for (int s = 0; s < 12; s++) { float a = s * 0.5236 + flow.y; float2 r = float2(pu.x * cos(a) - pu.y * sin(a), pu.x * sin(a) + pu.y * cos(a)); r += 0.5; if (all(r >= 0) && all(r <= 1)) t0 += tex0.Sample(smp0, pc + r); }\n"
                          "    float pr = length(pu), pa = atan2(pu.y, pu.x);\n"
                          "    float ghost = pow(0.5 + 0.5 * cos(3 * pa - flow.w), 6);\n"
                          "    t0 /= 12; t0.a *= flow.z * smoothstep(0.5, 0.42, pr) * (0.55 + 0.6 * ghost) * (0.9 + 0.1 * sin(pr * 60));\n"
                          "} else t0 = tex0.Sample(smp0, %s);\n", uv.c_str(), uv.c_str(), uv.c_str());
            else if (t.kind == kTex2D) body += F("float4 t%d = tex%d.Sample(smp%d, %s);\n", i, i, i, uv.c_str());
            else if (t.kind == kTexCube && !(cube = Hooks().Cube(t, i, F("tc%d.xyz", i))).empty()) body += F("float4 t%d = %s;\n", i, cube.c_str());
            else if (t.kind) body += F("float4 t%d = tex%d.Sample(smp%d, tc%d.xyz);\n", i, i, i, i);
            // a stage that reads a texture with none set: the console's texture unit is off and reads 0 (colour and
            // alpha: xemu's PS_TEXTUREMODES_NONE), so a draw modulating it is unseen. As white, the sewers' lamp lit
            // passes (a waterfall's second pass, a splash's disc under a pipe) came out as flat white shapes
            else body += F("float4 t%d = float4(0,0,0,0);\n", i);
            body += Hooks().Fetched(t, i);
            if ((k.alphakill >> i) & 1) body += F("if (t%d.a == 0) discard;\n", i);
            const char* dst = f[8] == 5 ? "tmp" : "cur";
            std::string rgb = FfOp(f[0], FfArg(f[1], i, false), FfArg(f[2], i, false), FfArg(f[3], i, false), i, false);
            std::string a = f[4] <= 1 ? std::string("cur.a") : FfOp(f[4], FfArg(f[5], i, true), FfArg(f[6], i, true), FfArg(f[7], i, true), i, true);
            body += F("{ float3 c3 = saturate(%s); float c1 = saturate(%s); %s = float4(c3, c1); }\n", rgb.c_str(), a.c_str(), dst);
        }
        body += "cur.rgb = saturate(cur.rgb + v1.rgb);\n";
        body += "float4 outc = float4(lerp(fogin.rgb, cur.rgb, fogin.a), cur.a);\n";
    }
    if (mat >= 0) body += MaterialShade(mat);
    // a glowing sprite (layout2d.w, d3d12.cpp's EmitterKind glow) past white, for the bloom
    // a soft particle: its alpha fades over layout2d.z of depth before the scene behind it (the scene's depth at t6;
    // a depth d is at n / (1 - d q), q = surf.w = 1 - near / far: the scene's view depth from the particle's own)
    if (k.soft && mat < 0)
        body += "{ float ds = sdepth.Load(int3(i.pos.xy, 0)); float zf = i.cpos.w;\n"
                "  float zs = zf * (1 - i.pos.z * surf.w) / max(1 - ds * surf.w, 1e-7);\n"
                "  float fd = saturate((zs - zf) / max(abs(layout2d.z), 1));\n"
                "  if (layout2d.z < 0) outc = float4(1 - fd, fd, 0, 1); else outc.a *= fd; }\n";  // < 0: the fade shown
    // close to the eye (the weapon in hand's own flash) no glow, and a softer sprite: firing does not blind
    // level 1's far water (-jitter.z): flat, where the near water rolls; the light and dark of swells laid over it in
    // the world (its flat coordinates), drifting with the water's time, fading with its fog
    body += "if (jitter.z < 0) {\n"
            "    float2 wq = i.tc0.xy * 2.5;\n"
            "    float wt = mrefl.z < 0 ? -mrefl.z : 0;\n"
            "    float sw = 0.25 * sin(wq.x * 6.283 + wt * 0.6 + sin(wq.y * 3.1)) + 0.15 * sin(wq.y * 9.4 - wt * 0.8 + wq.x * 2.3)"
            " + 0.1 * sin((wq.x + wq.y) * 15 + wt * 1.3);\n"
            "    outc.rgb *= 1 + sw * 0.4 * fa;\n"
            "}\n";
    // falling water (flow.x, d3d12.cpp): lit as water catching the light about it, its sheet a little more solid.
    // Its colour the sewer's dark grey water, its light the game's own as it falls off (the tunnel lamp's cone down
    // the sheet: pressed into a narrow range it read as a flat wall), its streaks brighter, never white: past a knee
    // the light eases off (a face turned to the lamp, the sheet's top bend and its foot, hit the old cap at 0.7 and
    // showed as flat white patches, its streaks gone)
    body += "if (flow.x > 0) { float wl = dot(outc.rgb, 0.333), fl = wl * flow.z; fl = fl < 0.3 ? fl : 0.3 + 0.18 * (1 - exp(-(fl - 0.3) / 0.18)); outc.rgb = float3(0.96, 1.0, 0.94) * fl + smoothstep(0.05, 0.12, wl) * float3(0.02, 0.022, 0.017); outc.a = saturate(outc.a * flow.w); }\n";
    body += "alphatest(outc.a);\n" + Hooks().Output() +
            "if (layout2d.w > 0) {\n"
            "    // the glow on the sprite's bright fire only: its smoke and dark edges stay as drawn (brightened past\n"
            "    // white beside them they read as a black smudge), and they fade a little more. Light alone (50 past\n"
            "    // the glow: tracers, sparks): its dark is see-through, a burst's tracers pile up into no blot\n"
            "    bool pure = layout2d.w >= 50;\n"
            "    float glow = pure ? layout2d.w - 50 : layout2d.w;\n"
            "    float gl = dot(outc.rgb, float3(0.299, 0.587, 0.114));\n"
            "    float hot = smoothstep(0.35, 0.8, gl);\n"
            "    outc.rgb *= lerp(1, lerp(0.7, glow, saturate((i.cpos.w - 150) / 350)), hot);\n"
            "    outc.a *= pure ? smoothstep(0.06, 0.35, gl) : lerp(0.6, 1, smoothstep(0.08, 0.3, gl));\n"
            "} else if (layout2d.w < 0) {\n"
            "    // smoke and dust lit by the flashes and fire near them (lt: d3d12.cpp SmokeLights, view space), drawn\n"
            "    // in their colour as the smoke's own is (a thick puff glows inside, thin haze less)\n"
            "    int nl = (int)lt[6].x;\n"
            "    if (nl > 0) {\n"
            "        float3 sp = float3(i.cpos.xy / i.cpos.w * lt[6].yz, 1) * i.cpos.w;\n"
            "        float3 add = 0;\n"
            "        for (int k = 0; k < nl; k++) {\n"
            "            float3 d = lt[k * 7].xyz - sp;\n"
            "            float r = lt[k * 7].w;\n"
            "            float q = dot(d, d) / (r * r);\n"
            "            if (q < 1) add += lt[k * 7 + 1].rgb * (1 - q) * (1 - q) / (1 + 4 * q);\n"
            "        }\n"
            "        float albedo = 0.35 + 0.65 * dot(outc.rgb, float3(0.299, 0.587, 0.114));\n"
            "        outc.rgb += add * albedo * 0.6 / (1 + 0.5 * max(max(add.r, add.g), add.b));\n"
            "    }\n"
            "    // smoke and dust drawn dark (by night): as see-through as dark, gone where black\n"
            "    float gl = dot(outc.rgb, float3(0.299, 0.587, 0.114));\n"
            "    outc.a *= smoothstep(0.03, 0.22, gl);\n"
            "}\n"
            "if (mrefl.w < -1.5) outc = float4(frac(i.tc0.xy / (i.tc0.w == 0 ? 1 : i.tc0.w)), 0, 1);  // tests (XBE_UV_DEBUG)\n"
            "else if (mrefl.w < 0) outc = float4(tess2.yzw, 1);  // tests (texview): each texture's own colour\n"
            "return outc;\n}\n";
    if (gbuf)
        body += "struct PsOut { float4 c : SV_Target0; float4 g : SV_Target1; };\n"
                "PsOut main(VOut i, bool front : SV_IsFrontFace) {\n"
                "    PsOut o;\n    o.c = shade_main(i, front);\n    o.g = g_gbuf;\n    return o;\n}\n";
    s += body;
    return k.quad ? QuadHlsl(std::move(s)) : s;
}

// the vertex shader hands on the pixel position as drawn and as pulled (e), the pixel shader takes the stretch back out
std::string QuadHlsl(std::string s) {
    auto sub = [&](const char* at, const char* before, const char* after) {
        size_t p = s.find(at);
        if (p != std::string::npos) s.replace(p, std::strlen(at), std::string(before) + at + after);
    };
    sub("    float fogamt : TEXCOORD4;\n", "", "    noperspective float4 e : TEXCOORD5;\n");
    sub("    if (abs(p.x - (surf.x - 0.5)) < 1e-3) p.x = surf.x;\n", "    o.e.xy = p.xy;\n", "");
    sub("    if (abs(p.y - (surf.y - 0.5)) < 1e-3) p.y = surf.y;\n", "", "    o.e.zw = p.xy;\n");
    sub("    o.pos = cp;\n", "    o.e = float4(sp, (float2(cp.x, -cp.y) / cp.w + 1) * 0.5 * surf.xy);\n", "");
    sub("float4 main(VOut i, bool front", R"(float4 unstretch(float4 t, float4 e) {
    float2 d = e.zw - e.xy, g = float2(ddx(e.x), ddy(e.y));
    return t + (d.x ? d.x / g.x * ddx(t) : 0) + (d.y ? d.y / g.y * ddy(t) : 0);
}
)", "");
    for (int i = 0; i < 4; i++) sub(F("i.tc%d", i).c_str(), "unstretch(", ", i.e)");
    return s;
}

}  // namespace gpu
