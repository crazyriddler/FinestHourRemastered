// Widescreen: the screen is drawn at the output's aspect with the console's height of view (Hor+). The renderer widens
// the screen sized targets (gpu::g_screen_widen) and the camera that draws to the screen sees as much more to the
// sides; 2D is placed by hud.cpp.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "common/cod.h"
#include "common/widescreen.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/shadows.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace cod::watch {
void Arm(uint32_t va, uint32_t n);
}

namespace {

float F(uint32_t va) {
    float f;
    std::memcpy(&f, HostPtr(va), 4);
    return f;
}
void SetF(uint32_t va, float f) { std::memcpy(HostPtr(va), &f, 4); }

// the screen camera and its view window's x as the game set it (4:3) and as widened
uint32_t g_narrow[1] = {0};
float g_narrow_x = 0, g_wide_x = 0;
int g_own_projection = 0;  // inside one of the game's passes that make their own projection

// The game's own passes for some models (lit static ones: buildings around a scene, props; and a second set of
// three matrices) make their projection with D3DXMatrixPerspectiveFovLH from the camera's view window x, as a 4:3
// field of view and a 4/3 aspect. Widened, that x made the models' view too tall, squashed and sliding against the
// rest as the view turned. The passes see the 4:3 x, and their 4/3 aspect becomes the screen's.
void OwnProjection(X86Context& ctx, uint8_t* base, X86Func* pass) {
    uint32_t cam = R32(0x3E25C4);
    bool swap = cam && cam == g_narrow[0] && g_narrow_x > 0 && std::fabs(F(cam + 0x68) - g_wide_x) < 1e-5f;
    if (swap) SetF(cam + 0x68, g_narrow_x);
    g_own_projection++;
    pass(ctx, base);
    g_own_projection--;
    if (swap) SetF(cam + 0x68, g_wide_x);
}

}  // namespace

// RwCameraSetViewWindow(camera, viewWindow): the half extents of the view at distance 1, x then y
X86_EXTERN_FUNC(__imp__sub_001C6A20);
X86_FUNC(sub_001C6A20) {
    uint32_t camera = R32(ctx.esp + 4), window = R32(ctx.esp + 8);
    uint32_t raster = camera ? R32(camera + 0x60) : 0;  // the camera's frame buffer
    uint32_t rw = raster ? R32(raster + 0xC) : 0, rh = raster ? R32(raster + 0x10) : 0;
    float x = F(window), y = F(window + 4);
    // the screen's camera: its frame buffer is the console's screen, and its view the console's 4:3
    bool screen = rw * 3 == rh * 4 && rw >= 320 && std::fabs(x * 0.75f - y) < 0.01f * x;
    if (screen) {
        cod::wide::SetCamera(camera);
        static uint32_t wide = mem::AllocSystem(8);
        float k = (cod::wide::FrontEnd() ? 1.0f : cod::wide::Widen()) * cod::wide::FovScale(x);
        SetF(wide, x * k);
        SetF(wide + 4, y * cod::wide::FovScale(x));
        W32(ctx.esp + 8, wide);  // the argument, the caller's own view window is left as it is
        cod::wide::SetViewWindow(x * k, y * cod::wide::FovScale(x));
        __imp__sub_001C6A20(ctx, base);
        static const char* const watch = std::getenv("XBE_WATCH");
        if (watch && !cod::wide::FrontEnd()) cod::watch::Arm(camera + uint32_t(std::strtoul(watch, nullptr, 16)), 4);
        g_narrow[0] = camera, g_narrow_x = x * cod::wide::FovScale(x), g_wide_x = x * k;
        return;
    }
    static int n = 0;
    if (n++ < 16) LOGI("widescreen: view window %.3f x %.3f of a %ux%u raster left as it is", x, y, rw, rh);
    __imp__sub_001C6A20(ctx, base);
}

