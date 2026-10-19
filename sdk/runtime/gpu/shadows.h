#pragma once
// Real time shadows from the scene's main directional light (d3d12.cpp draws them).
//
// The light: the game lights its models itself, and the draws carry what it used. A lit static model's vertex
// program has one directional light in its own space (direction, colour, ambient); its world-view-projection
// matrix and the camera's give the model's world matrix, so the light's world direction. The fixed function draws
// carry D3D lights, in world space already. Each frame's lights are gathered by direction; the strongest group is
// the main light, followed smoothly (and switched only once a new one holds), faded out when the game gives none.
// A level of the pack's shadows.txt can turn shadows off, set the light's direction, or scale their strength.
//
// The shadows: cascades over the view (a sphere around each slice, its centre snapped to the shadow map's texels so
// edges stay put as the view turns), each a tile of one depth atlas.
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace gpu::shadow {

enum Source : int { kNone = 0, kFixed = 1, kStatic = 2, kOverride = 3 };
const char* SourceName(int source);

// a directional light the game gave a draw: towards the light, world space
struct Candidate {
    float dir[3];
    float color;    // luminance of its colour
    float ambient;  // luminance of the ambient the draw had with it
    int source;
};

struct Light {
    bool on = false;
    float dir[3] = {0, 1, 0};  // world, towards the light
    float direct = 0;          // the light's luminance
    float ambient = 0;
    float weight = 0;    // 0 to 1: shadows fade in and out with the light's certainty
    float strength = 1;  // the level's
    int source = kNone;
};

// the levels' settings (shadows.txt beside the pack's materials): "<level> off", "<level> dir x y z",
// "<level> strength s", "<level> auto"; # comments
struct LevelOverride {
    bool off = false;
    bool dir = false;
    float d[3] = {0, 1, 0};
    float strength = 1;
};
LevelOverride OverrideFor(int level);

class Tracker {
  public:
    void Add(const Candidate& c);  // render thread, from the frame's draws
    // once a frame, after its draws: the light for the frame's shadows
    Light Resolve(double now, int level);
    // the last light, any thread (the materials' highlights follow it)
    bool Sun(float dir[3]) const;

  private:
    struct Group {
        float sum[3];
        float weight;
        float color, ambient;
        int source;
    };
    std::vector<Group> groups_;
    int best_source_ = kNone;
    // the light followed: its direction now, the direction it heads to, a new one waiting to be taken
    float cur_[3] = {0, 1, 0}, target_[3] = {0, 1, 0}, pending_[3] = {0, 1, 0};
    double pending_since_ = -1;
    float direct_ = 0, ambient_ = 0;
    float weight_ = 0;
    int source_ = kNone;
    bool have_ = false;
    int level_ = -2;
    double last_ = -1, seen_ = -1;
    mutable std::mutex lock_;
    float sun_[3] = {0, 1, 0};
    bool sun_ok_ = false;
};
Tracker& Lights();

// The camera as the frame drew: its view to world (row vectors, v * m) and its projection's terms
struct Camera {
    float view_to_world[16];
    float p00, p11, p22, p32;
    float near_z;
};

struct Cascades {
    int count = 0;
    float far_z[4] = {};            // each slice's far end in view depth
    float world_to_light[4][16];  // to the cascade's light clip: xy -1 to 1, z 0 to 1 (row vectors)
    float texel[4] = {};            // world size of a shadow map texel
    float depth_range[4] = {};      // world depth its z 0 to 1 spans
};

struct Quality {
    int cascades;
    uint32_t tile;     // texels per cascade
    float distance;    // world units shadows reach
    float softness;    // filter radius in texels
    int taps;
};
// 0 off, 1 low to 3 high; world units per metre
Quality QualityFor(uint32_t level, float units);

// the slices' spheres and light matrices; one cascade can be left as it was (alternate frames)
void Fit(const Camera& cam, const float light_dir[3], const Quality& q, Cascades& out);

// 4x4 helpers, row major with row vectors
void Mul(const float* a, const float* b, float* o);
bool Invert(const float* m, float* o);

}  // namespace gpu::shadow
