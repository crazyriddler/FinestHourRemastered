#pragma once
// Passes on the 3D scene before the 2D over it (d3d12.cpp ScenePost): ambient occlusion and contact shadows from the
// depth, a bloom of what is brighter than white (the scene targets hold more than 1 with HDR), and the composite
// that darkens by the occlusion, adds the bloom and rolls the highlights back into range.
// Parameters (b1): p[0] view window xy, near, far; p[1] light dir in view space xyz, contact shadow length;
// p[2] size xy, 1/size xy; p[3] ao radius, ao strength, contact strength, bloom strength; the composite's p[4] the
// player's pain (gpu::g_hurt), seconds
namespace gpu {

inline constexpr char kSceneCommon[] = R"(
cbuffer P : register(b1) {
    float4 p[5]; float4 lcount; float4 lpos[32]; float4 lcol[32]; float4 heat[8]; float4 heatz[8];
    // the sky (d3d12.cpp SkyConstants): the view's rows in the world (w: on, seconds, cloud cover), the sun in view
    // space (w: shown), in the world (w: its strength), switches (clouds, sun)
    float4 sky[6];
    // d3d12.cpp Grade: exposure, saturation, on; the gain; the lift; the grain
    float4 grade[4];
};
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
// the sky's: a ray's direction in the world from a point of the screen
float3 SkyRay(float2 uv) {
    float3 v = normalize(float3((uv.x * 2 - 1) * p[0].x, (1 - uv.y * 2) * p[0].y, 1));
    return normalize(v.x * sky[0].xyz + v.y * sky[1].xyz + v.z * sky[2].xyz);
}
float Hash(float2 q) {
    float3 h = frac(float3(q.xyx) * 0.1031);
    h += dot(h, h.yzx + 33.33);
    return frac((h.x + h.y) * h.z);
}
float Noise(float2 q) {
    float2 i = floor(q), f = frac(q);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), f.x), lerp(Hash(i + float2(0, 1)), Hash(i + 1), f.x), f.y);
}
float Fbm(float2 q, int octaves) {
    float s = 0, a = 0.5;
    for (int k = 0; k < octaves; k++) {
        s += a * Noise(q);
        q = q * 2.03 + float2(1.7, 9.2);
        a *= 0.5;
    }
    return s;
}
// clouds on a plane high above, carried by the wind and changing shape as they go (the noise warped by another that
// drifts at its own pace): their thickness along a ray (0 to 1), thinning to the horizon
float Clouds(float3 w, int octaves) {
    if (w.y < 0.01) return 0;
    float t = sky[1].w;
    float2 q = w.xz / (w.y + 0.12) * 1.6 + t * float2(0.020, 0.007);
    float2 warp = float2(Fbm(q * 0.6 + t * float2(0.007, -0.0045), 3), Fbm(q * 0.6 + float2(5.2, 1.3) - t * float2(0.0056, 0.008), 3));
    q += (warp - 0.5) * 1.2;
    float n = Fbm(q, octaves) + 0.25 * Fbm(q * 0.31 + 7.3, 2) - 0.12;
    return saturate((n - (1 - sky[2].w)) / 0.22) * saturate(w.y * 5);
}
// the depth buffer's value to view space depth, a perspective's: d = far / (far - near) * (1 - near / z)
float ViewZ(float d) { float n = p[0].z, f = p[0].w; return n * f / max(f - d * (f - n), 1e-6); }
// Level 1 (sky[0].w 2): the sky's pieces at the far plane a layer each (d3d12.cpp sky_phase_, steps of 2^-21); from
// 16 on the column of oil smoke over the city: sky to the passes, but veiled (no sun, clouds or shafts through it)
float SkyLayer(float d) { return round((1 - d) / 4.76837158e-7); }
bool Veiled(float d) { return sky[0].w > 1.5 && d >= 0.9999 && SkyLayer(d) >= 15.5 && SkyLayer(d) < 19.5; }
// level 1's far water (d3d12.cpp far_water): the river's ring out to the horizon, at layer 20
bool FarWater(float d) { return sky[0].w > 1.5 && d >= 0.9999 && abs(SkyLayer(d) - 20) < 0.5; }
float3 ViewPos(float2 uv, float d) {
    float z = ViewZ(d);
    return float3((uv.x * 2 - 1) * p[0].x, (1 - uv.y * 2) * p[0].y, 1) * z;
}
)";

// t0 depth: occlusion in r, the contact shadow folded in; t2 the guard band (d3d12.cpp DrawGuard): the depth past the
// screen's edges, the view widened by 1 / GUARD (0 where there is none)
inline constexpr char kSceneAo[] = R"(
Texture2D<float> depth : register(t0);
Texture2D<float> guard : register(t2);
static const float GUARD = 0.6666667;
float D(float2 uv) { return depth.SampleLevel(pnt, uv, 0); }
// the depth at a screen uv, on the screen or past it in the guard band: false where neither has it
bool DG(float2 uv, out float d) {
    d = 1;
    if (all(uv >= 0) && all(uv <= 1)) {
        d = D(uv);
        return true;
    }
    float2 g = (uv - 0.5) * GUARD + 0.5;
    if (any(g < 0) || any(g > 1)) return false;
    d = guard.SampleLevel(pnt, g, 0);
    return d > 0;
}
float4 ao(O i) : SV_Target {
    float d = D(i.uv);
    if (d >= 0.99999) return 1;  // the sky
    float3 P = ViewPos(i.uv, d);
    float2 px = p[2].zw;
    // the normal from the nearer of each pair of neighbours, so edges keep their own side
    float3 l = ViewPos(i.uv - float2(px.x, 0), D(i.uv - float2(px.x, 0))), r = ViewPos(i.uv + float2(px.x, 0), D(i.uv + float2(px.x, 0)));
    float3 u = ViewPos(i.uv - float2(0, px.y), D(i.uv - float2(0, px.y))), dn = ViewPos(i.uv + float2(0, px.y), D(i.uv + float2(0, px.y)));
    float3 dx = abs(r.z - P.z) < abs(P.z - l.z) ? r - P : P - l;
    float3 dy = abs(dn.z - P.z) < abs(P.z - u.z) ? dn - P : P - u;
    float3 N = normalize(cross(dy, dx));
    if (dot(N, P) > 0) N = -N;
    // occlusion: a spiral of samples in a disc of p[3].x in the view
    float radius = p[3].x;
    float2 rs = radius / P.z / (2 * p[0].xy);  // in uv
    float noise = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))));
    // samples past the screen's edge from the guard band (not the edge's depth repeated there, at a place it is not:
    // it darkened or lit a corner by the angle it was seen from); past the band too, left out
    float occ = 0, taken = 0;
    const int n = 12;
    [unroll] for (int k = 0; k < n; k++) {
        float a = (k + noise) * 2.399963;
        float rr = sqrt((k + 0.5) / n);
        float2 uv = i.uv + float2(cos(a), sin(a)) * rr * rs;
        float ds;
        if (!DG(uv, ds)) continue;
        taken += 1;
        float3 S = ViewPos(uv, ds);
        float3 v = S - P;
        float vv = dot(v, v);
        float fall = saturate(1 - vv / (radius * radius * 4));
        // only what rises above the surface by more than about 12 degrees occludes: the facets of a low detail
        // model, seen in its depth as a curve of flat pieces, do not darken themselves
        occ += saturate(dot(v, N) * rsqrt(vv + 1e-6) - 0.2) * fall;
    }
    float vis = saturate(1 - p[3].y * 0.7 * occ / max(taken, 1));
    // contact shadow: a short march towards the light, shadowed where the depth buffer has something in front
    float3 L = p[1].xyz;
    float sh = 0;
    if (p[3].z > 0 && dot(N, L) > 0.02) {
        float len = p[1].w;
        float3 Q = P + N * 0.01 * P.z;
        const int m = 12;
        [loop] for (int k = 1; k <= m; k++) {
            float3 S = Q + L * len * (k + noise) / m;
            float2 uv = float2(S.x / (S.z * p[0].x) * 0.5 + 0.5, 0.5 - S.y / (S.z * p[0].y) * 0.5);
            float ds;
            if (!DG(uv, ds)) break;
            float z = ViewZ(ds);
            float t = S.z - z;
            if (t > 0.02 * len && t < 0.6 * len) { sh = 1 - float(k) / (m + 1); break; }
        }
    }
    vis *= 1 - sh * p[3].z * saturate(dot(N, L) * 4);
    return vis;
}
float4 ao_depth(O i) : SV_Target { float d = D(i.uv); return frac(d * 16); }
// XBE_SCENE_DEBUG=4: the guard band against the screen's depth where both have it (black: the same; white: 5% apart)
float4 ao_guard(O i) : SV_Target {
    float d = D(i.uv);
    if (d >= 0.99999) return 0.5;
    float g = guard.SampleLevel(pnt, (i.uv - 0.5) * GUARD + 0.5, 0);
    return saturate(abs(ViewZ(g) - ViewZ(d)) / (ViewZ(d) * 0.05));
}
// One bounce of light (half size): the colour of the lit surfaces about a point (16 samples over a disc of 2.5 m in the
// view, as the scene drew them) that face it and that it faces, by the cosines and their nearness. Out: rgb the light
// arriving, a 1.
Texture2D gscene : register(t1);
float4 gi(O i) : SV_Target {
    float d = D(i.uv);
    if (d >= 0.99999) return 0;
    float3 P = ViewPos(i.uv, d);
    float2 px = p[2].zw * 2;
    float3 l = ViewPos(i.uv - float2(px.x, 0), D(i.uv - float2(px.x, 0))), r = ViewPos(i.uv + float2(px.x, 0), D(i.uv + float2(px.x, 0)));
    float3 u = ViewPos(i.uv - float2(0, px.y), D(i.uv - float2(0, px.y))), dn = ViewPos(i.uv + float2(0, px.y), D(i.uv + float2(0, px.y)));
    float3 dx = abs(r.z - P.z) < abs(P.z - l.z) ? r - P : P - l;
    float3 dy = abs(dn.z - P.z) < abs(P.z - u.z) ? dn - P : P - u;
    float3 N = normalize(cross(dy, dx));
    if (dot(N, P) > 0) N = -N;
    float radius = 250;
    float2 rs = radius / P.z / (2 * p[0].xy);
    float noise = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))));
    float3 sum = 0;
    float wsum = 0;
    const int n = 16;
    [loop] for (int k = 0; k < n; k++) {
        float a = (k + noise) * 2.399963;
        float rr = sqrt((k + 0.5) / n);
        float2 uv = i.uv + float2(cos(a), sin(a)) * rr * rs;
        if (any(uv < 0) || any(uv > 1)) continue;
        float ds = D(uv);
        if (ds >= 0.99999) continue;  // the sky: the game's ambient has it
        float3 S = ViewPos(uv, ds);
        float3 v = S - P;
        float vv = dot(v, v);
        float dist = sqrt(vv);
        float3 w = v / max(dist, 1e-3);
        float cosr = dot(N, w);  // the receiver faces it
        if (cosr <= 0.05) continue;
        float fall = 1 / (1 + vv / (radius * radius * 0.25));
        sum += gscene.SampleLevel(lin, uv, 0).rgb * cosr * fall;
        wsum += 1;
    }
    return float4(wsum > 0 ? sum / n : 0, 1);
}
float4 ao_viewz(O i) : SV_Target { return frac(ViewZ(D(i.uv)) / 4); }
)";

