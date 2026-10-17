#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/app.h"
#include "common/cod.h"
#include "common/overlay.h"
#include "common/pc_input.h"
#include "common/prompts.h"
#include "common/widescreen.h"
#include "core/control.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <cstdlib>

namespace cod {
bool InMenu() { return wide::FrontEnd() || Paused() || InMovie(); }

Settings& Get() {
    static Settings s;
    return s;
}
}  // namespace cod

namespace cod::watch {
void Dump();  // watch.cpp (tests)
}
namespace cod::menu_mouse {
void Dump();    // menu_mouse.cpp
void TestMouse(float x, float y, bool click);
void Update();  // render thread, each frame
void Draw();
void Filter(input::Pad& pad);  // the game's poll of port 0, last: the main menu's Quit Game
}
namespace cod::offline {
void Init();  // offline.cpp: the Multiplayer entry becomes PC Settings
}
extern int g_level_request;  // offline.cpp, tests: a level started from the front end
namespace cod::ragdoll {
void NewFrame();  // bodies.cpp: a new frame for the bodies' poses
}
namespace cod::damage {
extern std::atomic<bool> g_test;  // damage.cpp: the pain effect started, for tests
}

namespace {

// tests: where the game keeps a value. memsnap takes a copy of guest memory; memseq <pattern> lists the 32-bit words
// whose values over the copies follow the pattern (memseq abcbc: three values, the same letters the same value)
std::vector<std::vector<uint8_t>> g_snaps;
double g_fps = 0;  // the overlay's count, for the fps command

// tests: an effect on or off while the game runs (fx <name> <0|1>), not saved
bool* Effect(const std::string& name) {
    app::Settings& s = app::Get();
    static const std::pair<const char*, bool app::Settings::*> kEffects[] = {
        {"bloom", &app::Settings::bloom},
        {"hit_reactions", &app::Settings::hit_reactions},
        {"ao", &app::Settings::ao},
        {"contact_shadows", &app::Settings::contact_shadows},
        {"dynamic_lights", &app::Settings::dynamic_lights},
        {"light_halos", &app::Settings::light_halos},
        {"soft_particles", &app::Settings::soft_particles},
        {"heat_haze", &app::Settings::heat_haze},
        {"sun_shafts", &app::Settings::sun_shafts},
        {"aim_dof", &app::Settings::aim_dof},
        {"explosion_flash", &app::Settings::explosion_flash},
        {"flash_shadows", &app::Settings::flash_shadows},
        {"auto_exposure", &app::Settings::auto_exposure},
        {"cloud_shadows", &app::Settings::cloud_shadows},
        {"volumetric_fog", &app::Settings::volumetric_fog},
        {"soft_shadows", &app::Settings::soft_shadows},
        {"bounce_light", &app::Settings::bounce_light},
        {"character_light", &app::Settings::character_light},
        {"lit_smoke", &app::Settings::lit_smoke},
        {"ambient_particles", &app::Settings::ambient_particles},
        {"wind", &app::Settings::wind},
        {"barrel_heat", &app::Settings::barrel_heat},
        {"animated_fire", &app::Settings::animated_fire},
        {"shockwaves", &app::Settings::shockwaves},
        {"water", &app::Settings::water},
        {"water_interaction", &app::Settings::water_interaction},
        {"snow_tracks", &app::Settings::snow_tracks},
        {"lens_flare", &app::Settings::lens_flare},
        {"sharp_distance", &app::Settings::sharp_distance},
        {"weapon_materials", &app::Settings::weapon_materials},
        {"character_detail", &app::Settings::character_detail},
        {"color_grade", &app::Settings::color_grade},
        {"film_grain", &app::Settings::film_grain},
        {"smooth_models", &app::Settings::smooth_models},
        {"reflections", &app::Settings::reflections},
        {"sun_moon", &app::Settings::sun_moon},
        {"clouds", &app::Settings::clouds},
        {"materials", &app::Settings::materials},
        {"parallax", &app::Settings::parallax},
        {"detail", &app::Settings::detail},
        {"pixel_lighting", &app::Settings::pixel_lighting},
        {"height_blend", &app::Settings::height_blend},
        {"smaa", &app::Settings::smaa},
        {"taa", &app::Settings::taa},
        {"motion_blur", &app::Settings::motion_blur},
    };
    for (const auto& [n, member] : kEffects)
        if (name == n) return &(s.*member);
    return nullptr;
}

// tests: the player held where warp put it ("warp x y z hold"): its places in memory written again and again with
// the target (and each one's own offset from the camera), so it neither falls nor dies of the fall where the level is
// not loaded; "warp off" lets it go
struct Held {
    uint32_t at;
    float offset[3];
};
std::mutex g_held_lock;
std::vector<Held> g_held;
float g_held_to[3] = {};
std::atomic<bool> g_holding{false};

void HoldPlayer(const std::vector<Held>& places, const float to[3]) {
    {
        std::lock_guard lock(g_held_lock);
        g_held = places;
        for (int i = 0; i < 3; i++) g_held_to[i] = to[i];
    }
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            for (;;) {
                if (g_holding) {
                    std::lock_guard lock(g_held_lock);
                    for (const Held& h : g_held)
                        for (int i = 0; i < 3; i++) {
                            float v = g_held_to[i] + h.offset[i];
                            uint32_t bits;
                            std::memcpy(&bits, &v, 4);
                            kernel::W32(h.at + uint32_t(i) * 4, bits);
                        }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    });
    g_holding = !places.empty();
}

void MemSnap() {
    constexpr uint32_t kSize = 0x4000000;
    std::vector<uint8_t> now(kSize);
    uint8_t* base = static_cast<uint8_t*>(kernel::HostPtr(0));
    for (uint32_t va = 0; va < kSize;) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(base + va, &mi, sizeof(mi))) break;
        uint32_t end = uint32_t(std::min<uint64_t>(kSize, uint64_t(static_cast<uint8_t*>(mi.BaseAddress) - base) + mi.RegionSize));
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) std::memcpy(now.data() + va, base + va, end - va);
        va = end;
    }
    g_snaps.push_back(std::move(now));
    LOGR("memsnap %zu", g_snaps.size());
}

