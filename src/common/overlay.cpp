// The PC settings, as a screen of the game's own front end: the options' paper and stone bar (from the remaster pack),
// the menus' typefaces (Jost for their Futura, Courier Prime for their typewriter), the selection strip and the button
// prompts below. Keyboard (arrows, Enter, Escape, Q and E for the pages), mouse (point, click, wheel, right click back)
// and controller (d-pad or stick, A, B, LB and RB) all work it. Opened from the main menu's PC Settings or with F10.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "core/log.h"
#include "common/cod.h"
#include "common/overlay.h"
#include "common/widescreen.h"
#include "common/overlay_text.h"
#include "common/pc_input.h"
#include "common/prompts.h"
#include "core/input.h"
#include "gpu/backend.h"
#include "gpu/remaster.h"

namespace {

std::atomic<bool> g_open{false};
ImFont* g_title = nullptr;  // Jost Bold: the menus' Futura
ImFont* g_type = nullptr;   // Courier Prime Bold: their typewriter
ImFont* g_ui = nullptr;     // Segoe UI: the FPS counter, and the menu without the pack's fonts
ImFont* g_hud = nullptr;    // DejaVu Sans Condensed: the HUD's and the pause screen's

// the pack's pictures of the options screen (content ids of the game's front end textures)
constexpr const char* kPaper = "textures/41f8455def1f2b91.c.dds";  // fend_392, the paper and its surroundings
constexpr const char* kStone = "textures/ec0414aca61f7b19.c.dds";  // fend_208, the stone bar of prompts
constexpr const char* kStrip = "textures/cb8517244f0b8da6.c.dds";  // fend_394, the selection's paper strip

// a picture of the menus' own over the panel: ImGui's DX12 backend samples with wrap, so a picture's edge took in its
// far side's texels (the stone bar's opaque foot drew a thin dark line along its clear top, under the hints); its
// edges kept half a texel inside (1024 wide, 128 to 512 tall here)
void Picture(ImDrawList* dl, uint64_t id, ImVec2 a, ImVec2 b) {
    constexpr float kU = 0.5f / 1024, kV = 0.5f / 128;
    dl->AddImage(ImTextureID(id), a, b, ImVec2(kU, kV), ImVec2(1 - kU, 1 - kV));
}

std::vector<std::pair<uint32_t, uint32_t>> Modes() {
    static std::vector<std::pair<uint32_t, uint32_t>> modes;
    if (!modes.empty()) return modes;
    std::set<std::pair<uint32_t, uint32_t>> seen;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++)
        if (dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480) seen.insert({dm.dmPelsWidth, dm.dmPelsHeight});
    modes.assign(seen.rbegin(), seen.rend());
    return modes;
}

void Saved() { app::Save(); }

// The language the game started in: the game reads it once, at boot (its text and its speech). Closing the settings
// with another one starts the game again in it.
uint32_t BootLanguage() {
    static const uint32_t boot = app::Get().language;
    return boot;
}

// the display's mode and resolution change when the settings close: not a new mode on the screen at each step
bool g_display_changed = false;

