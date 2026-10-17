#pragma once
#include <cstdint>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

void Init();  // window thread, before boot
// the controller on that port, then GameInfo::pad
Pad Poll(int port);
bool AnyController();  // a controller is plugged in
Pad Last(int port);
Pad Raw(int port);     // the controller as it is, before GameInfo::pad (the settings overlay's)
// a button held for the game after everything else (test replays included): analog index, until (GetTickCount64 ms)
void Press(int analog, uint64_t ms);
void PressButtons(uint16_t bits, uint64_t ms);  // digital buttons (d-pad, start, back, stick clicks) the same way  // for ms from now    // the report the title got last (after GameInfo::pad and test replays)
enum Kind { kNoController, kXbox, kPlayStation, kNintendo, kOtherController };
Kind ControllerKind(int port);  // the controller on that port, by its buttons' names
void Rumble(int port, uint16_t left, uint16_t right);

}  // namespace input
