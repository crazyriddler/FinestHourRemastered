#pragma once
// The shadows' screen pass (d3d12.cpp ShadowMask): from the scene's depth, each pixel's place in the view, its
// cascade's shadow map filtered over a disc, and how much the scene's light there is the main light's (the rest,
// ambient and what the game baked or lights itself, stays). Out: r the scene's multiplier, g the shadow (1 lit),
// b the cascade, a the main light's share.
namespace gpu {

inline constexpr char kShadowMask[] = R"(
cbuffer P : register(b1) {
    float4 p0;   // 1 / P00, 1 / P11, P22, P32: the camera's projection
    float4 p1;   // the light in view space (towards it) xyz, strength
    float4 p2;   // size xy, 1 / size xy
    float4 p3;   // the light's luminance, the ambient's, fade start and end (view depth)
    float4 p4;   // the cascades' far ends (view depth)
    float4 p5;   // cascades, atlas size, filter radius in texels, taps
    float4 p6;   // per cascade: a texel's world size
    float4 p7;   // per cascade: 1 / its depth range
    float4 p8;   // viewport depth min, max - min, debug, 0
    float4 tile[4];   // atlas uv from a cascade's: xy scale, zw offset
    row_major float4x4 cas[4];  // view to the cascades' light clip
    float4 p9;   // the haze (FogPass): density per unit, the ground's height (view space, along p10), the sun's light
    float4 p10;  // the world's up in view space
};
Texture2D<float> depth : register(t0);
Texture2D<float> atlas : register(t1);
Texture2D scene : register(t2);
Texture2D<float> chars : register(t3);  // 1 on the characters
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
float ViewZ(float d) {
    // the depth buffer holds P22 + P32 / z within the viewport's depth range
    float k = (d - p8.x) / max(p8.y, 1e-6) - p0.z;
    return p0.w / (abs(k) < 1e-9 ? -1e-9 : k);
}
float3 ViewPos(float2 uv, float d) {
    float z = ViewZ(d);
    return float3((uv.x * 2 - 1) * p0.x, (1 - uv.y * 2) * p0.y, 1) * z;
}
float D(float2 uv) { return depth.SampleLevel(pnt, uv, 0); }

static const float2 kDisc[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464), float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420), float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590), float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790)};

// a bilinear blend of the four compares around uv
float Pcf(float2 uv, float z) {
    float4 g = atlas.GatherRed(pnt, uv);
    float4 lit = step(z, g);
    float2 f = frac(uv * p5.y - 0.5);
    return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);
}

