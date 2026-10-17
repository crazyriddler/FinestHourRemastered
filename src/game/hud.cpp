// 2D on a widened screen (widescreen.cpp). The menus and the HUD are RenderWare Maestro scenes, drawn through Rt2d
// with the screen's camera, which would stretch them across the wider screen. Their draws are tagged
// (gpu::State::layout2d) and placed by Layout: the front end's screens keep their 4:3 shape in the middle
// (backgrounds included, pillarboxed); in game each HUD element keeps its shape and its distance to the nearest
// edge (left third to the left edge, right third to the right, the rest centred), and overlays as wide as the screen
// fill it.
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "common/cod.h"
#include "common/widescreen.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// kBeside: a sprite beside Rt2d text; kInText: a button's picture in a line of it (a hint's "hold [LT] and ...")
enum Tag : uint32_t { kNone = 0, kHud = 1, kMenu = 2, kPixels = 0x100, kBeside = 0x200, kInText = 0x400 };

std::atomic<bool> g_front_end{false};
std::atomic<bool> g_paused{false};  // the pause screen is up: a menu over the game
int g_depth = 0;  // Rt2d renders nest

// Where an element goes. The lines of a paragraph go with its first one (same left edge). Elements side by side on a
// row (the compass and the stance, the ammunition's numbers and pictures) go together: the rows of the frame before,
// joined where they overlap in height and lie close, decide by their whole extent.
struct Box {
    float x0, y0, x1, y1;
};
struct Placed {
    Box b;
    int side;  // -1 left, 0 centre, 1 right
};
std::vector<Placed> g_placed;  // this frame's
std::vector<Box> g_seen, g_rows;  // this frame's boxes, the rows of the frame before
uint64_t g_frame = ~0ull;
constexpr float kRowGap = 64;  // console pixels between elements of a row

constexpr float kColumnGap = 24;  // between elements stacked in a column
constexpr float kGroupWidth = 128;  // wider elements (a line of text, the compass) stand alone
constexpr float kCentreReach = 72;  // console pixels from the screen's centre: the crosshair's place

// side by side on a row, or stacked in a column
bool SameRow(const Box& a, const Box& b) {
    float y_overlap = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
    float x_overlap = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
    float least_h = std::min(a.y1 - a.y0, b.y1 - b.y0);
    return (y_overlap >= least_h * 0.5f && -x_overlap < kRowGap) || (x_overlap > 0 && -y_overlap < kColumnGap);
}

void NewFrame(uint64_t frame) {
    g_frame = frame;
    g_placed.clear();
    // the rows: small boxes merged while any two touch
    g_rows.clear();
    for (const Box& b : g_seen)
        if (b.x1 - b.x0 <= kGroupWidth) g_rows.push_back(b);
    g_seen.clear();
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < g_rows.size() && !merged; i++)
            for (size_t j = i + 1; j < g_rows.size(); j++)
                if (SameRow(g_rows[i], g_rows[j])) {
                    Box& a = g_rows[i];
                    const Box& b = g_rows[j];
                    a = {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
                    g_rows.erase(g_rows.begin() + std::ptrdiff_t(j));
                    merged = true;
                    break;
                }
    }
}

int Side(float x0, float x1, float w) {
    float cx = (x0 + x1) * 0.5f;
    return cx < w / 3 ? -1 : cx > w * 2 / 3 ? 1 : 0;
}

