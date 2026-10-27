#pragma once
// Call of Duty: Finest Hour settings beyond the runtime's display ones, kept in the settings file
#include <cstdint>
#include <string>

struct X86Context;

namespace cod {

struct Settings {
    // general
    bool skip_intros = true;   // publisher and studio logos and the opening movie
    bool developer = false;    // the testers' keys: F9 marks a place (a screenshot and where, in logs/marks), F3 the threads
    // display
    bool widescreen = true;    // the screen at the output's aspect (Hor+), 2D kept in proportion: set each frame from the output
                               // (wider than 4:3), not a setting
    bool tv_frame = true;      // the 4:3 menus and movies inside an old television on a wide screen
    uint32_t fov = 0;          // horizontal field of view at 4:3 in degrees, 0 the game's
    bool fps_counter = false;
    // the HUD's pieces (hidden by their textures: the original and the remastered alike)
    bool hud_hit_markers = true;  // the marks of a shot of yours hitting and of where shots hitting you come from
    bool hud_compass = true;
    bool hud_stance = true;       // standing, crouching, lying down
    bool hud_crosshair = true;
    uint32_t hud_size = 2;        // 0 the original's, 1 small, 2 smaller: each element where it was (game/hud.cpp);
                                  // a graphics preset sets it too (the Original preset the original's, the others smaller)
    // input
    bool keyboard_mouse = true;
    uint32_t mouse_sensitivity = 100;  // percent
    bool invert_mouse = false;
    bool toggle_aim = true;  // the aim action's mouse buttons aim on a click and stop on the next (keys still hold)
    uint32_t button_prompts = 0;  // 0 the device in use, 1 the controller's, 2 the keyboard's
    // controller
    uint32_t pad_layout = 0;          // 0 modern (A jump, X reload, RB grenade...), 1 the original's
    uint32_t stick_deadzone = 5;      // percent of the right stick's travel
    uint32_t stick_sensitivity = 100; // percent
    uint32_t stick_sens_x = 100;      // percent: the controller's turn across
    uint32_t stick_sens_y = 100;      // percent: up and down
    uint32_t stick_outer_boost = 115; // percent: the turn at the stick's full tilt against its curve's (the centre kept)
    bool invert_stick_y = false;
    bool look_acceleration = false;   // the game's eased turn (ramps up, drifts on after the stick lets go)
    // cheats (PC Settings, Cheats; all off by default)
    bool free_camera = false;    // F11 flies the camera through the level (common/widescreen.cpp)
    bool god_mode = false;       // the player takes no damage
    bool infinite_ammo = false;  // every weapon's reserve kept full, grenades and mines never run out
    bool no_reload = false;      // the weapon in hand's magazine kept full: it never needs reloading
};

Settings& Get();
bool OverlayOpen();  // the settings overlay (F10) is up: the game gets no keys or mouse
bool InMovie();      // a movie plays (hud.cpp)
bool InMenu();       // a menu has the controller: the front end, the pause screen or a movie (game_info.cpp)
bool Paused();       // the pause screen is up (hud.cpp)

// Portuguese: the game's text from lang/pt.txt as it reads its files (game/language.cpp)
namespace lang {
void Init();
bool Portuguese();
void Patch(const std::string& xbox_path, uint64_t pos, uint8_t* data, uint32_t n);
}  // namespace lang

// the movies' subtitles in a language the game's own movies lack (game/movie_subs.cpp): lang/<code>_movies.bin
namespace movie_subs {
void Load(const char* code);
void Start(const std::string& movie);  // the movie PlayMovie plays (its file's name), "" when it ends
// drawn into the movie's frame BinkCopyToBuffer has just written (32 bit pixels)
void Draw(uint32_t bink, uint32_t dest, uint32_t pitch, uint32_t height);
}  // namespace movie_subs

// tests: the level scripts' checkpoint register read as n the next time (game/checkpoint.cpp)
void ForceCheckpoint(int n);
void ForceSections(const int s[4]);  // tests: the next starting sections, in place of the script's
// the checkpoint register now and the last starting sections (false: none given since the game started)
bool LastCheckpoint(int& reg, int sections[4]);
// the four sections of the level in memory now (-1 a free slot)
void LoadedSections(int sections[4]);
// the player's entity (game/damage.cpp, 0 before the game looked it up); god mode: its damage held off
uint32_t PlayerEntity();
// the game's current level set (game/hud.cpp: a level left, its last screens in the television)
void LevelSet();
void SetGodMode(bool on);
bool GodMode();
// tests: the level ended as its script ends it, the next one loaded (QuitAndLoadNextLevel; game/player_tests.cpp);
// 0: quit to the main menu (QuitLevel)
void EndLevel(int next);
// tests: the player handed weapon id n (0..0x26, the game's table) in place of the held one, its model streamed first
// (game/player_tests.cpp)
void GiveWeapon(int n);
// tests: n of a thrown item (a grenade's, a mine's id), chosen as the one thrown next
void GiveItem(int id, int count);
// the cheats' work in the player's update (game/cheats.cpp, from game/player_tests.cpp's hook)
void CheatsStep(X86Context& ctx, uint32_t player);

}  // namespace cod
