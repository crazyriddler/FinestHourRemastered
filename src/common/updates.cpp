// Updates from the game. At the start, on a thread of its own and never waited for (a bad connection or none delays
// nothing: short timeouts, then silence), the release's update.json (make_release.py: the latest update's version,
// size, SHA-256 and what it brings, by language) is read from GitHub; one newer than the installation's (version.txt
// beside the exe, written by the installer and the updater; none: a build, no check) is offered on the settings
// overlay's page in the main menu. Only when the player chooses it is the updater downloaded (to the Temp folder, its
// size and SHA-256 checked), run on the game's folder (--from-game: it waits for the game to close, shows its
// progress, starts the game again; elevated where the folder is not writable) and the game closed. The next start
// deletes the downloaded updater (it ran from there). "Don't ask again" turns the check off (PC Settings, General).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "common/updates.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/log.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace cod::updates {
namespace {

constexpr wchar_t kHost[] = L"github.com";
constexpr wchar_t kJsonPath[] = L"/crazyriddler/FinestHourRemastered/releases/download/update/update.json";
constexpr wchar_t kExePath[] = L"/crazyriddler/FinestHourRemastered/releases/download/update/";

std::atomic<State> g_state{State::None};
std::atomic<bool> g_cleaned{false};  // the Temp folder's leftovers gone (a download waits for it)
std::atomic<bool> g_shown{false};
std::atomic<float> g_progress{0};
std::mutex g_lock;
std::string g_version, g_error, g_file, g_sha256;
std::string g_notes[7];  // en fr it es pt ru pl
uint64_t g_size = 0;

fs::path TempDir() {
    wchar_t t[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, t);
    return fs::path(t) / L"FinestHourRemastered";
}

// a string's value of a key of the flat JSON object make_release.py writes ("" when not there)
std::string JsonString(const std::string& j, const std::string& key) {
    size_t at = j.find("\"" + key + "\"");
    if (at == std::string::npos) return "";
    at = j.find(':', at);
    if (at == std::string::npos) return "";
    at = j.find('"', at);
    if (at == std::string::npos) return "";
    std::string out;
    for (size_t i = at + 1; i < j.size(); i++) {
        char c = j[i];
        if (c == '"') return out;
        if (c != '\\' || i + 1 >= j.size()) {
            out += c;
            continue;
        }
        char e = j[++i];
        if (e == 'n') out += '\n';
        else if (e == 't') out += '\t';
        else if (e == 'u' && i + 4 < j.size()) {
            unsigned cp = std::strtoul(j.substr(i + 1, 4).c_str(), nullptr, 16);
            i += 4;
            if (cp < 0x80) out += char(cp);
            else if (cp < 0x800) out += char(0xC0 | cp >> 6), out += char(0x80 | (cp & 0x3F));
            else out += char(0xE0 | cp >> 12), out += char(0x80 | (cp >> 6 & 0x3F)), out += char(0x80 | (cp & 0x3F));
        } else out += e;
    }
    return "";
}
uint64_t JsonNumber(const std::string& j, const std::string& key) {
    size_t at = j.find("\"" + key + "\"");
    if (at == std::string::npos) return 0;
    at = j.find(':', at);
    return at == std::string::npos ? 0 : std::strtoull(j.c_str() + at + 1, nullptr, 10);
}

// a GET over HTTPS (redirects followed: GitHub's release files are on another host): the body to sink, false on any
// failure or when cancelled; timeouts short for the check
bool Get(const wchar_t* path, int timeout_ms, const std::function<bool(const char*, size_t, uint64_t)>& sink) {
    bool ok = false;
    // XBE_UPDATE_LOCAL=<folder>: its update.json and updater in place of GitHub's (a test of the whole flow)
    if (const wchar_t* local = _wgetenv(L"XBE_UPDATE_LOCAL")) {
        std::ifstream in(fs::path(local) / fs::path(path).filename(), std::ios::binary);
        if (!in) return false;
        std::vector<char> buf(1 << 20);
        uint64_t total = 0;
        while (in) {
            in.read(buf.data(), std::streamsize(buf.size()));
            if (in.gcount() <= 0) break;
            total += uint64_t(in.gcount());
            if (!sink(buf.data(), size_t(in.gcount()), total)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }
    HINTERNET s = WinHttpOpen(L"FinestHourRemastered", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"FinestHourRemastered", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    HINTERNET c = WinHttpConnect(s, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : nullptr;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                            WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            uint64_t total = 0;
            std::vector<char> buf(1 << 16);
            ok = true;
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(r, buf.data(), DWORD(buf.size()), &got)) {
                    ok = false;
                    break;
                }
                if (!got) break;
                total += got;
                if (!sink(buf.data(), got, total)) {
                    ok = false;
                    break;
                }
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

std::string Sha256(const fs::path& p) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return out;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::ifstream in(p, std::ios::binary);
        std::vector<char> buf(1 << 20);
        while (in) {
            in.read(buf.data(), std::streamsize(buf.size()));
            if (in.gcount() > 0) BCryptHashData(h, reinterpret_cast<PUCHAR>(buf.data()), ULONG(in.gcount()), 0);
        }
        unsigned char d[32];
        if (BCryptFinishHash(h, d, sizeof(d), 0) == 0) {
            char hex[65];
            for (int i = 0; i < 32; i++) std::snprintf(hex + i * 2, 3, "%02x", d[i]);
            out = hex;
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

std::string Installed() {
    std::ifstream f(app::ExeDir() / "version.txt");
    std::string v;
    std::getline(f, v);
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}

void Fail(const std::string& e) {
    {
        std::lock_guard l(g_lock);
        g_error = e;
    }
    LOGW("updates: %s", e.c_str());
    g_state = State::Failed;
}

// the downloaded updater of a time before: deleted (it may still be closing, having started this game)
void Cleanup() {
    struct Done {
        ~Done() { g_cleaned = true; }
    } done;
    const fs::path dir = TempDir();
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    for (int tries = 0; tries < 30; tries++) {
        bool left = false;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            std::error_code ec2;
            fs::remove(e.path(), ec2);
            if (ec2) left = true;
        }
        if (!left) {
            fs::remove(dir, ec);
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void Check() {
    const std::string installed = Installed();
    if (installed.empty() || !app::Get().check_updates) return;
    std::string json;
    const bool got = Get(kJsonPath, 4000, [&](const char* p, size_t n, uint64_t total) {
        json.append(p, n);
        return total < (1 << 20);  // (a small file)
    });
    if (!got) {
        LOGI("updates: no answer (no connection, or none there)");
        return;
    }
    const std::string v = JsonString(json, "version");
    LOGR("updates: installed %s, latest %s", installed.c_str(), v.empty() ? "?" : v.c_str());
    if (v.empty() || v <= installed) return;
    static const char* const langs[7] = {"en", "fr", "it", "es", "pt", "ru", "pl"};
    {
        std::lock_guard l(g_lock);
        g_version = v;
        g_file = JsonString(json, "file");
        g_sha256 = JsonString(json, "sha256");
        g_size = JsonNumber(json, "size");
        for (int i = 0; i < 7; i++) g_notes[i] = JsonString(json, std::string("notes_") + langs[i]);
    }
    if (g_file.empty() || g_file.find_first_of("/\\") != std::string::npos || g_sha256.size() != 64 || !g_size) return;
    g_state = State::Offered;
}

void Download() {
    std::string file, sha;
    uint64_t size;
    {
        std::lock_guard l(g_lock);
        file = g_file, sha = g_sha256, size = g_size;
    }
    while (!g_cleaned.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const fs::path dir = TempDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path part = dir / (fs::path(file).wstring() + L".part"), exe = dir / fs::path(file);
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) return Fail("The Temp folder cannot be written.");
        const std::wstring path = kExePath + fs::path(file).wstring();
        const bool got = Get(path.c_str(), 15000, [&](const char* p, size_t n, uint64_t total) {
            out.write(p, std::streamsize(n));
            g_progress = size ? float(double(total) / double(size)) : 0;
            return bool(out) && total <= size;
        });
        out.close();
        if (!got) {
            fs::remove(part, ec);
            return Fail("The download did not finish. Check your connection and try again.");
        }
    }
    if (fs::file_size(part, ec) != size || Sha256(part) != sha) {
        fs::remove(part, ec);
        return Fail("The downloaded update is damaged (its size or checksum differs). Try again.");
    }
    fs::remove(exe, ec);
    fs::rename(part, exe, ec);
    if (ec) return Fail("The downloaded update cannot be prepared in the Temp folder.");
    g_state = State::Starting;
    // the game's folder: written by the updater, elevated where this account cannot (Program Files)
    const fs::path game = app::ExeDir();
    bool writable = false;
    {
        const fs::path probe = game / L"update_probe.tmp";
        HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (h != INVALID_HANDLE_VALUE) writable = true, CloseHandle(h);
    }
    const std::wstring args = L"--from-game \"" + game.wstring() + L"\" --wait " + std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"FHR_UPDATE_ASKED", nullptr);  // (the game the updater starts again: a fresh start)
    SHELLEXECUTEINFOW si{sizeof(si)};
    si.fMask = SEE_MASK_NOASYNC;
    si.lpVerb = writable ? L"open" : L"runas";
    si.lpFile = exe.c_str();
    si.lpParameters = args.c_str();
    si.lpDirectory = dir.c_str();
    si.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&si)) return Fail("The updater could not be started.");
    LOGR("updates: the updater runs (%s), the game closes", writable ? "as this account" : "elevated");
    app::Save();
    PostMessageW(static_cast<HWND>(app::Window()), WM_CLOSE, 0, 0);
}

}  // namespace

void Start() {
    // (once a session: not again in the processes the game relaunches into once the page was shown; they inherit
    // the environment)
    const bool asked = GetEnvironmentVariableW(L"FHR_UPDATE_ASKED", nullptr, 0) != 0;
    std::thread([asked] {
        Cleanup();
        if (!asked) Check();
    }).detach();
}

State Current() { return g_state.load(); }
bool ShouldShow() { return g_state.load() == State::Offered && !g_shown.load(); }
void Shown() {
    g_shown = true;
    SetEnvironmentVariableW(L"FHR_UPDATE_ASKED", L"1");
}
std::string Version() {
    std::lock_guard l(g_lock);
    return g_version;
}
std::string Notes() {
    const uint32_t lang = app::Get().language;
    const int i = lang == 4 ? 1 : lang == 6 ? 2 : lang == 5 ? 3 : lang == 9 ? 4 : lang == 10 ? 5 : lang == 11 ? 6 : 0;
    std::lock_guard l(g_lock);
    return g_notes[i].empty() ? g_notes[0] : g_notes[i];
}
float Progress() { return g_progress.load(); }
std::string Error() {
    std::lock_guard l(g_lock);
    return g_error;
}
void Accept() {
    const State s = g_state.load();
    if (s != State::Offered && s != State::Failed) return;
    g_state = State::Downloading;
    g_progress = 0;
    std::thread(Download).detach();
}
void Later() {
    if (g_state.load() == State::Offered || g_state.load() == State::Failed) g_state = State::None;
}
void Never() {
    app::Get().check_updates = false;
    app::Save();
    Later();
}

}  // namespace cod::updates
