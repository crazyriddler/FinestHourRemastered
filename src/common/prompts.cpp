// Key caps in place of the controller's button pictures. The game has two sets of them, 32 by 32: the front end's
// and pause menu's (named 0000 to 0015 and 1001 to 1003 in its texture dictionaries) and the hints' in the game
// (Xbox_*_but). Each is known by its content id (gpu/remaster.h) and drawn with the key bound to what the button
// does there: in the menus A accepts and B goes back, in the game A jumps and B crouches.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "common/cod.h"
#include "common/pc_input.h"
#include "common/prompts.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/remaster.h"

namespace cod::prompts {
namespace {

using pc_input::Action;

// what a picture shows: the key of an action, or a fixed label
struct Picture {
    uint64_t id;
    int action;           // -1: label
    const wchar_t* label;
};

constexpr Picture kPictures[] = {
    // the menus'
    {0x7ff3c37aa2dfa043, pc_input::kDpadUp, nullptr},      // 0000 up
    {0xb8a2eda6d5db1e43, pc_input::kDpadDown, nullptr},    // 0001 down
    {0xf522a4076adc53cd, pc_input::kDpadLeft, nullptr},    // 0002 left
    {0xc2df373f5900b69b, pc_input::kDpadRight, nullptr},   // 0003 right
    {0x0a11b5e815b9db91, pc_input::kPause, nullptr},       // 0004 start
    {0xb48283a0617e6149, pc_input::kObjectives, nullptr},  // 0005 back
    {0xb4a5c98f7bc73284, -1, L"WASD"},                     // 0006 left stick
    {0x56c22005df619e50, -1, L"Mouse"},                    // 0007 right stick
    {0xe2c19c648cf52d2a, pc_input::kMenuAccept, nullptr},  // 0008 A
    {0xcb5c4437250b2de4, pc_input::kPause, nullptr},       // 0009 B: back, Escape in the menus
    {0x73e7f60cc2f7343b, pc_input::kGrenade, nullptr},     // 0010 X
    {0x36d63aca31cc1372, pc_input::kJump, nullptr},        // 0011 Y
    {0x5f8470b1a90c0359, pc_input::kUse, nullptr},         // 0012 black
    {0xaa7dcb7cc6cd8b8a, pc_input::kReload, nullptr},      // 0013 white
    {0xeb13c00f7591a846, pc_input::kAim, nullptr},         // 0014 left trigger
    {0x262d60f572d28203, pc_input::kFire, nullptr},        // 0015 right trigger
    {0xb4e56ad07ff014d2, -1, L"↑↓"},             // 1001 up and down
    {0xd98dea4a7c2b90bf, -1, L"←→"},             // 1002 left and right
    {0xdb3e9d0fb8e14130, -1, L"←↑↓→"}, // 1003 every way
    // the game's
    {0x60972f78dfe1777b, pc_input::kMelee, nullptr},       // A
    {0x16d39373e1498958, pc_input::kCrouch, nullptr},      // B
    {0x7a2ef7b0f903497f, pc_input::kGrenade, nullptr},     // X
    {0x615b94f7842ef99e, pc_input::kJump, nullptr},        // Y
    {0x808ac0fcf5c0754c, pc_input::kUse, nullptr},         // black
    {0xe1947eef9b029bd8, pc_input::kReload, nullptr},      // white
    {0xcb867f23e69e83ab, pc_input::kObjectives, nullptr},  // back
    {0x5ac76a0b336ac621, pc_input::kPause, nullptr},       // start
    {0xa826c527db643a59, pc_input::kAim, nullptr},         // left trigger
    {0x46b26b3ad9598352, pc_input::kFire, nullptr},        // right trigger
    {0x496a780a5dd7b91f, -1, L"Q E H"},                    // d-pad: next weapon, next grenade, health pack
};

// In the game (not the menus) the hints show the menus' pictures too: there each one stands for what its button does
// in the game (B crouches, not back; A strikes, not accept; left and right lean while aiming)
struct InGame {
    uint64_t id;
    int action;           // -1: label
    int action2;          // a second action's key after the first (left and right: both leans)
};
constexpr InGame kInGame[] = {
    {0x7ff3c37aa2dfa043, pc_input::kHealth, -1},        // 0000 up
    {0xb8a2eda6d5db1e43, pc_input::kCenterTurret, -1},  // 0001 down
    {0xf522a4076adc53cd, pc_input::kLeanLeft, -1},      // 0002 left
    {0xc2df373f5900b69b, pc_input::kLeanRight, -1},     // 0003 right
    {0xe2c19c648cf52d2a, pc_input::kMelee, -1},         // 0008 A
    {0xcb5c4437250b2de4, pc_input::kCrouch, -1},        // 0009 B
    {0xd98dea4a7c2b90bf, pc_input::kLeanLeft, pc_input::kLeanRight},  // 1002 left and right
};
// and on the controller, as the game's own set (the layout's buttons)
struct PadInGame {
    uint64_t id;
    uint64_t game;  // the game's picture standing for it (kPadPictures)
};
constexpr PadInGame kPadInGame[] = {
    {0xe2c19c648cf52d2a, 0x60972f78dfe1777b},  // A
    {0xcb5c4437250b2de4, 0x16d39373e1498958},  // B
    {0x73e7f60cc2f7343b, 0x7a2ef7b0f903497f},  // X
    {0x36d63aca31cc1372, 0x615b94f7842ef99e},  // Y
    {0x5f8470b1a90c0359, 0x808ac0fcf5c0754c},  // black
    {0xaa7dcb7cc6cd8b8a, 0xe1947eef9b029bd8},  // white
};

std::atomic<int> g_device{-1};  // 0 controller, 1 keyboard, -1 not known yet

std::wstring Label(int key) {
    switch (key) {
        case VK_LBUTTON: return L"LMB";
        case VK_RBUTTON: return L"RMB";
        case VK_MBUTTON: return L"MMB";
        case VK_XBUTTON1: return L"M4";
        case VK_XBUTTON2: return L"M5";
        case pc_input::kWheelUp: return L"MW↑";
        case pc_input::kWheelDown: return L"MW↓";
        case VK_UP: return L"↑";
        case VK_DOWN: return L"↓";
        case VK_LEFT: return L"←";
        case VK_RIGHT: return L"→";
        case VK_RETURN: return L"Enter";
        case VK_ESCAPE: return L"Esc";
        case VK_BACK: return L"Bksp";
        case VK_SPACE: return L"Space";
        case VK_TAB: return L"Tab";
        case VK_CAPITAL: return L"Caps";
        case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: return L"Shift";
        case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return L"Ctrl";
        case VK_LMENU: case VK_RMENU: case VK_MENU: return L"Alt";
    }
    std::string n = pc_input::KeyName(key);
    if (n.size() > 5) n.resize(5);
    return std::wstring(n.begin(), n.end());
}

// How a picture is drawn: a key cap (rounded square), a controller's round button or its shoulder buttons' rounded
// bar, the label's colour and typeface
struct Style {
    int shape = 0;  // 0 key, 1 round, 2 bar
    COLORREF ink = RGB(255, 255, 255);
    const wchar_t* font = L"Segoe UI";
    bool rim = true;
};

// a picture, size by size RGBA: a dark face with a rim and the label, drawn 4 times larger and filtered down
std::vector<uint8_t> Cap(const std::wstring& text, uint32_t size, const Style& st = {}) {
    const int s = int(size) * 4;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    auto px = static_cast<uint32_t*>(bits);
    auto clear = [&] { std::fill(px, px + size_t(s) * s, 0u); };
    std::vector<uint8_t> outer(size_t(s) * s), inner(size_t(s) * s), label(size_t(s) * s);
    auto grab = [&](std::vector<uint8_t>& m) {
        GdiFlush();
        for (size_t i = 0; i < m.size(); i++) m[i] = uint8_t(px[i] >> 8);  // green
    };
    HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(WHITE_BRUSH));
    const int pad = s / 16, rim = st.rim ? s / 20 : s / 40, round = s / 4;
    auto shape = [&](int in) {
        int x0 = pad + in, x1 = s - pad - in, y0 = pad + in, y1 = s - pad - in;
        if (st.shape == 1) Ellipse(dc, x0, y0, x1, y1);
        else if (st.shape == 2) RoundRect(dc, x0, y0 + s / 6, x1, y1 - s / 6, s / 3, s / 3);
        else RoundRect(dc, x0, y0, x1, y1, round - in, round - in);
    };
    clear();
    shape(0);
    grab(outer);
    clear();
    shape(rim);
    grab(inner);
    clear();
    // the largest font the label fits in
    const int room = s - 2 * (pad + rim) - (st.shape == 1 ? s / 5 : s / 10);
    HFONT font = nullptr;
    for (int h = s / 2; h > 8; h -= s / 64 + 1) {
        if (font) DeleteObject(font);
        font = CreateFontW(-h, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH, st.font);
        SelectObject(dc, font);
        SIZE ext{};
        GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &ext);
        if (ext.cx <= room && ext.cy <= room) break;
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT r{0, 0, s, s};
    DrawTextW(dc, text.c_str(), int(text.size()), &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    grab(label);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_bmp);
    if (font) DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);

