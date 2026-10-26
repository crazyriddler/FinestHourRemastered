#pragma once
// Updates from the game (updates.cpp): at the start a check on a thread of its own (the release's update.json on
// GitHub, never waited for), an update newer than the installation's offered in the main menu (the settings overlay's
// page), downloaded, checked and installed by the updater only when the player chooses it.
#include <string>

namespace cod::updates {

enum class State { None, Offered, Downloading, Starting, Failed };

void Start();             // once, at the game's start: the downloaded updater of a time before deleted, the check
State Current();
bool ShouldShow();        // offered, not yet shown this session (the overlay opens its page once)
void Shown();
std::string Version();    // the update's ("2026-10-26 21:59")
std::string Notes();      // what the update brings, in the game's language when the update has it
float Progress();         // the download's, 0 to 1
std::string Error();      // Failed's
void Accept();            // download, check, run the updater, close the game
void Later();             // not this time
void Never();             // no more checks at the start (the setting, back on in PC Settings)

}  // namespace cod::updates
