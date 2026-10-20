// The mouse in the game's own menus. Every 2D draw of a menu comes by (gpu::g_seen_2d). The menus draw each item's
// text in the item font in one colour, then the selected item's once more in another: so the items are the boxes in
// the common colour, the selected one the box drawn again. Pointing at an item moves the selection there (up and down
// presses, one at a time, until the menu shows it selected), a click accepts it, or on an option's value changes it;
// the wheel moves and the right button goes back (pc_input). A brass pointer is drawn while the mouse is in use.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "common/cod.h"
#include "common/prompts.h"
#include "common/widescreen.h"
#include "core/input.h"
#include "core/log.h"
#include "common/overlay.h"
#include "common/overlay_text.h"
#include "gpu/backend.h"
#include "xdk/dsound.h"

namespace cod::menu_mouse {
namespace {

constexpr uint64_t kItemFont = 0x291c6e09acfaebb9;  // fbld24: the menus' items
constexpr uint64_t kStrip = 0xcb8517244f0b8da6;     // fend_394: the options' selection strip
constexpr uint64_t kPauseFont = 0x8222729448b2f5f1;  // the HUD's font: the pause screen's list
constexpr uint64_t kTypeFont = 0xece9a6f491a366bc;   // the typewriter: the mission lists, the prompts

// tests: a pointer at console coordinates until a time, and a click
float g_test_x = 0, g_test_y = 0;
double g_test_until = -1;
bool g_test_click = false;

std::mutex g_lock;
std::vector<gpu::Draw2DSeen> g_frame, g_last;
uint64_t g_frame_id = ~0ull;

void Seen(const gpu::Draw2DSeen& d) {
    std::lock_guard l(g_lock);
    if (d.frame != g_frame_id) {
        g_last.swap(g_frame);
        g_frame.clear();
        g_frame_id = d.frame;
    }
    if (g_frame.size() < 2048) g_frame.push_back(d);
}

struct Install {
    Install() { gpu::g_seen_2d = Seen; }  // and g_recolor_2d, below
} g_install;

struct Box {
    float x0, y0, x1, y1;
};
struct Menu {
    std::vector<Box> items;  // top to bottom
    int selected = -1;
    float right = 0;         // the rows reach this far (the strip's end, or the widest item)
};

bool Same(const gpu::Draw2DSeen& a, const gpu::Draw2DSeen& b) {
    return std::fabs(a.x0 - b.x0) < 1 && std::fabs(a.y0 - b.y0) < 1 && std::fabs(a.x1 - b.x1) < 1 && std::fabs(a.y1 - b.y1) < 1;
}

// The pause screen's list, in the HUD's font: its items centred, grey, the selected one white and larger (the title
// above them white at the items' size); in a widened game its text arrives narrowed about the left (hud.cpp Layout)
Menu ReadPause(const std::vector<gpu::Draw2DSeen>& f) {
    Menu m;
    float widen = std::max(1.0f, gpu::g_screen_widen.load());
    std::vector<gpu::Draw2DSeen> grey, white;
    for (gpu::Draw2DSeen d : f) {
        if (d.tex != kPauseFont || (d.tag & 0xFF) != 2) continue;
        d.x0 *= widen, d.x1 *= widen;
        if (std::fabs((d.x0 + d.x1) * 0.5f - 320) > 40) continue;  // the prompts' row, the objectives
        auto& list = (d.color & 0xFFFFFF) == 0xFFFFFF ? white : grey;
        if (std::none_of(list.begin(), list.end(), [&](auto& o) { return Same(o, d); })) list.push_back(d);
    }
    if (grey.empty()) return m;
    float h = 0;
    for (const auto& d : grey) h = std::max(h, d.y1 - d.y0);
    std::vector<gpu::Draw2DSeen> items = grey;
    const gpu::Draw2DSeen* sel = nullptr;
    for (const auto& d : white)
        if (d.y1 - d.y0 > h + 3) items.push_back(d), sel = &d;
    std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.y0 < b.y0; });
    for (size_t i = 0; i < items.size(); i++) {
        m.items.push_back({items[i].x0, items[i].y0, items[i].x1, items[i].y1});
        m.right = std::max(m.right, items[i].x1);
        if (sel && Same(items[i], *sel)) m.selected = int(i);
    }
    return m;
}

