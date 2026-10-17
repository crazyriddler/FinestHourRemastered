#pragma once
// Motion (d3d12.cpp MotionPass, MotionBlurPass, TaaPass): the screen's velocity, each pixel's move since the frame
// before in uv (rg all of it, ba the object's own: what the camera's turn and travel do not explain); the per object
// motion blur; the temporal anti-aliasing that replaces SMAA.
namespace gpu {

// The velocity passes' parameters: mv0 1 / P00, 1 / P11, P22, P32 (the camera's projection); mv1 the viewport's depth
// min and range, the target's size; cam the view space now to the clip space of the frame before (the camera's own
// move); mv2 the frame's jitter in clip space.
inline constexpr char kMotionFns[] = R"(
    float4 mv0; float4 mv1; row_major float4x4 cam; float4 mv2;
};
float MViewZ(float d) {
    float k = (d - mv1.x) / max(mv1.y, 1e-6) - mv0.z;
    return mv0.w / (abs(k) < 1e-9 ? -1e-9 : k);
}
// the camera's own motion at a pixel of the given depth: where the point seen there was on the screen the frame
// before (with this frame's jitter, as the frame was drawn: still things move 0)
float2 CamVel(float2 pix, float d) {
    float2 uv = pix / mv1.zw;
    float z = MViewZ(d);
    float3 P = float3((uv.x * 2 - 1) * mv0.x, (1 - uv.y * 2) * mv0.y, 1) * z;
    float4 q = mul(float4(P, 1), cam);
    float2 n = q.xy / max(q.w, 1e-3) + mv2.xy;
    return uv - (n * float2(0.5, -0.5) + 0.5);
}
)";

// the objects' draws (their motion vertex shader: this frame's and the frame before's clip positions): where the
// scene's world depth has them (a tolerance: tessellated characters, cut out leaves keep what is behind them)
inline constexpr char kMotionPsMain[] = R"(
Texture2D<float> wdepth : register(t0);
struct MOut { float4 pos : SV_Position; float4 cur : TEXCOORD0; float4 prev : TEXCOORD1; };
float4 main(MOut i) : SV_Target {
    float ds = wdepth.Load(int3(i.pos.xy, 0));
    float zf = MViewZ(i.pos.z), zs = MViewZ(min(ds, 0.99999));
#ifdef OVER
    // see-through things (smoke, sparks, fire, tracers): where they are in front of what is there
    if (zf > zs + 4 + zs * 0.02) discard;
#else
    if (ds >= 0.99999) discard;
    if (abs(zf - zs) > 4 + zs * 0.02) discard;
#endif
    float2 v = (i.cur.xy / i.cur.w - i.prev.xy / max(i.prev.w, 1e-3)) * float2(0.5, -0.5);
    return float4(v, v - CamVel(i.pos.xy, ds));
}
)";

inline constexpr char kPassVs[] = R"(
SamplerState lin : register(s0);
SamplerState pnt : register(s1);
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
)";

// the background: everything moves with the camera alone (the objects' draws overwrite it)
inline constexpr char kCamVelMain[] = R"(
Texture2D<float> wdepth : register(t0);
float4 camvel(O i) : SV_Target {
    float d = min(wdepth.Load(int3(i.pos.xy, 0)), 0.99999);
    return float4(CamVel(i.pos.xy, d), 0, 0);
}
)";

