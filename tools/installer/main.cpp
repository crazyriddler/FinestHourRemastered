// Finest Hour Remastered's installer: a small styled window (Dear ImGui on Direct3D 11) that takes the player's own
// copy of Call of Duty: Finest Hour (its files, a disc image or an XISO), checks it is the release the port was made
// from, and installs the port beside it: the game's files under assets\, the port's files from the payload that
// ships beside the installer (FinestHourRemastered.index and its .data.NNN parts), shortcuts.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "resource.h"
#include "xdvdfs.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

// the release the port was recompiled from
constexpr char kXbeSha1[] = "938a7b1e17928de49cb3c5834d61abcd3d9081c8";
constexpr wchar_t kProduct[] = L"Finest Hour Remastered";
constexpr wchar_t kExe[] = L"FinestHourRemastered.exe";
constexpr char kRequired[] =
    "Call of Duty: Finest Hour for the original Xbox, PAL (Europe) release, title ID 4156002A, version 3.";

ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<IDXGISwapChain> g_swap;
ComPtr<ID3D11RenderTargetView> g_rtv;
ComPtr<ID3D11ShaderResourceView> g_art;
int g_art_w = 0, g_art_h = 0;
HWND g_hwnd = nullptr;
ImFont* g_title = nullptr;
ImFont* g_body = nullptr;
ImFont* g_mono = nullptr;

// --- the state the window shows -----------------------------------------------------------------------------------

enum class Page { Welcome, Source, Folder, Install, Done, Failed };
Page g_page = Page::Welcome;
std::wstring g_source;   // the game: a folder or an image
bool g_source_is_image = false;
std::string g_source_note;
bool g_source_ok = false;
std::wstring g_target;
bool g_shortcut_desktop = true, g_shortcut_menu = true;

std::mutex g_lock;
std::string g_status;       // under g_lock
std::vector<std::string> g_log;
std::atomic<double> g_progress{0};
std::atomic<bool> g_working{false}, g_cancel{false};
std::string g_error;

void Log(const std::string& s) {
    std::lock_guard l(g_lock);
    g_log.push_back(s);
    g_status = s;
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 1)) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(std::max(n, 1)) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string Sha1(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    uint8_t digest[20] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
            BCryptHashData(h, const_cast<PUCHAR>(data.data()), ULONG(data.size()), 0);
            BCryptFinishHash(h, digest, sizeof(digest), 0);
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    std::string hex;
    for (uint8_t b : digest) hex += "0123456789abcdef"[b >> 4], hex += "0123456789abcdef"[b & 15];
    return hex;
}

fs::path ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

// --- picking -------------------------------------------------------------------------------------------------------

bool Pick(bool folder, std::wstring& out) {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&d)))) return false;
    DWORD opts = 0;
    d->GetOptions(&opts);
    if (folder) {
        d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    } else {
        COMDLG_FILTERSPEC spec[] = {{L"Xbox disc image (*.iso, *.xiso)", L"*.iso;*.xiso"}, {L"All files", L"*.*"}};
        d->SetFileTypes(2, spec);
        d->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    }
    if (FAILED(d->Show(g_hwnd))) return false;
    ComPtr<IShellItem> item;
    if (FAILED(d->GetResult(&item))) return false;
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return false;
    out = p;
    CoTaskMemFree(p);
    return true;
}

// the chosen game checked: its default.xbe the release's
void CheckSource() {
    g_source_ok = false;
    std::vector<uint8_t> xbe;
    if (g_source_is_image) {
        xdvdfs::Image img;
        std::string err;
        if (!img.Open(g_source, err)) {
            g_source_note = err;
            return;
        }
        const xdvdfs::Entry* e = img.Find("default.xbe");
        if (!e || !img.Read(*e, xbe)) {
            g_source_note = "The image has no default.xbe: it is not this game's disc.";
            return;
        }
    } else {
        std::ifstream f(fs::path(g_source) / "default.xbe", std::ios::binary);
        if (!f) {
            g_source_note = "The folder has no default.xbe: choose the folder that holds the game's files\n"
                            "(default.xbe, COD.PAK, AUDIO, SHELL, SB_AGD, ...).";
            return;
        }
        xbe.assign(std::istreambuf_iterator<char>(f), {});
    }
    std::string sha = Sha1(xbe);
    if (sha != kXbeSha1) {
        g_source_note = "This copy is a different release of the game (default.xbe SHA-1 " + sha.substr(0, 12) +
                        "...).\nThe port only runs the release it was made from.";
        return;
    }
    g_source_ok = true;
    g_source_note = "Your copy is the right release.";
}

