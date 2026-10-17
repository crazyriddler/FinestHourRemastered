// Skip Intros: the publisher and studio logos and the opening movie are not played
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/cod.h"
#include "core/log.h"
#include "xdk/hle.h"

using namespace kernel;

void PlayMovie(X86Context& ctx, uint8_t* base);  // hud.cpp: the movie, 4:3 in the middle

// PlayMovie(name, ...), cdecl: plays a Bink movie from SHELL\MOVIES to its end or a button press
X86_FUNC(sub_000B2000) {
    // tests: XBE_SKIP_MOVIES leaves out every movie (the level's briefing film too): straight to the play
    static const bool skip_all = std::getenv("XBE_SKIP_MOVIES") != nullptr;
    if (skip_all) {
        LOGR("intro: movie %s skipped", ReadCString(R32(ctx.esp + 4)).c_str());
        Return(ctx, 0, 0);
        return;
    }
    if (cod::Get().skip_intros) {
        std::string name = ReadCString(R32(ctx.esp + 4));
        for (const char* intro : {"atvi.bik", "spark.bik", "kuju.bik", "Intro.bik"})
            if (_stricmp(name.c_str(), intro) == 0) {
                Return(ctx, 0, 0);  // cdecl
                return;
            }
    }
    PlayMovie(ctx, base);
}