void RestartIfNeeded() {
    if (g_display_changed) {
        g_display_changed = false;
        app::ApplyDisplay();
    }
    if (app::Get().language == BootLanguage()) return;
    app::Save();
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    // the same arguments, but a fresh start (not a relaunch's hidden handover)
    std::wstring line = GetCommandLineW();
    if (size_t at = line.find(L" --relaunched"); at != std::wstring::npos) line.erase(at, 13);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring dir = app::ExeDir().wstring();
    if (!CreateProcessW(path, line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    PostMessageW(static_cast<HWND>(app::Window()), WM_CLOSE, 0, 0);
}

std::string Fmt(const char* f, int v) {
    char b[64];
    std::snprintf(b, sizeof(b), f, v);
    return b;
}

// --- the menu's model ------------------------------------------------------------------------------------------

struct Item {
    std::string label;
    std::function<std::string()> value;        // empty for an action
    std::function<void(int)> change;           // -1 or 1: the previous or next value
    std::function<void()> activate;            // Enter or A: an action, or the next value
    std::string hint;
    int binding = -1;                          // a key binding row: the action
};

struct Page {
    const char* title;
    std::vector<Item> items;
};

Item Toggle(const char* label, bool* v, const char* hint = "") {
    Item it{label, [v] { return std::string(*v ? "On" : "Off"); }, [v](int) { *v = !*v; Saved(); }, nullptr, hint};
    it.activate = [v] { *v = !*v; Saved(); };
    return it;
}

Item Choice(const char* label, std::vector<std::string> names, std::function<int()> get, std::function<void(int)> set,
            const char* hint = "") {
    auto n = int(names.size());
    Item it{label, [names, get] { int i = get(); return i >= 0 && i < int(names.size()) ? names[size_t(i)] : std::string("-"); },
            [n, get, set](int d) { set((std::max(get(), 0) + d + n) % n); Saved(); }, nullptr, hint};
    it.activate = [n, get, set] { set((std::max(get(), 0) + 1) % n); Saved(); };
    return it;
}

Item Slider(const char* label, uint32_t* v, int lo, int hi, int step, const char* fmt, const char* hint = "",
            std::function<std::string(int)> name = nullptr) {
    Item it{label,
            [v, fmt, name] { return name ? name(int(*v)) : Fmt(fmt, int(*v)); },
            [v, lo, hi, step](int d) { *v = uint32_t(std::clamp(int(*v) + d * step, lo, hi)); Saved(); }, nullptr, hint};
    return it;
}

// --- graphics presets ------------------------------------------------------------------------------------------------
// Each sets every setting of the Graphics and Effects pages (never the display's: resolution, render scale, frame
// rate). Very High is everything on; each step down drops what costs the GPU most for what it shows (measured at
// 2560x1440 on levels 1 and 7, tools/perf_ab.py --paired): High the shadows' Ultra step (~0.9 ms), smooth models
// (~0.5), the volumetric fog (~0.3), the bounce light and the flashes' shadows; Medium the shadows' High step, the
// occlusion and contact shadows, the reflections, the floating dust, the soft shadows, SMAA and the lesser touches;
// Low the shadows down to Low, the clouds with the sun and moon (their sky pass, ~1.8 ms), per-pixel lighting
// (up to ~1.3), the surface materials, the 4x MSAA and temporal anti-aliasing (~0.8, SMAA stays), the dynamic
// lights (~0.6) and the water (~0.5); Original everything off: the game as it was, its own textures included.
enum Preset { kVeryHigh, kHigh, kMedium, kLow, kOriginal, kCustom };

using SBool = bool app::Settings::*;
using SUint = uint32_t app::Settings::*;
constexpr SBool kPresetFlags[] = {
    &app::Settings::smaa, &app::Settings::taa, &app::Settings::bloom, &app::Settings::ao, &app::Settings::contact_shadows,
    &app::Settings::dynamic_lights, &app::Settings::light_halos, &app::Settings::soft_particles, &app::Settings::heat_haze,
    &app::Settings::sun_shafts, &app::Settings::aim_dof, &app::Settings::explosion_flash, &app::Settings::damage_shake,
    &app::Settings::flash_shadows, &app::Settings::character_detail, &app::Settings::weapon_materials,
    &app::Settings::sharp_distance, &app::Settings::lens_flare, &app::Settings::water, &app::Settings::water_interaction,
    &app::Settings::snow_tracks, &app::Settings::shockwaves, &app::Settings::animated_fire, &app::Settings::barrel_heat,
    &app::Settings::wind, &app::Settings::ambient_particles, &app::Settings::lit_smoke, &app::Settings::character_light,
    &app::Settings::bounce_light, &app::Settings::soft_shadows, &app::Settings::volumetric_fog,
    &app::Settings::auto_exposure, &app::Settings::color_grade, &app::Settings::film_grain, &app::Settings::hit_reactions,
    &app::Settings::reflections, &app::Settings::sun_moon, &app::Settings::clouds,
    &app::Settings::remaster, &app::Settings::materials, &app::Settings::parallax, &app::Settings::detail,
    &app::Settings::pixel_lighting, &app::Settings::height_blend};
constexpr SUint kPresetValues[] = {&app::Settings::aniso, &app::Settings::sharpening, &app::Settings::shadow_quality,
                                   &app::Settings::specular, &app::Settings::models};

// a preset's values (the settings' own defaults are Very High)
app::Settings PresetValues(int p) {
    app::Settings v;
    v.aniso = 16, v.sharpening = 0, v.shadow_quality = 3, v.specular = 100;
    app::SetModels(v, 2);  // the remastered models in every preset but Original
    for (SBool f : kPresetFlags) v.*f = true;
    if (p >= kHigh) {
        v.shadow_quality = 2;
        v.volumetric_fog = v.bounce_light = v.flash_shadows = false;
    }
    if (p >= kMedium) {
        v.shadow_quality = 2, v.aniso = 8;
        v.smaa = v.ao = v.contact_shadows = v.reflections = v.ambient_particles = v.soft_shadows = false;
        v.water_interaction = v.light_halos = v.sun_shafts = v.lit_smoke = false;
        v.character_detail = v.sharp_distance = false;
    }
    if (p >= kLow) {
        v.shadow_quality = 1, v.aniso = 4;
        v.taa = false, v.smaa = true;  // (the cheap anti-aliasing kept)
        v.clouds = v.sun_moon = v.lens_flare = v.pixel_lighting = v.materials = v.parallax = v.detail = false;
        v.height_blend = v.weapon_materials = v.dynamic_lights = v.water = v.soft_particles = v.heat_haze = false;
        v.shockwaves = v.aim_dof = v.snow_tracks = false;
    }
    if (p >= kOriginal) {
        for (SBool f : kPresetFlags) v.*f = false;
        v.shadow_quality = 0, v.aniso = 0;
        app::SetModels(v, 0);
    }
    return v;
}

}  // namespace

namespace cod::overlay {

int CurrentPreset() {
    const app::Settings& s = app::Get();
    for (int p = kVeryHigh; p <= kOriginal; p++) {
        app::Settings v = PresetValues(p);
        bool same = true;
        for (SBool f : kPresetFlags) same = same && s.*f == v.*f;
        for (SUint f : kPresetValues) same = same && s.*f == v.*f;
        if (same) return p;
    }
    return kCustom;
}

// Settings saved before the shadows' Ultra went (it cost much for little): a file on one of the presets then takes
// that preset now (Very High's shadows High, High's and Medium's Medium), others keep theirs with Ultra shadows High
void MigrateGraphics(uint32_t saved_shadows) {
    constexpr uint32_t kOldShadows[] = {4, 3, 2, 1, 0};  // Very High to Original's then
    const app::Settings& s = app::Get();
    for (int p = kVeryHigh; p <= kOriginal; p++) {
        app::Settings v = PresetValues(p);
        bool same = saved_shadows == kOldShadows[p];
        for (SBool f : kPresetFlags) same = same && s.*f == v.*f;
        for (SUint f : kPresetValues) same = same && (f == &app::Settings::shadow_quality || s.*f == v.*f);
        if (same) {
            ApplyPreset(p);
            LOGR("settings: from before the shadows' Ultra went: preset %d, shadows %u", p, app::Get().shadow_quality);
            return;
        }
    }
    app::Get().shadow_quality = std::min<uint32_t>(saved_shadows, 3);
    LOGR("settings: from before the shadows' Ultra went: custom, shadows %u", app::Get().shadow_quality);
}

void ApplyPreset(int p) {
    if (p < kVeryHigh || p > kOriginal) return;
    app::Settings& s = app::Get();
    app::Settings v = PresetValues(p);
    for (SBool f : kPresetFlags) s.*f = v.*f;
    for (SUint f : kPresetValues) s.*f = v.*f;
    app::SetModels(s, s.models);
    // the HUD with it: the original's with the game's own look, the smallest otherwise (a monitor's distance)
    cod::Get().hud_size = p == kOriginal ? 0 : 2;
}

}  // namespace cod::overlay

namespace {

using cod::overlay::ApplyPreset;
using cod::overlay::CurrentPreset;

std::vector<Page> BuildPages() {
    app::Settings& s = app::Get();
    cod::Settings& c = cod::Get();
    std::vector<Page> pages;

    Page display{"DISPLAY", {}};
    display.items.push_back(Choice("Display Mode", {"Fullscreen", "Borderless", "Windowed"},
                                   [&s] { return int(s.display_mode); },
                                   [&s](int i) { s.display_mode = app::DisplayMode(i); g_display_changed = true; },
                                   "Fullscreen changes the screen's mode; borderless covers it with a window."));
    {
        auto modes = Modes();
        std::vector<std::string> names{"Desktop"};
        for (auto [w, h] : modes) names.push_back(Fmt("%d", int(w)) + "x" + Fmt("%d", int(h)));
        display.items.push_back(Choice("Resolution", names,
                                       [&s, modes] {
                                           for (size_t i = 0; i < modes.size(); i++)
                                               if (modes[i].first == s.width && modes[i].second == s.height) return int(i) + 1;
                                           return 0;
                                       },
                                       [&s, modes](int i) {
                                           if (i == 0) s.width = s.height = 0;
                                           else s.width = modes[size_t(i - 1)].first, s.height = modes[size_t(i - 1)].second;
                                           g_display_changed = true;
                                       },
                                       "Takes effect when you leave these settings."));
    }
    display.items.push_back(Slider("Render Scale", &s.render_percent, 25, 200, 5, "%d%%",
                                   "The game is drawn at this share of the screen's resolution: above 100% supersamples."));
    // (no frame rate setting: the game runs at 60 FPS always, its speed follows the frame rate)
    display.items.push_back(Toggle("V-Sync", &s.vsync));
    display.items.push_back(Choice("Renderer", {"Direct3D 12", "Vulkan"}, [&s] { return int(s.renderer); },
                                   [&s](int i) { s.renderer = uint32_t(i); },
                                   "The graphics API (Vulkan is experimental). Takes effect at the next start."));
    display.items.push_back(Toggle("Television Frame", &c.tv_frame,
                                   "On a wide screen, the menus and movies play in a 1940s television."));
    // the field of view in play (cutscenes and the weapon in hand keep the game's: common/widescreen.cpp FovScale)
    display.items.push_back(Slider("Field of View", &c.fov, 0, 120, 5, "%d", "Horizontal at 4:3, widened further on wider screens.",
                                   [](int v) { return v ? Fmt("%d°", v) : std::string("Default"); }));
    display.items.back().change = [&c](int d) {
        int v = int(c.fov);
        v = v == 0 ? (d > 0 ? 75 : 0) : v + d * 5;
        c.fov = v < 70 ? 0 : uint32_t(std::min(v, 110));
        Saved();
    };
    display.items.push_back(Slider("Brightness", &s.brightness, 50, 200, 5, "%d%%", "100% is the original picture."));
    display.items.push_back(Toggle("FPS Counter", &c.fps_counter));
    display.items.push_back(Choice("HUD Size", {"Original", "Small", "Smaller"},
                                   [&c] { return int(std::min<uint32_t>(c.hud_size, 2)); },
                                   [&c](int i) { c.hud_size = uint32_t(i); },
                                   "The compass, the stance, the crosshair, health and ammunition smaller, each in its place."));
    display.items.push_back(Toggle("Hit Markers", &c.hud_hit_markers,
                                   "The marks when your shots hit and from where you are being hit."));
    display.items.push_back(Toggle("Compass", &c.hud_compass, "The compass and its objective star."));
    display.items.push_back(Toggle("Stance", &c.hud_stance, "Standing, crouching or lying down, beside the compass."));
    display.items.push_back(Toggle("Crosshair", &c.hud_crosshair, "The aim's crosshair (a tank's sight stays)."));
    pages.push_back(std::move(display));

    Page graphics{"GRAPHICS", {}};
    {
        // the preset: Custom when the settings are none of them (stepping goes through the five, not Custom)
        Item it{"Graphics Preset",
                [] {
                    static const char* names[] = {"Very High", "High", "Medium", "Low", "Original", "Custom"};
                    return std::string(names[CurrentPreset()]);
                },
                [](int d) {
                    int p = CurrentPreset();
                    p = p == kCustom ? (d > 0 ? kVeryHigh : kOriginal) : (p + d + 5) % 5;
                    ApplyPreset(p);
                    Saved();
                },
                nullptr,
                "Every graphics setting at once. Changing any of them makes it Custom."};
        it.activate = [] {
            int p = CurrentPreset();
            ApplyPreset(p == kCustom ? kVeryHigh : (p + 1) % 5);
            Saved();
        };
        graphics.items.push_back(std::move(it));
    }
    {
        static const uint32_t an[] = {0, 2, 4, 8, 16};
        graphics.items.push_back(Choice("Anisotropic Filter", {"Game", "2x", "4x", "8x", "16x"},
                                        [&s] {
                                            for (int i = 0; i < 5; i++)
                                                if (an[i] == s.aniso) return i;
                                            return 0;
                                        },
                                        [&s](int i) { s.aniso = an[i]; }, "Textures stay sharp at a slant."));
    }
    graphics.items.push_back(Choice("Shadows", {"Off", "Low", "Medium", "High"},
                                    [&s] { return int(std::min<uint32_t>(s.shadow_quality, 3)); },
                                    [&s](int i) { s.shadow_quality = uint32_t(i); },
                                    "Real time shadows from the scene's main light: sharper and further with each step."));
    graphics.items.push_back(Toggle("Anti-Aliasing", &s.taa, "4x MSAA and temporal anti-aliasing: smooth edges, fences and foliage."));
    graphics.items.push_back(Slider("Sharpening", &s.sharpening, 0, 10, 1, "%d", "Contrast adaptive sharpening.",
                                    [](int v) { return v ? Fmt("%d", v) : std::string("Off"); }));
    graphics.items.push_back(Toggle("Remastered Textures", &s.remaster, "Every texture redrawn at four times the detail."));
    graphics.items.push_back(Toggle("Character Detail", &s.character_detail, "Close up, skin's pores and uniforms' weave."));
    graphics.items.push_back(Toggle("Weapon Materials", &s.weapon_materials, "Your weapon's wood and metal with relief and a metal sheen."));
    graphics.items.push_back(Toggle("Sharper Distance", &s.sharp_distance, "Distant surfaces' textures a little sharper."));
    graphics.items.push_back(Toggle("Surface Materials", &s.materials, "Relief and highlights of stone, wood, metal and cloth."));
    graphics.items.push_back(Toggle("Parallax", &s.parallax, "Cobbles, bricks and rubble with depth."));
    graphics.items.push_back(Toggle("Detail Textures", &s.detail, "Fine detail on surfaces close up."));
    graphics.items.push_back(Toggle("Height Blending", &s.height_blend, "Ground layers meet along their stones and cracks."));
    graphics.items.push_back(Slider("Specular", &s.specular, 0, 200, 10, "%d%%", "The strength of the surfaces' highlights."));
    graphics.items.push_back(Toggle("Per-Pixel Lighting", &s.pixel_lighting, "Smooth light on characters, no facets."));
    graphics.items.push_back(Choice("Models", {"Original", "Smooth", "Remastered"},
                                    [&s] { return int(std::min<uint32_t>(s.models, 2)); },
                                    [&s](int i) { app::SetModels(s, uint32_t(i)); },
                                    "Remastered: soldiers, weapons, vehicles, objects and scenery remodelled, rounder and "
                                    "denser. Smooth: the game's characters and weapons rounded. Original: the game's own."));
    graphics.items.push_back(Toggle("Reflections", &s.reflections, "Metal, glass, varnished wood and stone reflect what is around them."));
    pages.push_back(std::move(graphics));

    Page effects{"EFFECTS", {}};
    effects.items.push_back(Toggle("Bloom", &s.bloom, "Bright light and flashes glow."));
    effects.items.push_back(Toggle("Ambient Occlusion", &s.ao, "Soft shade in corners and creases."));
    effects.items.push_back(Toggle("Contact Shadows", &s.contact_shadows, "Small shadows where things meet the ground."));
    effects.items.push_back(Toggle("Dynamic Lights", &s.dynamic_lights, "Muzzle flashes, tracers, fire and explosions light the scene."));
    effects.items.push_back(Toggle("Light Halos", &s.light_halos, "The air glows around flashes and fire."));
    effects.items.push_back(Toggle("Muzzle Flash Shadows", &s.flash_shadows,
                                   "Shots, fires and big lights cast shadows: eight of each, near and far."));
    effects.items.push_back(Toggle("Soft Particles", &s.soft_particles, "Smoke and dust without hard edges on walls and floors."));
    effects.items.push_back(Toggle("Heat Haze", &s.heat_haze, "The air shimmers over fires and explosions."));
    effects.items.push_back(Toggle("Sun and Moon", &s.sun_moon, "The sun by day, the moon by night, where the level's light comes from."));
    effects.items.push_back(Toggle("Clouds", &s.clouds, "Drifting clouds over the sky, lit as the level is."));
    effects.items.push_back(Toggle("Lens Flare", &s.lens_flare, "A faint flare from the sun, and dirt on the lens catching bright light."));
    effects.items.push_back(Toggle("Water", &s.water, "Rivers, puddles and sewers ripple and reflect what is above them."));
    effects.items.push_back(Toggle("Water Interaction", &s.water_interaction,
                                   "Boats, oars, swimmers, falling bodies and shots stir the water: rings, wakes and splashes."));
    effects.items.push_back(Toggle("Snow Tracks", &s.snow_tracks, "Soldiers leave footprints in the snow, which gives under their weight."));
    effects.items.push_back(Toggle("Shockwaves", &s.shockwaves, "An explosion's shock ripples through the air; close ones spatter the view with dirt."));
    effects.items.push_back(Toggle("Animated Fire", &s.animated_fire, "Fires move inside, rising and wavering, not just still pictures."));
    effects.items.push_back(Toggle("Barrel Heat", &s.barrel_heat, "Your gun's barrel shimmers after firing and smokes after a long burst."));
    effects.items.push_back(Toggle("Wind", &s.wind, "Trees, bushes and flags sway in the wind."));
    effects.items.push_back(Toggle("Ambient Particles", &s.ambient_particles, "Dust floating where light falls, embers rising over fires."));
    effects.items.push_back(Toggle("Lit Smoke", &s.lit_smoke, "Smoke and dust glow with the fire and flashes inside and near them."));
    effects.items.push_back(Toggle("Character Lighting", &s.character_light,
                                   "Softer, warmer light through characters' skin and clothes."));
    effects.items.push_back(Toggle("Bounce Light", &s.bounce_light,
                                   "Lit surfaces tint what is near them (a sunlit wall's colour on the floor); dark places stay dark."));
    effects.items.push_back(Toggle("Soft Shadows", &s.soft_shadows,
                                   "The sun's shadows sharp at their foot, softer further from what casts them."));
    effects.items.push_back(Toggle("Volumetric Fog", &s.volumetric_fog,
                                   "A light haze in the air, lit by the sun through the gaps and by the flashes."));
    effects.items.push_back(Toggle("Sun Shafts", &s.sun_shafts, "Beams of sunlight past columns and ruins."));
    effects.items.push_back(Toggle("Aim Depth of Field", &s.aim_dof, "Aiming down the sights blurs the edges and the background."));
    effects.items.push_back(Toggle("Eye Adaptation", &s.auto_exposure,
                                   "The view adapts a little between dark rooms and bright streets."));
    effects.items.push_back(Toggle("Color Grading", &s.color_grade, "Each campaign's own tones: winter, desert, Europe."));
    effects.items.push_back(Toggle("Film Grain", &s.film_grain, "A faint, fine grain."));
    effects.items.push_back(Toggle("Explosion Flash", &s.explosion_flash, "A close explosion lights up the whole view."));
    effects.items.push_back(Toggle("Hit Reactions", &s.hit_reactions, "Soldiers flinch where bullets hit them."));
    effects.items.push_back(Toggle("Damage Shake", &s.damage_shake, "Being hit shakes the view a little."));
    pages.push_back(std::move(effects));

    Page controls{"CONTROLS", {}};
    controls.items.push_back(Toggle("Keyboard & Mouse", &c.keyboard_mouse));
    controls.items.push_back(Slider("Mouse Sensitivity", &c.mouse_sensitivity, 10, 400, 10, "%d%%"));
    controls.items.push_back(Toggle("Invert Mouse", &c.invert_mouse));
    controls.items.push_back(Toggle("Toggle Aim", &c.toggle_aim,
                                    "Aim with a click of the mouse's aim button and stop with the next, or by reloading or "
                                    "throwing a grenade (iron sights and scopes); keys and the controller's trigger still aim while "
                                    "held."));
    controls.items.push_back(Choice("Button Prompts", {"Automatic", "Controller", "Keyboard"},
                                    [&c] { return int(std::min<uint32_t>(c.button_prompts, 2)); },
                                    [&c](int i) { c.button_prompts = uint32_t(i); },
                                    "Automatic follows the last device used."));
    controls.items.push_back(Choice("Controller Layout", {"Modern", "Classic"},
                                    [&c] { return int(std::min<uint32_t>(c.pad_layout, 1)); },
                                    [&c](int i) { c.pad_layout = uint32_t(i); },
                                    "Modern: A jump, B crouch, X reload, Y weapon, LB use, RB grenade, RS melee."));
    controls.items.push_back(Slider("Stick Dead Zone", &c.stick_deadzone, 0, 40, 1, "%d%%",
                                    "How far the right stick moves before the view turns."));
    controls.items.push_back(Slider("Look Sensitivity", &c.stick_sensitivity, 25, 300, 5, "%d%%"));
    controls.items.push_back(Slider("Look Horizontal", &c.stick_sens_x, 25, 200, 5, "%d%%", "The controller's turn across."));
    controls.items.push_back(Slider("Look Vertical", &c.stick_sens_y, 25, 200, 5, "%d%%", "The controller's turn up and down."));
    controls.items.push_back(Slider("Fast Turn", &c.stick_outer_boost, 100, 400, 10, "%d%%",
                                    "The turn at the stick's edge, for quick turns; the centre stays precise."));
    controls.items.push_back(Toggle("Invert Look", &c.invert_stick_y, "The controller's vertical look."));
    controls.items.push_back(Toggle("Look Acceleration", &c.look_acceleration,
                                    "The original's eased turn: it ramps up and drifts on after the stick is let go."));
    pages.push_back(std::move(controls));

    Page keys{"KEYBOARD", {}};
    for (int a = 0; a < pc_input::kActionCount; a++) {
        if (a >= pc_input::kLookUp && a <= pc_input::kLookRight) continue;  // the mouse looks
        Item it;
        it.label = pc_input::ActionLabel(pc_input::Action(a));
        it.binding = a;
        it.hint = "Enter to change, then press the new key or mouse button. Escape cancels, Delete clears.";
        keys.items.push_back(std::move(it));
    }
    Item reset{"Default Keys", nullptr, nullptr, [] { pc_input::ResetKeys(); Saved(); }, "Every key back to its default."};
    keys.items.push_back(std::move(reset));
    pages.push_back(std::move(keys));

    Page general{"GENERAL", {}};
    general.items.push_back(Toggle("Skip Intros", &c.skip_intros, "Straight to the title screen."));
    {
        static const uint32_t codes[] = {1, 4, 6, 5, 9};
        BootLanguage();
        // UTF-8, as ImGui takes text
        general.items.push_back(Choice("Language", {"English", "Fran\xC3\xA7" "ais", "Italiano", "Espa\xC3\xB1ol",
                                                    "Portugu\xC3\xAA" "s"},
                                       [&s] {
                                           for (int i = 0; i < 5; i++)
                                               if (codes[i] == s.language) return i;
                                           return 0;
                                       },
                                       [&s](int i) { s.language = codes[i]; },
                                       "Text and speech. The game starts again in it when you leave these settings."));
    }
    general.items.push_back(Choice("Sound", {"Stereo", "5.1 Surround"}, [&s] { return int(std::min<uint32_t>(s.speakers, 1)); },
                                   [&s](int i) { s.speakers = uint32_t(i); },
                                   "5.1 puts the battle around you on a surround system: front, centre, back and subwoofer. "
                                   "Takes effect at the next start."));
    pages.push_back(std::move(general));

    // the cheats (the players asked for them; all off by default; the testers' keys stay with developer = true)
    Page cheats{"CHEATS", {}};
    cheats.items.push_back(Toggle("Free Camera", cod::wide::FlyEnabled(),
                                  "F11 in play flies the camera through the level and its walls, F11 again comes back: move with "
                                  "WASD or the left stick, look with the mouse or the right stick, Jump up, Crouch down, Fire "
                                  "faster. The player stays still."));
    cheats.items.push_back(Toggle("God Mode", &c.god_mode, "You take no damage."));
    cheats.items.push_back(Toggle("Infinite Ammo", &c.infinite_ammo, "Your weapons' ammunition, grenades and mines never run out."));
    cheats.items.push_back(Toggle("No Reload", &c.no_reload, "The magazine of the weapon in your hands stays full: no reloading."));
    pages.push_back(std::move(cheats));
    return pages;
}

// --- input -----------------------------------------------------------------------------------------------------

struct Nav {
    int move = 0, change = 0, page = 0;
    bool accept = false, back = false, clear = false;
};

// the controller's presses, with a repeat for held directions
Nav PadNav() {
    Nav n;
    static input::Pad last;
    static double held_since = 0, next_repeat = 0;
    input::Pad p = input::Raw(0);
    double now = ImGui::GetTime();
    auto edge = [&](bool a, bool b) { return a && !b; };
    int dy = (p.buttons & 0x1) || p.ly > 16000 ? -1 : (p.buttons & 0x2) || p.ly < -16000 ? 1 : 0;
    int dx = (p.buttons & 0x4) || p.lx < -16000 ? -1 : (p.buttons & 0x8) || p.lx > 16000 ? 1 : 0;
    static int ldx = 0, ldy = 0;
    if (dx != ldx || dy != ldy) {
        held_since = now, next_repeat = now + 0.4;
        n.move = dy, n.change = dx;
    } else if ((dx || dy) && now >= next_repeat) {
        next_repeat = now + 0.08;
        n.move = dy, n.change = dx;
    }
    ldx = dx, ldy = dy;
    n.accept = edge(p.analog[0] > 100, last.analog[0] > 100);
    n.back = edge(p.analog[1] > 100, last.analog[1] > 100) || edge(p.buttons & 0x10, last.buttons & 0x10);
    n.clear = edge(p.analog[2] > 100, last.analog[2] > 100);
    if (edge(p.analog[5] > 100, last.analog[5] > 100)) n.page = -1;  // LB (the game's white)
    if (edge(p.analog[4] > 100, last.analog[4] > 100)) n.page = 1;   // RB (black)
    if (n.move || n.change || n.accept || n.back || n.page) cod::prompts::PadUsed();
    last = p;
    return n;
}

Nav KeyNav() {
    Nav n;
    auto k = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
    if (k(ImGuiKey_UpArrow) || k(ImGuiKey_W)) n.move = -1;
    if (k(ImGuiKey_DownArrow) || k(ImGuiKey_S)) n.move = 1;
    if (k(ImGuiKey_LeftArrow) || k(ImGuiKey_A)) n.change = -1;
    if (k(ImGuiKey_RightArrow) || k(ImGuiKey_D)) n.change = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false) || ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) n.page = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Tab, false))
        n.page = 1;
    n.accept = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
               ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    n.back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
    n.clear = ImGui::IsKeyPressed(ImGuiKey_Delete, false);
    if (n.move || n.change || n.accept || n.back || n.page) cod::prompts::KeyboardUsed();
    return n;
}