// Tests: a free camera (the control line "cmd freecam <up> <pitch> <yaw>", "cmd freecam off", or XBE_FREECAM="up
// pitch yaw"): while the screen camera draws (RwCameraBeginUpdate to RwCameraEndUpdate), its frame's matrix is lifted
// by up centimetres and turned by pitch (up) and yaw (right) degrees, then put back for the game. Out of a room into
// the sky above it.
namespace {
uint8_t g_saved_ltm[128];  // the modelling and world matrices as the game had them
uint8_t g_written[128];    // as moved
uint32_t g_saved_frame = 0;

// the game's matrices back, unless it has set its own since (the camera's frame is moved for several passes a frame,
// not every one ended through RwCameraEndUpdate)
void Restore() {
    if (!g_saved_frame) return;
    if (!std::memcmp(HostPtr(g_saved_frame + 0x10), g_written, 64)) std::memcpy(HostPtr(g_saved_frame + 0x10), g_saved_ltm, 64);
    if (!std::memcmp(HostPtr(g_saved_frame + 0x50), g_written + 64, 64)) std::memcpy(HostPtr(g_saved_frame + 0x50), g_saved_ltm + 64, 64);
    g_saved_frame = 0;
}

void Turn(float* a, float* b, float angle) {  // a and b rotated in their plane
    float c = std::cos(angle), s = std::sin(angle);
    for (int k = 0; k < 3; k++) {
        float x = a[k], y = b[k];
        a[k] = x * c + y * s;
        b[k] = y * c - x * s;
    }
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_001C6720);
X86_FUNC(sub_001C6720) {  // RwCameraBeginUpdate(camera)
    static const bool env = [] {
        if (const char* e = std::getenv("XBE_FREECAM")) {
            float u = 0, p = 0, y = 0;
            if (std::sscanf(e, "%f %f %f", &u, &p, &y) >= 1) cod::wide::SetFreeCamera(true, u, p, y);
        }
        return true;
    }();
    (void)env;
    uint32_t camera = R32(ctx.esp + 4);
    float up, pitch, yaw;
    uint32_t frame = camera ? R32(camera + 4) : 0;
    Restore();
    float at_pos[3];
    const bool at_mode = cod::wide::FreeCameraAt(at_pos, yaw, pitch);
    if (frame && camera == cod::wide::Camera() && (at_mode || cod::wide::FreeCamera(up, pitch, yaw))) {
        // the frame's modelling matrix (0x10) and its world matrix (0x50, made again from the modelling one when the
        // frame is dirty): right, up, at, position, 16 bytes each; both moved alike
        std::memcpy(g_saved_ltm, HostPtr(frame + 0x10), 128);
        g_saved_frame = frame;
        constexpr float kRad = 3.14159265f / 180.0f;
        // yaw 999: towards the level's light (the sun), pitch degrees under it
        float sun[3];
        bool to_sun = !at_mode && yaw > 900 && gpu::shadow::Lights().Sun(sun);
        if (at_mode) {
            // anywhere: the heading and pitch as a direction (CameraAngles' inverse), looked along like the sun
            float yr = yaw * kRad, pr = pitch * kRad;
            sun[0] = std::sin(yr) * std::cos(pr), sun[1] = std::sin(pr), sun[2] = std::cos(yr) * std::cos(pr);
            to_sun = true, pitch = 0, up = 0;
        }
        // the sun as the sky draws it (d3d12.cpp SkyConstants): just over the horizon (not a view put anywhere, which
        // may look down)
        if (to_sun && !at_mode && sun[1] < 0.2f) {
            float hz = std::sqrt(sun[0] * sun[0] + sun[2] * sun[2]), k = std::sqrt(0.96f) / std::max(hz, 1e-6f);
            sun[0] *= k, sun[2] *= k, sun[1] = 0.2f;
        }
        for (uint32_t at : {0x10u, 0x50u}) {
            float m[4][4];
            std::memcpy(m, HostPtr(frame + at), 64);
            if (to_sun) {
                // looking at the sun, level (no roll): at towards it, right across the world's up, the frame's
                // handedness kept
                float t[3] = {sun[0], sun[1] - std::tan(pitch * kRad), sun[2]};
                float tn = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                for (float& v : t) v /= tn;
                auto cross = [](const float* a, const float* b, float* o) {
                    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
                };
                float handed[3];
                cross(m[0], m[1], handed);
                float hs = handed[0] * m[2][0] + handed[1] * m[2][1] + handed[2] * m[2][2] > 0 ? 1.0f : -1.0f;
                const float wup[3] = {0, 1, 0};
                float r[3], u[3];
                cross(wup, t, r);
                float rn = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
                for (float& v : r) v /= std::max(rn, 1e-6f);
                cross(t, r, u);
                if (u[1] < 0) for (float& v : u) v = -v;
                float h2[3];
                cross(r, u, h2);
                if ((h2[0] * t[0] + h2[1] * t[1] + h2[2] * t[2] > 0 ? 1.0f : -1.0f) != hs) for (float& v : r) v = -v;
                for (int j = 0; j < 3; j++) m[0][j] = r[j], m[1][j] = u[j], m[2][j] = t[j];
            } else {
                // the heading about the world's up, then the pitch about the camera's right
                float yr = yaw * kRad, c = std::cos(yr), s = std::sin(yr);
                for (int r = 0; r < 3; r++) {
                    float v0 = m[r][0], v2 = m[r][2];
                    m[r][0] = v0 * c - v2 * s;
                    m[r][2] = v0 * s + v2 * c;
                }
                Turn(m[2], m[1], pitch * kRad);
            }
            if (at_mode) m[3][0] = at_pos[0], m[3][1] = at_pos[1], m[3][2] = at_pos[2];
            else m[3][1] += up;
            std::memcpy(HostPtr(frame + at), m, 64);
        }
        std::memcpy(g_written, HostPtr(frame + 0x10), 128);
    }
    static int told = 0;
    static const bool freecam_log = std::getenv("XBE_FREECAM_LOG") != nullptr;
    bool tell = freecam_log && told < 12 && cod::wide::FreeCamera(up, pitch, yaw);
    float b_ltm = frame ? *(float*)HostPtr(frame + 0x84) : 0, b_mod = frame ? *(float*)HostPtr(frame + 0x44) : 0;
    uint32_t parent = frame ? R32(frame + 0xA0) : 0;
    __imp__sub_001C6720(ctx, base);
    if (tell) {
        told++;
        LOGR("freecam: cam %08X frame %08X parent %08X root %08X: ltm y %.0f -> %.0f, mod y %.0f -> %.0f, saved ltm y %.0f",
             camera, frame, parent, frame ? R32(frame + 0xAC) : 0, b_ltm, *(float*)HostPtr(frame + 0x84), b_mod,
             *(float*)HostPtr(frame + 0x44), *(float*)(g_saved_ltm + 0x74));
    }
}

X86_EXTERN_FUNC(__imp__sub_001C6710);
X86_FUNC(sub_001C6710) {  // RwCameraEndUpdate(camera)
    __imp__sub_001C6710(ctx, base);
    if (R32(ctx.esp + 4) == cod::wide::Camera()) Restore();
}

X86_EXTERN_FUNC(__imp__sub_000D9E30);
X86_FUNC(sub_000D9E30) { OwnProjection(ctx, base, __imp__sub_000D9E30); }

X86_EXTERN_FUNC(__imp__sub_0012E5E0);
X86_FUNC(sub_0012E5E0) { OwnProjection(ctx, base, __imp__sub_0012E5E0); }

// D3DXMatrixPerspectiveFovLH(out, fovy, aspect, near, far)
X86_EXTERN_FUNC(__imp__sub_00232EED);
X86_FUNC(sub_00232EED) {
    if (g_own_projection) {
        float aspect = F(ctx.esp + 12);
        float widen = cod::Get().widescreen && !cod::wide::FrontEnd() ? cod::wide::Widen() : 1.0f;
        if (std::fabs(aspect - 4.0f / 3.0f) < 1e-3f && widen > 1.0f) SetF(ctx.esp + 12, aspect * widen);
    }
    __imp__sub_00232EED(ctx, base);
}
