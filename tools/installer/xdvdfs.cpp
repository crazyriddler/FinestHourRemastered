#include "xdvdfs.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace xdvdfs {

namespace {

constexpr uint64_t kSector = 2048;
// where the game partition starts: an XISO, a redump image (XGD1), and the later discs' layouts
constexpr uint64_t kPartitions[] = {0, 0x18300000, 0xFD90000, 0x2080000};
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

}  // namespace

Image::~Image() {
    if (f_) std::fclose(f_);
}

bool Image::ReadAt(uint64_t at, void* dst, size_t n) {
    if (_fseeki64(f_, int64_t(base_ + at), SEEK_SET) != 0) return false;
    return std::fread(dst, 1, n, f_) == n;
}

bool Image::Open(const std::wstring& path, std::string& error) {
    f_ = _wfopen(path.c_str(), L"rb");
    if (!f_) {
        error = "The image cannot be opened.";
        return false;
    }
    for (uint64_t p : kPartitions) {
        base_ = p;
        char magic[20];
        if (!ReadAt(32 * kSector, magic, sizeof(magic)) || std::memcmp(magic, kMagic, 20) != 0) continue;
        uint32_t root[2];
        if (!ReadAt(32 * kSector + 20, root, sizeof(root))) continue;
        files_.clear();
        if (Walk(root[0], root[1], "", 0) && !files_.empty()) return true;
    }
    error = "This is not an Xbox disc image (no XDVDFS file system found).";
    return false;
}

// a directory is a binary tree of entries: each with its left and right subtrees' offsets (in 4 byte steps)
bool Image::Walk(uint64_t dir_sector, uint32_t dir_size, const std::string& prefix, int depth) {
    if (depth > 32 || dir_size == 0) return true;
    std::vector<uint8_t> table(dir_size);
    if (!ReadAt(dir_sector * kSector, table.data(), dir_size)) return false;
    std::vector<uint32_t> todo{0};
    std::vector<bool> seen(dir_size / 4 + 1, false);
    while (!todo.empty()) {
        uint32_t at = todo.back();
        todo.pop_back();
        if (at + 14 > dir_size || seen[at / 4]) continue;
        seen[at / 4] = true;
        const uint8_t* e = table.data() + at;
        uint16_t left, right;
        uint32_t sector, size;
        std::memcpy(&left, e, 2), std::memcpy(&right, e + 2, 2), std::memcpy(&sector, e + 4, 4), std::memcpy(&size, e + 8, 4);
        if (left == 0xFFFF && right == 0xFFFF) continue;  // padding
        uint8_t attr = e[12], len = e[13];
        if (at + 14 + len > dir_size) continue;
        std::string name(reinterpret_cast<const char*>(e + 14), len);
        if (left) todo.push_back(uint32_t(left) * 4);
        if (right) todo.push_back(uint32_t(right) * 4);
        std::string path = prefix.empty() ? name : prefix + "/" + name;
        if (attr & 0x10) {
            if (!Walk(sector, size, path, depth + 1)) return false;
        } else {
            files_.push_back({path, uint64_t(sector) * kSector, size});
        }
    }
    return true;
}

const Entry* Image::Find(const std::string& path) const {
    std::string want = Lower(path);
    for (const Entry& e : files_)
        if (Lower(e.path) == want) return &e;
    return nullptr;
}

bool Image::Read(const Entry& e, std::vector<uint8_t>& out) {
    out.resize(size_t(e.size));
    return e.size == 0 || ReadAt(e.offset, out.data(), out.size());
}

bool Image::Extract(const Entry& e, const std::wstring& to, const std::function<bool(uint64_t)>& progress) {
    FILE* o = _wfopen(to.c_str(), L"wb");
    if (!o) return false;
    std::vector<uint8_t> buf(4 << 20);
    uint64_t left = e.size, at = e.offset;
    bool ok = true;
    while (left > 0 && ok) {
        size_t n = size_t(std::min<uint64_t>(left, buf.size()));
        ok = ReadAt(at, buf.data(), n) && std::fwrite(buf.data(), 1, n, o) == n;
        at += n, left -= n;
        if (ok && progress && !progress(n)) ok = false;
    }
    std::fclose(o);
    return ok;
}

uint64_t Image::TotalSize() const {
    uint64_t t = 0;
    for (const Entry& e : files_) t += e.size;
    return t;
}

}  // namespace xdvdfs
