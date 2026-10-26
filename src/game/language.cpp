// Portuguese, Russian and Polish (languages 9, 10 and 11 in the settings): the game has English, German, French,
// Italian and Spanish, not these. It runs in English (its speech, kGame.console_language) and its text is put in the
// English text's place as the game reads it, from lang/pt.txt, lang/ru.txt or lang/pl.txt beside the exe (the port's
// own translations; nothing of the game's is written):
// - the string tables in COD.PAK ($key,English,German,French,Italian,Spanish, Latin-1): a row the translation has
//   becomes $key,Portuguese with the other columns empty, in the row's own length (spaces after the last column do
//   not show);
// - the subtitles in the sound banks (SB_AGD\<range>\ENGLISH\*.agd): each record holds the five languages' lines,
//   each in a slot of its own (English's 256 bytes); the English line becomes its Portuguese, found by the English
//   line's hash (FNV-1a 64 of its Latin-1 bytes), as long as the slot holds it.
// lang/<x>.txt: UTF-8 lines "$key<tab>text" and "@<hash in hex><tab>text" (\n a line break); # starts a comment.
// Portuguese in Latin-1 as the game's text is. Russian in Windows-1251's letters (А-я 0xC0-0xFF, Ё 0xA8, ё 0xB8), the
// rest Windows-1252 as the game's: its fonts get those letters in those codes (lang/ru_fonts.bin, made by
// tools/remaster/lang_fonts.py: the atlases' pixels where the Latin-1 accented letters were and their boxes in the
// METRICS1 files, put in COD.PAK's reads; the remastered atlases are the pack's, under the new atlases' ids).
// Polish in Windows-1250's codes for its own letters (ą 0xB9, ł 0xB3, ś 0x9C...; ó Latin-1's), the rest as the game's:
// lang/pl_fonts.bin draws them at those codes after the atlases' last boxes.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "common/cod.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kPortuguese = 9;
constexpr uint32_t kRussian = 10;
constexpr uint32_t kPolish = 11;

bool g_on = false;
uint32_t g_lang = 0;  // kPortuguese, kRussian or kPolish while on
std::unordered_map<std::string, std::string> g_keys;  // a string table's key -> its text (Latin-1)
std::unordered_map<uint64_t, std::string> g_lines;    // a subtitle's English line (its hash) -> its text (Latin-1)

uint64_t Fnv(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// a code point as the game's text has it: Russian's letters in Windows-1251's codes, Polish's in Windows-1250's, the
// punctuation Windows-1252 has past Latin-1 in its codes, the rest Latin-1 (with Russian, Latin-1's 0xC0 on are its
// letters: '?'; with Polish, the Latin-1 letters whose codes are its letters'); else '?'
char GameChar(uint32_t cp, uint32_t lang) {
    if (lang == kPolish) {
        static const std::pair<uint32_t, uint8_t> k1250[] = {
            {0x104, 0xA5}, {0x105, 0xB9}, {0x106, 0xC6}, {0x107, 0xE6}, {0x118, 0xCA}, {0x119, 0xEA},
            {0x141, 0xA3}, {0x142, 0xB3}, {0x143, 0xD1}, {0x144, 0xF1}, {0x15A, 0x8C}, {0x15B, 0x9C},
            {0x179, 0x8F}, {0x17A, 0x9F}, {0x17B, 0xAF}, {0x17C, 0xBF}};
        for (const auto& [u, b] : k1250)
            if (u == cp) return char(b);
        for (const auto& [u, b] : k1250)
            if (b == cp) return '?';  // (a Latin-1 letter in a Polish letter's code)
    }
    const bool russian = lang == kRussian;
    if (russian) {
        if (cp >= 0x410 && cp <= 0x44F) return char(0xC0 + (cp - 0x410));
        if (cp == 0x401) return char(0xA8);
        if (cp == 0x451) return char(0xB8);
        if (cp == 0x2116) return 'N';
        if (cp >= 0xC0 && cp <= 0xFF) return '?';
    }
    if (cp <= 0xFF) return char(cp);
    static const std::pair<uint32_t, uint8_t> k1252[] = {
        {0x20AC, 0x80}, {0x2026, 0x85}, {0x2018, 0x91}, {0x2019, 0x92}, {0x201C, 0x93}, {0x201D, 0x94},
        {0x2022, 0x95}, {0x2013, 0x96}, {0x2014, 0x97}, {0x2122, 0x99}, {0x201E, 0x84}, {0x201A, 0x82}};
    for (const auto& [u, b] : k1252)
        if (u == cp) return char(b);
    return '?';
}

// UTF-8 to the game's text (Latin-1, or a language's own codes: GameChar)
std::string GameText(std::string_view s, uint32_t lang) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = uint8_t(s[i]);
        uint32_t cp = c, len = 1;
        if (c >= 0xF0) cp = c & 0x07, len = 4;
        else if (c >= 0xE0) cp = c & 0x0F, len = 3;
        else if (c >= 0xC0) cp = c & 0x1F, len = 2;
        for (uint32_t k = 1; k < len && i + k < s.size(); k++) cp = cp << 6 | (uint8_t(s[i + k]) & 0x3F);
        i += len;
        out += GameChar(cp, lang);
    }
    return out;
}
std::string Latin1(std::string_view s) { return GameText(s, 0); }
std::string Text(std::string_view s) { return GameText(s, g_lang); }