// --- drawing ---------------------------------------------------------------------------------------------------

// the 4:3 screen in the middle of the window, in the console's 640 by 480
struct Screen {
    float x0, y0, s;
    ImVec2 P(float x, float y) const { return ImVec2(x0 + x * s, y0 + y * s); }
};

constexpr ImU32 kInk = IM_COL32(52, 47, 41, 255);       // the selected row's, the title's
constexpr ImU32 kInkSoft = IM_COL32(96, 88, 78, 255);   // the other rows'
constexpr ImU32 kInkFaint = IM_COL32(120, 112, 100, 255);
constexpr ImU32 kStoneInk = IM_COL32(40, 38, 36, 255);  // the prompts' words on the stone

void Text(ImDrawList* dl, ImFont* f, float size, ImVec2 at, ImU32 col, const std::string& t, float align = 0) {
    if (!f) f = g_ui;
    ImVec2 ext = f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str());
    dl->AddText(f, size, ImVec2(std::round(at.x - ext.x * align), std::round(at.y - ext.y * 0.5f)), col, t.c_str());
}

float Width(ImFont* f, float size, const std::string& t) {
    if (!f) f = g_ui;
    return f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str()).x;
}

// a prompt's picture: the pack's (keys by their binding, the controller's buttons by its kind), a drawn cap without it
void Prompt(ImDrawList* dl, ImVec2 c, float size, int vk, const char* pad_button, const char* fallback) {
    std::string name;
    if (cod::prompts::KeyboardShown() || !pad_button) {
        char b[16];
        std::snprintf(b, sizeof(b), "kb_%02x", vk);
        name = b;
    } else {
        input::Kind k = input::ControllerKind(0);
        name = std::string("pad_") + (k == input::kPlayStation ? "ps" : k == input::kNintendo ? "nin" : "xbox") + "_" + pad_button;
    }
    uint64_t id = gpu::UiTexture("prompts/" + name + ".dds");
    ImVec2 a(c.x - size / 2, c.y - size / 2), b(c.x + size / 2, c.y + size / 2);
    if (id) {
        dl->AddImage(ImTextureID(id), a, b);
    } else {
        dl->AddRectFilled(a, b, IM_COL32(30, 30, 32, 255), size * 0.2f);
        Text(dl, g_type, size * 0.5f, c, IM_COL32(240, 240, 240, 255), fallback, 0.5f);
    }
}

