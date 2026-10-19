// See shadows.h.
#include "gpu/shadows.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "core/log.h"
#include "gpu/remaster.h"

namespace gpu::shadow {

namespace {

float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

bool Normalize(float* v) {
    float n = std::sqrt(Dot(v, v));
    if (!(n > 1e-6f)) return false;
    for (int i = 0; i < 3; i++) v[i] /= n;
    return true;
}

void Cross(const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

float Degrees(const float* a, const float* b) { return std::acos(std::clamp(Dot(a, b), -1.0f, 1.0f)) * 57.29578f; }

// towards b by t, along the great circle
void Slerp(float* a, const float* b, float t) {
    float o[3];
    for (int i = 0; i < 3; i++) o[i] = a[i] + (b[i] - a[i]) * t;
    if (Normalize(o)) std::memcpy(a, o, sizeof(o));
}

std::map<int, LevelOverride> LoadOverrides() {
    std::map<int, LevelOverride> all;
    std::ifstream in(std::filesystem::path(remaster::Dir()) / "shadows.txt");
    for (std::string line; std::getline(in, line);) {
        if (auto c = line.find('#'); c != std::string::npos) line.resize(c);
        std::istringstream s(line);
        int level;
        std::string what;
        if (!(s >> level >> what)) continue;
        LevelOverride& o = all[level];
        if (what == "off") o.off = true;
        else if (what == "auto") o = LevelOverride{};
        else if (what == "dir" && (s >> o.d[0] >> o.d[1] >> o.d[2]) && Normalize(o.d)) o.dir = true;
        else if (what == "strength") s >> o.strength;
    }
    LOGR("shadows: %zu level settings", all.size());
    return all;
}

}  // namespace

const char* SourceName(int source) {
    switch (source) {
        case kFixed: return "fixed function light";
        case kStatic: return "static models' light";
        case kOverride: return "level setting";
        default: return "none";
    }
}

LevelOverride OverrideFor(int level) {
    static const std::map<int, LevelOverride> all = LoadOverrides();
    auto it = all.find(level);
    return it == all.end() ? LevelOverride{} : it->second;
}

Tracker& Lights() {
    static Tracker t;
    return t;
}

// lights within 8 degrees of each other are one
void Tracker::Add(const Candidate& c) {
    float d[3] = {c.dir[0], c.dir[1], c.dir[2]};
    if (!Normalize(d) || !(c.color > 0.01f)) return;
    for (Group& g : groups_) {
        float gd[3] = {g.sum[0], g.sum[1], g.sum[2]};
        if (g.source == c.source && Normalize(gd) && Dot(gd, d) > 0.99f) {
            for (int i = 0; i < 3; i++) g.sum[i] += d[i] * c.color;
            g.color += c.color, g.ambient += c.ambient, g.weight += 1;
            return;
        }
    }
    if (groups_.size() >= 16) return;
    groups_.push_back({{d[0] * c.color, d[1] * c.color, d[2] * c.color}, 1, c.color, c.ambient, c.source});
}

Light Tracker::Resolve(double now, int level) {
    Light out;
    double dt = last_ < 0 ? 0 : std::clamp(now - last_, 0.0, 0.25);
    last_ = now;
    LevelOverride ov = OverrideFor(level);
    if (level != level_) {
        // a new level: the light starts over
        level_ = level, have_ = false, weight_ = 0, pending_since_ = -1, seen_ = -1, best_source_ = kNone;
        LOGR("shadows: level %d%s%s", level, ov.off ? ", off by its setting" : "", ov.dir ? ", its light set" : "");
    }
    // the frame's main light: the explicit kinds first (the static models' over the fixed function's), the
    // strongest group of the kind
    // a kind once seen holds for the level: the fixed function lights (a held weapon's, a character's own) do not
    // take over when no static model is in view
    const Group* best = nullptr;
    for (const Group& g : groups_) best_source_ = std::max(best_source_, g.source);
    for (const Group& g : groups_) {
        if (g.source < best_source_) continue;
        if (!best || g.source > best->source || (g.source == best->source && g.color > best->color)) best = &g;
    }
    float dir[3];
    bool found = best && (std::memcpy(dir, best->sum, sizeof(dir)), Normalize(dir));
    if (ov.dir) found = true, std::memcpy(dir, ov.d, sizeof(dir));
    if (found) {
        int source = ov.dir ? kOverride : best->source;
        float direct = best ? best->color / best->weight : 0.6f, ambient = best ? best->ambient / best->weight : 0.25f;
        seen_ = now;
        if (!have_) {
            std::memcpy(cur_, dir, sizeof(dir));
            std::memcpy(target_, dir, sizeof(dir));
            have_ = true, direct_ = direct, ambient_ = ambient, source_ = source;
            LOGR("shadows: light %.3f %.3f %.3f (%s), light %.2f ambient %.2f", dir[0], dir[1], dir[2], SourceName(source), direct,
                 ambient);
        } else if (Degrees(dir, target_) > 12.0f || source != source_) {
            // hysteresis: a different light is taken once it held for half a second; small turns follow at once
            if (pending_since_ < 0 || Degrees(dir, pending_) > 12.0f) pending_since_ = now, std::memcpy(pending_, dir, sizeof(dir));
            if (now - pending_since_ > 0.5) {
                std::memcpy(target_, dir, sizeof(dir));
                source_ = source, pending_since_ = -1;
                LOGR("shadows: light now %.3f %.3f %.3f (%s), light %.2f ambient %.2f", dir[0], dir[1], dir[2], SourceName(source),
                     direct, ambient);
            }
        } else {
            std::memcpy(target_, dir, sizeof(dir));
            pending_since_ = -1;
        }
        float k = 1 - float(std::exp(-dt / 0.15));
        direct_ += (direct - direct_) * k, ambient_ += (ambient - ambient_) * k;
    }
    groups_.clear();
    // the static models' light is the level's: it stays while none is in view
    if (have_ && best_source_ == kStatic) seen_ = now;
    // the direction eases to its target (a quarter second), the shadows fade with the light's presence: in over a
    // half second, out after two seconds without it
    if (have_) Slerp(cur_, target_, 1 - float(std::exp(-dt / 0.25)));
    bool present = have_ && seen_ >= 0 && now - seen_ < 2.0;
    weight_ = std::clamp(weight_ + float(dt) * (present ? 2.0f : -1.0f), 0.0f, 1.0f);
    out.on = have_ && weight_ > 0 && !ov.off;
    std::memcpy(out.dir, cur_, sizeof(cur_));
    out.direct = direct_, out.ambient = ambient_, out.weight = weight_, out.strength = ov.strength, out.source = source_;
    {
        std::lock_guard l(lock_);
        std::memcpy(sun_, cur_, sizeof(cur_));
        sun_ok_ = have_;
    }
    return out;
}

bool Tracker::Sun(float dir[3]) const {
    std::lock_guard l(lock_);
    std::memcpy(dir, sun_, sizeof(sun_));
    return sun_ok_;
}

Quality QualityFor(uint32_t level, float units) {
    // (each step a step lower than before the lighting bake: High the old Medium, Medium the old Low, Low lower still)
    if (level >= 1 && level <= 3) {
        if (level == 1) return {2, 768, 30 * units, 1.0f, 4};
        level -= 1;
    }
    switch (level) {
        case 1: return {2, 1024, 35 * units, 1.2f, 4};
        case 2: return {3, 1536, 55 * units, 1.5f, 8};
        case 3: return {4, 2048, 80 * units, 1.6f, 12};
        case 4: return {4, 4096, 120 * units, 1.8f, 16};
        default: return {0, 0, 0, 0, 0};
    }
}

void Mul(const float* a, const float* b, float* o) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    std::memcpy(o, r, sizeof(r));
}

bool Invert(const float* m, float* o) {
    // by cofactors, in double: view matrices carry world positions in centimetres
    double a[16], inv[16];
    for (int i = 0; i < 16; i++) a[i] = m[i];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::fabs(det) < 1e-30) return false;
    for (int i = 0; i < 16; i++) o[i] = float(inv[i] / det);
    return true;
}

