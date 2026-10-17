// The boot splash: GameInfo::splash .bmp or .png beside the exe, centred in a borderless window. The game boots behind
// it with its window hidden and waits at its first frame until the splash has been up kMinimum, since this boot takes
// a fraction of a second. No file, or a relaunch: no splash.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/log.h"

using Microsoft::WRL::ComPtr;

namespace app {

namespace {

constexpr auto kMinimum = std::chrono::seconds(1);

HWND g_splash = nullptr;
std::atomic<bool> g_waiting{false};
std::chrono::steady_clock::time_point g_until;

// premultiplied BGRA, as UpdateLayeredWindow wants it; a picture without alpha comes out opaque
bool Read(const std::filesystem::path& path, std::vector<uint32_t>& pixels, UINT& w, UINT& h) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = converter->GetSize(&w, &h);
    if (SUCCEEDED(hr) && w && h) {
        pixels.resize(size_t(w) * h);
        hr = converter->CopyPixels(nullptr, w * 4, UINT(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data()));
    }
    if (FAILED(hr) || !w || !h) {
        LOGW("splash: %s could not be read (%08X)", path.string().c_str(), unsigned(hr));
        return false;
    }
    return true;
}

}  // namespace

bool OpenSplash() {
    if (std::wcsstr(GetCommandLineW(), L"--relaunched")) return false;
    std::vector<uint32_t> pixels;
    UINT w = 0, h = 0;
    std::filesystem::path path;
    for (const wchar_t* ext : {L".bmp", L".png"}) {
        std::error_code ec;
        path = ExeDir() / (std::wstring(kGame.splash) + ext);
        if (std::filesystem::exists(path, ec) && Read(path, pixels, w, h)) break;
        pixels.clear();
    }
    if (pixels.empty()) return false;

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32514));  // IDC_WAIT
    std::wstring cls = std::wstring(kGame.name) + L"Splash";
    wc.lpszClassName = cls.c_str();
    RegisterClassExW(&wc);
    // centred on the primary screen at one image pixel per screen pixel
    POINT at{(GetSystemMetrics(SM_CXSCREEN) - int(w)) / 2, (GetSystemMetrics(SM_CYSCREEN) - int(h)) / 2};
    g_splash = CreateWindowExW(WS_EX_LAYERED, wc.lpszClassName, kGame.title, WS_POPUP,
                               at.x, at.y, int(w), int(h), nullptr, nullptr, inst, nullptr);
    if (!g_splash) return false;

    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), LONG(w), -LONG(h), 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) {
        std::memcpy(bits, pixels.data(), pixels.size() * 4);
        HGDIOBJ old = SelectObject(dc, bitmap);
        SIZE size{LONG(w), LONG(h)};
        POINT origin{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(g_splash, screen, &at, &size, dc, &origin, 0, &blend, ULW_ALPHA);
        SelectObject(dc, old);
    }
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    // to the front once, without staying on top
    SetWindowPos(g_splash, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(g_splash, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    g_until = std::chrono::steady_clock::now() + kMinimum;
    g_waiting = true;
    LOGI("splash: %s, %ux%u", path.string().c_str(), w, h);
    return true;
}

void SplashFirstFrame() {
    if (!g_waiting.exchange(false)) return;
    std::this_thread::sleep_until(g_until);
    OpenGameWindow();
}

void CloseSplash() {
    if (g_splash) DestroyWindow(g_splash);
    g_splash = nullptr;
}

namespace {
HWND g_progress = nullptr;
constexpr int kPanelW = 560, kPanelH = 104;

// the panel: dark, a thin border, the line in white above a bar filled to the fraction (premultiplied BGRA)
void PaintProgress(const wchar_t* text, float fraction) {
    std::vector<uint32_t> px(size_t(kPanelW) * kPanelH);
    auto pm = [](uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
        return (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
    };
    for (int y = 0; y < kPanelH; y++)
        for (int x = 0; x < kPanelW; x++) {
            bool edge = x == 0 || y == 0 || x == kPanelW - 1 || y == kPanelH - 1;
            px[size_t(y) * kPanelW + x] = edge ? pm(90, 90, 90, 255) : pm(14, 14, 14, 238);
        }
    constexpr int kBarX = 40, kBarY = 70, kBarW = kPanelW - 80, kBarH = 8;
    int fill = int(std::clamp(fraction, 0.0f, 1.0f) * kBarW);
    for (int y = kBarY; y < kBarY + kBarH; y++)
        for (int x = kBarX; x < kBarX + kBarW; x++)
            px[size_t(y) * kPanelW + x] = x - kBarX < fill ? pm(214, 168, 72, 255) : pm(52, 52, 52, 255);

    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    // the text drawn white on black in a plain bitmap, its brightness the coverage laid over the panel
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kPanelW, -kPanelH, 1, 32, BI_RGB};
    void* mask_bits = nullptr;
    HBITMAP mask = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &mask_bits, nullptr, 0);
    if (mask && mask_bits) {
        std::memset(mask_bits, 0, size_t(kPanelW) * kPanelH * 4);
        HGDIOBJ old = SelectObject(dc, mask);
        HFONT font = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ old_font = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT r{0, 18, kPanelW, 58};
        DrawTextW(dc, text, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        GdiFlush();
        const uint32_t* m = static_cast<const uint32_t*>(mask_bits);
        for (size_t i = 0; i < px.size(); i++) {
            uint32_t c = (m[i] >> 8) & 0xFF;
            if (!c) continue;
            uint32_t p = px[i], keep = 255 - c;
            auto ch = [&](int sh) { return ((p >> sh) & 0xFF) * keep / 255 + c; };
            px[i] = (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
        }
        SelectObject(dc, old_font);
        DeleteObject(font);
        SelectObject(dc, old);
        DeleteObject(mask);
    }
    // tests: XBE_PROGRESS_SHOT=<file.bmp>, the panel once past half way
    static const char* shot = std::getenv("XBE_PROGRESS_SHOT");
    static bool shot_done = false;
    if (shot && !shot_done && fraction >= 0.5f) {
        shot_done = true;
        if (FILE* f = std::fopen(shot, "wb")) {
            BITMAPFILEHEADER fh{0x4D42, DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + px.size() * 4), 0, 0,
                                DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER))};
            std::fwrite(&fh, sizeof(fh), 1, f);
            std::fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
            std::fwrite(px.data(), 4, px.size(), f);
            std::fclose(f);
        }
    }
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) {
        std::memcpy(bits, px.data(), px.size() * 4);
        HGDIOBJ old = SelectObject(dc, bitmap);
        // centred on the game window's screen
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY), &mi);
        POINT at{(mi.rcMonitor.left + mi.rcMonitor.right - kPanelW) / 2, (mi.rcMonitor.top + mi.rcMonitor.bottom - kPanelH) / 2};
        SIZE size{kPanelW, kPanelH};
        POINT origin{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(g_progress, screen, &at, &size, dc, &origin, 0, &blend, ULW_ALPHA);
        SelectObject(dc, old);
    }
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}
}  // namespace

void Progress(const wchar_t* text, float fraction) {
    if (fraction < 0) {
        if (g_progress) DestroyWindow(g_progress);
        g_progress = nullptr;
    } else {
        if (!g_progress) {
            HINSTANCE inst = GetModuleHandleW(nullptr);
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32514));  // IDC_WAIT
            std::wstring cls = std::wstring(kGame.name) + L"Progress";
            wc.lpszClassName = cls.c_str();
            RegisterClassExW(&wc);
            // over the game's window (and the splash) while it lasts, kept off the taskbar
            g_progress = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.c_str(),
                                         kGame.title, WS_POPUP, 0, 0, kPanelW, kPanelH, nullptr, nullptr, inst, nullptr);
            if (!g_progress) return;
            PaintProgress(text, fraction);
            ShowWindow(g_progress, SW_SHOWNOACTIVATE);
        } else {
            PaintProgress(text, fraction);
        }
    }
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

}  // namespace app
