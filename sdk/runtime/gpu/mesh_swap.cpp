#include "gpu/mesh_swap.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dxgiformat.h>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu::meshes {
namespace {

constexpr char kMagic[4] = {'F', 'H', 'M', '1'};
constexpr uint32_t kCullCw = 0x900;  // D3DCULL_CW

#pragma pack(push, 1)
struct Header {
    char magic[4];
    uint64_t key, tex;
    uint32_t prim, count, flags;
    uint16_t regs, nregs;
    uint32_t vertex_count, index_count;
};
#pragma pack(pop)

std::mutex g_meshes_lock;
std::unordered_map<uint64_t, Mesh> g_meshes;  // as the loader reads them (a node's address stays: Mesh* kept)
std::unordered_set<uint64_t> g_draws;  // prim << 32 | count of the originals (from the headers, before the loader)

int RegCount(uint16_t regs) {
    int n = 0;
    for (; regs; regs &= uint16_t(regs - 1)) n++;
    return n;
}

bool ReadMesh(const std::filesystem::path& p, uint64_t& key, uint64_t& draw, Mesh& m) {
    std::ifstream f(p, std::ios::binary);
    Header h{};
    if (!f.read(reinterpret_cast<char*>(&h), sizeof(h)) || std::memcmp(h.magic, kMagic, 4) != 0) return false;
    if (h.nregs != RegCount(h.regs) || !h.vertex_count || !h.index_count || h.index_count % 3) return false;
    m.regs = h.regs;
    m.vertex_count = h.vertex_count;
    m.vertices.resize(size_t(h.vertex_count) * h.nregs * 4);
    m.indices.resize(h.index_count);
    if (!f.read(reinterpret_cast<char*>(m.vertices.data()), std::streamsize(m.vertices.size() * 4))) return false;
    if (!f.read(reinterpret_cast<char*>(m.indices.data()), std::streamsize(m.indices.size() * 4))) return false;
    for (uint32_t i : m.indices)
        if (i >= h.vertex_count) return false;
    if (h.flags & kMeshSource) {
        m.source.resize(h.vertex_count);
        if (!f.read(reinterpret_cast<char*>(m.source.data()), std::streamsize(m.source.size() * 4))) return false;
    }
    if (h.flags & kMeshTriSource) {
        m.tri_source.resize(h.index_count / 3);
        if (!f.read(reinterpret_cast<char*>(m.tri_source.data()), std::streamsize(m.tri_source.size() * 4))) return false;
    }
    key = h.key;
    draw = uint64_t(h.prim) << 32 | h.count;
    return true;
}

}  // namespace

// the headers read now (which draws may be replaced), the meshes on a thread of their own: the game starts as soon,
// a draw keeps its own mesh until its remastered one is read
void Load(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".fhm") continue;
        std::ifstream f(e.path(), std::ios::binary);
        Header h{};
        if (!f.read(reinterpret_cast<char*>(&h), sizeof(h)) || std::memcmp(h.magic, kMagic, 4) != 0) continue;
        g_draws.insert(uint64_t(h.prim) << 32 | h.count);
        files.push_back(e.path());
    }
    if (files.empty()) return;
    LOGR("meshes: %zu remastered meshes in %s, read on a thread", files.size(), dir.string().c_str());
    std::thread([files = std::move(files)] {
        size_t tris = 0, n = 0;
        for (const auto& path : files) {
            uint64_t key = 0, draw = 0;
            Mesh m;
            if (!ReadMesh(path, key, draw, m)) {
                LOGR("meshes: %s is not a mesh", path.filename().string().c_str());
                continue;
            }
            tris += m.indices.size() / 3;
            n++;
            std::lock_guard<std::mutex> guard(g_meshes_lock);
            g_meshes[key] = std::move(m);
        }
        LOGR("meshes: %zu remastered meshes read (%zu triangles)", n, tris);
    }).detach();
}

bool Candidate(uint32_t prim, uint32_t count) {
    return !g_draws.empty() && g_draws.count(uint64_t(prim) << 32 | count);
}

uint64_t Key(const Assembled& a) {
    // FNV-1a 64 over the floats' bytes, then the registers and the vertex count
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    };
    mix(a.vertices.data(), a.vertices.size() * 4);
    mix(&a.regs, sizeof(a.regs));
    mix(&a.vertex_count, sizeof(a.vertex_count));
    return h;
}

namespace {
uint64_t Fnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

std::mutex g_keys_lock;
std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> g_keys;  // where -> (check, key)
}  // namespace

DrawId IdOf(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    using namespace kernel;
    DrawId id;
    uint64_t h = 0xcbf29ce484222325ull;
    const uint32_t vb = s.streams[0].vb;
    const uint32_t data = vb ? R32(vb + 4) : 0;  // the buffer's data (physical)
    const uint32_t fields[] = {data, s.streams[0].stride, prim, start, count, s.base_vertex, s.vs_handle};
    h = Fnv(h, fields, sizeof(fields));
    const uintptr_t ip = reinterpret_cast<uintptr_t>(indices);
    id.where = Fnv(h, &ip, sizeof(ip));
    // the first vertices' bytes and the first indices'
    uint64_t c = 0xcbf29ce484222325ull;
    if (data) {
        // (where the draw's own vertices start: a buffer may hold many models)
        uint32_t first = indices ? uint32_t(*reinterpret_cast<const uint16_t*>(indices)) + s.base_vertex : start;
        const uint8_t* v = static_cast<const uint8_t*>(HostPtr(mem::VirtualFromPhysical(data)));
        c = Fnv(c, v + size_t(first) * s.streams[0].stride, 64);
    }
    if (indices) c = Fnv(c, indices, 32);
    id.check = c;
    return id;
}