// t0 occlusion, t1 depth: a 9 tap blur along p[2].zw scaled by the step in p[1].xy, weighted by depth
inline constexpr char kSceneBlur[] = R"(
Texture2D<float> occ : register(t0);
Texture2D<float> depth : register(t1);
float4 blur(O i) : SV_Target {
    float2 step = p[1].xy * p[2].zw * 2;
    float z0 = ViewZ(depth.SampleLevel(pnt, i.uv, 0));
    float sum = 0, wsum = 0;
    [unroll] for (int k = -4; k <= 4; k++) {
        float2 uv = i.uv + step * k;
        float z = ViewZ(depth.SampleLevel(pnt, uv, 0));
        float w = exp(-k * k / 12.0) * saturate(1 - abs(z - z0) / (0.05 * z0 + 1e-3));
        sum += occ.SampleLevel(pnt, uv, 0) * w;
        wsum += w;
    }
    return sum / max(wsum, 1e-4);
}
)";

// t0 the scene: bright pass at half size (what is over 0.85, weighted), 4 taps of a box
inline constexpr char kSceneBright[] = R"(
Texture2D scene : register(t0);
float4 bright(O i) : SV_Target {
    float2 h = p[2].zw * 0.5;
    float3 c = 0;
    c += scene.SampleLevel(lin, i.uv + float2(-h.x, -h.y), 0).rgb;
    c += scene.SampleLevel(lin, i.uv + float2(h.x, -h.y), 0).rgb;
    c += scene.SampleLevel(lin, i.uv + float2(-h.x, h.y), 0).rgb;
    c += scene.SampleLevel(lin, i.uv + float2(h.x, h.y), 0).rgb;
    c *= 0.25;
    float b = max(max(c.r, c.g), c.b);
    return float4(c * saturate((b - 0.85) * 2) / max(b, 1e-3) * max(b - 0.85, 0) * 3, 1);
}
)";

// Water Interaction's wave field (d3d12.cpp WaterInteraction): a square about the eye seen from above, a texel each
// few centimetres. The state (RGBA16F): the height now, the height a step before, the cover at the last step, 1 where
// wet. A step: the wave equation, damped, the field moved by q0.xy texels (it follows the eye), pushed where what
// crosses the water came or went (the cover: how far up the slab about the water's height the solid world reaches);
// dry texels hold nothing, waves end at the shores.
// q0: the move in texels, the speed (c dt / dx squared), the damping; q1: the push's gain, the size in texels, the
// most a step pushes (cm), the cover of something standing 10 cm out of the water
inline constexpr char kWaveHlsl[] = R"(
cbuffer P : register(b1) { float4 q0; float4 q1; float4 drops[8]; };
Texture2D<float4> state : register(t0);
Texture2D<float> water : register(t1);
Texture2D<float> cover : register(t2);
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
float H(int2 q, int n) {
    if (any(q < 0) || any(q >= n)) return 0;
    float4 s = state.Load(int3(q, 0));
    return s.a > 0 ? s.r : 0;
}
float4 wavestep(O i) : SV_Target {
    int n = (int)q1.y;
    int2 p = int2(i.pos.xy);
    int2 o = p + int2(q0.xy);
    // the cover softened over its neighbours (its edges are the rasterized hulls' steps)
    float c = 0;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++) c += cover.Load(int3(clamp(p + int2(x, y), 0, n - 1), 0)) * (x == 0 && y == 0 ? 4 : x == 0 || y == 0 ? 2 : 1);
    c /= 16;
    if (water.Load(int3(p, 0)) < -1e8) return float4(0, 0, c, 0);
    float4 s = all(o >= 0) && all(o < n) ? state.Load(int3(o, 0)) : 0;
    float lap = H(o + int2(1, 0), n) + H(o - int2(1, 0), n) + H(o + int2(0, 1), n) + H(o - int2(0, 1), n) - 4 * s.r;
    // a place just wet, or come into the field, starts still
    float push = s.a > 0 ? clamp((c - s.b) * q1.x, -q1.z, q1.z) : 0;
    // a hit landing on the water (drops: texel x, z, height, strength; the first step's): a ring from it
    [loop] for (int k = 0; k < 8; k++) {
        float4 d = drops[k];
        if (d.w <= 0) continue;
        float2 e = float2(p) + 0.5 - d.xy;
        float r2 = dot(e, e);
        if (r2 > 100) continue;
        int2 dt = clamp(int2(d.xy), 0, n - 1);
        float w = water.Load(int3(dt, 0));
        if (abs(w - d.z) < 40 && cover.Load(int3(dt, 0)) < q1.w) push += d.w * 2.5 * exp(-r2 / 8);
    }
    float h = s.r + (s.r - s.g) * q0.w + q0.z * lap - push;
    h = clamp(h * 0.9985, -12, 12);
    return float4(h, s.r, c, 1);
}
// what to splash (read back, d3d12.cpp WaterInteraction): rows 0..63 the field's 8 x 8 texel blocks (r how much
// the cover rose since the last step, on average; g the water's height), row 64 the drops (r 1 where one landed on
// water, g its height)
float4 waveevents(O i) : SV_Target {
    int n = (int)q1.y;
    int2 b = int2(i.pos.xy);
    if (b.y >= 64) {
        float4 d = drops[min(b.x, 7)];
        if (b.x >= 8 || d.w <= 0) return 0;
        int2 dt = clamp(int2(d.xy), 0, n - 1);
        float w = water.Load(int3(dt, 0));
        // on water, where nothing solid stands out of it (a hit on a boat's hull, the player sitting in one; a
        // puddle's floor under the water is no matter)
        return float4(w > -1e8 && abs(w - d.z) < 40 && cover.Load(int3(dt, 0)) < q1.w ? 1 : 0, w, 0, 0);
    }
    float gain = 0, wy = -1e9;
    [loop] for (int y = 0; y < 8; y++) {
        [loop] for (int x = 0; x < 8; x++) {
            int2 p = b * 8 + int2(x, y);
            float w = water.Load(int3(p, 0));
            if (w < -1e8) continue;
            wy = max(wy, w);
            int2 o = p + int2(q0.xy);
            float4 s = all(o >= 0) && all(o < n) ? state.Load(int3(o, 0)) : 0;
            if (s.a > 0) gain += max(cover.Load(int3(p, 0)) - s.b, 0);
        }
    }
    return float4(gain / 64, wy, 0, 0);
}
)";

// Water Interaction's splashes (d3d12.cpp DrawSplashes): each a camera facing square from the constants (its place
// and size; its opacity, kind, stretch upwards, seed), round drops or a soft mist, lit as the scene behind them,
// faded where they meet it, hidden behind the weapon in hand. Premultiplied.
inline constexpr char kSplashHlsl[] = R"(
cbuffer P : register(b1) { row_major float4x4 vpm; float4 right; float4 up; float4 info; float4 pt[4000]; };
Texture2D<float> wdepth : register(t0);
Texture2D<float> ddepth : register(t1);
Texture2D scene : register(t2);
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 d : TEXCOORD1; };
O vs(uint id : SV_VertexID, uint inst : SV_InstanceID) {
    static const float2 corner[6] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1)};
    float4 a = pt[inst * 2], b = pt[inst * 2 + 1];
    float2 c = corner[id];
    float3 w = a.xyz + (right.xyz * c.x + up.xyz * c.y * (1 + b.z)) * a.w;
    O o;
    o.pos = mul(float4(w, 1), vpm);
    o.uv = c;
    o.d = float4(b.x, o.pos.w, b.y, b.w);
    return o;
}
// the depth buffer's value to view depth (info.x P22, info.y P32; the viewport's depth range in right.w, up.w)
float ViewDepth(float d) { d = (d - right.w) / max(up.w - right.w, 1e-6); return info.y / min(d - info.x, -1e-6); }
float4 splashps(O i) : SV_Target {
    if (info.w > 5) return float4(1, 0, 0, 1);  // tests (XBE_SPLASH_RED)
    float r2 = dot(i.uv, i.uv);
    if (r2 > 1) discard;
    int2 p = int2(i.pos.xy);
    // the depth: the weapon in hand's where it is drawn (the depth now), else the world's
    float dn = ddepth.Load(int3(p, 0));
    float zb = ViewDepth(info.w > 0 && dn < 1 ? dn : wdepth.Load(int3(p, 0)));
    float soft = saturate((zb - i.d.y) / 25);
    float3 bg = scene.Load(int3(p, 0)).rgb;
    float a;
    float3 col;
    if (i.d.z < 0.5) {
        // a drop: a bright rim and core over what is behind (water catches the light)
        a = smoothstep(1, 0.3, r2) * 0.7;
        col = bg * 1.3 + info.z * (1.1 - 0.6 * r2);
    } else {
        // mist: soft, broken by its seed
        float n = sin(i.uv.x * 5.1 + i.d.w * 17) * sin(i.uv.y * 4.3 + i.d.w * 31);
        a = (1 - r2) * (1 - r2) * (0.55 + 0.45 * n) * 0.45;
        col = bg * 1.2 + info.z * 0.8;
    }
    a *= saturate(i.d.x) * soft;
    return float4(col * a, a);
}
)";