void Fit(const Camera& cam, const float light_dir[3], const Quality& q, Cascades& out) {
    out.count = q.cascades;
    // the slices: a practical split between logarithmic and even, from the near plane to the shadows' reach
    float n = std::max(cam.near_z, 1.0f), f = q.distance;
    float ends[5];
    ends[0] = n;
    for (int i = 1; i <= q.cascades; i++) {
        float t = float(i) / q.cascades;
        float lg = n * std::pow(f / n, t), ev = n + (f - n) * t;
        ends[i] = 0.82f * lg + 0.18f * ev;
    }
    // the light's frame: z along the light's travel
    float lz[3] = {-light_dir[0], -light_dir[1], -light_dir[2]};
    float up[3] = {0, 1, 0};
    if (std::fabs(lz[1]) > 0.95f) up[0] = 1, up[1] = 0;
    float lx[3], ly[3];
    Cross(up, lz, lx);
    Normalize(lx);
    Cross(lz, lx, ly);
    const float* m = cam.view_to_world;
    const float eye[3] = {m[12], m[13], m[14]}, ahead[3] = {m[8], m[9], m[10]};
    float k2 = 1 / (cam.p00 * cam.p00) + 1 / (cam.p11 * cam.p11);  // the slices' corners: (x, y) = z * tan, squared
    for (int c = 0; c < q.cascades; c++) {
        float a = ends[c], b = ends[c + 1];
        out.far_z[c] = b;
        // the smallest sphere through the slice's corners, on its axis: its size is the slice's alone
        float z0 = std::min(b, 0.5f * (a + b) * (1 + k2));
        float r = std::sqrt(std::max((b - z0) * (b - z0) + b * b * k2, (z0 - a) * (z0 - a) + a * a * k2));
        r = std::ceil(r / 16) * 16;
        float centre[3];
        for (int i = 0; i < 3; i++) centre[i] = eye[i] + ahead[i] * z0;
        // in the light's frame, the centre snapped to whole texels
        float texel = 2 * r / float(q.tile);
        float cx = std::floor(Dot(centre, lx) / texel) * texel, cy = std::floor(Dot(centre, ly) / texel) * texel;
        float cz = Dot(centre, lz);
        // depth: the sphere and what stands towards the light from it (further casters are flattened onto its near
        // end by the rasterizer's clamp)
        float back = std::max(r * 2, 0.75f * q.distance);
        float z_near = cz - r - back, range = 2 * r + back;
        out.texel[c] = texel;
        out.depth_range[c] = range;
        // world to light clip: x = (p.lx - cx) / r, y = (p.ly - cy) / r, z = (p.lz - z_near) / range
        float* w = out.world_to_light[c];
        for (int i = 0; i < 3; i++) {
            w[i * 4 + 0] = lx[i] / r;
            w[i * 4 + 1] = ly[i] / r;
            w[i * 4 + 2] = lz[i] / range;
            w[i * 4 + 3] = 0;
        }
        w[12] = -cx / r, w[13] = -cy / r, w[14] = -z_near / range, w[15] = 1;
    }
}

}  // namespace gpu::shadow