// --- installing ----------------------------------------------------------------------------------------------------

struct Payload {
    std::vector<std::pair<uint64_t, std::string>> files;  // size, path inside the install folder
    std::vector<fs::path> parts;
    uint64_t total = 0;
};

bool ReadPayload(Payload& p, std::string& err) {
    fs::path dir = ExeDir();
    std::ifstream idx(dir / "FinestHourRemastered.index");
    if (!idx) {
        err = "FinestHourRemastered.index is missing beside the installer.";
        return false;
    }
    std::string magic;
    int parts = 0;
    idx >> magic >> parts >> p.total;
    if (magic != "FHR1" || parts <= 0) {
        err = "FinestHourRemastered.index is damaged.";
        return false;
    }
    std::string line;
    std::getline(idx, line);
    while (std::getline(idx, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        p.files.push_back({std::stoull(line.substr(0, tab)), line.substr(tab + 1)});
    }
    for (int i = 1; i <= parts; i++) {
        wchar_t name[64];
        swprintf(name, 64, L"FinestHourRemastered.data.%03d", i);
        if (!fs::exists(dir / name)) {
            err = "The payload part " + Narrow(name) + " is missing beside the installer.";
            return false;
        }
        p.parts.push_back(dir / name);
    }
    return true;
}

// a stream over the payload's parts, one after another
struct PartStream {
    std::vector<fs::path> parts;
    size_t i = 0;
    FILE* f = nullptr;
    ~PartStream() {
        if (f) std::fclose(f);
    }
    size_t Read(uint8_t* dst, size_t n) {
        size_t got = 0;
        while (got < n) {
            if (!f) {
                if (i >= parts.size()) break;
                f = _wfopen(parts[i++].c_str(), L"rb");
                if (!f) break;
            }
            size_t r = std::fread(dst + got, 1, n - got, f);
            got += r;
            if (r == 0) std::fclose(f), f = nullptr;
        }
        return got;
    }
};

void CreateShortcut(const fs::path& lnk, const fs::path& target) {
    ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(target.parent_path().c_str());
    sl->SetDescription(kProduct);
    ComPtr<IPersistFile> pf;
    if (SUCCEEDED(sl.As(&pf))) pf->Save(lnk.c_str(), TRUE);
}

void Install() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto fail = [](const std::string& e) {
        g_error = e;
        Log("Failed: " + e);
        g_working = false;
        g_page = Page::Failed;
    };
    Payload pay;
    std::string err;
    if (!ReadPayload(pay, err)) return fail(err);
    fs::path target = g_target, assets = target / "assets";
    std::error_code ec;
    fs::create_directories(assets, ec);
    if (ec) return fail("The folder cannot be made: " + ec.message());
    // the game's own bytes
    uint64_t game_total = 0;
    xdvdfs::Image img;
    std::vector<fs::path> game_files;
    if (g_source_is_image) {
        if (!img.Open(g_source, err)) return fail(err);
        game_total = img.TotalSize();
    } else {
        for (auto& e : fs::recursive_directory_iterator(g_source, ec))
            if (e.is_regular_file(ec)) game_files.push_back(e.path()), game_total += e.file_size(ec);
    }
    const double all = double(game_total + pay.total);
    uint64_t done = 0;
    auto step = [&](uint64_t n) {
        done += n;
        g_progress = all > 0 ? double(done) / all : 1;
        return !g_cancel.load();
    };
    Log("Copying the game's files...");
    std::vector<uint8_t> buf(8 << 20);
    if (g_source_is_image) {
        for (const xdvdfs::Entry& e : img.Files()) {
            fs::path to = assets / fs::path(Widen(e.path));
            fs::create_directories(to.parent_path(), ec);
            if (!img.Extract(e, to.wstring(), step)) return fail(g_cancel ? "Cancelled." : "Cannot extract " + e.path);
        }
    } else {
        for (const fs::path& from : game_files) {
            fs::path rel = fs::relative(from, g_source, ec);
            fs::path to = assets / rel;
            fs::create_directories(to.parent_path(), ec);
            FILE* i = _wfopen(from.c_str(), L"rb");
            FILE* o = _wfopen(to.c_str(), L"wb");
            bool ok = i && o;
            while (ok) {
                size_t n = std::fread(buf.data(), 1, buf.size(), i);
                if (n == 0) break;
                ok = std::fwrite(buf.data(), 1, n, o) == n && step(n);
            }
            if (i) std::fclose(i);
            if (o) std::fclose(o);
            if (!ok) return fail(g_cancel ? "Cancelled." : "Cannot copy " + Narrow(rel.wstring()));
        }
    }
    Log("Installing Finest Hour Remastered...");
    PartStream ps{pay.parts};
    for (auto& [size, rel] : pay.files) {
        fs::path to = target / fs::path(Widen(rel));
        fs::create_directories(to.parent_path(), ec);
        FILE* o = _wfopen(to.c_str(), L"wb");
        if (!o) return fail("Cannot write " + rel);
        uint64_t left = size;
        while (left > 0) {
            size_t n = ps.Read(buf.data(), size_t(std::min<uint64_t>(left, buf.size())));
            if (n == 0 || std::fwrite(buf.data(), 1, n, o) != n || !step(n)) {
                std::fclose(o);
                return fail(g_cancel ? "Cancelled." : "The payload ended early (" + rel + ").");
            }
            left -= n;
        }
        std::fclose(o);
    }
    Log("Making shortcuts...");
    fs::path exe = target / kExe;
    PWSTR p = nullptr;
    if (g_shortcut_desktop && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)))
        CreateShortcut(fs::path(p) / (std::wstring(kProduct) + L".lnk"), exe);
    CoTaskMemFree(p), p = nullptr;
    if (g_shortcut_menu && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &p)))
        CreateShortcut(fs::path(p) / (std::wstring(kProduct) + L".lnk"), exe);
    CoTaskMemFree(p);
    g_progress = 1;
    Log("Done.");
    g_working = false;
    g_page = Page::Done;
}