// The campaigns' mission lists (a notebook of typewritten lines, the front's name above, the prompts below): one
// column of lines from the same left edge, the selected one marked by a small picture before it (its colour says
// only whether the mission was played)
Menu ReadNotebook(const std::vector<gpu::Draw2DSeen>& f) {
    Menu m;
    std::vector<gpu::Draw2DSeen> lines;
    for (const auto& d : f)
        if (d.tex == kTypeFont && (d.tag & 0xFF) == 2 && d.y0 > 85 && d.y1 < 390 &&
            std::none_of(lines.begin(), lines.end(), [&](auto& o) { return Same(o, d); }))
            lines.push_back(d);
    if (lines.size() < 1) return m;
    // the column: the left edge most lines share
    std::map<int, int> lefts;
    for (const auto& d : lines) lefts[int(d.x0 / 6)]++;
    int left = std::max_element(lefts.begin(), lefts.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
    std::vector<gpu::Draw2DSeen> items;
    for (const auto& d : lines)
        if (std::abs(int(d.x0 / 6) - left) <= 1) items.push_back(d);
    std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.y0 < b.y0; });
    for (const auto& d : items) {
        m.items.push_back({d.x0, d.y0, d.x1, d.y1});
        m.right = std::max(m.right, d.x1);
    }
    for (const auto& d : f) {
        if (d.tex == kTypeFont || !d.tex || (d.tag & 0xFF) != 2 || d.x1 - d.x0 > 40 || d.y1 - d.y0 > 40) continue;
        float cy = (d.y0 + d.y1) * 0.5f;
        for (size_t i = 0; i < m.items.size(); i++)
            if (d.x1 <= m.items[i].x0 + 6 && d.x1 > m.items[i].x0 - 80 && cy > m.items[i].y0 - 2 && cy < m.items[i].y1 + 2)
                m.selected = int(i);
    }
    if (m.selected < 0) m.items.clear();  // a page of text, not a list: a click goes on as the button would
    return m;
}

Menu Read() {
    std::vector<gpu::Draw2DSeen> f;
    {
        std::lock_guard l(g_lock);
        f = g_last;
    }
    if (cod::Paused() && !cod::wide::FrontEnd()) return ReadPause(f);
    Menu m;
    std::map<uint32_t, int> colours;
    for (const auto& d : f)
        if (d.tex == kItemFont) colours[d.color]++;
    if (colours.empty()) return ReadNotebook(f);
    uint32_t common = std::max_element(colours.begin(), colours.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
    if (colours[common] < 2) return m;  // a screen of text, not a list
    std::vector<gpu::Draw2DSeen> items;
    for (const auto& d : f)
        if (d.tex == kItemFont && d.color == common && std::none_of(items.begin(), items.end(), [&](auto& o) { return Same(o, d); }))
            items.push_back(d);
    std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.y0 < b.y0; });
    for (const auto& d : items) {
        m.items.push_back({d.x0, d.y0, d.x1, d.y1});
        m.right = std::max(m.right, d.x1);
    }
    for (const auto& d : f) {
        if (d.tex == kStrip) m.right = std::max(m.right, d.x1);
        if (d.tex != kItemFont || d.color == common) continue;
        for (size_t i = 0; i < items.size(); i++)
            if (Same(items[i], d)) m.selected = int(i);
    }
    return m;
}

// the console's 640 by 480 on the window: a widened screen's 4:3 middle at the window's height; else the frame as
// it is shown (d3d12.cpp's BlitFrame), 4:3 (or 16:9 when the console reports a wide display) in the window's middle,
// bars beside it on a wider window (a 4:3 resolution on a wide screen)
void Frame(float W, float H, float& sx, float& sy, float& left, float& top) {
    if (cod::Get().widescreen && gpu::g_screen_widen.load() > 1.0f) {
        sx = sy = H / 480.0f, left = (W - 640 * sx) / 2, top = 0;
        return;
    }
    float aspect = app::Widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
    float vw = std::min(W, H * aspect), vh = vw / aspect;
    sx = vw / 640.0f, sy = vh / 480.0f, left = (W - vw) / 2, top = (H - vh) / 2;
}