bool CachedKey(const DrawId& id, uint64_t& key) {
    std::lock_guard<std::mutex> guard(g_keys_lock);
    auto it = g_keys.find(id.where);
    if (it == g_keys.end() || it->second.first != id.check) return false;
    key = it->second.second;
    return true;
}

void StoreKey(const DrawId& id, uint64_t key) {
    std::lock_guard<std::mutex> guard(g_keys_lock);
    if (g_keys.size() > 100000) g_keys.clear();
    g_keys[id.where] = {id.check, key};
}

const Mesh* Find(uint64_t key, uint16_t regs) {
    std::lock_guard<std::mutex> guard(g_meshes_lock);
    auto it = g_meshes.find(key);
    if (it == g_meshes.end() || it->second.regs != regs) return nullptr;
    static std::unordered_set<uint64_t> told;
    if (told.size() < 64 && told.insert(key).second)
        LOGR("meshes: %016llx drawn remastered (%zu triangles)", (unsigned long long)key, it->second.indices.size() / 3);
    return &it->second;
}

void Fill(const Mesh& m, Assembled& a) {
    int nregs = RegCount(m.regs);
    a.vertices.clear();
    a.vertex_count = m.vertex_count;
    a.topology = Topology::Triangles;
    a.strip = false;
    a.decode = 0;
    for (int r = 0, k = 0; r < 16; r++)
        if (m.regs >> r & 1) a.elements[size_t(r)] = {DXGI_FORMAT_R32G32B32A32_FLOAT, 0, uint16_t(k++ * 16)};
    a.buffer_count = 1;
    a.buffers[0] = {reinterpret_cast<const uint8_t*>(m.vertices.data()), uint32_t(m.vertices.size() * 4), uint32_t(nregs * 16)};
    a.index_src = m.indices.data();
    a.index16 = false;
    a.index_count = uint32_t(m.indices.size());
    a.draw_count = 0;
    a.base_vertex = 0;
    a.resident = &m;
}

const std::filesystem::path& DumpDir() {
    static const std::filesystem::path dir = [] {
        const char* e = std::getenv("XBE_MESH_DUMP");
        return e && *e ? std::filesystem::path(e) : std::filesystem::path();
    }();
    return dir;
}

void Dump(const State& s, uint32_t prim, uint32_t count, uint64_t key, uint64_t tex, const Assembled& a, uint64_t frame) {
    static std::mutex lock;
    static std::unordered_set<uint64_t> done;
    struct Seen {
        uint64_t frame = ~0ull;
        int frames = 0;
    };
    static std::unordered_map<uint64_t, Seen> seen;
    std::lock_guard<std::mutex> guard(lock);
    if (DumpDir().empty() || done.count(key) || done.size() > 50000) return;
    if (seen.size() > 200000) seen.clear();  // (keys of vertices made anew each frame, never seen again)
    Seen& sn = seen[key];
    if (sn.frame != frame) sn.frame = frame, sn.frames++;
    if (sn.frames < 3) return;
    seen.erase(key);
    done.insert(key);
    std::error_code ec;
    std::filesystem::create_directories(DumpDir(), ec);
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx.fhm", (unsigned long long)key);
    std::ofstream f(DumpDir() / name, std::ios::binary);
    uint32_t cull = s.rs[kRsCullMode];
    Header h{};
    std::memcpy(h.magic, kMagic, 4);
    h.key = key;
    h.tex = tex;
    h.prim = prim;
    h.count = count;
    h.flags = (cull ? kMeshCull : 0) | (cull == kCullCw ? kMeshCcw : 0) | kMeshConstants;
    h.regs = a.regs;
    h.nregs = uint16_t(RegCount(a.regs));
    h.vertex_count = a.vertex_count;
    h.index_count = uint32_t(a.indices.size());
    f.write(reinterpret_cast<const char*>(&h), sizeof(h));
    f.write(reinterpret_cast<const char*>(a.vertices.data()), std::streamsize(a.vertices.size() * 4));
    f.write(reinterpret_cast<const char*>(a.indices.data()), std::streamsize(a.indices.size() * 4));
    f.write(reinterpret_cast<const char*>(s.vsc.data()), std::streamsize(sizeof(s.vsc)));
    if (done.size() <= 300 || done.size() % 100 == 0) LOGR("meshes: dumped %s (tex %016llx, %u vertices, %zu indices, prim %u count %u)", name, (unsigned long long)tex,
         a.vertex_count, a.indices.size(), prim, count);
}

}  // namespace gpu::meshes
