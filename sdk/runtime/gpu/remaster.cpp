// See remaster.h.
#include "gpu/remaster.h"
#include "gpu/keymap.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_set>

#include "app/app.h"
#include "core/log.h"
#include "gpu/shaders.h"

namespace gpu::remaster {

namespace {

constexpr uint32_t kMinSize = 4;  // smaller textures (fonts' cells, ramps) are not worth materials

const std::string& DumpFolder() {
    static const std::string dir = [] {
        const char* e = std::getenv("XBE_DUMP_TEXTURES");
        std::string d = e && *e ? e : "";
        if (!d.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(d, ec);
            LOGR("remaster: dumping textures to %s", d.c_str());
        }
        return d;
    }();
    return dir;
}

void Put32(std::vector<uint8_t>& v, size_t at, uint32_t x) { std::memcpy(v.data() + at, &x, 4); }

}  // namespace

std::string IdName(uint64_t id) {
    char s[17];
    std::snprintf(s, sizeof(s), "%016llx", (unsigned long long)id);
    return s;
}

uint64_t Identify(const TextureDesc& d, const uint8_t* guest, const uint32_t* palette) {
    if (d.cube || d.depth > 1 || d.width < kMinSize || d.height < kMinSize || !guest) return 0;
    // the size and format go in with the bytes: the same bytes at two sizes are two textures
    uint64_t seed = uint64_t(d.width) << 40 ^ uint64_t(d.height) << 16 ^ uint64_t(d.format) ^ 0x5EED5EEDull;
    uint64_t id = Hash(guest, LevelSize(d, 0), seed);
    if (palette) {
        uint32_t sig[16];
        for (int i = 0; i < 16; i++) {
            uint32_t c = palette[i], b[4] = {c & 0xFF, c >> 8 & 0xFF, c >> 16 & 0xFF, c >> 24};
            sig[i] = (b[0] + b[1] + b[2] + b[3]) | (b[0] ^ b[1] ^ b[2] ^ b[3]) << 10 |
                     std::max(std::max(b[0], b[1]), std::max(b[2], b[3])) << 18;
        }
        id = Hash(sig, sizeof(sig), id);
    }
    return id ? id : 1;
}

void Dump(uint64_t id, const TextureDesc& d, Host host, const std::vector<uint8_t>& top, uint32_t pitch,
          const uint8_t* guest, const uint32_t* palette) {
    const std::string& dir = DumpFolder();
    if (dir.empty() || !id) return;
    static std::mutex lock;
    static std::unordered_set<uint64_t> done;
    {
        std::lock_guard l(lock);
        if (!done.insert(id).second) return;
    }
    std::filesystem::path path = std::filesystem::path(dir) / (IdName(id) + ".dds");
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return;
    // DDS: the legacy header, DXT1/3/5 by four cc or 32 bit BGRA by masks, readable by every tool
    std::vector<uint8_t> h(128, 0);
    std::memcpy(h.data(), "DDS ", 4);
    bool bc = host != Host::BGRA8;
    uint32_t block = host == Host::BC1 ? 8 : 16;
    uint32_t bw = (d.width + 3) / 4, bh = (d.height + 3) / 4;
    uint32_t linear = bc ? bw * bh * block : d.width * d.height * 4;
    Put32(h, 4, 124);
    Put32(h, 8, 0x1 | 0x2 | 0x4 | 0x1000 | (bc ? 0x80000 : 0x8));  // caps, height, width, pixel format, size or pitch
    Put32(h, 12, d.height);
    Put32(h, 16, d.width);
    Put32(h, 20, bc ? linear : d.width * 4);
    Put32(h, 76, 32);  // pixel format size
    if (bc) {
        Put32(h, 80, 0x4);  // four cc
        std::memcpy(h.data() + 84, host == Host::BC1 ? "DXT1" : host == Host::BC2 ? "DXT3" : "DXT5", 4);
    } else {
        Put32(h, 80, 0x41);  // rgb, alpha
        Put32(h, 88, 32);
        Put32(h, 92, 0x00FF0000);
        Put32(h, 96, 0x0000FF00);
        Put32(h, 100, 0x000000FF);
        Put32(h, 104, 0xFF000000);
    }
    Put32(h, 108, 0x1000);  // texture
    FILE* f = _wfopen(path.wstring().c_str(), L"wb");
    if (!f) return;
    std::fwrite(h.data(), 1, h.size(), f);
    // rows as the decoder packed them
    uint32_t rows = bc ? bh : d.height, row = bc ? bw * block : d.width * 4;
    for (uint32_t y = 0; y < rows; y++) std::fwrite(top.data() + size_t(y) * pitch, 1, row, f);
    std::fclose(f);
    if (guest) {
        path.replace_extension(".raw");
        if (FILE* r = _wfopen(path.wstring().c_str(), L"wb")) {
            std::fwrite(guest, 1, LevelSize(d, 0), r);
            uint32_t none[256] = {};
            std::fwrite(palette ? palette : none, 4, 256, r);
            std::fclose(r);
        }
    }
}

}  // namespace gpu::remaster