// Per object motion blur: along the object's own motion (half the frame's: a 180 degree shutter), at most 4% of the
// screen; a still pixel by a moving thing takes the thing's smear (only from samples that move); the weapon in hand
// (t2 the depth now, mb0.w its switch: the depth was cleared for it) stays sharp.
inline constexpr char kMotionBlurHlsl[] = R"(
cbuffer B : register(b1) { float4 mb0; };  // size xy, strength, the weapon's depth cleared
Texture2D scene : register(t0);
Texture2D vel : register(t1);
Texture2D<float> dnow : register(t2);
float4 mblur(O i) : SV_Target {
    int2 px = int2(i.pos.xy);
    float4 c = scene.Load(int3(px, 0));
    // XBE_VELOCITY_DEBUG: the object's motion (red, green), the camera's (blue)
    if (mb0.z > 1.5) {
        float4 vv = vel.Load(int3(px, 0));
        return float4(abs(vv.zw) * 60, length(vv.xy - vv.zw) * 60, 1);
    }
    if (mb0.w > 0 && dnow.Load(int3(px, 0)) < 1) return c;
    float2 own = vel.Load(int3(px, 0)).zw;
    float2 v = own;
    // the weapon in hand is neither blurred nor blurred into what is around it: its motion and colour left out
    // (its edge smeared over the world behind read as a second outline)
    [unroll] for (int k = 0; k < 8; k++) {
        float a = k * 0.785398;
        int2 q = px + int2(round(float2(cos(a), sin(a)) * 8));
        if (mb0.w > 0 && dnow.Load(int3(q, 0)) < 1) continue;
        float2 n = vel.Load(int3(q, 0)).zw;
        if (dot(n, n) > dot(v, v)) v = n;
    }
    // the pixel moves itself (its own motion at least half the strongest near): every sample counts
    bool moving = dot(own, own) >= dot(v, v) * 0.25;
    v *= 0.5 * mb0.z;
    float len = length(v);
    if (len > 0.04) v *= 0.04 / len;
    float lenpx = length(v * mb0.xy);
    if (lenpx < 1.5) return c;
    float3 acc = c.rgb;
    float w = 1;
    [unroll] for (int s = 0; s < 12; s++) {
        float t = (s + 0.5) / 12 - 0.5;
        float2 uv = i.uv + v * t;
        float2 sv = vel.SampleLevel(pnt, uv, 0).zw;
        float wk = moving ? 1 : saturate(length(sv) / max(length(v), 1e-6) * 2 - 0.5);
        if (mb0.w > 0 && dnow.Load(int3(int2(uv * mb0.xy), 0)) < 1) wk = 0;
        acc += scene.SampleLevel(lin, uv, 0).rgb * wk;
        w += wk;
    }
    return float4(acc / w, c.a);
}
)";