// Snow Tracks' trail (d3d12.cpp SnowTracks): a square about the eye seen from above, a texel each 2.5 cm, kept: r how
// far the snow is pressed in (under 0 its rim pushed up), g the height of the step that pressed it. A pass moves it
// by q0.xy texels (it follows the eye) and presses the steps in (s: their texel x, z, height, the foot; then the way
// it points x, z). q0: the move, the size in texels, the steps
inline constexpr char kTrailHlsl[] = R"(
cbuffer P : register(b1) { float4 q0; float4 q1; float4 s[64]; };
Texture2D<float2> trail : register(t0);
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
// a boot's print in cm about its ankle (u along the foot, v across): a sole's outline (a rounded toe at +20, the
// ball 5.5 wide either side, a narrow waist under the arch, the heel 4.5 either side ending at -15), pressed in,
// the arch less deep than the ball and the heel; a rim of snow pushed up about it
float BootHalfWidth(float u) {
    float toe = 5.5 * sqrt(saturate(1 - pow((u - 8) / 12, 2)));
    float heel = 4.5 * sqrt(saturate(1 - pow((u + 10) / 5, 2)));
    float waist = u < -3 ? lerp(4.5, 3.6, smoothstep(-10, -3, u)) : lerp(3.6, 5.5, smoothstep(-3, 8, u));
    return u > 8 ? toe : u < -10 ? heel : waist;
}
float Boot(float u, float v) {
    float hw = BootHalfWidth(clamp(u, -14.8, 19.8));
    float r = max(abs(v) / max(hw, 0.6), u > 19.8 ? 1 + (u - 19.8) / 3 : u < -14.8 ? 1 + (-14.8 - u) / 3 : 0);
    float arch = 1 - 0.35 * smoothstep(-9, -4, u) * (1 - smoothstep(-1, 4, u));
    return r < 1 ? (1 - pow(r, 6)) * arch : -0.35 * saturate(1 - (r - 1) * 3);
}
float2 trailstep(O i) : SV_Target {
    int n = (int)q0.z;
    int2 p = int2(i.pos.xy);
    int2 o = p + int2(q0.xy);
    float2 t = all(o >= 0) && all(o < n) ? trail.Load(int3(o, 0)) : 0;
    [loop] for (int k = 0; k < (int)q0.w; k++) {
        float4 a = s[k * 2], b = s[k * 2 + 1];
        float2 e = (float2(p) + 0.5 - a.xy) * 2.5;  // cm
        if (dot(e, e) > 30 * 30) continue;
        float u = dot(e, b.xy), v = dot(e, float2(-b.y, b.x));
        float d = Boot(u, v);
        if (d > 0) t = float2(max(t.r, d), a.z);
        else if (t.r <= 0 && d < t.r) t = float2(d, a.z);
    }
    return t;
}
)";

// Level 1's sky tiles (d3d12.cpp SkyPass): a texel for each 16 x 16 pixels, r where any of them is the oil smoke's
// shells (Veiled), g where any is the far water; then the same spread over the reach of the passes that read them
// (SkyFeather's disc, HorizonSoft's band and disc): elsewhere skyfx leaves those out, they would change nothing there.
// q0: the screen's width and height, the tiles' width and height; q1: the reaches in tiles (smoke, water)
inline constexpr char kSkyTilesHlsl[] = R"(
cbuffer P : register(b1) { float4 q0; float4 q1; };
Texture2D<float> depth : register(t0);
Texture2D<float2> tiles : register(t0);
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
float Layer(float d) { return round((1 - d) / 4.76837158e-7); }
float2 skytiles(O i) : SV_Target {
    int2 at = int2(i.pos.xy) * 16;
    float veil = 0, water = 0;
    [loop] for (int y = 0; y < 16; y++) {
        [loop] for (int x = 0; x < 16; x++) {
            int2 q = at + int2(x, y);
            if (q.x >= (int)q0.x || q.y >= (int)q0.y) continue;
            float d = depth.Load(int3(q, 0));
            if (d < 0.9999) continue;
            float l = Layer(d);
            veil = max(veil, l >= 15.5 && l < 19.5 ? 1 : 0);
            water = max(water, abs(l - 20) < 0.5 ? 1 : 0);
        }
    }
    return float2(veil, water);
}
float2 skydilate(O i) : SV_Target {
    int2 at = int2(i.pos.xy);
    int rv = (int)q1.x, rw = (int)q1.y, r = max(rv, rw);
    float veil = 0, water = 0;
    [loop] for (int y = -r; y <= r; y++) {
        [loop] for (int x = -r; x <= r; x++) {
            int2 q = at + int2(x, y);
            if (any(q < 0) || q.x >= (int)q0.z || q.y >= (int)q0.w) continue;
            float2 t = tiles.Load(int3(q, 0));
            if (abs(x) <= rv && abs(y) <= rv) veil = max(veil, t.x);
            if (abs(x) <= rw && abs(y) <= rw) water = max(water, t.y);
        }
    }
    return float2(veil, water);
}
)";

