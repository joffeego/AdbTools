// ADB File Browser — an EUI-NEO desktop file browser for Android devices over ADB.
//
// Features:
//   - Auto-detect the adb executable (Android SDK, PATH, common locations)
//   - List and select connected devices (adb devices)
//   - Browse the device filesystem (ls -la), navigate into folders and up
//   - Editable path bar with "Go"
//   - Upload (adb push), Download (adb pull), Rename (mv), Delete (rm -rf)
//   - New folder (mkdir -p), right-click context menu, confirm dialog, toasts
//
// Build (from the repo root):
//   cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
//   cmake --build build --parallel
//
// Run:  .\build\adb_browser.exe

#include "eui_neo.h"

#include "core/adb.h"
#include "core/adbpath.h"
#include "core/appinfo.h"
#include "core/batch.h"
#include "core/fileio.h"
#include "core/package.h"
#include "core/paths.h"
#include "core/process.h"
#include "core/sha256.h"
#include "core/store.h"
#include "core/strings.h"
#include "core/version.h"


#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>
#endif

// Exception-isolated std::regex wrapper (regex_wrap.cpp). Returns nullptr /
// -1 on invalid patterns instead of throwing.
extern "C" void* adbRegexCompile(const char* pattern, int ignoreCase);
extern "C" int adbRegexSearchHandle(void* handle, const char* text);
extern "C" void adbRegexFree(void* handle);

namespace app {
namespace {

// The app's pure logic lives in adb::core (see core/*.h) so it can be unit
// tested without the GUI. This alias keeps the call sites readable and makes it
// obvious which helpers are now part of that separately testable module.
// It is declared inside `namespace app` on purpose: a namespace alias declared
// at global scope is looked up from here as app::<target>, which does not exist.
namespace core = ::adb::core;

// =============================================================================
// Colors (dark theme)
// =============================================================================
// Theme colors (mutable so the settings dialog can switch dark/light + accent).
eui::Color kBackground   {0.078f, 0.086f, 0.110f, 1.0f};
eui::Color kSurface      {0.125f, 0.137f, 0.169f, 1.0f};
eui::Color kSurfaceHover {0.176f, 0.196f, 0.239f, 1.0f};
eui::Color kSurfaceAct   {0.224f, 0.247f, 0.298f, 1.0f};
eui::Color kInk          {0.922f, 0.941f, 0.965f, 1.0f};
eui::Color kMuted        {0.545f, 0.600f, 0.675f, 1.0f};
eui::Color kBorder       {0.235f, 0.267f, 0.329f, 1.0f};
eui::Color kAccent       {0.250f, 0.545f, 0.945f, 1.0f};
eui::Color kAccentSoft   {0.150f, 0.215f, 0.320f, 1.0f};
eui::Color kTitleBar     {0.102f, 0.114f, 0.145f, 1.0f};

constexpr eui::Color kGreen        {0.290f, 0.720f, 0.470f, 1.0f};
constexpr eui::Color kRose         {0.910f, 0.310f, 0.380f, 1.0f};
constexpr eui::Color kAmber        {0.960f, 0.690f, 0.250f, 1.0f};
constexpr eui::Color kClear        {0.0f, 0.0f, 0.0f, 0.0f};
constexpr eui::Color kWhite        {1.0f, 1.0f, 1.0f, 1.0f};
constexpr eui::Color kCloseHover   {0.769f, 0.169f, 0.110f, 1.0f};
constexpr eui::Color kClosePressed {0.580f, 0.100f, 0.070f, 1.0f};

constexpr float kTitleBarHeight = 36.0f;
constexpr float kMirrorWindowWidth = 320.0f;
constexpr float kMirrorWindowHeight = 640.0f;

// The version and update repository live in core/appinfo.h: the CLI prints the
// same version, and two copies would eventually disagree about which release is
// installed.
using adb::core::kAppUpdateRepo;
using adb::core::kAppVersion;

constexpr float kScrollbarWidth = 10.0f;
constexpr float kRowHeight = 36.0f;


// Rough visual width estimate for laying out breadcrumb segments (ASCII vs CJK).
float approxTextWidth(const std::string& s, float fontSize) {
    float width = 0.0f;
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            width += fontSize * 0.62f;
            ++i;
        } else {
            width += fontSize;
            int extra = 0;
            if ((c & 0xE0) == 0xC0) extra = 1;
            else if ((c & 0xF0) == 0xE0) extra = 2;
            else if ((c & 0xF8) == 0xF0) extra = 3;
            i += 1 + static_cast<std::size_t>(extra);
        }
    }
    return width;
}

// Subprocess execution now lives in core/process.cpp so the GUI and the CLI
// share one implementation (and one place to fix process-handling bugs).
using adb::core::ProcessResult;
using adb::core::runProcess;
#ifdef _WIN32
using adb::core::quoteWinArg;
using adb::core::toUtf8;
using adb::core::toWide;
#endif


// =============================================================================
// Data model & adb output parsing - see core/adb.h
// =============================================================================
// The structs and parsers live in core/adb.cpp so they can be unit tested
// against real adb output; the using-declarations keep the call sites here
// unchanged.
using adb::core::Device;
using adb::core::FsEntry;
using adb::core::ListingResult;

// =============================================================================
// ADB discovery - see core/adbpath.h
// =============================================================================
// findAdb() and defaultDownloadDir() moved to core/adbpath.cpp so the GUI and
// the CLI resolve adb (and the download folder) identically. executableDir()
// stays here because this file defines it below and needs it for the settings
// paths; core has its own copy for the CLI.
using adb::core::defaultDownloadDir;
using adb::core::findAdb;

// Defined further down (it needs GetModuleFileNameW and the settings paths use
// it early), declared here so everything above the definition sees it.
std::string executableDir();


// =============================================================================
// Application state
// =============================================================================
struct AppSettings {
    std::string fontFamily = "Microsoft YaHei";
    std::string fontFile = "C:/Windows/Fonts/msyh.ttc";
    int fontWeight = 400;
    float uiScale = 1.0f;
    bool darkMode = true;
    float accentR = 0.25f;
    float accentG = 0.545f;
    float accentB = 0.945f;
    float fontSizeScale = 1.0f;
    int mirrorW = 320;
    int mirrorH = 640;
    int windowW = 1000;
    int windowH = 520;
};

struct AppState {
    bool initialized = false;

    std::string adbPath;
    bool adbFound = false;
    std::string adbVersion;
    bool deviceLoading = false;
    std::vector<Device> devices;
    std::string selectedDevice;

    std::string currentPath = "/";
    std::string pathInput = "/";
    bool pathEditing = false;
    std::vector<std::string> pathSuggestions;
    std::vector<FsEntry> entries;
    bool loading = false;
    bool busy = false;
    bool dirWritable = false;
    std::string selectedEntry;
    std::string downloadDir;

    std::string lastClickedName;
    std::chrono::steady_clock::time_point lastClickTime;
    std::chrono::steady_clock::time_point lastTitlePressTime;
    bool lastClickCtrl = false;

    eui::Signal<float> fileListScroll;
    int listGeneration = 0;

    bool settingsOpen = false;
    std::vector<std::string> fontList;
    bool fontsLoaded = false;
    bool weightDropdownOpen = false;
    bool scaleDropdownOpen = false;
    bool fontSizeDropdownOpen = false;

    bool toastVisible = false;
    std::string toastTitle;
    std::string toastMessage;

    bool deviceMenuOpen = false;
    float deviceMenuX = 0.0f;
    float deviceMenuY = 0.0f;

    bool rowMenuOpen = false;
    float rowMenuX = 0.0f;
    float rowMenuY = 0.0f;

    bool bookmarkMenuOpen = false;
    float bookmarkMenuX = 0.0f;
    float bookmarkMenuY = 0.0f;

    bool commandMenuOpen = false;
    float commandMenuX = 0.0f;
    float commandMenuY = 0.0f;

    bool bookmarkManageOpen = false;
    bool commandManageOpen = false;
    bool commandOutputOpen = false;
    std::string commandOutputTitle;
    std::string commandOutputText;
    std::string commandNameInput;
    std::string commandCmdInput;
    bool commandShellType = true;

    bool confirmDialogOpen = false;
    std::string confirmTitle = "删除";
    std::string confirmMessage;
    bool promptOpen = false;
    std::string promptTitle;
    std::string promptValue;
    int promptMode = 0;  // 0 = new folder, 1 = rename

    // Sorting & filtering
    int sortColumn = 0;      // 0 = name, 1 = size, 2 = date
    bool sortAscending = true;
    bool showHidden = false;

    // Path history (back / forward)
    std::vector<std::string> pathBack;
    std::vector<std::string> pathForward;

    // Device-side clipboard (copy / move)
    bool clipboardCut = false;
    std::string clipboardPath;
    std::string clipboardName;

    // Multi-select (names of selected entries)
    std::set<std::string> selectedSet;

    // Transfer progress (0..1, or -1 while indeterminate)
    float progress = 0.0f;
    std::string progressLabel;

    // Per-device last path (serial -> path)
    std::unordered_map<std::string, std::string> lastPaths;

    // Directory cache: key = serial + "|" + path
    std::unordered_map<std::string, ListingResult> dirCache;

    // Device info / logcat / text preview
    bool deviceInfoOpen = false;
    std::string deviceInfoText;
    bool logcatOpen = false;
    std::string logcatFilter;
    std::string logcatRegexError;      // non-empty when the regex is invalid
    bool logcatStreaming = false;
    long long logcatLastSeq = 0;       // last-seen line sequence (auto-scroll)
    eui::Signal<float> logcatScroll;
    void* logcatRegexHandle = nullptr; // compiled std::regex (owned)
    std::string logcatCompiledFilter;  // filter the handle was built from
    bool textPreviewOpen = false;
    std::string textPreviewPath;
    std::string textPreviewContent;
    std::string textPreviewRemote;

    // scrcpy screen mirroring (embedded window)
    bool scrcpyOpen = false;
    float logicalW = 1600.0f;
    float logicalH = 1000.0f;

    // File search filter + filtered view.
    std::string fileFilter;
    bool searchOpen = false;   // search box visible (toggled with Ctrl+F)
    std::vector<FsEntry> displayEntries;

    // App management.
    bool appManageOpen = false;
    std::string appListText;
    std::string appFilter;
    std::vector<std::string> appPackages;
    std::string appSelectedPackage;

    // Image preview.
    bool imagePreviewOpen = false;
    std::string imagePreviewRemote;
    std::string imagePreviewLocal;

    // File properties.
    bool propertiesOpen = false;
    std::string propertiesText;

    // Shortcuts help.
    bool helpOpen = false;

    // Update checker (adb / scrcpy).
    bool updateOpen = false;
    bool updateChecking = false;
    bool updateWorking = false;
    std::string updateStatus;

    // First-run setup. adb and scrcpy no longer ship with the installer (see
    // g_pendingAdbInstall), so the app offers to fetch whichever is missing the
    // first time it starts - otherwise a fresh install silently cannot reach a
    // device until the user finds "check for updates" on their own.
    bool setupOpen = false;
    bool setupDontAsk = false;         // the checkbox, as currently ticked
    bool setupPromptDismissed = false; // persisted: never show the dialog again

    // Generic confirm dialog (title/message + primary action).
    std::string confirmPrimaryText = "删除";
    std::function<void()> confirmAction;
};

AppState state;
AppSettings settings;

// Results of the last "check for updates" pass (kept separate from AppState so
// the version strings can be shared freely by the check/install async tasks).
struct UpdateInfo {
    std::string adbCurrent;
    std::string adbLatest;
    std::string adbUrl;
    std::string adbSha1;  // Google publishes SHA-1 digests in repository2-1.xml
    bool adbUpdate = false;
    std::string scrcpyCurrent;
    std::string scrcpyLatest;
    std::string scrcpyUrl;
    std::string scrcpySha256;
    bool scrcpyUpdate = false;
    std::string appCurrent;
    std::string appLatest;
    std::string appUrl;
    std::string appSha256;  // expected sha256 of the row named in appRow, when published
    std::string appRow;     // e.g. "adb_browser.exe" or "AdbFileBrowser-windows-x64.zip"
    bool appUpdate = false;
    std::string error;
};

UpdateInfo g_update;

// which component a download request refers to.
enum class PendingInstall { adb, scrcpy };

// The release packages deliberately ship no third-party binaries (bundled
// adb.exe / scrcpy.exe are what antivirus products flag, taking our exe down
// with them), so adb and scrcpy are fetched on demand instead. A download can be
// requested from outside the update dialog - the first-run setup dialog, the
// "no adb installed" empty state, the mirroring button - where the download URL
// may not be known yet, because it comes from the update check.
//
// One request can cover both components (the setup dialog's "download
// everything"), and the two downloads cannot run at the same time, so this is a
// queue: a request that cannot start yet is remembered, and whichever handler
// finishes next starts the remaining one.
bool g_pendingAdbInstall = false;
bool g_pendingScrcpyInstall = false;

void toast(const std::string& title, const std::string& message);
void checkForUpdates();
void startUpdateAdb();
void startUpdateScrcpy();

// Starts the next queued install whose download URL is known. Returns true when
// one was started.
bool advanceInstallQueue() {
    if (state.updateWorking || state.updateChecking) return false;
    if (g_pendingAdbInstall && !g_update.adbUrl.empty() && !g_update.adbLatest.empty()) {
        g_pendingAdbInstall = false;
        startUpdateAdb();
        return true;
    }
    if (g_pendingScrcpyInstall && !g_update.scrcpyUrl.empty() && !g_update.scrcpyLatest.empty()) {
        g_pendingScrcpyInstall = false;
        startUpdateScrcpy();
        return true;
    }
    return false;
}

// Requests the download for `what`, fetching the version information first when
// the URL is not known yet. Returns true when the install has started; false
// means "not yet" - either a check or the other component's download is running
// and will pick this up, or nothing could be started at all.
bool ensureToolsDownloadable(PendingInstall what) {
    if (what == PendingInstall::adb) {
        g_pendingAdbInstall = true;
    } else {
        g_pendingScrcpyInstall = true;
    }
    if (advanceInstallQueue()) return true;
    if (state.updateWorking) return false;  // the running install will chain to us
    if (!state.updateChecking) checkForUpdates();
    return false;
}

// Download progress shared with the worker thread via atomics.
std::atomic<long long> g_dlTotal{0};
std::atomic<long long> g_dlReceived{0};
std::atomic<int> g_dlStage{0};  // 0 idle, 1 download, 2 extract, 3 install

// logcat streaming buffer + lifecycle (fed by a detached reader thread).
std::mutex g_logcatMutex;
std::deque<std::string> g_logcatLines;
std::atomic<long long> g_logcatSeq{0};
std::atomic<bool> g_logcatStop{false};
std::atomic<bool> g_logcatRunning{false};
#ifdef _WIN32
// Handle of the running `adb logcat` process. Written by the logcat reader
// thread (#ifdef branch below) and read by stopLogcat() on the UI thread, so it
// is atomic: a plain HANDLE read concurrently with a write is a data race, and
// the reads here must never be able to take a stale value.
static std::atomic<HANDLE> g_logcatProcess{nullptr};
#endif

components::theme::ThemeColorTokens themeTokens() {
    components::theme::ThemeColorTokens t = components::theme::dark();
    t.background = kBackground;
    t.primary = kAccent;
    t.surface = kSurface;
    t.surfaceHover = kSurfaceHover;
    t.surfaceActive = kSurfaceAct;
    t.text = kInk;
    t.border = kBorder;
    return t;
}

const FsEntry* findSelected() {
    for (const FsEntry& e : state.entries) {
        if (e.name == state.selectedEntry) return &e;
    }
    return nullptr;
}

// Reset the file list scroll to the top. Bumping the generation gives the
// virtual list a fresh scroll-state id so the runtime offset also resets.
void resetListScroll() {
    state.fileListScroll.set(0.0f);
    ++state.listGeneration;
}

void sortEntries(std::vector<FsEntry>& entries) {
    if (!state.showHidden) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const FsEntry& e) {
            return !e.name.empty() && e.name[0] == '.';
        }), entries.end());
    }
    const int col = state.sortColumn;
    const bool asc = state.sortAscending;
    std::sort(entries.begin(), entries.end(), [col, asc](const FsEntry& a, const FsEntry& b) {
        if (a.isDir != b.isDir) return a.isDir;  // directories first
        int cmp = 0;
        if (a.isDir) {
            cmp = core::lower(a.name).compare(core::lower(b.name));
        } else if (col == 1) {  // size
            if (a.size != b.size) cmp = (a.size < b.size) ? -1 : 1;
            else cmp = core::lower(a.name).compare(core::lower(b.name));
        } else if (col == 2) {  // date
            cmp = a.date.compare(b.date);
            if (cmp == 0) cmp = core::lower(a.name).compare(core::lower(b.name));
        } else {  // name
            cmp = core::lower(a.name).compare(core::lower(b.name));
        }
        return asc ? (cmp < 0) : (cmp > 0);
    });
}

void applyFileFilter() {
    if (state.fileFilter.empty()) {
        state.displayEntries = state.entries;
        return;
    }
    const std::string needle = core::lower(state.fileFilter);
    state.displayEntries.clear();
    for (const FsEntry& e : state.entries) {
        if (core::lower(e.name).find(needle) != std::string::npos) {
            state.displayEntries.push_back(e);
        }
    }
}

void applySort() {
    sortEntries(state.entries);
    resetListScroll();
    applyFileFilter();
}

void toast(const std::string& title, const std::string& message) {
    state.toastTitle = title;
    state.toastMessage = message;
    state.toastVisible = true;
}

// -----------------------------------------------------------------------------
// Batch-operation reporting
//
// The accounting itself (per-item outcome, totals, "was every item attempted")
// lives in core/batch.cpp so the GUI and the CLI share one implementation and it
// can be unit tested. These helpers only turn an outcome into app wording.
// -----------------------------------------------------------------------------
using adb::core::BatchOutcome;

std::string batchSummary(const std::string& verb, const BatchOutcome& outcome) {
    return verb + "：" + outcome.text();
}

// A batch item is identified by its path on the device, but the label shown to
// the user should be just the file name.
BatchOutcome makeBatchOutcome(const std::vector<std::string>& names) {
    std::vector<std::string> labels;
    labels.reserve(names.size());
    for (const std::string& name : names) {
        std::filesystem::path p(name);
        labels.push_back(p.filename().string().empty() ? name : p.filename().string());
    }
    return BatchOutcome(std::move(labels));
}


std::vector<std::string> selectedNames() {
    std::vector<std::string> names;
    if (!state.selectedSet.empty()) {
        for (const auto& n : state.selectedSet) names.push_back(n);
    } else if (!state.selectedEntry.empty()) {
        names.push_back(state.selectedEntry);
    }
    return names;
}

// Title-bar press: single press drags the window, double press maximizes.
void onTitleBarPress() {
    const auto now = std::chrono::steady_clock::now();
    const bool isDouble = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - state.lastTitlePressTime).count() < 450;
    state.lastTitlePressTime = now;
    if (isDouble) {
        app::toggleMaximizeWindow();
    } else {
        app::startWindowDrag();
    }
}

// =============================================================================
// Settings (system font family / weight / UI scale)
// =============================================================================
std::string settingsFilePath() {
    // Store next to the executable (not the CWD) so settings survive updates
    // and are consistent regardless of how the app is launched.
    return executableDir() + "\\adb_file_browser_settings.txt";
}

void saveSettings() {
    // The key=value format and its parsing live in core/store.cpp (round-trip
    // tested); this only decides which keys the app persists.
    const std::vector<std::pair<std::string, std::string>> values = {
        {"fontFamily", settings.fontFamily},
        {"fontWeight", std::to_string(settings.fontWeight)},
        {"uiScale", std::to_string(settings.uiScale)},
        {"darkMode", settings.darkMode ? "1" : "0"},
        {"fontScale", std::to_string(settings.fontSizeScale)},
        {"accent", std::to_string(settings.accentR) + "," + std::to_string(settings.accentG) + "," +
                       std::to_string(settings.accentB)},
        {"sortColumn", std::to_string(state.sortColumn)},
        {"sortAscending", state.sortAscending ? "1" : "0"},
        {"showHidden", state.showHidden ? "1" : "0"},
        {"selectedDevice", state.selectedDevice},
        {"mirrorW", std::to_string(settings.mirrorW)},
        {"mirrorH", std::to_string(settings.mirrorH)},
        {"windowW", std::to_string(settings.windowW)},
        {"windowH", std::to_string(settings.windowH)},
        {"setupPromptDismissed", state.setupPromptDismissed ? "1" : "0"},
    };
    core::writeFileAtomic(settingsFilePath(), core::serializeKeyValues(values));
}

#ifdef _WIN32
static std::vector<std::string> g_fontNames;

int CALLBACK enumFontFamiliesProc(const LOGFONTW* logFont, const TEXTMETRICW*, DWORD, LPARAM) {
    std::string name = core::toUtf8(std::wstring(logFont->lfFaceName));
    if (name.empty() || name[0] == '@') return 1;
    if (std::find(g_fontNames.begin(), g_fontNames.end(), name) == g_fontNames.end()) {
        g_fontNames.push_back(name);
    }
    return 1;
}

std::vector<std::string> listSystemFonts() {
    g_fontNames.clear();
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(dc, &lf, enumFontFamiliesProc, 0, 0);
    ReleaseDC(nullptr, dc);
    std::vector<std::string> names = g_fontNames;
    std::sort(names.begin(), names.end());
    return names;
}

