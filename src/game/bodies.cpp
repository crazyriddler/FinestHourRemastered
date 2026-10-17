// The characters' bodies past their animation: RenderWare's skinning (sub_001ECCB0, _rpSkinD3D8PrepareMatrices) reads
// each skinned atomic's bone matrices from its RpHAnimHierarchy (world space, parents before children). Changed there,
// once a frame for each hierarchy, they change only how the body is drawn: the game's own animation, collision and AI
// go on as they were.
//
// Hit reactions: a bullet hitting a soldier (sub_000E78C0, the soldier's HitByBullet: the victim, the hit's record with
// its attacker and the part hit) gives his spine, chest, neck and head a push away from the shooter on a damped spring,
// added to the animation: a flinch of a few degrees that settles in half a second.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {
constexpr uint32_t kSkinAtomicOffset = 0x3F2C14;  // the skin plugin's offset into an atomic: its hierarchy pointer
constexpr int kSoldierNodes = 51;                  // the soldiers' skeleton

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Norm(V3 a) {
    float l = Len(a);
    return l > 1e-6f ? a * (1 / l) : V3{0, 1, 0};
}

// a rotation by the vector's length about its direction (Rodrigues)
V3 Rotate(V3 v, V3 axis_angle) {
    float a = Len(axis_angle);
    if (a < 1e-7f) return v;
    V3 k = axis_angle * (1 / a);
    float c = std::cos(a), s = std::sin(a);
    return v * c + Cross(k, v) * s + k * (Dot(k, v) * (1 - c));
}

V3 ReadV3(uint32_t va) {
    V3 v;
    std::memcpy(&v, HostPtr(va), 12);
    return v;
}
void WriteV3(uint32_t va, V3 v) { std::memcpy(HostPtr(va), &v, 12); }

// --- the flinch -----------------------------------------------------------------------------------------------------

// the bones a flinch turns, by node id: spine, chest, neck, head, and their share of the turn
struct Joint {
    int id;
    float share;
};
constexpr Joint kFlinch[] = {{110, 0.25f}, {111, 0.25f}, {112, 0.2f}, {452, 0.15f}, {200, 0.15f}};
constexpr int kFlinchJoints = int(std::size(kFlinch));

// --- the skeleton's points (the feet's, Snow Tracks) -------------------------------------------------------------------------------------------

enum Point { kPelvis, kChest, kNeck, kHead, kLShoulder, kLElbow, kLHand, kRShoulder, kRElbow, kRHand, kLHip, kLKnee, kLFoot,
             kRHip, kRKnee, kRFoot, kPoints };
// their nodes, by id, and their spheres' radii (cm)
constexpr int kPointNode[kPoints] = {1, 112, 452, 200, 135, 140, 145, 235, 240, 245, 2, 3, 4, 6, 7, 8};
// a bone's world matrix: right, up, at, position (RwMatrix, rows of 16 bytes)
struct M4 {
    V3 r, u, a, p;
};
M4 ReadM(uint32_t va) { return {ReadV3(va), ReadV3(va + 16), ReadV3(va + 32), ReadV3(va + 48)}; }
void WriteM(uint32_t va, const M4& m) { WriteV3(va, m.r), WriteV3(va + 16, m.u), WriteV3(va + 32, m.a), WriteV3(va + 48, m.p); }
// a point or a direction in a frame's coordinates and back (its axes orthogonal, of any length)
V3 ToLocal(const M4& m, V3 v) {
    return {Dot(v, m.r) / std::max(Dot(m.r, m.r), 1e-12f), Dot(v, m.u) / std::max(Dot(m.u, m.u), 1e-12f),
            Dot(v, m.a) / std::max(Dot(m.a, m.a), 1e-12f)};
}
V3 FromLocal(const M4& m, V3 l) { return m.r * l.x + m.u * l.y + m.a * l.z; }
// a matrix moved as a bone moved from o to n
M4 Carry(const M4& w, const M4& o, const M4& n) {
    return {FromLocal(n, ToLocal(o, w.r)), FromLocal(n, ToLocal(o, w.u)), FromLocal(n, ToLocal(o, w.a)),
            n.p + FromLocal(n, ToLocal(o, w.p - o.p))};
}
// a local matrix in a parent's space to the world, and back
M4 ToWorld(const M4& l, const M4& f) { return {FromLocal(f, l.r), FromLocal(f, l.u), FromLocal(f, l.a), f.p + FromLocal(f, l.p)}; }
M4 FromWorld(const M4& w, const M4& f) { return {ToLocal(f, w.r), ToLocal(f, w.u), ToLocal(f, w.a), ToLocal(f, w.p - f.p)}; }

