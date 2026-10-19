// The lighting bake (offline, Embree): ambient occlusion and the sky's reach over the static world the game draws
// (captured by d3d12.cpp CaptureGeometry: lvNN.geo, lvNN.eye), for every place on its surfaces, kept in a hash table
// the game's GPU looks up by a pixel's world position (d3d12.cpp BakedOcclusion, scene_shaders.h Baked).
//
//     bake <lvNN.geo> <out.bake> [--rays N] [--radius cm] [--prune 1]
//
// The surfaces are cut into cells of a grid (four sizes: 25 cm near where the eye went, coarser further), one sample
// a cell and facing (the six axes' directions: a wall's two sides, a corner's two faces apart): where in the cell the
// surface lies (its area's centre) and its normal. From each sample, rays over the hemisphere (cosine weighted):
// the occlusion is what they hit within the radius, nearer counting more; the sky's reach what they leave by.
//
// (Version 6, --colours lvNN.col: each entry's value followed by the light bounced onto the cell, 32 bits.)
// The table (out.bake): "BAK1", the version (2), the cell sizes, the entry count and the table's size (a power of
// two), then the entries (and after them the static draws' identities: their count, then 64 bits each): uint4 (key low, key high, value low, value high), an empty one all zero. The key: the cell's
// x y z (19 bits each, two's complement), its facing (3 bits) and size (2 bits), bit 63 set. The value: occlusion,
// sky (8 bits each, 255 none / all), the sample's normal (octahedral, 8 bits a component), its place along the normal
// from the cell's centre (signed 8 bits, of half the cell). The slot: Hash() below, probed one after the other.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <compressapi.h>
#include <embree4/rtcore.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Norm(V3 a) {
    float l = Len(a);
    return l > 1e-20f ? a * (1 / l) : V3{0, 1, 0};
}

constexpr float kCell[4] = {25, 50, 100, 400};  // the cell sizes (cm)
constexpr float kNear[3] = {3000, 6000, 12000};  // the finer three within this of the eye's way

struct Draw {
    uint8_t kind, flags;
    uint64_t tex;
    std::vector<V3> pos;
    std::vector<uint32_t> tris;
};

bool ReadGeo(const char* path, std::vector<Draw>& draws, std::vector<uint64_t>& ids) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    struct Rec {
        Draw d;
        uint64_t id;
        V3 lo, hi;
        int place = -1;  // its placement (the same draw again, within 3 cm)
    };
    std::vector<Rec> recs;
    // the soldiers' places (lvNN.act, the capture's): what they carry is no part of the level
    std::unordered_map<uint64_t, std::vector<V3>> soldiers;  // by metre columns (x z)
    auto column = [](float x, float z) { return uint64_t(uint32_t(int(std::floor(x / 100)))) << 32 | uint32_t(int(std::floor(z / 100))); };
    {
        std::string base = path;
        std::ifstream act(base.substr(0, base.size() - 4) + ".act", std::ios::binary);
        V3 e;
        while (act.read(reinterpret_cast<char*>(&e), 12))
            if (std::isfinite(e.x + e.y + e.z)) soldiers[column(e.x, e.z)].push_back(e);
    }
    auto by_a_soldier = [&](V3 c) {
        for (int dz = -2; dz <= 2; dz++)
            for (int dx = -2; dx <= 2; dx++) {
                auto it = soldiers.find(column(c.x + dx * 100.0f, c.z + dz * 100.0f));
                if (it == soldiers.end()) continue;
                for (V3 q : it->second)
                    if ((q.x - c.x) * (q.x - c.x) + (q.z - c.z) * (q.z - c.z) < 150.0f * 150.0f && c.y > q.y - 100 && c.y < q.y + 250) return true;
            }
        return false;
    };
    for (;;) {
        struct {
            char magic[4];
            uint8_t kind, flags;
            uint16_t pad;
            uint64_t tex, id;
            uint32_t verts, tris;
        } h;
        if (!in.read(reinterpret_cast<char*>(&h), sizeof(h))) break;
        if (std::memcmp(h.magic, "GEO2", 4) != 0) return false;
        Rec r{Draw{h.kind, h.flags, h.tex, std::vector<V3>(h.verts), std::vector<uint32_t>(size_t(h.tris) * 3)}, h.id, {}, {}};
        if (!in.read(reinterpret_cast<char*>(r.d.pos.data()), std::streamsize(h.verts) * 12)) break;
        if (!in.read(reinterpret_cast<char*>(r.d.tris.data()), std::streamsize(h.tris) * 12)) break;
        r.lo = {1e30f, 1e30f, 1e30f}, r.hi = {-1e30f, -1e30f, -1e30f};
        for (V3 p : r.d.pos) r.lo = {std::min(r.lo.x, p.x), std::min(r.lo.y, p.y), std::min(r.lo.z, p.z)}, r.hi = {std::max(r.hi.x, p.x), std::max(r.hi.y, p.y), std::max(r.hi.z, p.z)};
        recs.push_back(std::move(r));
    }
    // by mesh (texture and counts): its placements; the same draw again (a program's matrix is worked out from the
    // camera's: from another place it rounds differently, a new identity) is one within 3 cm. A mesh placed again and
    // again within 3 m of itself moves (debris drifting on the river, a boat rocking, what sways slowly: still for the
    // capture's 20 frames each time), turned another way each time: left out of the bake, the shadow map casts it
    std::unordered_map<std::string, std::vector<size_t>> by_mesh;
    for (size_t i = 0; i < recs.size(); i++) {
        char key[64];
        std::snprintf(key, sizeof(key), "%llx %zu %zu", (unsigned long long)recs[i].d.tex, recs[i].d.pos.size(), recs[i].d.tris.size());
        by_mesh[key].push_back(i);
    }
    size_t placements = 0, moving = 0, cut = 0;
    for (auto& [key, list] : by_mesh) {
        std::vector<size_t> firsts;  // a record of each placement
        for (size_t i : list) {
            Rec& r = recs[i];
            for (size_t k = 0; k < firsts.size() && r.place < 0; k++) {
                const Rec& f = recs[firsts[k]];
                if (std::fabs(f.lo.x - r.lo.x) < 3 && std::fabs(f.lo.y - r.lo.y) < 3 && std::fabs(f.lo.z - r.lo.z) < 3 && std::fabs(f.hi.x - r.hi.x) < 3 &&
                    std::fabs(f.hi.y - r.hi.y) < 3 && std::fabs(f.hi.z - r.hi.z) < 3)
                    r.place = int(k);
            }
            if (r.place < 0) r.place = int(firsts.size()), firsts.push_back(i);
        }
        std::vector<bool> moves(firsts.size(), false);
        bool all = false;
        // (the world's sectors, drawn with the identity for their world matrix, never move)
        const bool world = (recs[firsts[0]].d.flags & 2) != 0;
        if (world) all = true;
        // a mesh placed many times, most of them turned their own way: what the soldiers carry (helmets, rifles,
        // packs: drawn as static models, still for a while as their bearer stands), what is thrown about; all of it
        // left out. (A designer's repeats, sandbags, crates, windows, are turned a few ways at most.)
        // a mesh most of whose places were a soldier's: his helmet, rifle, pack, face (drawn as static models, still
        // while he stood)
        if (!world && !soldiers.empty() && firsts.size() >= 3) {
            size_t carried = 0;
            for (size_t k : firsts) carried += by_a_soldier((recs[k].lo + recs[k].hi) * 0.5f);
            if (carried * 2 > firsts.size()) moves.assign(firsts.size(), true), all = true;
        }
        if (firsts.size() > 20 && !all) {
            std::vector<V3> shapes;
            for (size_t k : firsts) {
                V3 e = recs[k].hi - recs[k].lo;
                bool known = false;
                for (V3 q : shapes)
                    if (std::fabs(q.x - e.x) <= 5 && std::fabs(q.y - e.y) <= 5 && std::fabs(q.z - e.z) <= 5) {
                        known = true;
                        break;
                    }
                if (!known) shapes.push_back(e);
                if (shapes.size() * 10 > firsts.size() * 3) break;  // (enough: turned its own way most times)
            }
            if (shapes.size() * 10 > firsts.size() * 3) moves.assign(firsts.size(), true), all = true;
        }
        for (size_t k = 0; k < firsts.size() && !all; k++) {
            const Rec& f = recs[firsts[k]];
            V3 c = (f.lo + f.hi) * 0.5f;
            int close_by = 0;
            for (size_t j = 0; j < firsts.size() && close_by < 3; j++) {
                if (j == k) continue;
                const Rec& g = recs[firsts[j]];
                // (turned another way: its bounds of another shape; a row of the same thing, sandbags, windows along
                // a wall, is placed again and again but always the same way)
                V3 ef = f.hi - f.lo, eg = g.hi - g.lo;
                bool turned = std::fabs(ef.x - eg.x) > 5 || std::fabs(ef.y - eg.y) > 5 || std::fabs(ef.z - eg.z) > 5;
                // (or drifting the same way: three within a metre)
                float apart = Len((g.lo + g.hi) * 0.5f - c);
                if ((turned && apart < 300) || apart < 100) close_by++;
            }
            moves[k] = close_by >= 3;
        }
        // (most of its places moving: all of it, a plane passing, a vehicle; the places it was seen apart too. And
        // a big thing, over 4 m, in more than six places: a vehicle stopping now and then, the tank the player
        // escorts, planes)
        {
            size_t m = 0;
            for (size_t k = 0; k < firsts.size(); k++) m += moves[k];
            V3 e = recs[firsts[0]].hi - recs[firsts[0]].lo;
            bool big = std::max({e.x, e.y, e.z}) > 400;
            if (!world && ((firsts.size() >= 3 && m * 2 > firsts.size()) || (big && firsts.size() > 6))) moves.assign(firsts.size(), true);
        }
        static const bool dbg = std::getenv("BAKE_MESHES") != nullptr;
        if (dbg) {
            size_t m = 0;
            for (size_t k = 0; k < firsts.size(); k++) m += moves[k];
            if (m >= 20) {
                V3 e = recs[firsts[0]].hi - recs[firsts[0]].lo;
                std::printf("  moving %zu of %zu x %s (%s; %.0f %.0f %.0f cm)\n", m, firsts.size(), key.c_str(), all ? "carried or turned" : "trail",
                            e.x, e.y, e.z);
            }
        }
        for (size_t k = 0; k < firsts.size(); k++) {
            placements++;
            if (moves[k]) {
                moving++;
                continue;
            }
            draws.push_back(recs[firsts[k]].d);
        }
        // the identities the game leaves out of its shadow map: every record of a placement baked (a model cut out
        // by its alpha, a fence, a tree, wire, a net, casts its holes in the shadow map: the bake's rays pass it,
        // a receiver alone; the world's sectors, alpha tested all of them, opaque textures, cast)
        for (size_t i : list) {
            const Rec& r = recs[i];
            bool cutout = r.d.kind == 1 && (r.d.flags & 1);
            if (cutout) cut++;
            if (!moves[size_t(r.place)] && !cutout) ids.push_back(r.id);
        }
    }
    std::printf("%zu records: %zu placements, %zu of them moving (left out), %zu records cut out\n", recs.size(), placements, moving, cut);
    // (BAKE_MESHES: the meshes kept most often)
    if (std::getenv("BAKE_MESHES")) {
        std::unordered_map<std::string, size_t> n;
        for (const Draw& d : draws) {
            char key[64];
            std::snprintf(key, sizeof(key), "%llx %zu %zu k%d", (unsigned long long)d.tex, d.pos.size(), d.tris.size(), d.kind);
            n[key]++;
        }
        std::vector<std::pair<size_t, std::string>> by;
        for (auto& [k, c] : n) by.push_back({c, k});
        std::sort(by.rbegin(), by.rend());
        for (size_t i = 0; i < by.size() && i < 12; i++) std::printf("  kept %zu x %s\n", by[i].first, by[i].second.c_str());
    }
    return true;
}