int g_page = 0, g_sel = 0, g_scroll = 0;

}  // namespace

namespace cod {

bool OverlayOpen() { return g_open; }

namespace overlay {

void Init() {
    ImGuiIO& io = ImGui::GetIO();
    char path[MAX_PATH];
    GetWindowsDirectoryA(path, MAX_PATH);
    std::string segoe = std::string(path) + "/Fonts/segoeui.ttf";
    if (GetFileAttributesA(segoe.c_str()) != INVALID_FILE_ATTRIBUTES) g_ui = io.Fonts->AddFontFromFileTTF(segoe.c_str(), 36.0f);
    // the pack's typefaces, large enough for a 4K screen and filtered down below that; Latin-1 for the languages
    std::wstring dir = gpu::remaster::Dir() + L"\\ui\\";
    auto load = [&](const wchar_t* file, float px) -> ImFont* {
        std::wstring w = dir + file;
        char utf8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
        if (GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        return io.Fonts->AddFontFromFileTTF(utf8, px, &cfg, io.Fonts->GetGlyphRangesDefault());
    };
    g_title = load(L"Jost-Bold.ttf", 96.0f);
    g_type = load(L"CourierPrime-Bold.ttf", 80.0f);
    g_hud = load(L"DejaVuSansCondensed.ttf", 72.0f);
    if (!g_ui) g_ui = io.Fonts->AddFontDefault();
    // the window's pointer is ours (pc_input hides it, menu_mouse draws the game's): ImGui leaves it alone
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
}

bool g_fresh = false;  // just opened: the pointer where it rests selects nothing
bool g_held = false;   // just opened: the press that opened the settings (Enter, A, a click) does nothing in them

ImFont* MenuFont() { return g_title ? g_title : g_ui; }
ImFont* HudFont() { return g_hud ? g_hud : g_ui; }
bool PauseRequested() { return false; }

void Toggle() {
    g_open = !g_open;
    if (g_open) g_sel = 0, g_scroll = 0, g_fresh = true, g_held = true;
    else RestartIfNeeded();
}

void Draw(double fps) {
    if (cod::Get().fps_counter) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.0f FPS", fps);
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        float size = std::max(16.0f, ImGui::GetIO().DisplaySize.y / 45.0f);
        fg->AddText(g_ui, size, ImVec2(11, 9), IM_COL32(0, 0, 0, 200), text);
        fg->AddText(g_ui, size, ImVec2(10, 8), IM_COL32(255, 230, 80, 255), text);
    }
    if (!g_open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    Screen sc;
    float tv[4];
    // the television's screen; for the frames it is not drawn while the settings open or close (the frame's choice of
    // it comes before the settings say they are open), the last one, the television drawn over it as always: the
    // panel showed for a moment at the full screen's size, outside the television
    static float last_tv[4];
    static ImVec2 last_size;
    static bool have_tv = false;
    bool in_tv = gpu::TvScreen(tv);
    if (in_tv) std::memcpy(last_tv, tv, sizeof(tv)), last_size = io.DisplaySize, have_tv = true;
    else if (have_tv && cod::Get().tv_frame && cod::Get().widescreen && last_size.x == io.DisplaySize.x &&
             last_size.y == io.DisplaySize.y)
        std::memcpy(tv, last_tv, sizeof(tv)), in_tv = true;
    if (in_tv) {
        // inside the television's screen, which shows around it
        sc.s = (tv[3] - tv[1]) / 480.0f;
        sc.x0 = tv[0], sc.y0 = tv[1];
        dl->PushClipRect(ImVec2(tv[0], tv[1]), ImVec2(tv[2], tv[3]));
    } else {
        sc.s = io.DisplaySize.y / 480.0f;
        sc.x0 = (io.DisplaySize.x - 640 * sc.s) * 0.5f;
        sc.y0 = 0;
        dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 255));
    }