struct Body {
    uint32_t hierarchy = 0;
    int index[kFlinchJoints] = {};   // the flinch joints' node indices
    std::vector<int> parent;         // each node's parent index (-1 the root)
    V3 angle, speed;                 // the flinch's spring: a world rotation (axis times angle) and its rate
    V3 twist_angle, twist_speed;     // and a turn about the spine
    float head_bias = 0;             // more of it in the neck and head (a hit there)
    uint64_t frame = ~0ull;          // the frame it was last applied
    std::vector<uint8_t> written;    // the matrices as we left them
    std::vector<uint8_t> original;   // and as the game had made them
    std::chrono::steady_clock::time_point last;
    // the bones as the game made them and as drawn, for what is attached to them (the head's own model)
    std::vector<M4> game, drawn;
    bool moved = false;
};

// what is drawn on a body (a head of its own skeleton, local to its atomic's frame): the body's bone it sits on
struct Attached {
    uint32_t body = 0;
    int node = -1;
    uint64_t frame = ~0ull;
    std::vector<uint8_t> written, original;
};
std::unordered_map<uint32_t, Attached> g_attached;  // by hierarchy

std::mutex g_lock;
std::unordered_map<uint32_t, Body> g_bodies;  // by hierarchy
std::atomic<uint64_t> g_frame{0};

bool Soldier(uint32_t h) { return h && int32_t(R32(h + 4)) == kSoldierNodes && !(R32(h) & 0x1000); }

int NodeIndex(uint32_t info, int id) {
    for (int i = 0; i < kSoldierNodes; i++)
        if (int32_t(R32(info + uint32_t(i) * 16)) == id) return i;
    return -1;
}

void TreeSurvey(uint32_t f, int depth) {
    if (!f || depth > 12) return;
    V3 p = ReadV3(f + 0x50 + 48);
    int objs = 0;
    for (uint32_t l = R32(f + 0x90); l && l != f + 0x90 && objs < 8; l = R32(l)) objs++;
    LOGR("tree: %*s%08X objs %d ltm %.0f %.0f %.0f", depth * 2, "", f, objs, p.x, p.y, p.z);
    for (uint32_t c = R32(f + 0x98); c; c = R32(c + 0x9C)) TreeSurvey(c, depth + 1);
}

Body* BodyOf(uint32_t h) {
    auto it = g_bodies.find(h);
    if (it != g_bodies.end()) return &it->second;
    if (!Soldier(h)) return nullptr;
    static int surveyed = 0;
    if (std::getenv("XBE_TREE_SURVEY") && surveyed++ < 1) {
        uint32_t pf = R32(h + 0x14);
        LOGR("tree: hierarchy %08X parent frame %08X its root %08X", h, pf, pf ? R32(pf + 0xA0) : 0);
        if (pf) TreeSurvey(R32(pf + 0xA0), 0);
    }
    Body b;
    b.hierarchy = h;
    uint32_t info = R32(h + 0x10);
    // parents from the push and pop flags, as RpHAnimHierarchyUpdateMatrices walks them
    b.parent.assign(kSoldierNodes, -1);
    std::vector<int> stack;
    int parent = -1;
    for (int i = 0; i < kSoldierNodes; i++) {
        uint32_t flags = R32(info + uint32_t(i) * 16 + 8);
        b.parent[size_t(i)] = parent;
        if (flags & 2) stack.push_back(parent);
        if (flags & 1) {
            parent = stack.empty() ? -1 : stack.back();
            if (!stack.empty()) stack.pop_back();
        } else {
            parent = i;
        }
    }
    for (int j = 0; j < kFlinchJoints; j++)
        if ((b.index[j] = NodeIndex(info, kFlinch[j].id)) < 0) return nullptr;
    b.last = std::chrono::steady_clock::now();
    return &g_bodies.emplace(h, std::move(b)).first->second;
}

// the soldier's body nearest a place (its root node, at the feet), within reach
Body* BodyNear(V3 p, float reach) {
    Body* best = nullptr;
    float best_d = reach;
    uint64_t frame = g_frame.load();
    for (auto& [h, b] : g_bodies) {
        // drawn lately, its hierarchy still a soldier's with its matrices
        if (frame - b.frame > 30 || !Soldier(h) || !R32(h + 8)) continue;
        V3 root = ReadV3(R32(h + 8) + 48);
        float d = Len(root - p);
        if (d < best_d) best_d = d, best = &b;
    }
    return best;
}

