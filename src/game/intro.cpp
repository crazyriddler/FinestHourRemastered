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

// Menu Ambience (Issue #44): the main menu's backdrop movie (SHELL\MOVIES\fend.bik) carries a minute of battle (wind,
// guns, explosions far off) the PlayStation 2 plays under the menu's music. The Xbox's menu mutes it as it opens it
// (sub_000B1DB0 opens it, then sub_000B1B00(0) sets its track's volume to 0) and gives it its volume back only for
// a menu without music (sub_000B1B40(0)), called before the movie is open: it stays silent. Its mute is left out:
// the track at kAmbience, under the music (at Bink's own volume, 0x8000, its explosions drowned the music out by
// 16 dB). Changed in the settings, it follows at once.
namespace {
constexpr uint32_t kAmbience = 0x5000;  // Bink's volume: 0x8000 its own, 0 none
uint32_t g_menu_movie = 0;          // fend.bik's handle while open
uint32_t g_menu_volume = 0x8000;    // the volume the game last gave its track
int g_menu_applied = -1;            // the Menu Ambience setting the track's volume follows

bool MenuMovie(const std::string& path) {
    size_t at = path.find_last_of("\\/");
    std::string file = path.substr(at == std::string::npos ? 0 : at + 1);
    return _stricmp(file.c_str(), "fend.bik") == 0;
}
}  // namespace

// BinkSetVolume(bink, track, volume), stdcall
X86_EXTERN_FUNC(__imp__sub_00253800);
X86_FUNC(sub_00253800) {
    const uint32_t bink = R32(ctx.esp + 4), track = R32(ctx.esp + 8);
    if (bink && bink == g_menu_movie && track == 0) {
        g_menu_volume = R32(ctx.esp + 12);
        g_menu_applied = cod::Get().menu_ambience ? 1 : 0;
        if (g_menu_applied && g_menu_volume == 0) W32(ctx.esp + 12, kAmbience);
    }
    __imp__sub_00253800(ctx, base);
}

// BinkOpen(name, flags), stdcall: every movie's handle and file, for the subtitles of a language whose movies the
// game lacks (game/movie_subs.cpp), and the menu's backdrop
X86_EXTERN_FUNC(__imp__sub_00251CC0);
X86_FUNC(sub_00251CC0) {
    const std::string name = ReadCString(R32(ctx.esp + 4));
    __imp__sub_00251CC0(ctx, base);
    if (!ctx.eax) return;
    cod::movie_subs::Opened(ctx.eax, name);
    if (MenuMovie(name)) g_menu_movie = ctx.eax, g_menu_volume = 0x8000, g_menu_applied = -1;
    else if (ctx.eax == g_menu_movie) g_menu_movie = 0;  // (its handle's memory another movie's now)
}

// (tests) XBE_BINK_DUMP's frames
static void Dump(uint32_t bink, uint32_t dest, uint32_t pitch, uint32_t height) {
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

// BinkCopyToBuffer: the frame on the screen, the subtitles of a language whose movies the game lacks drawn into it
// (game/movie_subs.cpp). Tests (XBE_BINK_DUMP=<folder>,<first>,<last>): each frame as copied out, raw (its pitch and
// height in the name), from frame first to last
X86_EXTERN_FUNC(__imp__sub_002539B0);
X86_FUNC(sub_002539B0) {  // BinkCopyToBuffer(bink, dest, pitch, height, x, y, flags), stdcall
    uint32_t bink = R32(ctx.esp + 4), dest = R32(ctx.esp + 8), pitch = R32(ctx.esp + 12), height = R32(ctx.esp + 16);
    __imp__sub_002539B0(ctx, base);
    // the menu's backdrop: Menu Ambience changed in the settings while it plays
    const int want = cod::Get().menu_ambience ? 1 : 0;
    if (bink && bink == g_menu_movie && g_menu_applied >= 0 && want != g_menu_applied) {
        const uint32_t eax = ctx.eax;
        CallGuest(ctx, 0x253800, {bink, 0, g_menu_volume});
        ctx.eax = eax;
    }
    Dump(bink, dest, pitch, height);
    cod::movie_subs::Draw(bink, dest, pitch, height);
}
