// See control.h.
#include "core/control.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"

namespace control {
namespace {

// tests: "pad <button> [ms]": the first pad's button held a while (150 ms), over the replay or the live pad
std::atomic<uint32_t> g_test_buttons{0};
std::atomic<int> g_test_analog{-1};
std::atomic<int64_t> g_test_pad_until{0};  // steady clock, ms
int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// clock -------------------------------------------------------------------------------------------------------------

struct Origin {
    LARGE_INTEGER freq{}, t0{};
    unsigned long long origin_ft = 0;  // FILETIME of the first instance's creation

    Origin() {
        FILETIME ct, xt, kt, ut, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        GetSystemTimeAsFileTime(&now);
        GetProcessTimes(GetCurrentProcess(), &ct, &xt, &kt, &ut);
        origin_ft = (unsigned long long)ct.dwHighDateTime << 32 | ct.dwLowDateTime;
        if (const char* e = std::getenv("XBE_CLOCK_ORIGIN"); e && *e) origin_ft = std::strtoull(e, nullptr, 10);
        unsigned long long now_ft = (unsigned long long)now.dwHighDateTime << 32 | now.dwLowDateTime;
        unsigned long long age_ms = now_ft > origin_ft ? (now_ft - origin_ft) / 10000ull : 0;
        t0.QuadPart -= (LONGLONG)(age_ms * (unsigned long long)freq.QuadPart / 1000ull);
    }
};

Origin& TheOrigin() {
    static Origin o;
    return o;
}

// control file ------------------------------------------------------------------------------------------------------

std::mutex g_lock;
std::wstring g_shot;
std::wstring g_record;
bool g_record_due = false;
std::atomic<void (*)(const std::string&)> g_command{nullptr};

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

void Line(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
    if (line.rfind("record ", 0) == 0) {
        std::lock_guard lock(g_lock);
        std::string arg = line.substr(7);
        g_record = arg == "stop" ? std::wstring() : Widen(arg);
        g_record_due = true;
    } else if (line.rfind("shot ", 0) == 0) {
        std::lock_guard lock(g_lock);
        g_shot = Widen(line.substr(5));
    } else if (line.rfind("key ", 0) == 0) {  // key <virtual key, hex>: pressed and let go on the game window
        unsigned vk = unsigned(std::strtoul(line.c_str() + 4, nullptr, 16));
        HWND w = static_cast<HWND>(app::Window());
        LPARAM lp = LPARAM(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16 | 1);
        PostMessageW(w, vk == VK_F10 ? WM_SYSKEYDOWN : WM_KEYDOWN, vk, lp);
        PostMessageW(w, vk == VK_F10 ? WM_SYSKEYUP : WM_KEYUP, vk, lp | LPARAM(0xC0000000));
    } else if (line.rfind("pad ", 0) == 0) {
        char name[16] = {};
        int ms = 150;
        std::sscanf(line.c_str() + 4, "%15s %d", name, &ms);
        static const std::pair<const char*, uint32_t> kDigital[] = {{"UP", 0x1},    {"DOWN", 0x2},   {"LEFT", 0x4},
                                                                    {"RIGHT", 0x8}, {"START", 0x10}, {"BACK", 0x20}};
        static const char* const kAnalog[] = {"A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT"};
        uint32_t b = 0;
        int an = -1;
        for (const auto& [n, v] : kDigital)
            if (!std::strcmp(name, n)) b = v;
        for (int i = 0; i < 8; i++)
            if (!std::strcmp(name, kAnalog[i])) an = i;
        g_test_buttons = b, g_test_analog = an, g_test_pad_until = SteadyMs() + ms;
    } else if (line == "draws") {
        gpu::g_log_draws = 3;
    } else if (line == "quit") {  // a test's end: the log kept, the process gone
        LOGR("control: quit");
        logx::Flush();
        TerminateProcess(GetCurrentProcess(), 0);
    } else if (line == "threads") {
        for (auto& t : kernel::Threads())
            if (t->ctx) {
                LOGR("thread %X", t->id);
                kernel::LogGuestStack(*t->ctx, 48);
            }
    } else if (line.rfind("cmd ", 0) == 0) {
        if (auto h = g_command.load()) h(line.substr(4));
    } else if (!line.empty()) {
        LOGW("control: unknown line '%s'", line.c_str());
        return;
    }
    LOGR("control: %s", line.c_str());
}

void Thread(std::string path, long long done) {
    for (;;) {
        Sleep(50);
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        long long size = _ftelli64(f);
        if (size < done) done = 0;  // the file was started over
        std::string text;
        if (size > done) {
            text.resize(size_t(size - done));
            _fseeki64(f, done, SEEK_SET);
            text.resize(std::fread(text.data(), 1, text.size(), f));
        }
        std::fclose(f);
        size_t end = text.rfind('\n');
        if (end == std::string::npos) continue;  // only whole lines
        done += (long long)end + 1;
        size_t at = 0;
        while (at <= end) {
            size_t nl = text.find('\n', at);
            Line(text.substr(at, nl - at));
            at = nl + 1;
        }
    }
}

// input recording ---------------------------------------------------------------------------------------------------

struct ReplayEvent {
    double ms;
    input::Pad pad;
};

struct Recorder {
    std::mutex mu;
    FILE* rec = nullptr;
    input::Pad last[4];
    bool seen[4] = {};
    std::vector<ReplayEvent> replay[4];
    struct Motion {
        double ms;
        int dx, dy;
    };
    std::vector<Motion> mouse;
    size_t next_mouse = 0;
    double anchor = -1, shift = 0;  // the replay's clock: since the origin, plus shift once anchored
    double Now() const { return SinceOriginMs() + shift; }
    size_t next[4] = {};
    input::Pad current[4];
    double replay_end = -1;