void MemSeq(const std::string& args) {
    // memseq <pattern> [below] [width]: values under <below> alone (an index), of 1, 2 or 4 bytes
    char pattern[17] = {};
    uint32_t below = 0xFFFFFFFF, width = 4;
    if (std::sscanf(args.c_str(), "%16s %i %u", pattern, &below, &width) < 1) return;
    size_t len = std::strlen(pattern);
    if (width != 1 && width != 2) width = 4;
    if (len != g_snaps.size()) return LOGR("memseq: %zu copies for %zu letters", g_snaps.size(), len);
    size_t found = 0;
    for (uint32_t va = 0x10000; va + 4 <= g_snaps[0].size(); va += width) {
        uint32_t v[16];
        bool ok = true;
        for (size_t i = 0; i < len && ok; i++) {
            v[i] = 0;
            std::memcpy(&v[i], g_snaps[i].data() + va, width);
            ok = v[i] < below;
            for (size_t j = 0; j < i && ok; j++) ok = (pattern[i] == pattern[j]) == (v[i] == v[j]);
        }
        if (!ok) continue;
        if (++found <= 20000) {
            char text[256];
            int n = std::snprintf(text, sizeof(text), "memseq:   %08X", va);
            for (size_t i = 0; i < len && n < 230; i++) n += std::snprintf(text + n, sizeof(text) - size_t(n), " %X", v[i]);
            LOGR("%s", text);
        }
    }
    LOGR("memseq %s: %zu places", pattern, found);
}

bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    // the testers' keys, for developer = true (or XBE_DEV) alone: F3 (the runtime's threads and a frame's capture) is
    // kept from the runtime otherwise
    static const bool developer = cod::Get().developer || std::getenv("XBE_DEV") != nullptr;
    if (msg == WM_KEYDOWN && (wp == VK_F3 || wp == VK_F9) && !developer) {
        *result = 0;
        return true;
    }
    // F9: a screenshot in logs/marks (named by the time), a mark in the log, and in the recording when there is one
    if (msg == WM_KEYDOWN && wp == VK_F9 && !(lp & (1 << 30))) {
        static int marks = 0;
        char text[48];
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::snprintf(text, sizeof(text), "%04d%02d%02d_%02d%02d%02d_%d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                      st.wSecond, ++marks);
        if (control::Recording()) control::RecordLine("mark", text);
        LOGR("mark %s at %.1f s", text, control::SinceOriginMs() / 1000.0);
        gpu::g_log_draws = 2;  // the frame's draws in the log, their textures: what the screenshot shows
        std::filesystem::create_directories(app::ExeDir() / "logs" / "marks");
        control::Shot((app::ExeDir() / "logs" / "marks" / ("mark_" + std::string(text) + ".bmp")).wstring());
        // where it was: the level, the camera's place and heading, beside the screenshot (tests go back there:
        // "level N", then "warp x y z")
        {
            float eye[3], yaw = 0, pitch = 0;
            if (cod::wide::CameraPos(eye)) {
                cod::wide::CameraAngles(yaw, pitch);
                LOGR("mark %s: level %d warp %.1f %.1f %.1f yaw %.1f pitch %.1f", text, gpu::g_level.load(), eye[0], eye[1], eye[2], yaw, pitch);
                int reg = 0, secs[4];
                bool known = cod::LastCheckpoint(reg, secs);
                char cp[64];
                if (known) std::snprintf(cp, sizeof(cp), "checkpoint %d %d %d %d %d", reg, secs[0], secs[1], secs[2], secs[3]);
                else std::snprintf(cp, sizeof(cp), "checkpoint %d", reg);
                int loaded[4];
                cod::LoadedSections(loaded);
                char ls[64];
                std::snprintf(ls, sizeof(ls), "sections %d %d %d %d", loaded[0], loaded[1], loaded[2], loaded[3]);
                LOGR("mark %s: %s, %s loaded", text, cp, ls);
                if (FILE* f = _wfopen((app::ExeDir() / "logs" / "marks" / ("mark_" + std::string(text) + ".txt")).c_str(), L"w")) {
                    std::fprintf(f, "level %d\nwarp %.1f %.1f %.1f\nyaw %.1f pitch %.1f\n", gpu::g_level.load(), eye[0], eye[1], eye[2], yaw, pitch);
                    std::fprintf(f, "%s\n", cp);
                    std::fprintf(f, "%s\n", ls);
                    std::fclose(f);
                }
            }
        }
        *result = 0;
        return true;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == VK_F10 && !(lp & (1 << 30))) {
        cod::overlay::Toggle();
        *result = 0;
        return true;
    }
    if (msg == WM_SYSKEYDOWN && wp == VK_F10) {  // F10 alone would open the window menu
        *result = 0;
        return true;
    }
    return pc_input::Message(hwnd, msg, wp, lp, result);
}

// The modern shooter layout on the game's buttons (its Standard layout): A jump (its Y), B crouch, X reload (white),
// Y next weapon (d-pad left), LB use (black), RB grenade (its X), right stick click melee (its A), d-pad down centre
// turret (its right stick click). SDL reports LB as white and RB as black.
void Modern(input::Pad& p) {
    const uint8_t a = p.analog[0], b = p.analog[1], x = p.analog[2], y = p.analog[3], rb = p.analog[4], lb = p.analog[5];
    const uint16_t in = p.buttons;
    p.analog[0] = (in & 0x80) ? 255 : 0;  // melee
    p.analog[1] = b;                       // crouch
    p.analog[2] = rb;                      // grenade
    p.analog[3] = a;                       // jump
    p.analog[4] = lb;                      // use
    p.analog[5] = x;                       // reload
    uint16_t out = in & (0x1 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40);
    if (y > 40) out |= 0x4;                // next weapon
    if (in & 0x2) out |= 0x80;             // centre turret
    p.buttons = out;
}

void Pad(int port, input::Pad& pad) {
    // the controller in use: a button, a trigger or a stick off centre
    if (pad.connected) {
        bool used = pad.buttons != 0;
        for (uint8_t a : pad.analog) used |= a > 40;
        for (int16_t v : {pad.lx, pad.ly, pad.rx, pad.ry}) used |= v > 12000 || v < -12000;
        if (used) cod::prompts::PadUsed();
        if (port == 0 && cod::Get().pad_layout == 0 && !cod::InMenu()) Modern(pad);
    }
    // the overlay takes the controller for itself
    if (cod::OverlayOpen()) {
        bool connected = pad.connected;
        pad = input::Pad{};
        pad.connected = connected;
    }
    pc_input::Pad(port, pad);
    if (port == 0) cod::menu_mouse::Filter(pad);
    if (port == 0 && !cod::InMenu() && !cod::OverlayOpen()) cod::wide::FlyUpdate(pad);  // Free Camera
    if (port == 0) gpu::g_aiming = pad.analog[6] > 100 && !cod::InMenu() && !cod::OverlayOpen();  // LT: aim
    // what the game is handed, keyboard and all, when it changes
    if (port == 0 && control::Recording()) {
        static input::Pad last;
        static bool first = true;
        if (first || last.buttons != pad.buttons || std::memcmp(last.analog, pad.analog, 8) || last.lx != pad.lx ||
            last.ly != pad.ly || last.rx != pad.rx || last.ry != pad.ry) {
            first = false;
            last = pad;
            char text[128];
            std::snprintf(text, sizeof(text), "%04X %u %u %u %u %u %u %u %u %d %d %d %d", pad.buttons, pad.analog[0],
                          pad.analog[1], pad.analog[2], pad.analog[3], pad.analog[4], pad.analog[5], pad.analog[6],
                          pad.analog[7], pad.lx, pad.ly, pad.rx, pad.ry);
            control::RecordLine("game", text);
        }
    }
}

