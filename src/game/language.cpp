// Portuguese (language 9 in the settings): the game has English, German, French, Italian and Spanish, not Portuguese.
// It runs in English (its speech, kGame.console_language) and its text is put in the English text's place as the
// game reads it, from lang/pt.txt beside the exe (the port's own translation; nothing of the game's is written):
// - the string tables in COD.PAK ($key,English,German,French,Italian,Spanish, Latin-1): a row the translation has
//   becomes $key,Portuguese with the other columns empty, in the row's own length (spaces after the last column do
//   not show);
// - the subtitles in the sound banks (SB_AGD\<range>\ENGLISH\*.agd): each record holds the five languages' lines,
//   each in a slot of its own (English's 256 bytes); the English line becomes its Portuguese, found by the English
//   line's hash (FNV-1a 64 of its Latin-1 bytes), as long as the slot holds it.
// lang/pt.txt: UTF-8 lines "$key<tab>text" and "@<hash in hex><tab>text" (\n a line break); # starts a comment.
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

bool g_on = false;
std::unordered_map<std::string, std::string> g_keys;  // a string table's key -> its text (Latin-1)
std::unordered_map<uint64_t, std::string> g_lines;    // a subtitle's English line (its hash) -> its text (Latin-1)

uint64_t Fnv(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// UTF-8 to Latin-1 (the game's text): what Latin-1 lacks becomes '?'
std::string Latin1(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = uint8_t(s[i]);
        uint32_t cp = c, len = 1;
        if (c >= 0xF0) cp = c & 0x07, len = 4;
        else if (c >= 0xE0) cp = c & 0x0F, len = 3;
        else if (c >= 0xC0) cp = c & 0x1F, len = 2;
        for (uint32_t k = 1; k < len && i + k < s.size(); k++) cp = cp << 6 | (uint8_t(s[i + k]) & 0x3F);
        i += len;
        out += cp <= 0xFF ? char(cp) : '?';
    }
    return out;
}

void Load() {
    std::ifstream in(app::ExeDir() / "lang" / "pt.txt", std::ios::binary);
    if (!in) {
        LOGE("language: lang/pt.txt not found: the game's English text stays");
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
        std::string key = line.substr(0, tab), text = Latin1(std::string_view(line).substr(tab + 1));
        for (size_t at = text.find("\\n"); at != std::string::npos; at = text.find("\\n", at + 1)) text.replace(at, 2, "\n");
        if (key[0] == '@') g_lines[std::strtoull(key.c_str() + 1, nullptr, 16)] = std::move(text);
        else g_keys[key] = std::move(text);
    }
    LOGR("language: Portuguese, %zu rows and %zu subtitles", g_keys.size(), g_lines.size());
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
                // the main menu's PC Settings (offline.cpp's row) and the keyboard's title prompt
                if (key == "$multiplayer") text = Latin1("Op\xC3\xA7\xC3\xB5" "es do PC");
                if (key == "$multiplayerCaps") text = Latin1("OP\xC3\x87\xC3\x95" "ES DO PC");
                if (key == "$pressStartXbox" && row.find("ENTER") != std::string_view::npos) text = "Pressione ENTER";
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
// lean keys (Q and E, prompts.cpp kInGame) or the controller's d-pad. UTF-8
const std::unordered_map<uint64_t, const char*>& Hints() {
    static const std::unordered_map<uint64_t, const char*> hints = {
        {0xc7575f0639300230ull, "Lean by holding $am$ and pressing $lr$."},
        {0x7540e4c29f9d37ecull, "Incl\xC3\xADnate manteniendo pulsado $am$ y pulsando $lr$.<X>Incl\xC3\xADnate manteniendo presionado $am$ y presionando $lr$."},
        {0x840e41130e68cbc5ull, "Maintenez $am$ et appuyez sur $lr$ pour vous pencher."},
        {0x332fe3b9b6a62d67ull, "Sporgiti oltre gli angoli tenendo premuto $am$ e premendo $lr$."},
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
    if (auto h = Hints().find(hash); h != Hints().end()) t = Latin1(h->second);
    else if (auto it = g_on ? g_lines.find(hash) : g_lines.end(); it != g_lines.end()) t = it->second;
    if (t.empty()) return;
    uint32_t saved_eax = ctx.eax;
    uint32_t grown = kernel::CallGuest(ctx, 0xE1140, {text, uint32_t(t.size() + 1)});
    ctx.eax = saved_eax;
    if (!grown) return;
    std::memcpy(kernel::HostPtr(grown), t.c_str(), t.size() + 1);
    kernel::W32(self + 0x10, grown);
}

namespace cod::lang {

bool Portuguese() { return g_on; }

void Init() {
    g_on = app::Get().language == kPortuguese;
    if (g_on) Load();
}

// a read of the game's files (offline.cpp's read hook)
void Patch(const std::string& path, uint64_t pos, uint8_t* data, uint32_t n) {
    if (!g_on || path.size() < 4) return;
    std::string low = path;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (low.size() >= 7 && low.compare(low.size() - 7, 7, "cod.pak") == 0) PatchTables(reinterpret_cast<char*>(data), n);
    else if (low.find("sb_agd") != std::string::npos && low.find("\\english\\") != std::string::npos &&
             low.compare(low.size() - 4, 4, ".agd") == 0)
        PatchBank(path, pos, data, n);
}

}  // namespace cod::lang
