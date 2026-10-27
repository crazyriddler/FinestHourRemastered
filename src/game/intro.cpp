// Skip Intros: the publisher and studio logos and the opening movie are not played
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/cod.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

void PlayMovie(X86Context& ctx, uint8_t* base);  // hud.cpp: the movie, 4:3 in the middle

// PlayMovie(name, ...), cdecl: plays a Bink movie from SHELL\MOVIES to its end or a button press
X86_FUNC(sub_000B2000) {
    // tests: XBE_SKIP_MOVIES leaves out every movie (the level's briefing film too): straight to the play
    static const bool skip_all = std::getenv("XBE_SKIP_MOVIES") != nullptr;
    // tests (XBE_MOVIE): the first movie played is this one (a bonus film, the credits) in its place
    static const char* test_movie = std::getenv("XBE_MOVIE");
    if (test_movie && *test_movie) {
        uint32_t name = mem::AllocSystem(uint32_t(std::strlen(test_movie)) + 1);
        std::memcpy(HostPtr(name), test_movie, std::strlen(test_movie) + 1);
        W32(ctx.esp + 4, name);
        test_movie = nullptr;
    }
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

// tests (XBE_BINK_DUMP=<folder>,<first>,<last>): each movie frame BinkCopyToBuffer copies out, raw (its pitch and
// height in the name), from frame first to last
X86_EXTERN_FUNC(__imp__sub_002539B0);
X86_FUNC(sub_002539B0) {  // BinkCopyToBuffer(bink, dest, pitch, height, x, y, flags), stdcall
    uint32_t bink = R32(ctx.esp + 4), dest = R32(ctx.esp + 8), pitch = R32(ctx.esp + 12), height = R32(ctx.esp + 16);
    __imp__sub_002539B0(ctx, base);
    static const char* spec = std::getenv("XBE_BINK_DUMP");
    if (!spec || !bink || !dest) return;
    static char dir[260] = {};
    static int first = 0, last = -1;
    if (!dir[0] && std::sscanf(spec, "%259[^,],%d,%d", dir, &first, &last) != 3) return;
    int frame = int(R32(bink + 0x0C));
    if (frame < first || frame > last) return;
    char path[400];
    std::snprintf(path, sizeof(path), "%s/f%05d_%ux%u.raw", dir, frame, pitch, height);
    if (FILE* f = std::fopen(path, "wb")) std::fwrite(HostPtr(dest), 1, size_t(pitch) * height, f), std::fclose(f);
}
