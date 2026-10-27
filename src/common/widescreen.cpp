// See widescreen.h.
#include "common/widescreen.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "app/app.h"
#include "common/cod.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/shadows.h"
#include "kernel/kernel.h"

namespace cod::wide {

namespace {
std::atomic<float> g_widest{0.0f};  // the widest view the game asked for: its unzoomed field of view
std::atomic<bool> g_front_end{false};
std::atomic<uint32_t> g_camera{0};
}

float Aspect() {
    app::Mode m = app::Output();
    if (!m.width || !m.height) return 4.0f / 3.0f;
    return std::max(float(m.width) / float(m.height), 4.0f / 3.0f);
}

// the screen widened whenever the output is wider than 4:3 (no setting: at 4:3 the game's own 4:3 screen)
float Widen() { return Aspect() / (4.0f / 3.0f); }

// the camera at the player's eye: play, not a cutscene's camera (nor a mounted gun's or a vehicle's placed away)
bool PlayerView() {
    uint32_t player = PlayerEntity();
    float eye[3];
    if (!player || !CameraPos(eye)) return false;
    float d2 = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t v = kernel::R32(player + 0x5C + uint32_t(i) * 4);  // its feet
        float f;
        std::memcpy(&f, &v, 4);
        float dd = eye[i] - f - (i == 1 ? 60.5f : 0.0f);
        d2 += dd * dd;
    }
    return d2 < 150.0f * 150.0f;
}

// Field of View: the game's own unless set, and in play alone (a cutscene keeps the game's: framed for it, wider it
// showed the scene's unfinished edges); zoomed views (the iron sights, a scope) widened by as much. The weapon in
// hand keeps the game's (gpu::g_weapon_scale: it was modelled to its edges for it)
float FovScale(float x) {
    float widest = g_widest.load();
    if (x > widest) g_widest = widest = x;
    float k = 1.0f;
    if (Get().fov && widest > 0 && !FrontEnd() && PlayerView()) {
        constexpr float kHalfDegree = 3.14159265f / 360.0f;
        k = std::tan(float(Get().fov) * kHalfDegree) / widest;
    }
    gpu::g_weapon_scale = k;
    return k;
}

void Update() { gpu::g_screen_widen = Widen(); }

void SetFrontEnd(bool on) { g_front_end = on; }
bool FrontEnd() { return g_front_end; }

void SetCamera(uint32_t camera) { g_camera = camera; }

// the screen's camera and its frame, if the camera is still one (RenderWare's camera type, 4, its frame within the
// console's memory): the game frees it as a level is left, and frames drawn on meanwhile (the leaving's loading
// screens) read it no more. Without this its freed block, used again for something else, gave a wild address to read:
// a crash at the end of a level, after the game had saved it
bool LiveCamera(uint32_t& camera, uint32_t& frame) {
    constexpr uint32_t kRam = 0x08000000;
    camera = g_camera;
    if (!camera || camera >= kRam || kernel::G<uint8_t>(camera) != 4) return false;
    frame = kernel::R32(camera + 4);
    return frame && frame < kRam;
}
uint32_t Camera() { return g_camera; }

namespace {
std::atomic<bool> g_free{false};
std::atomic<float> g_free_up{0}, g_free_pitch{0}, g_free_yaw{0};
}  // namespace

void SetFreeCamera(bool on, float up, float pitch, float yaw) {
    g_free_up = up, g_free_pitch = pitch, g_free_yaw = yaw;
    g_free = on;
}

bool FreeCamera(float& up, float& pitch, float& yaw) {
    up = g_free_up, pitch = g_free_pitch, yaw = g_free_yaw;
    return g_free;
}

namespace {
std::atomic<bool> g_at{false};
std::atomic<float> g_at_x{0}, g_at_y{0}, g_at_z{0}, g_at_yaw{0}, g_at_pitch{0};
// the path (SetFreeCameraPath): its ends (x y z yaw pitch), its length, its start; under the lock
std::mutex g_path_lock;
float g_path_a[5], g_path_b[5], g_path_len = 0;
std::chrono::steady_clock::time_point g_path_t0;
bool g_path = false;
}  // namespace

void SetFreeCameraPath(const float from[5], const float to[5], float seconds) {
    std::lock_guard l(g_path_lock);
    for (int i = 0; i < 5; i++) g_path_a[i] = from[i], g_path_b[i] = to[i];
    // (the yaw the short way round)
    while (g_path_b[3] - g_path_a[3] > 180) g_path_b[3] -= 360;
    while (g_path_b[3] - g_path_a[3] < -180) g_path_b[3] += 360;
    g_path_len = std::max(seconds, 0.01f);
    g_path_t0 = std::chrono::steady_clock::now();
    g_path = true;
    g_at = true;
}

void SetFreeCameraAt(bool on, const float pos[3], float yaw, float pitch) {
    {
        std::lock_guard l(g_path_lock);
        g_path = false;
    }
    if (pos) g_at_x = pos[0], g_at_y = pos[1], g_at_z = pos[2];
    g_at_yaw = yaw, g_at_pitch = pitch;
    g_at = on;
}