// t0 the scene, t1 the world's depth: on the sky (beyond most of the far plane), the clouds, then the sun by day or
// the moon by night, before the bloom (the sun's own glare). Day or night by the level's own sky about each point;
// the clouds take their colour from it and their light from the sun.
inline constexpr char kSceneSkyHlsl[] = R"(
Texture2D scene : register(t0);
Texture2D<float> depth : register(t1);
Texture2D<float2> skytiles : register(t2);  // level 1: kSkyTilesHlsl's, where the smoke (r) and the far water (g) are near
// Level 1 (sky[0].w 2): the sky's pieces each at a depth of its own by the far plane (d3d12.cpp sky_phase_, a step
// of 2^-21 apart), the oil smoke's shells veiled. About a pixel (a wide disc), the smoke's and the open sky's colours
// apart. Where the shells meet each other their colours mix. Over the smoke's outline against the open sky a thick
// billowing smoke of the shells' own colour, rising and churning (a 3D noise in the world's directions, so it stays
// where it is as the view turns), out over the sky and in over the shell's edge: the polygons are under it. Inside,
// the shell churns a little too. veil: how much smoke is at the pixel (the clouds and the sun behind it)
float Hash3(float3 q) {
    q = frac(q * 0.1031);
    q += dot(q, q.zyx + 31.32);
    return frac((q.x + q.y) * q.z);
}
float Noise3(float3 x) {
    float3 i = floor(x), f = frac(x);
    f = f * f * (3 - 2 * f);
    float a = lerp(lerp(Hash3(i), Hash3(i + float3(1, 0, 0)), f.x), lerp(Hash3(i + float3(0, 1, 0)), Hash3(i + float3(1, 1, 0)), f.x), f.y);
    float b = lerp(lerp(Hash3(i + float3(0, 0, 1)), Hash3(i + float3(1, 0, 1)), f.x), lerp(Hash3(i + float3(0, 1, 1)), Hash3(i + 1), f.x), f.y);
    return lerp(a, b, f.z);
}
float Fbm3(float3 q, int octaves) {
    float s = 0, a = 0.5;
    for (int k = 0; k < octaves; k++) {
        s += a * Noise3(q);
        q = q * 2.02 + float3(1.7, 9.2, 4.1);
        a *= 0.5;
    }
    return s;
}
// the smoke's density at a direction: billows (warped, rising, churning), 0 to 1
float Billow(float3 w, float t) {
    float3 q = w * 9 + float3(0, -t * 0.045, 0);
    float3 warp = float3(Fbm3(q * 0.7 + float3(0, 0, t * 0.03), 3), Fbm3(q * 0.7 + float3(5.2, 1.3, -t * 0.025), 3),
                         Fbm3(q * 0.7 + float3(2.9, 7.1, t * 0.02), 3));
    float n = Fbm3(q + (warp - 0.5) * 2.2, 5);
    return saturate((n - 0.28) * 2.2);
}
float3 SkyFeather(O i, float3 c, float d0, out float veil) {
    bool v0 = Veiled(d0);
    float l0 = SkyLayer(d0);
    veil = v0 ? 1 : 0;
    float R = 0.17 * p[2].y;
    // (the disc's turn each pixel's, and each frame's: the anti-aliasing averages the samples' grain out)
    float frame = floor(sky[1].w * 60);
    float noise = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))) + frame * 0.618034);
    float3 accs = 0;
    float ns = 0, nk = 0, shells = 0, ws = 0;
    [loop] for (int k = 0; k < 64; k++) {
        float a = (k + noise) * 2.399963;
        float2 q = i.pos.xy + float2(cos(a), sin(a)) * sqrt((k + 0.5) / 64) * R;
        if (any(q < 0) || any(q >= p[2].xy)) continue;
        float d = depth.Load(int3(q, 0));
        if (d < 0.9999) continue;  // the world before the sky
        [branch] if (Veiled(d)) {
            accs += scene.Load(int3(q, 0)).rgb, ns += 1;
            // the smoke to a pixel's left counts more: its billows reach further out on the column's right side
            ws += 1 + 2 * saturate((i.pos.x - q.x) / R);
            shells += v0 && SkyLayer(d) != l0 ? 1 : 0;
        } else {
            nk += 1;
        }
    }
    if (ns + nk < 4) return c;
    // the shells' own outlines over each other
    if (v0 && ns > 0) c = lerp(c, accs / ns, smoothstep(0, 0.45, shells / ns));
    float3 w = SkyRay(i.uv);
    float t = sky[1].w;
    float share = ws / (ws + nk);
    [branch] if (ns > 0) {
        float b = Billow(w, t);
        // the billows over the outline: dense from the edge in, thinning out over the sky in puffs
        float edge = saturate(4 * share * (1 - share));
        // (the puffs fade out with the smoke about: none at the disc's reach, no line along it)
        float reach = saturate(share * 8);
        float dens = smoothstep(0.05, 0.42, share + (b - 0.5) * 0.55 * reach);
        dens = saturate(dens * (0.75 + 0.6 * b) + edge * 0.35) * reach;
        if (v0) dens = max(dens, edge);  // the shell's own edge under them
        // their colour: the shells' about, darker in their cores, their thin ends a little lighter
        float3 smoke = accs / ns * lerp(1.18, 0.72, b);
        c = lerp(c, smoke, dens);
        veil = v0 ? 1 : dens;
    }
    // inside, the shell churning a little
    [branch] if (v0) c *= 0.88 + 0.24 * Billow(w * 1.6 + 3.1, t * 1.3);
    return c;
}
// Level 1's far water where it meets the sky (its square's straight edge): a band about that line out of focus, the
// most at the line, fading out either side; only the far water and the sky are mixed (not the boats or what is
// nearer). The far water: the river's ring drawn at the sky's layer 20, or the world past a fifth of the far plane.
// Only looking out to the sea (the world's +x, away from the city: from the river its skyline stays sharp).
// False away from it.
bool FarPix(int2 q) {
    float d = depth.Load(int3(q, 0));
    return FarWater(d) || (d < 0.9999 && ViewZ(d) > p[0].w * 0.22);
}
bool HorizonSoft(O i, float d0, out float3 c) {
    c = 0;
    float B = 0.03 * p[2].y, R = 0.014 * p[2].y;
    float frame = floor(sky[1].w * 60);
    float noise = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))) + frame * 0.618034);
    float fw = 0, fs = 0;
    [loop] for (int k = 0; k < 20; k++) {
        float a = (k + noise) * 2.399963;
        float2 q = clamp(i.pos.xy + float2(cos(a), sin(a)) * sqrt((k + 0.5) / 20) * B, 0, p[2].xy - 1);
        float d = depth.Load(int3(q, 0));
        if (d >= 0.9999 && !FarWater(d)) fs += 1;
        else if (FarPix(int2(q))) fw += 1;
    }
    if (fw < 0.5 || fs < 0.5) return false;
    float f = fw / (fw + fs);
    float w = pow(saturate(4 * f * (1 - f)), 0.6);
    float3 acc = 0;
    float n = 0;
    [loop] for (int k = 0; k < 24; k++) {
        float a = (k + noise) * 2.399963;
        float2 q = clamp(i.pos.xy + float2(cos(a), sin(a)) * sqrt((k + 0.5) / 24) * R * (0.4 + 0.6 * w), 0, p[2].xy - 1);
        float d = depth.Load(int3(q, 0));
        if (d < 0.9999 && !FarPix(int2(q))) continue;  // (the world before it: not mixed in)
        acc += scene.Load(int3(q, 0)).rgb, n += 1;
    }
    if (n < 1) return false;
    c = lerp(scene.Load(int3(i.pos.xy, 0)).rgb, acc / n, w);
    return true;
}
float4 skyfx(O i) : SV_Target {
    float4 s = scene.Load(int3(i.pos.xy, 0));
    float d0 = depth.Load(int3(i.pos.xy, 0));
    float2 near = sky[0].w > 1.5 ? skytiles.Load(int3(int2(i.pos.xy) >> 4, 0)) : 0;
    // level 1's far water: its edge against the sky soft, the rest as it is (no clouds or sun on it; all of it out of
    // focus made the beach's view blurred)
    [branch] if (sky[0].w > 1.5 && 1.0 * SkyRay(i.uv).x > 0.2 && (d0 >= 0.9999 || FarPix(int2(i.pos.xy)))) {
        float3 soft;
        if (HorizonSoft(i, d0, soft)) return float4(soft, s.a);
    }
    if (FarWater(d0)) return s;
    if (sky[0].w > 2.5) {
        // XBE_SKY_LAYERS: the oil smoke red, the open sky green (darker by its layer), the world blue
        if (d0 < 0.9999) return float4(0, 0, 0.6, 1);
        return Veiled(d0) ? float4(1, 0, 0, 1) : float4(0, 1 - SkyLayer(d0) * 0.12, 0, 1);
    }
    // a crack: a pixel that sees much further than the world on both sides of it, the sides about as far as each
    // other, across or along or aslant (the seams between the models: the sky or a far wall through them, a dotted
    // bright line along a roof's edge), takes the colour of those sides
    {
        int2 P = int2(i.pos.xy), last = int2(p[2].xy) - 1;
        float z0 = ViewZ(d0);
        float3 fill = 0;
        bool crack = false;
        [unroll] for (int dk = 0; dk < 4; dk++) {
            int2 dir = dk == 0 ? int2(1, 0) : dk == 1 ? int2(0, 1) : dk == 2 ? int2(1, 1) : int2(1, -1);
            int2 qa = clamp(P - dir, 0, last), qb = clamp(P + dir, 0, last);
            float za = ViewZ(depth.Load(int3(qa, 0))), zb = ViewZ(depth.Load(int3(qb, 0)));
            bool seam = max(za, zb) * 1.5 < z0 && abs(za - zb) < 0.15 * min(za, zb);
            fill = !crack && seam ? (scene.Load(int3(qa, 0)).rgb + scene.Load(int3(qb, 0)).rgb) * 0.5 : fill;
            crack = crack || seam;
        }
        if (crack) return float4(fill, s.a);
    }
    // (the world: left as it is, the target already holds it, scene being its copy)
    if (ViewZ(d0) < p[0].w * 0.6) discard;
    float3 c = s.rgb, w = SkyRay(i.uv), sun = sky[4].xyz;
    // level 1: the pieces' outlines melted; the oil smoke's share about (veil) keeps the clouds and the sun behind it
    float veil = 0;
    [branch] if (sky[0].w > 1.5 && d0 >= 0.9999 && near.x > 0) c = SkyFeather(i, c, d0, veil);
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    // the pixel's own brightness, by night never day (the level's light, sky[5].z: its brighter pieces stay night)
    float day = smoothstep(0.07, 0.25, lum) * (sky[5].z >= 2 ? sky[5].z - 2 : sky[5].z);
    float strength = sky[4].w;
    float cloud = 0;
    if (sky[5].x > 0) {
        cloud = Clouds(w, 5);
        if (cloud > 0) {
            // thicker towards the sun: that side in its own shade; the light through the edges facing the sun
            float toward = Clouds(normalize(w + sun * 0.08), 3);
            float lit = saturate(1 - (toward - cloud) * 2.5 - cloud * 0.35);
            // (by night the moon's grey on them)
            float3 base = lerp(c, lum.xxx * 1.3 + 0.015, 0.7) + float3(0.022, 0.025, 0.032) * (1 - day);
            float3 light = lerp(float3(0.55, 0.62, 0.8) * 0.2, float3(1, 0.9, 0.75), day) * strength;
            float edge = pow(saturate(dot(w, sun)), 8) * (1 - cloud);
            float3 cc = base * (0.72 + 0.5 * lit) + light * (lit * 0.22 + edge * 0.8) * (0.25 + 0.75 * day);
            c = lerp(c, cc, cloud * 0.92 * (1 - veil));
        }
    }
    // a moon of the level's own sky about the light (some night skies have one): not a second
    bool own_moon = false;
    if (day < 0.5 && sky[5].y > 0 && dot(w, sun) > 0.99) {
        float3 r = normalize(cross(sun, float3(0, 1, 0.001))), u = cross(r, sun);
        // (as far as 20 degrees off: level 14's crescent stands well away from its light)
        for (int k = 0; k < 96 && !own_moon; k++) {
            float ang = (k % 12) * 0.5236 + (k / 12) * 0.26, rad = 0.015 + 0.34 * (k / 12) / 7.0;
            float3 d = normalize(sun + (r * cos(ang) + u * sin(ang)) * rad);
            float3 v = float3(dot(d, sky[0].xyz), dot(d, sky[1].xyz), dot(d, sky[2].xyz));
            if (v.z <= 0) continue;
            float2 q = float2(v.x / v.z / p[0].x * 0.5 + 0.5, 0.5 - v.y / v.z / p[0].y * 0.5);
            if (any(q < 0) || any(q > 1)) continue;
            float3 o = scene.SampleLevel(pnt, q, 0).rgb;
            own_moon = dot(o, float3(0.2126, 0.7152, 0.0722)) > 0.22 && ViewZ(depth.SampleLevel(pnt, q, 0)) > p[0].w * 0.6;
        }
    }
    if (sky[5].y > 0 && sky[3].w > 0 && !own_moon) {
        float a = dot(w, sun);
        // the sun: a white hot disc (past white: the bloom's glare) and its halo; the moon: larger, pale, its face
        // mottled
        float disc = smoothstep(0.99990, 0.99994, a);
        float halo = exp((a - 1) * 3000) * 1.5 + exp((a - 1) * 150) * 0.22;
        float3 sunlight = float3(1, 0.94, 0.84) * (disc * 14 + halo) * strength;
        float moon = smoothstep(0.99983, 0.99986, a);
        float3 m = w - sun * a;
        float face = 0.72 + 0.28 * Fbm(float2(dot(m, sky[0].xyz), dot(m, sky[1].xyz)) * 1800, 3);
        float3 moonlight = float3(0.82, 0.87, 1) * (moon * face * 1.4 + exp((a - 1) * 2500) * 0.25 + exp((a - 1) * 180) * 0.04);
        c += lerp(moonlight, sunlight, day) * (1 - cloud * 0.92) * (1 - veil);
    }
    return float4(c, s.a);
}
)";