// Resolve a font family to its font file for the given weight (300 light,
// 400 regular, 700 bold). Reads both HKCU and HKLM (per-user fonts are in
// HKCU and often point to a full path outside C:\Windows\Fonts).
std::string resolveFontFileForFamily(const std::string& family, int weight) {
    std::string regularFile, boldFile, lightFile;
    const std::string target = core::lower(family);

    auto readHive = [&](HKEY root, const wchar_t* subkey) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            return;
        }
        DWORD index = 0;
        for (;;) {
            wchar_t valueName[512];
            wchar_t valueData[512];
            DWORD nameSize = 512;
            DWORD dataSize = 512 * sizeof(wchar_t);
            const LONG result = RegEnumValueW(key, index, valueName, &nameSize, nullptr, nullptr,
                                              reinterpret_cast<LPBYTE>(valueData), &dataSize);
            if (result != ERROR_SUCCESS) break;
            ++index;

            const std::string name = core::lower(core::toUtf8(valueName));
            const std::string data = core::toUtf8(valueData);
            const std::size_t paren = name.find(" (");
            const std::string display = paren == std::string::npos ? name : name.substr(0, paren);

            // The target family must be a leading word of the display name.
            if (display != target &&
                display.rfind(target + " ", 0) != 0 &&
                display.rfind(target + " &", 0) != 0 &&
                display.rfind(target + "&", 0) != 0) {
                continue;
            }
            const std::string rest = display.substr(target.size());
            const bool isBold = rest.find("bold") != std::string::npos ||
                                rest.find("粗") != std::string::npos;
            const bool isLight = rest.find("light") != std::string::npos ||
                                 rest.find("细") != std::string::npos;
            const bool isItalic = rest.find("italic") != std::string::npos ||
                                  rest.find("oblique") != std::string::npos;
            if (isItalic) continue;
            if (isLight && lightFile.empty()) lightFile = data;
            else if (isBold && boldFile.empty()) boldFile = data;
            else if (!isBold && !isLight && regularFile.empty()) regularFile = data;
        }
        RegCloseKey(key);
    };

    readHive(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts");
    readHive(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts");

    std::string file;
    if (weight >= 700 && !boldFile.empty()) file = boldFile;
    else if (weight <= 300 && !lightFile.empty()) file = lightFile;
    else file = regularFile.empty() ? (boldFile.empty() ? "" : boldFile) : regularFile;
    if (file.empty()) return "";

    // The registry value is either a bare file name (system fonts, relative to
    // C:\Windows\Fonts) or a full path (per-user fonts).
    const bool absolute = (file.size() >= 2 &&
                           std::isalpha(static_cast<unsigned char>(file[0])) && file[1] == ':') ||
                          (file.size() >= 2 && file[0] == '\\' && file[1] == '\\') ||
                          (file.size() >= 1 && file[0] == '/');
    return absolute ? file : "C:/Windows/Fonts/" + file;
}
#endif

struct AccentPreset {
    const char* name;
    float r;
    float g;
    float b;
};

const AccentPreset kAccentPresets[] = {
    {"蓝", 0.25f, 0.545f, 0.945f},
    {"青", 0.05f, 0.60f, 0.55f},
    {"绿", 0.18f, 0.63f, 0.35f},
    {"橙", 0.96f, 0.55f, 0.15f},
    {"红", 0.91f, 0.31f, 0.38f},
    {"紫", 0.55f, 0.35f, 0.85f},
};

void applyTheme() {
    if (settings.darkMode) {
        kBackground   = {0.078f, 0.086f, 0.110f, 1.0f};
        kSurface      = {0.125f, 0.137f, 0.169f, 1.0f};
        kSurfaceHover = {0.176f, 0.196f, 0.239f, 1.0f};
        kSurfaceAct   = {0.224f, 0.247f, 0.298f, 1.0f};
        kInk          = {0.922f, 0.941f, 0.965f, 1.0f};
        kMuted        = {0.545f, 0.600f, 0.675f, 1.0f};
        kBorder       = {0.235f, 0.267f, 0.329f, 1.0f};
        kTitleBar     = {0.102f, 0.114f, 0.145f, 1.0f};
    } else {
        kBackground   = {0.945f, 0.949f, 0.957f, 1.0f};
        kSurface      = {1.0f, 1.0f, 1.0f, 1.0f};
        kSurfaceHover = {0.900f, 0.906f, 0.918f, 1.0f};
        kSurfaceAct   = {0.840f, 0.850f, 0.865f, 1.0f};
        kInk          = {0.070f, 0.090f, 0.120f, 1.0f};
        kMuted        = {0.420f, 0.450f, 0.490f, 1.0f};
        kBorder       = {0.780f, 0.795f, 0.815f, 1.0f};
        kTitleBar     = {0.910f, 0.920f, 0.938f, 1.0f};
    }
    kAccent = {settings.accentR, settings.accentG, settings.accentB, 1.0f};
    kAccentSoft = eui::mixColor(kAccent, kBackground, 0.72f);
    // Theme globals changed: also repaint the separate mirror (投屏) window.
    app::requestUpdate();
}

float windowDpiScale() {
#ifdef _WIN32
    HDC dc = GetDC(nullptr);
    if (dc != nullptr) {
        const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        if (dpi > 0) return static_cast<float>(dpi) / 96.0f;
    }
#endif
    return 1.0f;
}

void saveWindowState() {
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(app::mainWindowHwnd());
    if (hwnd == nullptr || app::isWindowMaximized()) return;
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    // Persist a logical (DPI-independent) size so it restores correctly on a
    // different monitor/DPI than the one it was saved on.
    const float scale = windowDpiScale();
    settings.windowW = static_cast<int>((rc.right - rc.left) / scale);
    settings.windowH = static_cast<int>((rc.bottom - rc.top) / scale);
    saveSettings();
#endif
}

void clampToWorkArea(int& w, int& h) {
#ifdef _WIN32
    RECT wa{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        const int maxW = wa.right - wa.left;
        const int maxH = wa.bottom - wa.top;
        if (maxW > 0 && maxH > 0) {
            // Leave a small desktop margin so the window never literally fills
            // the whole work-area height/width.
            if (w > maxW - 32) w = std::max(0, maxW - 32);
            if (h > maxH - 48) h = std::max(0, maxH - 48);
        }
    }
#endif
}

void restoreWindowState() {
    if (settings.windowW >= 400 && settings.windowH >= 300) {
        const float scale = windowDpiScale();
        int w = static_cast<int>(settings.windowW * scale);
        int h = static_cast<int>(settings.windowH * scale);
        clampToWorkArea(w, h);
        app::setWindowSize(w, h);
    }
}

// Set the minimum window size so the fixed toolbar layout can't be shrunk into
// overlap. The toolbar needs ~840 logical width (after the compaction tweaks),
// and the layout is in logical units, so the physical minimum scales with both
// the monitor DPI and the UI scale setting.
void applyMinWindowSize() {
#ifdef _WIN32
    const float scale = windowDpiScale() * settings.uiScale;
    app::setMinWindowSize(static_cast<int>(880.0f * scale),
                          static_cast<int>(420.0f * scale));
#endif
}

void setDarkMode(bool dark) {
    settings.darkMode = dark;
    applyTheme();
    saveSettings();
}

void setAccentColor(float r, float g, float b) {
    settings.accentR = r;
    settings.accentG = g;
    settings.accentB = b;
    applyTheme();
    saveSettings();
}

void applySettingsNow() {
    applyTheme();
    if (!settings.fontFile.empty()) {
        app::setDefaultTextFont(settings.fontFile);
    }
    app::setFontScale(settings.fontSizeScale);
    app::setUiScale(settings.uiScale);
    saveSettings();
}

void loadSettings() {
    std::ifstream in(settingsFilePath());
    if (!in) return;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::unordered_map<std::string, std::string> stored = core::parseKeyValues(buffer.str());
    for (const auto& entry : stored) {
        const std::string& key = entry.first;
        const std::string& value = entry.second;
        if (key == "fontFamily") settings.fontFamily = value;
        else if (key == "fontWeight") settings.fontWeight = std::atoi(value.c_str());
        else if (key == "uiScale") settings.uiScale = static_cast<float>(std::atof(value.c_str()));
        else if (key == "darkMode") settings.darkMode = (std::atoi(value.c_str()) != 0);
        else if (key == "fontScale") settings.fontSizeScale = static_cast<float>(std::atof(value.c_str()));
        else if (key == "accent") {
            const std::size_t c1 = value.find(',');
            const std::size_t c2 = value.find(',', c1 + 1);
            if (c1 != std::string::npos && c2 != std::string::npos) {
                settings.accentR = static_cast<float>(std::atof(value.substr(0, c1).c_str()));
                settings.accentG = static_cast<float>(std::atof(value.substr(c1 + 1, c2 - c1 - 1).c_str()));
                settings.accentB = static_cast<float>(std::atof(value.substr(c2 + 1).c_str()));
            }
        }
        else if (key == "sortColumn") state.sortColumn = std::atoi(value.c_str());
        else if (key == "sortAscending") state.sortAscending = (std::atoi(value.c_str()) != 0);
        else if (key == "showHidden") state.showHidden = (std::atoi(value.c_str()) != 0);
        else if (key == "selectedDevice") state.selectedDevice = value;
        else if (key == "mirrorW") settings.mirrorW = std::atoi(value.c_str());
        else if (key == "mirrorH") settings.mirrorH = std::atoi(value.c_str());
        else if (key == "windowW") settings.windowW = std::atoi(value.c_str());
        else if (key == "windowH") settings.windowH = std::atoi(value.c_str());
        else if (key == "setupPromptDismissed") state.setupPromptDismissed = (std::atoi(value.c_str()) != 0);
    }
#ifdef _WIN32
    settings.fontFile = resolveFontFileForFamily(settings.fontFamily, settings.fontWeight);
#else
    settings.fontFile.clear();
#endif
    if (settings.fontFile.empty()) {
        settings.fontFamily = "Microsoft YaHei";
        settings.fontFile = "C:/Windows/Fonts/msyh.ttc";
        settings.fontWeight = 400;
    }
    if (settings.uiScale < 0.5f || settings.uiScale > 2.5f) {
        settings.uiScale = 1.0f;
    }
}

void openSettingsDialog() {
    state.settingsOpen = true;
    if (!state.fontsLoaded) {
        state.fontsLoaded = true;
        app::async::runOnce(
            "fonts.enum",
#ifdef _WIN32
            []() -> app::async::Result<std::vector<std::string>> {
                return app::async::success(listSystemFonts());
            },
#else
            []() -> app::async::Result<std::vector<std::string>> {
                return app::async::success(std::vector<std::string>{"Sans", "Serif", "Monospace"});
            },
#endif
            [](const app::async::Result<std::vector<std::string>>& result) {
                if (result.ok) state.fontList = result.value;
            });
    }
}

void selectFontFamily(const std::string& family) {
    settings.fontFamily = family;
#ifdef _WIN32
    settings.fontFile = resolveFontFileForFamily(family, settings.fontWeight);
#endif
    applySettingsNow();
}

void selectFontWeight(int weight) {
    settings.fontWeight = weight;
#ifdef _WIN32
    settings.fontFile = resolveFontFileForFamily(settings.fontFamily, weight);
#endif
    applySettingsNow();
}

void selectUiScale(float scale) {
    settings.uiScale = scale;
    applySettingsNow();
    applyMinWindowSize();
}

void selectFontScale(float scale) {
    settings.fontSizeScale = scale;
    applySettingsNow();
}

// =============================================================================
// Bookmarks (quick paths) & Commands
// =============================================================================
// The record types and their text formats live in core/store.h so the
// serialisation can be round-trip tested; only the file paths and the in-memory
// lists belong here.
using adb::core::Bookmark;
using adb::core::CommandEntry;

std::vector<Bookmark> bookmarks;
std::vector<CommandEntry> commands;

std::string bookmarksFilePath() { return executableDir() + "\\adb_file_browser_bookmarks.txt"; }
std::string commandsFilePath() { return executableDir() + "\\adb_file_browser_commands.txt"; }

void saveBookmarks() {
    core::writeFileAtomic(bookmarksFilePath(), core::serializeBookmarks(bookmarks));
}

void loadBookmarks() {
    bookmarks.clear();
    std::ifstream in(bookmarksFilePath());
    if (!in) return;
    std::ostringstream text;
    text << in.rdbuf();
    bookmarks = core::parseBookmarks(text.str());
}

void saveCommands() {
    core::writeFileAtomic(commandsFilePath(), core::serializeCommands(commands));
}

void loadCommands() {
    commands.clear();
    std::ifstream in(commandsFilePath());
    if (!in) return;
    std::ostringstream text;
    text << in.rdbuf();
    commands = core::parseCommands(text.str());
}

std::string lastPathsFilePath() { return executableDir() + "\\adb_file_browser_lastpaths.txt"; }

void saveLastPaths() {
    core::writeFileAtomic(lastPathsFilePath(), core::serializeLastPaths(state.lastPaths));
}

void loadLastPaths() {
    state.lastPaths.clear();
    std::ifstream in(lastPathsFilePath());
    if (!in) return;
    std::ostringstream text;
    text << in.rdbuf();
    state.lastPaths = core::parseLastPaths(text.str());
}

void rememberLastPath() {
    if (state.selectedDevice.empty() || state.currentPath.empty()) return;
    state.lastPaths[state.selectedDevice] = state.currentPath;
    saveLastPaths();
}

void addBookmark(const std::string& name, const std::string& path) {
    for (const Bookmark& b : bookmarks) {
        if (b.path == path) return;
    }
    bookmarks.push_back({name, path});
    saveBookmarks();
}

void removeBookmark(std::size_t index) {
    if (index < bookmarks.size()) {
        bookmarks.erase(bookmarks.begin() + index);
        saveBookmarks();
    }
}

void addCommand(const std::string& name, bool shell, const std::string& cmd) {
    commands.push_back({name, shell, cmd});
    saveCommands();
}

void removeCommand(std::size_t index) {
    if (index < commands.size()) {
        commands.erase(commands.begin() + index);
        saveCommands();
    }
}

void runCommandEntry(const CommandEntry& cmd) {
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    app::async::runOnce(
        "run.command",
        [adb, serial, cmd]() -> app::async::Result<std::string> {
            ProcessResult r;
            if (cmd.shell) {
                if (serial.empty()) {
                    return app::async::failure<std::string>("未选择设备");
                }
                r = runProcess(adb, {"-s", serial, "shell", cmd.command}, 60000);
            } else {
                r = runProcess("cmd", {"/c", cmd.command}, 60000);
            }
            const std::string lowerCmd = core::lower(cmd.command);
            const bool rebootsDevice = lowerCmd.find("reboot") != std::string::npos;
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                if (rebootsDevice) {
                    // adb reboot / adb shell reboot drop the connection while the
                    // device reboots, which often surfaces as exit code 255
                    // ("device offline" / "closed") even though it succeeded.
                    const std::string lowerOut = core::lower(msg);
                    const bool deviceWasNotReady =
                        lowerOut.find("unauthorized") != std::string::npos ||
                        lowerOut.find("not found") != std::string::npos ||
                        lowerOut.find("no devices") != std::string::npos ||
                        lowerOut.find("more than one") != std::string::npos ||
                        lowerOut.find("offline") != std::string::npos;
                    if (!deviceWasNotReady) {
                        return app::async::success<std::string>(
                            "重启命令已发送，设备正在重启。\n（重启时连接会断开，属正常现象。）");
                    }
                }
                if (msg.empty()) {
                    msg = "命令失败（退出码 " + std::to_string(r.exitCode) + "）";
                    if (r.exitCode == 255) {
                        msg += "\n\n设备可能离线或未授权：请检查设备上的 USB 调试授权弹窗，"
                               "或运行 adb kill-server 后重试。";
                    }
                }
                msg = "命令：" + cmd.command + "\n\n" + msg;
                return app::async::failure<std::string>(msg);
            }
            return app::async::success(core::trim(r.out));
        },
        [](const app::async::Result<std::string>& result) {
            state.commandOutputTitle = result.ok ? "命令输出" : "命令失败";
            state.commandOutputText = result.ok
                ? (result.value.empty() ? "(无输出)" : result.value)
                : result.error;
            state.commandOutputOpen = true;
        });
}

// =============================================================================
// Actions
// =============================================================================
void refreshListing(bool force = false);
void closeMirrorWindow();

void applyDeviceList(const std::vector<Device>& devices) {
    const std::string prevSerial = state.selectedDevice;
    state.devices = devices;

    if (!state.selectedDevice.empty()) {
        bool stillUsable = false;
        for (const Device& d : state.devices) {
            if (d.serial == state.selectedDevice && d.state == "device") { stillUsable = true; break; }
        }
        if (!stillUsable) state.selectedDevice.clear();
    }
    if (state.selectedDevice.empty()) {
        for (const Device& d : state.devices) {
            if (d.state == "device") { state.selectedDevice = d.serial; break; }
        }
    }
    if (state.selectedDevice.empty() && !state.devices.empty()) {
        state.toastTitle = "设备未就绪";
        state.toastMessage = "检测到设备，但状态为离线或未授权。\n请在设备上允许 USB 调试，或检查连接后重试。";
        state.toastVisible = true;
    }

    if (state.selectedDevice != prevSerial) {
        // Device changed: reset history/cache and restore its remembered path.
        state.pathBack.clear();
        state.pathForward.clear();
        state.dirCache.clear();
        auto it = state.lastPaths.find(state.selectedDevice);
        state.currentPath = (it != state.lastPaths.end() && !it->second.empty()) ? it->second : "/";
        state.pathInput = state.currentPath;
        resetListScroll();
        saveSettings();
        if (state.scrcpyOpen) {
            closeMirrorWindow();
            toast("设备已切换", "投屏已关闭，请重新打开投屏。");
        }
    }
    refreshListing(true);
}

void restartAdbServer() {
    if (!state.adbFound) return;
    const std::string adb = state.adbPath;
    state.deviceLoading = true;
    app::async::runOnce(
        "adb.restart",
        [adb]() -> app::async::Result<std::vector<Device>> {
            runProcess(adb, core::killServerArgs(), 15000);
            // Give the server a moment to fully stop before the next command
            // auto-restarts it and reconnects the devices.
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            ProcessResult r = runProcess(adb, core::devicesArgs(), 15000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure<std::vector<Device>>(
                    msg.empty() ? "重启 adb 失败（退出码 " + std::to_string(r.exitCode) + "）" : msg);
            }
            return app::async::success(core::parseDevices(r.out));
        },
        [](const app::async::Result<std::vector<Device>>& result) {
            state.deviceLoading = false;
            if (result.ok) {
                applyDeviceList(result.value);
                state.toastTitle = "adb 已重启";
                state.toastMessage = "已重启 adb 服务并刷新设备列表。";
                state.toastVisible = true;
            } else {
                state.toastTitle = "重启失败";
                state.toastMessage = result.error;
                state.toastVisible = true;
            }
        });
}

// Periodically poll `adb devices` so a device that disconnects (e.g. after
// `adb reboot` or an unplug) clears the stale listing and recovers when the
// device comes back.
void schedulePollDevices() {
    if (!state.adbFound) return;
    const std::string adb = state.adbPath;
    app::async::restart(
        "poll.devices",
        [adb]() -> app::async::Result<std::vector<Device>> {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            ProcessResult r = runProcess(adb, core::devicesArgs(), 15000);
            if (r.exitCode != 0) {
                return app::async::failure<std::vector<Device>>(core::trim(r.out));
            }
            return app::async::success(core::parseDevices(r.out));
        },
        [](const app::async::Result<std::vector<Device>>& result) {
            if (result.ok) {
                const std::vector<Device>& devices = result.value;
                const std::string selected = state.selectedDevice;

                bool selectedWasReady = false;
                for (const Device& d : state.devices) {
                    if (d.serial == selected && d.state == "device") { selectedWasReady = true; break; }
                }
                bool selectedIsReady = false;
                for (const Device& d : devices) {
                    if (d.serial == selected && d.state == "device") { selectedIsReady = true; break; }
                }

                state.devices = devices;

                if (!selected.empty() && selectedWasReady && !selectedIsReady) {
                    state.selectedDevice.clear();
                    state.entries.clear();
                    state.dirWritable = false;
                    state.selectedEntry.clear();
                    state.toastTitle = "设备已断开";
                    state.toastMessage = "设备连接已断开，重新连接后会自动恢复。";
                    state.toastVisible = true;
                } else if (state.selectedDevice.empty()) {
                    for (const Device& d : devices) {
                        if (d.state == "device") {
                            state.selectedDevice = d.serial;
                            state.currentPath = "/";
                            state.pathInput = "/";
                            state.selectedEntry.clear();
                            refreshListing(true);
                            break;
                        }
                    }
                }
            }
            schedulePollDevices();
        });
}

void refreshDevices() {
    if (!state.adbFound) {
        state.devices.clear();
        return;
    }
    const std::string adb = state.adbPath;
    state.deviceLoading = true;
    schedulePollDevices();
    app::async::runOnce(
        "adb.devices",
        [adb]() -> app::async::Result<std::vector<Device>> {
            ProcessResult r = runProcess(adb, core::devicesArgs(), 15000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure<std::vector<Device>>(
                    msg.empty() ? "获取设备列表失败（退出码 " + std::to_string(r.exitCode) + "）" : msg);
            }
            return app::async::success(core::parseDevices(r.out));
        },
        [](const app::async::Result<std::vector<Device>>& result) {
            state.deviceLoading = false;
            if (!result.ok) {
                state.toastTitle = "获取设备列表失败";
                state.toastMessage = result.error;
                state.toastVisible = true;
                return;
            }
            applyDeviceList(result.value);
        });
}

void refreshListing(bool force) {
    if (state.selectedDevice.empty() || !state.adbFound) {
        state.entries.clear();
        state.loading = false;
        return;
    }
    const std::string serial = state.selectedDevice;
    const std::string path = state.currentPath;
    const std::string cacheKey = serial + "|" + path;

    // Serve a cached listing instantly for back/forward navigation.
    if (!force) {
        auto it = state.dirCache.find(cacheKey);
        if (it != state.dirCache.end()) {
            state.loading = false;
            state.entries = it->second.entries;
            sortEntries(state.entries);
            applyFileFilter();
            state.dirWritable = it->second.writable;
            state.selectedEntry.clear();
            state.selectedSet.clear();
            return;
        }
    }

    state.loading = true;
    const std::string adb = state.adbPath;
    app::async::restart(
        "adb.list",
        [adb, serial, path]() -> app::async::Result<ListingResult> {
            // One round-trip for both the listing and the writable check.
            const std::string shellCmd =
                "ls -la " + core::shellQuote(path) + " 2>&1; __r=$?; echo __WRITE__; "
                "test -w " + core::shellQuote(path) + " && echo 1 || echo 0; exit $__r";
            ProcessResult r = runProcess(adb, {"-s", serial, "shell", shellCmd}, 30000);
            // Retry once on a transient adb drop (e.g. device still reconnecting).
            if (r.exitCode == 255 && core::trim(r.out).empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(800));
                r = runProcess(adb, {"-s", serial, "shell", shellCmd}, 30000);
            }
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                if (msg.empty()) {
                    msg = "读取目录失败（退出码 " + std::to_string(r.exitCode) + "）";
                    if (r.exitCode == 255) {
                        msg += "\n设备可能离线或未授权：请检查设备上的 USB 调试授权弹窗，"
                               "或运行 adb kill-server 后重试。";
                    }
                }
                return app::async::failure<ListingResult>(msg);
            }
            ListingResult result;
            const std::size_t marker = r.out.find("__WRITE__");
            const std::string lsOut = marker == std::string::npos ? r.out : r.out.substr(0, marker);
            const std::string writeOut = marker == std::string::npos ? std::string{} : r.out.substr(marker + 9);
            result.entries = core::parseLsLa(lsOut);
            result.writable = (core::trim(writeOut) == "1");
            return app::async::success(result);
        },
        [cacheKey](const app::async::Result<ListingResult>& result) {
            state.loading = false;
            if (result.ok) {
                state.entries = result.value.entries;
                sortEntries(state.entries);
                applyFileFilter();
                state.dirWritable = result.value.writable;
                if (state.dirCache.size() >= 128) {
                    state.dirCache.clear();
                }
                state.dirCache[cacheKey] = result.value;
            } else {
                state.entries.clear();
                state.toastTitle = "读取目录失败";
                state.toastMessage = result.error;
                state.toastVisible = true;
            }
            state.selectedEntry.clear();
            state.selectedSet.clear();
        });
}

void navigateTo(const std::string& path) {
    if (path == state.currentPath) return;
    state.pathBack.push_back(state.currentPath);
    if (state.pathBack.size() > 64) state.pathBack.erase(state.pathBack.begin());
    state.pathForward.clear();
    state.currentPath = path;
    state.pathInput = path;
    state.selectedEntry.clear();
    state.selectedSet.clear();
    resetListScroll();
    rememberLastPath();
    refreshListing();
}

void goToPath(const std::string& raw) {
    std::string path = core::trim(raw);
    if (path.empty()) path = "/";
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    navigateTo(path);
}

void goUp() {
    navigateTo(core::parentPath(state.currentPath));
}

void enterDir(const std::string& name) {
    navigateTo(core::joinPath(state.currentPath, name));
}

void goBack() {
    if (state.pathBack.empty()) return;
    state.pathForward.push_back(state.currentPath);
    state.currentPath = state.pathBack.back();
    state.pathBack.pop_back();
    state.pathInput = state.currentPath;
    state.selectedEntry.clear();
    state.selectedSet.clear();
    resetListScroll();
    rememberLastPath();
    refreshListing();
}

void goForward() {
    if (state.pathForward.empty()) return;
    state.pathBack.push_back(state.currentPath);
    state.currentPath = state.pathForward.back();
    state.pathForward.pop_back();
    state.pathInput = state.currentPath;
    state.selectedEntry.clear();
    state.selectedSet.clear();
    resetListScroll();
    rememberLastPath();
    refreshListing();
}

void onEntryClick(const std::string& name, bool isDir) {
    const auto now = std::chrono::steady_clock::now();
    const bool isDouble = (name == state.lastClickedName) &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - state.lastClickTime).count() < 450;
    state.lastClickedName = name;
    state.lastClickTime = now;
    const bool ctrl = state.lastClickCtrl;

    if (isDir && isDouble && !ctrl) {
        enterDir(name);  // double-click a folder opens it
    } else if (ctrl) {
        // Ctrl+click toggles multi-selection.
        if (state.selectedSet.count(name)) state.selectedSet.erase(name);
        else state.selectedSet.insert(name);
        state.selectedEntry = name;
    } else {
        state.selectedEntry = name;
        state.selectedSet.clear();
        state.selectedSet.insert(name);
    }
}

void openRowMenu(float x, float y, const std::string& name) {
    state.selectedEntry = name;
    state.rowMenuOpen = true;
    state.rowMenuX = x;
    state.rowMenuY = y;
}

void openDeviceMenu(float x, float y) {
    state.deviceMenuOpen = true;
    state.deviceMenuX = x;
    state.deviceMenuY = y;
}

void pullBatchStep(std::vector<std::string> names, std::size_t index, BatchOutcome outcome);

void doPull() {
    std::vector<std::string> names = selectedNames();
    if (names.empty() || state.selectedDevice.empty()) return;
    state.busy = true;
    state.progress = 0.0f;
    pullBatchStep(names, 0, makeBatchOutcome(names));
}

void pullBatchStep(std::vector<std::string> names, std::size_t index, BatchOutcome outcome) {
    if (index >= names.size()) {
        state.busy = false;
        state.progress = 0.0f;
        state.progressLabel.clear();
        toast(outcome.toastTitle("下载完成"),
              batchSummary("已下载到 " + state.downloadDir, outcome));
        return;
    }
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    const std::string remote = core::joinPath(state.currentPath, names[index]);
    const std::string localDir = state.downloadDir;
    state.progress = static_cast<float>(index) / static_cast<float>(names.size());
    state.progressLabel = "下载 " + std::to_string(index + 1) + "/" + std::to_string(names.size());
    app::async::restart(
        "adb.pull.one",
        [adb, serial, remote, localDir]() -> app::async::Result<void> {
            std::error_code ec;
            std::filesystem::create_directories(localDir, ec);
            ProcessResult r = runProcess(adb, core::pullArgs(serial, remote, localDir), 600000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure(msg.empty() ? "下载失败（退出码 " + std::to_string(r.exitCode) + "）" : msg);
            }
            return app::async::success();
        },
        [names, index, outcome](const app::async::Result<void>& result) mutable {
            outcome.record(index, result.ok, result.error);
            pullBatchStep(names, index + 1, std::move(outcome));
        });
}

void doPush() {
    if (state.selectedDevice.empty()) return;
    std::string local = eui::platform::chooseFile(eui::platform::FileDialogOptions{});
    if (local.empty()) return;
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    const std::string remoteDir = state.currentPath;
    state.busy = true;
    state.progress = -1.0f;
    state.progressLabel = "上传中…";
    app::async::runOnce(
        "adb.push",
        [adb, serial, local, remoteDir]() -> app::async::Result<std::string> {
            ProcessResult r = runProcess(adb, core::pushArgs(serial, local, remoteDir), 600000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure<std::string>(
                    msg.empty() ? "上传失败（退出码 " + std::to_string(r.exitCode) + "）" : msg);
            }
            return app::async::success(core::trim(r.out));
        },
        [](const app::async::Result<std::string>& result) {
            state.busy = false;
            state.progress = 0.0f;
            state.progressLabel.clear();
            state.toastTitle = result.ok ? "上传完成" : "上传失败";
            state.toastMessage = result.ok
                ? (result.value.empty() ? "已上传" : core::shorten(result.value, 220))
                : result.error;
            state.toastVisible = true;
            if (result.ok) refreshListing(true);
        });
}

void pushBatchStep(std::vector<std::string> files, std::size_t index, BatchOutcome outcome);

void pushFiles(const std::vector<std::string>& files) {
    if (files.empty() || state.selectedDevice.empty()) return;
    state.busy = true;
    state.progress = 0.0f;
    pushBatchStep(files, 0, makeBatchOutcome(files));
}

void pushBatchStep(std::vector<std::string> files, std::size_t index, BatchOutcome outcome) {
    if (index >= files.size()) {
        state.busy = false;
        state.progress = 0.0f;
        state.progressLabel.clear();
        toast(outcome.toastTitle("上传完成"), batchSummary("已上传", outcome));
        refreshListing(true);
        return;
    }
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    const std::string remoteDir = state.currentPath;
    state.progress = static_cast<float>(index) / static_cast<float>(files.size());
    state.progressLabel = "上传 " + std::to_string(index + 1) + "/" + std::to_string(files.size());
    app::async::restart(
        "adb.push.one",
        [adb, serial, file = files[index], remoteDir]() -> app::async::Result<void> {
            ProcessResult r = runProcess(adb, core::pushArgs(serial, file, remoteDir), 600000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure(msg.empty() ? "上传失败（退出码 " + std::to_string(r.exitCode) + "）" : msg);
            }
            return app::async::success();
        },
        [files, index, outcome](const app::async::Result<void>& result) mutable {
            outcome.record(index, result.ok, result.error);
            pushBatchStep(files, index + 1, std::move(outcome));
        });
}

void deleteBatchStep(std::vector<std::string> names, std::size_t index, BatchOutcome outcome);
void doDeleteConfirmed();

// Generic confirm dialog helper: shows title/message with a primary button that
// runs `action` when confirmed.
void showConfirm(const std::string& title, const std::string& message,
                 const std::string& primaryText, std::function<void()> action) {
    state.confirmTitle = title;
    state.confirmMessage = message;
    state.confirmPrimaryText = primaryText;
    state.confirmAction = std::move(action);
    state.confirmDialogOpen = true;
}

void confirmDelete() {
    std::vector<std::string> names = selectedNames();
    if (names.empty()) return;
    if (names.size() == 1) {
        showConfirm("删除", "确定要删除 \"" + names[0] + "\" 吗？\n此操作无法撤销。", "删除",
                    [] { doDeleteConfirmed(); });
    } else {
        showConfirm("删除", "确定要删除选中的 " + std::to_string(names.size()) + " 项吗？\n此操作无法撤销。",
                    "删除", [] { doDeleteConfirmed(); });
    }
}

void doDeleteConfirmed() {
    state.confirmDialogOpen = false;
    std::vector<std::string> names = selectedNames();
    if (names.empty() || state.selectedDevice.empty()) return;
    state.busy = true;
    state.progress = 0.0f;
    deleteBatchStep(names, 0, makeBatchOutcome(names));
}

void deleteBatchStep(std::vector<std::string> names, std::size_t index, BatchOutcome outcome) {
    if (index >= names.size()) {
        state.busy = false;
        state.progress = 0.0f;
        state.progressLabel.clear();
        toast(outcome.toastTitle("删除完成"), batchSummary("已删除", outcome));
        refreshListing(true);
        return;
    }
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    const std::string remote = core::joinPath(state.currentPath, names[index]);
    state.progress = static_cast<float>(index) / static_cast<float>(names.size());
    state.progressLabel = "删除 " + std::to_string(index + 1) + "/" + std::to_string(names.size());
    app::async::restart(
        "adb.delete.one",
        [adb, serial, remote]() -> app::async::Result<void> {
            ProcessResult r = runProcess(adb, core::deleteArgs(serial, remote), 60000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure(msg.empty() ? "删除失败（退出码 " + std::to_string(r.exitCode) + "）" : msg);
            }
            return app::async::success();
        },
        [names, index, outcome](const app::async::Result<void>& result) mutable {
            outcome.record(index, result.ok, result.error);
            deleteBatchStep(names, index + 1, std::move(outcome));
        });
}

void promptNewFolder() {
    state.promptTitle = "新建文件夹";
    state.promptValue = "";
    state.promptMode = 0;
    state.promptOpen = true;
}

void promptRename() {
    if (state.selectedEntry.empty()) return;
    state.promptTitle = "重命名";
    state.promptValue = state.selectedEntry;
    state.promptMode = 1;
    state.promptOpen = true;
}

void confirmPrompt() {
    std::string value = core::trim(state.promptValue);
    state.promptOpen = false;
    if (state.promptMode == 2) {
        // Wireless connect.
        if (value.empty()) return;
        const std::string adb = state.adbPath;
        state.busy = true;
        app::async::runOnce(
            "adb.connect",
            [adb, value]() -> app::async::Result<std::string> {
                ProcessResult r = runProcess(adb, core::connectArgs(value), 20000);
                if (r.exitCode != 0) {
                    std::string msg = core::trim(r.out);
                    return app::async::failure<std::string>(msg.empty() ? "连接失败" : msg);
                }
                return app::async::success(core::trim(r.out));
            },
            [](const app::async::Result<std::string>& result) {
                state.busy = false;
                toast(result.ok ? "已连接" : "连接失败", result.ok ? result.value : result.error);
                refreshDevices();
            });
        return;
    }
    if (value.empty() || value == "." || value == ".." || value.find('/') != std::string::npos) {
        state.toastTitle = "名称无效";
        state.toastMessage = "名称不能为空，且不能包含 '/'。";
        state.toastVisible = true;
        return;
    }
    if (state.selectedDevice.empty()) return;

    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    std::vector<std::string> args;
    std::string summary;
    if (state.promptMode == 0) {
        args = {"-s", serial, "shell", "mkdir", "-p", core::shellQuote(core::joinPath(state.currentPath, value))};
        summary = "已创建 " + value;
    } else {
        args = {"-s", serial, "shell", "mv",
                core::shellQuote(core::joinPath(state.currentPath, state.selectedEntry)),
                core::shellQuote(core::joinPath(state.currentPath, value))};
        summary = "已重命名为 " + value;
    }
    state.busy = true;
    app::async::runOnce(
        "adb.mkdir.mv",
        [adb, args = std::move(args), summary]() -> app::async::Result<std::string> {
            ProcessResult r = runProcess(adb, args, 60000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure<std::string>(
                    msg.empty() ? "操作失败（退出码 " + std::to_string(r.exitCode) + "）" : msg);
            }
            return app::async::success(summary);
        },
        [](const app::async::Result<std::string>& result) {
            state.busy = false;
            state.toastTitle = result.ok ? "完成" : "操作失败";
            state.toastMessage = result.ok ? result.value : result.error;
            state.toastVisible = true;
            refreshListing(true);
        });
}