float Cascade(int c, float3 P, float3 N, float ndl, float2 rot, float ch) {
    float t = p6[c];
    // off the surface along its normal, more as it turns from the light: no acne on slopes. A character is moved
    // towards the light past its own body (a third of a metre): its coarse facets do not shade each other, the
    // world still shades it
    float3 Q = ch > 0.5 ? P + p1.xyz * (33 + 3 * t) : P + N * t * (0.8 + 1.8 * (1 - ndl));
    float4 l = mul(float4(Q, 1), cas[c]);
    if (any(abs(l.xy) > 0.995)) return 1;
    float z = l.z - t * 0.6 * p7[c];
    float2 uv = float2(l.x * 0.5 + 0.5, 0.5 - l.y * 0.5) * tile[c].xy + tile[c].zw;
    float2 lo = tile[c].zw + 1.5 / p5.y, hi = tile[c].zw + tile[c].xy - 1.5 / p5.y;
    float r = p5.z / p5.y;
    // contact hardening (p8.w on): the blockers' average depth over a wider disc, the filter's radius by how far the
    // receiver is behind them: sharp where a shadow meets what casts it, softer away from it
    if (p8.w > 0) {
        float found = 0, sum = 0;
        float sr = r * 4;
        [unroll] for (int b = 0; b < 8; b++) {
            float2 o = kDisc[b * 2];
            o = float2(o.x * rot.x - o.y * rot.y, o.x * rot.y + o.y * rot.x);
            float bz = atlas.SampleLevel(pnt, clamp(uv + o * sr, lo, hi), 0);
            if (bz < z) sum += bz, found += 1;
        }
        if (found > 0) {
            float gap = (z - sum / found) / max(p7[c], 1e-6);  // the receiver behind the blockers, in world units
            r *= clamp(0.35 + gap / (t * 40), 0.35, 3.0);
        } else {
            return 1;  // nothing in the way
        }
    }
    int n = (int)p5.w;
    float s = 0;
    // five taps spread over the disc first (its corners and one by the middle): all lit or all shaded, so is the
    // rest (the full count is for the penumbra alone; most of the screen is wholly one or the other)
    uint first = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 7);
    if (n >= 8) {
        [unroll] for (int f = 0; f < 8; f++) {
            if (!(first & (1u << f))) continue;
            float2 o = kDisc[f];
            o = float2(o.x * rot.x - o.y * rot.y, o.x * rot.y + o.y * rot.x);
            s += Pcf(clamp(uv + o * r, lo, hi), z);
        }
        if (s < 1e-3 || s > 5 - 1e-3) return s / 5;
    } else {
        first = 0;
    }
    [loop] for (int k = 0; k < n; k++) {
        if (first & (1u << k)) continue;
        float2 o = kDisc[k];
        o = float2(o.x * rot.x - o.y * rot.y, o.x * rot.y + o.y * rot.x);
        s += Pcf(clamp(uv + o * r, lo, hi), z);
    }
    return s / n;
}