// --- Russian's and Polish's fonts ----------------------------------------------------------------------------------

struct FontPatch {
    uint64_t at;
    bool masked;  // 0xFF: the game's byte kept
    std::string bytes;
};
std::vector<FontPatch> g_fonts;

void LoadFonts() {
    const char* file = g_lang == kPolish ? "pl_fonts.bin" : "ru_fonts.bin";
    std::ifstream in(app::ExeDir() / "lang" / file, std::ios::binary);
    char magic[8] = {};
    uint64_t pak = 0;
    uint32_t count = 0;
    if (!in.read(magic, 8) || std::memcmp(magic, "FHRFONT1", 8) != 0 || !in.read(reinterpret_cast<char*>(&pak), 8) ||
        !in.read(reinterpret_cast<char*>(&count), 4)) {
        LOGE("language: lang/%s missing or not one: the language's own letters will not show", file);
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint64_t at = 0;
        uint32_t len = 0, kind = 0;
        if (!in.read(reinterpret_cast<char*>(&at), 8) || !in.read(reinterpret_cast<char*>(&len), 4) ||
            !in.read(reinterpret_cast<char*>(&kind), 4))
            break;
        FontPatch f{at, kind == 1, std::string(len, '\0')};
        if (!in.read(f.bytes.data(), len)) break;
        g_fonts.push_back(std::move(f));
    }
    std::sort(g_fonts.begin(), g_fonts.end(), [](const FontPatch& a, const FontPatch& b) { return a.at < b.at; });
    LOGR("language: %s, %zu patches", file, g_fonts.size());
}

void PatchFonts(uint64_t pos, uint8_t* data, uint32_t n) {
    auto it = std::upper_bound(g_fonts.begin(), g_fonts.end(), pos,
                               [](uint64_t p, const FontPatch& f) { return p < f.at; });
    if (it != g_fonts.begin()) --it;
    for (; it != g_fonts.end() && it->at < pos + n; ++it) {
        uint64_t a = std::max<uint64_t>(it->at, pos), e = std::min<uint64_t>(it->at + it->bytes.size(), pos + n);
        for (uint64_t k = a; k < e; k++) {
            uint8_t b = uint8_t(it->bytes[size_t(k - it->at)]);
            if (!it->masked || b != 0xFF) data[k - pos] = b;
        }
    }
}

void Load() {
    const char* file = g_lang == kRussian ? "ru.txt" : g_lang == kPolish ? "pl.txt" : "pt.txt";
    std::ifstream in(app::ExeDir() / "lang" / file, std::ios::binary);
    if (!in) {
        LOGE("language: lang/%s not found: the game's English text stays", file);
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
        std::string key = line.substr(0, tab), text = Text(std::string_view(line).substr(tab + 1));
        for (size_t at = text.find("\\n"); at != std::string::npos; at = text.find("\\n", at + 1)) text.replace(at, 2, "\n");
        if (key[0] == '@') g_lines[std::strtoull(key.c_str() + 1, nullptr, 16)] = std::move(text);
        else g_keys[key] = std::move(text);
    }
    LOGR("language: %s, %zu rows and %zu subtitles", g_lang == kRussian ? "Russian" : g_lang == kPolish ? "Polish" : "Portuguese", g_keys.size(),
         g_lines.size());
}

// --- the string tables ---------------------------------------------------------------------------------------------

// the row's field: quoted when it holds a comma, a quote or a line break
std::string Field(const std::string& text) {
    if (text.find_first_of(",\"\r\n") == std::string::npos) return text;
    std::string q = "\"";
    for (char c : text) q += c == '"' ? std::string("\"\"") : std::string(1, c);
    return q + "\"";
}