void setDownloadDir() {
    std::string file = eui::platform::chooseFile(eui::platform::FileDialogOptions{});
    if (file.empty()) return;
    std::filesystem::path p(file);
    state.downloadDir = p.parent_path().string();
    state.toastTitle = "下载目录";
    state.toastMessage = state.downloadDir;
    state.toastVisible = true;
}

// --- Clipboard (copy / move within the device) ---
void doCopy() {
    if (state.selectedEntry.empty()) return;
    state.clipboardCut = false;
    state.clipboardPath = core::joinPath(state.currentPath, state.selectedEntry);
    state.clipboardName = state.selectedEntry;
    toast("已复制", state.clipboardName);
}

void doMove() {
    if (state.selectedEntry.empty()) return;
    state.clipboardCut = true;
    state.clipboardPath = core::joinPath(state.currentPath, state.selectedEntry);
    state.clipboardName = state.selectedEntry;
    toast("已剪切", state.clipboardName);
}

void doPaste() {
    if (state.clipboardPath.empty() || state.selectedDevice.empty()) return;
    const std::string dest = core::joinPath(state.currentPath, state.clipboardName);
    if (dest == state.clipboardPath) {
        toast("无法粘贴", "源文件和目标位置相同。");
        return;
    }
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    const std::string src = state.clipboardPath;
    const bool cut = state.clipboardCut;
    state.busy = true;
    app::async::runOnce(
        "adb.paste",
        [adb, serial, src, dest, cut]() -> app::async::Result<std::string> {
            std::vector<std::string> args = {"-s", serial, "shell", cut ? "mv" : "cp", "-r",
                                             core::shellQuote(src), core::shellQuote(dest)};
            ProcessResult r = runProcess(adb, args, 120000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure<std::string>(
                    msg.empty() ? (cut ? "移动失败（退出码 " : "复制失败（退出码 ") + std::to_string(r.exitCode) + "）" : msg);
            }
            return app::async::success<std::string>(cut ? "已移动" : "已复制");
        },
        [cut](const app::async::Result<std::string>& result) {
            state.busy = false;
            toast(result.ok ? "完成" : "操作失败", result.ok ? result.value : result.error);
            if (result.ok && cut) {
                state.clipboardPath.clear();
                state.clipboardName.clear();
            }
            refreshListing(true);
        });
}

// --- Screenshot ---
void doScreenshot() {
    if (state.selectedDevice.empty()) return;
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    const std::string dir = state.downloadDir;
    state.busy = true;
    state.progress = -1.0f;
    state.progressLabel = "截图中…";
    app::async::runOnce(
        "adb.screenshot",
        [adb, serial, dir]() -> app::async::Result<std::string> {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            ProcessResult r = runProcess(adb, core::screencapArgs(serial), 30000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure<std::string>(
                    msg.empty() ? "截图失败（退出码 " + std::to_string(r.exitCode) + "）" : msg);
            }
            const long long ts = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const std::string file = dir + "/screenshot_" + std::to_string(ts) + ".png";
            std::ofstream out(file, std::ios::binary);
            if (!out) return app::async::failure<std::string>("无法写入截图文件");
            out.write(r.out.data(), static_cast<std::streamsize>(r.out.size()));
            out.close();
            return app::async::success(file);
        },
        [](const app::async::Result<std::string>& result) {
            state.busy = false;
            state.progress = 0.0f;
            toast(result.ok ? "截图完成" : "截图失败", result.ok ? result.value : result.error);
        });
}

// --- Install APK ---
void doInstallApk() {
    if (state.selectedDevice.empty()) return;
    std::string apk = eui::platform::chooseFile(eui::platform::FileDialogOptions{});
    if (apk.empty()) return;
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    state.busy = true;
    state.progress = -1.0f;
    state.progressLabel = "安装中…";
    app::async::runOnce(
        "adb.install",
        [adb, serial, apk]() -> app::async::Result<std::string> {
            ProcessResult r = runProcess(adb, core::installApkArgs(serial, apk), 300000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure<std::string>(
                    msg.empty() ? "安装失败（退出码 " + std::to_string(r.exitCode) + "）" : msg);
            }
            return app::async::success(core::trim(r.out));
        },
        [](const app::async::Result<std::string>& result) {
            state.busy = false;
            state.progress = 0.0f;
            toast(result.ok ? "安装完成" : "安装失败", result.ok ? "安装成功" : result.error);
        });
}

// --- Device info ---
void fetchAdbVersion() {
    if (state.adbPath.empty()) return;
    const std::string adb = state.adbPath;
    app::async::runOnce(
        "adb.version",
        [adb]() -> app::async::Result<std::string> {
            ProcessResult r = runProcess(adb, {"version"}, 15000);
            if (r.exitCode != 0) return app::async::failure<std::string>(core::trim(r.out));
            return app::async::success(core::trim(r.out));
        },
        [](const app::async::Result<std::string>& result) {
            if (result.ok) state.adbVersion = result.value;
        });
}

void openDeviceInfo() {
    if (state.selectedDevice.empty()) return;
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    state.deviceInfoOpen = true;
    state.deviceInfoText = "加载中…";
    app::async::restart(
        "adb.info",
        [adb, serial]() -> app::async::Result<std::string> {
            const std::string cmd =
                "echo '型号:'; getprop ro.product.model; echo '品牌:'; getprop ro.product.brand; "
                "echo 'Android:'; getprop ro.build.version.release; echo 'SDK:'; getprop ro.build.version.sdk; "
                "echo 'ABI:'; getprop ro.product.cpu.abi; echo '电量:'; dumpsys battery | grep level; "
                "echo '存储:'; df /data | tail -1";
            ProcessResult r = runProcess(adb, {"-s", serial, "shell", cmd}, 30000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure<std::string>(msg.empty() ? "获取设备信息失败" : msg);
            }
            return app::async::success(core::trim(r.out));
        },
        [](const app::async::Result<std::string>& result) {
            const std::string prefix = state.adbVersion.empty() ? std::string{} : ("adb: " + state.adbVersion + "\n\n");
            state.deviceInfoText = result.ok ? (prefix + result.value) : (prefix + "获取失败：\n" + result.error);
        });
}

// --- Logcat (continuous streaming) ---
void logcatRegexClear() {
    if (state.logcatRegexHandle != nullptr) {
        adbRegexFree(state.logcatRegexHandle);
        state.logcatRegexHandle = nullptr;
    }
    state.logcatCompiledFilter.clear();
    state.logcatRegexError.clear();
}

// Ensure the compiled regex matches the current filter. Returns true when the
// filter is empty or valid; false when it is an invalid regex.
bool logcatEnsureRegex() {
    const std::string& filter = state.logcatFilter;
    if (filter.empty()) {
        logcatRegexClear();
        return true;
    }
    if (state.logcatRegexHandle != nullptr && state.logcatCompiledFilter == filter) {
        return state.logcatRegexError.empty();
    }
    logcatRegexClear();
    // Case-insensitive matching.
    state.logcatRegexHandle = adbRegexCompile(filter.c_str(), 1);
    state.logcatCompiledFilter = filter;
    if (state.logcatRegexHandle == nullptr) {
        state.logcatRegexError = "正则表达式无效";
        return false;
    }
    return true;
}

bool logcatLineMatches(const std::string& line) {
    if (state.logcatFilter.empty()) return true;
    if (state.logcatRegexHandle == nullptr) return false;
    return adbRegexSearchHandle(state.logcatRegexHandle, line.c_str()) == 1;
}

void stopLogcat() {
    g_logcatStop = true;
#ifdef _WIN32
    // Force the adb logcat process to exit so the reader thread unblocks. The
    // reader owns the process handle and closes it on exit, so don't close it
    // here (that would double-close a handle the reader is still using).
    // A handle that the reader closed meanwhile simply makes TerminateProcess
    // fail harmlessly.
    HANDLE proc = g_logcatProcess.load();
    if (proc != nullptr) {
        TerminateProcess(proc, 0);
    }
#endif
    g_logcatRunning = false;
    state.logcatStreaming = false;
}

#ifdef _WIN32
void streamLogcatWorker(const std::string& adb, const std::string& serial) {
    // Clear the device log buffer so the stream starts fresh (no old history).
    runProcess(adb, core::logcatClearArgs(serial), 15000);

    const std::wstring cmd = core::quoteWinArg(core::toWide(adb)) + L" -s " + core::quoteWinArg(core::toWide(serial)) + L" logcat";
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        g_logcatRunning = false;
        return;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        g_logcatRunning = false;
        return;
    }
    CloseHandle(writePipe);
    core::adoptChildProcess(pi.hProcess);  // die with us, not after us
    CloseHandle(pi.hThread);
    g_logcatProcess = pi.hProcess;

    constexpr std::size_t kMaxLines = 3000;
    std::string pending;
    char buffer[8192];
    auto lastWake = std::chrono::steady_clock::now();

    auto drain = [&](const char* data, DWORD len) {
        pending.append(data, len);
        std::size_t pos = 0;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            {
                std::lock_guard<std::mutex> lock(g_logcatMutex);
                g_logcatLines.push_back(std::move(line));
                if (g_logcatLines.size() > kMaxLines) {
                    const std::size_t excess = g_logcatLines.size() - kMaxLines;
                    g_logcatLines.erase(g_logcatLines.begin(),
                                        g_logcatLines.begin() + static_cast<std::ptrdiff_t>(excess));
                }
            }
            g_logcatSeq.fetch_add(1);
        }
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastWake).count() >= 30) {
            lastWake = now;
            app::requestUpdate();
        }
    };

    for (;;) {
        if (g_logcatStop.load()) break;
        DWORD available = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            const DWORD toRead = available > static_cast<DWORD>(sizeof(buffer))
                                     ? static_cast<DWORD>(sizeof(buffer)) : available;
            DWORD read = 0;
            if (ReadFile(readPipe, buffer, toRead, &read, nullptr) && read > 0) {
                drain(buffer, read);
            }
            continue;
        }
        const DWORD wait = WaitForSingleObject(pi.hProcess, 0);
        if (wait == WAIT_OBJECT_0) break;
        Sleep(20);
    }
    // Drain the remaining tail before tearing down.
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
        const DWORD toRead = available > static_cast<DWORD>(sizeof(buffer))
                                 ? static_cast<DWORD>(sizeof(buffer)) : available;
        DWORD read = 0;
        if (!ReadFile(readPipe, buffer, toRead, &read, nullptr) || read == 0) break;
        drain(buffer, read);
    }
    // Publish "no process" BEFORE closing the handle: once stopLogcat() on the
    // UI thread can no longer observe this value, nobody else holds it and it
    // is safe for the reader thread to close.
    HANDLE expected = pi.hProcess;
    g_logcatProcess.compare_exchange_strong(expected, nullptr);
    // TerminateProcess is a request: wait for the process to actually be gone
    // before closing the handle and reporting, so we never leave a half-killed
    // adb behind.
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);
    g_logcatRunning = false;
    app::requestUpdate();
}
#endif  // _WIN32

void startLogcat() {
    if (state.selectedDevice.empty()) return;
    stopLogcat();
    {
        std::lock_guard<std::mutex> lock(g_logcatMutex);
        g_logcatLines.clear();
    }
    g_logcatSeq = 0;
    state.logcatLastSeq = 0;
    state.logcatScroll.set(0.0f);
    logcatRegexClear();  // recompile with the current filter on next render
    g_logcatStop = false;
    g_logcatRunning = true;
    state.logcatStreaming = true;
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
#ifdef _WIN32
    std::thread([](std::string a, std::string s) { streamLogcatWorker(a, s); }, adb, serial).detach();
#else
    g_logcatRunning = false;
    state.logcatStreaming = false;
#endif
}

void openLogcat() {
    if (state.selectedDevice.empty()) return;
    state.logcatOpen = true;
    startLogcat();
}

// --- Screen mirroring (launch scrcpy) ---
std::string executableDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::error_code ec;
        std::filesystem::path p(core::toUtf8(std::wstring(buf, static_cast<std::size_t>(n))));
        return p.parent_path().string();
    }
#endif
    return ".";
}

std::string findScrcpy() {
    std::vector<std::string> candidates;
    const std::string dir = executableDir();
    candidates.push_back(dir + "\\scrcpy.exe");
    candidates.push_back(dir + "\\scrcpy\\scrcpy.exe");
    candidates.push_back(dir + "/scrcpy");
    const char* pathEnv = std::getenv("PATH");
    if (pathEnv != nullptr && *pathEnv != '\0') {
        std::string pathStr = pathEnv;
        std::size_t start = 0;
        while (start <= pathStr.size()) {
            const std::size_t end = pathStr.find(';', start);
            const std::string d = pathStr.substr(start, end == std::string::npos ? std::string::npos : end - start);
            start = (end == std::string::npos) ? pathStr.size() + 1 : end + 1;
            if (!d.empty()) {
                candidates.push_back(d + "\\scrcpy.exe");
                candidates.push_back(d + "/scrcpy");
            }
        }
    }
    for (const std::string& candidate : candidates) {
        // is_regular_file semantics, not exists(): the "C:\dir/scrcpy" candidate
        // also names the scrcpy *directory*, so a plain existence check would
        // return that directory and scrcpy would then fail to launch with a
        // confusing error.
        //
        // core::isRegularFileNoThrow rather than std::filesystem directly: the
        // candidates include every PATH entry, and on a non-UTF-8 locale a PATH
        // entry with non-ASCII characters holds ANSI bytes, so constructing a
        // std::filesystem::path from it throws - and this file is compiled with
        // -fno-exceptions, where that throw becomes an abort.
        if (core::isRegularFileNoThrow(candidate)) return candidate;
    }
    return "";
}

struct MirrorLayout {
    float frameX = 0.0f;
    float frameY = 0.0f;
    float frameW = 0.0f;
    float frameH = 0.0f;
    float screenX = 0.0f;
    float screenY = 0.0f;
    float screenW = 0.0f;
    float screenH = 0.0f;
};

// Device screen resolution; overwritten from `wm size` on open.
int g_mirrorDeviceW = 1080;
int g_mirrorDeviceH = 2340;

void queryDeviceSize(const std::string& adb, const std::string& serial) {
    ProcessResult r = runProcess(adb, {"-s", serial, "shell", "wm", "size"}, 10000);
    const std::string out = core::trim(r.out);
    const std::size_t x = out.rfind('x');
    if (x == std::string::npos) return;
    std::size_t ws = x;
    while (ws > 0 && std::isdigit(static_cast<unsigned char>(out[ws - 1]))) --ws;
    std::size_t he = x + 1;
    while (he < out.size() && std::isdigit(static_cast<unsigned char>(out[he]))) ++he;
    if (ws == x || he == x + 1) return;
    const int w = std::atoi(out.substr(ws, x - ws).c_str());
    const int h = std::atoi(out.substr(x + 1, he - x - 1).c_str());
    if (w <= 0 || h <= 0) return;
    g_mirrorDeviceW = w;
    g_mirrorDeviceH = h;
}

// Layout as fractions of the actual window size. The window's aspect ratio is
// locked to the device by glfwSetWindowAspectRatio, so the frame simply fills
// the area below the title bar (no letterboxing).
MirrorLayout computeMirrorLayout(float W, float H) {
    const float bezel = 16.0f;
    const float sideMargin = 10.0f;
    const float frameTop = kTitleBarHeight;  // frame starts right below the title bar
    const float bottomMargin = 10.0f;
    // Keep the screen EXACTLY the device's aspect ratio so scrcpy doesn't
    // letterbox (which otherwise shows bars inside the screen area).
    const float aspect = static_cast<float>(g_mirrorDeviceW) / static_cast<float>(g_mirrorDeviceH);
    const float availW = std::max(1.0f, W - 2.0f * sideMargin - 2.0f * bezel);
    const float availH = std::max(1.0f, H - frameTop - bottomMargin - 2.0f * bezel);
    float sw = availW;
    float sh = availW / aspect;
    if (sh > availH) {
        sh = availH;
        sw = availH * aspect;
    }
    const float sx = sideMargin + bezel + (availW - sw) * 0.5f;
    const float sy = frameTop + bezel + (availH - sh) * 0.5f;
    MirrorLayout l;
    l.screenX = sx / W;
    l.screenY = sy / H;
    l.screenW = sw / W;
    l.screenH = sh / H;
    l.frameX = (sx - bezel) / W;
    l.frameY = (sy - bezel) / H;
    l.frameW = (sw + 2.0f * bezel) / W;
    l.frameH = (sh + 2.0f * bezel) / H;
    return l;
}

MirrorLayout g_mirrorLayout = computeMirrorLayout(kMirrorWindowWidth, kMirrorWindowHeight);

#ifdef _WIN32
static HANDLE g_scrcpyProcess = nullptr;
static HWND g_mirrorHwnd = nullptr;
static HWND g_scrcpyHwnd = nullptr;

struct FindWindowByPid {
    DWORD pid;
    HWND hwnd;
};

BOOL CALLBACK enumWindowByPid(HWND hwnd, LPARAM lparam) {
    auto* f = reinterpret_cast<FindWindowByPid*>(lparam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == f->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        f->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND findScrcpyWindow(DWORD pid) {
    FindWindowByPid f{pid, nullptr};
    EnumWindows(enumWindowByPid, reinterpret_cast<LPARAM>(&f));
    return f.hwnd;
}

void repositionScrcpy() {
    if (g_mirrorHwnd == nullptr || g_scrcpyHwnd == nullptr) return;
    RECT rc{};
    GetClientRect(g_mirrorHwnd, &rc);
    const int cw = std::max(1, static_cast<int>(rc.right - rc.left));
    const int ch = std::max(1, static_cast<int>(rc.bottom - rc.top));
    const MirrorLayout& l = g_mirrorLayout;
    const int x = static_cast<int>(cw * l.screenX);
    const int y = static_cast<int>(ch * l.screenY);
    const int w = std::max(1, static_cast<int>(cw * l.screenW));
    const int h = std::max(1, static_cast<int>(ch * l.screenH));
    SetWindowPos(g_scrcpyHwnd, HWND_TOP, x, y, w, h, SWP_NOZORDER | SWP_FRAMECHANGED);
    // CreateRoundRectRgn takes the ellipse width/height (2x the corner radius).
    const int radius = std::max(8, static_cast<int>(std::min(w, h) * 0.12f));
    SetWindowRgn(g_scrcpyHwnd, CreateRoundRectRgn(0, 0, w, h, radius * 2, radius * 2), TRUE);
}

void scrcpyEmbedLoop() {
    const DWORD scrcpyPid = g_scrcpyProcess != nullptr ? GetProcessId(g_scrcpyProcess) : 0;
    HWND mirrorHwnd = nullptr;
    HWND scrcpyHwnd = nullptr;
    for (int attempt = 0; attempt < 300; ++attempt) {
        mirrorHwnd = FindWindowW(nullptr, L"adb_browser_mirror");
        scrcpyHwnd = scrcpyPid != 0 ? findScrcpyWindow(scrcpyPid)
                                    : FindWindowW(nullptr, L"adb_browser_scrcpy");
        if (mirrorHwnd != nullptr && scrcpyHwnd != nullptr) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (mirrorHwnd == nullptr || scrcpyHwnd == nullptr) return;
    g_mirrorHwnd = mirrorHwnd;
    g_scrcpyHwnd = scrcpyHwnd;
    SetParent(scrcpyHwnd, mirrorHwnd);
    SetWindowLongPtrW(scrcpyHwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE);
    SetForegroundWindow(mirrorHwnd);
    repositionScrcpy();
    while (IsWindow(mirrorHwnd) && IsWindow(scrcpyHwnd)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        repositionScrcpy();
    }
    if (g_mirrorHwnd == mirrorHwnd) g_mirrorHwnd = nullptr;
    if (g_scrcpyHwnd == scrcpyHwnd) g_scrcpyHwnd = nullptr;
    // The mirror window is gone (closed via button or taskbar): stop scrcpy so
    // it doesn't keep running in the background.
    if (g_scrcpyProcess != nullptr) {
        TerminateProcess(g_scrcpyProcess, 0);
        CloseHandle(g_scrcpyProcess);
        g_scrcpyProcess = nullptr;
    }
    state.scrcpyOpen = false;
}
#endif

void mirrorTitlePress() {
    app::startWindowDrag();
}

void closeMirrorWindow() {
#ifdef _WIN32
    HWND hwnd = FindWindowW(nullptr, L"adb_browser_mirror");
    if (hwnd != nullptr) {
        // Remember the window size for next time.
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w >= 300 && h >= 500) {
            // Persist a logical (DPI-independent) size like the main window.
            const float scale = windowDpiScale();
            settings.mirrorW = static_cast<int>(w / scale);
            settings.mirrorH = static_cast<int>(h / scale);
            saveSettings();
        }
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
#endif
    state.scrcpyOpen = false;
}

void composeMirrorWindow(eui::Ui& ui, const eui::Screen& screen) {
    const float W = screen.width;
    const float H = screen.height;
    g_mirrorLayout = computeMirrorLayout(W, H);
    const MirrorLayout& l = g_mirrorLayout;
    const float fx = l.frameX * W;
    const float fy = l.frameY * H;
    const float fw = l.frameW * W;
    const float fh = l.frameH * H;
    const float sx = l.screenX * W;
    const float sy = l.screenY * H;
    const float sw = l.screenW * W;
    const float sh = l.screenH * H;
    const float screenRadius = std::min(sw, sh) * 0.12f;

    ui.stack("mirror.root")
        .size(W, H)
        .content([&] {
            // Theme-aware background + title bar (follows dark/light + accent).
            ui.rect("mirror.bg").size(W, H).color(kBackground).build();
            ui.rect("mirror.titlebar.bg").size(W, kTitleBarHeight).color(kTitleBar).build();
            ui.rect("mirror.titlebar.drag").size(W - 46.0f, kTitleBarHeight).color(kClear)
                .onPress([](const eui::PointerEvent&, const eui::Rect&) { mirrorTitlePress(); })
                .build();
            ui.text("mirror.titlebar.title").x(14.0f).y(0.0f).size(W - 60.0f, kTitleBarHeight)
                .text("投屏").fontSize(13.0f).lineHeight(13.0f).color(kInk)
                .verticalAlign(eui::VerticalAlign::Center).build();
            ui.rect("mirror.titlebar.close.hit").x(W - 46.0f).y(0.0f).size(46.0f, kTitleBarHeight)
                .states(kClear, kCloseHover, kClosePressed)
                .onClick([] { closeMirrorWindow(); })
                .build();
            ui.text("mirror.titlebar.close.icon").x(W - 46.0f).y(0.0f).size(46.0f, kTitleBarHeight)
                .icon(0xF00D).fontSize(12.0f).lineHeight(12.0f).color(kInk)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center).build();

            // Phone frame (bezel) — stays black like a real phone.
            ui.rect("mirror.frame")
                .x(fx).y(fy).size(fw, fh)
                .color(eui::Color{0.02f, 0.02f, 0.03f, 1.0f})
                .radius(screenRadius + 16.0f)
                .border(1.0f, kBorder)
                .build();

            // Screen area (scrcpy overlays exactly this rect).
            ui.rect("mirror.screen")
                .x(sx).y(sy).size(sw, sh)
                .color(eui::Color{0.0f, 0.0f, 0.0f, 1.0f})
                .radius(screenRadius)
                .build();

            // Home indicator uses the accent color (theme-aware).
            ui.rect("mirror.homeind")
                .x(W * 0.5f - 60.0f).y(fy + fh - 10.0f).size(120.0f, 4.0f)
                .color(kAccent)
                .radius(2.0f)
                .build();
        })
        .build();
}

void openMirror() {
    if (state.selectedDevice.empty()) {
        toast("未选择设备", "请先选择设备再投屏。");
        return;
    }
    if (state.scrcpyOpen) {
#ifdef _WIN32
        HWND hwnd = FindWindowW(nullptr, L"adb_browser_mirror");
        if (hwnd != nullptr) SetForegroundWindow(hwnd);
#endif
        return;
    }
    const std::string scrcpy = findScrcpy();
    if (scrcpy.empty()) {
        // scrcpy is not shipped with the app any more (see g_pendingScrcpyInstall),
        // so fetch it instead of telling the user to install it by hand.
        if (ensureToolsDownloadable(PendingInstall::scrcpy)) {
            toast("未找到 scrcpy", "正在下载 scrcpy…下载完成后请重新点击投屏。");
        } else {
            toast("未找到 scrcpy", "正在获取下载地址，拿到后会自动开始下载，请稍后再点投屏。");
        }
        return;
    }
    const std::string serial = state.selectedDevice;
    queryDeviceSize(state.adbPath, serial);
#ifdef _WIN32
    app::setMirrorAspectRatio(g_mirrorDeviceW, g_mirrorDeviceH);
    app::setMirrorResizeHandler([] { repositionScrcpy(); });
    // Launch scrcpy off-screen: it briefly shows its own window before being
    // embedded into the mirror window, which otherwise flashes in the middle
    // of the screen. --window-x/y place it far outside any visible area.
    std::wstring cmd = core::quoteWinArg(core::toWide(scrcpy)) + L" -s " + core::quoteWinArg(core::toWide(serial)) +
                       L" --window-title=adb_browser_scrcpy --window-borderless --stay-awake"
                       L" --window-x=-32000 --window-y=-32000";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        toast("启动失败", "无法启动 scrcpy。");
        return;
    }
    CloseHandle(pi.hThread);
    core::adoptChildProcess(pi.hProcess);  // the mirror window closes with the app
    g_scrcpyProcess = pi.hProcess;
    state.scrcpyOpen = true;
    // Mirror size is stored in logical units; convert to physical for the
    // window (same DPI handling as the main window).
    const float mirrorScale = windowDpiScale();
    app::openWindow("adb_browser_mirror",
                    static_cast<int>(settings.mirrorW * mirrorScale),
                    static_cast<int>(settings.mirrorH * mirrorScale),
                    composeMirrorWindow);
    std::thread([] { scrcpyEmbedLoop(); }).detach();
#else
    state.scrcpyOpen = true;
    const std::string cmd = core::shellQuote(scrcpy) + " -s " + core::shellQuote(serial) + " &";
    std::system(cmd.c_str());
#endif
    toast("已启动投屏", "正在连接设备，连接后画面会显示在投屏窗口内。");
}


// -----------------------------------------------------------------------------
// Update checker & installer (adb / scrcpy / this app)
// -----------------------------------------------------------------------------

std::string scrcpyTargetDir() {
    const std::string s = findScrcpy();
    if (!s.empty()) {
        std::filesystem::path p(s);
        const std::string dir = p.parent_path().string();
        const std::string exe = executableDir();
        if (dir == exe || (dir.size() > exe.size() && dir.compare(0, exe.size(), exe) == 0 &&
                           (dir[exe.size()] == '\\' || dir[exe.size()] == '/'))) {
            return dir;
        }
    }
    return executableDir() + "\\scrcpy";
}

std::string adbTargetDir(const std::string& adbPath) {
    if (!adbPath.empty()) {
        std::filesystem::path p(adbPath);
        const std::string dir = p.parent_path().string();
        const std::string exe = executableDir();
        if (dir == exe || (dir.size() > exe.size() && dir.compare(0, exe.size(), exe) == 0 &&
                           (dir[exe.size()] == '\\' || dir[exe.size()] == '/'))) {
            return dir;
        }
        return dir;  // SDK / PATH installs: update in place.
    }
    return executableDir();
}

bool extractZip(const std::string& zipPath, const std::string& destDir, std::string& err) {
    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);
    if (ec) { err = "创建解压目录失败：" + ec.message(); return false; }
    // Windows 10+ ships bsdtar (tar.exe) which handles zip archives.
    ProcessResult r = runProcess("tar", {"-xf", zipPath, "-C", destDir}, 600000);
    if (r.exitCode == 0) return true;
    // Fallback to PowerShell's Expand-Archive. Note: -NoProfile -Command does
    // not need an execution-policy override (that flag only governs .ps1 files)
    // and "-ExecutionPolicy Bypass" is a classic malware marker that antivirus
    // engines weight heavily, so it is deliberately not used here.
    const std::string ps = "[Console]::OutputEncoding=[Text.Encoding]::UTF8; Expand-Archive -LiteralPath '" + zipPath + "' -DestinationPath '" + destDir + "' -Force";
    ProcessResult r2 = runProcess("powershell", {"-NoProfile", "-NonInteractive", "-Command", ps}, 600000);
    if (r2.exitCode == 0) return true;
    err = "解压失败：" + (core::trim(r2.out).empty() ? r2.err : core::trim(r2.out));
    return false;
}

