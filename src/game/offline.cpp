// Offline: multiplayer is Xbox Live and System Link only, in an executable of its own (Game_XboxMultiplayer.xbe)
// that this port does not include. The main menu's Multiplayer becomes PC Settings: its label in the string table
// (COD.PAK's CSV of $key,English,German,French,Italian,Spanish) is rewritten as the game reads it, and choosing it
// opens the settings overlay.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

#include "common/cod.h"
#include "common/overlay.h"
#include "core/input.h"
#include "core/control.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {
constexpr uint32_t kMultiplayerMessage = 0x60C;  // the front end's id of urlMultiplayer

struct Row {
    std::string_view key;  // with its comma
    const char* text[5];   // English, German, French, Italian, Spanish
};
constexpr Row kRows[] = {
    {"\n$multiplayer,", {"PC Settings", "PC-Einstellungen", "Options PC", "Opzioni PC", "Opciones de PC"}},
    {"\n$multiplayerCaps,", {"PC SETTINGS", "PC-OPTIONEN", "OPTIONS PC", "OPZIONI PC", "OPCIONES DE PC"}},
    // the title screen's, for the keyboard (Latin-1 as the table)
    {"\n$pressStartXbox,", {"Press ENTER", "ENTER dr\xFC" "cken", "Appuyez sur ENTR\xC9" "E", "Premi INVIO", "Pulsa INTRO"}},
};
bool g_keyboard_title = false;  // the title screen asks for the keyboard's key (decided when the table is read)

// the row's new text in the length of the old: each language keeps its field's centring (the menu's text is left
// aligned and the table pads it with spaces), shifted by the difference in letters
bool Rewrite(char* line, size_t len, const Row& row) {
    std::string_view old(line, len);
    std::string out(row.key.substr(1));
    size_t at = out.size();
    std::string spaces[5], texts[5];
    for (int i = 0; i < 5; i++) {
        size_t end = i < 4 ? old.find(',', at) : old.size();
        if (end == std::string_view::npos) return false;
        std::string_view field = old.substr(at, end - at);
        size_t lead = field.find_first_not_of(' ');
        if (lead == std::string_view::npos) lead = field.size();
        int keep = int(lead) + int(field.size() - lead) - int(std::strlen(row.text[i]));
        spaces[i] = std::string(size_t(std::max(0, keep)), ' ');
        texts[i] = row.text[i];
        at = end + 1;
    }
    auto build = [&] {
        std::string s(row.key.substr(1));
        for (int i = 0; i < 5; i++) s += spaces[i] + texts[i] + (i < 4 ? "," : "");
        return s;
    };
    out = build();
    // too long: the widest padding goes first
    while (out.size() > len) {
        auto widest = std::max_element(spaces, spaces + 5, [](auto& a, auto& b) { return a.size() < b.size(); });
        if (widest->empty()) return false;
        widest->pop_back();
        out = build();
    }
    out.resize(len, ' ');  // trailing spaces after the last language do not show
    std::copy(out.begin(), out.end(), line);
    return true;
}

void PatchStrings(const std::string& path, uint64_t pos, uint8_t* data, uint32_t n) {
    struct Then {  // Portuguese after this file's rows (language.cpp)
        const std::string& path;
        uint64_t pos;
        uint8_t* data;
        uint32_t n;
        ~Then() { cod::lang::Patch(path, pos, data, n); }
    } then{path, pos, data, n};
    if (path.size() < 7) return;
    std::string tail = path.substr(path.size() - 7);
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (tail != "cod.pak") return;
    std::string_view view(reinterpret_cast<char*>(data), n);
    for (const Row& row : kRows) {
        if (row.key == kRows[2].key && !g_keyboard_title) continue;
        size_t at = view.find(row.key);
        if (at == std::string_view::npos) continue;
        size_t start = at + 1, end = view.find('\r', start);
        if (end == std::string_view::npos) continue;
        bool ok = Rewrite(reinterpret_cast<char*>(data) + start, end - start, row);
        LOGR("offline: %.*s %s", int(row.key.size() - 2), row.key.data() + 1, ok ? "rewritten" : "kept");
    }
}

}  // namespace

namespace cod::offline {
void Init() {
    // the keyboard's prompts if they are chosen, or chosen by the device and no controller is plugged in
    const Settings& s = Get();
    g_keyboard_title = s.button_prompts == 2 || (s.button_prompts == 0 && s.keyboard_mouse && !input::AnyController());
    cod::lang::Init();
    kernel::SetReadHook(PatchStrings);
}
}  // namespace cod::offline

// tests: a level index forced whenever the game sets the current level (the next level started is that one)
int g_level_request = -1;

X86_EXTERN_FUNC(__imp__sub_00129250);
X86_FUNC(sub_00129250) {  // SetCurrentLevel(index)
    if (g_level_request >= 0) {
        LOGR("offline: level %d replaced by %d", int32_t(R32(ctx.esp + 4)), g_level_request);
        W32(ctx.esp + 4, uint32_t(g_level_request));
    }
    gpu::g_level = int32_t(R32(ctx.esp + 4));
    if (gpu::g_level >= 0) control::Anchor();
    __imp__sub_00129250(ctx, base);
}

// the front end's message handler (thiscall, the message's id in the argument)
X86_EXTERN_FUNC(__imp__sub_000B0830);
X86_FUNC(sub_000B0830) {
    uint32_t self = ctx.ecx, message = R32(ctx.esp + 4);
    if (self && message && message == R32(self + kMultiplayerMessage)) {
        LOGR("offline: PC Settings chosen");
        cod::overlay::Toggle();
        ctx.eax = 0;
        ctx.esp += 8;  // ret 4
        return;
    }
    __imp__sub_000B0830(ctx, base);
}