bool Layout(const gpu::Draw2D& d_in, float& scale, float& offset) {
    gpu::Draw2D d = d_in;
    float w = float(d.gw), k = 1.0f / d.widen;
    float centre = w * 0.5f * (1 - k);
    scale = k;
    // Rt2d in the game (the HUD's text, the pause screen) is drawn through the widened camera: its x arrives already
    // narrowed by the widening, about the screen's left. Placed from its true box, it keeps the rest of its scale.
    const bool narrowed = d.bounded && !(d.tag & kPixels) && !g_front_end && d.widen > 1.0f;
    if (narrowed) {
        // the names over the squad's heads (their own green, fading with distance) are where the game projected
        // them on the widened screen: undone, they are in place
        if ((d.color & 0xFFFFFF) == 0x00FF3F) {
            // but the name of the friend the crosshair is on, over its right arm, goes with the crosshair, as at 4:3:
            // from 344.5 across, its letters as wide. It arrives at 357.3 / widen - 25, its letters narrowed by
            // 1 / 1.336 (measured at 16:9 and 21:9: the game sizes it against the widened view)
            float cy = (d.y0 + d.y1) * 0.5f;
            if (std::fabs(cy - 228.5f) < 12.0f && std::fabs(d.x0 - (357.3f / d.widen - 25.0f)) < 10.0f) {
                scale = 1.336f;
                offset = 344.5f * k + centre - d.x0 * scale;
                return true;
            }
            scale = d.widen;
            offset = 0;
            return true;
        }
        d.x0 *= d.widen, d.x1 *= d.widen;
        scale = 1;
    }
    // the pause screen's key and button pictures, beside its narrowed text: square (they arrive at their 4:3 width,
    // the screen's units square), their right edge where the narrowed placement puts it, clear of their word
    if ((d.tag & kBeside) && (d.tag & 0xFF) == kMenu && g_paused && !g_front_end && d.widen > 1.0f && d.bounded) {
        float cx = (d.x0 + d.x1) * 0.5f;
        float narrowed = k * k;
        float right = d.x1 * narrowed + centre + cx * (1 - narrowed);
        scale = std::min(1.0f, (d.y1 - d.y0) / std::max(d.x1 - d.x0, 1.0f));
        offset = right - d.x1 * scale;
        return true;
    }
    // a button's picture in a hint's line in the game: the game places it in the line's gap as it measured the line,
    // narrowed with it, but draws it in the screen's pixels. Its centre goes where the line's narrowing undone puts
    // it (the line placed as its side places text), its own width kept
    if ((d.tag & kInText) && (d.tag & 0xFF) == kHud && !g_paused && !g_front_end && d.widen > 1.0f && d.bounded) {
        float cs = (d.x0 + d.x1) * 0.5f, ct = cs * d.widen;
        int side = Side(ct, ct, w);
        float line = side < 0 ? 0 : side > 0 ? w * (1 - k) : centre;
        scale = k;
        offset = line + cs * (1 - k);
        return true;
    }
    if ((d.tag & 0xFF) == kMenu) {
        offset = centre;
        return true;
    }
    // a screen wide overlay (fades, flashes, letterbox bars): stretched to the edges
    if (!d.bounded || d.x1 - d.x0 >= w * 0.9f) {
        if (!narrowed) return false;
        scale = d.widen, offset = 0;  // its narrowing undone
        return true;
    }
    if (d.frame != g_frame) NewFrame(d.frame);
    Box b{d.x0, d.y0, d.x1, d.y1};
    // the crosshair's pieces, and what the game shows under it (a friend's name), stay in the middle on their own:
    // they spread as the player moves and would join the rows beside them
    float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
    if (std::fabs(cx - w * 0.5f) < kCentreReach && std::fabs(cy - float(d.gh) * 0.5f) < kCentreReach) {
        offset = centre;
        return true;
    }
    if (g_seen.size() < 512) g_seen.push_back(b);
    int side = 2;
    for (const Placed& p : g_placed)
        if (std::fabs(p.b.x0 - b.x0) < 1.5f && b.y0 < p.b.y1 + 40 && b.y1 > p.b.y0 - 40) side = p.side;
    if (side == 2) {
        float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
        for (const Box& r : g_rows)
            if (b.x1 - b.x0 <= kGroupWidth && cx >= r.x0 - 2 && cx <= r.x1 + 2 && cy >= r.y0 - 2 && cy <= r.y1 + 2) {
                side = Side(r.x0, r.x1, w);
                break;
            }
    }
    if (side == 2) side = Side(b.x0, b.x1, w);
    if (g_placed.size() < 512) g_placed.push_back({b, side});
    offset = side < 0 ? 0 : side > 0 ? w * (1 - k) : centre;
    return true;
}

struct Install {
    Install() { gpu::g_layout_2d = Layout; }
} g_install;