void PatchTables(char* data, size_t n) {
    static constexpr std::string_view kHeader = "language,English,German,French,Italian,Spanish";
    std::string_view view(data, n);
    for (size_t at = view.find(kHeader); at != std::string_view::npos; at = view.find(kHeader, at + 1)) {
        size_t p = view.find('\n', at);
        while (p != std::string_view::npos && p + 1 < n && data[p + 1] != '\0') {
            // a row ends at a line's end outside quotes (a quoted field may hold line breaks)
            size_t start = p + 1, end = start;
            for (bool quoted = false; end < n && data[end] != '\0'; end++) {
                if (data[end] == '"') quoted = !quoted;
                else if (!quoted && (data[end] == '\r' || data[end] == '\n')) break;
            }
            std::string_view row(data + start, end - start);
            size_t comma = row.find(',');
            if (comma != std::string_view::npos) {
                std::string key(row.substr(0, comma));
                auto it = g_keys.find(key);
                std::string text = it != g_keys.end() ? it->second : std::string();
                // the main menu's PC Settings (offline.cpp's row) and the keyboard's title prompt (UTF-8)
                const bool ru = g_lang == kRussian, pl = g_lang == kPolish;
                if (key == "$multiplayer")
                    text = Text(ru ? "\xD0\x9D\xD0\xB0\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD0\xBA\xD0\xB8 \xD0\x9F\xD0\x9A"
                                : pl ? "Opcje PC" : "Op\xC3\xA7\xC3\xB5" "es do PC");
                if (key == "$multiplayerCaps")
                    text = Text(ru ? "\xD0\x9D\xD0\x90\xD0\xA1\xD0\xA2\xD0\xA0\xD0\x9E\xD0\x99\xD0\x9A\xD0\x98 \xD0\x9F\xD0\x9A"
                                : pl ? "OPCJE PC" : "OP\xC3\x87\xC3\x95" "ES DO PC");
                if (key == "$pressStartXbox" && row.find("ENTER") != std::string_view::npos)
                    text = Text(ru ? "\xD0\x9D\xD0\xB0\xD0\xB6\xD0\xBC\xD0\xB8\xD1\x82\xD0\xB5 ENTER"
                                : pl ? "Naci\xC5\x9Bnij ENTER" : "Pressione ENTER");
                if (!text.empty()) {
                    std::string out = key + "," + Field(text) + ",,,,";
                    if (out.size() <= row.size()) {
                        out.resize(row.size(), ' ');
                        std::memcpy(data + start, out.data(), out.size());
                    } else {
                        LOGW("language: %s does not fit its row (%zu > %zu)", key.c_str(), out.size(), row.size());
                    }
                }
            }
            if (end >= n || data[end] == '\0') break;
            p = view.find('\n', end);
        }
    }
}

// --- the sound banks' subtitles ------------------------------------------------------------------------------------

struct Put {
    uint64_t at;
    std::string bytes;
};
std::mutex g_bank_lock;
std::unordered_map<std::string, std::vector<Put>> g_banks;  // a bank's Xbox path -> its lines' new bytes

std::vector<Put> Scan(const std::vector<uint8_t>& b) {
    constexpr size_t kSlot = 256;  // a language's line in a record, with its terminating zero
    std::vector<Put> out;
    size_t n = b.size();
    for (size_t i = 0; i < n;) {
        if (b[i] < 0x20 || (i && b[i - 1] != 0)) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && b[j] >= 0x20) j++;
        if (j < n && b[j] == 0 && j - i >= 2) {
            auto it = g_lines.find(Fnv(b.data() + i, j - i));
            if (it != g_lines.end()) {
                size_t z = j;
                while (z < n && b[z] == 0 && z - i < kSlot) z++;
                const std::string& t = it->second;
                if (t.size() + 1 <= z - i) {
                    std::string bytes = t;
                    bytes.resize(std::max(j - i, t.size() + 1), '\0');
                    out.push_back({i, std::move(bytes)});
                } else {
                    LOGW("language: a subtitle does not fit its slot (%zu > %zu)", t.size(), z - i - 1);
                }
            }
        }
        i = j;
    }
    return out;
}

void PatchBank(const std::string& path, uint64_t pos, uint8_t* data, uint32_t n) {
    std::lock_guard lock(g_bank_lock);
    auto it = g_banks.find(path);
    if (it == g_banks.end()) {
        std::vector<uint8_t> whole;
        std::vector<Put> puts;
        if (kernel::ReadGameFile(path, whole)) puts = Scan(whole);
        it = g_banks.emplace(path, std::move(puts)).first;
    }
    for (const Put& p : it->second) {
        uint64_t a = std::max<uint64_t>(p.at, pos), e = std::min<uint64_t>(p.at + p.bytes.size(), pos + n);
        if (a < e) std::memcpy(data + (a - pos), p.bytes.data() + (a - p.at), size_t(e - a));
    }
}

// --- the levels' text --------------------------------------------------------------------------------------------