    // the screen: the paper, the stone bar over its foot
    // as the options screen places it (fitted to the game's own: 640 by 516 from 18 above the top)
    if (uint64_t id = gpu::UiTexture(kPaper)) Picture(dl, id, sc.P(0, -18), sc.P(640, 498));
    else dl->AddRectFilled(sc.P(84, 40), sc.P(533, 382), IM_COL32(205, 198, 182, 255));
    if (uint64_t id = gpu::UiTexture(kStone)) Picture(dl, id, sc.P(0, 352), sc.P(640, 480));
    else dl->AddRectFilled(sc.P(0, 380), sc.P(640, 480), IM_COL32(110, 110, 110, 255));

    std::vector<Page> pages = BuildPages();
    const int np = int(pages.size());
    const bool rebinding = pc_input::Rebinding() >= 0;
    Nav n = rebinding ? Nav{} : PadNav();
    Nav k = rebinding ? Nav{} : KeyNav();
    n.move += k.move, n.change += k.change, n.page += k.page;
    n.accept |= k.accept, n.back |= k.back, n.clear |= k.clear;

    // pages: the title between the page prompts
    g_page = (g_page + np) % np;
    const float title_y = 62;
    Text(dl, g_title, 26 * sc.s, sc.P(308, title_y), kInk, Tr(pages[size_t(g_page)].title), 0.5f);
    {
        float half = Width(g_title, 26 * sc.s, Tr(pages[size_t(g_page)].title)) / sc.s / 2 + 26;
        Prompt(dl, sc.P(308 - half, title_y), 20 * sc.s, 'Q', "lb", "Q");
        Prompt(dl, sc.P(308 + half, title_y), 20 * sc.s, 'E', "rb", "E");
        // the page's dots
        for (int i = 0; i < np; i++)
            dl->AddCircleFilled(sc.P(308 + (i - (np - 1) / 2.0f) * 11, title_y + 19), 2.6f * sc.s,
                                i == g_page ? kInk : IM_COL32(150, 142, 128, 255));
        // the prompts can be clicked
        if (ImGui::IsMouseClicked(0)) {
            ImVec2 m = io.MousePos;
            auto close_to = [&](float x) { ImVec2 c = sc.P(x, title_y); return std::fabs(m.x - c.x) < 12 * sc.s && std::fabs(m.y - c.y) < 12 * sc.s; };
            if (close_to(308 - half)) n.page = -1;
            if (close_to(308 + half)) n.page = 1;
        }
    }
    if (n.page) {
        g_page = (g_page + n.page + np) % np, g_sel = 0, g_scroll = 0;
    }
    Page& page = pages[size_t(g_page)];
    const int count = int(page.items.size());

