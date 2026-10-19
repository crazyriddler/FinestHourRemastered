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

#include "kernel/kernel.h"
#include "xdk/hle.h"

void cod_job_thread_idle(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    kernel::CoreYield yield;  // (sharing the title thread's core: its turn while this one rests)
    Sleep(1);
}

namespace cod::jobs {

// The loading thread builds RenderWare's objects (frames, atomics, the resource arena's blocks) that the title's
// thread walks and draws, with no lock between them: on the Xbox it ran only while the title thread waited (one core,
// the title thread above it). On a PC the two ran at once and corrupted RenderWare's lists (a dirty frame's object
// list, the arena's block list: crashes in sub_001CE830 and sub_001CB8E0). They share one core again
// (kernel::ShareOneCore): the loading thread runs while the title thread waits on something, as it did. The title
// thread is not the XBE's entry (0x13409E): that one only sizes a stack, starts the CRT's main (0x13402A) on a thread
// of its own with it and exits; main's thread is the one that runs the game.
constexpr uint32_t kTitleMain = 0x13402A;   // the title thread: the CRT's main, started by the XBE's entry
constexpr uint32_t kLoadingThread = 0x68D90;

void Init() {
    kernel::ShareOneCore(kTitleMain);
    kernel::ShareOneCore(kLoadingThread);
}

}  // namespace cod::jobs