std::string findDirContaining(const std::filesystem::path& root, const std::string& name, int depth) {
    if (depth > 6) return "";
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (e.is_directory(ec)) {
            std::string r = findDirContaining(e.path(), name, depth + 1);
            if (!r.empty()) return r;
        } else if (!ec && e.path().filename().string() == name) {
            return e.path().parent_path().string();
        }
    }
    return "";
}

bool copyFileOver(const std::filesystem::path& src, const std::filesystem::path& dst, std::string& err) {
    // core::replaceFileOver rather than std::filesystem::copy_file directly: the
    // adb updater replaces adb.exe while it is still running, and Windows refuses
    // to overwrite a running image (it only allows renaming it). See core/fileio.h.
    return core::replaceFileOver(src.string(), dst.string(), err);
}

void cleanupDir(const std::string& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

std::string makeUpdateWorkDir(const char* tag) {
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::filesystem::path base = std::filesystem::temp_directory_path();
    std::filesystem::path dir = base / (std::string("adb_browser_") + tag + "_" + std::to_string(ts));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

#ifdef _WIN32
struct HttpResult {
    bool ok = false;
    long status = 0;
    std::string body;
    std::string error;
};

struct WinHttpRequest {
    HINTERNET session = nullptr;
    HINTERNET conn = nullptr;
    HINTERNET request = nullptr;
    long status = 0;
    void close() {
        if (request) { WinHttpCloseHandle(request); request = nullptr; }
        if (conn) { WinHttpCloseHandle(conn); conn = nullptr; }
        if (session) { WinHttpCloseHandle(session); session = nullptr; }
    }
    ~WinHttpRequest() { close(); }
};

bool openWinHttpRequest(const std::string& url, WinHttpRequest& wr, std::string& err) {
    std::wstring current = core::toWide(url);
    for (int hop = 0; hop < 8; ++hop) {
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256];
        wchar_t path[4096];
        uc.lpszHostName = host;
        uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = 4096;
        if (!WinHttpCrackUrl(current.c_str(), 0, 0, &uc)) { err = "无法解析 URL"; return false; }

        if (wr.session == nullptr) {
            wr.session = WinHttpOpen(L"adb-browser-updater/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!wr.session) { err = "WinHttpOpen 失败"; return false; }
            // Bound DNS/connect/send/receive so a blocked or slow host can't
            // leave the "checking" state hanging forever (e.g. GitHub in CN).
            WinHttpSetTimeouts(wr.session, 5000, 6000, 10000, 15000);
        }
        if (wr.conn) { WinHttpCloseHandle(wr.conn); wr.conn = nullptr; }
        if (wr.request) { WinHttpCloseHandle(wr.request); wr.request = nullptr; }

        wr.conn = WinHttpConnect(wr.session, host, uc.nPort, 0);
        if (!wr.conn) { err = "WinHttpConnect 失败"; return false; }
        const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        wr.request = WinHttpOpenRequest(wr.conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!wr.request) { err = "WinHttpOpenRequest 失败"; return false; }
        const wchar_t* headers = L"User-Agent: adb-browser-updater/1.0\r\nAccept: */*\r\n";
        if (!WinHttpSendRequest(wr.request, headers, -1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(wr.request, nullptr)) {
            err = "请求失败";
            return false;
        }
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(wr.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        wr.status = static_cast<long>(status);
        if (status == 200) return true;
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            wchar_t location[4096];
            DWORD locSize = sizeof(location);
            if (WinHttpQueryHeaders(wr.request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                    location, &locSize, WINHTTP_NO_HEADER_INDEX)) {
                std::wstring next(location, locSize / sizeof(wchar_t));
                if (next.rfind(L"http", 0) != 0) {
                    std::wstring base = (uc.nScheme == INTERNET_SCHEME_HTTPS ? L"https://" : L"http://");
                    base += host;
                    if (next.empty() || next[0] != L'/') base += L"/";
                    base += next;
                    next = base;
                }
                current = next;
                continue;
            }
            err = "重定向失败";
            return false;
        }
        err = "HTTP " + std::to_string(status);
        return false;
    }
    err = "重定向次数过多";
    return false;
}

HttpResult httpGetString(const std::string& url, std::size_t maxBytes) {
    HttpResult res;
    WinHttpRequest wr;
    std::string err;
    if (!openWinHttpRequest(url, wr, err)) { res.error = err; return res; }
    res.status = wr.status;
    std::vector<char> buf;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(wr.request, &avail) && avail > 0) {
        if (buf.size() + avail > maxBytes) break;
        const std::size_t old = buf.size();
        buf.resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(wr.request, buf.data() + old, avail, &read)) break;
        buf.resize(old + read);
        if (read == 0) break;
    }
    res.body.assign(buf.begin(), buf.end());
    res.ok = true;
    return res;
}

bool downloadFile(const std::string& url, const std::wstring& outPath, std::string& err) {
    WinHttpRequest wr;
    if (!openWinHttpRequest(url, wr, err)) return false;
    if (wr.status != 200) { err = "HTTP " + std::to_string(wr.status); return false; }

    DWORD contentLength = 0;
    DWORD lenSize = sizeof(contentLength);
    WinHttpQueryHeaders(wr.request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &lenSize, WINHTTP_NO_HEADER_INDEX);
    g_dlTotal = contentLength;
    g_dlReceived = 0;

    HANDLE file = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { err = "无法写入临时文件"; return false; }
    long long received = 0;
    std::vector<char> buf(65536);
    bool ok = true;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(wr.request, &avail) && avail > 0) {
        const DWORD want = avail > static_cast<DWORD>(buf.size()) ? static_cast<DWORD>(buf.size()) : avail;
        DWORD read = 0;
        if (!WinHttpReadData(wr.request, buf.data(), want, &read) || read == 0) {
            err = "读取数据失败";
            ok = false;
            break;
        }
        DWORD written = 0;
        if (!WriteFile(file, buf.data(), read, &written, nullptr) || written != read) {
            err = "写入临时文件失败";
            ok = false;
            break;
        }
        received += read;
        g_dlReceived = received;
    }
    CloseHandle(file);
    g_dlReceived = received;
    return ok;
}

// GitHub (github.com and its release CDN) is often unreachable in some regions.
// If a direct download fails, retry through common GitHub mirror prefixes.
bool downloadFileWithFallback(const std::string& url, const std::wstring& outPath, std::string& err) {
    if (downloadFile(url, outPath, err)) {
        return true;
    }
    if (url.find("github.com") == std::string::npos &&
        url.find("githubusercontent.com") == std::string::npos) {
        return false;
    }
    static const char* kProxies[] = {
        "https://ghfast.top/",
        "https://gh-proxy.com/",
        "https://gh.con.sh/",
    };
    for (const char* proxy : kProxies) {
        if (downloadFile(std::string(proxy) + url, outPath, err)) {
            return true;
        }
    }
    return false;
}

void stopScrcpyIfRunning() {
    if (state.scrcpyOpen) closeMirrorWindow();
    if (g_scrcpyProcess != nullptr) {
        TerminateProcess(g_scrcpyProcess, 0);
        CloseHandle(g_scrcpyProcess);
        g_scrcpyProcess = nullptr;
    }
    HWND hwnd = FindWindowW(nullptr, L"adb_browser_scrcpy");
    if (hwnd != nullptr) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    state.scrcpyOpen = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
}

app::async::Result<std::string> installScrcpyWorker(const std::string& url, const std::string& version,
                                                    const std::string& expectedSha256) {
    const std::string work = makeUpdateWorkDir("scrcpy");
    const std::string zip = work + "\\scrcpy.zip";
    const std::string extracted = work + "\\extracted";
    std::string err;

    g_dlStage = 1;
    if (!downloadFileWithFallback(url, core::toWide(zip), err)) {
        cleanupDir(work);
        return app::async::failure<std::string>("下载失败：" + err);
    }
    // Downloads can travel via a third-party GitHub mirror, so verify the
    // archive against the checksum published in the release before unpacking.
    if (!expectedSha256.empty()) {
        const std::string actual = core::sha256FileHex(core::toWide(zip), err);
        if (actual.empty()) {
            cleanupDir(work);
            return app::async::failure<std::string>("校验失败：" + err);
        }
        if (core::lower(actual) != core::lower(expectedSha256)) {
            cleanupDir(work);
            return app::async::failure<std::string>("scrcpy 更新包校验失败（SHA-256 不匹配），已取消安装。");
        }
    }
    g_dlStage = 2;
    if (!extractZip(zip, extracted, err)) {
        cleanupDir(work);
        return app::async::failure<std::string>(err);
    }
    const std::string srcDir = findDirContaining(extracted, "scrcpy.exe", 0);
    if (srcDir.empty()) {
        cleanupDir(work);
        return app::async::failure<std::string>("压缩包中未找到 scrcpy.exe");
    }
    g_dlStage = 3;
    const std::string target = scrcpyTargetDir();
    std::error_code ec;
    std::filesystem::create_directories(target, ec);
    for (const auto& e : std::filesystem::directory_iterator(srcDir, ec)) {
        if (ec) break;
        std::filesystem::copy(e.path(), std::filesystem::path(target) / e.path().filename(),
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { err = "安装失败：" + ec.message(); break; }
    }
    cleanupDir(work);
    if (!err.empty()) return app::async::failure<std::string>(err);
    return app::async::success<std::string>("scrcpy 已更新到 " + version);
}

app::async::Result<std::string> installAdbWorker(const std::string& url, const std::string& version,
                                                 const std::string& adbPath,
                                                 const std::string& expectedSha1) {
    const std::string work = makeUpdateWorkDir("adb");
    const std::string zip = work + "\\platform-tools.zip";
    const std::string extracted = work + "\\extracted";
    std::string err;

    g_dlStage = 1;
    if (!downloadFile(url, core::toWide(zip), err)) {
        // Fallback to the always-current "latest" URL if the versioned one fails.
        const std::string fallback = "https://dl.google.com/android/repository/platform-tools-latest-windows.zip";
        if (!downloadFile(fallback, core::toWide(zip), err)) {
            cleanupDir(work);
            return app::async::failure<std::string>("下载失败：" + err);
        }
    }
    // Google publishes a SHA-1 (not SHA-256) in repository2-1.xml. It is weak
    // against deliberate collisions but still catches truncated or substituted
    // downloads; a mismatch is reported instead of being ignored.
    std::string adbVerifyWarning;
    if (!expectedSha1.empty()) {
        std::string hashErr;
        const std::string actual = core::sha1FileHex(core::toWide(zip), hashErr);
        if (!actual.empty() && core::lower(actual) != core::lower(expectedSha1)) {
            adbVerifyWarning = "（注意：下载包校验值与官方公布的不一致，可能不是官方文件）";
        }
    }
    g_dlStage = 2;
    if (!extractZip(zip, extracted, err)) {
        cleanupDir(work);
        return app::async::failure<std::string>(err);
    }
    const std::string srcDir = findDirContaining(extracted, "adb.exe", 0);
    if (srcDir.empty()) {
        cleanupDir(work);
        return app::async::failure<std::string>("压缩包中未找到 adb.exe");
    }
    g_dlStage = 3;
    if (!adbPath.empty()) runProcess(adbPath, core::killServerArgs(), 15000);
    const std::string target = adbTargetDir(adbPath);
    std::error_code ec;
    std::filesystem::create_directories(target, ec);
    static const char* kAdbFiles[] = {"adb.exe", "fastboot.exe", "AdbWinApi.dll",
                                      "AdbWinUsbApi.dll", "source.properties"};
    for (const char* f : kAdbFiles) {
        const std::filesystem::path src = std::filesystem::path(srcDir) / f;
        if (!std::filesystem::exists(src, ec)) continue;
        if (!copyFileOver(src, std::filesystem::path(target) / f, err)) {
            cleanupDir(work);
            return app::async::failure<std::string>(err);
        }
    }
    cleanupDir(work);
    return app::async::success<std::string>("adb 已更新到 " + version + adbVerifyWarning);
}

app::async::Result<std::string> installAppUpdateWorker(const std::string& url, const std::string& version,
                                                       const std::string& expectedSha256) {
    const std::string work = makeUpdateWorkDir("app");
    const std::string zip = work + "\\app.zip";
    const std::string extracted = work + "\\extracted";
    std::string err;

    g_dlStage = 1;
    if (!downloadFileWithFallback(url, core::toWide(zip), err)) {
        cleanupDir(work);
        return app::async::failure<std::string>("下载失败：" + err);
    }
    // Verify the archive before anything is unpacked or executed. The digest
    // comes from the GitHub release metadata (or its .sha256 sidecar).
    if (!expectedSha256.empty()) {
        const std::string actual = core::sha256FileHex(core::toWide(zip), err);
        if (actual.empty()) {
            cleanupDir(work);
            return app::async::failure<std::string>("校验失败：" + err);
        }
        if (core::lower(actual) != core::lower(expectedSha256)) {
            cleanupDir(work);
            return app::async::failure<std::string>("更新包校验失败（SHA-256 不匹配），已取消安装。");
        }
    }
    g_dlStage = 2;
    if (!extractZip(zip, extracted, err)) {
        cleanupDir(work);
        return app::async::failure<std::string>(err);
    }
    const std::string srcDir = findDirContaining(extracted, "adb_browser.exe", 0);
    if (srcDir.empty()) {
        cleanupDir(work);
        return app::async::failure<std::string>("更新包中未找到 adb_browser.exe");
    }
    g_dlStage = 3;

    const std::string exeDir = executableDir();
    const std::string exePath = exeDir + "\\adb_browser.exe";
    const std::string backupPath = exeDir + "\\adb_browser.exe.old";
    const std::string newExe = srcDir + "\\adb_browser.exe";

    std::error_code ec;
    if (!std::filesystem::exists(newExe, ec)) {
        cleanupDir(work);
        return app::async::failure<std::string>("更新包中未找到 adb_browser.exe");
    }

    // Note: the archive itself was already verified against the published
    // sha256 above, and the executable we copy out of it comes straight from
    // that verified archive - so no second digest is needed here.

    // Windows allows renaming a running executable (but not overwriting it), so
    // move the current one aside and copy the new one into place.
    std::filesystem::remove(backupPath, ec);
    ec.clear();
    std::filesystem::rename(exePath, backupPath, ec);
    if (ec) {
        cleanupDir(work);
        return app::async::failure<std::string>("无法替换当前程序：" + ec.message());
    }
    if (!copyFileOver(newExe, exePath, err)) {
        std::filesystem::rename(backupPath, exePath, ec);  // roll back
        cleanupDir(work);
        return app::async::failure<std::string>(err);
    }

    // Best-effort: also refresh assets/ and scrcpy/ if the package carries them.
    //
    // These used to be replaced by remove_all(dst) followed by copy(). If that
    // copy failed halfway - disk full, antivirus locking a file, a locked
    // scrcpy.exe - the app was left with an EMPTY assets/ or scrcpy/ directory,
    // i.e. a freshly "updated" program with no fonts, no icon and no adb. Now
    // the old directory is moved aside first and only deleted once the new one
    // is fully in place; a failure rolls it back and is reported.
    std::string subError;
    for (const char* sub : {"assets", "scrcpy"}) {
        const std::filesystem::path src = std::filesystem::path(srcDir) / sub;
        if (!std::filesystem::exists(src, ec)) continue;
        ec.clear();
        const std::filesystem::path dst = std::filesystem::path(exeDir) / sub;
        const std::filesystem::path backup = std::filesystem::path(exeDir) / (std::string(sub) + ".old");

        // Drop a leftover backup from an earlier update; ignore failures here,
        // a locked scrcpy.exe must not prevent the update itself.
        std::filesystem::remove_all(backup, ec);
        ec.clear();

        const bool hadExisting = std::filesystem::exists(dst, ec);
        ec.clear();
        if (hadExisting) {
            std::filesystem::rename(dst, backup, ec);
            if (ec) {
                subError = std::string("无法更新 ") + sub + " 目录：" + ec.message();
                break;
            }
        }

        std::filesystem::copy(src, dst,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing,
                              ec);
        if (ec) {
            const std::string reason = ec.message();
            ec.clear();
            if (hadExisting) {
                std::filesystem::remove_all(dst, ec);   // undo the partial copy
                ec.clear();
                std::filesystem::rename(backup, dst, ec);  // then restore originals
                ec.clear();
            }
            subError = std::string("更新 ") + sub + " 目录失败（已保留原目录）：" + reason;
            break;
        }

        if (hadExisting) {
            std::filesystem::remove_all(backup, ec);
            ec.clear();
        }
    }

    cleanupDir(work);
    if (!subError.empty()) {
        // adb_browser.exe was already replaced, so say so explicitly: the app
        // is updated, only the accompanying folders were left untouched.
        return app::async::failure<std::string>(
            "已更新到 " + version + "，但 " + subError + "。重启程序后新版本生效。");
    }
    return app::async::success<std::string>("已更新到 " + version + "，重启程序后生效。");
}
#endif  // _WIN32








void checkForUpdates();
void openUpdateDialog() {
    state.updateOpen = true;
    if (!g_update.adbLatest.empty() || !g_update.scrcpyLatest.empty()) return;  // already checked
    checkForUpdates();
}

void checkForUpdates() {
    if (state.updateChecking || state.updateWorking) return;
    state.updateChecking = true;
    state.updateStatus = "正在检查更新…";
    const std::string adb = state.adbPath;
    const std::string scrcpy = findScrcpy();
    app::async::runOnce(
        "update.check",
        [adb, scrcpy]() -> app::async::Result<UpdateInfo> {
            UpdateInfo info;
            if (!adb.empty()) {
                ProcessResult r = runProcess(adb, {"version"}, 15000);
                if (r.exitCode == 0) info.adbCurrent = core::adbShortVersion(r.out);
            }
            if (!scrcpy.empty()) {
                ProcessResult r = runProcess(scrcpy, {"--version"}, 15000);
                if (r.exitCode == 0) info.scrcpyCurrent = core::scrcpyShortVersion(r.out);
            }
#ifdef _WIN32
            HttpResult hr = httpGetString("https://api.github.com/repos/Genymobile/scrcpy/releases/latest", 4 * 1024 * 1024);
            if (hr.ok) {
                std::string tag = core::jsonStringValue(hr.body, "tag_name");
                if (!tag.empty() && tag[0] == 'v') tag = tag.substr(1);
                info.scrcpyLatest = tag;
                info.scrcpyUrl = core::findWin64AssetUrl(hr.body);
                if (info.scrcpyUrl.empty() && !tag.empty()) {
                    info.scrcpyUrl = "https://github.com/Genymobile/scrcpy/releases/download/v" + tag +
                                     "/scrcpy-win64-v" + tag + ".zip";
                }
                info.scrcpySha256 = core::findAssetSha256(hr.body, info.scrcpyUrl);
            } else if (info.error.empty()) {
                info.error = "获取 scrcpy 最新版本失败：" + hr.error;
            }
            // adb: platform-tools revision from Google's official repository XML
            // (dl.google.com is reachable even where GitHub is not).
            HttpResult hr2 = httpGetString("https://dl.google.com/android/repository/repository2-1.xml", 8 * 1024 * 1024);
            if (hr2.ok) {
                info.adbLatest = core::parsePlatformToolsVersion(hr2.body);
                // Prefer the exact archive URL + checksum published for Windows.
                core::parsePlatformToolsWindowsArchive(hr2.body, info.adbUrl, info.adbSha1);
                if (info.adbUrl.empty() && !info.adbLatest.empty()) {
                    info.adbUrl = "https://dl.google.com/android/repository/platform-tools_r" +
                                  info.adbLatest + "-windows.zip";
                }
            } else if (info.error.empty()) {
                info.error = "获取 adb 最新版本失败：" + hr2.error;
            }
            // This app itself: latest release from the configured GitHub repo.
            info.appCurrent = kAppVersion;
            HttpResult hr3 = httpGetString(std::string("https://api.github.com/repos/") + kAppUpdateRepo + "/releases/latest", 2 * 1024 * 1024);
            if (hr3.ok) {
                std::string tag = core::jsonStringValue(hr3.body, "tag_name");
                if (!tag.empty() && tag[0] == 'v') tag = tag.substr(1);
                info.appLatest = tag;
                core::findAppReleaseAsset(hr3.body, info.appUrl, info.appRow, info.appSha256);
                // Older releases carry no asset "digest"; fall back to the
                // ".sha256" sidecar the release workflow publishes next to each
                // artifact (the digest lookup itself lives in core/package).
                if (info.appSha256.empty() && !info.appUrl.empty()) {
                    HttpResult hrSha = httpGetString(info.appUrl + ".sha256", 64 * 1024);
                    if (hrSha.ok) {
                        const std::string body = core::trim(hrSha.body);
                        // A mirror answering with an HTML error page must not be
                        // mistaken for a digest.
                        if (!body.empty() && body[0] != '<') {
                            const auto rows = core::parseSha256Rows(body);
                            if (!rows.empty()) info.appSha256 = rows.front().second;
                        }
                    }
                }
            } else if (info.error.empty()) {
                info.error = "获取本软件最新版本失败：" + hr3.error;
            }
#else
            if (info.error.empty()) info.error = "此平台暂不支持在线检查更新。";
#endif
            info.adbUpdate = !info.adbLatest.empty() &&
                             (info.adbCurrent.empty() || core::compareVersions(info.adbCurrent, info.adbLatest) < 0);
            info.scrcpyUpdate = !info.scrcpyLatest.empty() &&
                                (info.scrcpyCurrent.empty() || core::compareVersions(info.scrcpyCurrent, info.scrcpyLatest) < 0);
            info.appUpdate = !info.appLatest.empty() &&
                             core::compareVersions(info.appCurrent, info.appLatest) < 0;
            return app::async::success(info);
        },
        [](const app::async::Result<UpdateInfo>& result) {
            state.updateChecking = false;
            if (!result.ok) {
                state.updateStatus = "检查失败：" + result.error;
                return;
            }
            g_update = result.value;
            // An install requested before this check ran (the setup dialog, the
            // "no adb installed" state, the mirroring button) could only get its
            // download URL now.
            if (g_pendingAdbInstall || g_pendingScrcpyInstall) {
                if (!advanceInstallQueue()) {
                    g_pendingAdbInstall = false;
                    g_pendingScrcpyInstall = false;
                    state.updateStatus = "未能获取下载地址，请检查网络后重试。";
                    toast("下载失败", state.updateStatus);
                }
                return;
            }
            if (g_update.adbUpdate || g_update.scrcpyUpdate || g_update.appUpdate) {
                // Distinguish "newer release exists but has no downloadable ZIP"
                // from a normal update, so the status line does not promise an
                // update the button cannot perform.
                if (g_update.appUpdate && g_update.appUrl.empty()) {
                    state.updateStatus = "有更新的版本 " + g_update.appLatest +
                                         "，但该 Release 没有可用的下载包，请到 Releases 页面手动下载。";
                } else {
                    state.updateStatus = "发现新版本，点击下方按钮即可更新。";
                }
            } else if (!g_update.adbLatest.empty() || !g_update.scrcpyLatest.empty() || !g_update.appLatest.empty()) {
                state.updateStatus = "已是最新版本。";
            } else {
                state.updateStatus = g_update.error.empty() ? "无法获取最新版本信息。" : g_update.error;
            }
        });
}

void startUpdateScrcpy() {
    if (state.updateWorking) return;
    if (g_update.scrcpyUrl.empty() || g_update.scrcpyLatest.empty()) {
        toast("无法更新", "未获取到 scrcpy 下载地址，请先检查更新。");
        return;
    }
    const std::string url = g_update.scrcpyUrl;
    const std::string version = g_update.scrcpyLatest;
    state.updateWorking = true;
    state.updateStatus = "正在更新 scrcpy…";
    g_dlStage = 0;
    g_dlTotal = 0;
    g_dlReceived = 0;
#ifdef _WIN32
    stopScrcpyIfRunning();
    const std::string sha256 = g_update.scrcpySha256;
    app::async::restart(
        "update.install.scrcpy",
        [url, version, sha256]() -> app::async::Result<std::string> {
            return installScrcpyWorker(url, version, sha256);
        },
        [version](const app::async::Result<std::string>& result) {
            state.updateWorking = false;
            g_dlStage = 0;
            if (result.ok) {
                state.updateStatus = result.value;
                toast("更新完成", result.value);
                g_update.scrcpyCurrent = version;
                g_update.scrcpyUpdate = false;
            } else {
                state.updateStatus = result.error;
                toast("更新失败", result.error);
            }
            // Nothing else is normally queued behind scrcpy, but keep the queue
            // draining in one place so a failure mid-chain cannot strand it.
            advanceInstallQueue();
        });
#else
    state.updateWorking = false;
    toast("无法更新", "在线更新目前仅支持 Windows。");
#endif
}

void startUpdateAdb() {
    if (state.updateWorking) return;
    if (g_update.adbUrl.empty() || g_update.adbLatest.empty()) {
        toast("无法更新", "未获取到 adb 下载地址，请先检查更新。");
        return;
    }
    const std::string url = g_update.adbUrl;
    const std::string version = g_update.adbLatest;
    const std::string adbPath = state.adbPath;
    const std::string sha1 = g_update.adbSha1;
    state.updateWorking = true;
    state.updateStatus = "正在更新 adb…";
    g_dlStage = 0;
    g_dlTotal = 0;
    g_dlReceived = 0;
#ifdef _WIN32
    app::async::restart(
        "update.install.adb",
        [url, version, adbPath, sha1]() -> app::async::Result<std::string> {
            return installAdbWorker(url, version, adbPath, sha1);
        },
        [version](const app::async::Result<std::string>& result) {
            state.updateWorking = false;
            g_dlStage = 0;
            if (result.ok) {
                state.updateStatus = result.value;
                toast("更新完成", result.value);
                g_update.adbCurrent = version;
                g_update.adbUpdate = false;
                state.adbPath = findAdb();
                state.adbFound = !state.adbPath.empty();
                fetchAdbVersion();
            } else {
                state.updateStatus = result.error;
                toast("更新失败", result.error);
            }
            // "Download everything" still owes scrcpy, if it is missing.
            advanceInstallQueue();
        });
#else
    state.updateWorking = false;
    toast("无法更新", "在线更新目前仅支持 Windows。");
#endif
}

void startUpdateApp() {
    if (state.updateWorking) return;
    if (g_update.appUrl.empty() || g_update.appLatest.empty()) {
        toast("无法更新", "未获取到本软件的下载地址，请先检查更新。");
        return;
    }
    const std::string url = g_update.appUrl;
    const std::string version = g_update.appLatest;
    const std::string sha256 = g_update.appSha256;
    state.updateWorking = true;
    state.updateStatus = "正在更新本软件…";
    g_dlStage = 0;
    g_dlTotal = 0;
    g_dlReceived = 0;
#ifdef _WIN32
    app::async::restart(
        "update.install.app",
        [url, version, sha256]() -> app::async::Result<std::string> {
            return installAppUpdateWorker(url, version, sha256);
        },
        [version](const app::async::Result<std::string>& result) {
            state.updateWorking = false;
            g_dlStage = 0;
            if (result.ok) {
                state.updateStatus = result.value;
                toast("更新完成", result.value);
                g_update.appCurrent = version;
                g_update.appUpdate = false;
            } else {
                state.updateStatus = result.error;
                toast("更新失败", result.error);
            }
        });
#else
    state.updateWorking = false;
    toast("无法更新", "在线更新目前仅支持 Windows。");
#endif
}

// --- Text preview / edit ---
void openTextPreview(const std::string& name) {
    if (state.selectedDevice.empty()) return;
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    const std::string remote = core::joinPath(state.currentPath, name);
    state.textPreviewOpen = true;
    state.textPreviewContent = "加载中…";
    state.textPreviewRemote = remote;
    app::async::restart(
        "adb.text.read",
        [adb, serial, remote]() -> app::async::Result<std::string> {
            ProcessResult r = runProcess(adb, {"-s", serial, "shell", "cat", core::shellQuote(remote)}, 30000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure<std::string>(msg.empty() ? "读取文件失败" : msg);
            }
            return app::async::success(r.out);
        },
        [](const app::async::Result<std::string>& result) {
            state.textPreviewContent = result.ok ? result.value : ("读取失败：\n" + result.error);
        });
}

void saveTextPreview() {
    if (state.textPreviewRemote.empty() || state.selectedDevice.empty()) return;
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    const std::string remote = state.textPreviewRemote;
    const std::string content = state.textPreviewContent;
    state.busy = true;
    app::async::runOnce(
        "adb.text.save",
        [adb, serial, remote, content]() -> app::async::Result<std::string> {
            const std::string tmp = "adb_browser_edit_tmp.txt";
            {
                std::ofstream out(tmp, std::ios::binary);
                if (!out) return app::async::failure<std::string>("无法写入临时文件");
                out.write(content.data(), static_cast<std::streamsize>(content.size()));
            }
            ProcessResult r = runProcess(adb, core::pushArgs(serial, tmp, remote), 120000);
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure<std::string>(
                    msg.empty() ? "保存失败（退出码 " + std::to_string(r.exitCode) + "）" : msg);
            }
            return app::async::success<std::string>("已保存");
        },
        [](const app::async::Result<std::string>& result) {
            state.busy = false;
            toast(result.ok ? "完成" : "保存失败", result.ok ? result.value : result.error);
        });
}