// control channel commands (tests): look <dx> <dy>
void Command(const std::string& line) {
    int dx = 0, dy = 0;
    if (std::sscanf(line.c_str(), "look %d %d", &dx, &dy) == 2) pc_input::AddLook(dx, dy);
    if (std::sscanf(line.c_str(), "level %d", &dx) == 1) g_level_request = dx;
    if (std::sscanf(line.c_str(), "hold %x %d", &dx, &dy) == 2) pc_input::HoldKey(dx, dy);
    if (line == "menudump") cod::menu_mouse::Dump();
    {
        if (line == "memsnap") MemSnap();
        char name[40] = {};
        int on = 0;
        if (std::sscanf(line.c_str(), "fx %39s %d", name, &on) == 2) {
            if (std::string(name) == "shadows") app::Get().shadow_quality = on ? 4 : 0, LOGR("fx: shadows %d", on);
            else if (std::string(name) == "remaster") app::Get().remaster = on != 0, LOGR("fx: remaster %d", on);
            else if (std::string(name) == "specular") app::Get().specular = uint32_t(std::max(on, 0)), LOGR("fx: specular %d", on);
            else if (bool* e = Effect(name)) *e = on != 0, LOGR("fx: %s %d", name, on);
            else LOGR("fx: no effect %s", name);
        }
        if (line == "fps") LOGR("fps: %.1f", g_fps);
        if (line == "capture") gpu::g_capture = true;  // a frame to XBE_CAPTURE_DIR (as F3)
        // tests: where the camera is; warp x y z: every place in memory that holds the player's position (three
        // floats by the camera: the same across, at most 200 below) moved by the camera's way to x y z
        float eye[3];
        if (line == "whereami" && cod::wide::CameraPos(eye)) {
            float yaw = 0, pitch = 0;
            cod::wide::CameraAngles(yaw, pitch);
            LOGR("whereami: %.1f %.1f %.1f yaw %.1f pitch %.1f", eye[0], eye[1], eye[2], yaw, pitch);
        }
        float to[3];
        if (line == "warp off") g_holding = false, LOGR("warp: let go");
        float tl[4] = {0, 0, 0, 480};
        if (line == "testlight off") gpu::g_test_light[3] = 0.0f;
        else if (int mf = 0; std::sscanf(line.c_str(), "map %d", &mf) == 1)
            gpu::g_map_frames = mf, LOGR("map: gathering %d frames", mf);
        else if (std::sscanf(line.c_str(), "testlight %f %f %f %f", &tl[0], &tl[1], &tl[2], &tl[3]) >= 3)
            for (int k = 0; k < 4; k++) gpu::g_test_light[k] = tl[k];
        // tests: the entities of the player's own kind (its vtable): soldiers, allies and enemies; each one's place
        // (+0x5C, as the player's)
        if ((line == "actors" || line == "actors near") && cod::PlayerEntity()) {
            const bool near_mode = line == "actors near";  // any object (a vtable in the image's data) placed near
            uint32_t player = cod::PlayerEntity(), vt = kernel::R32(player);
            uint8_t* base = static_cast<uint8_t*>(kernel::HostPtr(0));
            float me[3];
            for (int i = 0; i < 3; i++) { uint32_t v = kernel::R32(player + 0x5C + uint32_t(i) * 4); std::memcpy(&me[i], &v, 4); }
            int found = 0;
            for (uint32_t va = 0x10000; va < 0x4000000 && found < 64;) {
                MEMORY_BASIC_INFORMATION mi{};
                if (!VirtualQuery(base + va, &mi, sizeof(mi))) break;
                uint32_t end = uint32_t(std::min<uint64_t>(0x4000000, uint64_t(static_cast<uint8_t*>(mi.BaseAddress) - base) + mi.RegionSize));
                bool rw = mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
                for (uint32_t a = va; rw && a + 0x70 <= end && found < 64; a += 16) {
                    uint32_t avt = kernel::R32(a);
                    // (the player's kind, the soldiers' (0x2E7000, allies and enemies), or near: anything placed)
                    if (avt != vt && avt != 0x2E7000 && !(near_mode && avt >= 0x2D0000 && avt < 0x340000)) continue;
                    float pos[3];
                    for (int i = 0; i < 3; i++) { uint32_t v = kernel::R32(a + 0x5C + uint32_t(i) * 4); std::memcpy(&pos[i], &v, 4); }
                    if (!(std::fabs(pos[0]) < 1e6f && std::fabs(pos[1]) < 1e6f && std::fabs(pos[2]) < 1e6f)) continue;
                    float d = std::sqrt((pos[0] - me[0]) * (pos[0] - me[0]) + (pos[2] - me[2]) * (pos[2] - me[2]));
                    if (near_mode && (d > 1500 || std::fabs(pos[1] - me[1]) > 400)) continue;
                    LOGR("actors: %08X vt %08X%s at %.1f %.1f %.1f, %.0f away", a, avt, a == player ? " (player)" : "", pos[0], pos[1], pos[2], d);
                    found++;
                }
                va = end;
            }
            LOGR("actors: %d", found);
        }
        if (line == "perf start") gpu::g_frame_stats = 1;
        if (line.rfind("perf stop", 0) == 0) {
            std::snprintf(gpu::g_frame_stats_label, sizeof(gpu::g_frame_stats_label), "%s", line.size() > 10 ? line.c_str() + 10 : "");
            gpu::g_frame_stats = 2;
        }
        if (line == "god on" || line == "god off") cod::SetGodMode(line == "god on"), LOGR("god: %s (player %08X)", line.c_str() + 4, cod::PlayerEntity());
        if (std::sscanf(line.c_str(), "warp %f %f %f", &to[0], &to[1], &to[2]) == 3 && cod::wide::CameraPos(eye)) {
            const bool hold = line.find(" hold") != std::string::npos;
            const bool holdall = line.find(" holdall") != std::string::npos;  // every place found, not the player's alone
            g_holding = false;  // (the places are looked for where the player is now)
            std::vector<Held> places;
            uint8_t* base = static_cast<uint8_t*>(kernel::HostPtr(0));
            constexpr uint32_t kSize = 0x4000000;
            int moved = 0;
            for (uint32_t va = 0x10000; va < kSize;) {
                MEMORY_BASIC_INFORMATION mi{};
                if (!VirtualQuery(base + va, &mi, sizeof(mi))) break;
                uint32_t end = uint32_t(std::min<uint64_t>(kSize, uint64_t(static_cast<uint8_t*>(mi.BaseAddress) - base) + mi.RegionSize));
                bool rw = mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
                for (uint32_t a = va; rw && a + 12 <= end; a += 4) {
                    float* f = reinterpret_cast<float*>(base + a);
                    float dy = eye[1] - f[1];
                    if (std::fabs(f[0] - eye[0]) < 1.0f && std::fabs(f[2] - eye[2]) < 1.0f && dy > -1.0f && dy < 200.0f) {
                        LOGR("warp:   %08X %.1f %.1f %.1f", a, f[0], f[1], f[2]);
                        // held: the player's own entity alone (its other copies are drawing's and the camera's)
                        uint32_t player = cod::PlayerEntity();
                        if (holdall || (player && a >= player && a < player + 0x2000))
                            places.push_back({a, {f[0] - eye[0], f[1] - eye[1], f[2] - eye[2]}});
                        f[0] += to[0] - eye[0], f[1] += to[1] - eye[1], f[2] += to[2] - eye[2];
                        moved++;
                    }
                }
                va = end;
            }
            LOGR("warp: from %.1f %.1f %.1f to %.1f %.1f %.1f, %d places", eye[0], eye[1], eye[2], to[0], to[1], to[2], moved);
            if (hold) {
                HoldPlayer(places, to);
                for (const Held& h : places) LOGR("warp:   held %08X (player %08X + %X)", h.at, cod::PlayerEntity(), h.at - cod::PlayerEntity());
                LOGR("warp: held there, %zu places", places.size());
            }
        }
        unsigned long long tex = 0;
        if (std::sscanf(line.c_str(), "hidetex %llx", &tex) == 1)
            for (auto& slot : gpu::g_hidden_tex)
                if (!slot.load()) {
                    slot = tex;
                    break;
                }
        // a file of texture ids, one a line (tests: many hidden at once)
        if (line.rfind("hidelist ", 0) == 0)
            if (FILE* f = std::fopen(line.substr(9).c_str(), "r")) {
                size_t slot = 0;
                while (slot < std::size(gpu::g_hidden_tex) && std::fscanf(f, "%llx", &tex) == 1)
                    if (tex) gpu::g_hidden_tex[slot++] = tex;
                std::fclose(f);
            }
        if (std::sscanf(line.c_str(), "texview %d", &on) == 1) gpu::g_texview = on != 0;
        if (std::sscanf(line.c_str(), "weaponwhole %d", &on) == 1) gpu::g_weapon_whole = on;
        // tests: the prompts as for a controller in hand or the keyboard (a test's pad presses come after the check)
        if (line == "prompts pad") cod::prompts::PadUsed();
        if (line == "prompts keys") cod::prompts::KeyboardUsed();
        if (std::sscanf(line.c_str(), "hud %d", &on) == 1) gpu::g_hide_hud = on == 0, LOGR("hud: %d", on);
        if (line == "showtex")
            for (auto& slot : gpu::g_hidden_tex) slot = 0;
        if (line.rfind("memseq ", 0) == 0) MemSeq(line.substr(7));
        unsigned at = 0, words = 0;
        if (std::sscanf(line.c_str(), "memdump %x %u", &at, &words) == 2)
            for (unsigned i = 0; i < words; i += 4)
                LOGR("memdump %08X: %08X %08X %08X %08X", at + i * 4, kernel::R32(at + i * 4), kernel::R32(at + i * 4 + 4), kernel::R32(at + i * 4 + 8),
                     kernel::R32(at + i * 4 + 12));
    }
    if (line == "hurt") cod::damage::g_test = true;
    {
        float up, pitch, yaw = 0;
        float fp[3], fyaw = 0, fpitch = 0;
        if (std::sscanf(line.c_str(), "freecam at %f %f %f %f %f", &fp[0], &fp[1], &fp[2], &fyaw, &fpitch) >= 3)
            cod::wide::SetFreeCameraAt(true, fp, fyaw, fpitch), LOGR("freecam: at %.1f %.1f %.1f yaw %.1f pitch %.1f", fp[0], fp[1], fp[2], fyaw, fpitch);
        float pa[5], pb[5], dur = 0;
        if (std::sscanf(line.c_str(), "freecam path %f %f %f %f %f %f %f %f %f %f %f", &pa[0], &pa[1], &pa[2], &pa[3], &pa[4],
                        &pb[0], &pb[1], &pb[2], &pb[3], &pb[4], &dur) == 11)
            cod::wide::SetFreeCameraPath(pa, pb, dur), LOGR("freecam: path over %.1f s", dur);
        if (line == "freecam off") cod::wide::SetFreeCamera(false, 0, 0, 0), cod::wide::SetFreeCameraAt(false, nullptr, 0, 0);
        if (line == "fly on" || line == "fly off") *cod::wide::FlyEnabled() = line == "fly on";  // the Free Camera (tests)
        // the level script's checkpoint register (SetCheckPointRegister, 0x39450, keeps it at 0x3B4740): which
        // checkpoint a restart of the level starts from
        int cp = 0;
        int secs[4];
        if (std::sscanf(line.c_str(), "checkpoint %d %d %d %d %d", &cp, &secs[0], &secs[1], &secs[2], &secs[3]) == 5) cod::ForceSections(secs);
        if (std::sscanf(line.c_str(), "checkpoint %d", &cp) == 1) cod::ForceCheckpoint(cp), LOGR("checkpoint: %d asked for", cp);
        if (line == "checkpoint") LOGR("checkpoint: %u", kernel::R32(0x3B4740));
        if (line == "sections") {
            int loaded[4];
            cod::LoadedSections(loaded);
            LOGR("sections: %d %d %d %d", loaded[0], loaded[1], loaded[2], loaded[3]);
        }
        else if (std::sscanf(line.c_str(), "freecam %f %f sun", &up, &pitch) == 2 && line.find("sun") != std::string::npos)
            cod::wide::SetFreeCamera(true, up, pitch, 999);
        else if (std::sscanf(line.c_str(), "freecam %f %f %f", &up, &pitch, &yaw) >= 2) cod::wide::SetFreeCamera(true, up, pitch, yaw);
    }
    if (line == "watchdump") cod::watch::Dump();
    {
        float mx, my;
        char word[16] = {};
        if (std::sscanf(line.c_str(), "mouse %f %f %15s", &mx, &my, word) >= 2) cod::menu_mouse::TestMouse(mx, my, word[0] == 'c');
    }
    float yaw, pitch;
    if (line == "angles" && cod::wide::CameraAngles(yaw, pitch)) LOGR("camera: yaw %.2f pitch %.2f", yaw, pitch);
}