// The reflections. t0 the scene, t1 the depth now, t2 the reflection buffer (the materials': octahedral normal, view
// depth, reflectivity and roughness, metal by the sign), t3 the world's depth. A pixel whose buffer is its own (the
// depths agree: nothing else drawn over it) reflects along its normal: the ray walks the screen through the world's
// depth, longer steps further on, then narrows onto what it met; what it meets is the reflection, the sky (from the
// screen's own, about the level's top) where it leaves the screen or runs towards the eye. Fresnel: little head on,
// more at a grazing angle; metal tints what it reflects with its colour and takes its place, the rest add it.
inline constexpr char kSceneSsrHlsl[] = R"(
Texture2D scene : register(t0);
Texture2D<float> depthnow : register(t1);
Texture2D gb : register(t2);
Texture2D<float> world : register(t3);
Texture2D<float> aoc : register(t4);
float3 OctDecode(float2 e) {
    float3 n = float3(e, 1 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1 - abs(n.yx)) * (n.xy >= 0 ? 1 : -1);
    return normalize(n);
}
float2 ToScreen(float3 X) { return float2(X.x / X.z / p[0].x * 0.5 + 0.5, 0.5 - X.y / X.z / p[0].y * 0.5); }
// the sky as the level draws it: what the screen shows of it, top and sides
float3 SkyColour(float3 R) {
    float3 acc = 0;
    float n = 0;
    [unroll] for (int k = 0; k < 12; k++) {
        float2 q = float2((k % 4 + 0.5) / 4, (k / 4 + 0.5) / 3 * 0.45);
        if (ViewZ(world.SampleLevel(pnt, q, 0)) > p[0].w * 0.6) acc += scene.SampleLevel(lin, q, 0).rgb, n++;
    }
    float3 sky = n > 0 ? acc / n : float3(0.35, 0.4, 0.45) * 0.4;
    return sky * saturate(R.y * 1.5 + 0.6);
}
float4 ssr(O i) : SV_Target {
    int3 px = int3(i.pos.xy, 0);
    float4 s = scene.Load(px);
    float4 g = gb.Load(px);
    // the depth: the weapon in hand's where it is drawn (the depth now), else the world's
    float dn = depthnow.Load(px);
    float d = dn < 1 ? dn : world.Load(px);
    float z = ViewZ(d);
    // 4: the buffer as written: reflectivity code red, its depth green, the depth now blue (both /5000)
    if (sky[5].w == 4) return float4(abs(g.w) / 64, g.z / 5000, z / 5000, 1);
    if (g.w == 0) return s;
    if (abs(z - g.z) > max(3, z * 0.01)) return s;
    float code = abs(g.w);
    float refl = floor(code) / 63, rough = saturate(frac(code) / 0.95);
    bool metal = g.w < 0;
    if (refl < 0.01) return s;
    float3 N = OctDecode(g.xy);
    float3 P = ViewPos(i.uv, d);
    float3 V = normalize(-P);
    float3 R = reflect(-V, N);
    float ndv = saturate(dot(N, V));
    // a broad Fresnel (the game's surfaces are rough, and a sheen at a grazing angle is what reads)
    float f0 = metal ? 0.7 : 0.15;
    float F = f0 + (1 - f0) * pow(1 - ndv, 3);
    float amount = saturate(refl * F * 2);
    if (amount < 0.015) return s;
    // fewer steps where the reflection is faint
    int steps = (int)lerp(14, 40, saturate(amount * 3));
    // the walk: from just off the surface, a step of 1.5% of the depth growing 18% each, up to 40 m
    float3 hitc = 0;
    float found = 0;
    if (R.z > -0.2) {
        float3 X = P + N * (2 + z * 0.002);
        float step = max(z * 0.015, 4);
        float dither = Hash(i.pos.xy) * 0.5 + 0.5;
        float3 prev = X;
        float grow = steps < 40 ? 1.3 : 1.18;
        [loop] for (int k = 0; k < steps; k++) {
            prev = X;
            X += R * step * (k == 0 ? dither : 1);
            step *= grow;
            if (X.z < 1 || length(X - P) > 4000) break;
            float2 q = ToScreen(X);
            if (any(q < 0) || any(q > 1)) break;
            float zs = ViewZ(world.SampleLevel(pnt, q, 0));
            float gap = X.z - zs;
            // nothing by the eye itself is reflected (the gun in hand or mounted, the hands): seen from the far
            // ground it threw a dark, blocky copy of the gun and its flash down the line of fire
            if (zs < 150) break;
            if (gap > 0 && gap < max(step * 1.5, 40)) {
                // narrowed onto the surface met
                float3 a = prev, b = X;
                [unroll] for (int r = 0; r < 5; r++) {
                    float3 m = (a + b) * 0.5;
                    float2 mq = ToScreen(m);
                    if (m.z - ViewZ(world.SampleLevel(pnt, mq, 0)) > 0) b = m; else a = m;
                }
                float2 hq = ToScreen(b);
                // blurrier the rougher and the further
                float2 o = rough * 0.012 * float2(p[2].y / p[2].x, 1) * (1 + length(b - P) * 0.002);
                hitc = (scene.SampleLevel(lin, hq, 0).rgb * 2 + scene.SampleLevel(lin, hq + o, 0).rgb +
                        scene.SampleLevel(lin, hq - o, 0).rgb + scene.SampleLevel(lin, hq + float2(o.x, -o.y), 0).rgb +
                        scene.SampleLevel(lin, hq - float2(o.x, -o.y), 0).rgb) / 6;
                // faded at the screen's edges and with the way it went
                float2 e = min(hq, 1 - hq);
                found = saturate(min(e.x, e.y) * 12) * saturate(1 - length(b - P) / 4000);
                break;
            }
        }
    }
    float3 Rw = R.x * sky[0].xyz + R.y * sky[1].xyz + R.z * sky[2].xyz;
    // a surface the game left dark (a room's floor, its walls) sees no sky and little of the bright outside: its
    // reflection fades with its own light (else a dark room shone with the sky and the street, and as the camera
    // tilted and the reflected rays left the screen it went dark again)
    float lit = smoothstep(0.015, 0.15, dot(s.rgb, float3(0.299, 0.587, 0.114)));
    amount *= lerp(0.2, 1, lit);
    // specular occlusion: corners and creases (the AO) see little of the scene to reflect, less the rougher
    float ao = aoc.SampleLevel(lin, i.uv, 0);
    amount *= saturate(pow(ao, 1.5 + rough * 2) * 1.1 - 0.05);
    float3 env = lerp(SkyColour(Rw) * lit, hitc, found);
    // XBE_SSR_DEBUG: 1 what is reflected and how much, 2 reflectivity (red) and metal (green), 3 found on the screen
    if (sky[5].w == 1) return float4(env * amount * 3, 1);
    if (sky[5].w == 2) return float4(refl, metal ? 1 : 0, 0, 1);
    if (sky[5].w == 3) return float4(found, 0, amount, 1);
    float3 c = s.rgb;
    if (metal) {
        float3 tint = c / max(max(max(c.r, c.g), c.b), 0.05);
        c = lerp(c, env * lerp(1, tint, 0.6) * 1.2, min(amount, 0.6));
    } else {
        c += env * amount;
    }
    return float4(c, s.a);
}
)";

// t0 the scene, t1 the world's depth: the sun's shafts (p[1]: the sun on the screen, strength, aspect), at half size.
// From each pixel towards the sun, the bright sky it crosses, fading with the distance along and from the sun.
inline constexpr char kSceneRaysHlsl[] = R"(
Texture2D scene : register(t0);
Texture2D<float> depth : register(t1);
float4 rays(O i) : SV_Target {
    float2 sun = p[1].xy;
    const int n = 40;
    float2 st = (sun - i.uv) / n;
    float2 uv = i.uv;
    float3 acc = 0;
    float w = 1;
    for (int k = 0; k < n; k++) {
        uv += st;
        if (any(uv < 0) || any(uv > 1)) break;
        // the sky: the dome's depth by the far plane (by 0.99999 a 20 cm near plane leaves it just short of 1)
        float dk = depth.SampleLevel(pnt, uv, 0);
        if (ViewZ(dk) > p[0].w * 0.6 && !Veiled(dk)) {
            // the sky, brightest about the sun (its disc and glare, which the sky's picture may not have)
            float3 c = scene.SampleLevel(lin, uv, 0).rgb;
            // (the sun is drawn in the sky now, skyfx: its own brightness feeds the shafts)
            c = min(c, 1.5);  // the sun's disc no brighter than its glare: no streak along the sky's one open side
            acc += c * saturate(max(max(c.r, c.g), c.b) - 0.3) * 0.5 * w;
        }
        w *= 0.97;
    }
    float fall = saturate(1 - length((i.uv - sun) * float2(p[1].w, 1)) * 0.6);
    return float4(acc / n * p[1].z * fall * fall * 2.0, 1);
}
)";

// t0 the scene: blurred for aiming's depth of field, at half size (a disc of 12 taps)
inline constexpr char kSceneDofHlsl[] = R"(
Texture2D scene : register(t0);
float4 dofblur(O i) : SV_Target {
    static const float2 disc[12] = {float2(0.0, 1.0), float2(0.87, 0.5), float2(0.87, -0.5), float2(0.0, -1.0),
                                    float2(-0.87, -0.5), float2(-0.87, 0.5), float2(0.0, 0.5), float2(0.43, 0.25),
                                    float2(0.43, -0.25), float2(0.0, -0.5), float2(-0.43, -0.25), float2(-0.43, 0.25)};
    float3 c = scene.SampleLevel(lin, i.uv, 0).rgb;
    for (int k = 0; k < 12; k++) c += scene.SampleLevel(lin, i.uv + disc[k] * p[2].zw * 6, 0).rgb;
    return float4(c / 13, 1);
}
)";

// t0 a smaller level: its tent filtered upsample (p[2].zw its texel), blended over the larger one
inline constexpr char kSceneUp[] = R"(
Texture2D src : register(t0);
float4 up(O i) : SV_Target {
    float2 t = p[2].zw;
    float3 c = src.SampleLevel(lin, i.uv, 0).rgb * 4;
    c += (src.SampleLevel(lin, i.uv + float2(-t.x, 0), 0).rgb + src.SampleLevel(lin, i.uv + float2(t.x, 0), 0).rgb +
          src.SampleLevel(lin, i.uv + float2(0, -t.y), 0).rgb + src.SampleLevel(lin, i.uv + float2(0, t.y), 0).rgb) * 2;
    c += src.SampleLevel(lin, i.uv - t, 0).rgb + src.SampleLevel(lin, i.uv + t, 0).rgb +
         src.SampleLevel(lin, i.uv + float2(-t.x, t.y), 0).rgb + src.SampleLevel(lin, i.uv + float2(t.x, -t.y), 0).rgb;
    return float4(c / 16, 1);
}
)";