// --- drawing -------------------------------------------------------------------------------------------------------

const ImVec4 kGold(0.86f, 0.70f, 0.38f, 1.0f), kText(0.92f, 0.90f, 0.85f, 1.0f), kDim(0.62f, 0.60f, 0.55f, 1.0f),
    kRed(0.90f, 0.35f, 0.30f, 1.0f), kGreen(0.55f, 0.82f, 0.45f, 1.0f);

bool Button(const char* label, bool enabled = true, float w = 170) {
    if (!enabled) ImGui::BeginDisabled();
    bool r = ImGui::Button(label, ImVec2(w, 42));
    if (!enabled) ImGui::EndDisabled();
    return r;
}

void Frame() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    ImVec2 size = vp->Size;
    // the art, covering the window, darkened towards the left where the text is
    if (g_art) {
        float k = std::max(size.x / g_art_w, size.y / g_art_h);
        ImVec2 sz(g_art_w * k, g_art_h * k), at((size.x - sz.x) * 0.5f, (size.y - sz.y) * 0.5f);
        bg->AddImage(reinterpret_cast<ImTextureID>(g_art.Get()), at, ImVec2(at.x + sz.x, at.y + sz.y));
    }
    bg->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(8, 9, 8, 245), IM_COL32(8, 9, 8, 120), IM_COL32(8, 9, 8, 120),
                                IM_COL32(8, 9, 8, 245));
    bg->AddRectFilled(ImVec2(0, 0), ImVec2(size.x, 4), IM_COL32(220, 178, 96, 255));

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    // the close button, top right
    ImGui::SetCursorPos(ImVec2(size.x - 54, 14));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (ImGui::Button("X", ImVec2(40, 32)) && !g_working) PostQuitMessage(0);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(56, 48));
    ImGui::PushFont(g_title);
    ImGui::TextColored(kGold, "FINEST HOUR");
    ImGui::SetCursorPosX(56);
    ImGui::TextColored(kText, "REMASTERED");
    ImGui::PopFont();
    ImGui::SetCursorPosX(58);
    ImGui::TextColored(kDim, "A native PC port of Call of Duty: Finest Hour  -  by crazyriddler");
    ImGui::Dummy(ImVec2(0, 28));

    const float x = 58, wrap = 600;
    ImGui::PushTextWrapPos(x + wrap);
    auto para = [&](const char* s, ImVec4 c = kText) {
        ImGui::SetCursorPosX(x);
        ImGui::TextColored(c, "%s", s);
    };
    switch (g_page) {
        case Page::Welcome:
            para("Welcome. This installs Finest Hour Remastered, a native Windows port of the original Xbox game with "
                 "a remastered renderer, from your own copy of the game.");
            ImGui::Dummy(ImVec2(0, 10));
            para("You need:", kGold);
            para(kRequired);
            ImGui::Dummy(ImVec2(0, 10));
            para("Its files (a folder with default.xbe), a disc image (.iso) or an XISO. No game files are "
                 "included: they are read from your copy.",
                 kDim);
            ImGui::Dummy(ImVec2(0, 14));
            ImGui::SetCursorPosX(x);
            if (Button("Continue")) g_page = Page::Source;
            break;
        case Page::Source:
            para("Where is your copy of the game?", kGold);
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::SetCursorPosX(x);
            if (Button("Game folder...", true, 200)) {
                std::wstring p;
                if (Pick(true, p)) g_source = p, g_source_is_image = false, CheckSource();
            }
            ImGui::SameLine();
            if (Button("Disc image / XISO...", true, 230)) {
                std::wstring p;
                if (Pick(false, p)) g_source = p, g_source_is_image = true, CheckSource();
            }
            ImGui::Dummy(ImVec2(0, 14));
            if (!g_source.empty()) {
                para(Narrow(g_source).c_str(), kDim);
                para(g_source_note.c_str(), g_source_ok ? kGreen : kRed);
            }
            ImGui::Dummy(ImVec2(0, 24));
            ImGui::SetCursorPosX(x);
            if (Button("Back")) g_page = Page::Welcome;
            ImGui::SameLine();
            if (Button("Continue", g_source_ok)) g_page = Page::Folder;
            break;
        case Page::Folder: {
            para("Install to:", kGold);
            ImGui::Dummy(ImVec2(0, 6));
            static char buf[1024] = {};
            if (!buf[0]) std::snprintf(buf, sizeof(buf), "%s", Narrow(g_target).c_str());
            ImGui::SetCursorPosX(x);
            ImGui::SetNextItemWidth(470);
            if (ImGui::InputText("##target", buf, sizeof(buf))) g_target = Widen(buf);
            ImGui::SameLine();
            if (Button("Browse...", true, 120)) {
                std::wstring p;
                if (Pick(true, p)) {
                    g_target = (fs::path(p) / kProduct).wstring();
                    std::snprintf(buf, sizeof(buf), "%s", Narrow(g_target).c_str());
                }
            }
            ImGui::Dummy(ImVec2(0, 6));
            para("About 9 GB: the game's files and the remastered textures.", kDim);
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::SetCursorPosX(x);
            ImGui::Checkbox("Desktop shortcut", &g_shortcut_desktop);
            ImGui::SetCursorPosX(x);
            ImGui::Checkbox("Start menu shortcut", &g_shortcut_menu);
            ImGui::Dummy(ImVec2(0, 24));
            ImGui::SetCursorPosX(x);
            if (Button("Back")) g_page = Page::Source;
            ImGui::SameLine();
            if (Button("Install", !g_target.empty())) {
                g_page = Page::Install;
                g_working = true;
                g_cancel = false;
                std::thread(Install).detach();
            }
            break;
        }
        case Page::Install: {
            std::string status;
            {
                std::lock_guard l(g_lock);
                status = g_status;
            }
            para(status.c_str(), kGold);
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::SetCursorPosX(x);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kGold);
            char pct[32];
            std::snprintf(pct, sizeof(pct), "%.0f%%", g_progress.load() * 100);
            ImGui::ProgressBar(float(g_progress.load()), ImVec2(wrap, 26), pct);
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 24));
            ImGui::SetCursorPosX(x);
            if (Button("Cancel", !g_cancel)) g_cancel = true;
            break;
        }
        case Page::Done:
            para("Finest Hour Remastered is installed.", kGreen);
            ImGui::Dummy(ImVec2(0, 8));
            para("In the game, choose PC Settings in the main menu (or press F10) for the display, graphics and control "
                 "options. The first start prepares the shaders for your graphics card.",
                 kDim);
            ImGui::Dummy(ImVec2(0, 24));
            ImGui::SetCursorPosX(x);
            if (Button("Play now")) {
                fs::path exe = fs::path(g_target) / kExe;
                ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, exe.parent_path().c_str(), SW_SHOWNORMAL);
                PostQuitMessage(0);
            }
            ImGui::SameLine();
            if (Button("Close")) PostQuitMessage(0);
            break;
        case Page::Failed:
            para("The installation did not finish.", kRed);
            para(g_error.c_str(), kDim);
            ImGui::Dummy(ImVec2(0, 24));
            ImGui::SetCursorPosX(x);
            if (Button("Back")) g_page = Page::Folder;
            ImGui::SameLine();
            if (Button("Close")) PostQuitMessage(0);
            break;
    }
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPos(ImVec2(58, size.y - 40));
    ImGui::TextColored(kDim, "Not affiliated with Activision. Call of Duty is a trademark of Activision.");
    ImGui::End();
}