// --- turning bones ----------------------------------------------------------------------------------------------------

bool Below(const Body& b, int node, int top) {
    for (int q = node; q >= 0; q = b.parent[size_t(q)])
        if (q == top) return true;
    return false;
}

// a node and all below it turned by a rotation (axis times angle) about a point, then moved
void TurnSubtree(const Body& b, uint32_t mats, int top, V3 turn, V3 pivot, V3 move) {
    for (int i = 0; i < kSoldierNodes; i++) {
        if (!Below(b, i, top)) continue;
        uint32_t m = mats + uint32_t(i) * 64;
        WriteV3(m, Rotate(ReadV3(m), turn));
        WriteV3(m + 16, Rotate(ReadV3(m + 16), turn));
        WriteV3(m + 32, Rotate(ReadV3(m + 32), turn));
        WriteV3(m + 48, Rotate(ReadV3(m + 48) - pivot, turn) + pivot + move);
    }
}

// the turn taking direction a to direction b
V3 TurnBetween(V3 a, V3 b) {
    a = Norm(a), b = Norm(b);
    V3 axis = Cross(a, b);
    float sn = Len(axis), cs = Dot(a, b);
    if (sn < 1e-6f) return cs > 0 ? V3{} : Norm(Cross(a, std::fabs(a.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0})) * 3.14159265f;
    return axis * (std::atan2(sn, cs) / sn);
}

// a bone (node to its child point) turned, with all below it, to point at the child point's simulated place
void AimBone(const Body& b, uint32_t mats, int node, int child_node, V3 target, float weight) {
    V3 p = ReadV3(mats + uint32_t(node) * 64 + 48), c = ReadV3(mats + uint32_t(child_node) * 64 + 48);
    V3 turn = TurnBetween(c - p, target - p) * weight;
    if (Len(turn) > 1e-5f) TurnSubtree(b, mats, node, turn, p, V3{});
}

// --- the flinch ---------------------------------------------------------------------------------------------------------

// the flinch's turn applied to the hierarchy's world matrices: each joint turns about its own place (as the turns
// above it moved it), and all below it turn with it
void Apply(Body& b, float scale) {
    uint32_t mats = R32(b.hierarchy + 8);
    if (!mats) return;
    std::vector<std::vector<std::pair<V3, V3>>> chain(kSoldierNodes);  // per node: (axis angle, pivot), root first
    for (int i = 0; i < kSoldierNodes; i++) {
        int p = b.parent[size_t(i)];
        if (p >= 0) chain[size_t(i)] = chain[size_t(p)];
        for (int j = 0; j < kFlinchJoints; j++) {
            if (b.index[j] != i) continue;
            float share = kFlinch[j].share * (j >= 3 ? 1 + b.head_bias : 1);
            V3 turn = (b.angle + b.twist_angle) * (share * scale);
            V3 pivot = ReadV3(mats + uint32_t(i) * 64 + 48);
            for (auto& [r, c] : chain[size_t(i)]) pivot = Rotate(pivot - c, r) + c;
            chain[size_t(i)].push_back({turn, pivot});
        }
    }
    for (int i = 0; i < kSoldierNodes; i++) {
        const auto& ch = chain[size_t(i)];
        if (ch.empty()) continue;
        uint32_t m = mats + uint32_t(i) * 64;
        V3 right = ReadV3(m), up = ReadV3(m + 16), at = ReadV3(m + 32), p = ReadV3(m + 48);
        for (auto& [r, c] : ch) {
            right = Rotate(right, r), up = Rotate(up, r), at = Rotate(at, r);
            p = Rotate(p - c, r) + c;
        }
        WriteV3(m, right), WriteV3(m + 16, up), WriteV3(m + 32, at), WriteV3(m + 48, p);
    }
}

float Step(Body& b) {
    auto now = std::chrono::steady_clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - b.last).count(), 0.0f, 0.05f);
    b.last = now;
    // a spring of about 2.5 Hz, under-damped a little: the flinch, a small rebound, still
    const float k = 250.0f, c = 14.0f;
    for (int n = 0; n < 4; n++) {
        float h = dt / 4;
        b.speed = b.speed + (b.angle * -k + b.speed * -c) * h;
        b.angle = b.angle + b.speed * h;
        b.twist_speed = b.twist_speed + (b.twist_angle * -k + b.twist_speed * -c) * h;
        b.twist_angle = b.twist_angle + b.twist_speed * h;
    }
    b.head_bias *= std::exp(-dt * 3);
    return dt;
}