uint32_t g_tag = kNone;  // the tag set

void Tag2D(uint32_t tag) {
    std::lock_guard lock(d3d::Lock());
    g_tag = tag;
    d3d::State().layout2d = tag;
}

// Screens of their own, drawn 4:3 in the middle: the front end (title screen to the start of a level), movies and
// the loading screens with their briefings
int g_screens = 0;

void EnterScreen() {
    if (g_screens++ == 0) {
        cod::wide::SetFrontEnd(true);
        g_front_end = true;
        Tag2D(kMenu);
    }
}

void LeaveScreen() {
    if (--g_screens == 0) {
        cod::wide::SetFrontEnd(false);
        g_front_end = false;
        Tag2D(kNone);
    }
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_000A5D20);
X86_FUNC(sub_000A5D20) {  // the front end
    EnterScreen();
    __imp__sub_000A5D20(ctx, base);
    LeaveScreen();
}

X86_EXTERN_FUNC(__imp__sub_000A3E10);
X86_FUNC(sub_000A3E10) {  // a level's loading screen
    EnterScreen();
    __imp__sub_000A3E10(ctx, base);
    LeaveScreen();
}

// the loading screen's progress bar, drawn from the loading thread outside the screen's own render: in the middle
// with the screen it fills (stretched with the widened screen, it ran past its frame)
X86_EXTERN_FUNC(__imp__sub_000B30A0);
X86_FUNC(sub_000B30A0) {
    uint32_t outer = g_tag;
    Tag2D(kMenu | kPixels);
    __imp__sub_000B30A0(ctx, base);
    Tag2D(outer);
}

X86_EXTERN_FUNC(__imp__sub_000B2000);
std::atomic<bool> g_movie{false};
void PlayMovie(X86Context& ctx, uint8_t* base) {  // intro.cpp's hook plays it through here
    EnterScreen();
    g_movie = true;
    __imp__sub_000B2000(ctx, base);
    g_movie = false;
    LeaveScreen();
}

namespace cod {
bool InMovie() { return g_movie; }
bool Paused() { return g_paused; }
}  // namespace cod

X86_EXTERN_FUNC(__imp__sub_000A7B20);
X86_FUNC(sub_000A7B20) {  // the HUD's pause screen shown
    g_paused = true;
    __imp__sub_000A7B20(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_000A7B40);
X86_FUNC(sub_000A7B40) {  // and hidden
    g_paused = false;
    __imp__sub_000A7B40(ctx, base);
}

// sprites the game places in the console's screen pixels and draws through a vertex program: the pad's buttons
// beside their text in prompts, and the HUD's pictures (compass, stance, health, ammunition)
void Sprites(X86Context& ctx, uint8_t* base, X86Func* draw, uint32_t extra = kBeside) {
    uint32_t outer = g_tag;
    Tag2D((g_front_end || g_paused ? kMenu : kHud) | kPixels | extra);
    draw(ctx, base);
    Tag2D(outer);
}

X86_EXTERN_FUNC(__imp__sub_000A68D0);
X86_FUNC(sub_000A68D0) { Sprites(ctx, base, __imp__sub_000A68D0, kBeside | kInText); }  // a picture in a line of text

X86_EXTERN_FUNC(__imp__sub_000A84E0);
X86_FUNC(sub_000A84E0) { Sprites(ctx, base, __imp__sub_000A84E0); }

// a framed panel in screen pixels (the pause screen's box): stretched with the widened screen untagged
X86_EXTERN_FUNC(__imp__sub_0011D380);
X86_FUNC(sub_0011D380) { Sprites(ctx, base, __imp__sub_0011D380, 0); }

// Rt2d's object render, which every Maestro scene draws through
X86_EXTERN_FUNC(__imp__sub_001CC3A0);
X86_FUNC(sub_001CC3A0) {
    uint32_t outside = g_front_end ? kMenu : kNone;
    if (g_depth++ == 0) Tag2D(g_front_end || g_paused ? kMenu : kHud);
    __imp__sub_001CC3A0(ctx, base);
    if (--g_depth == 0) Tag2D(outside);
}