// the mouse in the console's 640 by 480 of a menu, from the window's client area
bool MousePos(float& x, float& y, float& scale, float& left, float& top) {
    HWND w = static_cast<HWND>(app::Window());
    float tv[4];
    bool in_tv = gpu::TvScreen(tv);
    if (ImGui::GetTime() < g_test_until && w) {
        RECT r;
        GetClientRect(w, &r);
        float W = float(r.right), H = float(r.bottom), sx;
        Frame(W, H, sx, scale, left, top);
        if (in_tv) scale = (tv[3] - tv[1]) / 480.0f, left = tv[0], top = tv[1];
        x = g_test_x, y = g_test_y;
        return true;
    }
    POINT p;
    RECT r;
    if (!w || !GetCursorPos(&p) || !ScreenToClient(w, &p) || !GetClientRect(w, &r)) return false;
    float W = float(r.right), H = float(r.bottom);
    if (W <= 0 || H <= 0) return false;
    float sx, sy;
    Frame(W, H, sx, sy, left, top);
    if (in_tv) sx = sy = (tv[3] - tv[1]) / 480.0f, left = tv[0], top = tv[1];
    x = (float(p.x) - left) / sx;
    y = (float(p.y) - top) / sy;
    scale = sy;
    return p.x >= 0 && p.y >= 0 && p.x < r.right && p.y < r.bottom;
}


// --- Quit Game: a fifth row under the main menu's four, the port's own --------------------------------------------
// The main menu is a Maestro scene of the game's data (its rows, their states and animations); the row is drawn over
// it in the menu's typeface, colours and spacing. Down from the last row moves onto it (the game's input held back
// meanwhile, and its own highlight drawn in the plain colour: one selection), up or back leaves it; accepting it asks
// to quit in the pause screen's style, Yes closes the game. The mouse points and clicks at it like at the others.
std::atomic<bool> g_main{false};       // the main menu is up (its four rows where they are)
std::atomic<int> g_main_sel{-1};       // the game's selected row
std::atomic<bool> g_quit_focus{false}; // the row has the selection
std::atomic<bool> g_dialog{false};     // asking
std::atomic<int> g_yes{0};             // the answer selected: 0 no, 1 yes
std::atomic<bool> g_swallow{false};    // the game sees no input until everything is let go
Box g_quit_row{}, g_dialog_rows[2]{};  // console units (640 x 480)
std::mutex g_quit_lock;

uint32_t Language() {
    static const uint32_t boot = app::Get().language;  // the game's, read once at boot
    return boot;
}
const char* Text(int which) {
    // 0 the row, 1 the question, 2 no, 3 yes; UTF-8
    static const char* en[] = {"Quit Game", "Quit to Windows?", "No", "Yes"};
    static const char* fr[] = {"Quitter le jeu", "Quitter vers Windows ?", "Non", "Oui"};
    static const char* it[] = {"Esci dal gioco", "Uscire a Windows?", "No", "S\xC3\xAC"};
    static const char* es[] = {"Salir del juego", "\xC2\xBFSalir a Windows?", "No", "S\xC3\xAD"};
    static const char* pt[] = {"Sair do jogo", "Sair para o Windows?", "N\xC3\xA3o", "Sim"};
    uint32_t l = Language();
    const char** t = l == 4 ? fr : l == 6 ? it : l == 5 ? es : l == 9 ? pt : en;
    return t[which];
}

// the main menu: the front end's list of four rows at the title screen's places
bool MainMenu(const Menu& m) {
    if (!cod::wide::FrontEnd() || cod::InMovie() || m.items.size() != 4) return false;
    const float ys[4] = {201, 238, 276, 314};
    for (int i = 0; i < 4; i++)
        if (std::fabs(m.items[size_t(i)].y0 - ys[i]) > 4) return false;
    return true;
}

// the menu's 640 x 480 on the window (as MousePos)
bool Mapping(float& scale, float& left, float& top) {
    HWND w = static_cast<HWND>(app::Window());
    RECT r;
    if (!w || !GetClientRect(w, &r) || r.right <= 0 || r.bottom <= 0) return false;
    float W = float(r.right), H = float(r.bottom);
    float tv[4], sx;
    Frame(W, H, sx, scale, left, top);
    if (gpu::TvScreen(tv)) scale = (tv[3] - tv[1]) / 480.0f, left = tv[0], top = tv[1];
    return true;
}

bool In(const Box& b, float x, float y, float pad) { return x >= b.x0 - pad && x < b.x1 + pad && y >= b.y0 - 2 && y < b.y1 + 2; }

// --- PC Options: a button under the pause screen's options page, the port's own ------------------------------------
// The page's frame (its outer top at 66..574 x 87) has room under it, above the prompts: a button in the frame's style
// and the pause screen's typeface. Down from the page's last row moves onto it (the game's input held back, its
// highlight drawn plain), up or back leaves it; accepting it (or a click) opens the PC settings.
std::atomic<bool> g_popts{false};      // the page is up
std::atomic<int> g_popts_sel{-1}, g_popts_count{0};
std::atomic<bool> g_pc_focus{false};
std::atomic<bool> g_pc_open{false};    // the settings asked for: opened on the render thread
Box g_pc_button{};
std::mutex g_pc_lock;