float Scale() {
    static const float s = [] {
        const char* e = std::getenv("XBE_HIT_SCALE");
        return e ? float(std::atof(e)) : 1.0f;
    }();
    return s;
}

}  // namespace

namespace {
// what a body holds or wears (gpu::g_carry): the nearest of a moved body's bones to its origin, as the game had it,
// carries it as it was moved
bool CarryWorld(float w[16]) {
    std::lock_guard l(g_lock);
    M4 m{{w[0], w[1], w[2]}, {w[4], w[5], w[6]}, {w[8], w[9], w[10]}, {w[12], w[13], w[14]}};
    uint64_t frame = g_frame.load();
    const Body* owner = nullptr;
    int node = -1;
    float best = 40;
    for (auto& [h, b] : g_bodies) {
        if (!b.moved || b.game.size() != size_t(kSoldierNodes) || b.drawn.size() != size_t(kSoldierNodes) || frame - b.frame > 2)
            continue;
        // within the body's reach at all
        if (Len(b.game[1].p - m.p) > 200) continue;
        for (int i = 0; i < kSoldierNodes; i++) {
            float d = Len(b.game[size_t(i)].p - m.p);
            if (d < best) best = d, owner = &b, node = i;
        }
    }
    if (!owner) return false;
    M4 n = Carry(m, owner->game[size_t(node)], owner->drawn[size_t(node)]);
    V3 rows[4] = {n.r, n.u, n.a, n.p};
    for (int r = 0; r < 4; r++) w[r * 4] = rows[r].x, w[r * 4 + 1] = rows[r].y, w[r * 4 + 2] = rows[r].z;
    return true;
}

const bool g_carry_registered = [] {
    gpu::g_carry = CarryWorld;
    return true;
}();
}  // namespace

namespace cod::ragdoll {
void NewFrame() { g_frame++; }
}  // namespace cod::ragdoll

