// The game's job thread (sub_00068D90, started with the game) takes work from a queue of one (0x348168) and, with
// none, reads the queue again at once: a loop that on the Xbox shared its one core by time slices, and on a PC keeps a
// whole core busy for the whole game (an APU's or a laptop's power). Its path with nothing queued (0x691CF, a hook in
// config/game.toml) sleeps a millisecond before the next look: work queued waits a millisecond at most.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "xdk/hle.h"

void cod_job_thread_idle(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    Sleep(1);
}