    // rows
    constexpr int kRows = 9;
    constexpr float kTop = 104, kStep = 23.5f, kLabelX = 127, kValueX = 309, kAltX = 425;
    g_sel = std::clamp(g_sel + n.move, 0, count - 1);
    if (n.move) {
        if (g_sel < g_scroll) g_scroll = g_sel;
        if (g_sel >= g_scroll + kRows) g_scroll = g_sel - kRows + 1;
    }
    // the mouse: pointing selects, the wheel scrolls, a click does what Enter does (a value's arrows change it)
    static ImVec2 last_mouse(-1, -1);
    const bool opening = g_fresh;
    bool mouse_moved = !g_fresh && (io.MousePos.x != last_mouse.x || io.MousePos.y != last_mouse.y);
    last_mouse = io.MousePos;
    g_fresh = false;
    if (!rebinding && io.MouseWheel != 0) g_scroll = std::clamp(g_scroll - int(io.MouseWheel), 0, std::max(0, count - kRows));
    int hover = -1, hover_slot = 0;
    {
        ImVec2 m = io.MousePos;
        for (int r = 0; r < kRows && g_scroll + r < count; r++) {
            ImVec2 a = sc.P(110, kTop + r * kStep - kStep / 2), b = sc.P(517, kTop + r * kStep + kStep / 2);
            if (m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y) {
                hover = g_scroll + r;
                hover_slot = m.x >= sc.P(kAltX - 8, 0).x ? 1 : 0;
            }
        }
        if (hover >= 0 && mouse_moved && !rebinding) g_sel = hover, cod::prompts::KeyboardUsed();
    }
    static int slot = 0;  // a binding row's column
    Item& sel = page.items[size_t(g_sel)];
    if (sel.binding >= 0) {
        if (n.change) slot = std::clamp(slot + n.change, 0, 1);
        if (hover == g_sel && mouse_moved) slot = hover_slot;
    }
    bool clicked = !rebinding && ImGui::IsMouseClicked(0) && hover >= 0;
    if (ImGui::IsMouseClicked(1) && !rebinding) n.back = true;
    // the press that chose PC Settings in the game's menu is not one on the first row: nothing is accepted until
    // it is let go
    if (g_held) {
        bool down = ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_Space) ||
                    ImGui::IsKeyDown(ImGuiKey_KeypadEnter) || ImGui::IsMouseDown(0) || input::Raw(0).analog[0] > 100;
        if (down || opening) n.accept = false, clicked = false;
        if (!down) g_held = false;
    }