namespace {
// Snow Tracks: a soldier's feet, once a frame. A foot that stops low (set down) a fifth of a metre or more from where
// it last stepped makes a step (gpu::g_steps): its sole under the ankle, pointing the way the hips face, turned to
// the way the body goes when it moves.
struct Feet {
    int foot[2] = {-1, -1}, hip[2] = {-1, -1}, pelvis = -1;
    V3 prev[2], stamp[2], prev_pelvis;
    bool stamped[2] = {}, seen = false;
    float sign = 1;
    uint64_t frame = ~0ull;
    std::chrono::steady_clock::time_point at;
};
std::unordered_map<uint32_t, Feet> g_feet;  // by hierarchy

void WatchFeet(uint32_t h) {
    uint64_t frame = g_frame.load();
    uint32_t mats = R32(h + 8), info = R32(h + 0x10);
    if (!mats || !info) return;
    Feet& f = g_feet[h];
    if (f.frame == frame) return;
    f.frame = frame;
    if (f.pelvis < 0) {
        f.foot[0] = NodeIndex(info, kPointNode[kLFoot]), f.foot[1] = NodeIndex(info, kPointNode[kRFoot]);
        f.hip[0] = NodeIndex(info, kPointNode[kLHip]), f.hip[1] = NodeIndex(info, kPointNode[kRHip]);
        f.pelvis = NodeIndex(info, kPointNode[kPelvis]);
        if (f.foot[0] < 0 || f.foot[1] < 0 || f.hip[0] < 0 || f.hip[1] < 0 || f.pelvis < 0) {
            f.pelvis = -2;
            return;
        }
    }
    if (f.pelvis < 0) return;
    auto at = [&](int node) { return ReadV3(mats + uint32_t(node) * 64 + 48); };
    auto now = std::chrono::steady_clock::now();
    float dt = f.seen ? std::chrono::duration<float>(now - f.at).count() : 0;
    f.at = now;
    V3 foot[2] = {at(f.foot[0]), at(f.foot[1])}, pelvis = at(f.pelvis);
    V3 hips = at(f.hip[1]) - at(f.hip[0]);
    V3 fwd{-hips.z, 0, hips.x};
    float fl = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
    if (fl < 1e-3f) return;
    fwd = fwd * (1 / fl);
    if (f.seen && dt > 1e-4f && dt < 0.25f) {
        V3 v = (pelvis - f.prev_pelvis) * (1 / dt);
        float sp = std::sqrt(v.x * v.x + v.z * v.z);
        if (sp > 60) f.sign = v.x * fwd.x + v.z * fwd.z >= 0 ? 1.0f : -1.0f;
        for (int i = 0; i < 2; i++) {
            V3 d = foot[i] - f.prev[i];
            float fs = std::sqrt(d.x * d.x + d.z * d.z) / dt;
            bool low = foot[i].y <= foot[1 - i].y + 4 && foot[i].y < pelvis.y - 60;
            V3 m = foot[i] - f.stamp[i];
            if (fs < 45 && low && (!f.stamped[i] || m.x * m.x + m.z * m.z > 20 * 20)) {
                f.stamp[i] = foot[i], f.stamped[i] = true;
                std::lock_guard l(gpu::g_steps.m);
                if (gpu::g_steps.list.size() < 256)
                    gpu::g_steps.list.push_back({foot[i].x, foot[i].y - 9, foot[i].z, fwd.x * f.sign, fwd.z * f.sign, i});
            }
        }
    }
    f.prev[0] = foot[0], f.prev[1] = foot[1], f.prev_pelvis = pelvis, f.seen = true;
    static uint64_t swept = 0;
    if (frame - swept > 600) {
        swept = frame;
        for (auto it = g_feet.begin(); it != g_feet.end();) it = frame - it->second.frame > 600 ? g_feet.erase(it) : std::next(it);
    }
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_001ECCB0);
X86_FUNC(sub_001ECCB0) {  // the skin's bone matrices for an atomic (dest, atomic, skin)
    uint32_t atomic = R32(ctx.esp + 8);
    uint32_t h = atomic ? R32(R32(kSkinAtomicOffset) + atomic) : 0;
    if (h && app::Get().snow_tracks && Soldier(h)) {
        std::lock_guard l(g_lock);
        WatchFeet(h);
    }
    if (h && app::Get().hit_reactions && Soldier(h)) {
        std::lock_guard l(g_lock);
        uint64_t frame = g_frame.load();
        static uint64_t fetched = ~0ull;
        if (fetched != frame) {
            fetched = frame;
            // bodies not drawn for a while go (their soldier may be gone, its memory another's)
            static uint64_t swept = 0;
            if (frame - swept > 120) {
                swept = frame;
                for (auto it = g_bodies.begin(); it != g_bodies.end();)
                    it = frame - it->second.frame > 600 && it->second.frame != ~0ull ? g_bodies.erase(it) : std::next(it);
            }
        }
        Body* b = BodyOf(h);
        uint32_t mats = R32(h + 8);
        if (b && b->frame != frame && mats) {
            size_t bytes = size_t(kSoldierNodes) * 64;
            // the game made no new pose since (an idle or hidden soldier): its own back first, not ours twice
            if (b->written.size() == bytes && !std::memcmp(HostPtr(mats), b->written.data(), bytes))
                std::memcpy(HostPtr(mats), b->original.data(), bytes);
            b->original.assign(static_cast<uint8_t*>(HostPtr(mats)), static_cast<uint8_t*>(HostPtr(mats)) + bytes);
            b->game.resize(kSoldierNodes);
            for (int i = 0; i < kSoldierNodes; i++) b->game[size_t(i)] = ReadM(mats + uint32_t(i) * 64);
            float dt = Step(*b);
            if (app::Get().hit_reactions && Len(b->angle) + Len(b->twist_angle) > 1e-4f) Apply(*b, Scale());
            b->written.assign(static_cast<uint8_t*>(HostPtr(mats)), static_cast<uint8_t*>(HostPtr(mats)) + bytes);
            b->frame = frame;
            b->drawn.resize(kSoldierNodes);
            b->moved = false;
            for (int i = 0; i < kSoldierNodes; i++) {
                b->drawn[size_t(i)] = ReadM(mats + uint32_t(i) * 64);
                b->moved |= Len(b->drawn[size_t(i)].p - b->game[size_t(i)].p) > 0.05f;
            }
        }
    } else if (h && (R32(h) & 0x1000) && app::Get().hit_reactions) {
        // a model of its own skeleton (a head; its matrices in the world, as the body's): moved as the body's bone it sits on
        std::lock_guard l(g_lock);
        uint32_t frame_obj = R32(atomic + 4);
        uint32_t mats = R32(h + 8);
        int nodes = int32_t(R32(h + 4));
        uint64_t frame = g_frame.load();
        if (frame_obj && mats && nodes > 0 && nodes <= 64) {
            Attached& at = g_attached[h];
            size_t bytes = size_t(nodes) * 64;
            if (at.frame != frame) {
                if (at.written.size() == bytes && !std::memcmp(HostPtr(mats), at.written.data(), bytes))
                    std::memcpy(HostPtr(mats), at.original.data(), bytes);
                at.original.assign(static_cast<uint8_t*>(HostPtr(mats)), static_cast<uint8_t*>(HostPtr(mats)) + bytes);
                M4 f = ReadM(mats);  // its first bone, where it is
                // the bone it sits on: once, the nearest of any body's bones as the game has them
                Body* owner = nullptr;
                if (at.body) {
                    auto it = g_bodies.find(at.body);
                    if (it != g_bodies.end() && it->second.game.size() == size_t(kSoldierNodes)) owner = &it->second;
                }
                if (!owner) {
                    float best = 35;
                    for (auto& [bh, bb] : g_bodies) {
                        if (bb.game.size() != size_t(kSoldierNodes) || frame - bb.frame > 2) continue;
                        for (int i = 0; i < kSoldierNodes; i++) {
                            float d = Len(bb.game[size_t(i)].p - f.p);
                            if (d < best) best = d, owner = &bb, at.node = i;
                        }
                    }
                    if (owner) at.body = owner->hierarchy;
                }
                if (owner && owner->moved && at.node >= 0) {
                    const M4& o = owner->game[size_t(at.node)];
                    const M4& n = owner->drawn[size_t(at.node)];
                    for (int i = 0; i < nodes; i++) {
                        uint32_t m = mats + uint32_t(i) * 64;
                        WriteM(m, Carry(ReadM(m), o, n));
                    }
                }
                at.written.assign(static_cast<uint8_t*>(HostPtr(mats)), static_cast<uint8_t*>(HostPtr(mats)) + bytes);
                at.frame = frame;
            }
        }
    }
    __imp__sub_001ECCB0(ctx, base);
}

// A soldier hit by a bullet (thiscall: the soldier, the hit's record: +0x0C the attacker, +0x24 the part hit, -1 none,
// +0x28 the damage). Positions from the entities' frames (+0x340, their world matrix at 0x50).
X86_EXTERN_FUNC(__imp__sub_000E78C0);
X86_FUNC(sub_000E78C0) {
    uint32_t victim = ctx.ecx, hit = R32(ctx.esp + 4);
    if (app::Get().hit_reactions && victim && hit) {
        uint32_t attacker = R32(hit + 0x0C);
        uint32_t vf = R32(victim + 0x340), af = attacker ? R32(attacker + 0x340) : 0;
        int32_t part = int32_t(R32(hit + 0x24));
        float damage;
        std::memcpy(&damage, HostPtr(hit + 0x28), 4);
        if (vf && part != -1) {
            V3 vp = ReadV3(vf + 0x50 + 48);
            V3 from = af ? ReadV3(af + 0x50 + 48) : vp - V3{0, 0, 100};
            V3 dir = vp - from;
            dir.y = 0;
            float dl = Len(dir);
            std::lock_guard l(g_lock);
            Body* b = BodyNear(vp, 150);
            if (b && dl > 1) {
                dir = dir * (1 / dl);
                // away from the shooter: a turn about the horizontal axis across the shot, the chest leaning back;
                // a little twist about the spine either way
                static std::mt19937 rng{1234};
                std::uniform_real_distribution<float> u(-1, 1);
                float strength = std::clamp(0.6f + damage / 60.0f, 0.6f, 1.6f);
                V3 axis = Cross(V3{0, 1, 0}, dir);
                b->speed = b->speed + axis * (4.0f * strength);
                b->twist_speed = b->twist_speed + V3{0, 1, 0} * (u(rng) * 1.2f * strength);
                b->head_bias = part == 0 ? 1.0f : 0.0f;
                static const bool log = std::getenv("XBE_RAGDOLL_SURVEY") != nullptr;
                if (log) LOGR("ragdoll: hit soldier %08X part %d damage %.0f from %.2f %.2f, hit points %d", victim, part, damage, dir.x,
                              dir.z, int32_t(R32(victim + 0x410)));
            }
        }
    }
    __imp__sub_000E78C0(ctx, base);
    // tests (XBE_KILL_TEST): a soldier hit dies of it
    static const bool kill = std::getenv("XBE_KILL_TEST") != nullptr;
    if (kill && victim) {
        float zero = 0;
        std::memcpy(HostPtr(victim + 0x410), &zero, 4);
    }
}
