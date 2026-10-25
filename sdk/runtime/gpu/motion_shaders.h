#pragma once
// The passes on the finished scene (renderer.cpp): the Anti-Aliasing (FxaaPass).
namespace gpu {

// the passes' full screen triangle (vs), uv 0 to 1 down the screen
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