    std::vector<uint8_t> out(size_t(size) * size * 4);
    for (uint32_t y = 0; y < size; y++)
        for (uint32_t x = 0; x < size; x++) {
            float o = 0, in = 0, t = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    size_t k = size_t(y * 4 + j) * s + x * 4 + i;
                    o += outer[k], in += inner[k], t += label[k];
                }
            o /= 16 * 255.0f, in /= 16 * 255.0f, t /= 16 * 255.0f;
            // rim light grey (a controller's: dark), face dark with a little gradient, label in its colour
            float shade = 1.0f - 0.25f * float(y) / float(size);
            float face[3] = {44 * shade, 46 * shade, 50 * shade};
            float rimc[3] = {205, 205, 200};
            if (!st.rim) rimc[0] = rimc[1] = rimc[2] = 90;
            float ink[3] = {float(GetRValue(st.ink)), float(GetGValue(st.ink)), float(GetBValue(st.ink))};
            uint8_t* d = &out[(size_t(y) * size + x) * 4];
            for (int c = 0; c < 3; c++) {
                float v = rimc[c] * (o - in) / std::max(o, 1e-3f) + face[c] * in / std::max(o, 1e-3f);
                v = v * (1 - t) + ink[c] * t;
                d[c] = uint8_t(std::clamp(v, 0.0f, 255.0f));
            }
            d[3] = uint8_t(std::clamp(o * 0.94f + t * 0.06f, 0.0f, 1.0f) * 255);
        }
    return out;
}

