#pragma once
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>
#include <memory>

#include "gpu/state.h"

namespace gpu {

// D3DCLEAR flags as the XDK defines them
enum : uint32_t {
    kClearTarget = 0xF0,  // ARGB channels
    kClearZ = 0x01,
    kClearStencil = 0x02,
};

class Backend {
public:
    virtual ~Backend() = default;
    // the title's back buffer size, the output window may differ
    virtual bool Init(uint32_t width, uint32_t height) = 0;
    // rects: count D3DRECTs (4 x int32), null for the whole viewport
    virtual void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    // indices: the title's 16 bit indices, null for non indexed draws, then start is the first vertex
    virtual void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) = 0;
    virtual void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                      uint32_t dst_points_va) = 0;
    virtual void Present(const Surface& front) = 0;
    // the CPU is about to touch this resource's memory: write GPU results back first
    virtual void Sync(uint32_t data_va, uint32_t size) = 0;
    // a lock that does not wait for the GPU (NOOVERWRITE): a result a frame or two old is what the console gives too
    virtual void SyncLate(uint32_t data_va) = 0;
    // SyncLate in two for the render thread: DeliverLate (any thread) writes the newest finished copy to guest memory,
    // false when there is none recent; RecordLate (in order with the draws) starts a copy for a later lock
    virtual bool DeliverLate(uint32_t) { return false; }
    virtual void RecordLate(uint32_t) {}
    virtual void Flush() = 0;
    virtual void SetGamma(const uint8_t* ramp /*3 x 256*/) = 0;

    // the XDK's fences and busy checks against the render thread; a backend that draws at once is never behind
    virtual uint32_t Fence() { return ++fence_; }
    virtual bool FencePending(uint32_t) { return false; }
    virtual void WaitFence(uint32_t) {}
    virtual bool Busy(uint32_t /*resource*/) { return false; }
    virtual void WaitResource(uint32_t /*resource*/) {}

private:
    uint32_t fence_ = 0;
};

std::unique_ptr<Backend> CreateBackend();
// the same backend driven from a thread of its own: calls return once recorded
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner);

// perf diagnostic: the title's thread recording for the render thread, and waiting on it
inline std::atomic<uint64_t> g_perf_capture_ns{0}, g_perf_wait_ns{0}, g_perf_waits{0};
inline std::atomic<uint64_t> g_perf_wait_kind_ns[6];  // read-back, resource, fence, flush, copy, next frame
inline std::atomic<uint64_t> g_perf_sync_late{0}, g_perf_sync_block{0};  // read-backs the title waited for, by cause

// F3: save the next frame, every draw and target, under diag::CaptureDir
inline std::atomic<bool> g_capture{false};
// frames shown so far (the host's watchdog: a game that stops showing them has stalled)
inline std::atomic<uint64_t> g_presented{0};
// trailers and stills: the game's HUD (2D tagged 1 by its layout, subtitles too) left undrawn
inline std::atomic<bool> g_hide_hud{false};
inline std::atomic<int> g_weapon_whole{0};  // tests (cmd weaponwhole 1): the weapon in hand as the game draws it
// Widescreen: the title draws its screen wider than the console's (its projection and 2D placed by the game's hooks).
// Screen sized targets are this many times wider than the render scale makes them, and the frame is shown at the
// output's aspect. 1 keeps the console's frame.
inline std::atomic<float> g_screen_widen{1.0f};
inline std::atomic<bool> g_aiming{false};  // the player aims down the sights (the game's aim held in play)
inline std::atomic<bool> g_texview{false};
// tests: every frame's time from "perf start" to "perf stop <label>" (control lines), the report in the log: average,
// lowest and highest frame rates, the 1% and 0.1% lows, the worst frames and what they did
inline std::atomic<int> g_frame_stats{0};  // 1 recording, 2 the report due
inline char g_frame_stats_label[64];  // tests: each draw in its texture's own colour (logged), to tell what is where
inline std::atomic<uint64_t> g_hidden_tex[256]{};
// the game's: textures whose draws its settings hide (the HUD's pieces), by their content ids, 0 ends the list
inline std::atomic<uint64_t> g_game_hidden_tex[32]{};
// tests: a shot's flash held steady at a place (world x y z, radius; radius 0 none): its light and shadows
inline std::atomic<float> g_test_light[4]{};  // tests: draws of these textures skipped (hidetex)
inline std::atomic<float> g_hurt{0.0f};  // the player's pain, 0 to 1: the 3D scene shakes and wavers by it

