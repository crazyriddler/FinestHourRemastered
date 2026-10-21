// Tests (XBE_POISON): the game's heap made to show its use of freed memory at once, as a debug heap does. Every
// block the game's C runtime hands out (malloc, sub_001E25A3; realloc, sub_001E2688) has its size kept (XBE_POISON=all
// also fills it with 0xCD: the game then stops after its image starts again, waiting on a block it takes for
// zeroed, as fresh memory is on the console); every block it frees (free, sub_001E2406), and every entry RenderWare's free lists take back
// (RwFreeListFree, sub_001C2FD0: its entry size the list's first field), is filled with 0xDD before it goes back. A
// pointer read from freed memory is then 0xDDDDDDDD, outside the console's memory: the access faults where it is
// made (the crash log names the guest's stack), not hours later where the memory, used again, was trodden on.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {
enum Mode { kOff, kPoison, kAll };

Mode TheMode() {
    static const Mode m = [] {
        const char* e = std::getenv("XBE_POISON");
        if (!e || !*e) return kOff;
        const bool all = std::strcmp(e, "all") == 0;
        LOGR("debug heap: freed memory filled with 0xDD%s", all ? ", new with 0xCD" : "");
        return all ? kAll : kPoison;
    }();
    return m;
}

std::mutex g_lock;
std::unordered_map<uint32_t, uint32_t> g_sizes;  // the C runtime's blocks: address -> size

void Fill(uint32_t va, uint32_t n, uint8_t v) {
    if (!va || !n || n > (64u << 20)) return;
    std::memset(HostPtr(va), v, n);
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_001E25A3);
X86_FUNC(sub_001E25A3) {  // malloc(size)
    const uint32_t size = R32(ctx.esp + 4);
    __imp__sub_001E25A3(ctx, base);
    if (TheMode() == kOff || !ctx.eax) return;
    std::lock_guard<std::mutex> guard(g_lock);
    g_sizes[ctx.eax] = size;
    if (TheMode() == kAll) Fill(ctx.eax, size, 0xCD);
}

X86_EXTERN_FUNC(__imp__sub_001E2688);
X86_FUNC(sub_001E2688) {  // realloc(block, size)
    const uint32_t old = R32(ctx.esp + 4), size = R32(ctx.esp + 8);
    __imp__sub_001E2688(ctx, base);
    if (TheMode() == kOff) return;
    std::lock_guard<std::mutex> guard(g_lock);
    uint32_t was = 0;
    if (auto it = g_sizes.find(old); it != g_sizes.end()) was = it->second, g_sizes.erase(it);
    if (ctx.eax) {
        g_sizes[ctx.eax] = size;
        if (size > was && TheMode() == kAll) Fill(ctx.eax + was, size - was, 0xCD);  // (the new part)
    }
}

X86_EXTERN_FUNC(__imp__sub_001E2406);
X86_FUNC(sub_001E2406) {  // free(block)
    const uint32_t block = R32(ctx.esp + 4);
    if (TheMode() != kOff && block) {
        std::lock_guard<std::mutex> guard(g_lock);
        if (auto it = g_sizes.find(block); it != g_sizes.end()) {
            Fill(block, it->second, 0xDD);
            g_sizes.erase(it);
        }
    }
    __imp__sub_001E2406(ctx, base);
}

// RwFreeListFree(list, entry): the entry filled before the list takes it back (not the list's own fixed entries,
// between [0x3F6DE0] and [0x3F6DE4], which it leaves)
X86_EXTERN_FUNC(__imp__sub_001C2FD0);
X86_FUNC(sub_001C2FD0) {
    const uint32_t list = R32(ctx.esp + 4), entry = R32(ctx.esp + 8);
    if (TheMode() != kOff && list && entry && !(entry >= R32(0x3F6DE0) && entry <= R32(0x3F6DE4))) {
        const uint32_t size = R32(list);  // the list's entry size
        if (size && size < 0x10000) Fill(entry, size, 0xDD);
    }
    __imp__sub_001C2FD0(ctx, base);
}
