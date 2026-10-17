#pragma once
#include <cstdint>
#include <string>

#include "core/input.h"

// Test tooling, off unless its variable is set.
//   XBE_CONTROL=<file>        a thread reads the lines appended to the file and acts on them:
//       shot <path>           the next presented frame, as the window shows it, to a BMP at <path>
//       cmd <text>            hands <text> to the game's command handler (SetCommandHandler)
//       pad <button> [ms]     the first pad's button held a while (150 ms): A B X Y BLACK WHITE LT RT UP DOWN LEFT
//                             RIGHT START BACK
//   XBE_INPUT_RECORD=<file>   writes every polled pad state change
//   XBE_INPUT_REPLAY=<file>   plays a recording back in place of the controllers
// Times are milliseconds since the first instance started (XBE_CLOCK_ORIGIN carries it across relaunches).
namespace control {
void Start();                         // once, at launch
bool TakeShot(std::wstring& path);    // the renderer, each present: a shot is due
// "record <file.mp4>" / "record stop" (or XBE_RECORD at launch): the renderer, each present: a change is due
// (path empty: stop)
bool TakeRecord(std::wstring& path);
void SetCommandHandler(void (*handler)(const std::string&));
double SinceOriginMs();
void PublishOrigin();                 // before a relaunch: the next instance keeps this clock
// input::Poll: the pad after recording, or the replayed one
input::Pad Filter(int port, const input::Pad& live);
bool Replaying();
// the recording (XBE_INPUT_RECORD), more lines: "<ms> <kind> <text>", which the replay skips
void RecordLine(const char* kind, const char* text);
bool Recording();
void Shot(const std::wstring& path);  // a screenshot of the next presented frame
// the replay's mouse motion (its "mouse" lines) due by now; false when not replaying
bool ReplayMouse(int& dx, int& dy);
// a level starts: a replay with an "<ms> anchor" line carries on from that time (a recorded session's level
// start, whatever the menus took before)
void Anchor();
}  // namespace control