// The volumetric haze (d3d12.cpp FogPass, half size): from the eye to the surface (or 120 m into the sky), p9.w steps
// through the air; each step's haze (thin, thicker low: p9.x density, p9.y the ground's height in view space along
// the world's up p10.xyz) takes the sun's light where the sun's cascades see it (its beams past what shades them),
// forward scattered (towards the sun brighter). Out: rgb the light the haze adds, a how much it hides behind it.
float SunAt(float3 X) {
    int count = (int)p5.x;
    int c = 0;
    [unroll] for (int k = 0; k < 3; k++) c += (k < count - 1 && X.z > p4[k]) ? 1 : 0;
    float4 l = mul(float4(X, 1), cas[c]);
    if (any(abs(l.xy) > 0.995) || l.z > 1) return 1;
    float2 uv = float2(l.x * 0.5 + 0.5, 0.5 - l.y * 0.5) * tile[c].xy + tile[c].zw;
    return Pcf(uv, l.z - p6[c] * 2 * p7[c]);
}
float4 fog(O i) : SV_Target {
    float d = D(i.uv);
    float3 P = d >= 0.99999 ? ViewPos(i.uv, 0.5) : ViewPos(i.uv, d);
    float len = d >= 0.99999 ? 12000 : min(length(P), 12000);
    float3 dir = normalize(P);
    float3 up = p10.xyz;
    float jitter = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))));
    int n = (int)p9.w;
    float stepl = len / n;
    float mu = dot(dir, p1.xyz);
    // the sun's light through the haze: a broad forward lobe (Henyey-Greenstein, g 0.6) and a little all round
    float g = 0.6;
    float phase = (1 - g * g) / pow(1 + g * g - 2 * g * mu, 1.5) * 0.08 + 0.05;
    float trans = 1, sun = 0;
    [loop] for (int k = 0; k < n; k++) {
        float3 X = dir * (k + jitter) * stepl;
        float h = dot(X, up) - p9.y;  // height over the ground
        float dens = p9.x * (0.35 + 0.65 * exp(-max(h, 0) / 600));
        float a = 1 - exp(-dens * stepl);
        float lit = p3.x > 0 && X.z < p3.w ? SunAt(X) : 1;
        sun += trans * a * lit;
        trans *= 1 - a;
    }
    return float4(p9.zzz * sun * phase * p1.w, 1 - trans);
}
float4 mask(O i) : SV_Target {
    float d = D(i.uv);
    if (d >= 0.99999) return float4(1, 1, 0, 0);  // the sky
    float3 P = ViewPos(i.uv, d);
    if (P.z <= 0 || P.z >= p3.w) return float4(1, 1, 1, 0);
    // the normal from the nearer of each pair of neighbours, so edges keep their own side
    float2 px = p2.zw;
    float3 l = ViewPos(i.uv - float2(px.x, 0), D(i.uv - float2(px.x, 0))), r = ViewPos(i.uv + float2(px.x, 0), D(i.uv + float2(px.x, 0)));
    float3 u = ViewPos(i.uv - float2(0, px.y), D(i.uv - float2(0, px.y))), dn = ViewPos(i.uv + float2(0, px.y), D(i.uv + float2(0, px.y)));
    float3 dx = abs(r.z - P.z) < abs(P.z - l.z) ? r - P : P - l;
    float3 dy = abs(dn.z - P.z) < abs(P.z - u.z) ? dn - P : P - u;
    float3 N = normalize(cross(dy, dx));
    if (dot(N, P) > 0) N = -N;
    float3 L = p1.xyz;
    float ndl = dot(N, L);
    // the main light's share of the light here: what faces away from it has none to lose
    // a character's normal from the depth is its facets': the game lights it smoothly itself, the shadow takes an
    // even share of it
    float ch = chars.Load(int3(i.pos.xy, 0));
    float direct = p3.x * (ch > 0.5 ? 0.55 : saturate(ndl));
    float share = direct / (p3.y + direct + 0.02) * p1.w;
    // the game's look keeps its shade open (its baked light has the sky's and the bounce's in it): a shadow takes
    // at most under half of what is there, a character (lit for its place by the game) a quarter
    share = min(share * 0.6, ch > 0.5 ? 0.25 : 0.45);
    // what glows (lamps, fire, the sky's light through a window) is not the light's to take
    float lum = dot(scene.Load(int3(i.pos.xy, 0)).rgb, float3(0.2126, 0.7152, 0.0722));
    share *= saturate((1.15 - lum) / 0.45);
    // and what the game drew dark already (its own shade, inside the buildings) is not darkened again much
    share *= lerp(0.25, 1, smoothstep(0.03, 0.22, lum));
    int count = (int)p5.x;
    int c = 0;
    [unroll] for (int k = 0; k < 3; k++) c += (k < count - 1 && P.z > p4[k]) ? 1 : 0;
    if (p8.z == 6) {
        float4 lc = mul(float4(P, 1), cas[c]);
        float2 uv = float2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5) * tile[c].xy + tile[c].zw;
        return float4(lc.z, atlas.SampleLevel(pnt, uv, 0), saturate((lc.z - atlas.SampleLevel(pnt, uv, 0)) * 20 + 0.5), 1);
    }
    float s = 1;
    if (share > 0.002 && (ndl > -0.05 || ch > 0.5)) {
        float a = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715)))) * 6.2831853;
        float2 rot = float2(cos(a), sin(a));
        float nl = saturate(ndl);
        s = Cascade(c, P, N, nl, rot, ch);
        // the last tenth of a cascade blends into the next
        float start = c == 0 ? 0 : p4[c - 1];
        float b = saturate((P.z - lerp(start, p4[c], 0.88)) / max(p4[c] - lerp(start, p4[c], 0.88), 1e-3));
        if (b > 0 && c + 1 < count) s = lerp(s, Cascade(c + 1, P, N, nl, rot, ch), b);
        s = lerp(s, 1, saturate((P.z - p3.z) / max(p3.w - p3.z, 1e-3)));
    }
    return float4(1 - share * (1 - s), s, (c + 0.5) / 4, share);
}
)";

}  // namespace gpu