// The controller's pictures. Each of the game's button pictures shows the button that does it on the controller in
// hand: the menus' set as the buttons are (LB and RB in place of white and black), the game's set by the layout.
enum PadButton { pA, pB, pX, pY, pLB, pRB, pLT, pRT, pLS, pRS, pView, pMenu, pNone };
struct PadPicture {
    uint64_t id;
    PadButton classic, modern;
};
constexpr PadPicture kPadPictures[] = {
    {0x0a11b5e815b9db91, pMenu, pMenu}, {0xb48283a0617e6149, pView, pView}, {0xb4a5c98f7bc73284, pLS, pLS},
    {0x56c22005df619e50, pRS, pRS},     {0xe2c19c648cf52d2a, pA, pA},       {0xcb5c4437250b2de4, pB, pB},
    {0x73e7f60cc2f7343b, pX, pX},       {0x36d63aca31cc1372, pY, pY},       {0x5f8470b1a90c0359, pRB, pRB},
    {0xaa7dcb7cc6cd8b8a, pLB, pLB},     {0xeb13c00f7591a846, pLT, pLT},     {0x262d60f572d28203, pRT, pRT},
    // the game's: A melee, B crouch, X grenade, Y jump, black use, white reload (see game_info.cpp's Modern)
    {0x60972f78dfe1777b, pA, pRS},      {0x16d39373e1498958, pB, pB},       {0x7a2ef7b0f903497f, pX, pRB},
    {0x615b94f7842ef99e, pY, pA},       {0x808ac0fcf5c0754c, pRB, pLB},     {0xe1947eef9b029bd8, pLB, pX},
    {0xcb867f23e69e83ab, pView, pView}, {0x5ac76a0b336ac621, pMenu, pMenu}, {0xa826c527db643a59, pLT, pLT},
    {0x46b26b3ad9598352, pRT, pRT},
};