// --- Copy path / filename ---
void copyPathToClipboard() {
    if (state.selectedEntry.empty()) return;
    const std::string path = core::joinPath(state.currentPath, state.selectedEntry);
    // ::core is the framework's namespace; the unqualified "core" inside
    // namespace app is the alias for adb::core declared above.
    ::core::window::setClipboardText(path);
    toast("已复制路径", path);
}

void copyFileNameToClipboard() {
    if (state.selectedEntry.empty()) return;
    ::core::window::setClipboardText(state.selectedEntry);
    toast("已复制文件名", state.selectedEntry);
}

// --- File properties ---
void openFileProperties() {
    const FsEntry* e = findSelected();
    if (e == nullptr) return;
    std::string text;
    text += "名称: " + e->name + "\n";
    text += "路径: " + core::joinPath(state.currentPath, e->name) + "\n";
    text += "类型: " + std::string(e->isDir ? "文件夹" : (e->isLink ? "符号链接" : "文件")) + "\n";
    text += "大小: " + (e->isDir ? "-" : core::formatSize(e->size)) + "\n";
    text += "权限: " + e->perms + "\n";
    text += "修改时间: " + e->date + "\n";
    if (e->isLink) text += "链接目标: " + e->linkTarget + "\n";
    state.propertiesText = text;
    state.propertiesOpen = true;
}

// --- Wireless connect ---
void promptWirelessConnect() {
    state.promptTitle = "无线连接";
    state.promptValue = "192.168.1.100:5555";
    state.promptMode = 2;
    state.promptOpen = true;
}

// --- App management ---
void fetchAppList() {
    if (state.selectedDevice.empty()) return;
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    state.appListText = "加载中…";
    app::async::restart(
        "adb.apps",
        [adb, serial]() -> app::async::Result<std::vector<std::string>> {
            ProcessResult r = runProcess(adb, {"-s", serial, "shell", "pm", "list", "packages", "-3"}, 30000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure<std::vector<std::string>>(msg.empty() ? "获取应用列表失败" : msg);
            }
            std::vector<std::string> packages = core::parsePackageList(r.out);
            std::sort(packages.begin(), packages.end());
            return app::async::success(std::move(packages));
        },
        [](const app::async::Result<std::vector<std::string>>& result) {
            if (result.ok) {
                state.appPackages = result.value;
                state.appListText.clear();
            } else {
                state.appPackages.clear();
                state.appListText = "获取失败：\n" + result.error;
            }
        });
}

void openAppManage() {
    if (state.selectedDevice.empty()) return;
    state.appManageOpen = true;
    state.appSelectedPackage.clear();
    fetchAppList();
}

void uninstallSelectedApp() {
    if (state.appSelectedPackage.empty() || state.selectedDevice.empty()) return;
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    const std::string pkg = state.appSelectedPackage;
    state.busy = true;
    app::async::runOnce(
        "adb.uninstall",
        [adb, serial, pkg]() -> app::async::Result<std::string> {
            ProcessResult r = runProcess(adb, core::uninstallArgs(serial, pkg), 120000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure<std::string>(msg.empty() ? "卸载失败" : msg);
            }
            return app::async::success<std::string>("已卸载 " + pkg);
        },
        [](const app::async::Result<std::string>& result) {
            state.busy = false;
            toast(result.ok ? "完成" : "卸载失败", result.ok ? result.value : result.error);
            if (result.ok) fetchAppList();
        });
}

void clearSelectedAppData() {
    if (state.appSelectedPackage.empty() || state.selectedDevice.empty()) return;
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    const std::string pkg = state.appSelectedPackage;
    state.busy = true;
    app::async::runOnce(
        "adb.app.clear",
        [adb, serial, pkg]() -> app::async::Result<std::string> {
            ProcessResult r = runProcess(adb, core::clearAppDataArgs(serial, pkg), 120000);
            if (r.exitCode != 0) {
                std::string msg = core::trim(r.out);
                return app::async::failure<std::string>(msg.empty() ? "清数据失败" : msg);
            }
            return app::async::success<std::string>("已清理 " + pkg + " 的数据");
        },
        [](const app::async::Result<std::string>& result) {
            state.busy = false;
            toast(result.ok ? "完成" : "清数据失败", result.ok ? result.value : result.error);
        });
}

// --- Image preview ---
void openImagePreview(const std::string& name) {
    if (state.selectedDevice.empty()) return;
    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    const std::string remote = core::joinPath(state.currentPath, name);
    const std::string base = remote.substr(remote.find_last_of('/') + 1);
    const std::string tmp = ".adb_preview_" + base;
    state.imagePreviewRemote = remote;
    state.imagePreviewLocal.clear();
    state.imagePreviewOpen = true;
    app::async::restart(
        "adb.image.preview",
        [adb, serial, remote, tmp]() -> app::async::Result<std::string> {
            ProcessResult r = runProcess(adb, core::pullArgs(serial, remote, tmp), 120000);
            if (r.exitCode != 0) {
                return app::async::failure<std::string>("预览失败");
            }
            return app::async::success(std::string(tmp));
        },
        [](const app::async::Result<std::string>& result) {
            if (result.ok) {
                state.imagePreviewLocal = result.value;
            } else {
                state.imagePreviewLocal.clear();
                toast("预览失败", result.error);
            }
        });
}

void closeTopmost() {
    if (state.settingsOpen) state.settingsOpen = false;
    else if (state.updateOpen) state.updateOpen = false;
    else if (state.confirmDialogOpen) state.confirmDialogOpen = false;
    else if (state.promptOpen) state.promptOpen = false;
    else if (state.commandOutputOpen) state.commandOutputOpen = false;
    else if (state.deviceInfoOpen) state.deviceInfoOpen = false;
    else if (state.logcatOpen) state.logcatOpen = false;
    else if (state.textPreviewOpen) state.textPreviewOpen = false;
    else if (state.imagePreviewOpen) state.imagePreviewOpen = false;
    else if (state.propertiesOpen) state.propertiesOpen = false;
    else if (state.appManageOpen) state.appManageOpen = false;
    else if (state.helpOpen) state.helpOpen = false;
    else if (state.bookmarkManageOpen) state.bookmarkManageOpen = false;
    else if (state.commandManageOpen) state.commandManageOpen = false;
    else if (state.deviceMenuOpen) state.deviceMenuOpen = false;
    else if (state.rowMenuOpen) state.rowMenuOpen = false;
    else if (state.bookmarkMenuOpen) state.bookmarkMenuOpen = false;
    else if (state.commandMenuOpen) state.commandMenuOpen = false;
}

bool anyOverlayOpen() {
    return state.settingsOpen || state.updateOpen || state.confirmDialogOpen || state.promptOpen ||
           state.commandOutputOpen || state.deviceInfoOpen || state.logcatOpen ||
           state.textPreviewOpen || state.imagePreviewOpen || state.propertiesOpen ||
           state.appManageOpen || state.helpOpen || state.bookmarkManageOpen ||
           state.commandManageOpen || state.deviceMenuOpen || state.rowMenuOpen ||
           state.bookmarkMenuOpen || state.commandMenuOpen;
}

void handleGlobalKey(const eui::KeyEvent& ev) {
    if (!ev.isDown()) return;
    if (anyOverlayOpen()) {
        if (ev.key == eui::InputKey::Escape) closeTopmost();
        return;
    }
    if (state.pathEditing) {
        if (ev.key == eui::InputKey::Escape) {
            state.pathEditing = false;
            state.pathInput = state.currentPath;
            state.pathSuggestions.clear();
        }
        return;
    }
    switch (ev.key) {
        case eui::InputKey::F5: refreshListing(true); break;
        case eui::InputKey::F2: promptRename(); break;
        case eui::InputKey::Backspace: goUp(); break;
        case eui::InputKey::Enter: {
            const FsEntry* e = findSelected();
            if (e && e->isDir) enterDir(e->name);
            break;
        }
        case eui::InputKey::Delete: confirmDelete(); break;
        case eui::InputKey::F:
            if (ev.modifiers.control) {
                state.searchOpen = true;
                app::requestFocus("list.search.input.hit");
            }
            break;
        case eui::InputKey::L:
            if (ev.modifiers.control) {
                state.pathInput = state.currentPath;
                state.pathSuggestions.clear();
                state.pathEditing = true;
                app::requestFocus("path.edit.input.hit");
            }
            break;
        case eui::InputKey::C: if (ev.modifiers.control && !ev.modifiers.shift) doCopy(); break;
        case eui::InputKey::X: if (ev.modifiers.control) doMove(); break;
        case eui::InputKey::V: if (ev.modifiers.control) doPaste(); break;
        case eui::InputKey::A:
            if (ev.modifiers.control) {
                state.selectedSet.clear();
                for (const FsEntry& e : state.entries) state.selectedSet.insert(e.name);
            }
            break;
        case eui::InputKey::Escape:
            if (state.searchOpen) {
                state.fileFilter.clear();
                state.searchOpen = false;
                applyFileFilter();
                resetListScroll();
            }
            break;
        default: break;
    }
}

// =============================================================================
// UI helpers
// =============================================================================
struct Columns {
    float nameW = 0.0f;
    float sizeX = 0.0f;
    float dateX = 0.0f;
    float permsX = 0.0f;
};

Columns columns(float contentWidth) {
    const float permsW = 128.0f;
    const float dateW = 130.0f;
    const float sizeW = 84.0f;
    Columns c;
    c.permsX = contentWidth - permsW;
    c.dateX = c.permsX - dateW;
    c.sizeX = c.dateX - sizeW;
    c.nameW = c.sizeX - 34.0f - 8.0f;
    return c;
}

void toolButton(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
                unsigned int icon, const std::string& label, bool primary, bool enabled,
                std::function<void()> onClick) {
    eui::Color base = primary ? kAccent : kSurface;
    eui::Color hover = primary ? eui::mixColor(kAccent, kWhite, 0.14f) : kSurfaceHover;
    eui::Color pressed = primary ? eui::mixColor(kAccent, {0.0f, 0.0f, 0.0f, 1.0f}, 0.16f) : kSurfaceAct;
    eui::Color textCol = primary ? kWhite : kInk;

    ui.stack(id + ".wrap")
        .x(x).y(y).size(w, h)
        .content([&] {
            components::button(ui, id)
                .size(w, h)
                .icon(icon)
                .iconSize(13.0f)
                .fontSize(13.0f)
                .text(label)
                .colors(base, hover, pressed)
                .textColor(textCol)
                .iconColor(primary ? textCol : kAccent)
                .radius(8.0f)
                .border(1.0f, primary ? eui::mixColor(kAccent, kWhite, 0.25f) : kBorder)
                .shadow(0.0f, 0.0f, 0.0f, kClear)
                .transition(0.16f)
                .disabled(!enabled)
                .onClick(std::move(onClick))
                .build();
        })
        .build();
}

void centeredMessage(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
                     const std::string& text) {
    ui.text(id)
        .x(x).y(y).size(w, h)
        .text(text)
        .fontSize(15.0f)
        .lineHeight(15.0f)
        .color(kMuted)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .maxWidth(w - 24.0f)
        .wrap(true)
        .build();
}

// =============================================================================
// Composable sections
// =============================================================================
void composeFileRow(eui::Ui& ui, const std::string& rowId, std::int64_t index, float w, float h) {
    const FsEntry& e = state.displayEntries[static_cast<std::size_t>(index)];
    const bool selected = state.selectedSet.count(e.name) != 0;
    Columns c = columns(w);

    eui::Color normal = selected ? kAccentSoft : kClear;
    eui::Color hover = selected ? kAccentSoft : kSurfaceHover;
    eui::Color pressed = selected ? kAccentSoft : kSurfaceAct;

    unsigned int icon = e.isDir ? 0xF07B : (e.isLink ? 0xF0C1 : 0xF15B);
    eui::Color iconColor = e.isDir ? kAmber : (e.isLink ? kAccent : kMuted);

    std::string displayName = e.name;
    if (e.isLink && !e.linkTarget.empty()) displayName += " \xE2\x86\x92 " + e.linkTarget;

    ui.stack(rowId)
        .size(w, h)
        .content([&] {
            ui.rect(rowId + ".bg")
                .size(w, h - 2.0f)
                .states(normal, hover, pressed)
                .radius(6.0f)
                .onPress([](const eui::PointerEvent& ev, const eui::Rect&) {
                    state.lastClickCtrl = ev.modifiers.control;
                })
                .onClick([name = e.name, isDir = e.isDir] { onEntryClick(name, isDir); })
                .onContextMenu([name = e.name](const eui::PointerEvent& ev, const eui::Rect&) {
                    openRowMenu(static_cast<float>(ev.x), static_cast<float>(ev.y), name);
                })
                .build();

            ui.text(rowId + ".icon")
                .x(6.0f).y(0.0f).size(24.0f, h)
                .icon(icon)
                .fontSize(16.0f)
                .lineHeight(16.0f)
                .color(iconColor)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            ui.text(rowId + ".name")
                .x(34.0f).y(0.0f).size(c.nameW, h)
                .text(core::shorten(displayName, static_cast<int>(c.nameW / 9.0f)))
                .fontSize(15.0f)
                .lineHeight(15.0f)
                .color(kInk)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            ui.text(rowId + ".size")
                .x(c.sizeX).y(0.0f).size(78.0f, h)
                .text(e.isDir ? "" : core::formatSize(e.size))
                .fontSize(13.0f)
                .lineHeight(13.0f)
                .color(kMuted)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            ui.text(rowId + ".date")
                .x(c.dateX).y(0.0f).size(124.0f, h)
                .text(e.date)
                .fontSize(12.0f)
                .lineHeight(12.0f)
                .color(kMuted)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            ui.text(rowId + ".perms")
                .x(c.permsX).y(0.0f).size(122.0f, h)
                .text(e.perms)
                .fontSize(12.0f)
                .lineHeight(12.0f)
                .fontFamily("")
                .color(kMuted)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
        })
        .build();
}

void composeHeader(eui::Ui& ui, float x, float y, float w, float h) {
    Columns c = columns(w - kScrollbarWidth);
    auto sortable = [&](const std::string& id, float lx, float lw, const std::string& text, bool right, int col) {
        const bool active = (state.sortColumn == col);
        std::string display = text;
        if (active) display += state.sortAscending ? " \xE2\x86\x91" : " \xE2\x86\x93";
        ui.stack(id + ".wrap")
            .x(lx).y(y).size(lw, h)
            .content([&] {
                ui.rect(id + ".hit")
                    .size(lw, h)
                    .states(kClear, kSurfaceHover, kSurfaceAct)
                    .radius(4.0f)
                    .onClick([col] {
                        if (state.sortColumn == col) state.sortAscending = !state.sortAscending;
                        else { state.sortColumn = col; state.sortAscending = true; }
                        applySort();
                        saveSettings();
                    })
                    .build();
                ui.text(id + ".label")
                    .size(lw, h)
                    .text(display)
                    .fontSize(13.0f)
                    .lineHeight(13.0f)
                    .color(active ? kAccent : kMuted)
                    .horizontalAlign(right ? eui::HorizontalAlign::Right : eui::HorizontalAlign::Left)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            })
            .build();
    };
    sortable("hdr.name", x + 34.0f, c.nameW, "名称", false, 0);
    sortable("hdr.size", x + c.sizeX, 78.0f, "大小", true, 1);
    sortable("hdr.date", x + c.dateX, 124.0f, "修改时间", true, 2);
    ui.text("hdr.perms")
        .x(x + c.permsX).y(y).size(122.0f, h)
        .text("权限")
        .fontSize(13.0f)
        .lineHeight(13.0f)
        .color(kMuted)
        .horizontalAlign(eui::HorizontalAlign::Right)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
}

void composeFileList(eui::Ui& ui, float x, float y, float w, float h) {
    ui.rect("list.bg")
        .x(x).y(y).size(w, h)
        .color(kSurface)
        .radius(10.0f)
        .border(1.0f, kBorder)
        .build();

    if (!state.adbFound) {
        centeredMessage(ui, "list.noadb", x, y, w, h - 96.0f,
                        "未找到 adb。\n可以点下面的按钮自动下载官方 platform-tools，\n"
                        "也可以手动选择已有的 adb.exe。");
        // The release packages ship no adb (a bundled adb.exe is one of the
        // things antivirus products flag), so the app has to be able to fetch it.
        const float gap = 12.0f;
        const float bw = 172.0f;
        const float bx = x + w * 0.5f - bw - gap * 0.5f;
        toolButton(ui, "list.dladb", bx, y + h - 48.0f, bw, 36.0f,
                   0xF019, "下载并安装 adb", true, true, [] {
                       if (ensureToolsDownloadable(PendingInstall::adb)) {
                           toast("正在下载 adb", "下载完成后会自动放到程序目录并启用。");
                       } else {
                           toast("正在获取下载地址", "拿到地址后会自动开始下载 adb，请稍候。");
                       }
                   });
        toolButton(ui, "list.locate", bx + bw + gap, y + h - 48.0f, bw, 36.0f,
                   0xF002, "手动选择 adb", false, true, [] {
                       std::string exe = eui::platform::chooseFile(eui::platform::FileDialogOptions{});
                       if (!exe.empty()) {
                           state.adbPath = exe;
                           state.adbFound = true;
                           refreshDevices();
                       }
                   });
        return;
    }

    if (state.devices.empty()) {
        centeredMessage(ui, "list.nodev", x, y, w, h - 44.0f,
                        "未连接设备。\n请开启 USB 调试并连接设备后刷新。");
        toolButton(ui, "list.refreshdev", x + w * 0.5f - 80.0f, y + h - 48.0f, 160.0f, 36.0f,
                   0xF021, "刷新设备", true, true, [] { refreshDevices(); });
        return;
    }

    if (state.selectedDevice.empty()) {
        centeredMessage(ui, "list.nosel", x, y, w, h,
                        "设备未就绪。\n请从左上角设备菜单选择就绪的设备，\n或点“重启 adb 服务”重连。");
        return;
    }

    if ((state.loading && state.entries.empty())) {
        centeredMessage(ui, "list.loading", x, y, w, h, "加载中…");
        return;
    }

    if (state.entries.empty()) {
        centeredMessage(ui, "list.empty", x, y, w, h, "空文件夹");
        return;
    }

    if (state.displayEntries.empty()) {
        centeredMessage(ui, "list.nomatch", x, y, w, h, "没有匹配的文件");
    } else {
        components::virtualList(ui, "file.list." + std::to_string(state.listGeneration))
            .x(x + 6.0f).y(y + 6.0f)
            .size(w - 12.0f, h - 12.0f)
            .theme(themeTokens())
            .itemCount(static_cast<std::int64_t>(state.displayEntries.size()))
            .rowHeight(kRowHeight)
            .scrollbarWidth(kScrollbarWidth)
            .scrollbarGap(0.0f)
            .bind(state.fileListScroll)
            .row([&](eui::Ui& rui, const std::string& rowId, std::int64_t index, float rw, float rh) {
                composeFileRow(rui, rowId, index, rw, rh);
            })
            .build();
    }

    // Floating search box at the top-right of the list (Ctrl+F to show, X to hide).
    if (state.searchOpen) {
        ui.stack("list.search.wrap")
            .x(x + w - 244.0f).y(y + 4.0f).size(228.0f, 32.0f)
            .zIndex(20)
            .content([&] {
                components::input(ui, "list.search.input")
                    .theme(themeTokens())
                    .size(192.0f, 32.0f)
                    .fontSize(13.0f)
                    .fontFamily("")
                    .placeholder("搜索文件…")
                    .value(state.fileFilter)
                    .onChange([](const std::string& v) {
                        state.fileFilter = v;
                        applyFileFilter();
                        resetListScroll();
                    })
                    .build();
                ui.rect("list.search.close")
                    .x(196.0f).y(0.0f).size(32.0f, 32.0f)
                    .states(kClear, kSurfaceHover, kSurfaceAct)
                    .radius(6.0f)
                    .onClick([] {
                        state.fileFilter.clear();
                        state.searchOpen = false;
                        applyFileFilter();
                        resetListScroll();
                    })
                    .build();
                ui.text("list.search.close.icon")
                    .x(196.0f).y(0.0f).size(32.0f, 32.0f)
                    .icon(0xF00D)
                    .fontSize(12.0f).lineHeight(12.0f)
                    .color(kMuted)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            })
            .build();
    }
}

void composeTitleBar(eui::Ui& ui, float W, float h) {
    const float btnW = 46.0f;
    const bool maximized = app::isWindowMaximized();

    ui.rect("titlebar.bg")
        .size(W, h)
        .color(kTitleBar)
        .build();

    // Draggable region (covers everything except the window control buttons).
    ui.rect("titlebar.drag")
        .size(W - btnW * 6.0f, h)
        .color(kClear)
        .onPress([](const eui::PointerEvent&, const eui::Rect&) { onTitleBarPress(); })
        .build();

    ui.text("titlebar.title")
        .x(14.0f).y(0.0f).size(W - btnW * 6.0f - 28.0f, h)
        .text(std::string("ADB 文件浏览器  v") + kAppVersion)
        .fontSize(13.0f).lineHeight(13.0f)
        .color(kMuted)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    // Update checker
    ui.rect("titlebar.update.hit")
        .x(W - btnW * 6.0f).y(0.0f).size(btnW, h)
        .states(kClear, kSurfaceHover, kSurfaceAct)
        .onClick([] { openUpdateDialog(); })
        .build();
    ui.text("titlebar.update.icon")
        .x(W - btnW * 6.0f).y(0.0f).size(btnW, h)
        .icon(0xF019)
        .fontSize(14.0f).lineHeight(14.0f)
        .color(kInk)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    // Help
    ui.rect("titlebar.help.hit")
        .x(W - btnW * 5.0f).y(0.0f).size(btnW, h)
        .states(kClear, kSurfaceHover, kSurfaceAct)
        .onClick([] { state.helpOpen = true; })
        .build();
    ui.text("titlebar.help.icon")
        .x(W - btnW * 5.0f).y(0.0f).size(btnW, h)
        .icon(0xF059)
        .fontSize(14.0f).lineHeight(14.0f)
        .color(kInk)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    // Settings
    ui.rect("titlebar.settings.hit")
        .x(W - btnW * 4.0f).y(0.0f).size(btnW, h)
        .states(kClear, kSurfaceHover, kSurfaceAct)
        .onClick([] { openSettingsDialog(); })
        .build();
    ui.text("titlebar.settings.icon")
        .x(W - btnW * 4.0f).y(0.0f).size(btnW, h)
        .icon(0xF013)
        .fontSize(14.0f).lineHeight(14.0f)
        .color(kInk)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    // Minimize
    ui.rect("titlebar.min.hit")
        .x(W - btnW * 3.0f).y(0.0f).size(btnW, h)
        .states(kClear, kSurfaceHover, kSurfaceAct)
        .onClick([] { app::minimizeWindow(); })
        .build();
    ui.text("titlebar.min.icon")
        .x(W - btnW * 3.0f).y(0.0f).size(btnW, h)
        .icon(0xF2D1)
        .fontSize(12.0f).lineHeight(12.0f)
        .color(kInk)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    // Maximize / Restore
    ui.rect("titlebar.max.hit")
        .x(W - btnW * 2.0f).y(0.0f).size(btnW, h)
        .states(kClear, kSurfaceHover, kSurfaceAct)
        .onClick([] { app::toggleMaximizeWindow(); })
        .build();
    ui.text("titlebar.max.icon")
        .x(W - btnW * 2.0f).y(0.0f).size(btnW, h)
        .icon(maximized ? 0xF2D2 : 0xF2D0)
        .fontSize(12.0f).lineHeight(12.0f)
        .color(kInk)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    // Close
    ui.rect("titlebar.close.hit")
        .x(W - btnW).y(0.0f).size(btnW, h)
        .states(kClear, kCloseHover, kClosePressed)
        .onClick([] { stopLogcat(); saveWindowState(); app::closeWindow(); })
        .build();
    ui.text("titlebar.close.icon")
        .x(W - btnW).y(0.0f).size(btnW, h)
        .icon(0xF00D)
        .fontSize(13.0f).lineHeight(13.0f)
        .color(kInk)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
}

// A small hover label that appears below an element (never covering it).
void composeHoverTip(eui::Ui& ui, const std::string& id, const std::string& sourceId,
                     const std::string& text, float cx, float top) {
    const float tw = approxTextWidth(text, 12.0f) + 28.0f;
    ui.stack(id)
        .x(cx - tw * 0.5f).y(top)
        .size(tw, 26.0f)
        .zIndex(600)
        .hoverOpacityFrom(sourceId)
        .content([&] {
            ui.rect(id + ".bg")
                .size(tw, 26.0f)
                .color(kSurface)
                .radius(6.0f)
                .border(1.0f, kBorder)
                .build();
            ui.text(id + ".text")
                .size(tw, 26.0f)
                .text(text)
                .fontSize(12.0f).lineHeight(12.0f)
                .color(kInk)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
        })
        .build();
}