bool FreeCameraAt(float pos[3], float& yaw, float& pitch) {
    {
        std::lock_guard l(g_path_lock);
        if (g_path && g_at) {
            float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_path_t0).count() / g_path_len;
            t = std::clamp(t, 0.0f, 1.0f);
            float e = t * t * (3 - 2 * t);  // eased in and out
            float v[5];
            for (int i = 0; i < 5; i++) v[i] = g_path_a[i] + (g_path_b[i] - g_path_a[i]) * e;
            pos[0] = v[0], pos[1] = v[1], pos[2] = v[2], yaw = v[3], pitch = v[4];
            return true;
        }
    }
    pos[0] = g_at_x, pos[1] = g_at_y, pos[2] = g_at_z;
    yaw = g_at_yaw, pitch = g_at_pitch;
    return g_at;
}

namespace {
std::atomic<bool> g_fly_on{false};    // F11 with the Free Camera cheat on
std::atomic<bool> g_fly_test{false};  // tests and the testers' flight: flown whatever the setting
std::atomic<bool> g_fly{false};
float g_fly_pos[3] = {}, g_fly_yaw = 0, g_fly_pitch = 0;
std::chrono::steady_clock::time_point g_fly_last;
}  // namespace

bool* FlyEnabled() { return &cod::Get().free_camera; }
void FlyToggle() {
    if (cod::Get().free_camera) g_fly_on = !g_fly_on;
}
void FlyForce(bool on) { g_fly_test = on; }
bool FlyActive() { return g_fly; }

void FlyLook(int counts_x, int counts_y, float sensitivity) {
    if (!g_fly) return;
    g_fly_yaw -= float(counts_x) * 0.05f * sensitivity;  // (the heading grows to the left)
    g_fly_pitch = std::clamp(g_fly_pitch - float(counts_y) * 0.05f * sensitivity, -89.0f, 89.0f);
}

void FlyAt(const float pos[3], float yaw, float pitch) {
    std::memcpy(g_fly_pos, pos, sizeof(g_fly_pos));
    g_fly_yaw = yaw, g_fly_pitch = pitch;
}

void FlyUpdate(input::Pad& pad) {
    auto now = std::chrono::steady_clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - g_fly_last).count(), 0.0f, 0.1f);
    g_fly_last = now;
    if (!cod::Get().free_camera) g_fly_on = false;
    bool want = (g_fly_on || g_fly_test) && !g_front_end;
    if (!want) {
        if (g_fly) SetFreeCameraAt(false, nullptr, 0, 0), g_fly = false;
        return;
    }
    if (!g_fly) {
        // from where the camera is, looking where it looks
        if (!CameraPos(g_fly_pos) || !CameraAngles(g_fly_yaw, g_fly_pitch)) return;
        g_fly = true;
        dt = 0;
    }
    auto axis = [](int16_t v) {
        float f = float(v) / 32767.0f;
        return std::fabs(f) < 0.15f ? 0.0f : f;
    };
    // the right stick turns at up to 120 degrees a second
    g_fly_yaw -= axis(pad.rx) * 120.0f * dt;
    g_fly_pitch = std::clamp(g_fly_pitch + axis(pad.ry) * 90.0f * dt, -89.0f, 89.0f);
    constexpr float kRad = 3.14159265f / 180.0f;
    float yr = g_fly_yaw * kRad, pr = g_fly_pitch * kRad;
    float fwd[3] = {std::sin(yr) * std::cos(pr), std::sin(pr), std::cos(yr) * std::cos(pr)};
    float right[3] = {-std::cos(yr), 0, std::sin(yr)};  // the screen's right (measured)
    // about 18 m a second (three times the 6 it was), four times that with fire held; jump up, crouch down (Y, B)
    float speed = 720.0f * (pad.analog[7] > 64 ? 4.0f : 1.0f) * dt;
    float f = axis(pad.ly), r = axis(pad.lx), u = (pad.analog[3] > 64 ? 1.0f : 0.0f) - (pad.analog[1] > 64 ? 1.0f : 0.0f);
    for (int i = 0; i < 3; i++) g_fly_pos[i] += (fwd[i] * f + right[i] * r) * speed + (i == 1 ? u * speed : 0.0f);
    SetFreeCameraAt(true, g_fly_pos, g_fly_yaw, g_fly_pitch);
    static const bool fly_log = std::getenv("XBE_FLY_LOG") != nullptr;
    static float since = 0;
    if (fly_log && (since += dt) > 0.5f) {
        since = 0;
        LOGR("fly: %.1f %.1f %.1f yaw %.1f pitch %.1f", g_fly_pos[0], g_fly_pos[1], g_fly_pos[2], g_fly_yaw, g_fly_pitch);
    }
    // the game: a still controller (start and back kept, for the pause screen)
    bool connected = pad.connected;
    uint16_t keep = pad.buttons & 0x30;
    pad = input::Pad{};
    pad.connected = connected, pad.buttons = keep;
}

