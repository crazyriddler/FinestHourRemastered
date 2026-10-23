#pragma once
#include "core/input.h"
#include <cstdint>
// Widescreen and field of view, shared by the renderer settings and the game's hooks
namespace cod::wide {

float Aspect();          // the output's width over its height, 4:3 at the least
float Widen();           // how many times wider than 4:3 the screen is drawn
// the game's view window half width at distance 1 (tan of half the 4:3 field of view) scaled by the field of view
// setting, relative to the widest unzoomed view the game asked for
float FovScale(float x);
void Update();           // each frame: the renderer's widening follows the settings
// the front end runs (hud.cpp): its screens are 4:3, its camera keeps the console's view
void SetFrontEnd(bool on);
bool FrontEnd();
// tests: the screen camera's heading and pitch in degrees, false before a level draws
bool CameraAngles(float& yaw, float& pitch);
// tests: the camera's place in the world (its frame's matrix)
bool CameraPos(float pos[3]);
// the remastered materials' view of the frame (gpu::g_material_frame): the screen camera's view window and the
// light's direction in its view space
void SetViewWindow(float x, float y);
void UpdateMaterialFrame();
void SetCamera(uint32_t camera);
uint32_t Camera();  // the screen camera (RwCamera), 0 before one
// tests: a free camera, the screen camera's frame lifted (centimetres) and turned (degrees) while it draws
void SetFreeCamera(bool on, float up, float pitch, float yaw);
bool FreeCamera(float& up, float& pitch, float& yaw);
// tests: the camera put anywhere (the world's x y z), looking along a heading and pitch (degrees, as CameraAngles)
void SetFreeCameraAt(bool on, const float pos[3], float yaw, float pitch);
bool FreeCameraAt(float pos[3], float& yaw, float& pitch);
// a moving free camera (shots): from one place and view to another over seconds, eased, from now (FreeCameraAt
// gives where it is each frame); then it stays at the end
void SetFreeCameraPath(const float from[5], const float to[5], float seconds);
// Free Camera (PC Options): the camera flown through the level, walls and all, the player held still. FlyEnabled the
// setting (not saved); FlyActive while it flies; FlyLook the mouse's counts; FlyUpdate takes port 0's controller
// (keyboard included) for the flight and hands the game a still one
bool* FlyEnabled();
bool FlyActive();
void FlyLook(int counts_x, int counts_y, float sensitivity);
// tests ("fly at x y z yaw pitch"): the flying camera put there, as an F9 mark's "freecam at" gives it
void FlyAt(const float pos[3], float yaw, float pitch);
void FlyUpdate(input::Pad& pad);

}  // namespace cod::wide