void composeTopBar(eui::Ui& ui, float x, float y, float w, float h) {
    const float devW = 200.0f;

    // Device selector (opens a context menu).
    std::string deviceLabel = state.selectedDevice.empty() ? "无设备" : state.selectedDevice;
    const float menuX = x;
    const float menuY = y + h + 6.0f;
    ui.stack("topbar.device")
        .x(x).y(y).size(devW, h)
        .content([&] {
            ui.rect("topbar.device.bg")
                .size(devW, h)
                .states(kSurface, kSurfaceHover, kSurfaceAct)
                .radius(8.0f)
                .border(1.0f, kBorder)
                .onClick([menuX, menuY] { openDeviceMenu(menuX, menuY); })
                .build();
            ui.text("topbar.device.icon")
                .x(10.0f).y(0.0f).size(26.0f, h)
                .icon(0xF10B)
                .fontSize(16.0f)
                .lineHeight(16.0f)
                .color(kAccent)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui.text("topbar.device.name")
                .x(42.0f).y(0.0f).size(devW - 74.0f, h)
                .text(core::shorten(deviceLabel, static_cast<int>((devW - 74.0f) / 8.0f)))
                .fontSize(14.0f)
                .lineHeight(14.0f)
                .color(kInk)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui.text("topbar.device.caret")
                .x(devW - 26.0f).y(0.0f).size(20.0f, h)
                .icon(0xF078)
                .fontSize(10.0f)
                .lineHeight(10.0f)
                .color(kMuted)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
        })
        .build();

    // Refresh-devices button.
    toolButton(ui, "topbar.refreshdev", x + devW + 6.0f, y, h, h, 0xF021, "", false, true,
               [] { refreshDevices(); });
    composeHoverTip(ui, "topbar.refreshdev.tip", "topbar.refreshdev.bg",
                    "刷新设备", x + devW + 6.0f + h * 0.5f, y + h + 4.0f);

    const float cmdX = x + devW + 6.0f + h + 6.0f;
    toolButton(ui, "topbar.command", cmdX, y, h, h, 0xF120, "", false, true,
               [cmdX, y, h] {
                   state.commandMenuOpen = true;
                   state.commandMenuX = cmdX;
                   state.commandMenuY = y + h + 4.0f;
               });
    composeHoverTip(ui, "topbar.command.tip", "topbar.command.bg",
                    "常用命令", cmdX + h * 0.5f, y + h + 4.0f);

    // Device tools + view options.
    const float ssX = cmdX + h + 6.0f;
    toolButton(ui, "topbar.screenshot", ssX, y, h, h, 0xF030, "", false, !state.selectedDevice.empty(),
               [] { doScreenshot(); });
    composeHoverTip(ui, "topbar.screenshot.tip", "topbar.screenshot.bg", "截图", ssX + h * 0.5f, y + h + 4.0f);
    const float apkX = ssX + h + 6.0f;
    toolButton(ui, "topbar.install", apkX, y, h, h, 0xF50D, "", false, !state.selectedDevice.empty(),
               [] { doInstallApk(); });
    composeHoverTip(ui, "topbar.install.tip", "topbar.install.bg", "安装 APK", apkX + h * 0.5f, y + h + 4.0f);
    const float infoX = apkX + h + 6.0f;
    toolButton(ui, "topbar.info", infoX, y, h, h, 0xF05A, "", false, !state.selectedDevice.empty(),
               [] { openDeviceInfo(); });
    composeHoverTip(ui, "topbar.info.tip", "topbar.info.bg", "设备信息", infoX + h * 0.5f, y + h + 4.0f);
    const float logcatX = infoX + h + 6.0f;
    toolButton(ui, "topbar.logcat", logcatX, y, h, h, 0xF15C, "", false, !state.selectedDevice.empty(),
               [] { openLogcat(); });
    composeHoverTip(ui, "topbar.logcat.tip", "topbar.logcat.bg", "logcat", logcatX + h * 0.5f, y + h + 4.0f);
    const float hiddenX = logcatX + h + 6.0f;
    toolButton(ui, "topbar.hidden", hiddenX, y, h, h, state.showHidden ? 0xF06E : 0xF070, "", false, true,
               [] { state.showHidden = !state.showHidden; refreshListing(); saveSettings(); });
    composeHoverTip(ui, "topbar.hidden.tip", "topbar.hidden.bg",
                    state.showHidden ? "隐藏文件已显示" : "显示隐藏文件", hiddenX + h * 0.5f, y + h + 4.0f);
    const float mirrorX = hiddenX + h + 6.0f;
    toolButton(ui, "topbar.mirror", mirrorX, y, h, h, 0xF108, "", false, !state.selectedDevice.empty(),
               [] { openMirror(); });
    composeHoverTip(ui, "topbar.mirror.tip", "topbar.mirror.bg", "投屏", mirrorX + h * 0.5f, y + h + 4.0f);

    // Right-aligned file actions (always icon-only, described by tooltips).
    const bool canAct = !state.selectedEntry.empty();
    float rightX = x + w;
    auto place = [&](const std::string& id, unsigned int icon, bool primary, bool enabled,
                     std::function<void()> cb, const std::string& tip) {
        rightX -= 38.0f;
        toolButton(ui, id, rightX, y, 38.0f, h, icon, "", primary, enabled, std::move(cb));
        composeHoverTip(ui, id + ".tip", id + ".bg", tip, rightX + 19.0f, y + h + 4.0f);
        rightX -= 6.0f;
    };

    place("topbar.delete", 0xF1F8, false, canAct, [] { confirmDelete(); }, "删除");
    place("topbar.rename", 0xF303, false, canAct, [] { promptRename(); }, "重命名");
    place("topbar.download", 0xF019, false, canAct, [] { doPull(); }, "下载");
    place("topbar.newfolder", 0xF65E, false, true, [] { promptNewFolder(); }, "新建文件夹");
    place("topbar.upload", 0xF093, true, true, [] { doPush(); }, "上传");
}

// Query directory name suggestions that match the path being typed.
void updatePathSuggestions() {
    const std::string input = state.pathInput;
    if (input.empty() || state.selectedDevice.empty()) {
        state.pathSuggestions.clear();
        return;
    }

    std::string dirPart;
    std::string partial;
    const std::size_t slash = input.rfind('/');
    if (slash == std::string::npos) {
        dirPart = state.currentPath;
        partial = input;
    } else if (slash == 0) {
        dirPart = "/";
        partial = input.substr(1);
    } else {
        dirPart = input.substr(0, slash);
        partial = input.substr(slash + 1);
    }
    if (partial.empty()) {
        state.pathSuggestions.clear();
        return;
    }

    const std::string adb = state.adbPath;
    const std::string serial = state.selectedDevice;
    const std::string partialLower = core::lower(partial);
    app::async::restart(
        "path.suggest",
        [adb, serial, dirPart, partialLower]() -> app::async::Result<std::vector<std::string>> {
            ProcessResult r = runProcess(adb, {"-s", serial, "shell", "ls", "-la", core::shellQuote(dirPart)}, 15000);
            if (r.exitCode != 0) {
                return app::async::success(std::vector<std::string>{});
            }
            std::vector<FsEntry> entries = core::parseLsLa(r.out);
            std::vector<std::string> suggestions;
            for (const FsEntry& e : entries) {
                if (!e.isDir) continue;
                if (core::lower(e.name).rfind(partialLower, 0) != 0) continue;
                suggestions.push_back((dirPart == "/" ? "/" : dirPart + "/") + e.name);
            }
            std::sort(suggestions.begin(), suggestions.end());
            if (suggestions.size() > 8) suggestions.resize(8);
            return app::async::success(std::move(suggestions));
        },
        [](const app::async::Result<std::vector<std::string>>& result) {
            if (result.ok) state.pathSuggestions = result.value;
        });
}

void composeBreadcrumb(eui::Ui& ui, float x, float y, float w, float h) {
    const float navW = h;
    const float step = navW + 8.0f;
    toolButton(ui, "path.back", x, y, navW, h, 0xF053, "", false, !state.pathBack.empty(), [] { goBack(); });
    toolButton(ui, "path.forward", x + step, y, navW, h, 0xF054, "", false, !state.pathForward.empty(), [] { goForward(); });
    toolButton(ui, "path.up", x + step * 2.0f, y, navW, h, 0xF062, "", false, true, [] { goUp(); });
    toolButton(ui, "path.refresh", x + step * 3.0f, y, navW, h, 0xF021, "", false, true,
               [] { refreshListing(true); });
    const float bookX = x + step * 4.0f;
    toolButton(ui, "path.bookmark", bookX, y, navW, h, 0xF005, "", false, true,
               [bookX, y, h] {
                   state.bookmarkMenuOpen = true;
                   state.bookmarkMenuX = bookX;
                   state.bookmarkMenuY = y + h + 4.0f;
               });
    composeHoverTip(ui, "path.bookmark.tip", "path.bookmark.bg",
                    "快捷路径", bookX + h * 0.5f, y + h + 4.0f);

    const float crumbX = x + step * 5.0f;
    const float crumbW = std::max(0.0f, w - step * 5.0f);

    if (state.pathEditing) {
        // Editing mode: type an arbitrary path, then confirm or cancel.
        const float btnW = 32.0f;
        const float inputW = crumbW - btnW * 2.0f - 16.0f;
        ui.stack("path.edit.wrap")
            .x(crumbX).y(y).size(crumbW, h)
            .content([&] {
                components::input(ui, "path.edit.input")
                    .theme(themeTokens())
                    .size(inputW, h)
                    .fontSize(14.0f)
                    .fontFamily("")
                    .placeholder("/")
                    .value(state.pathInput)
                    .onChange([](const std::string& v) {
                        state.pathInput = v;
                        updatePathSuggestions();
                    })
                    .onEnter([] {
                        state.pathSuggestions.clear();
                        state.pathEditing = false;
                        goToPath(state.pathInput);
                    })
                    .build();
                toolButton(ui, "path.edit.ok", inputW + 8.0f, 0.0f, btnW, h, 0xF00C, "", true, true,
                           [] {
                               state.pathSuggestions.clear();
                               state.pathEditing = false;
                               goToPath(state.pathInput);
                           });
                toolButton(ui, "path.edit.cancel", inputW + btnW + 16.0f, 0.0f, btnW, h, 0xF00D, "", false, true,
                           [] {
                               state.pathSuggestions.clear();
                               state.pathEditing = false;
                               state.pathInput = state.currentPath;
                           });
            })
            .build();

        // Auto-complete suggestions.
        if (!state.pathSuggestions.empty()) {
            const float sH = std::min(232.0f, static_cast<float>(state.pathSuggestions.size()) * 28.0f + 8.0f);
            ui.stack("path.suggest")
                .x(crumbX).y(y + h + 4.0f)
                .size(crumbW, sH)
                .zIndex(700)
                .content([&] {
                    ui.rect("path.suggest.bg")
                        .size(crumbW, sH)
                        .color(kSurface)
                        .radius(8.0f)
                        .border(1.0f, kBorder)
                        .build();
                    for (std::size_t i = 0; i < state.pathSuggestions.size(); ++i) {
                        const std::string& s = state.pathSuggestions[i];
                        const float iy = 4.0f + static_cast<float>(i) * 28.0f;
                        ui.rect("path.suggest.item." + std::to_string(i))
                            .x(4.0f).y(iy).size(crumbW - 8.0f, 28.0f)
                            .states(kClear, kSurfaceHover, kSurfaceAct)
                            .radius(6.0f)
                            .onClick([s] {
                                state.pathSuggestions.clear();
                                state.pathEditing = false;
                                goToPath(s);
                            })
                            .build();
                        ui.text("path.suggest.label." + std::to_string(i))
                            .x(14.0f).y(iy).size(crumbW - 28.0f, 28.0f)
                            .text(s)
                            .fontSize(13.0f).lineHeight(13.0f)
                            .fontFamily("")
                            .color(kInk)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .build();
                    }
                })
                .build();
        }
        return;
    }

    const float editW = 32.0f;
    const float segW = crumbW - editW - 6.0f;

    ui.stack("breadcrumb")
        .x(crumbX).y(y).size(crumbW, h)
        .content([&] {
            ui.rect("breadcrumb.bg")
                .size(crumbW, h)
                .color(kSurface)
                .radius(8.0f)
                .border(1.0f, kBorder)
                .onClick([] { state.pathInput = state.currentPath; state.pathSuggestions.clear(); state.pathEditing = true; app::requestFocus("path.edit.input.hit"); })
                .build();

            ui.stack("breadcrumb.segs")
                .x(4.0f).y(0.0f).size(segW, h)
                .clip()
                .content([&] {
                    const std::vector<std::string> parts = core::splitPath(state.currentPath);
                    const float fontSize = 13.0f;
                    const float sepW = 16.0f;
                    float cursorX = 6.0f;

                    auto addSegment = [&](const std::string& id, const std::string& label,
                                          const std::string& path, bool isLast) {
                        const float labelW = approxTextWidth(label, fontSize) + 24.0f;
                        if (isLast) {
                            ui.text(id)
                                .x(cursorX).y(0.0f).size(labelW, h)
                                .text(label)
                                .fontSize(fontSize).lineHeight(fontSize)
                                .color(kInk)
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .build();
                        } else {
                            ui.stack(id)
                                .x(cursorX).y(3.0f).size(labelW, h - 6.0f)
                                .content([&] {
                                    ui.rect(id + ".bg")
                                        .size(labelW, h - 6.0f)
                                        .states(kClear, kSurfaceHover, kSurfaceAct)
                                        .radius(6.0f)
                                        .onClick([path] { goToPath(path); })
                                        .build();
                                    ui.text(id + ".label")
                                        .size(labelW, h - 6.0f)
                                        .text(label)
                                        .fontSize(fontSize).lineHeight(fontSize)
                                        .color(kAccent)
                                        .horizontalAlign(eui::HorizontalAlign::Center)
                                        .verticalAlign(eui::VerticalAlign::Center)
                                        .build();
                                })
                                .build();
                        }
                        cursorX += labelW;
                        if (!isLast) {
                            ui.text(id + ".sep")
                                .x(cursorX).y(0.0f).size(sepW, h)
                                .text(">")
                                .fontSize(11.0f).lineHeight(11.0f)
                                .color(kMuted)
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .build();
                            cursorX += sepW;
                        }
                    };

                    addSegment("breadcrumb.root", "/", "/", parts.empty());

                    std::string prefix;
                    for (std::size_t i = 0; i < parts.size(); ++i) {
                        prefix += "/" + parts[i];
                        addSegment("breadcrumb.crumb." + std::to_string(i), parts[i], prefix,
                                   i == parts.size() - 1);
                    }
                })
                .build();

            ui.rect("breadcrumb.edit.hit")
                .x(crumbW - editW - 4.0f).y(3.0f).size(editW, h - 6.0f)
                .states(kClear, kSurfaceHover, kSurfaceAct)
                .radius(6.0f)
                .onClick([] { state.pathInput = state.currentPath; state.pathSuggestions.clear(); state.pathEditing = true; app::requestFocus("path.edit.input.hit"); })
                .build();
            ui.text("breadcrumb.edit.icon")
                .x(crumbW - editW - 4.0f).y(3.0f).size(editW, h - 6.0f)
                .icon(0xF303)
                .fontSize(13.0f).lineHeight(13.0f)
                .color(kMuted)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
        })
        .build();
}

void composeStatusBar(eui::Ui& ui, float x, float y, float w, float h) {
    std::string left = state.adbPath.empty()
        ? "未找到 adb"
        : (state.selectedDevice.empty() ? "adb 就绪" : state.selectedDevice);
    left += "   下载目录: " + state.downloadDir;

    std::string count;
    if (state.busy) count = "处理中…";
    else if (state.loading) count = "加载中…";
    else count = std::to_string(state.entries.size()) + " 项";

    ui.text("status.left")
        .x(x).y(y).size(w * 0.45f, h)
        .text(left)
        .fontSize(13.0f).lineHeight(13.0f)
        .color(kMuted)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    // Writable status of the current directory.
    if (!state.selectedDevice.empty() && !state.loading && !state.busy) {
        ui.text("status.writable")
            .x(x + w * 0.45f).y(y).size(w * 0.14f, h)
            .text(state.dirWritable ? "可写" : "只读")
            .fontSize(13.0f).lineHeight(13.0f)
            .color(state.dirWritable ? kGreen : kAmber)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .build();
    }

    if (state.busy) {
        const float barW = std::min(w * 0.22f, 260.0f);
        const float barX = x + w - barW - 8.0f;
        const float barY = y + (h - 6.0f) * 0.5f;
        ui.rect("status.progress.bg")
            .x(barX).y(barY).size(barW, 6.0f)
            .color(kSurfaceHover).radius(3.0f).build();
        if (state.progress >= 0.0f) {
            const float p = std::clamp(state.progress, 0.0f, 1.0f);
            ui.rect("status.progress.fill")
                .x(barX).y(barY).size(std::max(2.0f, barW * p), 6.0f)
                .color(kAccent).radius(3.0f).build();
        }
        ui.text("status.progress.label")
            .x(barX - 250.0f).y(y).size(240.0f, h)
            .text(state.progressLabel.empty() ? "处理中…" : state.progressLabel)
            .fontSize(12.0f).lineHeight(12.0f)
            .color(kMuted)
            .horizontalAlign(eui::HorizontalAlign::Right)
            .verticalAlign(eui::VerticalAlign::Center)
            .build();
    } else {
        ui.text("status.right")
            .x(x + w * 0.62f).y(y).size(w * 0.38f, h)
            .text(count)
            .fontSize(13.0f).lineHeight(13.0f)
            .color(kMuted)
            .horizontalAlign(eui::HorizontalAlign::Right)
            .verticalAlign(eui::VerticalAlign::Center)
            .build();
    }
}

void composeToast(eui::Ui& ui, float w, float h) {
    components::toast(ui, "toast")
        .screen(w, h)
        .visible(state.toastVisible)
        .theme(themeTokens())
        .title(state.toastTitle)
        .message(state.toastMessage)
        .icon(0xF05A)
        .duration(4.0f)
        .zIndex(1400)
        .onDismiss([] { state.toastVisible = false; })
        .onAutoDismiss([] { state.toastVisible = false; })
        .build();
}

void composeDeviceMenu(eui::Ui& ui, float w, float h) {
    std::vector<std::string> items;
    for (const Device& d : state.devices) {
        std::string suffix;
        if (d.state == "unauthorized") suffix = "  (未授权)";
        else if (d.state == "offline") suffix = "  (离线)";
        else if (d.state != "device") suffix = "  (" + d.state + ")";
        items.push_back(d.serial + suffix);
    }
    if (items.empty()) items.push_back("（无设备）");
    items.push_back("无线连接");
    items.push_back("应用管理");
    items.push_back("重启 adb 服务");

    components::contextMenu(ui, "device.menu")
        .screen(w, h)
        .open(state.deviceMenuOpen)
        .position(state.deviceMenuX, state.deviceMenuY)
        .theme(themeTokens())
        .items(items)
        .zIndex(1200)
        .onSelect([](int idx) {
            state.deviceMenuOpen = false;
            const int n = static_cast<int>(state.devices.size());
            if (idx >= 0 && idx < n) {
                const Device& d = state.devices[static_cast<std::size_t>(idx)];
                if (d.state != "device") {
                    state.selectedDevice.clear();
                    state.toastTitle = "设备未就绪";
                    state.toastMessage = d.state == "unauthorized"
                        ? "该设备未授权 USB 调试：请在设备屏幕上点“允许”，然后点“重启 adb 服务”重连。"
                        : "该设备当前离线：请检查 USB 连接后重试。";
                    state.toastVisible = true;
                    return;
                }
                state.selectedDevice = d.serial;
                state.currentPath = "/";
                state.pathInput = state.currentPath;
                state.selectedEntry.clear();
                resetListScroll();
                refreshListing(true);
            } else if (idx == n) {
                promptWirelessConnect();
            } else if (idx == n + 1) {
                openAppManage();
            } else if (idx == n + 2) {
                restartAdbServer();
            }
        })
        .onOpenChange([](bool v) { state.deviceMenuOpen = v; })
        .build();
}

void composeRowMenu(eui::Ui& ui, float w, float h) {
    const FsEntry* entry = findSelected();
    const bool isDir = entry != nullptr && entry->isDir;

    std::vector<std::string> items;
    std::vector<std::function<void()>> actions;
    if (isDir) {
        items.push_back("打开"); actions.push_back([] { enterDir(state.selectedEntry); });
    }
    items.push_back("下载"); actions.push_back([] { doPull(); });
    items.push_back("复制"); actions.push_back([] { doCopy(); });
    items.push_back("剪切"); actions.push_back([] { doMove(); });
    if (!state.clipboardPath.empty()) {
        items.push_back("粘贴"); actions.push_back([] { doPaste(); });
    }
    if (!isDir) {
        items.push_back("查看/编辑"); actions.push_back([] { openTextPreview(state.selectedEntry); });
        items.push_back("预览图片"); actions.push_back([] { openImagePreview(state.selectedEntry); });
    }
    items.push_back("复制路径"); actions.push_back([] { copyPathToClipboard(); });
    items.push_back("复制文件名"); actions.push_back([] { copyFileNameToClipboard(); });
    items.push_back("属性"); actions.push_back([] { openFileProperties(); });
    items.push_back("重命名"); actions.push_back([] { promptRename(); });
    items.push_back("删除"); actions.push_back([] { confirmDelete(); });

    components::contextMenu(ui, "row.menu")
        .screen(w, h)
        .open(state.rowMenuOpen)
        .position(state.rowMenuX, state.rowMenuY)
        .theme(themeTokens())
        .items(items)
        .zIndex(1200)
        .onSelect([actions](int idx) {
            state.rowMenuOpen = false;
            if (idx >= 0 && idx < static_cast<int>(actions.size())) actions[static_cast<std::size_t>(idx)]();
        })
        .onOpenChange([](bool v) { state.rowMenuOpen = v; })
        .build();
}

// A compact dropdown-like field with a popup that only exists while open
// (avoids the always-present transparent popup layer that disturbs neighbors).
void composeOptionField(eui::Ui& ui, const std::string& id, float x, float y, float w,
                        const std::vector<std::string>& items, int selected, bool open,
                        std::function<void(int)> onSelect, std::function<void()> onToggle) {
    ui.rect(id + ".field")
        .x(x).y(y).size(w, 36.0f)
        .states(kSurface, kSurfaceHover, kSurfaceAct)
        .radius(8.0f)
        .border(1.0f, kBorder)
        .onClick([onToggle] { onToggle(); })
        .build();
    ui.text(id + ".field.label")
        .x(x + 12.0f).y(y).size(w - 44.0f, 36.0f)
        .text((selected >= 0 && selected < static_cast<int>(items.size())) ? items[selected] : "")
        .fontSize(14.0f).lineHeight(14.0f)
        .color(kInk)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
    ui.text(id + ".field.chevron")
        .x(x + w - 32.0f).y(y).size(24.0f, 36.0f)
        .icon(open ? 0xF077 : 0xF078)
        .fontSize(11.0f).lineHeight(11.0f)
        .color(kAccent)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    if (!open) return;

    const float itemH = 34.0f;
    const float popupH = itemH * static_cast<float>(items.size()) + 8.0f;
    ui.stack(id + ".popup")
        .x(x).y(y + 40.0f).size(w, popupH)
        .content([&] {
            ui.rect(id + ".popup.bg")
                .size(w, popupH)
                .color(kSurface)
                .radius(8.0f)
                .border(1.0f, kBorder)
                .build();
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                const float iy = 4.0f + static_cast<float>(i) * itemH;
                const bool active = (i == selected);
                ui.rect(id + ".popup.item." + std::to_string(i))
                    .x(4.0f).y(iy).size(w - 8.0f, itemH)
                    .states(active ? kAccentSoft : kClear,
                            active ? kAccentSoft : kSurfaceHover,
                            kSurfaceAct)
                    .radius(6.0f)
                    .onClick([onSelect, i] { onSelect(i); })
                    .build();
                ui.text(id + ".popup.label." + std::to_string(i))
                    .x(16.0f).y(iy).size(w - 32.0f, itemH)
                    .text(items[i])
                    .fontSize(14.0f).lineHeight(14.0f)
                    .color(active ? kInk : kMuted)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            }
        })
        .build();
}

void composeBookmarkMenu(eui::Ui& ui, float w, float h) {
    std::vector<std::string> items;
    items.push_back("★ 添加当前路径");
    for (const Bookmark& b : bookmarks) items.push_back(b.name);
    items.push_back("管理快捷路径");

    components::contextMenu(ui, "bookmark.menu")
        .screen(w, h)
        .open(state.bookmarkMenuOpen)
        .position(state.bookmarkMenuX, state.bookmarkMenuY)
        .theme(themeTokens())
        .items(items)
        .zIndex(1200)
        .onSelect([](int idx) {
            state.bookmarkMenuOpen = false;
            const int n = static_cast<int>(bookmarks.size());
            if (idx == 0) {
                addBookmark(state.currentPath, state.currentPath);
            } else if (idx == n + 1) {
                state.bookmarkManageOpen = true;
            } else if (idx >= 1 && idx <= n) {
                goToPath(bookmarks[static_cast<std::size_t>(idx - 1)].path);
            }
        })
        .onOpenChange([](bool v) { state.bookmarkMenuOpen = v; })
        .build();
}

void composeCommandMenu(eui::Ui& ui, float w, float h) {
    std::vector<std::string> items;
    for (const CommandEntry& c : commands) items.push_back(c.name);
    items.push_back("管理命令");

    components::contextMenu(ui, "command.menu")
        .screen(w, h)
        .open(state.commandMenuOpen)
        .position(state.commandMenuX, state.commandMenuY)
        .theme(themeTokens())
        .items(items)
        .zIndex(1200)
        .onSelect([](int idx) {
            state.commandMenuOpen = false;
            const int n = static_cast<int>(commands.size());
            if (idx == n) {
                state.commandManageOpen = true;
            } else if (idx >= 0 && idx < n) {
                runCommandEntry(commands[static_cast<std::size_t>(idx)]);
            }
        })
        .onOpenChange([](bool v) { state.commandMenuOpen = v; })
        .build();
}

void composeBookmarkManageDialog(eui::Ui& ui, float w, float h) {
    components::dialog(ui, "bookmark.manage")
        .screen(w, h)
        .open(state.bookmarkManageOpen)
        .theme(themeTokens())
        .size(460.0f, 420.0f)
        .zIndex(1300)
        .content([&] {
            const float pw = 460.0f, ph = 420.0f;
            ui.text("bookmark.manage.title")
                .x(24.0f).y(16.0f).size(pw - 48.0f, 30.0f)
                .text("管理快捷路径")
                .fontSize(18.0f).lineHeight(18.0f)
                .color(kInk)
                .build();

            if (bookmarks.empty()) {
                ui.text("bookmark.manage.empty")
                    .x(24.0f).y(60.0f).size(pw - 48.0f, 40.0f)
                    .text("暂无快捷路径")
                    .fontSize(14.0f).lineHeight(14.0f)
                    .color(kMuted)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            } else {
                components::scrollView(ui, "bookmark.manage.list")
                    .x(24.0f).y(60.0f).size(pw - 48.0f, ph - 60.0f - 56.0f)
                    .theme(themeTokens())
                    .scrollbarWidth(8.0f).scrollbarGap(2.0f)
                    .contentKey("bookmark.manage." + std::to_string(bookmarks.size()))
                    .content([&](eui::Ui& body, float cw, float) {
                        const float rowH = 40.0f;
                        for (std::size_t i = 0; i < bookmarks.size(); ++i) {
                            const Bookmark& b = bookmarks[i];
                            const std::string id = "bookmark.manage.row." + std::to_string(i);
                            const float y = static_cast<float>(i) * rowH;
                            body.stack(id)
                                .y(y).size(cw, rowH)
                                .content([&] {
                                    body.rect(id + ".bg").size(cw, rowH - 2.0f).color(kSurface).radius(6.0f).build();
                                    body.text(id + ".name").x(10.0f).y(3.0f).size(cw - 70.0f, 18.0f)
                                        .text(b.name).fontSize(13.0f).lineHeight(13.0f).color(kInk).build();
                                    body.text(id + ".path").x(10.0f).y(21.0f).size(cw - 70.0f, 15.0f)
                                        .text(b.path).fontSize(11.0f).lineHeight(11.0f).color(kMuted).build();
                                    body.rect(id + ".del").x(cw - 54.0f).y(7.0f).size(46.0f, 26.0f)
                                        .states(kRose, eui::mixColor(kRose, kWhite, 0.12f), eui::mixColor(kRose, {0.0f, 0.0f, 0.0f, 1.0f}, 0.16f))
                                        .radius(6.0f)
                                        .onClick([i] { removeBookmark(i); })
                                        .build();
                                    body.text(id + ".del.label").x(cw - 54.0f).y(7.0f).size(46.0f, 26.0f)
                                        .text("删除").fontSize(12.0f).lineHeight(12.0f).color(kWhite)
                                        .horizontalAlign(eui::HorizontalAlign::Center)
                                        .verticalAlign(eui::VerticalAlign::Center)
                                        .build();
                                })
                                .build();
                        }
                    })
                    .build();
            }

            toolButton(ui, "bookmark.manage.close", pw - 24.0f - 100.0f, ph - 56.0f, 100.0f, 40.0f,
                       0xF00D, "关闭", false, true, [] { state.bookmarkManageOpen = false; });
        })
        .onOpenChange([](bool v) { state.bookmarkManageOpen = v; })
        .build();
}