    Recorder() {
        // appended: a relaunch into the next level's image carries on the same recording
        if (const char* e = std::getenv("XBE_INPUT_RECORD"); e && *e) rec = std::fopen(e, "a");
        if (const char* e = std::getenv("XBE_INPUT_REPLAY"); e && *e) Load(e);
        SinceOriginMs();
    }
    void Load(const char* path) {
        FILE* f = std::fopen(path, "r");
        if (!f) return;
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            double ms;
            int port, conn, a[8], lx, ly, rx, ry;
            int mx, my;
            if (std::sscanf(line, "%lf mouse %d %d", &ms, &mx, &my) == 3) {
                mouse.push_back({ms, mx, my});
                continue;
            }
            if (std::strstr(line, " anchor") && std::sscanf(line, "%lf", &ms) == 1) {
                anchor = ms;
                continue;
            }
            unsigned buttons;
            if (std::sscanf(line, "%lf %d %d %x %d %d %d %d %d %d %d %d %d %d %d %d", &ms, &port, &conn, &buttons, &a[0], &a[1],
                            &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &lx, &ly, &rx, &ry) != 16 ||
                port < 0 || port > 3)
                continue;
            input::Pad p;
            p.connected = conn != 0;
            p.buttons = uint16_t(buttons);
            for (int i = 0; i < 8; i++) p.analog[i] = uint8_t(a[i]);
            p.lx = int16_t(lx), p.ly = int16_t(ly), p.rx = int16_t(rx), p.ry = int16_t(ry);
            replay[port].push_back({ms, p});
            replay_end = std::max(replay_end, ms + 1000.0);
        }
        std::fclose(f);
        LOGR("input replay: %s, until %.1f s", path, replay_end / 1000.0);
    }
    static bool Same(const input::Pad& a, const input::Pad& b) {
        return a.connected == b.connected && a.buttons == b.buttons && !std::memcmp(a.analog, b.analog, 8) && a.lx == b.lx &&
               a.ly == b.ly && a.rx == b.rx && a.ry == b.ry;
    }
    input::Pad Filter(int port, input::Pad live) {
        if (port < 0 || port > 3) return live;
        std::lock_guard lk(mu);
        double now = Now();
        if (now < replay_end) {
            auto& ev = replay[port];
            while (next[port] < ev.size() && ev[next[port]].ms <= now) current[port] = ev[next[port]++].pad;
            live = current[port];
        }
        if (port == 0 && SteadyMs() < g_test_pad_until.load()) {
            live.connected = true;
            live.buttons |= uint16_t(g_test_buttons.load());
            if (int an = g_test_analog.load(); an >= 0) live.analog[an] = 255;
        }
        if (rec && (!seen[port] || !Same(last[port], live))) {
            seen[port] = true;
            last[port] = live;
            std::fprintf(rec, "%.1f %d %d %04X %u %u %u %u %u %u %u %u %d %d %d %d\n", now, port, live.connected ? 1 : 0,
                         live.buttons, live.analog[0], live.analog[1], live.analog[2], live.analog[3], live.analog[4],
                         live.analog[5], live.analog[6], live.analog[7], live.lx, live.ly, live.rx, live.ry);
            std::fflush(rec);
        }
        return live;
    }
};

Recorder& TheRecorder() {
    static Recorder r;
    return r;
}

}  // namespace

