// The level scripts' checkpoint register (tests, and a start from a later checkpoint). The script function
// SetCheckPointRegister (0x39450) keeps its value at 0x3B4740, GetCheckPointRegister (0x394F0) copies it onto the
// script's stack. Restarting the level clears it, and the level's loader reads it before the script does (which
// sections to load): a checkpoint asked for (cod.h ForceCheckpoint) is held there, written again and again, until the
// script has read it after a restart.
#include <atomic>
#include <chrono>
#include <thread>

#include "common/cod.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {
constexpr uint32_t kRegister = 0x3B4740;
std::atomic<int> g_forced{-1};
std::atomic<bool> g_reset_seen{false};  // the register went to 0 (a restart) since it was asked for
std::atomic<int> g_sections[4] = {-1, -1, -1, -1};  // tests: the starting sections put in place of the script's
std::atomic<int> g_last_sections[4] = {-1, -1, -1, -1};  // the last starting sections the level was given
}  // namespace

namespace cod {
// The level's sections in memory: four slots of 0x44 bytes from 0x3E2680 (IsLevelSectionLoaded, 0x392F0), each in
// use when its first word is not 0, its section's number the 16 bits at 8. -1 for a free slot.
void LoadedSections(int sections[4]) {
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t slot = 0x3E2680 + i * 0x44;
        sections[i] = kernel::R32(slot) ? int(int16_t(kernel::R32(slot + 8) & 0xFFFF)) : -1;
    }
}

bool LastCheckpoint(int& reg, int sections[4]) {
    reg = int(kernel::R32(kRegister));
    for (int i = 0; i < 4; i++) sections[i] = g_last_sections[i];
    return sections[0] >= 0;
}

void ForceSections(const int s[4]) {
    for (int i = 0; i < 4; i++) g_sections[i] = s ? s[i] : -1;
}

void ForceCheckpoint(int n) {
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            for (;;) {
                int n = g_forced;
                if (n >= 0) {
                    if (kernel::R32(kRegister) != uint32_t(n)) g_reset_seen = true;
                    kernel::W32(kRegister, uint32_t(n));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    });
    g_reset_seen = false;
    g_forced = n;
}
}  // namespace cod

X86_EXTERN_FUNC(__imp__sub_000394F0);
X86_FUNC(sub_000394F0) {  // GetCheckPointRegister
    int forced = g_forced;
    if (forced >= 0) {
        kernel::W32(kRegister, uint32_t(forced));
        if (g_reset_seen) g_forced = -1;  // read after the restart: the level has it
    }
    LOGR("checkpoint: read %u%s", kernel::R32(kRegister), forced >= 0 ? " (asked for)" : "");
    __imp__sub_000394F0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00039450);
X86_FUNC(sub_00039450) {  // SetCheckPointRegister
    __imp__sub_00039450(ctx, base);
    LOGR("checkpoint: set %u", kernel::R32(kRegister));
}

// the sections a (re)start loads (SetStartingSections' four, 0x39530, handed to 0x10E740 as an array): logged
X86_EXTERN_FUNC(__imp__sub_0010E740);
X86_FUNC(sub_0010E740) {
    uint32_t list = kernel::R32(ctx.esp + 4);
    if (g_sections[0] >= 0)
        for (int i = 0; i < 4; i++) kernel::W32(list + uint32_t(i) * 4, uint32_t(g_sections[i].exchange(-1)));
    for (int i = 0; i < 4; i++) g_last_sections[i] = int(kernel::R32(list + uint32_t(i) * 4));
    LOGR("checkpoint: starting sections %d %d %d %d (register %u)", int(kernel::R32(list)), int(kernel::R32(list + 4)),
         int(kernel::R32(list + 8)), int(kernel::R32(list + 12)), kernel::R32(kRegister));
    __imp__sub_0010E740(ctx, base);
}