// Light from what the game draws glowing (muzzle flashes, tracers, fire, explosions, sparks): the game's table gives a
// texture's light by its content id, and d3d12.cpp puts one at each sprite drawn with it (app::Settings dynamic_lights)
struct EmitterKind {
    float color[3] = {1, 1, 1};  // its strength folded in
    float radius = 300;          // world units, for a sprite of the reference size
    float size = 0;              // the sprite size (world units) the radius is for, 0: the radius whatever the size
    float hold = 0.05f;          // seconds its light fades over once the sprite is gone
    bool flicker = false;        // a fire's
    float glow = 1;              // the sprite itself drawn this much brighter, past white: the bloom takes it
    bool light = true;           // it lights the scene (smoke and dust do not)
    float soft = 0;              // a soft particle: faded over this depth (world units) where it meets the scene
    float heat = 0;              // the air above it shimmers (0 to 1)
    float flash = 0;             // close by, the whole view flashes with it (an explosion's)
    bool shadows = false;        // a shot's muzzle flash: it casts shadows (the four nearest the eye)
    bool pure = false;           // light alone, no smoke (tracers, sparks): where the game shades it dark, see-through
    float impact = 0;            // a hit's dust or spray: where it lands on water, a ring and a splash this strong (Water Interaction)
    bool tint = false;           // its light the colour the game gives the sprite (its vertices'), its strength the colour's
};
inline std::atomic<bool (*)(uint64_t tex, EmitterKind& kind)> g_emitter{nullptr};


// Snow Tracks: the soldiers' steps as their feet are set down (game/bodies.cpp), in the world: the foot's place
// (its sole's height), the way it points (x, z), which foot (0 left, 1 right). The renderer takes them each frame.
struct Step {
    float x, y, z, dx, dz;
    int side;
};
struct Steps {
    std::mutex m;
    std::vector<Step> list;
};
inline Steps g_steps;

// tests (the control channel's "map N"): the world's floors gathered from the solid draws of the next N frames,
// then written to logs/map.txt (a line a 3 m cell: x z, the heights of its floors)
inline std::atomic<int> g_map_frames{0};

