// Reading the game from an Xbox disc image: the XDVDFS file system of a "redump" ISO (the game partition at
// 0x18300000, after the video partition) or of an XISO (the game partition alone, at 0). Files are listed with
// their paths and extracted, nothing else.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace xdvdfs {

struct Entry {
    std::string path;  // with '/' between folders
    uint64_t offset = 0;  // in the image
    uint64_t size = 0;
};

class Image {
  public:
    ~Image();
    bool Open(const std::wstring& path, std::string& error);
    const std::vector<Entry>& Files() const { return files_; }
    const Entry* Find(const std::string& path) const;  // case-insensitive
    bool Read(const Entry& e, std::vector<uint8_t>& out);
    // copies the entry into a host file, reporting the bytes as they go
    bool Extract(const Entry& e, const std::wstring& to, const std::function<bool(uint64_t)>& progress);
    uint64_t TotalSize() const;

  private:
    bool ReadAt(uint64_t at, void* dst, size_t n);
    bool Walk(uint64_t dir_sector, uint32_t dir_size, const std::string& prefix, int depth);
    FILE* f_ = nullptr;
    uint64_t base_ = 0;
    std::vector<Entry> files_;
};

}  // namespace xdvdfs