// ---------------------------------------------------------------------------------------------------------------------
// the pack

namespace gpu::remaster {

namespace {

struct Store {
    std::filesystem::path dir;
    KeyMap<uint64_t, Material> materials;
    std::mutex lock;
    std::condition_variable cv;
    std::deque<uint64_t> todo;
    std::deque<Loaded> done;
    std::unordered_set<uint64_t> asked;
    bool started = false;

    Store() {
        const char* e = std::getenv("XBE_REMASTER_DIR");
        dir = e && *e ? std::filesystem::path(e) : app::ExeDir() / "remaster";
        std::ifstream f(dir / "materials.txt");
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream in(line);
            std::string id, cls;
            int pbr = 0, tiles = 0;
            Material m;
            if (!(in >> id >> cls >> pbr >> m.normal >> m.pom >> m.spec >> m.detail >> tiles)) continue;
            m.pbr = pbr != 0;
            m.tiles = tiles != 0;
            // the classes' reflectivity: metal and glass most, varnished wood, worn stone and tiles a little, cloth,
            // skin, paper and the ground hardly
            static const std::pair<const char*, float> kRefl[] = {
                {"metal", 0.9f}, {"glass", 0.9f}, {"wood", 0.2f}, {"stone", 0.25f}, {"concrete", 0.2f},
                {"roof", 0.25f}, {"brick", 0.15f}, {"plaster", 0.12f}, {"atlas", 0.12f}};
            for (const auto& [name, r] : kRefl)
                if (cls == name) m.refl = r;
            m.metal = cls == "metal" ? 1.0f : 0.0f;
            m.sway = cls == "foliage" ? 1.0f : cls == "fabric" ? 0.5f : 0.0f;
            m.water = cls == "water";
            m.skin = cls == "skin";
            m.cloth = cls == "fabric";
            // the detail colour grain of its kind (tools/remaster/details.py ALBEDO, after its 7 normal maps); none for
            // what is not a surface (the HUD, effects, the sky, water, glass)
            static const char* const kGrains[] = {"stone", "brick", "concrete", "wood", "metal", "fabric", "ground",
                                                  "roof", "foliage", "paper", "plaster", "skin", "snow", "grain"};
            for (int g = 0; g < 14; g++)
                if (cls == kGrains[g]) m.grain = 7 + g;
            if (cls == "atlas") m.grain = 7 + 13;
            materials[std::strtoull(id.c_str(), nullptr, 16)] = m;
        }
        // the snowy ground (tools/remaster/snow.py)
        std::ifstream sf(dir / "snow.txt");
        size_t snowy = 0;
        while (std::getline(sf, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto it = materials.find(std::strtoull(line.c_str(), nullptr, 16));
            if (it != materials.end()) it->second.snow = true, it->second.grain = 7 + 12, snowy++;
        }
        // the characters' textures' detail grain by their names (tools/remaster/grains.py: their classes are often
        // wrong for uniforms), over the class's
        std::ifstream gf(dir / "grains.txt");
        size_t grained = 0;
        while (std::getline(gf, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream in(line);
            std::string id, kind;
            if (!(in >> id >> kind)) continue;
            static const char* const kKinds[] = {"stone", "brick", "concrete", "wood", "metal", "fabric", "ground",
                                                 "roof", "foliage", "paper", "plaster", "skin", "snow", "grain"};
            auto it = materials.find(std::strtoull(id.c_str(), nullptr, 16));
            if (it == materials.end()) continue;
            it->second.grain = -1;
            for (int g = 0; g < 14; g++)
                if (kind == kKinds[g]) it->second.grain = 7 + g;
            grained++;
        }
        if (!materials.empty())
            LOGR("remaster: %zu textures in %s (%zu snowy, %zu grains by name)", materials.size(), dir.string().c_str(), snowy, grained);
    }

    void Worker() {
        for (;;) {
            uint64_t id;
            {
                std::unique_lock l(lock);
                cv.wait(l, [&] { return !todo.empty(); });
                id = todo.front();
                todo.pop_front();
            }
            Loaded ld;
            ld.id = id;
            std::wstring base = (dir / "textures" / IdName(id)).wstring();
            bool ok = ReadDds(base + L".c.dds", ld.color);
            auto it = materials.find(id);
            if (ok && it != materials.end() && it->second.pbr) {
                if (!ReadDds(base + L".n.dds", ld.normal) || !ReadDds(base + L".m.dds", ld.mat)) ld.normal = ld.mat = Image{};
            }
            if (!ok) LOG_FIRST(16, "remaster: no colour map for %s", IdName(id).c_str());
            std::lock_guard l(lock);
            if (ok) done.push_back(std::move(ld));
        }
    }
};

Store& TheStore() {
    static Store s;
    return s;
}

uint32_t BlockBytes(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC4_UNORM:
            return 8;
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC7_UNORM:
            return 16;
        default:
            return 0;
    }
}

}  // namespace

uint32_t Image::RowBytes(uint32_t mip) const {
    uint32_t mw = std::max(1u, w >> mip);
    if (uint32_t b = BlockBytes(fmt)) return (mw + 3) / 4 * b;
    return mw * 4;
}
uint32_t Image::Rows(uint32_t mip) const {
    uint32_t mh = std::max(1u, h >> mip);
    return BlockBytes(fmt) ? (mh + 3) / 4 : mh;
}

bool ReadDds(const std::wstring& path, Image& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b(size_t(std::max(size, 0L)));
    size_t got = std::fread(b.data(), 1, b.size(), f);
    std::fclose(f);
    if (got < 128 || std::memcmp(b.data(), "DDS ", 4)) return false;
    auto u32 = [&](size_t at) {
        uint32_t v;
        std::memcpy(&v, b.data() + at, 4);
        return v;
    };
    out.h = u32(12);
    out.w = u32(16);
    out.mips = std::max(1u, u32(28));
    size_t at = 128;
    if (!std::memcmp(b.data() + 84, "DX10", 4)) {
        out.fmt = DXGI_FORMAT(u32(128));
        at += 20;
    } else if (!std::memcmp(b.data() + 84, "DXT1", 4)) {
        out.fmt = DXGI_FORMAT_BC1_UNORM;
    } else if (!std::memcmp(b.data() + 84, "DXT5", 4)) {
        out.fmt = DXGI_FORMAT_BC3_UNORM;
    } else if (!std::memcmp(b.data() + 84, "ATI2", 4) || !std::memcmp(b.data() + 84, "BC5U", 4)) {
        out.fmt = DXGI_FORMAT_BC5_UNORM;
    } else {
        return false;
    }
    if (!BlockBytes(out.fmt) && out.fmt != DXGI_FORMAT_R8G8B8A8_UNORM) return false;  // compressed, or plain RGBA
    out.offsets.clear();
    size_t total = 0;
    for (uint32_t m = 0; m < out.mips; m++) {
        out.offsets.push_back(total);
        total += size_t(out.RowBytes(m)) * out.Rows(m);
    }
    if (at + total > b.size()) return false;
    out.data.assign(b.begin() + std::ptrdiff_t(at), b.begin() + std::ptrdiff_t(at + total));
    return true;
}

bool Active() {
    static const bool has = !TheStore().materials.empty();
    return has && app::Get().remaster;
}

const Material* Find(uint64_t id) {
    if (!id) return nullptr;
    Store& s = TheStore();
    auto it = s.materials.find(id);
    return it == s.materials.end() ? nullptr : &it->second;
}

std::wstring Dir() { return TheStore().dir.wstring(); }

bool ReadPrompt(const std::string& name, Image& out) {
    return ReadDds((TheStore().dir / "prompts" / (name + ".dds")).wstring(), out) && out.fmt == DXGI_FORMAT_R8G8B8A8_UNORM;
}

std::vector<Image> ReadDetails() {
    std::vector<Image> out;
    const std::filesystem::path dir = TheStore().dir / "details";
    for (int i = 0;; i++) {
        Image im;
        if (!ReadDds((dir / (std::to_string(i) + ".dds")).wstring(), im)) break;
        if (!out.empty() && (im.fmt != out[0].fmt || im.w != out[0].w || im.h != out[0].h || im.mips != out[0].mips)) break;
        out.push_back(std::move(im));
    }
    return out;
}

void Request(uint64_t id) {
    Store& s = TheStore();
    std::lock_guard l(s.lock);
    if (!s.asked.insert(id).second) return;
    if (!s.started) {
        s.started = true;
        for (int i = 0; i < 4; i++) std::thread([&s] { s.Worker(); }).detach();
    }
    s.todo.push_back(id);
    s.cv.notify_one();
}

bool TakeLoaded(Loaded& out) {
    Store& s = TheStore();
    std::lock_guard l(s.lock);
    if (s.done.empty()) return false;
    out = std::move(s.done.front());
    s.done.pop_front();
    return true;
}

}  // namespace gpu::remaster
