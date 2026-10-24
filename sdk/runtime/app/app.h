#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height
    uint32_t fps_cap = 60;           // 0 unlimited (no setting: the game's speed follows its frame rate)
    bool vsync = true;
    uint32_t renderer = 0;           // the graphics API: 0 Direct3D 12, 1 Vulkan (from the next start)
    uint32_t speakers = 0;           // the sound: 0 stereo, 1 5.1 surround (from the next start)
    uint32_t aniso = 16;             // 0 as the game asks
    bool smaa = true;                // SMAA on the shown frame
    bool taa = true;                 // Anti-Aliasing: 4x MSAA (cut outs' edges as coverage) and temporal of the 3D
    bool motion_blur = false;        // what moves blurs along its motion (retired: off, not offered)
    uint32_t sharpening = 0;         // contrast adaptive sharpening of the shown frame, 0 to 10
    bool hdr = true;                 // the scene in 16 bit float: light added past white keeps its brightness
    bool bloom = true;               // what is brighter than white glows
    bool ao = true;                  // screen space ambient occlusion
    bool contact_shadows = true;     // short screen space shadows from the main light
    uint32_t shadow_quality = 3;     // the main light's shadow maps: 0 off, 1 low, 2 medium, 3 high
    bool dynamic_lights = true;      // muzzle flashes, tracers, fire and explosions light what is around them
    bool light_halos = true;         // the air glows about those lights
    bool soft_particles = true;      // smoke, dust and fire fade where they meet the scene
    bool heat_haze = true;           // the air over fires and explosions shimmers
    bool sun_shafts = true;          // beams of sunlight past what stands before the sun
    bool aim_dof = true;             // aiming down the sights blurs the view's edges and what lies behind the target
    bool explosion_flash = true;     // a close explosion lights up the whole view
    bool damage_shake = true;        // being hit shakes the view a little
    bool flash_shadows = true;       // the four muzzle flashes nearest the eye cast shadows
    bool character_detail = true;    // close up, skin's pores and uniforms' weave
    bool weapon_materials = true;    // the weapon in hand's wood and metal with relief and a metal sheen
    bool sharp_distance = true;      // distant surfaces' textures a little sharper
    bool lens_flare = true;          // the sun seen through a lens: a faint flare and dirt on the lens glowing in bright light
    bool water = true;               // water ripples and reflects the scene above it
    bool water_interaction = true;   // what crosses the water stirs it: rings, wakes, splashes
    bool snow_tracks = true;         // footprints in the snow, which gives under the soldiers
    bool shockwaves = true;          // an explosion's shock ripples out through the air; close ones spatter the view with dirt
    bool animated_fire = true;       // fires move inside their sprites, rising and wavering
    bool barrel_heat = true;         // the gun's barrel shimmers after firing, and smokes a little after a long burst
    bool wind = true;                // trees, bushes and flags sway in the wind
    bool ambient_particles = true;   // dust floating where light falls, embers rising over fires
    bool lit_smoke = true;           // smoke and dust lit from inside by the flashes, fire and explosions near them
    bool character_light = true;     // characters: a soft warm light through the skin and a faint rim on their outline
    bool bounce_light = true;        // light bounced off nearby lit surfaces (one bounce, never into dark rooms)
    bool soft_shadows = true;        // the sun's shadows sharp where they touch, softer the further from what casts them
    bool volumetric_fog = true;      // a light haze in the air, thicker low, lit by the sun (its beams past shadows) and the flashes
    bool auto_exposure = true;       // the eye adapts a little between dark rooms and bright streets (the game's light kept)
    bool color_grade = true;         // each campaign's own tones: the Russian winter, the African desert, Europe
    bool film_grain = true;          // a faint, fine grain
    bool hit_reactions = true;       // soldiers flinch where bullets hit them
    uint32_t models = 2;             // the models: 0 the game's own, 1 smooth (characters and weapons tessellated into
                                     // curved surfaces, PN triangles), 2 remastered (remaster/meshes: soldiers, weapons
                                     // in first and third person, pickups)
    // (the two below follow models, SetModels; not saved. Tests toggle them alone: "cmd fx remastered_models 0")
    bool remastered_models = true;
    bool smooth_models = false;
    bool reflections = true;         // glossy surfaces (metal, glass, varnished wood, wet stone) reflect the scene
    bool sun_moon = true;            // the sun (by day) or the moon (by night) in the sky, where the level's light is
    bool clouds = true;              // drifting clouds over the sky, lit by it and the sun
    bool remaster = true;            // the remaster pack's textures and materials, when there is one
    bool materials = true;           // its normal, height and specular maps lighting the surfaces
    bool parallax = true;            // parallax occlusion from its height maps
    bool detail = true;              // detail normal maps tiled over the surfaces close up
    bool pixel_lighting = true;      // vertex programs' lighting at each pixel instead of each vertex
    bool height_blend = true;        // blended surfaces fade in along their height maps
    uint32_t specular = 100;         // percent
    uint32_t brightness = 100;       // percent: the frame's gamma (the game was tuned on CRT televisions)
    float normal_green = 1;          // the normal maps' green: 1 up, -1 down
    uint32_t language = 1;           // the console's (XC_LANGUAGE): 1 English, 3 German, 4 French, 5 Spanish, 6 Italian,
                                     // 9 Portuguese; without a setting, Windows' own (Load). The title sees
                                     // ConsoleLanguage()
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
};

Settings& Get();
void Load();
void Save();
void SetModels(Settings& s, uint32_t models);  // models and the two that follow it
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
float RenderScale(uint32_t guest_height);
// the output is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9, else 4:3 as on a 4:3 TV
bool Widescreen();

std::filesystem::path ExeDir();
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
// a task's progress in a small window over the game's (the shaders made at start): its line and how far, 0 to 1; the
// same thread each call, which keeps the window's messages; a fraction below 0 closes it
void Progress(const wchar_t* text, float fraction);
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
    bool hidden = false;  // a tester's flag: read, written only when on (not offered to the players in their file)
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
    // the console's language the title is told for the setting's (a language the port gives the title's text in and
    // the title lacks: one it has underneath), null for the setting's own
    uint32_t (*console_language)(uint32_t language);
};
extern const GameInfo kGame;

// the language the title sees (XC_LANGUAGE)
inline uint32_t ConsoleLanguage() {
    return kGame.console_language ? kGame.console_language(Get().language) : Get().language;
}

// sleeps out the rest of the frame for the fps cap
void Pace();

}  // namespace app
