#pragma once
// the settings overlay (overlay.cpp)
namespace cod::overlay {

void Init();     // window thread, before the first frame: fonts
void Toggle();
struct ImFont* MenuFont();  // the menus' typeface (the pack's Jost Bold), else the plain one
struct ImFont* HudFont();   // the HUD's and pause screen's (DejaVu Sans Condensed), else the plain one
bool PauseRequested();      // tests: the PC options asked for from the pause screen
void Draw(double fps);   // render thread, each frame inside ImGui's
// sections other files add
void GraphicsExtra();
void GeneralExtra();
// graphics presets: 0 Very High, 1 High, 2 Medium, 3 Low, 4 Original, 5 Custom (none of them)
int CurrentPreset();
void ApplyPreset(int preset);

}  // namespace cod::overlay