// a button's picture for the controller's kind: Xbox letters in their colours on dark buttons, PlayStation's shapes,
// Nintendo's letters by position
std::vector<uint8_t> PadCap(PadButton b, input::Kind kind, uint32_t size) {
    Style st;
    st.rim = false;
    std::wstring t;
    const bool ps = kind == input::kPlayStation, nin = kind == input::kNintendo;
    switch (b) {
        case pA: case pB: case pX: case pY: {
            st.shape = 1;
            int i = int(b - pA);
            if (ps) {
                const wchar_t* sym[] = {L"\u2715", L"\u25CB", L"\u25A1", L"\u25B3"};
                const COLORREF col[] = {RGB(124, 167, 232), RGB(232, 106, 106), RGB(215, 139, 196), RGB(76, 194, 163)};
                t = sym[i], st.ink = col[i], st.font = L"Segoe UI Symbol";
            } else {
                const wchar_t* xb[] = {L"A", L"B", L"X", L"Y"};
                const wchar_t* sw[] = {L"B", L"A", L"Y", L"X"};  // Nintendo's letters at the same places
                const COLORREF col[] = {RGB(106, 191, 69), RGB(226, 58, 58), RGB(58, 142, 226), RGB(242, 194, 48)};
                t = nin ? sw[i] : xb[i];
                st.ink = nin ? RGB(255, 255, 255) : col[i];
            }
            break;
        }
        case pLB: t = ps ? L"L1" : nin ? L"L" : L"LB"; st.shape = 2; break;
        case pRB: t = ps ? L"R1" : nin ? L"R" : L"RB"; st.shape = 2; break;
        case pLT: t = ps ? L"L2" : nin ? L"ZL" : L"LT"; st.shape = 0; break;
        case pRT: t = ps ? L"R2" : nin ? L"ZR" : L"RT"; st.shape = 0; break;
        case pLS: t = ps ? L"L3" : L"LS"; st.shape = 1; break;
        case pRS: t = ps ? L"R3" : L"RS"; st.shape = 1; break;
        case pView: t = ps ? L"\u25A4" : nin ? L"\u2212" : L"\u29C9"; st.shape = 1; st.font = L"Segoe UI Symbol"; break;
        case pMenu: t = ps ? L"\u2261" : nin ? L"+" : L"\u2261"; st.shape = 1; st.font = L"Segoe UI Symbol"; break;
        default: return {};
    }
    return Cap(t, size, st);
}

// a picture of the pack's set (Kenney's Input Prompts), empty when it has none
std::vector<uint8_t> Icon(const std::string& name, uint32_t size) {
    gpu::remaster::Image im;
    if (!gpu::remaster::ReadPrompt(name, im) || im.w != size || im.h != size) return {};
    return std::vector<uint8_t>(im.data.begin(), im.data.begin() + std::ptrdiff_t(size_t(size) * size * 4));
}

std::string LabelIcon(const std::wstring& label) {
    if (label == L"\u2191\u2193") return "kb_updown";
    if (label == L"\u2190\u2192") return "kb_leftright";
    if (label == L"\u2190\u2191\u2193\u2192") return "kb_arrows";
    if (label == L"WASD") return "kb_wasd";
    if (label == L"Mouse") return "kb_mouse";
    return {};
}

