// The movies' subtitles in a language whose movies the game lacks (Russian: the briefings, the acts' and the ending's,
// translated by the community): lang/<code>_movies.bin (tools/subs_render.py from lang/<code>_movies.txt), each
// subtitle a picture in the movie's own 640x480 pixels, drawn into the frame as BinkCopyToBuffer writes it to the
// screen: in the picture as if burned in, through the television and its tube with it.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/cod.h"
#include "core/log.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

struct Sub {
    std::string movie;  // lower case, no extension
    float start = 0, end = 0;
    uint16_t x = 0, y = 0, w = 0, h = 0;
    std::vector<uint8_t> px;  // alpha, grey for each pixel, row by row
};

std::vector<Sub> g_subs;
std::mutex g_lock;
std::string g_movie;  // the one playing, lower case

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool Read(std::ifstream& in, void* p, size_t n) { return bool(in.read(static_cast<char*>(p), std::streamsize(n))); }

}  // namespace

namespace cod::movie_subs {

void Load(const char* code) {
    const auto path = app::ExeDir() / "lang" / (std::string(code) + "_movies.bin");
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    char magic[4];
    uint32_t version = 0, count = 0;
    if (!Read(in, magic, 4) || std::memcmp(magic, "FHMS", 4) || !Read(in, &version, 4) || version != 1 || !Read(in, &count, 4))
        return LOGE("movie subtitles: %s_movies.bin unreadable", code);
    std::vector<Sub> subs(count);
    for (Sub& s : subs) {
        uint8_t len = 0;
        char name[256];
        if (!Read(in, &len, 1) || !Read(in, name, len) || !Read(in, &s.start, 4) || !Read(in, &s.end, 4) || !Read(in, &s.x, 2) ||
            !Read(in, &s.y, 2) || !Read(in, &s.w, 2) || !Read(in, &s.h, 2))
            return LOGE("movie subtitles: %s_movies.bin cut short", code);
        s.movie = Lower(std::string(name, len));
        // runs of clear and drawn pixels
        const size_t n = size_t(s.w) * s.h;
        s.px.assign(n * 2, 0);
        for (size_t at = 0; at < n;) {
            uint16_t clear = 0, lit = 0;
            if (!Read(in, &clear, 2) || !Read(in, &lit, 2) || at + clear + lit > n || !Read(in, &s.px[(at + clear) * 2], size_t(lit) * 2))
                return LOGE("movie subtitles: %s_movies.bin damaged", code);
            at += size_t(clear) + lit;
        }
        if (s.x + s.w > 640 || s.y + s.h > 480) return LOGE("movie subtitles: %s_movies.bin out of the picture", code);
    }
    std::lock_guard lock(g_lock);
    g_subs = std::move(subs);
    LOGR("movie subtitles: %zu from %s_movies.bin", g_subs.size(), code);
}

void Start(const std::string& movie) {
    std::string m = Lower(movie);
    if (m.size() > 4 && m.compare(m.size() - 4, 4, ".bik") == 0) m.resize(m.size() - 4);
    std::lock_guard lock(g_lock);
    g_movie = m;
}

void Draw(uint32_t bink, uint32_t dest, uint32_t pitch, uint32_t height) {
    std::lock_guard lock(g_lock);
    if (g_subs.empty() || g_movie.empty() || !bink || !dest || pitch < 640 * 4 || height < 480) return;
    // the frame being shown: the movie's frame counter is two ahead of it as the copy runs (measured against the
    // movie's frames), at its own rate (30 for these)
    uint32_t num = R32(bink + 0x14), den = R32(bink + 0x18);
    const double fps = num && den && num / den < 240 ? double(num) / den : 30.0;
    const double t = (double(R32(bink + 0x0C)) - 2) / fps;
    uint8_t* out = static_cast<uint8_t*>(HostPtr(dest));
    for (const Sub& s : g_subs) {
        if (s.movie != g_movie || t < s.start || t >= s.end) continue;
        for (uint32_t y = 0; y < s.h; y++) {
            uint8_t* row = out + size_t(s.y + y) * pitch + size_t(s.x) * 4;
            const uint8_t* p = &s.px[size_t(y) * s.w * 2];
            for (uint32_t x = 0; x < s.w; x++, row += 4, p += 2) {
                const uint32_t a = p[0], g = p[1];
                if (!a) continue;
                for (int c = 0; c < 3; c++) row[c] = uint8_t((row[c] * (255 - a) + g * a + 127) / 255);
            }
        }
    }
}

}  // namespace cod::movie_subs