// t0 a level: the next smaller, a 13 tap downsample (p[2].zw its source texel)
inline constexpr char kSceneDown[] = R"(
Texture2D src : register(t0);
float4 down(O i) : SV_Target {
    float2 t = p[2].zw;
    float3 c = src.SampleLevel(lin, i.uv, 0).rgb * 0.125;
    c += (src.SampleLevel(lin, i.uv + t * float2(-1, -1), 0).rgb + src.SampleLevel(lin, i.uv + t * float2(1, -1), 0).rgb +
          src.SampleLevel(lin, i.uv + t * float2(-1, 1), 0).rgb + src.SampleLevel(lin, i.uv + t * float2(1, 1), 0).rgb) * 0.125;
    c += (src.SampleLevel(lin, i.uv + t * float2(-2, 0), 0).rgb + src.SampleLevel(lin, i.uv + t * float2(2, 0), 0).rgb +
          src.SampleLevel(lin, i.uv + t * float2(0, -2), 0).rgb + src.SampleLevel(lin, i.uv + t * float2(0, 2), 0).rgb) * 0.0625;
    c += (src.SampleLevel(lin, i.uv + t * float2(-2, -2), 0).rgb + src.SampleLevel(lin, i.uv + t * float2(2, -2), 0).rgb +
          src.SampleLevel(lin, i.uv + t * float2(-2, 2), 0).rgb + src.SampleLevel(lin, i.uv + t * float2(2, 2), 0).rgb) * 0.03125;
    return float4(c, 1);
}
)";

// t0 the scene, t1 the occlusion, t2 to t5 the bloom levels: the scene as it is drawn on
inline constexpr char kSceneComposite[] = R"(
Texture2D scene : register(t0);
Texture2D<float> occ : register(t1);
Texture2D b0 : register(t2);
Texture2D b1 : register(t3);
Texture2D b2 : register(t4);
Texture2D b3 : register(t5);
// shadow_shaders.h's mask: r the multiplier, g the shadow, b the cascade, a the share (the atlas for shadow_atlas)
Texture2D shadowm : register(t6);
// the clouds' shadows (grade[3].y on, the world's depth at t2): the way from a point towards the sun meets the clouds'
// layer; how thick they are that way takes the sun's share of its light (the mask: g lit, a the share), on
// the sunlit ground alone, never in a room or a shadow
float CloudShade(O i, float4 m) {
    if (grade[3].y <= 0 || sky[5].x <= 0) return m.r;
    float d = b0.Load(int3(i.pos.xy, 0)).r;
    float3 L = sky[4].xyz;
    if (d >= 0.99999 || L.y < 0.05 || (m.a <= 0.002 && grade[3].z <= 0)) return m.r;
    float3 P = ViewPos(i.uv, d);
    // the point in the world (the camera's rotation, then its place: grade[1].xyz in this pass): the clouds' layer's
    // pattern at its place, shifted towards the sun as far as the layer's height makes it, drifting with their wind
    float3 X = P.x * sky[0].xyz + P.y * sky[1].xyz + P.z * sky[2].xyz + grade[1].xyz;
    float2 at = (X.xz + L.xz / max(L.y, 0.2) * 3000) / 4000;
    float t = sky[1].w;
    float2 q = at + t * float2(0.020, 0.007) * 0.5;
    float2 warp = float2(Fbm(q * 0.6 + t * float2(0.007, -0.0045), 2), Fbm(q * 0.6 + float2(5.2, 1.3) - t * float2(0.0056, 0.008), 2));
    q += (warp - 0.5) * 1.2;
    float n = Fbm(q, 3) + 0.25 * Fbm(q * 0.31 + 7.3, 2) - 0.12;
    float cover = smoothstep(0.0, 0.3, n - (1 - sky[2].w));
    if (grade[3].z > 0) return 1 - cover;  // XBE_CLOUD_DEBUG: the clouds' cover alone
    return 1 - m.a * (1 - m.g * (1 - 0.8 * cover));
}
// the bounce (grade[0].x the strength, t5): the colour that arrives times the surface's own colour (guessed from how
// it is drawn), as a share of its own light: lit surfaces take a tint, dark ones (shade, rooms) next to none; never
// more than a quarter brighter
float3 Bounce(O i, float3 c) {
    if (grade[0].x <= 0) return c;
    float3 g = b3.SampleLevel(lin, i.uv, 0).rgb;
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    float3 albedo = saturate(c / max(max(max(c.r, c.g), c.b), 0.05)) * 0.6;
    float3 add = albedo * g * grade[0].x * smoothstep(0.03, 0.25, l);
    float al = dot(add, float3(0.2126, 0.7152, 0.0722));
    return c + add * min(1, l * 0.25 / max(al, 1e-4));
}
float4 occlude(O i) : SV_Target {
    float4 s = scene.Load(int3(i.pos.xy, 0));
    s.rgb = Bounce(i, s.rgb);
    if (grade[3].z > 0) return float4(CloudShade(i, float4(1, 1, 0, 1)).xxx, 1);  // XBE_CLOUD_DEBUG: the cover alone
    return float4(s.rgb * occ.SampleLevel(lin, i.uv, 0) * CloudShade(i, shadowm.Load(int3(i.pos.xy, 0))), s.a);
}
// the shadows' debug views (XBE_SHADOW_DEBUG)
float4 shadow_mask(O i) : SV_Target { return float4(shadowm.Load(int3(i.pos.xy, 0)).rrr, 1); }
float4 shadow_rgb(O i) : SV_Target { return float4(shadowm.Load(int3(i.pos.xy, 0)).rgb, 1); }
float4 shadow_raw(O i) : SV_Target { return float4(shadowm.Load(int3(i.pos.xy, 0)).ggg, 1); }
float4 shadow_share(O i) : SV_Target { return float4(shadowm.Load(int3(i.pos.xy, 0)).aaa, 1); }
float4 shadow_cascades(O i) : SV_Target {
    float4 m = shadowm.Load(int3(i.pos.xy, 0));
    float3 tint[4] = {float3(1, 0.35, 0.35), float3(0.35, 1, 0.35), float3(0.4, 0.5, 1), float3(1, 1, 0.3)};
    float3 c = scene.Load(int3(i.pos.xy, 0)).rgb * m.r;
    return float4(m.b > 0 ? lerp(c, tint[min(int(m.b * 4), 3)] * (0.35 + 0.65 * m.g), 0.45) : c, 1);
}
float4 shadow_atlas(O i) : SV_Target {
    float4 s = scene.Load(int3(i.pos.xy, 0));
    float2 a = (i.uv - float2(0.5 - 0.5 * p[2].y / p[2].x, 0)) / float2(p[2].y / p[2].x, 1);
    if (all(a >= 0) && all(a <= 1)) return float4(saturate(shadowm.SampleLevel(pnt, a, 0).r).xxx, 1);
    return s;
}
// the player's pain: the view shakes a little (an unsteady jolt fading with it), wavers like heat and parts its
// colours at the edges; zoomed in a touch so the shaken picture keeps its borders
float2 Hurt(float2 uv, float h, float t) {
    float aspect = p[2].x / p[2].y;
    float2 shake = float2(sin(t * 53.0) * 0.6 + sin(t * 31.0 + 1.3) * 0.4, sin(t * 47.0 + 0.7) * 0.6 + sin(t * 23.0 + 2.1) * 0.4);
    uv = 0.5 + (uv - 0.5) * (1 - 0.03 * h) + shake * float2(1 / aspect, 1) * 0.009 * h * h;
    uv += float2(sin(uv.y * 23.0 + t * 7.0), sin(uv.x * 17.0 * aspect + t * 5.0)) * 0.0025 * h;
    return uv;
}
// A light's shadow at P (view space): the way from P to the light C traced through the world's depth (t1, kept
// before the weapon in hand was drawn), on the screen. Something solid in the way, nearer the eye than the ray and not
// far behind its front (a soldier, a column, not the wall the ray runs along), shades P wherever it lies on the way:
// the shadow is the most the way is covered at any of its samples (a soldier's legs are a short stretch of a long way;
// covered stretches averaged over the whole way left them almost no shadow). Its source does not count: what is within
// src of the light (the gun firing a flash and the hands holding it; a fire's burning heap), and for the eye's own
// flash what is nearer than its muzzle. The samples: about one each 6 pixels of the way on the screen, 16 to 48,
// moved along it by the pixel; the cover fades in over the depth gap, and the shadow keeps a little of the light.
float LightShadow(float3 P, float3 C, float2 uv0, float src) {
    float dist = length(C - P);
    float stop = 1 - src / max(dist, 1);  // the way up to the source's own reach
    if (stop <= 0) return 1;
    float2 a = float2(P.x / P.z / p[0].x, -P.y / P.z / p[0].y);
    float3 E = P + (C - P) * stop;
    float2 b = float2(E.x / max(E.z, 1) / p[0].x, -E.y / max(E.z, 1) / p[0].y);
    int n = clamp((int)(length((b - a) * 0.5 * p[2].xy) / 6), 16, 48);
    // (interleaved gradient noise: a shadow's edge in an even fine pattern, not grain)
    float2 px = floor(uv0 * p[2].xy);
    float dither = frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715)))) * 0.8 + 0.1;
    float thick = 70 + dist * 0.08;
    bool eyeflash = length(C) < 250;
    float cover = 0;
    [loop] for (int s = 0; s < n; s++) {
        float t = (s + dither) / n * stop;
        float3 X = P + (C - P) * t;
        if (X.z <= 1) break;
        float2 q = float2(X.x / X.z / p[0].x * 0.5 + 0.5, 0.5 - X.y / X.z / p[0].y * 0.5);
        if (any(q < 0) || any(q > 1)) break;
        float zs = ViewZ(occ.SampleLevel(pnt, q, 0));
        float gap = X.z - zs;
        float3 O = float3((q.x * 2 - 1) * p[0].x * zs, (1 - q.y * 2) * p[0].y * zs, zs);
        // (the eye's own flash: nothing nearer than its muzzle; nothing by the eye itself)
        if (zs < 60 || (eyeflash && zs < C.z + 10) || length(O - C) < src) continue;
        float bias = 4 + X.z * 0.004;
        cover = max(cover, smoothstep(bias, bias * 2.5, gap) * (1 - smoothstep(thick * 0.7, thick, gap)));
        if (cover > 0.99) break;
    }
    return 1 - cover * 0.9;
}

