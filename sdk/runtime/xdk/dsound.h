#pragma once
// DirectSound (dsound.cpp): a sound of the title's played again, for a port's own menu row standing among the
// title's (its move sound: the title has none to play for it)
namespace xdk::dsound {

void RememberNextSound();  // the title's next sound, within a quarter second, is remembered
void ReplayRemembered();   // played again as the title played it: its buffer, region, rate and volume

}  // namespace xdk::dsound