double SinceOriginMs() {
    Origin& o = TheOrigin();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - o.t0.QuadPart) * 1000.0 / double(o.freq.QuadPart);
}

void PublishOrigin() {
    char v[32];
    std::snprintf(v, sizeof v, "%llu", TheOrigin().origin_ft);
    SetEnvironmentVariableA("XBE_CLOCK_ORIGIN", v);
}

void Start() {
    TheRecorder();
    const char* e = std::getenv("XBE_CONTROL");
    if (!e || !*e) return;
    // a relaunched instance follows the same file from its current end: the lines before were for the old one
    std::string path = e;
    long long size = 0;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        size = _ftelli64(f);
        std::fclose(f);
    }
    std::thread([path, size] { Thread(path, size); }).detach();
}

bool TakeRecord(std::wstring& path) {
    std::lock_guard lock(g_lock);
    static bool launched = false;
    if (!launched) {
        launched = true;
        if (const char* e = std::getenv("XBE_RECORD"); e && *e) g_record = Widen(e), g_record_due = true;
    }
    if (!g_record_due) return false;
    g_record_due = false;
    path = g_record;
    return true;
}

bool TakeShot(std::wstring& path) {
    std::lock_guard lock(g_lock);
    if (g_shot.empty()) return false;
    path.swap(g_shot);
    g_shot.clear();
    return true;
}

void SetCommandHandler(void (*handler)(const std::string&)) { g_command = handler; }

input::Pad Filter(int port, const input::Pad& live) { return TheRecorder().Filter(port, live); }

void RecordLine(const char* kind, const char* text) {
    Recorder& r = TheRecorder();
    if (!r.rec) return;
    std::lock_guard lk(r.mu);
    std::fprintf(r.rec, "%.1f %s %s\n", SinceOriginMs(), kind, text);
    std::fflush(r.rec);
}

bool Recording() { return TheRecorder().rec != nullptr; }

bool ReplayMouse(int& dx, int& dy) {
    Recorder& r = TheRecorder();
    std::lock_guard lk(r.mu);
    double now = r.Now();
    if (r.replay_end < 0 || now >= r.replay_end) return false;
    dx = dy = 0;
    while (r.next_mouse < r.mouse.size() && r.mouse[r.next_mouse].ms <= now) {
        dx += r.mouse[r.next_mouse].dx;
        dy += r.mouse[r.next_mouse].dy;
        r.next_mouse++;
    }
    return true;
}

void Shot(const std::wstring& path) {
    std::lock_guard lock(g_lock);
    g_shot = path;
}

void Anchor() {
    Recorder& r = TheRecorder();
    std::lock_guard lk(r.mu);
    if (r.anchor < 0 || r.shift != 0) return;
    r.shift = r.anchor - SinceOriginMs();
    LOGR("input replay: anchored, %.1f s on", r.anchor / 1000.0);
}

bool Replaying() {
    Recorder& r = TheRecorder();
    return r.replay_end >= 0 && r.Now() < r.replay_end;
}

}  // namespace control