void composeCommandManageDialog(eui::Ui& ui, float w, float h) {
    components::dialog(ui, "command.manage")
        .screen(w, h)
        .open(state.commandManageOpen)
        .theme(themeTokens())
        .size(540.0f, 560.0f)
        .zIndex(1300)
        .content([&] {
            const float pw = 540.0f, ph = 560.0f;
            ui.text("command.manage.title")
                .x(24.0f).y(16.0f).size(pw - 160.0f, 30.0f)
                .text("管理命令")
                .fontSize(18.0f).lineHeight(18.0f)
                .color(kInk)
                .build();
            toolButton(ui, "command.manage.close", pw - 24.0f - 100.0f, 12.0f, 100.0f, 36.0f,
                       0xF00D, "关闭", false, true, [] { state.commandManageOpen = false; });

            ui.text("command.manage.name.label").x(24.0f).y(54.0f).size(100.0f, 22.0f)
                .text("名称").fontSize(13.0f).lineHeight(13.0f).color(kMuted).verticalAlign(eui::VerticalAlign::Center).build();
            ui.stack("command.manage.name.wrap").x(24.0f).y(78.0f).size(pw - 48.0f, 34.0f)
                .content([&] {
                    components::input(ui, "command.manage.name")
                        .theme(themeTokens())
                        .size(pw - 48.0f, 34.0f)
                        .fontSize(14.0f)
                        .placeholder("命令名称")
                        .value(state.commandNameInput)
                        .onChange([](const std::string& v) { state.commandNameInput = v; })
                        .build();
                }).build();

            ui.text("command.manage.cmd.label").x(24.0f).y(118.0f).size(100.0f, 22.0f)
                .text("命令").fontSize(13.0f).lineHeight(13.0f).color(kMuted).verticalAlign(eui::VerticalAlign::Center).build();
            ui.stack("command.manage.cmd.wrap").x(24.0f).y(142.0f).size(pw - 48.0f, 34.0f)
                .content([&] {
                    components::input(ui, "command.manage.cmd")
                        .theme(themeTokens())
                        .size(pw - 48.0f, 34.0f)
                        .fontSize(14.0f)
                        .placeholder("命令内容，如 ls -la 或 ipconfig")
                        .value(state.commandCmdInput)
                        .onChange([](const std::string& v) { state.commandCmdInput = v; })
                        .build();
                }).build();

            ui.text("command.manage.type.label").x(24.0f).y(182.0f).size(100.0f, 30.0f)
                .text("类型").fontSize(13.0f).lineHeight(13.0f).color(kMuted).verticalAlign(eui::VerticalAlign::Center).build();
            ui.stack("command.manage.type.wrap").x(110.0f).y(180.0f).size(200.0f, 32.0f)
                .content([&] {
                    components::segmented(ui, "command.manage.type")
                        .theme(themeTokens())
                        .size(200.0f, 32.0f)
                        .items({"adb shell", "cmd"})
                        .selected(state.commandShellType ? 0 : 1)
                        .fontSize(13.0f)
                        .onChange([](int idx) { state.commandShellType = (idx == 0); })
                        .build();
                }).build();
            toolButton(ui, "command.manage.add", pw - 24.0f - 100.0f, 180.0f, 100.0f, 32.0f,
                       0xF067, "添加", true, true, [] {
                           const std::string name = core::trim(state.commandNameInput);
                           const std::string cmd = core::trim(state.commandCmdInput);
                           if (!name.empty() && !cmd.empty()) {
                               addCommand(name, state.commandShellType, cmd);
                               state.commandNameInput.clear();
                               state.commandCmdInput.clear();
                           }
                       });

            ui.text("command.manage.list.label").x(24.0f).y(226.0f).size(200.0f, 24.0f)
                .text("已有命令").fontSize(13.0f).lineHeight(13.0f).color(kMuted).build();

            if (commands.empty()) {
                ui.text("command.manage.empty")
                    .x(24.0f).y(252.0f).size(pw - 48.0f, 40.0f)
                    .text("暂无命令")
                    .fontSize(14.0f).lineHeight(14.0f).color(kMuted)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            } else {
                components::scrollView(ui, "command.manage.list")
                    .x(24.0f).y(252.0f).size(pw - 48.0f, ph - 252.0f - 16.0f)
                    .theme(themeTokens())
                    .scrollbarWidth(8.0f).scrollbarGap(2.0f)
                    .contentKey("command.manage." + std::to_string(commands.size()))
                    .content([&](eui::Ui& body, float cw, float) {
                        const float rowH = 38.0f;
                        for (std::size_t i = 0; i < commands.size(); ++i) {
                            const CommandEntry& c = commands[i];
                            const std::string id = "command.manage.row." + std::to_string(i);
                            const float y = static_cast<float>(i) * rowH;
                            body.stack(id)
                                .y(y).size(cw, rowH)
                                .content([&] {
                                    body.rect(id + ".bg").size(cw, rowH - 2.0f).color(kSurface).radius(6.0f).build();
                                    body.text(id + ".name").x(10.0f).y(2.0f).size(cw - 170.0f, 18.0f)
                                        .text(c.name).fontSize(13.0f).lineHeight(13.0f).color(kInk).build();
                                    body.text(id + ".cmd").x(10.0f).y(21.0f).size(cw - 170.0f, 14.0f)
                                        .text(core::shorten(c.command, 50)).fontSize(11.0f).lineHeight(11.0f).color(kMuted).build();
                                    body.text(id + ".type").x(cw - 160.0f).y(9.0f).size(100.0f, 18.0f)
                                        .text(c.shell ? "adb shell" : "cmd").fontSize(11.0f).lineHeight(11.0f)
                                        .color(c.shell ? kAccent : kAmber).horizontalAlign(eui::HorizontalAlign::Center).build();
                                    body.rect(id + ".del").x(cw - 54.0f).y(6.0f).size(46.0f, 26.0f)
                                        .states(kRose, eui::mixColor(kRose, kWhite, 0.12f), eui::mixColor(kRose, {0.0f, 0.0f, 0.0f, 1.0f}, 0.16f))
                                        .radius(6.0f)
                                        .onClick([i] { removeCommand(i); })
                                        .build();
                                    body.text(id + ".del.label").x(cw - 54.0f).y(6.0f).size(46.0f, 26.0f)
                                        .text("删除").fontSize(12.0f).lineHeight(12.0f).color(kWhite)
                                        .horizontalAlign(eui::HorizontalAlign::Center)
                                        .verticalAlign(eui::VerticalAlign::Center)
                                        .build();
                                })
                                .build();
                        }
                    })
                    .build();
            }
        })
        .onOpenChange([](bool v) { state.commandManageOpen = v; })
        .build();
}

void composeCommandOutputDialog(eui::Ui& ui, float w, float h) {
    components::dialog(ui, "command.output")
        .screen(w, h)
        .open(state.commandOutputOpen)
        .theme(themeTokens())
        .size(580.0f, 460.0f)
        .zIndex(1300)
        .content([&] {
            const float pw = 580.0f, ph = 460.0f;
            ui.text("command.output.title")
                .x(24.0f).y(16.0f).size(pw - 48.0f, 28.0f)
                .text(state.commandOutputTitle)
                .fontSize(17.0f).lineHeight(17.0f)
                .color(kInk)
                .build();

            components::scrollView(ui, "command.output.list")
                .x(24.0f).y(52.0f).size(pw - 48.0f, ph - 52.0f - 56.0f)
                .theme(themeTokens())
                .scrollbarWidth(8.0f).scrollbarGap(2.0f)
                .contentKey("command.output." + std::to_string(state.commandOutputText.size()))
                .content([&](eui::Ui& body, float cw, float) {
                    std::istringstream iss(state.commandOutputText);
                    std::string line;
                    int i = 0;
                    while (std::getline(iss, line) && i < 1000) {
                        const float y = static_cast<float>(i) * 20.0f;
                        body.text("command.output.line." + std::to_string(i))
                            .x(4.0f).y(y).size(cw - 8.0f, 20.0f)
                            .text(line)
                            .fontSize(12.0f).lineHeight(12.0f)
                            .color(kInk)
                            .build();
                        ++i;
                    }
                })
                .build();

            toolButton(ui, "command.output.close", pw - 24.0f - 100.0f, ph - 56.0f, 100.0f, 40.0f,
                       0xF00D, "关闭", false, true, [] { state.commandOutputOpen = false; });
        })
        .onOpenChange([](bool v) { state.commandOutputOpen = v; })
        .build();
}

void composeSettingsDialog(eui::Ui& ui, float w, float h) {
    // Responsive panel size so the settings stay fully visible on small windows.
    const float pw = std::min(560.0f, std::max(360.0f, w - 48.0f));
    const float ph = std::min(520.0f, std::max(380.0f, h - 48.0f));

    components::dialog(ui, "settings.dialog")
        .screen(w, h)
        .open(state.settingsOpen)
        .theme(themeTokens())
        .size(pw, ph)
        .zIndex(1300)
        .content([&] {
            // Title + close
            ui.text("settings.title")
                .x(24.0f).y(16.0f).size(pw - 160.0f, 30.0f)
                .text("设置")
                .fontSize(19.0f).lineHeight(19.0f)
                .color(kInk)
                .build();
            toolButton(ui, "settings.close", pw - 24.0f - 100.0f, 12.0f, 100.0f, 36.0f,
                       0xF00D, "关闭", false, true, [] { state.settingsOpen = false; });

            // Theme row: dark mode toggle + accent color swatches.
            ui.stack("settings.dark.wrap")
                .x(24.0f).y(52.0f)
                .content([&] {
                    components::toggleSwitch(ui, "settings.dark")
                        .theme(themeTokens())
                        .size(150.0f, 28.0f)
                        .trackSize(40.0f, 22.0f)
                        .fontSize(13.0f)
                        .text("深色模式")
                        .checked(settings.darkMode)
                        .onChange([](bool v) { setDarkMode(v); })
                        .build();
                })
                .build();

            ui.text("settings.accent.label")
                .x(200.0f).y(52.0f).size(60.0f, 28.0f)
                .text("主题色")
                .fontSize(13.0f).lineHeight(13.0f)
                .color(kMuted)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            for (int i = 0; i < 6; ++i) {
                const AccentPreset& p = kAccentPresets[i];
                const float sx = 262.0f + static_cast<float>(i) * 30.0f;
                const bool active = std::fabs(settings.accentR - p.r) < 0.01f &&
                                    std::fabs(settings.accentG - p.g) < 0.01f &&
                                    std::fabs(settings.accentB - p.b) < 0.01f;
                ui.rect("settings.accent." + std::to_string(i))
                    .x(sx).y(52.0f).size(24.0f, 24.0f)
                    .color(eui::Color{p.r, p.g, p.b, 1.0f})
                    .radius(12.0f)
                    .border(active ? 2.0f : 0.0f, active ? kInk : kClear)
                    .onClick([r = p.r, g = p.g, b = p.b] { setAccentColor(r, g, b); })
                    .build();
            }

            // Font label + scrollable font list (composed first so the dropdown
            // popups below render on top of it).
            ui.text("settings.font.label")
                .x(24.0f).y(160.0f).size(200.0f, 24.0f)
                .text("字体")
                .fontSize(13.0f).lineHeight(13.0f)
                .color(kMuted)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            const float fontListH = std::max(60.0f, ph - 188.0f - 16.0f);
            if (state.fontList.empty()) {
                ui.text("settings.font.loading")
                    .x(24.0f).y(188.0f).size(pw - 48.0f, fontListH)
                    .text("正在加载字体…")
                    .fontSize(14.0f).lineHeight(14.0f)
                    .color(kMuted)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            } else {
                ui.stack("settings.font.wrap")
                    .x(24.0f).y(188.0f).size(pw - 48.0f, fontListH)
                    .content([&] {
                        ui.rect("settings.font.wrap.bg")
                            .size(pw - 48.0f, fontListH)
                            .color(kBackground)
                            .radius(8.0f)
                            .border(1.0f, kBorder)
                            .build();
                        components::scrollView(ui, "settings.font.list")
                            .x(4.0f).y(4.0f).size(pw - 56.0f, fontListH - 8.0f)
                            .theme(themeTokens())
                            .scrollbarWidth(8.0f)
                            .scrollbarGap(2.0f)
                            .contentKey("settings.fonts." + std::to_string(state.fontList.size()))
                            .content([&](eui::Ui& body, float cw, float) {
                                const float rowH = 30.0f;
                                for (std::size_t i = 0; i < state.fontList.size(); ++i) {
                                    const std::string& name = state.fontList[i];
                                    const bool selected = (name == settings.fontFamily);
                                    const std::string id = "settings.font.row." + std::to_string(i);
                                    const float y = static_cast<float>(i) * rowH;
                                    body.stack(id)
                                        .y(y).size(cw, rowH)
                                        .content([&] {
                                            body.rect(id + ".bg")
                                                .size(cw, rowH - 2.0f)
                                                .states(selected ? kAccentSoft : kClear,
                                                        selected ? kAccentSoft : kSurfaceHover,
                                                        kSurfaceAct)
                                                .radius(6.0f)
                                                .onClick([name] { selectFontFamily(name); })
                                                .build();
                                            body.text(id + ".label")
                                                .x(10.0f).y(0.0f).size(cw - 20.0f, rowH)
                                                .text(name)
                                                .fontSize(14.0f).lineHeight(14.0f)
                                                .color(selected ? kInk : kMuted)
                                                .verticalAlign(eui::VerticalAlign::Center)
                                                .build();
                                        })
                                        .build();
                                }
                            })
                            .build();
                    })
                    .build();
            }

            // Weight + font size + scale dropdowns (side by side).
            const float labelW = 60.0f;
            const float rowGap = 10.0f;
            const float labelGap = 6.0f;
            const float dropW = (pw - 24.0f * 2.0f - (labelW + labelGap) * 3.0f - rowGap * 2.0f) / 3.0f;
            const float groupW = labelW + labelGap + dropW + rowGap;

            ui.text("settings.weight.label")
                .x(24.0f).y(112.0f).size(labelW, 36.0f)
                .text("字重")
                .fontSize(13.0f).lineHeight(13.0f)
                .color(kMuted)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            composeOptionField(ui, "settings.weight",
                               24.0f + labelW + labelGap, 108.0f, dropW,
                               {"细体", "常规", "粗体"},
                               settings.fontWeight <= 300 ? 0 : (settings.fontWeight >= 700 ? 2 : 1),
                               state.weightDropdownOpen,
                               [](int idx) {
                                   selectFontWeight(idx == 0 ? 300 : (idx == 2 ? 700 : 400));
                                   state.weightDropdownOpen = false;
                               },
                               [] { state.weightDropdownOpen = !state.weightDropdownOpen; });

            const float fsX = 24.0f + groupW;
            ui.text("settings.fontsize.label")
                .x(fsX).y(112.0f).size(labelW, 36.0f)
                .text("字号")
                .fontSize(13.0f).lineHeight(13.0f)
                .color(kMuted)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            composeOptionField(ui, "settings.fontsize",
                               fsX + labelW + labelGap, 108.0f, dropW,
                               {"小", "默认", "大", "特大"},
                               settings.fontSizeScale <= 0.95f ? 0 :
                               (settings.fontSizeScale >= 1.25f ? 3 :
                                (settings.fontSizeScale >= 1.12f ? 2 : 1)),
                               state.fontSizeDropdownOpen,
                               [](int idx) {
                                   selectFontScale(idx == 0 ? 0.9f : (idx == 2 ? 1.15f : (idx == 3 ? 1.3f : 1.0f)));
                                   state.fontSizeDropdownOpen = false;
                               },
                               [] { state.fontSizeDropdownOpen = !state.fontSizeDropdownOpen; });

            const float scaleX = 24.0f + groupW * 2.0f;
            ui.text("settings.scale.label")
                .x(scaleX).y(112.0f).size(labelW, 36.0f)
                .text("界面大小")
                .fontSize(13.0f).lineHeight(13.0f)
                .color(kMuted)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            composeOptionField(ui, "settings.scale",
                               scaleX + labelW + labelGap, 108.0f, dropW,
                               {"80%", "100%", "125%", "150%"},
                               settings.uiScale <= 0.85f ? 0 :
                               (settings.uiScale >= 1.4f ? 3 :
                                (settings.uiScale >= 1.15f ? 2 : 1)),
                               state.scaleDropdownOpen,
                               [](int idx) {
                                   selectUiScale(idx == 0 ? 0.8f : (idx == 2 ? 1.25f : (idx == 3 ? 1.5f : 1.0f)));
                                   state.scaleDropdownOpen = false;
                               },
                               [] { state.scaleDropdownOpen = !state.scaleDropdownOpen; });
        })
        .onOpenChange([](bool v) { state.settingsOpen = v; })
        .build();
}

void composeUpdateDialog(eui::Ui& ui, float w, float h) {
    const float pw = std::min(560.0f, std::max(360.0f, w - 48.0f));
    const float ph = 440.0f;

    const std::string adbCur = g_update.adbCurrent.empty() ? "未安装" : g_update.adbCurrent;
    const std::string adbLat = g_update.adbLatest.empty() ? "未知" : g_update.adbLatest;
    const std::string scCur = g_update.scrcpyCurrent.empty() ? "未安装" : g_update.scrcpyCurrent;
    const std::string scLat = g_update.scrcpyLatest.empty() ? "未知" : g_update.scrcpyLatest;
    const std::string appCur = g_update.appCurrent.empty() ? kAppVersion : g_update.appCurrent;
    const std::string appLat = g_update.appLatest.empty() ? "未知" : g_update.appLatest;

    const bool adbHas = g_update.adbUpdate && !g_update.adbLatest.empty();
    const bool scHas = g_update.scrcpyUpdate && !g_update.scrcpyLatest.empty();
    // A newer release only counts as updatable when it actually carries the
    // portable ZIP. A release with a tag but no matching asset (someone forgot
    // to upload it, or only published the installer) used to leave this button
    // enabled; clicking it failed with "未获取到本软件的下载地址".
    const bool appHas = g_update.appUpdate && !g_update.appLatest.empty() && !g_update.appUrl.empty();
    const std::string adbBtn = g_update.adbCurrent.empty() && !g_update.adbLatest.empty()
                                   ? "安装 " + g_update.adbLatest
                                   : (adbHas ? "更新到 " + g_update.adbLatest
                                             : (!g_update.adbLatest.empty() ? "已是最新" : "无更新信息"));
    const std::string scBtn = g_update.scrcpyCurrent.empty() && !g_update.scrcpyLatest.empty()
                                  ? "安装 " + g_update.scrcpyLatest
                                  : (scHas ? "更新到 " + g_update.scrcpyLatest
                                           : (!g_update.scrcpyLatest.empty() ? "已是最新" : "无更新信息"));
    const std::string appBtn = appHas ? ("更新到 " + g_update.appLatest)
                                      : (g_update.appLatest.empty()
                                             ? "无更新信息"
                                             : (g_update.appUrl.empty() && g_update.appUpdate
                                                    ? "新版本缺少下载包"
                                                    : "已是最新"));
    const bool busy = state.updateChecking || state.updateWorking;

    components::dialog(ui, "update.dialog")
        .screen(w, h)
        .open(state.updateOpen)
        .theme(themeTokens())
        .size(pw, ph)
        .zIndex(1300)
        .content([&] {
            ui.text("update.title")
                .x(24.0f).y(16.0f).size(pw - 160.0f, 30.0f)
                .text("检查更新")
                .fontSize(19.0f).lineHeight(19.0f)
                .color(kInk)
                .build();
            toolButton(ui, "update.close", pw - 24.0f - 100.0f, 12.0f, 100.0f, 36.0f,
                       0xF00D, "关闭", false, true, [] { state.updateOpen = false; });

            // --- This app section ---
            ui.text("update.app.title")
                .x(24.0f).y(52.0f).size(200.0f, 24.0f)
                .text("本软件")
                .fontSize(15.0f).lineHeight(15.0f)
                .color(kInk)
                .build();
            ui.text("update.app.cur")
                .x(24.0f).y(80.0f).size(pw - 48.0f, 20.0f)
                .text("当前版本：" + appCur)
                .fontSize(13.0f).lineHeight(13.0f)
                .color(kMuted)
                .build();
            ui.text("update.app.lat")
                .x(24.0f).y(104.0f).size(pw - 48.0f, 20.0f)
                .text("最新版本：" + appLat)
                .fontSize(13.0f).lineHeight(13.0f)
                .color(appHas ? kAccent : kMuted)
                .build();
            toolButton(ui, "update.app.btn", pw - 24.0f - 132.0f, 84.0f, 132.0f, 36.0f,
                       0xF019, appBtn, appHas, appHas && !busy, [version = g_update.appLatest] {
                showConfirm("更新本软件",
                            "确定要更新到 " + version + " 吗？\n更新完成后需要重启程序才能生效。",
                            "更新", [] { startUpdateApp(); });
            });

            // --- ADB section ---
            ui.text("update.adb.title")
                .x(24.0f).y(152.0f).size(200.0f, 24.0f)
                .text("ADB（Android 调试桥）")
                .fontSize(15.0f).lineHeight(15.0f)
                .color(kInk)
                .build();
            ui.text("update.adb.cur")
                .x(24.0f).y(180.0f).size(pw - 48.0f, 20.0f)
                .text("当前版本：" + adbCur)
                .fontSize(13.0f).lineHeight(13.0f)
                .color(kMuted)
                .build();
            ui.text("update.adb.lat")
                .x(24.0f).y(204.0f).size(pw - 48.0f, 20.0f)
                .text("最新版本：" + adbLat)
                .fontSize(13.0f).lineHeight(13.0f)
                .color(adbHas ? kAccent : kMuted)
                .build();
            toolButton(ui, "update.adb.btn", pw - 24.0f - 132.0f, 184.0f, 132.0f, 36.0f,
                       0xF019, adbBtn, adbHas, adbHas && !busy, [version = g_update.adbLatest] {
                showConfirm("更新 adb",
                            "确定要更新 adb 到 " + version + " 吗？\n更新会替换当前的 adb 程序文件。",
                            "更新", [] { startUpdateAdb(); });
            });

            // --- scrcpy section ---
            ui.text("update.scrcpy.title")
                .x(24.0f).y(252.0f).size(200.0f, 24.0f)
                .text("scrcpy（投屏）")
                .fontSize(15.0f).lineHeight(15.0f)
                .color(kInk)
                .build();
            ui.text("update.scrcpy.cur")
                .x(24.0f).y(280.0f).size(pw - 48.0f, 20.0f)
                .text("当前版本：" + scCur)
                .fontSize(13.0f).lineHeight(13.0f)
                .color(kMuted)
                .build();
            ui.text("update.scrcpy.lat")
                .x(24.0f).y(304.0f).size(pw - 48.0f, 20.0f)
                .text("最新版本：" + scLat)
                .fontSize(13.0f).lineHeight(13.0f)
                .color(scHas ? kAccent : kMuted)
                .build();
            toolButton(ui, "update.scrcpy.btn", pw - 24.0f - 132.0f, 284.0f, 132.0f, 36.0f,
                       0xF019, scBtn, scHas, scHas && !busy, [version = g_update.scrcpyLatest] {
                showConfirm("更新 scrcpy",
                            "确定要更新 scrcpy 到 " + version + " 吗？\n更新会替换当前的 scrcpy 程序文件。",
                            "更新", [] { startUpdateScrcpy(); });
            });

            // --- Bottom: re-check + status + progress ---
            toolButton(ui, "update.recheck", 24.0f, 352.0f, 112.0f, 36.0f,
                       0xF021, "检查更新", false, !busy, [] { checkForUpdates(); });

            ui.text("update.status")
                .x(150.0f).y(352.0f).size(pw - 174.0f, 36.0f)
                .text(state.updateStatus.empty() ? "点击“检查更新”查看可用的新版本。" : state.updateStatus)
                .fontSize(12.0f).lineHeight(12.0f)
                .color(kMuted)
                .verticalAlign(eui::VerticalAlign::Center)
                .wrap(true)
                .maxWidth(pw - 174.0f)
                .build();

            if (state.updateWorking) {
                const long long total = g_dlTotal.load();
                const long long got = g_dlReceived.load();
                float frac = total > 0 ? static_cast<float>(static_cast<double>(got) / static_cast<double>(total)) : 0.0f;
                frac = std::max(0.0f, std::min(1.0f, frac));
                ui.rect("update.progress.bg")
                    .x(24.0f).y(398.0f).size(pw - 48.0f, 8.0f)
                    .color(kBorder)
                    .radius(4.0f)
                    .build();
                ui.rect("update.progress.fill")
                    .x(24.0f).y(398.0f).size((pw - 48.0f) * frac, 8.0f)
                    .color(kAccent)
                    .radius(4.0f)
                    .build();
            }
        })
        .onOpenChange([](bool v) { state.updateOpen = v; })
        .build();
}

// -----------------------------------------------------------------------------
// First-run setup.
//
// adb and scrcpy are deliberately not shipped with the app (unsigned third-party
// binaries inside the archive are what got the whole download flagged by
// antivirus), and the app is useless without adb, so a fresh install has to say
// so instead of leaving the user to discover "check for updates" on their own.
//
// Shown once at startup when either component is missing, and never again once
// the user ticks "do not show this again".
// -----------------------------------------------------------------------------
void closeSetupDialog() {
    state.setupOpen = false;
    if (state.setupDontAsk != state.setupPromptDismissed) {
        state.setupPromptDismissed = state.setupDontAsk;
        saveSettings();
    }
}

// The row status: whether the component is usable, and its version when known.
std::string toolRowStatus(bool present, const std::string& version, const std::string& ready) {
    if (!present) return "未安装";
    if (version.empty()) return ready;
    return ready + "（" + version + "）";
}

