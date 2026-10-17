// Tests: who reads a field of the game's memory. XBE_WATCH=<what> arms a guard page over it; each access to the
// watched bytes records the host instruction, and the control line "cmd watchdump" names them through the pdb
// (the recompiled function each lies in, sub_XXXXXXXX: the console address).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>

#include "core/log.h"
#include "kernel/kernel.h"

#pragma comment(lib, "dbghelp.lib")

namespace cod::watch {
namespace {

std::atomic<uintptr_t> g_lo{0}, g_hi{0};
std::atomic<void*> g_page{nullptr};
std::mutex g_lock;
std::map<uintptr_t, uint32_t> g_hits;
thread_local bool t_rearm = false;

LONG CALLBACK Handler(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    void* page = g_page.load();
    if (!page) return EXCEPTION_CONTINUE_SEARCH;
    if (code == STATUS_GUARD_PAGE_VIOLATION) {
        uintptr_t a = e->ExceptionRecord->ExceptionInformation[1];
        if (a >= g_lo && a < g_hi) {
            std::lock_guard l(g_lock);
            g_hits[uintptr_t(e->ContextRecord->Rip)]++;
        }
        // single step the access, then guard the page again
        e->ContextRecord->EFlags |= 0x100;
        t_rearm = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == STATUS_SINGLE_STEP && t_rearm) {
        t_rearm = false;
        DWORD old;
        VirtualProtect(page, 1, PAGE_READWRITE | PAGE_GUARD, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

// watch the guest bytes [va, va + n)
void Arm(uint32_t va, uint32_t n) {
    if (g_page) return;
    uintptr_t lo = uintptr_t(kernel::HostPtr(va));
    g_lo = lo, g_hi = lo + n;
    void* page = reinterpret_cast<void*>(lo & ~uintptr_t(0xFFF));
    AddVectoredExceptionHandler(1, Handler);
    g_page = page;
    DWORD old;
    VirtualProtect(page, 1, PAGE_READWRITE | PAGE_GUARD, &old);
    LOGR("watch: guest %08X..%08X armed", va, va + n);
}

void Dump() {
    HANDLE p = GetCurrentProcess();
    static bool init = SymInitialize(p, nullptr, TRUE);
    (void)init;
    std::map<uintptr_t, uint32_t> hits;
    {
        std::lock_guard l(g_lock);
        hits = g_hits;
    }
    for (auto [rip, n] : hits) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* s = reinterpret_cast<SYMBOL_INFO*>(buf);
        s->SizeOfStruct = sizeof(SYMBOL_INFO);
        s->MaxNameLen = 255;
        DWORD64 off = 0;
        const char* name = SymFromAddr(p, rip, &off, s) ? s->Name : "?";
        LOGR("watch: %6u reads at %s+0x%llx", n, name, (unsigned long long)off);
    }
}

}  // namespace cod::watch