// What a character holds and wears, drawn with a world matrix of its own (a rifle, a helmet): the game moves its
// matrix as the bone it sits on, which the flinch may have moved. The game's hook gives the matrix
// as drawn (rows: right, up, at, position, 4 floats each), true when it changed it.
inline std::atomic<bool (*)(float world[16])> g_carry{nullptr};
// 2D placed by the game: a draw in screen space (pretransformed vertices) to the screen, its bounds in console pixels.
// The game's g_layout_2d says where it goes (on a widened screen, and at any width for its own reasons: the HUD's
// size), x' = x * sx + ox and y' = y * sy + oy, or false to leave it as drawn (stretched with a widened screen).
struct Draw2D {
    float x0, y0, x1, y1;
    uint32_t gw, gh;  // the screen in console pixels
    float widen;      // g_screen_widen of the targets
    uint32_t tag;     // State::layout2d of the draw, 0 for plain pretransformed vertices; bit 8 marks screen pixel positions
    bool bounded;     // the bounds are known (else the whole screen)
    uint64_t frame;   // presents so far: the draws of one frame share it
    uint32_t color;   // the first vertex's colour (ARGB), white when the draw has none
    uint64_t tex = 0; // its first stage's texture (content id, gpu/remaster.h), 0 for none
    // a quad turned on the screen (a compass's dial): its box as if unturned, about its centre, the same at any angle
    // (its bounds grow and shrink as it turns); else the bounds
    float ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0;
};
struct Place2D {
    float sx = 1, ox = 0, sy = 1, oy = 0;
};
inline std::atomic<bool (*)(const Draw2D&, Place2D&)> g_layout_2d{nullptr};
inline std::atomic<int> g_log_draws{0};
// the level's own geometry being drawn (RpWorldSectorRender, the game's src/game/matfx.cpp): its draws keep the
// game's models (the remastered meshes are for what stands in the level: soldiers, weapons, vehicles, objects)
inline std::atomic<int> g_world_sector{0};
// tests (control line "drawstack <texture name>"): guest addresses whose draws (a stage's D3D texture one of them)
// log their callers and the stages' texture transforms, each distinct caller once
inline std::array<std::atomic<uint32_t>, 16> g_stack_tex{};
// The 4:3 screens on a wide display: the picture inside an old television (the pack's ui/tv_frame.dds), set by the
// game each frame. TvScreen gives the screen's opening in window pixels while the frame shows, false otherwise.
inline std::atomic<bool> g_tv_frame{false};
inline std::atomic<bool> g_tv_movies{false};  // movies the CPU draws whole in it too, wherever they play
bool TvScreen(float rect[4]);
// The overlay's own rows that belong to the 4:3 picture (the port's menu row, its question): an ImDrawList the game's
// overlay fills each frame in a layer of kTvUiW x kTvUiH for the picture's 640 x 480, shown in the television's tube
// with the rest of the picture (its curve, scan lines and softness) on the next frame; null for none
inline constexpr uint32_t kTvUiW = 1920, kTvUiH = 1440;
inline std::atomic<void*> g_tv_ui{nullptr};
// every 2D draw to the screen, in console pixels as it was drawn (before any widescreen placement): its stage 0
// texture's content id and its first vertex's colour too (the game's menus followed by the mouse)
struct Draw2DSeen {
    float x0, y0, x1, y1;
    uint64_t tex;
    uint32_t color;
    uint32_t tag;
    uint64_t frame;
};
inline std::atomic<void (*)(const Draw2DSeen&)> g_seen_2d{nullptr};

// Textures the game replaces while it runs (button prompts for the keyboard): by content id (gpu/remaster.h), an
// RGBA image of its own size; each call replaces the whole set, an empty one restores the title's own
struct TextureOverride {
    uint64_t id = 0;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
};
void SetTextureOverrides(std::vector<TextureOverride> set);  // any thread
// A DDS of the remaster pack (its path under the pack's folder) for the overlay's drawing: an ImTextureID, 0 when
// the pack has none. Inside the overlay's frame (GameInfo::ui), loaded once.
uint64_t UiTexture(const std::string& pack_path);
// the same for a picture in memory (RGBA, w by h), kept under its name
uint64_t UiImage(const std::string& name, uint32_t w, uint32_t h, const uint8_t* rgba);
// a texture the game watches for by its content id: each draw with it counts (a screen known by its picture)
inline std::atomic<uint64_t> g_watch_texture{0};
inline std::atomic<uint32_t> g_watch_hits{0};
// Remastered materials (remaster.h): the screen camera's view window (half extents at distance 1, as drawn) and the
// main light's direction towards the light in that camera's view space (x right, y up, z ahead), set by the game
struct MaterialFrame {
    float view_x = 0.7f, view_y = 0.53f;
    float light[3] = {-0.3f, 0.8f, -0.5f};
    float sun[3] = {0.40f, 0.82f, 0.41f};  // the same light in the world (the renderer takes it to its own view)
    float near_plane = 0.1f, far_plane = 1000.0f;  // the camera's, for view depth from the depth buffer
    float units = 1.0f;                            // world units per metre: the effects' sizes
};
// a 2D draw's vertex colour replaced (a menu's own highlight hidden while a port's item has the selection): true and
// the colour (ARGB, its alpha kept from the draw's)
inline std::atomic<bool (*)(const Draw2DSeen& d, uint32_t& argb)> g_recolor_2d{nullptr};
inline std::atomic<int> g_level{-1};  // the game's current level index, for the shadows' level settings
inline std::mutex g_material_frame_lock;
inline MaterialFrame g_material_frame;  // frames left whose draws to the screen are logged (control line "draws")

}  // namespace gpu