// tests (XBE_LANG_DUMP=<file>): each text the string loader reads, "@<hash>\t<asset id>\t<text>" (Latin-1)
FILE* Dump() {
    static FILE* f = [] {
        const char* p = std::getenv("XBE_LANG_DUMP");
        return p ? std::fopen(p, "ab") : nullptr;
    }();
    return f;
}

// hints written again in every language, by their text's hash: the lean's named the d-pad in words ("the
// directional button"), read as the arrow keys on a keyboard; with the left and right picture ($lr$) the hint shows the
// lean keys (Q and E, prompts.cpp kInGame) or the controller's d-pad, no full stop after it (it read as part of the
// picture, "E."). UTF-8
const std::unordered_map<uint64_t, const char*>& Hints() {
    static const std::unordered_map<uint64_t, const char*> hints = {
        {0xc7575f0639300230ull, "Lean by holding $am$ and pressing $lr$"},
        {0x7540e4c29f9d37ecull, "Incl\xC3\xADnate manteniendo pulsado $am$ y pulsando $lr$<X>Incl\xC3\xADnate manteniendo presionado $am$ y presionando $lr$"},
        {0x840e41130e68cbc5ull, "Maintenez $am$ et appuyez sur $lr$ pour vous pencher."},
        {0x332fe3b9b6a62d67ull, "Sporgiti oltre gli angoli tenendo premuto $am$ e premendo $lr$"},
        {0x896dd949da0c7dd5ull, "Lehnen Sie sich, indem Sie $am$ gedr\xC3\xBC" "ckt halten und $lr$ dr\xC3\xBC" "cken."},
    };
    return hints;
}

}  // namespace

// The levels' and the pause screen's text (the objectives, the hints, the menus in play) are string assets in the
// levels' streams (chunk 0x719, value type 9), one per language; the game reads its own language's alone. Their loader
// (sub_00124C80, this, stream, header) allocates the text's length (the chunk's less its 0x1C header, and a zero) at
// this + 0x10 and reads it: a text the translation has is put in its place, its buffer made the translation's length
// (the game's realloc, sub_000E1140, in the loader's heap).
X86_EXTERN_FUNC(__imp__sub_00124C80);
X86_FUNC(sub_00124C80) {
    const uint32_t self = ctx.ecx, header = kernel::R32(ctx.esp + 8);
    __imp__sub_00124C80(ctx, base);
    FILE* dump = Dump();
    uint32_t text = kernel::R32(self + 0x10);
    if (!text) return;
    const char* s = reinterpret_cast<const char*>(kernel::HostPtr(text));
    size_t len = std::strlen(s);
    uint64_t hash = Fnv(reinterpret_cast<const uint8_t*>(s), len);
    if (dump && len) {
        std::fprintf(dump, "@%016llx\t%08x%08x\t%s\n", static_cast<unsigned long long>(hash), kernel::R32(header + 0x10),
                     kernel::R32(header + 0x14), s);
        std::fflush(dump);
    }
    std::string t;
    // (the translation's own first: the hints' are the game's languages')
    if (auto it = g_on ? g_lines.find(hash) : g_lines.end(); it != g_lines.end()) t = it->second;
    else if (auto h = Hints().find(hash); h != Hints().end()) t = Latin1(h->second);
    if (t.empty()) return;
    uint32_t saved_eax = ctx.eax;
    uint32_t grown = kernel::CallGuest(ctx, 0xE1140, {text, uint32_t(t.size() + 1)});
    ctx.eax = saved_eax;
    if (!grown) return;
    std::memcpy(kernel::HostPtr(grown), t.c_str(), t.size() + 1);
    kernel::W32(self + 0x10, grown);
}

namespace cod::lang {

bool Portuguese() { return g_on && g_lang == kPortuguese; }

void Init() {
    const uint32_t l = app::Get().language;
    g_on = l == kPortuguese || l == kRussian || l == kPolish;
    g_lang = g_on ? l : 0;
    if (g_on) Load();
    if (g_lang == kRussian || g_lang == kPolish) LoadFonts();
}

// a read of the game's files (offline.cpp's read hook)
void Patch(const std::string& path, uint64_t pos, uint8_t* data, uint32_t n) {
    if (!g_on || path.size() < 4) return;
    std::string low = path;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (low.size() >= 7 && low.compare(low.size() - 7, 7, "cod.pak") == 0) {
        PatchTables(reinterpret_cast<char*>(data), n);
        if (!g_fonts.empty()) PatchFonts(pos, data, n);
    }
    else if (low.find("sb_agd") != std::string::npos && low.find("\\english\\") != std::string::npos &&
             low.compare(low.size() - 4, 4, ".agd") == 0)
        PatchBank(path, pos, data, n);
}

}  // namespace cod::lang