// --- the window and Direct3D 11 ------------------------------------------------------------------------------------

bool LoadArt() {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_ART), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    const void* data = LockResource(g);
    DWORD size = SizeofResource(nullptr, r);
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICStream> stream;
    wic->CreateStream(&stream);
    stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), size);
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    dec->GetFrame(0, &frame);
    ComPtr<IWICFormatConverter> conv;
    wic->CreateFormatConverter(&conv);
    conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    UINT w, h;
    conv->GetSize(&w, &h);
    std::vector<uint8_t> px(size_t(w) * h * 4);
    conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data());
    D3D11_TEXTURE2D_DESC td{w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA sd{px.data(), w * 4, 0};
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(g_dev->CreateTexture2D(&td, &sd, &tex))) return false;
    g_dev->CreateShaderResourceView(tex.Get(), nullptr, &g_art);
    g_art_w = int(w), g_art_h = int(h);
    return true;
}

ImFont* FontFromResource(int id, float px) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return nullptr;
    void* data = LockResource(LoadResource(nullptr, r));
    DWORD size = SizeofResource(nullptr, r);
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, int(size), px, &cfg);
}

void MakeTarget() {
    g_rtv.Reset();
    ComPtr<ID3D11Texture2D> bb;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb.Get(), nullptr, &g_rtv);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (g_swap && w != SIZE_MINIMIZED) {
                g_rtv.Reset();
                g_swap->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
                MakeTarget();
            }
            return 0;
        case WM_NCHITTEST: {
            // the top band drags the window (no title bar), the close button aside
            LRESULT hit = DefWindowProcW(h, msg, w, l);
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(h, &p);
            RECT rc;
            GetClientRect(h, &rc);
            if (hit == HTCLIENT && p.y < 44 && p.x < rc.right - 64) return HTCAPTION;
            return hit;
        }
        case WM_CLOSE:
            if (g_working) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    PWSTR pf = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX86, 0, nullptr, &pf)))
        g_target = (fs::path(pf).parent_path() / L"Games" / kProduct).wstring();
    CoTaskMemFree(pf);
    if (g_target.empty()) g_target = L"C:\\Games\\Finest Hour Remastered";

    // unattended: --source <the game's folder or disc image> [--target <folder>] [--shortcuts] [--log <file>]; the
    // same checks and steps as the window's, the outcome in the exit code (0 installed, 1 failed, 2 not the release)
    // and the log
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc); argv && argc > 2) {
        std::wstring log;
        bool unattended = false;
        g_shortcut_desktop = g_shortcut_menu = false;
        for (int i = 1; i < argc; i++) {
            std::wstring a = argv[i];
            if (a == L"--source" && i + 1 < argc) g_source = argv[++i], unattended = true;
            else if (a == L"--target" && i + 1 < argc) g_target = argv[++i];
            else if (a == L"--log" && i + 1 < argc) log = argv[++i];
            else if (a == L"--shortcuts") g_shortcut_desktop = g_shortcut_menu = true;
        }
        LocalFree(argv);
        if (unattended) {
            g_source_is_image = fs::is_regular_file(g_source);
            CheckSource();
            int code = 2;
            if (g_source_ok) {
                Install();
                code = g_page == Page::Done ? 0 : 1;
            }
            if (!log.empty())
                if (FILE* f = _wfopen(log.c_str(), L"w")) {
                    std::fprintf(f, "source: %s\ncheck: %s\n", Narrow(g_source).c_str(), g_source_note.c_str());
                    for (const std::string& l : g_log) std::fprintf(f, "%s\n", l.c_str());
                    std::fprintf(f, "exit %d\n", code);
                    std::fclose(f);
                }
            return code;
        }
        g_shortcut_desktop = g_shortcut_menu = true;
    }

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst};
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FinestHourSetup";
    RegisterClassExW(&wc);
    float dpi = GetDpiForSystem() / 96.0f;
    int ww = int(1040 * dpi), wh = int(640 * dpi);
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Finest Hour Remastered Setup", WS_POPUP | WS_MINIMIZEBOX,
                             sx, sy, ww, wh, nullptr, nullptr, inst, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                             &sd, &g_swap, &g_dev, nullptr, &g_ctx)) &&
        FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                             &g_swap, &g_dev, nullptr, &g_ctx))) {
        MessageBoxW(nullptr, L"Direct3D 11 is not available.", kProduct, MB_ICONERROR);
        return 1;
    }
    MakeTarget();
    ShowWindow(g_hwnd, show);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    g_body = FontFromResource(IDR_FONT_BODY, 18 * dpi);
    g_title = FontFromResource(IDR_FONT_TITLE, 58 * dpi);
    if (!g_body) g_body = io.Fonts->AddFontDefault();
    if (!g_title) g_title = g_body;
    io.FontDefault = g_body;
    ImGuiStyle& st = ImGui::GetStyle();
    st.FrameRounding = 3, st.FramePadding = ImVec2(12, 8), st.ItemSpacing = ImVec2(12, 10);
    st.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.19f, 0.15f, 0.92f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.55f, 0.43f, 0.22f, 1.0f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.75f, 0.58f, 0.28f, 1.0f);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.12f, 0.10f, 0.92f);
    st.Colors[ImGuiCol_CheckMark] = kGold;
    st.Colors[ImGuiCol_Text] = kText;
    st.ScaleAllSizes(dpi);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev.Get(), g_ctx.Get());
    LoadArt();
    if (const wchar_t* pg = _wgetenv(L"FHR_SETUP_PAGE")) {
        g_page = Page(std::clamp(_wtoi(pg), 0, 5));
        if (g_page == Page::Source) g_source = L"D:\\Games\\COD", g_source_ok = true, g_source_note = "Your copy is the right release.";
        if (g_page == Page::Install) Log("Installing Finest Hour Remastered..."), g_progress = 0.42;
    }

    MSG msg{};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) goto out;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Frame();
        ImGui::Render();
        const float clear[4] = {0.03f, 0.03f, 0.03f, 1};
        g_ctx->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
        g_ctx->ClearRenderTargetView(g_rtv.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // FHR_SETUP_SHOT=<file.bmp> (FHR_SETUP_PAGE=0..5): the window's picture after a few frames, then quit (docs)
        static int frames = 0;
        if (const wchar_t* shot = _wgetenv(L"FHR_SETUP_SHOT"); shot && ++frames == 20) {
            ComPtr<ID3D11Texture2D> bb, st;
            g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
            D3D11_TEXTURE2D_DESC d;
            bb->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d.MiscFlags = 0;
            g_dev->CreateTexture2D(&d, nullptr, &st);
            g_ctx->CopyResource(st.Get(), bb.Get());
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                FILE* f = _wfopen(shot, L"wb");
                if (f) {
                    uint32_t row = d.Width * 3 + 3 & ~3u, size = 54 + row * d.Height;
                    uint8_t hdr[54] = {'B', 'M'};
                    auto put = [&](int at, uint32_t v) { std::memcpy(hdr + at, &v, 4); };
                    put(2, size), put(10, 54), put(14, 40), put(18, d.Width), put(22, d.Height), put(34, row * d.Height);
                    hdr[26] = 1, hdr[28] = 24;
                    std::fwrite(hdr, 1, 54, f);
                    std::vector<uint8_t> line(row, 0);
                    for (uint32_t y = d.Height; y-- > 0;) {
                        const uint8_t* s = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                        for (uint32_t x = 0; x < d.Width; x++) line[x * 3] = s[x * 4 + 2], line[x * 3 + 1] = s[x * 4 + 1], line[x * 3 + 2] = s[x * 4];
                        std::fwrite(line.data(), 1, row, f);
                    }
                    std::fclose(f);
                }
                g_ctx->Unmap(st.Get(), 0);
            }
            break;
        }
        g_swap->Present(1, 0);
    }
out:
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
