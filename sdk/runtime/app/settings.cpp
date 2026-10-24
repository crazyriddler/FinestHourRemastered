// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void SetModels(Settings& s, uint32_t models) {
    s.models = std::min<uint32_t>(models, 2);
    s.smooth_models = s.models == 1;
    s.remastered_models = s.models == 2;
}

// Windows' display language as the console's: English, French, Spanish, Italian or Portuguese, else English
uint32_t SystemLanguage() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_FRENCH: return 4;
        case LANG_SPANISH: return 5;
        case LANG_ITALIAN: return 6;
        case LANG_PORTUGUESE: return 9;
        default: return 1;
    }
}

void Load() {
    Settings& s = Get();
    s.language = SystemLanguage();  // until the settings file says otherwise
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.renderer = t["renderer"].value_or(std::string(s.renderer ? "vulkan" : "d3d12")) == "vulkan" ? 1 : 0;
    s.speakers = t["speakers"].value_or(std::string(s.speakers ? "5.1" : "stereo")) == "5.1" ? 1 : 0;
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.smaa = t["smaa"].value_or(s.smaa);
    s.fxaa = t["fxaa"].value_or(s.fxaa);
    s.language = t["language"].value_or(s.language);
    // the remaster's look, on unless the player turned a part of it off (PC settings, Graphics and Effects)
    s.remaster = t["remaster"].value_or(s.remaster);
    s.bloom = t["bloom"].value_or(s.bloom);
    s.ao = t["ambient_occlusion"].value_or(s.ao);
    s.contact_shadows = t["contact_shadows"].value_or(s.contact_shadows);
    s.dynamic_lights = t["dynamic_lights"].value_or(s.dynamic_lights);
    s.light_halos = t["light_halos"].value_or(s.light_halos);
    s.soft_particles = t["soft_particles"].value_or(s.soft_particles);
    s.heat_haze = t["heat_haze"].value_or(s.heat_haze);
    s.sun_shafts = t["sun_shafts"].value_or(s.sun_shafts);
    s.aim_dof = t["aim_depth_of_field"].value_or(s.aim_dof);
    s.explosion_flash = t["explosion_flash"].value_or(s.explosion_flash);
    s.damage_shake = t["damage_shake"].value_or(s.damage_shake);
    s.flash_shadows = t["flash_shadows"].value_or(s.flash_shadows);
    s.auto_exposure = t["auto_exposure"].value_or(s.auto_exposure);
    s.volumetric_fog = t["volumetric_fog"].value_or(s.volumetric_fog);
    s.soft_shadows = t["soft_shadows"].value_or(s.soft_shadows);
    s.bounce_light = t["bounce_light"].value_or(s.bounce_light);
    s.character_light = t["character_light"].value_or(s.character_light);
    s.lit_smoke = t["lit_smoke"].value_or(s.lit_smoke);
    s.ambient_particles = t["ambient_particles"].value_or(s.ambient_particles);
    s.wind = t["wind"].value_or(s.wind);
    s.barrel_heat = t["barrel_heat"].value_or(s.barrel_heat);
    s.animated_fire = t["animated_fire"].value_or(s.animated_fire);
    s.shockwaves = t["shockwaves"].value_or(s.shockwaves);
    s.water = t["water"].value_or(s.water);
    s.water_interaction = t["water_interaction"].value_or(s.water_interaction);
    s.snow_tracks = t["snow_tracks"].value_or(s.snow_tracks);
    s.lens_flare = t["lens_flare"].value_or(s.lens_flare);
    s.sharp_distance = t["sharp_distance"].value_or(s.sharp_distance);
    s.weapon_materials = t["weapon_materials"].value_or(s.weapon_materials);
    s.character_detail = t["character_detail"].value_or(s.character_detail);
    s.color_grade = t["color_grade"].value_or(s.color_grade);
    s.film_grain = t["film_grain"].value_or(s.film_grain);
    s.reflections = t["reflections"].value_or(s.reflections);
    // the models: one setting now; from before, the two toggles (the remastered ones on, or the smooth ones, or
    // neither: the game's own)
    if (auto m = t["models"].value<int64_t>()) {
        SetModels(s, uint32_t(std::clamp<int64_t>(*m, 0, 2)));
    } else if (t["remastered_models"] || t["smooth_models"]) {
        SetModels(s, t["remastered_models"].value_or(true) ? 2u : t["smooth_models"].value_or(false) ? 1u : 0u);
    } else {
        SetModels(s, s.models);
    }
    s.hit_reactions = t["hit_reactions"].value_or(s.hit_reactions);
    s.sun_moon = t["sun_moon"].value_or(s.sun_moon);
    s.clouds = t["clouds"].value_or(s.clouds);
    s.materials = t["materials"].value_or(s.materials);
    s.parallax = t["parallax"].value_or(s.parallax);
    s.detail = t["detail"].value_or(s.detail);
    s.pixel_lighting = t["pixel_lighting"].value_or(s.pixel_lighting);
    s.height_blend = t["height_blend"].value_or(s.height_blend);
    s.specular = std::min<uint32_t>(t["specular"].value_or(s.specular), 200);
    // the pipeline's own: for development only (XBE_DEV_SETTINGS, side by side comparisons)
    if (std::getenv("XBE_DEV_SETTINGS")) {
        s.hdr = t["hdr"].value_or(s.hdr);
        s.normal_green = float(t["normal_green"].value_or(double(s.normal_green)));
    }
    s.sharpening = std::min<uint32_t>(t["sharpening"].value_or(s.sharpening), 10);
    s.shadow_quality = std::min<uint32_t>(t["shadow_quality"].value_or(s.shadow_quality), 3);
    s.brightness = std::clamp<uint32_t>(t["brightness"].value_or(s.brightness), 50, 200);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"vsync", s.vsync},
                  {"renderer", s.renderer ? "vulkan" : "d3d12"},
                  {"speakers", s.speakers ? "5.1" : "stereo"},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"smaa", s.smaa},
                  {"fxaa", s.fxaa},
                  {"language", int64_t(s.language)},
                  {"sharpening", int64_t(s.sharpening)},
                  {"shadow_quality", int64_t(s.shadow_quality)},
                  {"remaster", s.remaster},
                  {"materials", s.materials},
                  {"parallax", s.parallax},
                  {"detail", s.detail},
                  {"height_blend", s.height_blend},
                  {"specular", int64_t(s.specular)},
                  {"pixel_lighting", s.pixel_lighting},
                  {"bloom", s.bloom},
                  {"ambient_occlusion", s.ao},
                  {"contact_shadows", s.contact_shadows},
                  {"dynamic_lights", s.dynamic_lights},
                  {"light_halos", s.light_halos},
                  {"soft_particles", s.soft_particles},
                  {"heat_haze", s.heat_haze},
                  {"sun_shafts", s.sun_shafts},
                  {"aim_depth_of_field", s.aim_dof},
                  {"explosion_flash", s.explosion_flash},
                  {"damage_shake", s.damage_shake},
                  {"flash_shadows", s.flash_shadows},
                  {"auto_exposure", s.auto_exposure},
                  {"volumetric_fog", s.volumetric_fog},
                  {"soft_shadows", s.soft_shadows},
                  {"bounce_light", s.bounce_light},
                  {"character_light", s.character_light},
                  {"lit_smoke", s.lit_smoke},
                  {"ambient_particles", s.ambient_particles},
                  {"wind", s.wind},
                  {"barrel_heat", s.barrel_heat},
                  {"animated_fire", s.animated_fire},
                  {"shockwaves", s.shockwaves},
                  {"water", s.water},
                  {"water_interaction", s.water_interaction},
                  {"snow_tracks", s.snow_tracks},
                  {"lens_flare", s.lens_flare},
                  {"sharp_distance", s.sharp_distance},
                  {"weapon_materials", s.weapon_materials},
                  {"character_detail", s.character_detail},
                  {"color_grade", s.color_grade},
                  {"film_grain", s.film_grain},
                  {"reflections", s.reflections},
                  {"models", int64_t(s.models)},
                  {"hit_reactions", s.hit_reactions},
                  {"sun_moon", s.sun_moon},
                  {"clouds", s.clouds},
                  {"brightness", int64_t(s.brightness)},
                  {"join_ip", s.join_ip}};
    for (const Option& o : kGame.options) {
        if (o.hidden && !(o.flag && *o.flag)) continue;
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

Mode Output() {
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

bool Widescreen() {
    // asked per frame and per HUD quad: the desktop mode lookup only runs when the resolution setting changes
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<bool> wide{true};
    const Settings& s = Get();
    uint64_t k = uint64_t(s.width) << 32 | s.height;
    if (key.exchange(k) != k) {
        Mode m = Output();
        wide = m.width * 2 >= m.height * 3;  // 3:2 or wider
    }
    return wide;
}

float RenderScale(uint32_t guest_height) {
    float h = Output().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app
