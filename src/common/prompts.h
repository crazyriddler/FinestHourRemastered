#pragma once
// Button prompts for the keyboard: while the keyboard and mouse are what the player uses, the controller's button
// pictures in the menus and the game's hints are replaced by key caps showing the bound keys (prompts.cpp).
namespace cod::prompts {

void PadUsed();       // a controller's button or stick moved
void KeyboardUsed();  // a key, a mouse button or the mouse moved
void Update();        // render thread, each frame: the pictures follow the device and the bindings
bool KeyboardShown(); // the prompts show keys (else the controller's buttons)

}  // namespace cod::prompts