// the eye's way: 8 m voxels it passed through, and every voxel's distance from them (cm, by a sweep)
struct Near {
    V3 lo;
    int nx = 0, ny = 0, nz = 0;
    std::vector<float> dist;
    std::vector<V3> points;  // the played places, one a metre
    std::unordered_map<uint64_t, std::vector<uint32_t>> buckets;  // by 10 m columns (x z)
    static uint64_t Bucket(int x, int z) { return uint64_t(uint32_t(x)) << 32 | uint32_t(z); }
    static constexpr float kVox = 800;
    int SizeAt(V3 p) const {
        float d = At(p);
        return d < kNear[0] ? 0 : d < kNear[1] ? 1 : d < kNear[2] ? 2 : 3;
    }
    float At(V3 p) const {
        if (dist.empty()) return 0;
        int x = std::clamp(int((p.x - lo.x) / kVox), 0, nx - 1), y = std::clamp(int((p.y - lo.y) / kVox), 0, ny - 1),
            z = std::clamp(int((p.z - lo.z) / kVox), 0, nz - 1);
        return dist[(size_t(z) * ny + y) * nx + x];
    }
};

Near ReadEyes(const std::string& base, V3 blo, V3 bhi) {
    Near n;
    std::vector<V3> eyes;
    size_t counts[2] = {};
    // where the eye went (lvNN.eye) and where the soldiers were (lvNN.act): the level's played places
    for (int f = 0; f < 2; f++) {
        std::ifstream in(base + (f ? ".act" : ".eye"), std::ios::binary);
        V3 e;
        while (in.read(reinterpret_cast<char*>(&e), 12))
            if (std::isfinite(e.x + e.y + e.z) && std::fabs(e.x) < 1e6f && std::fabs(e.z) < 1e6f) eyes.push_back(e), counts[f]++;
    }
    std::printf("%zu eye places, %zu soldiers' places\n", counts[0], counts[1]);
    {
        std::unordered_set<uint64_t> seen;
        for (V3 p : eyes) {
            uint64_t k = uint64_t(uint32_t(int(std::floor(p.x / 100)))) * 73856093ull ^ uint64_t(uint32_t(int(std::floor(p.y / 100)))) * 19349663ull ^
                         uint64_t(uint32_t(int(std::floor(p.z / 100)))) * 83492791ull;
            if (!seen.insert(k).second) continue;
            n.buckets[Near::Bucket(int(std::floor(p.x / 1000)), int(std::floor(p.z / 1000)))].push_back(uint32_t(n.points.size()));
            n.points.push_back(p);
        }
    }
    if (eyes.empty()) return n;
    n.lo = blo;
    n.nx = std::max(1, int((bhi.x - blo.x) / Near::kVox) + 1);
    n.ny = std::max(1, int((bhi.y - blo.y) / Near::kVox) + 1);
    n.nz = std::max(1, int((bhi.z - blo.z) / Near::kVox) + 1);
    n.dist.assign(size_t(n.nx) * n.ny * n.nz, 1e30f);
    for (V3 p : eyes) {
        int x = int((p.x - blo.x) / Near::kVox), y = int((p.y - blo.y) / Near::kVox), z = int((p.z - blo.z) / Near::kVox);
        if (x < 0 || y < 0 || z < 0 || x >= n.nx || y >= n.ny || z >= n.nz) continue;
        n.dist[(size_t(z) * n.ny + y) * n.nx + x] = 0;
    }
    // a chamfer sweep, forward and back (26 neighbours' steps): near enough to the true distance
    auto idx = [&](int x, int y, int z) { return (size_t(z) * n.ny + y) * n.nx + x; };
    for (int pass = 0; pass < 2; pass++) {
        int s = pass ? -1 : 1;
        for (int z = pass ? n.nz - 1 : 0; z >= 0 && z < n.nz; z += s)
            for (int y = pass ? n.ny - 1 : 0; y >= 0 && y < n.ny; y += s)
                for (int x = pass ? n.nx - 1 : 0; x >= 0 && x < n.nx; x += s) {
                    float& d = n.dist[idx(x, y, z)];
                    for (int dz = -1; dz <= 1; dz++)
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++) {
                                int X = x + dx, Y = y + dy, Z = z + dz;
                                if (X < 0 || Y < 0 || Z < 0 || X >= n.nx || Y >= n.ny || Z >= n.nz) continue;
                                d = std::min(d, n.dist[idx(X, Y, Z)] + Near::kVox * std::sqrt(float(dx * dx + dy * dy + dz * dz)));
                            }
                }
    }
    return n;
}