    // what the presses do
    if (!rebinding) {
        if (sel.binding >= 0) {
            if (n.accept || clicked) pc_input::StartRebind(sel.binding, slot);
            if (n.clear) pc_input::Keys().key[sel.binding][slot] = 0, Saved();
        } else {
            if (n.change && sel.change) sel.change(n.change);
            if (n.accept || clicked) {
                if (clicked && sel.change && hover == g_sel) {
                    // a click on the value's < steps back, anywhere else on the row forward
                    float back = sc.P(kValueX - 4, 0).x, label = sc.P(kValueX - 30, 0).x;
                    sel.change(io.MousePos.x >= label && io.MousePos.x < back ? -1 : 1);
                } else if (sel.activate) {
                    sel.activate();
                } else if (sel.change) {
                    sel.change(1);
                }
            }
        }
    }
    // closed by back: this frame still drawn whole, the settings let go after it (returning here, half drawn, the
    // paper and its title showed alone for a frame as the menu came back)
    const bool closing = !rebinding && n.back;
    static int last_rebind = -1;
    if (last_rebind >= 0 && !rebinding) Saved();
    last_rebind = pc_input::Rebinding();

    // the rows themselves
    const float fs = 19.5f * sc.s, vs = 17.5f * sc.s;
    for (int r = 0; r < kRows && g_scroll + r < count; r++) {
        int i = g_scroll + r;
        const Item& it = page.items[size_t(i)];
        float y = kTop + r * kStep;
        bool on = i == g_sel;
        if (on) {
            if (uint64_t id = gpu::UiTexture(kStrip)) Picture(dl, id, sc.P(110, y - 13), sc.P(517, y + 13));
            else dl->AddRectFilled(sc.P(110, y - 13), sc.P(517, y + 13), IM_COL32(170, 162, 146, 255));
        }
        ImU32 ink = on ? kInk : kInkSoft;
        {
            // in the game's language; a long one smaller, to stay clear of the value
            std::string label = Tr(it.label);
            float room = (kValueX - 22 - kLabelX) * sc.s, lw = Width(g_title, fs, label);
            Text(dl, g_title, lw > room ? fs * room / lw : fs, sc.P(kLabelX, y + 1), ink, label);
        }
        if (it.binding >= 0) {
            const pc_input::Bindings& b = pc_input::Keys();
            for (int sl = 0; sl < 2; sl++) {
                float x = sl ? kAltX : kValueX;
                std::string v = pc_input::Rebinding() == it.binding && slot == sl && on ? "..." : (app::Get().language == 1 ? pc_input::FileKeyName(b.key[it.binding][sl]) : pc_input::KeyName(b.key[it.binding][sl]));
                if (v.empty()) v = "-";
                if (v.size() > 9) v = v.substr(0, 9);
                bool cell = on && slot == sl;
                if (cell) dl->AddRect(sc.P(x - 5, y - 10), sc.P(x + 105, y + 10), kInk, 2 * sc.s, 0, 1.2f * sc.s);
                Text(dl, g_type, vs, sc.P(x, y + 1), cell ? kInk : kInkSoft, v);
            }
        } else if (it.value) {
            std::string v = Tr(it.value());
            Text(dl, g_type, vs, sc.P(kValueX, y + 1), ink, v);
            if (on && it.change) {
                Text(dl, g_type, vs, sc.P(kValueX - 14, y + 1), kInkFaint, "<");
                Text(dl, g_type, vs, sc.P(kValueX + 6 + Width(g_type, vs, v) / sc.s, y + 1), kInkFaint, ">");
            }
        }
    }
    // more above or below
    // (drawn: the typefaces have no arrows)
    auto arrow = [&](float y, float dir) {
        ImVec2 c = sc.P(313, y);
        float r = 5 * sc.s;
        dl->AddTriangleFilled(ImVec2(c.x - r, c.y - dir * r * 0.6f), ImVec2(c.x + r, c.y - dir * r * 0.6f),
                              ImVec2(c.x, c.y + dir * r * 0.6f), kInkSoft);
    };
    if (g_scroll > 0) arrow(kTop - 16, -1);
    if (g_scroll + kRows < count) arrow(kTop + kRows * kStep - 8, 1);

