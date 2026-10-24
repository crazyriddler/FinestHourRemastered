#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace gpu {

// a key that is already a hash, or an address: mixed with one multiply (the library's std::hash runs FNV over its
// bytes, eight multiplies in a row, for every lookup of the maps looked up on every draw)
struct KeyHash {
    size_t operator()(uint64_t k) const noexcept {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdull;
        return size_t(k ^ (k >> 32));
    }
};
template <class K, class T>
using KeyMap = std::unordered_map<K, T, KeyHash>;

}  // namespace gpu