int Facing(V3 n) {
    float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    if (ax >= ay && ax >= az) return n.x > 0 ? 0 : 1;
    if (ay >= az) return n.y > 0 ? 2 : 3;
    return n.z > 0 ? 4 : 5;
}

uint64_t Key(int x, int y, int z, int facing, int size) {
    constexpr uint64_t m = (1u << 19) - 1;
    return (uint64_t(uint32_t(x)) & m) | (uint64_t(uint32_t(y)) & m) << 19 | (uint64_t(uint32_t(z)) & m) << 38 |
           uint64_t(facing) << 57 | uint64_t(size) << 60 | 1ull << 63;
}

// the slot (the same in the shader: scene_shaders.h BakeHash)
uint32_t Hash(uint64_t key) {
    uint32_t a = uint32_t(key), b = uint32_t(key >> 32);
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

struct Sample {
    double px = 0, py = 0, pz = 0, nx = 0, ny = 0, nz = 0, w = 0;
};

uint32_t Bits(float v01, int bits) { return uint32_t(std::clamp(int(std::lround(v01 * float((1 << bits) - 1))), 0, (1 << bits) - 1)); }
// a cell's value (32 bits): occlusion (6 bits, 63 none), the sun's reach (6), the sky's (4), the surface's normal
// (octahedral, 5 bits a component), its place along it from the cell's centre (6 bits, signed, of half a cell); bit
// 63 of the 64 kept here marks a cell kept
uint64_t Value(float ao, float sun, float sky, float ox, float oy, float along) {
    uint32_t pl = uint32_t(std::clamp(int(std::lround(along * 31)), -31, 31)) & 63;
    uint32_t v = Bits(ao, 6) | Bits(sun, 6) << 6 | Bits(sky, 4) << 12 | Bits(ox * 0.5f + 0.5f, 5) << 16 | Bits(oy * 0.5f + 0.5f, 5) << 21 | pl << 26;
    return uint64_t(v) | 1ull << 63;
}

// ---- The reflections' probes (--probes lvNN.col): what a reflective surface sees about it, baked ----
// The colours are the game's own: each cell's as it was drawn (d3d12.cpp BakeColourCapture, lvNN.col: the cells'
// average colours, how many of their samples reflected, the sky's colour 128 ways). Probes stand on a grid (--grid,
// 3 m) at the corners of its cubes about every reflective surface near the eye's way; each traces an octahedral map of
// what it sees (y the pole, kTile texels a side and a border copied across the fold for bilinear reads): the colour
// of the cell each ray meets (or the sky's that way) and the distance (the game's parallax and whether a probe sees
// the place it lights). The file (out.refl): "PRB1", version, the grid, the tile, the probes, the tiles a row, the
// atlas's size, raw and packed sizes; then (LZMS) each probe's key (its grid x y z as a cell key, facing 7 size 3) and
// the atlas (RGBA8: the colour's square root of a quarter, the distance's square root of kFar's share).
struct Col {
    float r = 0, g = 0, b = 0;
    uint32_t n = 0, refl = 0;
};
constexpr int kTile = 12;
constexpr int kTileB = kTile + 2;
constexpr int kTilesRow = 256;
constexpr float kFar = 12000;

// octahedral with y the pole (scene_shaders.h ProbeOct)
void OctEncode(V3 d, float& u, float& v) {
    float l1 = std::fabs(d.x) + std::fabs(d.y) + std::fabs(d.z);
    float x = d.x / l1, z = d.z / l1;
    if (d.y < 0) {
        float tx = (1 - std::fabs(z)) * (x >= 0 ? 1.0f : -1.0f), tz = (1 - std::fabs(x)) * (z >= 0 ? 1.0f : -1.0f);
        x = tx, z = tz;
    }
    u = x, v = z;
}
V3 OctDecode(float u, float v) {
    V3 n{u, 1 - std::fabs(u) - std::fabs(v), v};
    if (n.y < 0) {
        float tx = (1 - std::fabs(v)) * (u >= 0 ? 1.0f : -1.0f), tz = (1 - std::fabs(u)) * (v >= 0 ? 1.0f : -1.0f);
        n.x = tx, n.z = tz;
    }
    return Norm(n);
}


int sx19(uint64_t v) { return v >= (1u << 18) ? int(v) - (1 << 19) : int(v); }

// The cells' colours as the game drew them (lvNN.col, d3d12.cpp BakeColourCapture) and the sky's 128 ways
struct Colours {
    std::unordered_map<uint64_t, Col> cells;
    // where no cell of the place was seen (its far side, a corner never in view): the colours of what was seen about
    // it, in 2 m voxels by facing (the lit side of a wall apart from its shaded one), then by the voxel alone
    static constexpr float kVox = 200;
    struct Sum {
        double r = 0, g = 0, b = 0, w = 0;
    };
    std::unordered_map<uint64_t, Sum> vox;
    static uint64_t VoxKey(int x, int y, int z, int facing) { return Key(x, y, z, facing, 0); }
    V3 avg{0.1f, 0.1f, 0.1f};  // the level's (cells weighed by their samples, up to 100)
    V3 sky[128];
    bool Load(const char* path) {
        std::ifstream in(path, std::ios::binary);
        char magic[4];
        uint32_t count = 0;
        if (!in.read(magic, 4) || std::memcmp(magic, "COL1", 4) != 0 || !in.read(reinterpret_cast<char*>(&count), 4)) return false;
        cells.reserve(count);
        double sum[3] = {}, n = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint64_t key;
            Col c;
            if (!in.read(reinterpret_cast<char*>(&key), 8) || !in.read(reinterpret_cast<char*>(&c.r), 12) || !in.read(reinterpret_cast<char*>(&c.n), 4) ||
                !in.read(reinterpret_cast<char*>(&c.refl), 4))
                break;
            cells[key] = c;
            double w = std::min<double>(c.n, 100);
            sum[0] += c.r * w, sum[1] += c.g * w, sum[2] += c.b * w, n += w;
            if (c.n >= 2) {
                constexpr uint64_t m = (1u << 19) - 1;
                int size = int((key >> 60) & 3), facing = int((key >> 57) & 7);
                if (facing > 5) continue;
                float cs = kCell[size];
                float area = cs * cs;
                V3 centre{(float(sx19(key & m)) + 0.5f) * cs, (float(sx19((key >> 19) & m)) + 0.5f) * cs, (float(sx19((key >> 38) & m)) + 0.5f) * cs};
                int vx = int(std::floor(centre.x / kVox)), vy = int(std::floor(centre.y / kVox)), vz = int(std::floor(centre.z / kVox));
                for (int f : {facing, 7}) {
                    Sum& v = vox[VoxKey(vx, vy, vz, f)];
                    v.r += c.r * area, v.g += c.g * area, v.b += c.b * area, v.w += area;
                }
            }
        }
        if (n > 0) avg = {float(sum[0] / n), float(sum[1] / n), float(sum[2] / n)};
        // the sky 128 ways (16 round, 8 up); a way never seen from the nearest seen up or down, else the seen's average
        uint32_t skyn[128] = {};
        bool have_sky = true;
        for (int k = 0; k < 128; k++) {
            if (!in.read(reinterpret_cast<char*>(&sky[k]), 12) || !in.read(reinterpret_cast<char*>(&skyn[k]), 4)) {
                have_sky = false;
                break;
            }
        }
        V3 all{0, 0, 0};
        int seen = 0;
        if (have_sky)
            for (int k = 0; k < 128; k++)
                if (skyn[k] >= 20) all = all + sky[k], seen++;
        all = seen ? all * (1.0f / seen) : V3{0.3f, 0.32f, 0.35f};
        V3 filled[128];
        for (int az = 0; az < 16; az++)
            for (int el = 0; el < 8; el++) {
                int best = -1;
                for (int d = 0; d < 8 && best < 0; d++)
                    for (int e : {el - d, el + d})
                        if (best < 0 && e >= 0 && e < 8 && have_sky && skyn[e * 16 + az] >= 20) best = e * 16 + az;
                filled[el * 16 + az] = best >= 0 ? sky[best] : all;
            }
        std::memcpy(sky, filled, sizeof(sky));
        std::printf("%zu cells' colours, the sky %s (average %.3f %.3f %.3f)\n", cells.size(), have_sky ? "seen" : "none", all.x, all.y, all.z);
        return true;
    }
    V3 Sky(V3 d) const {
        int az = std::clamp(int(std::floor((std::atan2(d.z, d.x) / 6.2831853f + 0.5f) * 16)), 0, 15);
        int el = std::clamp(int(std::floor((d.y + 0.2f) / 1.2f * 8)), 0, 7);
        return sky[el * 16 + az];
    }
    bool Find(int x, int y, int z, int facing, int size, V3& out) const {
        auto it = cells.find(Key(x, y, z, facing, size));
        if (it == cells.end() || it->second.n < 2) return false;
        out = {it->second.r, it->second.g, it->second.b};
        return true;
    }
    // a place's colour: the cell it is in (as the game finds it: scene_shaders.h Baked), else its neighbours' along
    // the surface
    bool At(V3 h, V3 n, const Near& reach, V3& out) const {
        int facing = Facing(n), axis = facing >> 1;
        int first = reach.SizeAt(h);
        for (int size = first; size < std::min(first + 2, 4); size++) {
            float cs = kCell[size];
            float g[3] = {h.x / cs, h.y / cs, h.z / cs};
            int c[3] = {int(std::floor(g[0])), int(std::floor(g[1])), int(std::floor(g[2]))};
            int la = c[axis], alt = la + (g[axis] - float(la) < 0.5f ? -1 : 1);
            int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
            V3 sum{0, 0, 0};
            int got = 0;
            for (int ring = 0; ring < 2 && !got; ring++)
                for (int layer : {la, alt})
                    for (int d1 = -ring; d1 <= ring; d1++)
                        for (int d2 = -ring; d2 <= ring; d2++) {
                            if (ring && std::abs(d1) < ring && std::abs(d2) < ring) continue;
                            int q[3];
                            q[axis] = layer, q[a1] = c[a1] + d1, q[a2] = c[a2] + d2;
                            V3 v;
                            if (Find(q[0], q[1], q[2], facing, size, v)) sum = sum + v, got++;
                        }
            if (got) {
                out = sum * (1.0f / got);
                return true;
            }
        }
        // the voxels: its own by facing, its own, then the 26 about it by facing
        int vx = int(std::floor(h.x / kVox)), vy = int(std::floor(h.y / kVox)), vz = int(std::floor(h.z / kVox));
        for (int f : {facing, 7}) {
            auto it = vox.find(VoxKey(vx, vy, vz, f));
            if (it != vox.end() && it->second.w > 0) {
                const Sum& v = it->second;
                out = {float(v.r / v.w), float(v.g / v.w), float(v.b / v.w)};
                return true;
            }
        }
        Sum acc;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    auto it = vox.find(VoxKey(vx + dx, vy + dy, vz + dz, facing));
                    if (it == vox.end()) continue;
                    acc.r += it->second.r, acc.g += it->second.g, acc.b += it->second.b, acc.w += it->second.w;
                }
        if (acc.w > 0) {
            out = {float(acc.r / acc.w), float(acc.g / acc.w), float(acc.b / acc.w)};
            return true;
        }
        return false;
    }
};