// Temporal anti-aliasing: this frame (jittered) blended into the history reprojected along the velocity (Catmull-Rom),
// the history clipped to the neighbourhood's colour box (YCoCg, tone mapped: no ghosts, no fireflies); faster where
// things move; a reset (a cut, a new size) takes the frame as it is. The weapon in hand moves with the eye.
inline constexpr char kTaaHlsl[] = R"(
cbuffer T : register(b1) { float4 ta0; float4 ta1; };  // size xy, 1 / size; reset, the weapon's depth cleared
Texture2D cur : register(t0);
Texture2D hist : register(t1);
Texture2D vel : register(t2);
Texture2D<float> dnow : register(t3);
float3 Tm(float3 c) { return c / (1 + max(max(c.r, c.g), c.b)); }
float3 Itm(float3 c) { return c / max(1 - max(max(c.r, c.g), c.b), 1e-4); }
float3 ToY(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 FromY(float3 y) { return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }
float3 Hist(float2 uv) {
    float2 sp = uv * ta0.xy;
    float2 tc = floor(sp - 0.5) + 0.5;
    float2 f = sp - tc;
    float2 w0 = f * (-0.5 + f * (1 - 0.5 * f));
    float2 w1 = 1 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 o12 = w2 / w12;
    float2 t0 = (tc - 1) * ta0.zw, t3 = (tc + 2) * ta0.zw, t12 = (tc + o12) * ta0.zw;
    float3 r = hist.SampleLevel(lin, float2(t12.x, t0.y), 0).rgb * (w12.x * w0.y) +
               hist.SampleLevel(lin, float2(t0.x, t12.y), 0).rgb * (w0.x * w12.y) +
               hist.SampleLevel(lin, t12, 0).rgb * (w12.x * w12.y) +
               hist.SampleLevel(lin, float2(t3.x, t12.y), 0).rgb * (w3.x * w12.y) +
               hist.SampleLevel(lin, float2(t12.x, t3.y), 0).rgb * (w12.x * w3.y);
    float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / ws, 0);
}
float4 taa(O i) : SV_Target {
    int2 px = int2(i.pos.xy);
    float4 c0 = cur.Load(int3(px, 0));
    float3 cy = ToY(Tm(max(c0.rgb, 0)));
    float3 m1 = 0, m2 = 0, lo = 1e9, hi = -1e9;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++) {
            float3 s = ToY(Tm(max(cur.Load(int3(px + int2(x, y), 0)).rgb, 0)));
            m1 += s, m2 += s * s, lo = min(lo, s), hi = max(hi, s);
        }
    if (ta1.x > 0) return c0;
    // the velocity: the pixel's, or a neighbour's moving thing's (its edge carries its motion)
    float4 v4 = vel.Load(int3(px, 0));
    [unroll] for (int k = 0; k < 4; k++) {
        int2 o = int2(k & 1 ? 1 : -1, k & 2 ? 1 : -1);
        float4 n = vel.Load(int3(px + o, 0));
        if (dot(n.zw, n.zw) > dot(v4.zw, v4.zw) * 1.5 + 1e-8) v4 = n;
    }
    float2 v = v4.xy;  // (the weapon in hand's own motion too: it sways with the eye)
    float2 huv = i.uv - v;
    if (any(huv < 0) || any(huv > 1)) return c0;
    // the weapon in hand (in front, its own depth): it moves with the eye's every sway, its history held tighter and
    // replaced faster (no double outline as it moves), still averaged where it rests (its fine grain)
    bool weapon = ta1.y > 0 && dnow.Load(int3(px, 0)) < 1;
    float3 mean = m1 / 9, sig = sqrt(abs(m2 / 9 - mean * mean));
    float gamma = weapon ? 0.85 : 1.25;
    float3 blo = max(lo, mean - sig * gamma), bhi = min(hi, mean + sig * gamma);
    float3 h = ToY(Tm(Hist(huv)));
    float3 pc = 0.5 * (bhi + blo), e = 0.5 * (bhi - blo) + 1e-4;
    float3 d = h - pc;
    float3 u = abs(d / e);
    float ma = max(u.x, max(u.y, u.z));
    if (ma > 1) h = pc + d / ma;
    float speed = length(v * ta0.xy);
    float a = lerp(0.09, 0.25, saturate(speed / 12));
    if (weapon) a = lerp(0.2, 0.6, saturate(speed / 4));
    float3 r = lerp(h, cy, a);
    return float4(Itm(FromY(r)), c0.a);
}
)";

// The film grain after the temporal anti-aliasing (its history keeps none: it would be averaged away): faint, fine,
// in the mid tones (g0.x its strength, g0.y the time)
inline constexpr char kGrainHlsl[] = R"(
cbuffer G : register(b1) { float4 g0; };
Texture2D scene : register(t0);
float GHash(float2 q) {
    float3 h = frac(float3(q.xyx) * 0.1031);
    h += dot(h, h.yzx + 33.33);
    return frac((h.x + h.y) * h.z);
}
float4 grain(O i) : SV_Target {
    float4 c = scene.Load(int3(i.pos.xy, 0));
    float l = dot(saturate(c.rgb), float3(0.2126, 0.7152, 0.0722));
    float2 q = i.pos.xy + float2(frac(g0.y * 7.31) * 613, frac(g0.y * 3.17) * 411);
    float n = (GHash(q) + GHash(q + 17.3) - 1) * g0.x;
    c.rgb += n * 4 * l * (1 - l);
    return c;
}
)";

}  // namespace gpu
