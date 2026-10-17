// The sampling profiler (sampler.h). The walk of a stopped thread's stack takes no lock it could hold (no
// allocation): RtlLookupFunctionEntry and RtlVirtualUnwind; names are looked up afterwards (DbgHelp, the exe's PDB).
#include "core/sampler.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sampler {
namespace {

constexpr int kFrames = 7;  // this exe's innermost frames kept

struct Sample {
    std::chrono::steady_clock::time_point t;
    DWORD tid;
    uint64_t leaf;             // where it was, any module
    uint64_t own[kFrames];     // this exe's frames, innermost first
};

std::mutex g_lock;
std::vector<Sample> g_samples;
std::atomic<bool> g_run{false};
std::thread g_thread;
uintptr_t g_lo = 0, g_hi = 0;

struct Watched {
    HANDLE h;
    DWORD tid;
    ULONG64 cycles;
};

void Refresh(std::vector<Watched>& threads) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof(te)};
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        if (std::any_of(threads.begin(), threads.end(), [&](const Watched& w) { return w.tid == te.th32ThreadID; })) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (h) threads.push_back({h, te.th32ThreadID, 0});
    }
    CloseHandle(snap);
}

void Run() {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
    g_lo = reinterpret_cast<uintptr_t>(dos), g_hi = g_lo + nt->OptionalHeader.SizeOfImage;
    std::vector<Watched> threads;
    auto refreshed = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    std::vector<Sample> batch;
    while (g_run) {
        auto now = std::chrono::steady_clock::now();
        if (now - refreshed > std::chrono::milliseconds(500)) Refresh(threads), refreshed = now;
        for (Watched& w : threads) {
            ULONG64 c = 0;
            if (!QueryThreadCycleTime(w.h, &c) || c == w.cycles) continue;  // not run since
            w.cycles = c;
            if (SuspendThread(w.h) == DWORD(-1)) continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(w.h, &ctx)) {
                Sample s{now, w.tid, ctx.Rip, {}};
                int own = 0;
                for (int depth = 0; depth < 48 && ctx.Rip && own < kFrames; depth++) {
                    if (ctx.Rip >= g_lo && ctx.Rip < g_hi) s.own[own++] = ctx.Rip;
                    DWORD64 image = 0;
                    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
                    if (!f) {  // a leaf: its return address on top of the stack
                        ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                        ctx.Rsp += 8;
                        continue;
                    }
                    void* handler_data = nullptr;
                    DWORD64 frame = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, f, &ctx, &handler_data, &frame, nullptr);
                }
                batch.push_back(s);
            }
            ResumeThread(w.h);
        }
        if (!batch.empty()) {
            std::lock_guard<std::mutex> l(g_lock);
            g_samples.insert(g_samples.end(), batch.begin(), batch.end());
            batch.clear();
        }
        Sleep(1);
    }
    for (Watched& w : threads) CloseHandle(w.h);
}

std::string Name(uint64_t a) {
    static std::unordered_map<uint64_t, std::string> names;
    static bool init = false;
    if (!init) {
        init = true;
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    }
    auto it = names.find(a);
    if (it != names.end()) return it->second;
    char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 off = 0;
    std::string n;
    if (SymFromAddr(GetCurrentProcess(), a, &off, sym)) {
        n = sym->Name;
    } else {
        char t[32];
        std::snprintf(t, sizeof(t), "%llx", (unsigned long long)a);
        n = t;
    }
    IMAGEHLP_MODULE64 mod{sizeof(mod)};
    if ((a < g_lo || a >= g_hi) && SymGetModuleInfo64(GetCurrentProcess(), a, &mod)) n = std::string(mod.ModuleName) + "!" + n;
    // this exe's own: its file and line too (functions in unnamed namespaces are missing from the names)
    if (a >= g_lo && a < g_hi) {
        IMAGEHLP_LINE64 line{sizeof(line)};
        DWORD disp = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), a, &disp, &line) && line.FileName) {
            std::string f = line.FileName;
            size_t slash = f.find_last_of("/\\");
            if (slash != std::string::npos) f = f.substr(slash + 1);
            if (f.rfind("x86_recomp", 0) != 0) n += " @" + f + ":" + std::to_string(line.LineNumber);
        }
    }
    return names[a] = n;
}