// The glowing sprites' lights (d3d12.cpp DynLightConstants: view space position and radius, colour): on the world
// where its depth was kept (t1, from before the weapon in hand cleared it), on the weapon where it is drawn (t6, the
// depth now). The surface's facing from its depth; what it reflects guessed from how the game lit it.
float3 Glow(O i, float3 c) {
    float now = shadowm.Load(int3(i.pos.xy, 0)).r;
    float d = now < 1 ? now : occ.Load(int3(i.pos.xy, 0));
    float3 P = ViewPos(i.uv, d);
    float3 N = normalize(cross(ddx(P), ddy(P)));
    if (dot(N, P) > 0) N = -N;
    bool sky = d >= 0.99999;
    float len = length(P);
    float3 dir = P / max(len, 1e-3);
    float3 sum = 0, air = 0;
    for (int k = 0; k < (int)lcount.x; k++) {
        float3 C = lpos[k].xyz;
        float r = lpos[k].w;
        // the air lit around it, between the eye and the surface: the light's inverse square along the ray, in
        // closed form (atan), across its reach; a halo in the dark and the smoke
        float t0 = dot(C, dir);
        float h = max(length(C - dir * t0), r * 0.05);
        float a = max(t0 - r, 0), b = min(t0 + r, len);
        // (none from a light by the eye itself: the weapon in hand's own flash would blind)
        // (none past its radius: saturate(1 - h / r) is 0 there, the atans skipped)
        [branch] if (b > a && h < r) air += lcol[k].rgb * ((atan((b - t0) / h) - atan((a - t0) / h)) * (r / h) * saturate(1 - h / r) * 0.0045 *
                                          saturate((length(C) - 100) / 300));
        float3 L = C - P;
        float d2 = dot(L, L);
        if (sky || d2 >= r * r) continue;
        // a soft fall: the window to 0 at the radius, times an inverse square of a quarter of it
        float f = 1 - d2 / (r * r), q = d2 / (r * r);
        float ndl = saturate(dot(N, L * rsqrt(max(d2, 1e-4)))) * 0.8 + 0.2;
        float lit = 1;
        bool casts = lcol[k].w > 0.75;
        // (its source left out: a shot's flash's gun and hands, 60 cm; a fire's (0.875) burning heap, a metre)
        if (casts && now >= 1) lit = LightShadow(P, C, i.uv, lcol[k].w < 0.95 ? 100 : 60);
        // the lights that cast (the shots' flashes, fires, big lights) reach further with their strength: what they
        // light at a middle distance stands out from what they leave in shadow
        float fall = casts ? f * f / (1 + 2 * q) * 1.6 : f * f / (1 + 6 * q);
        sum += lcol[k].rgb * (fall * ndl * lit);
    }
    // the weapon in hand, by the muzzle itself, takes less
    if (now < 1) sum *= 0.35, air *= 0.5;
    // many lights at once (an explosion's dozens of sprites) light less than their sum: a soft knee
    sum /= 1 + 0.45 * max(max(sum.r, sum.g), sum.b);
    air /= 1 + 1.5 * max(max(air.r, air.g), air.b);
    air *= lcount.w;  // the halos switched off
    float3 albedo = saturate(c / 0.35) * 0.8 + 0.03;
    return c + albedo * sum + air;
}
// Ambient motes: points in the air about the eye (cells of 1 m, most with a point, drifting), drawn where nothing solid is
// in front: dust where the light falls (the scene behind it is lit), embers near the fires' lights (rising, warm). Their
// view space place from the world's (the camera's rows sky[0..2], its place heatz[7].yzw: SceneFinal).
float3 Motes(O i, float3 c) {
    float t = p[4].y;
    float3 eye = heatz[7].yzw;
    // (the weapon in hand's depth where it is drawn: no motes behind it in front of it)
    float wn = shadowm.Load(int3(i.pos.xy, 0)).r;
    float zs = wn < 1 ? ViewZ(wn) : ViewZ(occ.SampleLevel(pnt, i.uv, 0));
    float3 ray = normalize(float3((i.uv.x * 2 - 1) * p[0].x, (1 - i.uv.y * 2) * p[0].y, 1));
    float3 add = 0;
    float px = 2 * p[0].y / p[2].y;  // a pixel's size at depth 1
    // (a step in the cell of the step before adds what that one did: the same point, worked out once)
    float3 lastCell = float3(1e9, 1e9, 1e9), lastAdd = 0;
    // along the ray, the cells it crosses at a few depths
    [loop] for (int k = 0; k < 16; k++) {
        float z = 40 + k * 45;
        if (z > zs) break;
        float3 Vv = ray * (z / ray.z);
        float3 W = eye + Vv.x * sky[0].xyz + Vv.y * sky[1].xyz + Vv.z * sky[2].xyz;
        float3 cell = floor(W / 100);
        [branch] if (all(cell == lastCell)) {
            add += lastAdd;
            continue;
        }
        lastCell = cell, lastAdd = 0;
        float h = Hash(cell.xz + cell.y * 17.31);
        if (h > 0.7) continue;
        float h2 = Hash(cell.zx * 1.7 + 3.1), h3 = Hash(cell.xy * 2.3 + 7.9);
        // the point in its cell, drifting slowly (embers rise)
        float3 Pw = (cell + float3(h / 0.7, h2, h3)) * 100 + float3(sin(t * 0.3 + h * 20), cos(t * 0.23 + h2 * 20), sin(t * 0.27 + h3 * 20)) * 20;
        // back to view space
        float3 d = Pw - eye;
        float3 Pv = float3(dot(d, sky[0].xyz), dot(d, sky[1].xyz), dot(d, sky[2].xyz));
        if (Pv.z < 20 || Pv.z > zs) continue;
        float2 q = float2(Pv.x / Pv.z / p[0].x * 0.5 + 0.5, 0.5 - Pv.y / Pv.z / p[0].y * 0.5);
        float dist = length((q - i.uv) * p[2].xy);
        float size = max(1.0, 1.5 / (px * Pv.z));
        float spot = saturate(1 - dist / size);
        if (spot <= 0) continue;
        // lit: the fire lights near it (embers), else the scene's light about it (dust in a beam)
        float3 lit = 0;
        for (int l = 0; l < (int)lcount.x; l++) {
            // embers by fires alone (lcol.w 0.5: a flickering light), not by shots' flashes
            if (abs(lcol[l].w - 0.5) > 0.1 && abs(lcol[l].w - 0.875) > 0.05) continue;  // fires (0.875: one that casts)
            float3 L = lpos[l].xyz - Pv;
            float r = lpos[l].w * 0.8;
            float qq = dot(L, L) / (r * r);
            if (qq < 1) lit += lcol[l].rgb * (1 - qq) * 1.5;
        }
        // dust shows against what is near (a lit wall, a beam in a room), not against the open sky
        float3 behind = scene.SampleLevel(lin, q, 0).rgb;
        float bl = dot(behind, float3(0.2126, 0.7152, 0.0722));
        float near = 1 - smoothstep(1500, 4000, zs);
        float3 dust = behind * smoothstep(0.2, 0.55, bl) * 0.5 * near;
        float3 inc = (lit + dust) * spot * spot * heatz[6].y;
        if (heatz[6].y > 1.5) inc += float3(spot, 0, spot);  // XBE_MOTES_DEBUG: every mote magenta
        lastAdd = inc;
        add += inc;
    }
    return c + add;
}
// d3d12.cpp SceneFinal: t1 the world's depth, t2 the bloom, t3 the sun's shafts (b1), t4 the blurred scene (b2),
// t6 the depth now; p[4] pain, time, flash, aiming's blur; lcount: lights, heat sources, shafts
float4 composite(O i) : SV_Target {
    float4 s;
    float2 uv = i.uv;
    float h = p[4].x, t = p[4].y;
    if (h > 0) uv = Hurt(uv, h, t);
    // the air over fires and explosions shimmers: a rising column over each (heat: its place, reach and strength
    // on the screen), only what is behind it
    int nh = (int)lcount.y;
    // (not over the weapon in hand and the hands: the world behind them shimmered through them)
    const bool weapon = shadowm.Load(int3(i.pos.xy, 0)).r < 1;
    if (nh > 0 && !weapon) {
        float aspect = p[2].x / p[2].y;
        float zp = ViewZ(occ.SampleLevel(pnt, uv, 0));
        float2 off = 0;
        for (int k = 0; k < nh; k++) {
            float2 e = (uv - heat[k].xy) * float2(aspect, 1) / max(heat[k].z, 1e-4);
            e.y = e.y < 0 ? e.y * 0.5 : e.y * 1.8;
            float f = saturate(1 - dot(e, e)) * heat[k].w * (zp > heatz[k].x ? 1 : 0);
            // waves across several directions, rising: no pattern of their own to see
            float2 q = uv * float2(aspect, 1) + float2(0, t * 0.05);
            float n1 = sin(dot(q, float2(173, 241)) + t * 9 + k * 1.7), n2 = sin(dot(q, float2(-263, 197)) - t * 13);
            float n3 = sin(dot(q, float2(97, -419)) + t * 17), n4 = sin(dot(q, float2(331, 59)) - t * 7);
            off += f * float2(n1 * 0.5 + n3 * 0.3 + n4 * 0.2, n2 * 0.5 + n4 * 0.3 + n1 * 0.2);
        }
        uv += off * 0.0007;
    }
    // the blasts' shock (heatz[3..4].yzw: the ring's centre, its radius now, its strength in the tens): the air at the
    // ring's front pushed outwards, the picture bending as it passes
    [unroll] for (int sk = 3; sk <= 4; sk++) {
        float rr = heatz[sk].w;
        if (rr <= 0) continue;
        float strength = floor(rr / 10) / 100;
        float radius = rr - floor(rr / 10) * 10;
        float aspect = p[2].x / p[2].y;
        float2 e = (uv - heatz[sk].yz) * float2(aspect, 1);
        float d = length(e);
        float front = exp(-pow((d - radius) / (radius * 0.12 + 0.01), 2));
        uv -= e / max(d, 1e-3) * float2(1 / aspect, 1) * front * strength * 0.025;
        nh = max(nh, 1);
    }
    // the barrel's heat (heatz[5].yzw: the muzzle on the screen, the heat): the air over it shimmers, rising
    float bheat = heatz[5].w;
    if (bheat > 0) {
        float aspect = p[2].x / p[2].y;
        float2 e = (uv - heatz[5].yz) * float2(aspect, 1);
        // a column rising from the muzzle, widening
        float up = -e.y;
        float col = saturate(1 - abs(e.x) / (0.02 + max(up, 0) * 0.4)) * saturate(up * 20 + 1) * saturate(1 - up / 0.25);
        float2 q = uv * float2(aspect, 1) + float2(0, t * 0.15);
        float2 wob = float2(sin(dot(q, float2(211, 157)) + t * 11), sin(dot(q, float2(-173, 233)) - t * 9));
        uv += wob * col * bheat * 0.0012;
        nh = max(nh, 1);
    }
    if (h > 0 || nh > 0) {
        s = scene.SampleLevel(lin, uv, 0);
        if (h > 0) {
            float2 fringe = (uv - 0.5) * 0.006 * h;
            s.r = scene.SampleLevel(lin, uv + fringe, 0).r;
            s.b = scene.SampleLevel(lin, uv - fringe, 0).b;
        }
    } else {
        s = scene.Load(int3(i.pos.xy, 0));
    }
    float3 c = s.rgb;
    // dirt on the view after a close blast (heatz[2].y, its pattern heatz[2].z): specks and smears, darker brown,
    // more at the edges, fading
    if (heatz[2].y > 0) {
        float aspect = p[2].x / p[2].y;
        // round specks in cells, each its own size and place, soft edged; a few smears; mostly towards the edges
        float2 q = i.uv * float2(aspect, 1) * 14;
        float2 cell = floor(q), f = frac(q);
        float amount = 0;
        [unroll] for (int oy = -1; oy <= 1; oy++)
            [unroll] for (int ox = -1; ox <= 1; ox++) {
                float2 cc = cell + float2(ox, oy);
                float h = Hash(cc + heatz[2].z * 7.3);
                if (h > 0.3) continue;
                float2 at = float2(ox, oy) + float2(Hash(cc * 1.3 + 2.1), Hash(cc * 2.7 + 5.3));
                float r = 0.08 + 0.25 * Hash(cc * 3.1 + 1.7);
                float2 d = (f - at) * float2(1, 1 + Hash(cc * 4.3) * 0.6);
                amount = max(amount, (1 - smoothstep(r * 0.6, r, length(d))) * (0.5 + 0.5 * h / 0.3));
            }
        float edge = saturate(length((i.uv - 0.5) * float2(aspect, 1)) * 1.6 - 0.15);
        amount *= heatz[2].y * edge;
        c = lerp(c, c * float3(0.5, 0.42, 0.33) + float3(0.025, 0.02, 0.012), amount * 0.8);
    }
    // after a long burst (the heat high) a faint wisp of smoke from the muzzle, curling up
    if (bheat > 0.35) {
        float aspect = p[2].x / p[2].y;
        float2 e = (i.uv - heatz[5].yz) * float2(aspect, 1);
        float up = -e.y;
        float x = e.x - sin(up * 18 + t * 1.5) * 0.015 * up * 6;
        float wisp = saturate(1 - abs(x) / (0.008 + up * 0.08)) * saturate(up * 30) * saturate(1 - up / 0.3);
        wisp *= 0.5 + 0.5 * Noise(float2(x * 60, up * 40 - t * 2));
        c = lerp(c, max(c, 0.18) * 1.15, wisp * (bheat - 0.35) * 0.5);
    }
    // aiming: what lies well behind the point aimed at blurs (the weapon in hand and the near stay sharp)
    // the weapon in hand while aiming: out of focus (the eye on the distance), its sights in the middle less so
    if (p[4].w > 0 && shadowm.Load(int3(i.pos.xy, 0)).r < 1) {
        float2 e = (i.uv - float2(0.5, 0.5)) * float2(p[2].x / p[2].y, 1);
        float coc = smoothstep(0.04, 0.22, length(e)) * 0.85 * p[4].w;
        c = lerp(c, b2.SampleLevel(lin, uv, 0).rgb, coc);
    }
    if (p[4].w > 0 && shadowm.Load(int3(i.pos.xy, 0)).r >= 1) {
        float zc = min(min(ViewZ(occ.SampleLevel(pnt, float2(0.5, 0.5), 0)), ViewZ(occ.SampleLevel(pnt, float2(0.5, 0.47), 0))),
                       ViewZ(occ.SampleLevel(pnt, float2(0.5, 0.53), 0)));
        float z = ViewZ(occ.SampleLevel(pnt, uv, 0));
        // and the edges of the view, gently: the eye on the sights
        float2 e = (i.uv - 0.5) * float2(p[2].x / p[2].y, 1);
        float coc = max(saturate((z - zc * 1.3) / (zc * 1.5 + 300)), smoothstep(0.35, 0.85, length(e)) * 0.7) * p[4].w;
        c = lerp(c, b2.SampleLevel(lin, uv, 0).rgb, coc);
    }
    // the levels upsampled into the first (d3d12.cpp ScenePost), the sun's shafts
    c += b0.SampleLevel(lin, uv, 0).rgb * p[3].w;
    // the lens (heatz[0].y on): its dirt lit by the bloom's brightest light (a smudge pattern catching it), and the
    // sun's flare (heatz[1].yzw: on the screen, strength) when the sun is not hidden: ghosts along the line through
    // the centre, a faint halo
    if (heatz[0].y > 0) {
        float aspect = p[2].x / p[2].y;
        float3 glow = b0.SampleLevel(lin, uv, 5).rgb;
        float2 q = i.uv * float2(aspect, 1) * 3;
        float dirt = smoothstep(0.45, 0.8, Fbm(q, 3)) * 0.7 + smoothstep(0.75, 0.9, Noise(q * 7)) * 0.5;
        c += glow * dirt * 0.25 * p[3].w;
        float strength = heatz[1].w;
        float2 sun = heatz[1].yz;
        if (strength > 0 && all(sun > -0.2) && all(sun < 1.2)) {
            float ds = all(sun > 0) && all(sun < 1) ? occ.SampleLevel(pnt, sun, 0) : 0;
            float hid = all(sun > 0) && all(sun < 1) ? shadowm.Load(int3(sun * p[2].xy, 0)).r : 1;  // (the weapon in hand over it)
            float seen = ds >= 0.9999 && !Veiled(ds) && hid >= 1 ? 1 : 0;
            if (seen > 0) {
                float2 dir = 0.5 - sun;
                float3 flare = 0;
                // (soft blobs that fade into the sky, no rims: a hard edged disc read as a second moon or sun)
                [unroll] for (int g = 1; g <= 4; g++) {
                    float2 at = sun + dir * (g * 0.45);
                    float r = 0.02 + 0.02 * g;
                    float d = length((i.uv - at) * float2(aspect, 1)) / r;
                    flare += exp(-d * d * 2.5) * float3(1.0, 0.8, 0.5) * (0.35 / g);
                }
                c += flare * strength * 0.1;
            }
        }
    }
    // (not over the weapon in hand and the hands: drawn after the world's depth, the shafts went through them)
    if (lcount.z > 0 && shadowm.Load(int3(i.pos.xy, 0)).r >= 1) c += b1.SampleLevel(lin, uv, 0).rgb;
    if (lcount.x > 0) c = Glow(i, c);
    if (heatz[6].y > 0) c = Motes(i, c);
    // XBE_LIGHTS_DEBUG: the dynamic lights' light alone, on a grey world
    if (sky[5].z >= 2) return float4(lcount.x > 0 ? Glow(i, float3(0.3, 0.3, 0.3)) - 0.3 : float3(0, 0, 0), 1);
    // a close explosion's flash
    c = c * (1 + p[4].z) + p[4].z * 0.06 * float3(1, 0.75, 0.45);
    // the volumetric haze (d3d12.cpp FogPass, t5 at half size): what it hides takes its colour (the sky's or the
    // game's fog's, lit by the scene's own light around), and the sun's light scattered in it
    // (not over the weapon in hand and the hands: the haze of the world behind them, the sky's, washed them out)
    if (grade[2].w > 0 && shadowm.Load(int3(i.pos.xy, 0)).r >= 1) {
        float4 f = b3.SampleLevel(lin, uv, 0);
        float3 tint = float3(grade[3].y, grade[3].z, grade[3].w);
        float ambient = dot(c, float3(0.2126, 0.7152, 0.0722));
        c = lerp(c, tint * lerp(0.25, 0.6, saturate(ambient * 3)), f.a * 0.55) + f.rgb * tint * 1.6;
    }
    // the eye's exposure (a little, d3d12.cpp Exposure), the campaign's tones: saturation, a gain, a lift that fades
    // out in the light (the blacks stay black where the game drew them so)
    if (grade[0].w > 0) {
        c *= grade[0].x;
        float l = dot(c, float3(0.2126, 0.7152, 0.0722));
        c = max(lerp(l.xxx, c, grade[0].y), 0);
        c = c * grade[1].rgb + grade[2].rgb * saturate(l * 8) * (1 - saturate(l * 2));
    }
    // highlights over 0.8 roll off towards 1, everything below stays as the game drew it
    const float a = 0.8;
    c = c < a ? c : a + (1 - a) * (1 - exp(-(c - a) / (1 - a)));
    // a faint grain, finest in the mid tones, none in black or white
    if (grade[3].x > 0) {
        float l = dot(saturate(c), float3(0.2126, 0.7152, 0.0722));
        float2 q = i.pos.xy + float2(frac(p[4].y * 7.31) * 613, frac(p[4].y * 3.17) * 411);
        float n = (Hash(q) + Hash(q + 17.3) - 1) * grade[3].x;
        c += n * 4 * l * (1 - l);
    }
    return float4(c, s.a);
}
float4 composite_debug(O i) : SV_Target { return float4(occ.SampleLevel(lin, i.uv, 0).xxx, 1); }
)";

}  // namespace gpu