std::string PadIcon(PadButton b, input::Kind kind) {
    const char* k = kind == input::kPlayStation ? "ps" : kind == input::kNintendo ? "nin" : "xbox";
    const char* n[] = {"a", "b", "x", "y", "lb", "rb", "lt", "rt", "ls", "rs", "view", "menu"};
    return b < pNone ? std::string("pad_") + k + "_" + n[b] : std::string();
}

int KeyOf(int action) {
    const pc_input::Bindings& b = pc_input::Keys();
    return b.key[action][0] ? b.key[action][0] : b.key[action][1];
}

}  // namespace

void PadUsed() { g_device = 0; }
void KeyboardUsed() { g_device = 1; }

bool KeyboardShown() {
    const Settings& set = Get();
    int dev = g_device;
    return set.button_prompts == 2 || (set.button_prompts == 0 && set.keyboard_mouse && dev != 0);
}

void Update() {
    const Settings& set = Get();
    bool keys = KeyboardShown();
    // in the game, or a menu (the front end, the pause screen, a movie)
    const bool game = !cod::InMenu();
    // what the pictures depend on: the device, then each picture's key, or the controller's kind and layout
    static std::string last = "-";
    const input::Kind kind = keys ? input::kNoController : input::ControllerKind(0);
    std::string sig(1, keys ? 'k' : 'p');
    sig += game ? 'g' : 'm';
    if (!keys) sig += std::to_string(int(kind)) + std::to_string(set.pad_layout);
    if (keys) {
        for (const Picture& p : kPictures) sig += std::to_string(p.action >= 0 ? KeyOf(p.action) : 0) + ",";
        for (const InGame& g : kInGame) sig += std::to_string(KeyOf(g.action)) + "," + std::to_string(g.action2 >= 0 ? KeyOf(g.action2) : 0) + ",";
    }
    if (sig == last) return;
    last = sig;
    std::vector<gpu::TextureOverride> out;
    if (keys) {
        constexpr uint32_t kSize = 128;
        for (const Picture& p : kPictures) {
            std::wstring text = p.label ? p.label : L"";
            std::string icon = LabelIcon(text);
            int action = p.action, action2 = -1;
            if (game)
                for (const InGame& g : kInGame)
                    if (g.id == p.id) action = g.action, action2 = g.action2;
            if (action >= 0) {
                int key = KeyOf(action);
                if (!key) continue;  // nothing bound: the controller's picture stays
                text = Label(key);
                char n[16];
                std::snprintf(n, sizeof(n), "kb_%02x", key);
                icon = n;
                if (action2 >= 0 && KeyOf(action2)) text += L" " + Label(KeyOf(action2)), icon.clear();  // two keys
            }
            gpu::TextureOverride o;
            o.id = p.id;
            o.w = o.h = kSize;
            if (!icon.empty()) o.rgba = Icon(icon, kSize);
            if (o.rgba.empty()) o.rgba = Cap(text, kSize);
            out.push_back(std::move(o));
        }
    } else if (kind != input::kNoController) {
        constexpr uint32_t kSize = 128;
        for (const PadPicture& p : kPadPictures) {
            gpu::TextureOverride o;
            o.id = p.id;
            o.w = o.h = kSize;
            const PadPicture* src = &p;
            if (game)
                for (const PadInGame& g : kPadInGame)
                    if (g.id == p.id)
                        for (const PadPicture& q : kPadPictures)
                            if (q.id == g.game) src = &q;
            PadButton b = set.pad_layout == 0 ? src->modern : src->classic;
            o.rgba = Icon(PadIcon(b, kind), kSize);
            if (o.rgba.empty()) o.rgba = PadCap(b, kind, kSize);
            if (!o.rgba.empty()) out.push_back(std::move(o));
        }
    }
    LOGR("prompts: %s", keys ? "keyboard" : kind == input::kPlayStation ? "PlayStation controller" : "controller");
    gpu::SetTextureOverrides(std::move(out));
}

}  // namespace cod::prompts