void SetViewWindow(float x, float y) {
    std::lock_guard lock(gpu::g_material_frame_lock);
    gpu::g_material_frame.view_x = x;
    gpu::g_material_frame.view_y = y;
}

// The materials' light, steady over the level: the main light the shadows find follows what is in view (a room's
// models lit one way, the street's another), and the surfaces' relief and sheen turned with it as the camera only
// tilted. Each direction met is timed; the one seen longest is used, another only once it was seen twice as long.
void StableSun(float sun[3]) {
    struct Seen {
        float dir[3];
        double seconds;
    };
    static std::vector<Seen> seen;
    static int level = -2;
    static size_t used = 0;
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double dt = std::clamp(std::chrono::duration<double>(now - last).count(), 0.0, 0.1);
    last = now;
    if (gpu::g_level.load() != level) level = gpu::g_level.load(), seen.clear(), used = 0;
    size_t at = seen.size();
    for (size_t i = 0; i < seen.size(); i++)
        if (seen[i].dir[0] * sun[0] + seen[i].dir[1] * sun[1] + seen[i].dir[2] * sun[2] > 0.966f) {  // within 15 degrees
            at = i;
            break;
        }
    if (at == seen.size()) {
        if (seen.size() >= 16) return;
        seen.push_back({{sun[0], sun[1], sun[2]}, 0});
    }
    seen[at].seconds += dt;
    if (used >= seen.size() || (at != used && seen[at].seconds > 2 * seen[used].seconds + 1)) used = at;
    std::memcpy(sun, seen[used].dir, sizeof(float) * 3);
}

void UpdateMaterialFrame() {
    uint32_t c = 0, frame = 0;
    if (!LiveCamera(c, frame)) return;
    // the camera's world matrix (RwFrame ltm at 0x50): right, up, at; the world is y up
    float m[3][3];
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) {
            uint32_t v = kernel::R32(frame + 0x50 + uint32_t(r) * 16 + uint32_t(k) * 4);
            std::memcpy(&m[r][k], &v, 4);
        }
    // the sun: the scene's main light as the shadows found it, else high, from the side, as the levels' baked light
    // mostly falls
    float sun[3] = {0.40f, 0.82f, 0.41f};
    if (gpu::shadow::Lights().Sun(sun)) StableSun(sun);
    float l[3];
    for (int r = 0; r < 3; r++) l[r] = m[r][0] * sun[0] + m[r][1] * sun[1] + m[r][2] * sun[2];
    float n = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (n < 1e-4f) return;
    // RwCamera: near and far planes at 0x80 and 0x84
    float planes[2];
    for (int k = 0; k < 2; k++) {
        uint32_t v = kernel::R32(c + 0x80 + uint32_t(k) * 4);
        std::memcpy(&planes[k], &v, 4);
    }
    static int logged = 0;
    static const bool far_log = std::getenv("XBE_FAR_LOG") != nullptr;
    if (++logged % (far_log ? 300 : 30000) == 0) {
        float pos[3];
        for (int k = 0; k < 3; k++) {
            uint32_t v = kernel::R32(frame + 0x80 + uint32_t(k) * 4);
            std::memcpy(&pos[k], &v, 4);
        }
        LOGR("materials: camera near %.3f far %.3f at %.2f %.2f %.2f", planes[0], planes[1], pos[0], pos[1], pos[2]);
    }
    std::lock_guard lock(gpu::g_material_frame_lock);
    for (int r = 0; r < 3; r++) gpu::g_material_frame.light[r] = l[r] / n;
    for (int r = 0; r < 3; r++) gpu::g_material_frame.sun[r] = sun[r];
    gpu::g_material_frame.units = 100;  // the game's world is in centimetres (near plane 20, far 42465)
    if (planes[0] > 0 && planes[1] > planes[0]) {
        gpu::g_material_frame.near_plane = planes[0];
        gpu::g_material_frame.far_plane = planes[1];
    }
}

bool CameraAngles(float& yaw, float& pitch) {
    // the camera's frame (RwObject parent at 4), its world matrix (ltm at 0x50): right, up, at, pos
    uint32_t c = 0, frame = 0;
    if (!LiveCamera(c, frame)) return false;
    float at[3];
    for (int i = 0; i < 3; i++) {
        uint32_t v = kernel::R32(frame + 0x70 + i * 4);
        std::memcpy(&at[i], &v, 4);
    }
    float fx = at[0], fy = at[1], fz = at[2];
    yaw = std::atan2(fx, fz) * 57.29578f;
    pitch = std::atan2(fy, std::sqrt(fx * fx + fz * fz)) * 57.29578f;
    return true;
}

bool CameraPos(float pos[3]) {
    uint32_t c = 0, frame = 0;
    if (!LiveCamera(c, frame)) return false;
    for (int i = 0; i < 3; i++) {
        uint32_t v = kernel::R32(frame + 0x80 + i * 4);
        std::memcpy(&pos[i], &v, 4);
    }
    return true;
}

}  // namespace cod::wide