void Ui(double fps) {
    g_fps = fps;
    cod::ragdoll::NewFrame();
    cod::wide::Update();
    cod::wide::UpdateMaterialFrame();
    cod::prompts::Update();
    if (static const bool trace = std::getenv("XBE_CAMTRACE") != nullptr; trace) {
        float yaw, pitch;
        if (cod::wide::CameraAngles(yaw, pitch)) LOGR("camtrace %.4f %.4f", yaw, pitch);
    }
    pc_input::SetCapture(!cod::wide::FrontEnd());
    // the television around the 4:3 screens: the front end, the loading screens and the movies, not the pause screen
    gpu::g_tv_frame = cod::Get().tv_frame && cod::Get().widescreen && (cod::wide::FrontEnd() || cod::InMovie());
    gpu::g_tv_movies = cod::Get().tv_frame && cod::Get().widescreen;
    cod::menu_mouse::Update();
    cod::overlay::Draw(fps);
    cod::menu_mouse::Draw();
}

}  // namespace

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Finest Hour Remastered",
    .short_title = L"Finest Hour Remastered",
    .name = L"CODFH",
    .log = "codfh",
    .settings = "codfh.toml",
    .splash = L"splash",
    .missing = L"Put Call of Duty: Finest Hour in the assets folder: its disc image (.iso), or default.xbe with its "
               L"folders (AUDIO, SHELL, SB_AGD, ...) and COD.PAK beside it.",
    .images = {{"default.xbe", "CODFH"}},
    .options =
        {
            {"skip_intros", &cod::Get().skip_intros, nullptr},
            {"developer", &cod::Get().developer, nullptr},
            {"widescreen", &cod::Get().widescreen, nullptr},
            {"tv_frame", &cod::Get().tv_frame, nullptr},
            {"fov", nullptr, &cod::Get().fov},
            {"fps_counter", &cod::Get().fps_counter, nullptr},
            {"keyboard_mouse", &cod::Get().keyboard_mouse, nullptr},
            {"mouse_sensitivity", nullptr, &cod::Get().mouse_sensitivity},
            {"invert_mouse", &cod::Get().invert_mouse, nullptr},
            {"button_prompts", nullptr, &cod::Get().button_prompts},
            {"pad_layout", nullptr, &cod::Get().pad_layout},
            {"stick_deadzone", nullptr, &cod::Get().stick_deadzone},
            {"stick_sensitivity", nullptr, &cod::Get().stick_sensitivity},
            {"stick_outer_boost", nullptr, &cod::Get().stick_outer_boost},
            {"stick_sensitivity_x", nullptr, &cod::Get().stick_sens_x},
            {"stick_sensitivity_y", nullptr, &cod::Get().stick_sens_y},
            {"invert_stick_y", &cod::Get().invert_stick_y, nullptr},
            {"look_acceleration", &cod::Get().look_acceleration, nullptr},
        },
    .load = pc_input::Load,
    .save = pc_input::Save,
    .init =
        [] {
            cod::wide::Update();
            cod::offline::Init();
            control::SetCommandHandler(Command);
        },
    .loaded = nullptr,
    .window =
        [](void* hwnd) {
            cod::overlay::Init();
            pc_input::Window(hwnd);
        },
    .message = Message,
    .ui = Ui,
    .pad = Pad,
    .rumble = nullptr,
};