// the game's highlight hidden while the row or the question has the selection
bool Recolor(const gpu::Draw2DSeen& d, uint32_t& argb) {
    if (g_popts && g_pc_focus && d.tex == kPauseFont && (d.color & 0xFFFFFF) == 0xFFFFFF && d.y0 > 130 && d.y1 < 350) {
        argb = 0xFFC8C8C8;
        return true;
    }
    if (!g_main || !(g_quit_focus || g_dialog) || d.tex != kItemFont || (d.color & 0xFFFFFF) != 0xCCCCCC) return false;
    argb = 0xFF999999;
    return true;
}

int g_target = -1, g_action = 0;  // where the selection goes, then: 1 accept, 2 next value
double g_last_press = 0;
int g_pressed_from = -1;
POINT g_last_cursor{-1, -1};
double g_mouse_time = -100;
bool g_left_was = false;

}  // namespace

// The Quit row taking the selection or giving it back sounds as the game's rows do: the game's move sound, as it last
// played it (the row is the port's: the game has nothing to play for it)
void QuitSound() {
    static bool was = false;
    const bool now = g_main && g_quit_focus;
    if (now != was && g_main && !g_dialog) xdk::dsound::ReplayRemembered();
    was = now;
}

void Update() {
    QuitSound();
    const bool menu = (cod::wide::FrontEnd() || cod::Paused()) && !cod::InMovie();
    HWND w = static_cast<HWND>(app::Window());
    bool testing = ImGui::GetTime() < g_test_until;
    bool focus = w && (GetForegroundWindow() == w || testing);
    double now = ImGui::GetTime();
    {
        // the pointer shows while the mouse moves, in the settings too
        POINT c;
        GetCursorPos(&c);
        if (c.x != g_last_cursor.x || c.y != g_last_cursor.y) {
            if (g_last_cursor.x != -1 && focus) g_mouse_time = now, cod::prompts::KeyboardUsed();
            g_last_cursor = c;
        }
    }
    {
        static bool installed = (gpu::g_recolor_2d = Recolor, true);
        (void)installed;
        Menu mm = Read();
        bool main = MainMenu(mm) && !cod::OverlayOpen();
        if (!main) g_quit_focus = false, g_dialog = false;
        static const bool trace = std::getenv("XBE_MENU_TRACE") != nullptr;
        if (trace && main != g_main.load()) {
            LOGR("menu: main %d, %zu rows", int(main), mm.items.size());
            for (const Box& i : mm.items) LOGR("menu:   row %.0f..%.0f x %.0f..%.0f", i.x0, i.x1, i.y0, i.y1);
        }
        g_main = main;
        g_main_sel = mm.selected;
        // the pause screen's options page: its frame's outer top, its bottom. The frame is centred and as wide as
        // its longest row in the language (66..574 in Spanish, 111..530 in English)
        bool popts = false;
        float bottom = 0;
        if (cod::Paused() && !cod::wide::FrontEnd() && !cod::InMovie()) {
            std::lock_guard l(g_lock);
            float fx0 = 0, fx1 = 0;
            for (const auto& d : g_last)
                if (!d.tex && std::fabs(d.y0 - 87) < 3 && std::fabs(d.x0 + d.x1 - 640) < 4 && d.x1 - d.x0 > 300 &&
                    d.x1 - d.x0 > fx1 - fx0)
                    popts = true, fx0 = d.x0, fx1 = d.x1;  // the outermost
            if (popts)
                for (const auto& d : g_last)
                    if (!d.tex && std::fabs(d.x0 - fx0) < 3 && std::fabs(d.x1 - fx1) < 3 && d.y0 > 300) bottom = std::max(bottom, d.y1);
        }
        popts = popts && bottom > 300 && bottom < 380 && !mm.items.empty();
        if (!popts) g_pc_focus = false;
        g_popts = popts;
        g_popts_sel = mm.selected;
        g_popts_count = int(mm.items.size());
        {
            std::lock_guard l(g_pc_lock);
            g_pc_button = {320 - 110, bottom + 8, 320 + 110, bottom + 46};
        }
        if (g_pc_open.exchange(false) && !cod::OverlayOpen()) cod::overlay::Toggle();
        if (main) {
            // the fifth row: the rows' step below the last, as tall, centred on them
            std::lock_guard l(g_quit_lock);
            const Box& a = mm.items[2];
            const Box& b = mm.items[3];
            // the game centres its rows' text on the screen's middle (their boxes reach past the letters unevenly)
            const float cx = 320;
            float half = (b.x1 - b.x0) * 0.5f * 1.5f;
            g_quit_row = {cx - half, b.y0 + (b.y0 - a.y0), cx + half, b.y1 + (b.y0 - a.y0)};
        }
    }
    if (!menu || cod::OverlayOpen() || !cod::Get().keyboard_mouse || !focus) {
        g_target = -1, g_action = 0;
        return;
    }
    if (g_popts) {
        float mx, my, scale, x0, y0;
        bool inside = MousePos(mx, my, scale, x0, y0);
        bool left_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || g_test_click;
        static bool was = false;
        bool clicked = left_down && !was;
        if (g_test_click && was) g_test_click = false;
        was = left_down;
        bool moved = now - g_mouse_time < 0.05 || testing;
        Box b;
        {
            std::lock_guard l(g_pc_lock);
            b = g_pc_button;
        }
        if (inside && In(b, mx, my, 4)) {
            if (moved) g_pc_focus = true;
            if (clicked) g_pc_open = true, g_swallow = true;
            g_target = -1, g_action = 0;
            g_left_was = left_down;
            return;
        }
        if (g_pc_focus && moved && inside) g_pc_focus = false;  // pointing elsewhere: the game's rows again
    }
    if (g_main) {
        float mx, my, scale, x0, y0;
        bool inside = MousePos(mx, my, scale, x0, y0);
        bool left_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || g_test_click;
        static bool was = false;
        bool clicked = left_down && !was;
        if (g_test_click && was) g_test_click = false;
        was = left_down;
        bool moved = now - g_mouse_time < 0.05 || testing;
        Box row, rows[2];
        {
            std::lock_guard l(g_quit_lock);
            row = g_quit_row, rows[0] = g_dialog_rows[0], rows[1] = g_dialog_rows[1];
        }
        if (g_dialog) {
            for (int i = 0; i < 2; i++)
                if (inside && In(rows[i], mx, my, 20)) {
                    if (moved) g_yes = i;
                    if (clicked) {
                        if (i == 1) PostMessageW(w, WM_CLOSE, 0, 0);
                        g_dialog = false, g_swallow = true;
                    }
                }
            g_target = -1, g_action = 0;
            return;
        }
        if (inside && In(row, mx, my, 12)) {
            if (moved) g_quit_focus = true;
            if (clicked) g_dialog = true, g_yes = 0, g_quit_focus = true;
            g_target = -1, g_action = 0;
            g_left_was = left_down;
            return;
        }
        if (g_quit_focus && moved && inside) g_quit_focus = false;  // pointing elsewhere: the game's rows again
    }
    if (testing) g_mouse_time = now;
    bool left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || g_test_click;
    if (g_test_click && g_left_was) g_test_click = false, left = false;
    bool click = left && !g_left_was;
    g_left_was = left;
    if (click) g_mouse_time = now;

    Menu m = Read();
    float mx, my, scale, x0, y0;
    bool inside = MousePos(mx, my, scale, x0, y0);
    int hover = -1;
    bool on_value = false;
    if (inside && !m.items.empty()) {
        float l = m.items[0].x0;
        for (const Box& b : m.items) l = std::min(l, b.x0);
        for (size_t i = 0; i < m.items.size(); i++) {
            const Box& b = m.items[i];
            // the row: its own text's height, from the leftmost item to the rows' end
            float lo = std::min(b.x0, l) - 12, hi = std::max(b.x1, m.right) + 12;
            if (my >= b.y0 - 1 && my < b.y1 + 1 && mx >= lo && mx < hi) {
                hover = int(i);
                on_value = mx > b.x1 + 16 && m.right > b.x1 + 40;
            }
        }
    }
    bool recent = now - g_mouse_time < 0.05;
    if (hover >= 0 && recent && m.selected >= 0) g_target = hover;
    if (click && hover >= 0) g_target = hover, g_action = on_value ? 2 : 1;

    // one press at a time, the next once the menu showed the last (or a quarter second went by)
    if (m.selected >= 0 && g_target >= 0 && now - g_last_press > 0.06 &&
        (m.selected != g_pressed_from || now - g_last_press > 0.25)) {
        if (g_target != m.selected) {
            input::PressButtons(g_target > m.selected ? 0x2 : 0x1, 40);
            if (g_main) xdk::dsound::RememberNextSound();
            g_pressed_from = m.selected;
            g_last_press = now;
        } else {
            if (g_action == 1) input::Press(0, 60);
            if (g_action == 2) input::PressButtons(0x8, 60);
            if (g_action) g_last_press = now, g_pressed_from = -1;
            g_action = 0;
            g_target = -1;
        }
    }
    // a click outside any list (a screen of text, a picture): accept as the button would
    if (click && hover < 0 && m.items.empty() && inside) input::Press(0, 60);
}