int BakeProbes(RTCScene scene, const Near& reach, const Colours& col, const char* outpath, float grid) {
    auto t0 = std::chrono::steady_clock::now();
    // the probes: the corners of the grid's cubes about each reflective surface near the eye's way (a cell at the
    // finer two sizes, a third of its samples reflective), raised off it by half a metre (the game's too)
    constexpr float kBias = 50;
    std::unordered_map<uint64_t, uint32_t> index;
    std::vector<std::array<int, 3>> where;
    size_t reflective = 0;
    for (auto& [key, c] : col.cells) {
        int size = int((key >> 60) & 3), facing = int((key >> 57) & 7);
        if (size > 1 || facing > 5 || c.n < 4 || c.refl * 3 < c.n) continue;
        reflective++;
        float cs = kCell[size];
        constexpr uint64_t m = (1u << 19) - 1;
        V3 centre{(float(sx19(key & m)) + 0.5f) * cs, (float(sx19((key >> 19) & m)) + 0.5f) * cs, (float(sx19((key >> 38) & m)) + 0.5f) * cs};
        V3 nrm{0, 0, 0};
        (&nrm.x)[facing >> 1] = (facing & 1) ? -1.0f : 1.0f;
        V3 lo = centre + nrm * kBias - V3{cs * 0.5f + 10, cs * 0.5f + 10, cs * 0.5f + 10};
        V3 hi = centre + nrm * kBias + V3{cs * 0.5f + 10, cs * 0.5f + 10, cs * 0.5f + 10};
        for (int gz = int(std::floor(lo.z / grid)); gz <= int(std::floor(hi.z / grid)) + 1; gz++)
            for (int gy = int(std::floor(lo.y / grid)); gy <= int(std::floor(hi.y / grid)) + 1; gy++)
                for (int gx = int(std::floor(lo.x / grid)); gx <= int(std::floor(hi.x / grid)) + 1; gx++) {
                    uint64_t k = Key(gx, gy, gz, 7, 3);
                    if (index.emplace(k, uint32_t(where.size())).second) where.push_back({gx, gy, gz});
                }
    }
    std::printf("%zu reflective cells: %zu probes (a grid of %.0f cm)\n", reflective, where.size(), grid);
    const uint32_t n = uint32_t(where.size());
    const uint32_t rows = (n + kTilesRow - 1) / kTilesRow;
    const uint32_t aw = kTilesRow * kTileB, ah = std::max(1u, rows) * kTileB;
    std::vector<uint8_t> atlas(size_t(aw) * ah * 4, 0);
    std::atomic<uint32_t> next{0};
    std::atomic<size_t> unknown{0}, hits{0};
    auto work = [&](unsigned seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> U(0, 1);
        std::vector<float> tex(size_t(kTile) * kTile * 4);
        for (;;) {
            uint32_t i = next.fetch_add(1);
            if (i >= n) break;
            V3 o{where[i][0] * grid, where[i][1] * grid, where[i][2] * grid};
            V3 known{0, 0, 0};
            int known_n = 0;
            struct Ray {
                V3 c;
                float t;
                bool hit, have;
            };
            std::vector<Ray> res(size_t(kTile) * kTile * 9);
            for (int tj = 0; tj < kTile; tj++)
                for (int ti = 0; ti < kTile; ti++)
                    for (int r = 0; r < 9; r++) {
                        float u = (float(ti) + (float(r % 3) + U(rng)) / 3) / kTile * 2 - 1;
                        float v = (float(tj) + (float(r / 3) + U(rng)) / 3) / kTile * 2 - 1;
                        V3 dir = OctDecode(u, v);
                        RTCRayHit rh{};
                        rh.ray.org_x = o.x, rh.ray.org_y = o.y, rh.ray.org_z = o.z;
                        rh.ray.dir_x = dir.x, rh.ray.dir_y = dir.y, rh.ray.dir_z = dir.z;
                        rh.ray.tnear = 0.0f, rh.ray.tfar = kFar, rh.ray.mask = 0xFFFFFFFF;
                        rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                        rtcIntersect1(scene, &rh);
                        Ray& out = res[(size_t(tj) * kTile + ti) * 9 + r];
                        if (rh.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
                            out = {col.Sky(dir), kFar, false, true};
                            continue;
                        }
                        V3 ng = Norm({rh.hit.Ng_x, rh.hit.Ng_y, rh.hit.Ng_z});
                        if (Dot(ng, dir) > 0) ng = ng * -1.0f;
                        V3 h = o + dir * rh.ray.tfar;
                        V3 c;
                        bool have = col.At(h, ng, reach, c);
                        out = {c, rh.ray.tfar, true, have};
                        if (have) known = known + c, known_n++;
                    }
            V3 fill = known_n ? known * (1.0f / known_n) : col.avg;
            size_t unk = 0, hit = 0;
            for (int t = 0; t < kTile * kTile; t++) {
                V3 sum{0, 0, 0};
                float dist = 0;
                for (int r = 0; r < 9; r++) {
                    const Ray& q = res[size_t(t) * 9 + r];
                    sum = sum + (q.have ? q.c : fill);
                    dist += q.t;
                    hit += q.hit, unk += q.hit && !q.have;
                }
                tex[t * 4 + 0] = sum.x / 9, tex[t * 4 + 1] = sum.y / 9, tex[t * 4 + 2] = sum.z / 9, tex[t * 4 + 3] = dist / 9;
            }
            unknown += unk, hits += hit;
            // the tile with its border: a border texel the interior one across the fold
            uint32_t tx = (i % kTilesRow) * kTileB, ty = (i / kTilesRow) * kTileB;
            for (int bj = -1; bj <= kTile; bj++)
                for (int bi = -1; bi <= kTile; bi++) {
                    int si = bi, sj = bj;
                    if (si < 0 || si >= kTile) si = si < 0 ? 0 : kTile - 1, sj = kTile - 1 - sj;
                    if (sj < 0 || sj >= kTile) sj = sj < 0 ? 0 : kTile - 1, si = kTile - 1 - si;
                    const float* t = &tex[(size_t(sj) * kTile + si) * 4];
                    uint8_t* d = &atlas[((size_t(ty) + bj + 1) * aw + tx + bi + 1) * 4];
                    for (int ch = 0; ch < 3; ch++) d[ch] = uint8_t(std::lround(std::sqrt(std::clamp(t[ch] / 4, 0.0f, 1.0f)) * 255));
                    d[3] = uint8_t(std::lround(std::sqrt(std::clamp(t[3] / kFar, 0.0f, 1.0f)) * 255));
                }
        }
    };
    std::vector<std::thread> threads;
    unsigned nt = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned k = 0; k < nt; k++) threads.emplace_back(work, 777 + k);
    for (auto& th : threads) th.join();
    std::vector<uint8_t> raw;
    raw.reserve(size_t(n) * 8 + atlas.size());
    for (auto& w : where) {
        uint64_t k = Key(w[0], w[1], w[2], 7, 3);
        raw.insert(raw.end(), reinterpret_cast<uint8_t*>(&k), reinterpret_cast<uint8_t*>(&k) + 8);
    }
    raw.insert(raw.end(), atlas.begin(), atlas.end());
    COMPRESSOR_HANDLE comp = nullptr;
    std::vector<uint8_t> packed(raw.size() + (1 << 20));
    SIZE_T packed_n = 0;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &comp) || !Compress(comp, raw.data(), raw.size(), packed.data(), packed.size(), &packed_n)) {
        std::printf("cannot pack\n");
        return 1;
    }
    CloseCompressor(comp);
    struct {
        char magic[4];
        uint32_t version;
        float grid;
        uint32_t tile, count, row, w, h;
        uint64_t raw, packed;
    } head{{'P', 'R', 'B', '1'}, 1, grid, uint32_t(kTile), n, uint32_t(kTilesRow), aw, ah, raw.size(), packed_n};
    std::ofstream out(outpath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&head), sizeof(head));
    out.write(reinterpret_cast<const char*>(packed.data()), std::streamsize(packed_n));
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%u probes, %.1f%% of the rays' hits with no colour seen; an atlas of %ux%u, %.1f MB packed to %.1f MB; %.1f s\n", n,
                hits ? 100.0 * double(unknown) / double(hits) : 0.0, aw, ah, raw.size() / 1048576.0, packed_n / 1048576.0, secs);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("bake <lvNN.geo> <out.bake> [--rays N] [--radius cm] [--prune 1] [--colours lvNN.col]\n"
                    "bake <lvNN.geo> <out.refl> --probes <lvNN.col> [--grid cm]   (the reflections' probes)\n");
        return 1;
    }
    int rays = 128;
    float radius = 200;
    bool prune = false;  // --prune: cells no played place sees left out
    const char* probes = nullptr;
    const char* colours_path = nullptr;  // --colours: the bounced light too (version 6)
    float grid = 300;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--probes")) probes = argv[i + 1];
        if (!std::strcmp(argv[i], "--colours")) colours_path = argv[i + 1];
        if (!std::strcmp(argv[i], "--grid")) grid = float(std::atof(argv[i + 1]));
        if (!std::strcmp(argv[i], "--rays")) rays = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--radius")) radius = float(std::atof(argv[i + 1]));
        if (!std::strcmp(argv[i], "--prune")) prune = std::atoi(argv[i + 1]) != 0;
    }
    auto t0 = std::chrono::steady_clock::now();
    std::vector<Draw> draws;
    std::vector<uint64_t> ids;
    if (!ReadGeo(argv[1], draws, ids)) {
        std::printf("cannot read %s\n", argv[1]);
        return 1;
    }
    // the scene for the rays: every triangle
    RTCDevice device = rtcNewDevice(nullptr);
    RTCScene scene = rtcNewScene(device);
    size_t tri_total = 0;
    V3 blo{1e30f, 1e30f, 1e30f}, bhi{-1e30f, -1e30f, -1e30f};
    for (const Draw& d : draws) {
        if (d.kind == 1 && (d.flags & 1)) continue;  // (cut out by its alpha: the shadow map casts it)
        RTCGeometry g = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
        float* vb = static_cast<float*>(rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 12, d.pos.size()));
        std::memcpy(vb, d.pos.data(), d.pos.size() * 12);
        uint32_t* ib = static_cast<uint32_t*>(rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 12, d.tris.size() / 3));
        std::memcpy(ib, d.tris.data(), d.tris.size() * 4);
        rtcCommitGeometry(g);
        rtcAttachGeometry(scene, g);
        rtcReleaseGeometry(g);
        tri_total += d.tris.size() / 3;
        for (V3 p : d.pos) blo = {std::min(blo.x, p.x), std::min(blo.y, p.y), std::min(blo.z, p.z)}, bhi = {std::max(bhi.x, p.x), std::max(bhi.y, p.y), std::max(bhi.z, p.z)};
    }
    rtcSetSceneBuildQuality(scene, RTC_BUILD_QUALITY_HIGH);
    rtcCommitScene(scene);
    std::printf("%zu triangles\n", tri_total);
    std::string geo = argv[1];
    Near reach = ReadEyes(geo.substr(0, geo.size() - 4), blo, bhi);
    if (probes) {
        Colours col;
        if (!col.Load(probes)) {
            std::printf("cannot read %s\n", probes);
            return 1;
        }
        int r = BakeProbes(scene, reach, col, argv[2], grid);
        rtcReleaseScene(scene);
        rtcReleaseDevice(device);
        return r;
    }
    Colours colours;
    const bool bounce = colours_path && colours.Load(colours_path);
    if (colours_path && !bounce) std::printf("cannot read %s: no bounced light\n", colours_path);
    // which way a one sided triangle's front faces: its winding's normal times the draw's (front counter clockwise,
    // mirrored) times the game's own convention, found here: what is drawn one sided near the eye faces it
    // (by the kind of draw: the world's sectors, other fixed function draws, the programs' models; one undecided both
    // sides)
    auto group = [](const Draw& d) { return d.kind == 1 ? 2 : (d.flags & 2) ? 0 : 1; };
    double vote[3] = {}, weight[3] = {};
    std::vector<double> dvote(draws.size(), 0), dweight(draws.size(), 0);  // each draw's own
    for (size_t di = 0; di < draws.size(); di++) {
        const Draw& d = draws[di];
        if (!(d.flags & 4)) continue;
        float sign = ((d.flags & 8) ? -1.0f : 1.0f) * ((d.flags & 16) ? -1.0f : 1.0f);
        for (size_t t = 0; t + 2 < d.tris.size(); t += 3) {
            V3 a = d.pos[d.tris[t]], b = d.pos[d.tris[t + 1]], c = d.pos[d.tris[t + 2]];
            V3 m = (a + b + c) * (1.0f / 3);
            if (reach.At(m) > 1500) continue;
            V3 cr = Cross(b - a, c - a);
            float area = 0.5f * Len(cr);
            if (area < 100) continue;
            // the nearest played place in its column
            int bx = int(std::floor(m.x / 1000)), bz = int(std::floor(m.z / 1000));
            float best = 1e30f;
            V3 e{};
            for (int dz = -1; dz <= 1; dz++)
                for (int dx = -1; dx <= 1; dx++) {
                    auto it = reach.buckets.find(Near::Bucket(bx + dx, bz + dz));
                    if (it == reach.buckets.end()) continue;
                    for (uint32_t k : it->second) {
                        float l = Len(reach.points[k] - m);
                        if (l < best) best = l, e = reach.points[k];
                    }
                }
            if (best > 1500) continue;
            // (seen from there: nothing between; what the eye sees of a one sided draw is its front)
            V3 to = e - m;
            float l = Len(to);
            V3 dir = to * (1 / std::max(l, 1e-3f));
            V3 o = m + dir * 2.0f;
            RTCRay ray{};
            ray.org_x = o.x, ray.org_y = o.y, ray.org_z = o.z;
            ray.dir_x = dir.x, ray.dir_y = dir.y, ray.dir_z = dir.z;
            ray.tnear = 0.0f, ray.tfar = std::max(l - 4.0f, 0.0f), ray.mask = 0xFFFFFFFF;
            rtcOccluded1(scene, &ray);
            if (ray.tfar < 0) continue;
            double v = (Dot(cr, to) > 0 ? 1.0 : -1.0) * sign * std::min(area, 10000.0f);
            vote[group(d)] += v, weight[group(d)] += std::min(area, 10000.0f);
            dvote[di] += v, dweight[di] += std::min(area, 10000.0f);
        }
    }
    float convention[3];
    const char* names[3] = {"world", "fixed function", "models"};
    for (int g = 0; g < 3; g++) {
        double share = weight[g] > 0 ? vote[g] / weight[g] : 0;
        convention[g] = share > 0.2 ? 1.0f : share < -0.2 ? -1.0f : 0.0f;  // (two faced planes, both faces seen, water it down)
        std::printf("one sided fronts, %s: %s (%.2f of %.0f)\n", names[g], convention[g] > 0 ? "as wound" : convention[g] < 0 ? "reversed" : "both sides",
                    share, weight[g]);
    }
    // the samples: each triangle's area spread over the cells it covers, at its size
    std::unordered_map<uint64_t, Sample> cells;
    size_t own = 0, by_group = 0, both = 0, own_as[3] = {}, own_rev[3] = {};
    for (size_t di = 0; di < draws.size(); di++) {
        const Draw& d = draws[di];
        // which side: the draw's own vote where it has one (a square metre seen, three quarters one way), else its
        // kind's, else both
        float conv = 0;
        if (dweight[di] >= 10000 && std::fabs(dvote[di]) >= 0.5 * dweight[di]) {
            conv = dvote[di] > 0 ? 1.0f : -1.0f, own++;
            (conv > 0 ? own_as : own_rev)[group(d)]++;
        }
        else if (convention[group(d)] != 0) conv = convention[group(d)], by_group++;
        else both++;
        for (size_t t = 0; t + 2 < d.tris.size(); t += 3) {
            V3 a = d.pos[d.tris[t]], b = d.pos[d.tris[t + 1]], c = d.pos[d.tris[t + 2]];
            V3 cr = Cross(b - a, c - a);
            float area = 0.5f * Len(cr);
            if (area < 1e-2f) continue;
            V3 n0 = cr * (1 / (2 * area));
            // each place's cell at its voxel's size (the game reads the same map): the triangle sampled for the
            // finest one about it
            int finest = 3;
            for (V3 q : {a, b, c, (a + b) * 0.5f, (b + c) * 0.5f, (a + c) * 0.5f, (a + b + c) * (1.0f / 3)}) finest = std::min(finest, reach.SizeAt(q));
            float cs = kCell[finest];
            // both sides: a triangle's winding does not tell which side is seen (some are drawn from either side);
            // the side never seen is never looked up
            // (one sided: its front's side alone)
            float front = conv * ((d.flags & 8) ? -1.0f : 1.0f) * ((d.flags & 16) ? -1.0f : 1.0f);
            for (int side = 0; side < 2; side++) {
            if ((d.flags & 4) && front != 0 && (side ? -front : front) < 0) continue;
            V3 n = side ? n0 * -1.0f : n0;
            int facing = Facing(n);
            float span = std::max({Len(b - a), Len(c - a), Len(c - b)});
            int steps = std::clamp(int(std::ceil(span / (cs * 0.5f))), 1, 4000);
            float w = area / (float(steps + 1) * float(steps + 2) * 0.5f);
            for (int i = 0; i <= steps; i++)
                for (int j = 0; i + j <= steps; j++) {
                    // (its edges and corners too, a hair inside: a ground's cells along its triangles' edges had none
                    // and read as lines across it)
                    float u = float(i) / steps, v = float(j) / steps;
                    V3 p = a + (b - a) * u + (c - a) * v;
                    p = p + ((a + b + c) * (1.0f / 3) - p) * 0.02f;
                    int size = reach.SizeAt(p);
                    float pcs = kCell[size];
                    int x = int(std::floor(p.x / pcs)), y = int(std::floor(p.y / pcs)), z = int(std::floor(p.z / pcs));
                    Sample& s = cells[Key(x, y, z, facing, size)];
                    s.px += p.x * w, s.py += p.y * w, s.pz += p.z * w;
                    s.nx += n.x * w, s.ny += n.y * w, s.nz += n.z * w;
                    s.w += w;
                }
            }
        }
    }
    std::printf("sides: %zu draws by their own view, %zu by their kind's, %zu both\n", own, by_group, both);
    std::printf("  their own: world %zu as wound %zu reversed, models %zu as wound %zu reversed\n", own_as[0], own_rev[0], own_as[2], own_rev[2]);
    size_t per_size[4] = {};
    for (auto& [k, s] : cells) per_size[(k >> 60) & 3]++;
    std::printf("%zu cells (%zu, %zu, %zu, %zu by size)\n", cells.size(), per_size[0], per_size[1], per_size[2], per_size[3]);
    // the sun (towards it, lvNN.sun from the capture): none, no shadows baked (their value all lit)
    V3 sun{0, 0, 0};
    bool have_sun = false;
    {
        std::ifstream in(geo.substr(0, geo.size() - 4) + ".sun", std::ios::binary);
        if (in.read(reinterpret_cast<char*>(&sun), 12) && Len(sun) > 0.5f) sun = Norm(sun), have_sun = true;
        std::printf("sun %s %.3f %.3f %.3f\n", have_sun ? "at" : "none", sun.x, sun.y, sun.z);
    }
    // the air over the floors near the eye's way (half metre cells, facing 6, up to 3 m over a floor): the sun's
    // reach there for what moves (a soldier in a building's shadow)
    size_t volume = 0;
    if (have_sun) {
        std::vector<uint64_t> add;
        for (auto& [k, s] : cells) {
            int facing = int((k >> 57) & 7), size = int((k >> 60) & 3);
            if (facing != 2 || size > 0) continue;
            V3 p{float(s.px / s.w), float(s.py / s.w), float(s.pz / s.w)};
            int x = int(std::floor(p.x / kCell[1])), z = int(std::floor(p.z / kCell[1])), y0 = int(std::floor(p.y / kCell[1]));
            for (int y = y0; y <= y0 + 5; y++) add.push_back(Key(x, y, z, 6, 1));
        }
        for (uint64_t k : add)
            if (cells.emplace(k, Sample{}).second) volume++;
        std::printf("%zu cells of air\n", volume);
    }
    // the rays
    std::vector<std::pair<uint64_t, Sample>> list(cells.begin(), cells.end());
    std::vector<uint64_t> values(list.size());
    std::vector<uint32_t> bounces(list.size(), 0);
    std::atomic<size_t> next{0};
    auto work = [&](unsigned seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> U(0, 1);
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= list.size()) break;
            const Sample& s = list[i].second;
            uint64_t key = list[i].first;
            int size = int((key >> 60) & 3);
            float cs = kCell[size];
            int cx = int((key >> 0) & ((1u << 19) - 1)), cy = int((key >> 19) & ((1u << 19) - 1)), cz = int((key >> 38) & ((1u << 19) - 1));
            auto sx = [](int v) { return v >= (1 << 18) ? v - (1 << 19) : v; };
            V3 centre{(float(sx(cx)) + 0.5f) * cs, (float(sx(cy)) + 0.5f) * cs, (float(sx(cz)) + 0.5f) * cs};
            // the sun from a point: the share of a disc of 1 degree about it that rays reach
            auto sun_from = [&](V3 o, V3 n, int count) {
                V3 t = Norm(std::fabs(sun.y) < 0.9f ? Cross({0, 1, 0}, sun) : Cross({1, 0, 0}, sun));
                V3 bt = Cross(sun, t);
                int lit = 0;
                for (int r = 0; r < count; r++) {
                    float rr = std::sqrt((r + U(rng)) / count) * 0.01745f, ph = 6.2831853f * U(rng);
                    V3 dir = Norm(sun + t * (rr * std::cos(ph)) + bt * (rr * std::sin(ph)));
                    RTCRay ray{};
                    ray.org_x = o.x, ray.org_y = o.y, ray.org_z = o.z;
                    ray.dir_x = dir.x, ray.dir_y = dir.y, ray.dir_z = dir.z;
                    ray.tnear = 0.0f, ray.tfar = 1e7f, ray.mask = 0xFFFFFFFF;
                    // (a surface lying on this one: the first few centimetres left out)
                    if (Len(n) > 0.5f) ray.tnear = 6.0f;
                    rtcOccluded1(scene, &ray);
                    if (ray.tfar >= 0) lit++;
                }
                return float(lit) / float(count);
            };
            if (((key >> 57) & 7) == 6) {
                // the air: the sun alone (no occlusion, no normal)
                uint32_t su = uint32_t(std::clamp(int(std::lround(sun_from(centre, {0, 0, 0}, 32) * 255)), 0, 255));
                values[i] = Value(1, su / 255.0f, 1, 0, 0, 0);
                continue;
            }
            V3 p{float(s.px / s.w), float(s.py / s.w), float(s.pz / s.w)};
            V3 n = Norm({float(s.nx), float(s.ny), float(s.nz)});
            // (--prune) seen from where the level is played: within 3 m of a played place, or in sight of one of the
            // nearest (50 m) on its side; else never seen (the inside of a building's shell), left out
            if (prune && !reach.points.empty()) {
                bool seen = false;
                std::vector<std::pair<float, V3>> near_pts;
                int bx = int(std::floor(p.x / 1000)), bz = int(std::floor(p.z / 1000));
                for (int dz = -5; dz <= 5 && !seen; dz++)
                    for (int dx = -5; dx <= 5 && !seen; dx++) {
                        auto it = reach.buckets.find(Near::Bucket(bx + dx, bz + dz));
                        if (it == reach.buckets.end()) continue;
                        for (uint32_t k : it->second) {
                            V3 q = reach.points[k];
                            V3 d = q - p;
                            float l = Len(d);
                            if (l < 300) {
                                seen = true;
                                break;
                            }
                            if (l < 5000 && Dot(d, n) > 0) near_pts.push_back({l, q});
                        }
                    }
                if (!seen) {
                    std::sort(near_pts.begin(), near_pts.end(), [](auto& x, auto& y) { return x.first < y.first; });
                    for (size_t k = 0; k < near_pts.size() && k < 16 && !seen; k++) {
                        V3 d = near_pts[k].second - p;
                        float l = Len(d);
                        RTCRay ray{};
                        V3 o = p + n * 2.0f;
                        ray.org_x = o.x, ray.org_y = o.y, ray.org_z = o.z;
                        ray.dir_x = d.x / l, ray.dir_y = d.y / l, ray.dir_z = d.z / l;
                        ray.tnear = 0.0f, ray.tfar = l - 20, ray.mask = 0xFFFFFFFF;
                        rtcOccluded1(scene, &ray);
                        if (ray.tfar >= 0) seen = true;
                    }
                }
                if (!seen) {
                    values[i] = 0;
                    continue;
                }
            }
            // the tangent frame
            V3 t = Norm(std::fabs(n.y) < 0.9f ? Cross({0, 1, 0}, n) : Cross({1, 0, 0}, n));
            V3 bt = Cross(n, t);
            V3 o = p + n * 1.5f;
            float occ = 0, sky = 0;
            int close = 0;  // rays stopped within half a metre
            // the light bounced off what the rays meet near by (the cells' colours as drawn, nearer counting more:
            // the screen's bounce's falloff, 2.5 m); what has no colour (never seen) left out of the share
            constexpr float kBounceR = 250;
            V3 bsum{0, 0, 0};
            int bnear = 0, bknown = 0;
            for (int r = 0; r < rays; r++) {
                // cosine weighted (stratified)
                float u1 = (float(r % 16) + U(rng)) / 16, u2 = (float(r / 16) + U(rng)) / float((rays + 15) / 16);
                float rr = std::sqrt(u1), ph = 6.2831853f * u2;
                V3 dir = Norm(t * (rr * std::cos(ph)) + bt * (rr * std::sin(ph)) + n * std::sqrt(std::max(0.0f, 1 - u1)));
                RTCRayHit rh{};
                rh.ray.org_x = o.x, rh.ray.org_y = o.y, rh.ray.org_z = o.z;
                rh.ray.dir_x = dir.x, rh.ray.dir_y = dir.y, rh.ray.dir_z = dir.z;
                rh.ray.tnear = 0.0f, rh.ray.tfar = 1e7f, rh.ray.mask = 0xFFFFFFFF;
                rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                rtcIntersect1(scene, &rh);
                // a surface lying on this one, close and parallel (a decal's mesh, rubble or a puddle laid over the
                // floor, the same wall drawn twice): passed through
                float travelled = 0;
                for (int skip = 0; skip < 3 && rh.hit.geomID != RTC_INVALID_GEOMETRY_ID && travelled + rh.ray.tfar < 6; skip++) {
                    V3 ng = Norm({rh.hit.Ng_x, rh.hit.Ng_y, rh.hit.Ng_z});
                    if (std::fabs(Dot(ng, n)) < 0.95f) break;
                    travelled += rh.ray.tfar + 0.2f;
                    V3 q = o + dir * travelled;
                    rh.ray.org_x = q.x, rh.ray.org_y = q.y, rh.ray.org_z = q.z;
                    rh.ray.tnear = 0.0f, rh.ray.tfar = 1e7f;
                    rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                    rtcIntersect1(scene, &rh);
                }
                if (rh.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
                    if (dir.y > 0) sky += 1;
                    continue;
                }
                float d = (rh.ray.tfar + travelled) / radius;
                if (rh.ray.tfar + travelled < 50) close++;
                if (bounce && rh.ray.tfar + travelled < kBounceR * 3) {
                    float dist = rh.ray.tfar + travelled;
                    V3 ng = Norm({rh.hit.Ng_x, rh.hit.Ng_y, rh.hit.Ng_z});
                    if (Dot(ng, dir) > 0) ng = ng * -1.0f;
                    V3 c;
                    bnear++;
                    if (colours.At(o + dir * dist, ng, reach, c)) bsum = bsum + c * (1 / (1 + dist * dist / (kBounceR * kBounceR * 0.25f))), bknown++;
                }
                if (d < 1) occ += (1 - d) * (1 - d);
            }
            // a side inside a wall or a solid model (the far side of a one sided surface baked as both): shut in all
            // round, never seen; left out
            if (close >= rays * 97 / 100) {
                values[i] = 0;
                continue;
            }
            occ /= float(rays), sky /= float(rays);
            uint32_t ao = uint32_t(std::clamp(int(std::lround((1 - occ) * 255)), 0, 255));
            uint32_t sk = uint32_t(std::clamp(int(std::lround(sky * 255)), 0, 255));
            // the sun: none on what faces away from it
            uint32_t su = 255;
            if (have_sun) su = Dot(n, sun) <= 0.02f ? 0 : uint32_t(std::clamp(int(std::lround(sun_from(p + n * 1.5f, n, 32) * 255)), 0, 255));
            // the normal, octahedral; the sample's place along it from the cell's centre, in half cells
            float l1 = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
            float ox = n.x / l1, oy = n.y / l1;
            if (n.z < 0) {
                float tx = (1 - std::fabs(oy)) * (ox >= 0 ? 1 : -1), ty = (1 - std::fabs(ox)) * (oy >= 0 ? 1 : -1);
                ox = tx, oy = ty;
            }
            float along = Dot(p - centre, n) / (cs * 0.5f);
            values[i] = Value(ao / 255.0f, su / 255.0f, sk / 255.0f, ox, oy, along);
            if (bknown) {
                // (10 bits a channel: the square root of half of it)
                V3 b = bsum * (float(bnear) / float(bknown) / float(rays));
                auto q = [](float v) { return uint32_t(std::lround(std::sqrt(std::clamp(v * 0.5f, 0.0f, 1.0f)) * 1023)); };
                bounces[i] = q(b.x) | q(b.y) << 10 | q(b.z) << 20;
            }
        }
    };
    std::vector<std::thread> threads;
    unsigned nt = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned k = 0; k < nt; k++) threads.emplace_back(work, 1234 + k);
    for (auto& th : threads) th.join();
    // the file: the head, then (packed, LZMS) the entries (key 64 bits, value 32) and the static draws' identities
    // (their count, 64 bits each); the game lays the entries out in its table of 64 bit slots (a fingerprint of the
    // key, the value): half full, a key's first slot by Hash, then the next ones
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::vector<uint8_t> raw;
    raw.reserve(list.size() * 16 + 4 + ids.size() * 8);
    auto put = [&](const void* p, size_t n) { raw.insert(raw.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    size_t kept = 0;
    for (size_t i = 0; i < list.size(); i++)
        if (values[i]) {
            uint32_t v = uint32_t(values[i]);
            put(&list[i].first, 8), put(&v, 4), kept++;
            if (bounce) put(&bounces[i], 4);
        }
    uint32_t tsize = uint32_t(std::max<size_t>(kept + 1, kept * 2));  // (half full: a cell not there is found out in a slot or two)
    uint32_t nid = uint32_t(ids.size());
    put(&nid, 4);
    put(ids.data(), ids.size() * 8);
    // the cells' size by place (Near's voxels, 2 bits each, 16 to 64 bits): its corner, voxel size, counts, the bits
    {
        float lo[4] = {reach.lo.x, reach.lo.y, reach.lo.z, Near::kVox};
        uint32_t dims[3] = {uint32_t(reach.nx), uint32_t(reach.ny), uint32_t(reach.nz)};
        if (reach.dist.empty()) dims[0] = dims[1] = dims[2] = 0;
        put(lo, 16), put(dims, 12);
        size_t voxels = size_t(dims[0]) * dims[1] * dims[2];
        std::vector<uint64_t> bits((voxels + 15) / 16, 0);
        for (size_t v = 0; v < voxels; v++) {
            float d = reach.dist[v];
            uint64_t sz = d < kNear[0] ? 0 : d < kNear[1] ? 1 : d < kNear[2] ? 2 : 3;
            bits[v / 16] |= sz << ((v % 16) * 2);
        }
        put(bits.data(), bits.size() * 8);
    }
    COMPRESSOR_HANDLE comp = nullptr;
    std::vector<uint8_t> packed(raw.size() + (1 << 20));
    SIZE_T packed_n = 0;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &comp) ||
        !Compress(comp, raw.data(), raw.size(), packed.data(), packed.size(), &packed_n)) {
        std::printf("cannot pack\n");
        return 1;
    }
    CloseCompressor(comp);
    std::ofstream out(argv[2], std::ios::binary);
    struct {
        char magic[4];
        uint32_t version;
        float cells[4];
        uint32_t count, size;
        float sun[3];
        uint32_t have_sun;
        uint64_t raw, packed;
    } head{{'B', 'A', 'K', '1'}, bounce ? 6u : 5u, {kCell[0], kCell[1], kCell[2], kCell[3]}, uint32_t(kept), tsize, {sun.x, sun.y, sun.z},
           have_sun ? 1u : 0u, raw.size(), packed_n};
    out.write(reinterpret_cast<const char*>(&head), sizeof(head));
    out.write(reinterpret_cast<const char*>(packed.data()), std::streamsize(packed_n));
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%zu cells (%zu shut in left out), %u static draws: %.1f MB packed to %.1f MB (a table of %u slots, %.1f MB, in the game); %.1f s\n",
                kept, list.size() - kept, nid, raw.size() / 1048576.0, packed_n / 1048576.0, tsize, tsize * 8 / 1048576.0, secs);
    rtcReleaseScene(scene);
    rtcReleaseDevice(device);
    return 0;
}