std::string ThreadName(DWORD tid) {
    std::string n = std::to_string(tid);
    if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid)) {
        PWSTR d = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &d)) && d && *d) {
            char t[128];
            WideCharToMultiByte(CP_UTF8, 0, d, -1, t, sizeof(t), nullptr, nullptr);
            n += " " + std::string(t);
        }
        if (d) LocalFree(d);
        CloseHandle(h);
    }
    return n;
}

}  // namespace

void Start() {
    Stop();
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_samples.clear();
    }
    g_run = true;
    g_thread = std::thread(Run);
}

void Stop() {
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
}

std::string Top(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to, int n) {
    std::map<std::pair<DWORD, std::string>, int> counts;
    {
        std::lock_guard<std::mutex> l(g_lock);
        for (const Sample& s : g_samples) {
            if (s.t < from || s.t > to) continue;
            std::string key = s.own[0] ? Name(s.own[0]) : "?";
            for (int k = 1; k < kFrames && s.own[k]; k++) key += " < " + Name(s.own[k]);
            if (s.leaf != s.own[0]) {
                std::string leaf = Name(s.leaf);
                // a thread waiting (woken now and then): not what took the time
                bool waiting = false;
                for (const char* w : {"Wait", "Delay", "GetMessage", "Sleep", "RemoveIoCompletion"})
                    waiting |= leaf.find(w) != std::string::npos;
                if (waiting) continue;
                key = "[" + leaf + "] " + key;
            }
            counts[{s.tid, key}]++;
        }
    }
    std::vector<std::pair<int, std::pair<DWORD, std::string>>> order;
    for (auto& [k, c] : counts) order.push_back({c, k});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string out;
    for (int i = 0; i < n && i < int(order.size()); i++)
        out += "      " + std::to_string(order[i].first) + "x thread " + ThreadName(order[i].second.first) + ": " +
               order[i].second.second + "\n";
    return out;
}

std::string Profile(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to, int n) {
    struct Thread {
        int busy = 0, waiting = 0;
        std::map<std::string, int> self, incl;
    };
    std::map<DWORD, Thread> threads;
    {
        std::lock_guard<std::mutex> l(g_lock);
        for (const Sample& s : g_samples) {
            if (s.t < from || s.t > to) continue;
            Thread& t = threads[s.tid];
            std::string leaf = Name(s.leaf);
            bool waiting = false;
            for (const char* w : {"Wait", "Delay", "GetMessage", "Sleep", "RemoveIoCompletion", "Alert"})
                waiting |= leaf.find(w) != std::string::npos;
            if (waiting) {
                t.waiting++;
                continue;
            }
            t.busy++;
            t.self[leaf]++;
            std::map<std::string, bool> seen;
            for (int k = 0; k < kFrames && s.own[k]; k++) {
                std::string f = Name(s.own[k]);
                if (!seen[f]) t.incl[f]++, seen[f] = true;
            }
        }
    }
    int all = 0;
    for (auto& [tid, t] : threads) all += t.busy;
    std::vector<std::pair<int, DWORD>> order;
    for (auto& [tid, t] : threads) order.push_back({t.busy, tid});
    std::sort(order.rbegin(), order.rend());
    std::string out;
    auto top = [&](const std::map<std::string, int>& m, int busy, const char* what) {
        std::vector<std::pair<int, std::string>> v;
        for (auto& [f, c] : m) v.push_back({c, f});
        std::sort(v.rbegin(), v.rend());
        for (int i = 0; i < n && i < int(v.size()) && v[i].first * 200 >= busy; i++) {
            char line[64];
            std::snprintf(line, sizeof(line), "        %s %5.1f%% ", what, 100.0 * v[i].first / std::max(busy, 1));
            out += line + v[i].second.substr(0, 160) + "\n";
        }
    };
    for (auto& [busy, tid] : order) {
        if (busy * 50 < all) continue;  // threads under 2% of the busy samples
        const Thread& t = threads[tid];
        char head[160];
        std::snprintf(head, sizeof(head), "      thread %s: %d busy samples (%.1f%% of all), %d waiting\n", ThreadName(tid).c_str(), busy,
                      100.0 * busy / std::max(all, 1), t.waiting);
        out += head;
        top(t.self, busy, "self");
        top(t.incl, busy, "incl");
    }
    return out;
}

}  // namespace sampler