void DrawQuit() {
    gpu::g_tv_ui = nullptr;
    if (!g_main) return;
    float scale, left, top;
    ImFont* font = cod::overlay::MenuFont();
    if (!font || !Mapping(scale, left, top)) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // in the television: drawn in the picture's own layer (gpu::g_tv_ui), the tube's curve, scan lines and softness
    // over it as over the game's rows
    float tv[4];
    const bool in_tv = gpu::TvScreen(tv);
    if (in_tv) {
        static ImDrawList list(ImGui::GetDrawListSharedData());
        list._ResetForNewFrame();
        list.PushClipRect(ImVec2(0, 0), ImVec2(float(gpu::kTvUiW), float(gpu::kTvUiH)));
        list.PushTextureID(font->ContainerAtlas->TexID);
        dl = &list;
        scale = gpu::kTvUiH / 480.0f, left = 0, top = 0;
    }
    auto at = [&](float x, float y) { return ImVec2(left + x * scale, top + y * scale); };
    Box row;
    {
        std::lock_guard l(g_quit_lock);
        row = g_quit_row;
    }
    // the menu's text: its rows' cap height fills about two thirds of their box
    float px = (row.y1 - row.y0) * 1.02f * scale;
    // the game's menu font sets its letters further apart: the spacing that makes "Extras" as wide as the game's
    // own (its letters span 68 console units at the rows' size, its box more)
    auto spaced = [&](const char* t, float size, float track, ImVec2 p, ImU32 col, bool draw) {
        float x = p.x;
        for (const char* c = t; *c;) {
            const char* next = c + 1;
            while (*next && (*next & 0xC0) == 0x80) next++;  // a whole UTF-8 character
            if (draw) dl->AddText(font, size, ImVec2(x, p.y), col, c, next);
            x += font->CalcTextSizeA(size, FLT_MAX, 0, c, next).x + (*next ? track : 0);
            c = next;
        }
        return x - p.x;
    };
    const float track = std::max(0.0f, (68 * scale - spaced("Extras", px, 0, ImVec2(0, 0), 0, false)) / 5);
    auto centred = [&](const char* t, float cx, float y, float size, ImU32 col) {
        float tr = track * size / px;
        float w = spaced(t, size, tr, ImVec2(0, 0), 0, false);
        ImVec2 p = at(cx, y);
        p.x -= w * 0.5f;
        // the dark outline the game's rows have (its font atlas's: about a console pixel and a fifth)
        const float r = 1.2f * scale * size / px;
        for (int k = 0; k < 12; k++) {
            float a = k * 0.5235988f;
            spaced(t, size, tr, ImVec2(p.x + std::cos(a) * r, p.y + std::sin(a) * r), IM_COL32(0x28, 0x2A, 0x32, 150), true);
        }
        spaced(t, size, tr, p, col, true);
    };
    float cx = (row.x0 + row.x1) * 0.5f;
    bool lit = g_quit_focus || g_dialog;
    centred(Text(0), cx, row.y0 - 1, px, lit ? IM_COL32(0xCC, 0xCC, 0xCC, 255) : IM_COL32(0x99, 0x99, 0x99, 255));
    if (!g_dialog) {
        if (in_tv) gpu::g_tv_ui = dl;
        return;
    }
    // the question, as the pause screen asks: a black box in a white frame, the answers under a rule
    ImVec2 lo = at(160, 170), hi = at(480, 330);
    dl->AddRectFilled(at(0, 0), at(640, 480), IM_COL32(0, 0, 0, 120));
    dl->AddRectFilled(lo, hi, IM_COL32(0, 0, 0, 235));
    float t = std::max(1.0f, 2.0f * scale);
    dl->AddRect(at(152, 162), at(488, 338), IM_COL32(255, 255, 255, 255), 0, 0, t * 1.5f);
    dl->AddRect(lo, hi, IM_COL32(255, 255, 255, 255), 0, 0, t);
    dl->AddLine(at(160, 230), at(480, 230), IM_COL32(255, 255, 255, 255), t);
    centred(Text(1), 320, 184, 26 * scale, IM_COL32(255, 255, 255, 255));
    for (int i = 0; i < 2; i++) {
        bool sel = g_yes == i;
        float y = 246 + i * 34;
        float size = (sel ? 30.0f : 24.0f) * scale;
        centred(Text(2 + i), 320, y + (sel ? -2 : 1), size, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(0xC8, 0xC8, 0xC8, 255));
        std::lock_guard l(g_quit_lock);
        g_dialog_rows[i] = {250, y - 2, 390, y + 30};
    }
    if (in_tv) gpu::g_tv_ui = dl;
}

