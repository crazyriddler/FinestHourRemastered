#pragma once
// Motion (renderer.cpp MotionPass, MotionBlurPass): the screen's velocity, each pixel's move since the frame before
// in uv (rg all of it, ba the object's own: what the camera's turn and travel do not explain); the per object motion
// blur. And the Anti-Aliasing (FxaaPass).
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

// Anti-Aliasing: FXAA 3.11 (quality, 12 steps) on the 3D scene before the 2D is drawn over it (the HUD and the text
// stay sharp). Its luma from the colour tone mapped (the scene can go past white). f0: 1 / size, size.
inline constexpr char kFxaaHlsl[] = R"(
cbuffer F : register(b1) { float4 f0; };
Texture2D scene : register(t0);
float FLuma(float3 c) {
    c = max(c, 0) / (1 + max(max(c.r, c.g), max(c.b, 0)));
    return sqrt(dot(c, float3(0.299, 0.587, 0.114)));
}
float FL(float2 uv) { return FLuma(scene.SampleLevel(lin, uv, 0).rgb); }
float4 fxaa(O i) : SV_Target {
    const float kSteps[12] = {1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};
    float2 rcp = f0.xy;
    float2 uv = (floor(i.pos.xy) + 0.5) * rcp;
    int2 p = int2(i.pos.xy);
    float4 rgbM = scene.Load(int3(p, 0));
    float lumaM = FLuma(rgbM.rgb);
    float lumaS = FLuma(scene.Load(int3(p, 0), int2(0, 1)).rgb);
    float lumaE = FLuma(scene.Load(int3(p, 0), int2(1, 0)).rgb);
    float lumaN = FLuma(scene.Load(int3(p, 0), int2(0, -1)).rgb);
    float lumaW = FLuma(scene.Load(int3(p, 0), int2(-1, 0)).rgb);
    float rangeMax = max(max(max(lumaN, lumaW), max(lumaE, lumaS)), lumaM);
    float rangeMin = min(min(min(lumaN, lumaW), min(lumaE, lumaS)), lumaM);
    float range = rangeMax - rangeMin;
    if (range < max(0.0625, rangeMax * 0.125)) return rgbM;
    float lumaNW = FLuma(scene.Load(int3(p, 0), int2(-1, -1)).rgb);
    float lumaSE = FLuma(scene.Load(int3(p, 0), int2(1, 1)).rgb);
    float lumaNE = FLuma(scene.Load(int3(p, 0), int2(1, -1)).rgb);
    float lumaSW = FLuma(scene.Load(int3(p, 0), int2(-1, 1)).rgb);
    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = -2.0 * lumaM + lumaNS, edgeVert1 = -2.0 * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = -2.0 * lumaE + lumaNESE, edgeVert2 = -2.0 * lumaN + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2), edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    float edgeHorz3 = -2.0 * lumaW + lumaNWSW, edgeVert3 = -2.0 * lumaS + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4, edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = rcp.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) lumaN = lumaW, lumaS = lumaE;
    else lengthSign = rcp.y;
    float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = saturate(abs(subpixB) * subpixRcpRange);
    float2 posB = uv;
    float2 offNP = horzSpan ? float2(rcp.x, 0) : float2(0, rcp.y);
    if (!horzSpan) posB.x += lengthSign * 0.5;
    else posB.y += lengthSign * 0.5;
    float2 posN = posB - offNP * kSteps[0], posP = posB + offNP * kSteps[0];
    float subpixD = -2.0 * subpixC + 3.0;
    float lumaEndN = FL(posN), lumaEndP = FL(posP);
    float subpixE = subpixC * subpixC;
    if (!pairN) lumaNN = lumaSS;
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    lumaEndN -= lumaNN * 0.5;
    lumaEndP -= lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled, doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) posN -= offNP * kSteps[1];
    if (!doneP) posP += offNP * kSteps[1];
    [unroll] for (int k = 2; k < 12; k++) {
        if (doneN && doneP) break;
        if (!doneN) lumaEndN = FL(posN) - lumaNN * 0.5;
        if (!doneP) lumaEndP = FL(posP) - lumaNN * 0.5;
        doneN = abs(lumaEndN) >= gradientScaled;
        doneP = abs(lumaEndP) >= gradientScaled;
        if (!doneN) posN -= offNP * kSteps[k];
        if (!doneP) posP += offNP * kSteps[k];
    }
    float dstN = horzSpan ? uv.x - posN.x : uv.y - posN.y;
    float dstP = horzSpan ? posP.x - uv.x : posP.y - uv.y;
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero, goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLengthRcp = 1.0 / (dstP + dstN);
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = dst * -spanLengthRcp + 0.5;
    float subpixH = subpixG * 0.75;
    float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if (!horzSpan) uv.x += pixelOffsetSubpix * lengthSign;
    else uv.y += pixelOffsetSubpix * lengthSign;
    return float4(scene.SampleLevel(lin, uv, 0).rgb, rgbM.a);
}
)";

}  // namespace gpu