void composeSetupDialog(eui::Ui& ui, float w, float h) {
    // The dialog asks for a network download, so it must not claim to work when
    // the check has not returned yet: the buttons stay disabled until the URLs
    // (and versions) are known.
    const bool adbPresent = state.adbFound;
    const std::string adbVersion = g_update.adbCurrent.empty() ? state.adbVersion : g_update.adbCurrent;
    const bool scrcpyPresent = !findScrcpy().empty();
    const std::string scrcpyVersion = g_update.scrcpyCurrent;

    const bool adbDownloadable = !adbPresent && !g_update.adbUrl.empty() && !g_update.adbLatest.empty();
    const bool scrcpyDownloadable = !scrcpyPresent && !g_update.scrcpyUrl.empty() && !g_update.scrcpyLatest.empty();
    const bool anyMissing = !adbPresent || !scrcpyPresent;
    const bool busy = state.updateChecking || state.updateWorking;

    const float pw = std::min(560.0f, std::max(360.0f, w - 48.0f));
    const float ph = 372.0f;
    const float rowX = 24.0f;
    const float rowW = pw - 48.0f;
    const float btnW = 132.0f;

    components::dialog(ui, "setup.dialog")
        .screen(w, h)
        .open(state.setupOpen)
        .theme(themeTokens())
        .size(pw, ph)
        .zIndex(1400)
        .content([&] {
            ui.text("setup.title")
                .x(rowX).y(16.0f).size(pw - 48.0f, 30.0f)
                .text("首次使用设置")
                .fontSize(19.0f).lineHeight(19.0f)
                .color(kInk)
                .build();
            ui.text("setup.intro")
                .x(rowX).y(50.0f).size(rowW, 44.0f)
                .text("adb 和 scrcpy 不再随安装包提供（无签名的第三方程序容易被杀毒软件误报）。"
                      "可以在这里一键下载，程序会校验官方公布的校验值。")
                .fontSize(12.5f).lineHeight(16.0f)
                .color(kMuted)
                .wrap(true).maxWidth(rowW)
                .build();

            // --- adb ---
            ui.text("setup.adb.title")
                .x(rowX).y(106.0f).size(200.0f, 24.0f)
                .text("adb（Android 调试桥）")
                .fontSize(15.0f).lineHeight(15.0f)
                .color(kInk)
                .build();
            ui.text("setup.adb.state")
                .x(rowX).y(130.0f).size(rowW - btnW - 16.0f, 20.0f)
                .text(adbPresent ? toolRowStatus(true, adbVersion, "已就绪，可以连接手机了")
                                 : (g_update.adbLatest.empty() ? "未安装（正在获取版本信息…）"
                                                               : "未安装，最新版 " + g_update.adbLatest))
                .fontSize(12.5f).lineHeight(12.5f)
                .color(adbPresent ? kMuted : kAccent)
                .build();
            toolButton(ui, "setup.adb.btn", rowX + rowW - btnW, 120.0f, btnW, 36.0f,
                       0xF019, adbPresent ? "已安装" : (adbDownloadable ? "下载并安装" : "获取中…"),
                       !adbPresent, adbDownloadable && !busy, [] {
                           if (ensureToolsDownloadable(PendingInstall::adb)) {
                               toast("正在下载 adb", "下载完成后会自动放到程序目录并启用。");
                           }
                       });

            // --- scrcpy ---
            ui.text("setup.scrcpy.title")
                .x(rowX).y(182.0f).size(200.0f, 24.0f)
                .text("scrcpy（投屏）")
                .fontSize(15.0f).lineHeight(15.0f)
                .color(kInk)
                .build();
            ui.text("setup.scrcpy.state")
                .x(rowX).y(206.0f).size(rowW - btnW - 16.0f, 20.0f)
                .text(scrcpyPresent ? toolRowStatus(true, scrcpyVersion, "已就绪，可以投屏了")
                                    : (g_update.scrcpyLatest.empty() ? "未安装（正在获取版本信息…）"
                                                                     : "未安装，最新版 " + g_update.scrcpyLatest))
                .fontSize(12.5f).lineHeight(12.5f)
                .color(scrcpyPresent ? kMuted : kAccent)
                .build();
            toolButton(ui, "setup.scrcpy.btn", rowX + rowW - btnW, 196.0f, btnW, 36.0f,
                       0xF019, scrcpyPresent ? "已安装" : (scrcpyDownloadable ? "下载并安装" : "获取中…"),
                       !scrcpyPresent, scrcpyDownloadable && !busy, [] {
                           if (ensureToolsDownloadable(PendingInstall::scrcpy)) {
                               toast("正在下载 scrcpy", "下载完成后即可在工具栏点「投屏」。");
                           }
                       });

            ui.text("setup.note")
                .x(rowX).y(248.0f).size(rowW, 16.0f)
                .text("也可以点「稍后再说」，之后在标题栏最左侧的更新按钮里单独安装。")
                .fontSize(11.5f).lineHeight(11.5f)
                .color(kMuted)
                .build();

            // --- bottom: install everything / dismiss + do-not-ask ---
            toolButton(ui, "setup.all", rowX, 274.0f, 150.0f, 36.0f,
                       0xF019, anyMissing ? "全部下载" : "都装好了",
                       true, anyMissing && !busy, [] {
                           const bool needAdb = !state.adbFound;
                           const bool needScrcpy = findScrcpy().empty();
                           if (!needAdb && !needScrcpy) return;
                           g_pendingAdbInstall = needAdb;
                           g_pendingScrcpyInstall = needScrcpy;
                           if (!advanceInstallQueue() && !state.updateChecking) {
                               checkForUpdates();
                           }
                       });
            toolButton(ui, "setup.close", rowX + 158.0f, 274.0f, 110.0f, 36.0f,
                       0xF00D, anyMissing ? "稍后再说" : "关闭", false, true, [] { closeSetupDialog(); });

            ui.stack("setup.dontask.wrap")
                .x(rowX + 286.0f).y(274.0f).size(rowW - 286.0f, 36.0f)
                .content([&] {
                    components::checkbox(ui, "setup.dontask")
                        .size(rowW - 286.0f, 36.0f)
                        .text("不再提示")
                        .fontSize(12.5f)
                        .theme(themeTokens())
                        .checked(state.setupDontAsk)
                        .onChange([](bool v) { state.setupDontAsk = v; })
                        .build();
                })
                .build();

            ui.text("setup.status")
                .x(rowX).y(316.0f).size(rowW, 16.0f)
                .text(busy ? (state.updateStatus.empty() ? "正在处理…" : state.updateStatus)
                           : (anyMissing ? "缺少的组件下载完成后会立刻可用，不用重启。"
                                         : "adb 与 scrcpy 都已就绪。"))
                .fontSize(11.5f).lineHeight(11.5f)
                .color(kMuted)
                .build();

            if (state.updateWorking) {
                const long long total = g_dlTotal.load();
                const long long got = g_dlReceived.load();
                float frac = total > 0 ? static_cast<float>(static_cast<double>(got) / static_cast<double>(total)) : 0.0f;
                frac = std::max(0.0f, std::min(1.0f, frac));
                ui.rect("setup.progress.bg")
                    .x(rowX).y(340.0f).size(rowW, 8.0f)
                    .color(kBorder)
                    .radius(4.0f)
                    .build();
                ui.rect("setup.progress.fill")
                    .x(rowX).y(340.0f).size(rowW * frac, 8.0f)
                    .color(kAccent)
                    .radius(4.0f)
                    .build();
            }
        })
        .onOpenChange([](bool v) {
            if (!v) {
                closeSetupDialog();
            } else {
                state.setupOpen = true;
            }
        })
        .build();
}

void composeConfirmDialog(eui::Ui& ui, float w, float h) {
    components::dialog(ui, "confirm.dialog")
        .screen(w, h)
        .open(state.confirmDialogOpen)
        .theme(themeTokens())
        .title(state.confirmTitle)
        .message(state.confirmMessage)
        .primaryText(state.confirmPrimaryText)
        .secondaryText("取消")
        .zIndex(1500)
        .onPrimary([] {
            state.confirmDialogOpen = false;
            if (state.confirmAction) state.confirmAction();
        })
        .onOpenChange([](bool v) { state.confirmDialogOpen = v; })
        .build();
}

void composePromptDialog(eui::Ui& ui, float w, float h) {
    components::dialog(ui, "prompt.dialog")
        .screen(w, h)
        .open(state.promptOpen)
        .theme(themeTokens())
        .size(460.0f, 200.0f)
        .zIndex(1300)
        .content([&] {
            ui.text("prompt.title")
                .x(24.0f).y(18.0f).size(412.0f, 26.0f)
                .text(state.promptTitle)
                .fontSize(18.0f)
                .lineHeight(18.0f)
                .color(kInk)
                .build();

            ui.stack("prompt.input.wrap")
                .x(24.0f).y(58.0f).size(412.0f, 40.0f)
                .content([&] {
                    components::input(ui, "prompt.input")
                        .theme(themeTokens())
                        .size(412.0f, 40.0f)
                        .fontSize(15.0f)
                        .fontFamily("")
                        .placeholder("名称")
                        .value(state.promptValue)
                        .onChange([](const std::string& v) { state.promptValue = v; })
                        .onEnter([] { confirmPrompt(); })
                        .build();
                })
                .build();

            toolButton(ui, "prompt.cancel", 24.0f, 140.0f, 120.0f, 40.0f, 0xF00D, "取消",
                       false, true, [] { state.promptOpen = false; });
            toolButton(ui, "prompt.ok", 460.0f - 24.0f - 120.0f, 140.0f, 120.0f, 40.0f,
                       0xF00C, "确定", true, true, [] { confirmPrompt(); });
        })
        .onOpenChange([](bool v) { state.promptOpen = v; })
        .build();
}

} // anonymous namespace

// =============================================================================
// DSL app entry points
// =============================================================================
void composeDeviceInfoDialog(eui::Ui& ui, float w, float h) {
    components::dialog(ui, "device.info")
        .screen(w, h)
        .open(state.deviceInfoOpen)
        .theme(themeTokens())
        .size(520.0f, 420.0f)
        .zIndex(1300)
        .content([&] {
            const float pw = 520.0f, ph = 420.0f;
            ui.text("device.info.title")
                .x(24.0f).y(16.0f).size(pw - 48.0f, 28.0f)
                .text("设备信息")
                .fontSize(17.0f).lineHeight(17.0f)
                .color(kInk).build();
            components::scrollView(ui, "device.info.list")
                .x(24.0f).y(52.0f).size(pw - 48.0f, ph - 52.0f - 56.0f)
                .theme(themeTokens()).scrollbarWidth(8.0f).scrollbarGap(2.0f)
                .contentKey("device.info." + std::to_string(state.deviceInfoText.size()))
                .content([&](eui::Ui& body, float cw, float) {
                    std::istringstream iss(state.deviceInfoText);
                    std::string line;
                    int i = 0;
                    while (std::getline(iss, line) && i < 200) {
                        body.text("device.info.line." + std::to_string(i))
                            .x(4.0f).y(static_cast<float>(i) * 20.0f).size(cw - 8.0f, 20.0f)
                            .text(line).fontSize(12.0f).lineHeight(12.0f).color(kInk).build();
                        ++i;
                    }
                })
                .build();
            toolButton(ui, "device.info.close", pw - 24.0f - 100.0f, ph - 56.0f, 100.0f, 40.0f,
                       0xF00D, "关闭", false, true, [] { state.deviceInfoOpen = false; });
        })
        .onOpenChange([](bool v) { state.deviceInfoOpen = v; })
        .build();
}

void composeLogcatDialog(eui::Ui& ui, float w, float h) {
    components::dialog(ui, "logcat.dialog")
        .screen(w, h)
        .open(state.logcatOpen)
        .theme(themeTokens())
        .size(760.0f, 560.0f)
        .zIndex(1300)
        .content([&] {
            const float pw = 760.0f, ph = 560.0f;
            const bool streaming = g_logcatRunning.load();

            ui.text("logcat.title")
                .x(24.0f).y(16.0f).size(160.0f, 28.0f)
                .text("logcat（实时）")
                .fontSize(16.0f).lineHeight(16.0f).color(kInk).build();
            ui.text("logcat.status")
                .x(150.0f).y(16.0f).size(120.0f, 28.0f)
                .text(streaming ? "\xE2\x97\x8F 运行中" : "\xE2\x97\x8B 已停止")
                .fontSize(13.0f).lineHeight(13.0f)
                .color(streaming ? kGreen : kMuted)
                .verticalAlign(eui::VerticalAlign::Center).build();
            toolButton(ui, "logcat.close", pw - 24.0f - 100.0f, 12.0f, 100.0f, 36.0f,
                       0xF00D, "关闭", false, true, [] { state.logcatOpen = false; });

            // Filter (case-insensitive regex).
            const float actionW = 104.0f;
            const float filterW = pw - 24.0f * 2.0f - actionW - 8.0f;
            ui.stack("logcat.filter.wrap")
                .x(24.0f).y(52.0f).size(filterW, 36.0f)
                .content([&] {
                    components::input(ui, "logcat.filter")
                        .theme(themeTokens()).size(filterW, 36.0f).fontSize(13.0f)
                        .placeholder("正则过滤，不区分大小写（如 error|fatal）")
                        .value(state.logcatFilter)
                        .onChange([](const std::string& v) { state.logcatFilter = v; })
                        .build();
                }).build();

            const float actionX = 24.0f + filterW + 8.0f;
            if (streaming) {
                toolButton(ui, "logcat.clear", actionX, 52.0f, actionW, 36.0f,
                           0xF12D, "清空", false, true, [] {
                    std::lock_guard<std::mutex> lock(g_logcatMutex);
                    g_logcatLines.clear();
                    g_logcatSeq = 0;
                    state.logcatLastSeq = 0;
                    state.logcatScroll.set(0.0f);
                });
            } else {
                toolButton(ui, "logcat.restart", actionX, 52.0f, actionW, 36.0f,
                           0xF021, "重新开始", true, true, [] { startLogcat(); });
            }

            // Build the filtered view under the buffer lock.
            const bool regexOk = logcatEnsureRegex();
            std::vector<std::string> shown;
            {
                std::lock_guard<std::mutex> lock(g_logcatMutex);
                shown.reserve(g_logcatLines.size());
                for (const std::string& line : g_logcatLines) {
                    if (logcatLineMatches(line)) shown.push_back(line);
                }
                if (shown.size() > 800) {
                    const std::size_t excess = shown.size() - 800;
                    shown.erase(shown.begin(), shown.begin() + static_cast<std::ptrdiff_t>(excess));
                }
            }

            std::size_t totalLines = 0;
            { std::lock_guard<std::mutex> lock(g_logcatMutex); totalLines = g_logcatLines.size(); }
            std::string meta;
            if (!state.logcatRegexError.empty()) {
                meta = "正则表达式无效，请修改过滤条件";
            } else if (state.logcatFilter.empty()) {
                meta = "共 " + std::to_string(totalLines) + " 行";
            } else {
                meta = "共 " + std::to_string(totalLines) + " 行，匹配 " + std::to_string(shown.size()) + " 行";
            }
            ui.text("logcat.meta")
                .x(24.0f).y(94.0f).size(pw - 48.0f, 20.0f)
                .text(meta)
                .fontSize(12.0f).lineHeight(12.0f)
                .color(state.logcatRegexError.empty() ? kMuted : kRose)
                .build();

            // Auto-scroll to the tail whenever new lines arrived since last frame.
            const long long seq = g_logcatSeq.load();
            if (seq != state.logcatLastSeq) {
                state.logcatLastSeq = seq;
                state.logcatScroll.set(1.0e9f);  // clamped to the max offset -> bottom
            }

            components::scrollView(ui, "logcat.list")
                .x(24.0f).y(118.0f).size(pw - 48.0f, ph - 118.0f - 16.0f)
                .theme(themeTokens()).scrollbarWidth(8.0f).scrollbarGap(2.0f)
                .bind(state.logcatScroll)
                .contentKey("logcat." + std::to_string(shown.size()) + "." + state.logcatFilter)
                .content([&](eui::Ui& body, float cw, float) {
                    if (shown.empty()) {
                        body.text("logcat.empty")
                            .x(4.0f).y(0.0f).size(cw - 8.0f, 40.0f)
                            .text(regexOk ? "（等待日志…）" : "（无匹配）")
                            .fontSize(12.0f).lineHeight(12.0f).color(kMuted).build();
                        return;
                    }
                    for (std::size_t i = 0; i < shown.size(); ++i) {
                        body.text("logcat.line." + std::to_string(i))
                            .x(4.0f).y(static_cast<float>(i) * 18.0f).size(cw - 8.0f, 18.0f)
                            .text(shown[i]).fontSize(11.0f).lineHeight(11.0f).color(kInk).build();
                    }
                })
                .build();
        })
        .onOpenChange([](bool v) {
            state.logcatOpen = v;
            if (!v) {
                stopLogcat();
                logcatRegexClear();
            }
        })
        .build();
}

void composeTextPreviewDialog(eui::Ui& ui, float w, float h) {
    components::dialog(ui, "text.preview")
        .screen(w, h)
        .open(state.textPreviewOpen)
        .theme(themeTokens())
        .size(640.0f, 560.0f)
        .zIndex(1300)
        .content([&] {
            const float pw = 640.0f, ph = 560.0f;
            ui.text("text.preview.title")
                .x(24.0f).y(16.0f).size(pw - 48.0f, 26.0f)
                .text(state.textPreviewRemote)
                .fontSize(15.0f).lineHeight(15.0f).color(kInk).build();
            ui.stack("text.preview.input.wrap")
                .x(24.0f).y(48.0f).size(pw - 48.0f, ph - 48.0f - 56.0f)
                .content([&] {
                    components::input(ui, "text.preview.input")
                        .theme(themeTokens()).size(pw - 48.0f, ph - 48.0f - 56.0f)
                        .fontSize(13.0f).fontFamily("").multiline(true)
                        .value(state.textPreviewContent)
                        .onChange([](const std::string& v) { state.textPreviewContent = v; })
                        .build();
                }).build();
            toolButton(ui, "text.preview.save", pw - 24.0f - 200.0f, ph - 56.0f, 92.0f, 40.0f,
                       0xF0C7, "保存", true, true, [] { saveTextPreview(); });
            toolButton(ui, "text.preview.close", pw - 24.0f - 100.0f, ph - 56.0f, 100.0f, 40.0f,
                       0xF00D, "关闭", false, true, [] { state.textPreviewOpen = false; });
        })
        .onOpenChange([](bool v) { state.textPreviewOpen = v; })
        .build();
}

void composeImagePreviewDialog(eui::Ui& ui, float w, float h) {
    components::dialog(ui, "image.preview")
        .screen(w, h)
        .open(state.imagePreviewOpen)
        .theme(themeTokens())
        .size(520.0f, 640.0f)
        .zIndex(1300)
        .content([&] {
            const float pw = 520.0f, ph = 640.0f;
            ui.text("image.preview.title")
                .x(24.0f).y(16.0f).size(pw - 140.0f, 28.0f)
                .text(state.imagePreviewRemote)
                .fontSize(15.0f).lineHeight(15.0f).color(kInk).build();
            toolButton(ui, "image.preview.close", pw - 24.0f - 100.0f, 12.0f, 100.0f, 36.0f,
                       0xF00D, "关闭", false, true, [] { state.imagePreviewOpen = false; });
            ui.rect("image.preview.bg")
                .x(24.0f).y(56.0f).size(pw - 48.0f, ph - 72.0f)
                .color(kBackground).radius(10.0f).border(1.0f, kBorder).build();
            if (!state.imagePreviewLocal.empty()) {
                ui.image("image.preview.img")
                    .x(32.0f).y(64.0f).size(pw - 64.0f, ph - 88.0f)
                    .source(state.imagePreviewLocal)
                    .fit(eui::ImageFit::Contain)
                    .build();
            } else {
                ui.text("image.preview.loading")
                    .x(24.0f).y(56.0f).size(pw - 48.0f, ph - 72.0f)
                    .text("加载中…").fontSize(14.0f).lineHeight(14.0f).color(kMuted)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center).build();
            }
        })
        .onOpenChange([](bool v) { state.imagePreviewOpen = v; })
        .build();
}

void composePropertiesDialog(eui::Ui& ui, float w, float h) {
    components::dialog(ui, "file.properties")
        .screen(w, h)
        .open(state.propertiesOpen)
        .theme(themeTokens())
        .size(420.0f, 340.0f)
        .zIndex(1300)
        .content([&] {
            const float pw = 420.0f, ph = 340.0f;
            ui.text("file.properties.title")
                .x(24.0f).y(16.0f).size(pw - 48.0f, 26.0f)
                .text("属性").fontSize(17.0f).lineHeight(17.0f).color(kInk).build();
            components::scrollView(ui, "file.properties.list")
                .x(24.0f).y(52.0f).size(pw - 48.0f, ph - 52.0f - 56.0f)
                .theme(themeTokens()).scrollbarWidth(8.0f).scrollbarGap(2.0f)
                .contentKey("file.properties." + std::to_string(state.propertiesText.size()))
                .content([&](eui::Ui& body, float cw, float) {
                    std::istringstream iss(state.propertiesText);
                    std::string line;
                    int i = 0;
                    while (std::getline(iss, line) && i < 40) {
                        body.text("file.properties.line." + std::to_string(i))
                            .x(4.0f).y(static_cast<float>(i) * 22.0f).size(cw - 8.0f, 22.0f)
                            .text(line).fontSize(13.0f).lineHeight(13.0f).color(kInk).build();
                        ++i;
                    }
                })
                .build();
            toolButton(ui, "file.properties.close", pw - 24.0f - 100.0f, ph - 56.0f, 100.0f, 40.0f,
                       0xF00D, "关闭", false, true, [] { state.propertiesOpen = false; });
        })
        .onOpenChange([](bool v) { state.propertiesOpen = v; })
        .build();
}

void composeAppManageDialog(eui::Ui& ui, float w, float h) {
    components::dialog(ui, "app.manage")
        .screen(w, h)
        .open(state.appManageOpen)
        .theme(themeTokens())
        .size(560.0f, 560.0f)
        .zIndex(1300)
        .content([&] {
            const float pw = 560.0f, ph = 560.0f;
            ui.text("app.manage.title")
                .x(24.0f).y(16.0f).size(160.0f, 28.0f)
                .text("应用管理").fontSize(17.0f).lineHeight(17.0f).color(kInk).build();
            ui.stack("app.manage.filter.wrap")
                .x(200.0f).y(12.0f).size(pw - 340.0f, 36.0f)
                .content([&] {
                    components::input(ui, "app.manage.filter")
                        .theme(themeTokens()).size(pw - 340.0f, 36.0f).fontSize(13.0f)
                        .placeholder("过滤包名").value(state.appFilter)
                        .onChange([](const std::string& v) { state.appFilter = v; })
                        .build();
                }).build();
            toolButton(ui, "app.manage.refresh", pw - 24.0f - 200.0f, 12.0f, 92.0f, 36.0f,
                       0xF021, "刷新", true, true, [] { fetchAppList(); });
            toolButton(ui, "app.manage.close", pw - 24.0f - 100.0f, 12.0f, 100.0f, 36.0f,
                       0xF00D, "关闭", false, true, [] { state.appManageOpen = false; });

            const float listH = ph - 56.0f - 56.0f;
            ui.rect("app.manage.list.bg")
                .x(24.0f).y(56.0f).size(pw - 48.0f, listH)
                .color(kBackground).radius(8.0f).border(1.0f, kBorder).build();
            components::scrollView(ui, "app.manage.list")
                .x(24.0f).y(56.0f).size(pw - 48.0f, listH)
                .theme(themeTokens()).scrollbarWidth(8.0f).scrollbarGap(2.0f)
                .contentKey("app.manage." + std::to_string(state.appPackages.size()))
                .content([&](eui::Ui& body, float cw, float) {
                    const float rowH = 30.0f;
                    int shown = 0;
                    for (std::size_t i = 0; i < state.appPackages.size(); ++i) {
                        const std::string& pkg = state.appPackages[i];
                        if (!state.appFilter.empty() && pkg.find(state.appFilter) == std::string::npos) continue;
                        const float y = static_cast<float>(shown) * rowH;
                        const bool selected = (pkg == state.appSelectedPackage);
                        body.stack("app.manage.row." + std::to_string(i))
                            .y(y).size(cw, rowH)
                            .content([&] {
                                body.rect("app.manage.row." + std::to_string(i) + ".bg")
                                    .size(cw, rowH - 2.0f)
                                    .states(selected ? kAccentSoft : kClear, kSurfaceHover, kSurfaceAct)
                                    .radius(6.0f)
                                    .onClick([pkg] { state.appSelectedPackage = pkg; })
                                    .build();
                                body.text("app.manage.row." + std::to_string(i) + ".label")
                                    .x(10.0f).y(0.0f).size(cw - 20.0f, rowH)
                                    .text(pkg).fontSize(13.0f).lineHeight(13.0f).color(kInk)
                                    .verticalAlign(eui::VerticalAlign::Center).build();
                            }).build();
                        ++shown;
                    }
                })
                .build();
            toolButton(ui, "app.manage.uninstall", 24.0f, ph - 52.0f, 120.0f, 40.0f,
                       0xF1F8, "卸载", false, !state.appSelectedPackage.empty(), [] { uninstallSelectedApp(); });
            toolButton(ui, "app.manage.clear", 156.0f, ph - 52.0f, 120.0f, 40.0f,
                       0xF12D, "清数据", false, !state.appSelectedPackage.empty(), [] { clearSelectedAppData(); });
        })
        .onOpenChange([](bool v) { state.appManageOpen = v; })
        .build();
}

void composeHelpDialog(eui::Ui& ui, float w, float h) {
    components::dialog(ui, "help.dialog")
        .screen(w, h)
        .open(state.helpOpen)
        .theme(themeTokens())
        .size(460.0f, 420.0f)
        .zIndex(1300)
        .content([&] {
            const float pw = 460.0f, ph = 420.0f;
            ui.text("help.title")
                .x(24.0f).y(16.0f).size(pw - 48.0f, 28.0f)
                .text("快捷键").fontSize(17.0f).lineHeight(17.0f).color(kInk).build();
            components::scrollView(ui, "help.list")
                .x(24.0f).y(52.0f).size(pw - 48.0f, ph - 52.0f - 56.0f)
                .theme(themeTokens()).scrollbarWidth(8.0f).scrollbarGap(2.0f)
                .contentKey("help")
                .content([&](eui::Ui& body, float cw, float) {
                    const char* lines[] = {
                        "F5          刷新目录",
                        "F2          重命名",
                        "Enter       打开选中文件夹",
                        "Backspace   返回上一级",
                        "Delete      删除选中项",
                        "Ctrl+L      编辑路径",
                        "Ctrl+C      复制",
                        "Ctrl+X      剪切",
                        "Ctrl+V      粘贴",
                        "Ctrl+A      全选",
                        "Esc         关闭弹窗 / 取消编辑",
                    };
                    for (int i = 0; i < 11; ++i) {
                        body.text("help.line." + std::to_string(i))
                            .x(4.0f).y(static_cast<float>(i) * 26.0f).size(cw - 8.0f, 26.0f)
                            .text(lines[i]).fontSize(13.0f).lineHeight(13.0f).color(kInk).build();
                    }
                })
                .build();
            toolButton(ui, "help.close", pw - 24.0f - 100.0f, ph - 56.0f, 100.0f, 40.0f,
                       0xF00D, "关闭", false, true, [] { state.helpOpen = false; });
        })
        .onOpenChange([](bool v) { state.helpOpen = v; })
        .build();
}

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("ADB 文件浏览器")
        .pageId("adb_file_browser")
        .clearColor(kBackground)
        .windowSize(1000, 520)
        .textFont("C:/Windows/Fonts/msyh.ttc")
        .fps(90.0)
        .onKeyEvent([](const eui::KeyEvent& ev) { handleGlobalKey(ev); });
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    if (!state.initialized) {
        state.initialized = true;
        state.adbPath = findAdb();
        state.adbFound = !state.adbPath.empty();
        state.downloadDir = defaultDownloadDir();
        loadSettings();
        applyTheme();
        loadBookmarks();
        loadCommands();
        loadLastPaths();
        app::setDefaultTextFont(settings.fontFile);
        app::setFontScale(settings.fontSizeScale);
        app::setUiScale(settings.uiScale);
        restoreWindowState();
        applyMinWindowSize();
        fetchAdbVersion();
        refreshDevices();
        // Neither adb nor scrcpy ships with the app any more, so offer to fetch
        // whatever is missing instead of leaving a fresh install unable to talk
        // to a phone with no hint about what to do. The dialog needs the download
        // URLs, which is what the update check fetches.
        if (!state.setupPromptDismissed && (!state.adbFound || findScrcpy().empty())) {
            state.setupOpen = true;
            checkForUpdates();
        }
        app::setDropHandler([](const std::vector<std::string>& files) {
            if (state.selectedDevice.empty()) {
                toast("未选择设备", "请先选择要上传到的设备。");
                return;
            }
            pushFiles(files);
        });
    }

    const float W = screen.width;
    const float H = screen.height;
    state.logicalW = W;
    state.logicalH = H;
    const float margin = 14.0f;
    const float contentW = std::max(0.0f, W - margin * 2.0f);
    const float x = margin;

    const float topY = kTitleBarHeight + 10.0f;
    const float topBarH = 44.0f;
    const float gap = 10.0f;
    const float pathY = topY + topBarH + gap;
    const float pathBarH = 40.0f;
    const float headerY = pathY + pathBarH + gap;
    const float headerH = 24.0f;
    const float listY = headerY + headerH + 4.0f;
    const float statusH = 30.0f;
    const float listH = std::max(0.0f, H - listY - statusH - 12.0f);
    const float statusY = H - statusH - 8.0f;

    ui.stack("root")
        .size(W, H)
        .content([&] {
            ui.rect("background")
                .size(W, H)
                .color(kBackground)
                .build();

            composeTitleBar(ui, W, kTitleBarHeight);
            composeTopBar(ui, x, topY, contentW, topBarH);
            composeBreadcrumb(ui, x, pathY, contentW, pathBarH);
            composeHeader(ui, x + 6.0f, headerY, contentW - 12.0f, headerH);
            composeFileList(ui, x, listY, contentW, listH);
            composeStatusBar(ui, x, statusY, contentW, statusH);

            composeToast(ui, W, H);
            composeDeviceMenu(ui, W, H);
            composeRowMenu(ui, W, H);
            composeBookmarkMenu(ui, W, H);
            composeCommandMenu(ui, W, H);
            composeConfirmDialog(ui, W, H);
            composePromptDialog(ui, W, H);
            composeSettingsDialog(ui, W, H);
            composeUpdateDialog(ui, W, H);
            composeSetupDialog(ui, W, H);
            composeBookmarkManageDialog(ui, W, H);
            composeCommandManageDialog(ui, W, H);
            composeCommandOutputDialog(ui, W, H);
            composeDeviceInfoDialog(ui, W, H);
            composeLogcatDialog(ui, W, H);
            composeTextPreviewDialog(ui, W, H);
            composeImagePreviewDialog(ui, W, H);
            composePropertiesDialog(ui, W, H);
            composeAppManageDialog(ui, W, H);
            composeHelpDialog(ui, W, H);
        })
        .build();
}

} // namespace app