// the PC options' button: the page's frame in small (a white line, a dark grey band, a white line, black), its words
// in the pause screen's typeface and spacing, white and larger when selected
void DrawPcButton() {
    if (!g_popts || cod::OverlayOpen()) return;
    float scale, left, top;
    ImFont* font = cod::overlay::HudFont();
    if (!font || !Mapping(scale, left, top)) return;
    Box b;
    {
        std::lock_guard l(g_pc_lock);
        b = g_pc_button;
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    auto at = [&](float x, float y) { return ImVec2(left + x * scale, top + y * scale); };
    const ImU32 white = IM_COL32(255, 255, 255, 255), band = IM_COL32(64, 64, 64, 255);
    dl->AddRectFilled(at(b.x0, b.y0), at(b.x1, b.y1), white);
    dl->AddRectFilled(at(b.x0 + 2, b.y0 + 2), at(b.x1 - 2, b.y1 - 2), band);
    dl->AddRectFilled(at(b.x0 + 7, b.y0 + 7), at(b.x1 - 7, b.y1 - 7), white);
    dl->AddRectFilled(at(b.x0 + 8, b.y0 + 8), at(b.x1 - 8, b.y1 - 8), IM_COL32(0, 0, 0, 255));
    bool lit = g_pc_focus;
    const char* text = cod::overlay::Tr("PC Options");
    float size = (lit ? 23.0f : 20.0f) * scale, track = 1.9f * size / 20.0f;
    float wsum = 0;
    for (const char* c = text; *c;) {
        const char* next = c + 1;
        while (*next && (*next & 0xC0) == 0x80) next++;
        wsum += font->CalcTextSizeA(size, FLT_MAX, 0, c, next).x + (*next ? track : 0);
        c = next;
    }
    ImVec2 p = at((b.x0 + b.x1) * 0.5f, (b.y0 + b.y1) * 0.5f);
    p.x -= wsum * 0.5f, p.y -= size * 0.58f;
    ImU32 col = lit ? white : IM_COL32(0xC8, 0xC8, 0xC8, 255);
    for (const char* c = text; *c;) {
        const char* next = c + 1;
        while (*next && (*next & 0xC0) == 0x80) next++;
        dl->AddText(font, size, p, col, c, next);
        p.x += font->CalcTextSizeA(size, FLT_MAX, 0, c, next).x + (*next ? track : 0);
        c = next;
    }
}

void Draw() {
    const bool menu = (cod::wide::FrontEnd() || cod::Paused()) && !cod::InMovie();
    DrawQuit();
    DrawPcButton();
    if (!(menu || cod::OverlayOpen()) || !cod::Get().keyboard_mouse) return;
    double now = ImGui::GetTime();
    if (now - g_mouse_time > 4.0) return;  // the pointer fades out when the mouse rests
    float mx, my, scale, x0, y0;
    if (!MousePos(mx, my, scale, x0, y0)) return;
    uint64_t id = gpu::UiTexture("ui/cursor.dds");
    if (!id) return;
    float a = std::clamp(float(4.0 - (now - g_mouse_time)) / 0.5f, 0.0f, 1.0f);
    float size = 22 * scale;
    ImVec2 p(x0 + mx * scale, y0 + my * scale);
    ImGui::GetForegroundDrawList()->AddImage(ImTextureID(id), ImVec2(p.x - size * 0.08f, p.y - size * 0.04f),
                                             ImVec2(p.x + size * 0.92f, p.y + size * 0.96f), ImVec2(0, 0), ImVec2(1, 1),
                                             IM_COL32(255, 255, 255, int(a * 255)));
}

// the game's poll of port 0, last: the row's and the question's keys and buttons, the game's input held back
void Filter(input::Pad& pad) {
    static bool prev[6] = {};
    bool up = (pad.buttons & 1) || pad.ly > 16000, down = (pad.buttons & 2) || pad.ly < -16000;
    bool lr = (pad.buttons & 0xC) || pad.lx > 16000 || pad.lx < -16000;
    bool accept = pad.analog[0] > 64 || (pad.buttons & 0x10), back = pad.analog[1] > 64;
    bool any = up || down || lr || accept || back || pad.buttons || pad.analog[2] > 64 || pad.analog[3] > 64;
    bool now[6] = {up, down, lr, accept, back, any};
    bool edge[6];
    for (int i = 0; i < 6; i++) edge[i] = now[i] && !prev[i], prev[i] = now[i];
    auto hold = [&] {
        bool c = pad.connected;
        pad = input::Pad{};
        pad.connected = c;
    };
    // the pause screen: the game's controls bind B to nothing there (its scripts; its prompts show B for continue and
    // back) and START leaves it from every page, its question too. B leaves it as well, as Escape does on the keyboard;
    // still held in the game after, it is not the game's (B crouches there) until let go
    static bool b_left = false;
    if (b_left && !cod::Paused()) {
        if (back) pad.analog[1] = 0;
        else b_left = false;
    }
    auto pause_back = [&] {
        if (back && cod::Paused() && !cod::wide::FrontEnd() && !cod::InMovie()) {
            pad.buttons |= 0x10;
            pad.analog[1] = 0;
            b_left = true;
        }
    };
    static const bool trace = std::getenv("XBE_MENU_TRACE") != nullptr;
    if (trace && any && edge[5]) LOGR("menu: poll main %d sel %d focus %d dialog %d swallow %d up %d down %d accept %d back %d", int(g_main), int(g_main_sel),
                                     int(g_quit_focus), int(g_dialog), int(g_swallow), up, down, accept, back);
    // the PC settings up (opened from the button): when they close, the key that closed them (Escape, B) is the
    // game's no more, nor does it open them again; the selection back on the game's rows
    if (cod::OverlayOpen()) {
        g_pc_focus = false;
        g_swallow = true;
        hold();
        return;
    }
    if (g_popts) {
        if (g_swallow) {
            if (any) hold();
            else g_swallow = false;
            return;
        }
        if (g_pc_focus) {
            if (edge[0] || edge[4]) g_pc_focus = false, g_swallow = true;
            if (edge[3]) g_pc_open = true, g_swallow = true;
            hold();
            return;
        }
        if (g_swallow) {
            if (any) hold();
            else g_swallow = false;
            return;
        }
        // down from the page's last row onto the button
        if (edge[1] && g_popts_count > 0 && g_popts_sel == g_popts_count - 1) {
            g_pc_focus = true;
            hold();
        }
        pause_back();
        return;
    }
    if (!g_main) {
        g_swallow = false;
        pause_back();
        return;
    }
    if (g_dialog) {
        if (edge[0] || edge[1] || edge[2]) g_yes = 1 - g_yes;
        if (edge[3]) {
            if (g_yes == 1) PostMessageW(static_cast<HWND>(app::Window()), WM_CLOSE, 0, 0);
            g_dialog = false;
        }
        if (edge[4]) g_dialog = false;
        if (!g_dialog) g_swallow = true;
        hold();
        return;
    }
    if (g_quit_focus) {
        if (edge[0] || edge[4]) g_quit_focus = false, g_swallow = true;
        if (edge[3]) g_dialog = true, g_yes = 0;
        hold();
        return;
    }
    if (g_swallow) {
        if (any) hold();
        else g_swallow = false;
        return;
    }
    // down from the last row onto this one
    if (edge[1] && g_main_sel == 3) {
        g_quit_focus = true;
        hold();
        return;
    }
    // a move of the game's own: its sound, for the Quit row's (QuitSound)
    if (edge[0] || edge[1]) xdk::dsound::RememberNextSound();
}

void TestMouse(float x, float y, bool click) {
    g_test_x = x, g_test_y = y;
    g_test_until = ImGui::GetTime() + 3;
    g_test_click = click;
}

void Dump() {
    std::lock_guard l(g_lock);
    for (const auto& d : g_last)
        LOGR("menu: %016llx %08X tag %X  %.0f..%.0f x %.0f..%.0f", (unsigned long long)d.tex, d.color, d.tag, d.x0, d.x1, d.y0, d.y1);
}

}  // namespace cod::menu_mouse