    // the selected row's hint at the foot of the paper
    {
        std::string hint = Tr(rebinding ? std::string("Press the new key or mouse button. Escape cancels, Delete clears.") : sel.hint);
        if (!hint.empty()) {
            // two lines at most within the paper (each broken at the last space that fits), the type a little
            // smaller where two lines of it do not hold the hint (the longer languages); a line once broke only
            // the first, and the second ran off the paper's sides
            const float wmax = 270 * sc.s;
            auto wrap = [&](float size) {
                std::vector<std::string> lines;
                std::string rest = hint;
                while (!rest.empty()) {
                    if (Width(g_type, size, rest) <= wmax) {
                        lines.push_back(rest);
                        break;
                    }
                    size_t cut = rest.size();
                    while (cut > 0 && (rest[cut - 1] != ' ' || Width(g_type, size, rest.substr(0, cut - 1)) > wmax)) cut--;
                    if (cut == 0) {  // (a word longer than the line)
                        lines.push_back(rest);
                        break;
                    }
                    lines.push_back(rest.substr(0, cut - 1));
                    rest = rest.substr(cut);
                }
                return lines;
            };
            float hs = 11 * sc.s;
            std::vector<std::string> lines = wrap(hs);
            while (lines.size() > 2 && hs > 8.5f * sc.s) hs *= 0.94f, lines = wrap(hs);
            const float step = hs / sc.s + 1;
            for (size_t i = 0; i < lines.size() && i < 3; i++)
                Text(dl, g_type, hs, sc.P(300, 322 + step * float(i)), kInkSoft, lines[i], 0.5f);
        }
    }

    // the prompts on the stone
    struct P {
        int vk;
        const char* pad;
        const char* fallback;
        const char* word;
    };
    std::vector<P> prompts;
    if (rebinding) {
        prompts = {{VK_ESCAPE, nullptr, "Esc", "Cancel"}, {VK_DELETE, nullptr, "Del", "Clear"}};
    } else if (sel.binding >= 0) {
        prompts = {{VK_RETURN, "a", "Enter", "Change"}, {VK_DELETE, "x", "Del", "Clear"}, {VK_ESCAPE, "b", "Esc", "Back"}};
    } else {
        prompts = {{VK_RIGHT, nullptr, "<>", "Change"}, {VK_ESCAPE, "b", "Esc", "Back"}};
        if (!cod::prompts::KeyboardShown()) prompts[0] = {VK_RIGHT, "a", "A", "Change"};
    }
    const float py = 416, ps = 22;
    float total = 0;
    std::vector<float> widths;
    for (const P& p : prompts) {
        float w = ps + 8 + Width(g_type, 20 * sc.s, Tr(p.word)) / sc.s;
        widths.push_back(w);
        total += w;
    }
    float gap = (420 - total) / float(prompts.size() + 1), x = 110 + gap;
    for (size_t i = 0; i < prompts.size(); i++) {
        const P& p = prompts[i];
        if (p.vk == VK_RIGHT && cod::prompts::KeyboardShown()) {
            uint64_t id = gpu::UiTexture("prompts/kb_leftright.dds");
            if (id) dl->AddImage(ImTextureID(id), sc.P(x, py - ps / 2), sc.P(x + ps, py + ps / 2));
        } else {
            Prompt(dl, sc.P(x + ps / 2, py), ps * sc.s, p.vk, p.pad, Tr(p.fallback));
        }
        Text(dl, g_type, 20 * sc.s, sc.P(x + ps + 8, py + 1), kStoneInk, Tr(p.word));
        x += widths[i] + gap;
    }
    if (in_tv) {
        dl->PopClipRect();
        // the television again over the panel: its glass rounds the corners, its reflections lie over it
        float w = io.DisplaySize.x, h = io.DisplaySize.y;
        float vw = std::min(w, h * 16.0f / 9.0f), vh = vw * 9.0f / 16.0f;
        if (uint64_t id = gpu::UiTexture("ui/tv_frame.dds"))
            dl->AddImage(ImTextureID(id), ImVec2((w - vw) / 2, (h - vh) / 2), ImVec2((w + vw) / 2, (h + vh) / 2));
    }
    if (closing) {
        g_open = false;
        RestartIfNeeded();
    }
}

// the pages hold every option; the extra sections of the old window are gone
void GraphicsExtra() {}
void GeneralExtra() {}

}  // namespace overlay
}  // namespace cod
