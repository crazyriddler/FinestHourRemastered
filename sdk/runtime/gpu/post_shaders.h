#pragma once
// Passes on the shown frame (d3d12.cpp PostProcess): SMAA and contrast adaptive sharpening. Each source is a full
// screen pass in the backend's Pass form: vs and the entry, the parameters at b1, sources at t0 to t2, samplers s0
// linear and s1 point.
#include <string>

#include "gpu/smaa_shader.h"

namespace gpu {

inline constexpr char kSmaaConfig[] = R"(
cbuffer P : register(b1) { float4 metrics; float4 unused; };
#define SMAA_RT_METRICS metrics
#define SMAA_CUSTOM_SL
#define SMAA_THRESHOLD 0.05
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)";

inline constexpr char kSmaaCommon[] = R"(
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
Texture2D tex2 : register(t2);
float4 corner(uint id, out float2 uv) {
    uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";

// the three passes: edges from the frame's luma, blend weights from the edges and SMAA's area and search lookups,
// then the frame blended by the weights
inline const std::string& SmaaPass(int pass) {
    static const std::string edges = std::string(kSmaaConfig) + kSmaaHlsl + kSmaaCommon + R"(
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o[3] : TEXCOORD1; };
V vs(uint id : SV_VertexID) { V o; o.p = corner(id, o.uv); SMAAEdgeDetectionVS(o.uv, o.o); return o; }
float4 smaa_edges(V i) : SV_Target { return float4(SMAALumaEdgeDetectionPS(i.uv, i.o, tex0), 0, 0); }
)";
    static const std::string weights = std::string(kSmaaConfig) + kSmaaHlsl + kSmaaCommon + R"(
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o[3] : TEXCOORD2; };
V vs(uint id : SV_VertexID) { V o; o.p = corner(id, o.uv); SMAABlendingWeightCalculationVS(o.uv, o.pix, o.o); return o; }
float4 smaa_weights(V i) : SV_Target { return SMAABlendingWeightCalculationPS(i.uv, i.pix, i.o, tex0, tex1, tex2, 0); }
)";
    static const std::string blend = std::string(kSmaaConfig) + kSmaaHlsl + kSmaaCommon + R"(
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o : TEXCOORD1; };
V vs(uint id : SV_VertexID) { V o; o.p = corner(id, o.uv); SMAANeighborhoodBlendingVS(o.uv, o.o); return o; }
float4 smaa_blend(V i) : SV_Target { return SMAANeighborhoodBlendingPS(i.uv, i.o, tex0, tex1); }
)";
    return pass == 0 ? edges : pass == 1 ? weights : blend;
}

// Contrast adaptive sharpening, after AMD FidelityFX CAS (MIT): each pixel takes its four neighbours with a negative
// weight that is largest where the 3x3 neighbourhood has the most room left before it would clip, so flat areas and
// soft gradients sharpen while edges already at full contrast do not ring. p0.x strength 0..1; p1.zw the size.
// The scene's brightness for the eye's exposure (d3d12.cpp Exposure): a 16 by 12 grid of the scene's points, each its
// own pixel: the log luminance times its weight (the middle weighing more), the weight; averaged as it is read back
inline constexpr char kMeterHlsl[] = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; };
Texture2D src : register(t0);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
float2 meter(O i) : SV_Target {
    float2 uv = floor(i.p.xy) / float2(16, 12) + float2(0.5 / 16, 0.5 / 12);
    float3 c = src.Load(int3(uv * p1.zw, 0)).rgb;
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    float k = 1 + 2 * saturate(1 - length((uv - 0.5) * 2));
    return float2(log2(max(l, 1e-3)) * k, k);
}
)";

inline constexpr char kCasHlsl[] = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; };
Texture2D src : register(t0);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
float3 at(int2 p, int2 d) { return src.Load(int3(clamp(p + d, int2(0, 0), int2(p1.zw) - 1), 0)).rgb; }
float4 cas(O i) : SV_Target {
    int2 p = int2(i.p.xy);
    float4 ce = src.Load(int3(p, 0));
    float3 a = at(p, int2(-1, -1)), b = at(p, int2(0, -1)), c = at(p, int2(1, -1));
    float3 d = at(p, int2(-1, 0)), e = ce.rgb, f = at(p, int2(1, 0));
    float3 g = at(p, int2(-1, 1)), h = at(p, int2(0, 1)), k = at(p, int2(1, 1));
    float3 mn = min(min(min(d, e), min(f, b)), h);
    mn += min(mn, min(min(a, c), min(g, k)));
    float3 mx = max(max(max(d, e), max(f, b)), h);
    mx += max(mx, max(max(a, c), max(g, k)));
    float3 amp = sqrt(saturate(min(mn, 2 - mx) / max(mx, 1e-5)));
    float3 w = amp * (-1 / lerp(8.0, 5.0, p0.x));
    float3 r = ((b + d + f + h) * w + e) / (1 + 4 * w);
    return float4(saturate(r), ce.a);
}
)";

}  // namespace gpu
