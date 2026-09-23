#include "core/adbpath.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>

#include "core/process.h"
#include "core/fileio.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace adb::core {

std::string executableDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::error_code ec;
        std::filesystem::path p(toUtf8(std::wstring(buf, static_cast<std::size_t>(n))));
        return p.parent_path().string();
    }
#endif
    std::error_code ec;
    return std::filesystem::current_path(ec).string();
}

bool isUsablePathEntry(const std::string& dir) {
    if (dir.empty()) return false;
#ifdef _WIN32
    // A UNC entry is not probed at all. When the server is unreachable the stat
    // blocks for the TCP connect timeout - measured at 21026 ms for
    // "\\192.0.2.201\share\adb.exe" (TEST-NET-1), then 0.1 ms for every later call
    // on that host thanks to the SMB negative cache, and another ~21 s for the
    // next unreachable host. adb is not installed on network shares in practice,
    // while a laptop off the office network hits exactly this case on every
    // launch. See core/adbpath.h for the numbers.
    if (dir.size() >= 2 && (dir[0] == '\\' || dir[0] == '/') &&
        (dir[1] == '\\' || dir[1] == '/')) {
        return false;
    }
    // "X:..." - check the drive exists first. GetDriveTypeW does not touch the
    // network for a mapped drive, and returns DRIVE_NO_ROOT_DIR for a letter that
    // is no longer assigned, which GetFileAttributes would otherwise stall on.
    if (dir.size() >= 2 && std::isalpha(static_cast<unsigned char>(dir[0])) && dir[1] == ':') {
        const std::wstring root = toWide(dir.substr(0, 2) + "\\");
        const UINT type = GetDriveTypeW(root.c_str());
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) return false;
        // A network drive is skipped as well: for a persistent mapping whose
        // server is gone (the "red X" drive Explorer hangs on) the next stat
        // triggers a reconnect and stalls for the same ~21 s. GetDriveTypeW
        // itself answers DRIVE_REMOTE without any network traffic, so this check
        // costs nothing - the stall it prevents would be the whole startup.
        if (type == DRIVE_REMOTE) return false;
    }
    return true;
#else
    (void)dir;
    return true;
#endif
}

std::string findAdb() {
    std::vector<std::string> candidates;

    const char* sdkRoot = std::getenv("ANDROID_SDK_ROOT");
    if (sdkRoot == nullptr || *sdkRoot == '\0') sdkRoot = std::getenv("ANDROID_HOME");
    if (sdkRoot != nullptr && *sdkRoot != '\0') {
        candidates.push_back(std::string(sdkRoot) + "\\platform-tools\\adb.exe");
        candidates.push_back(std::string(sdkRoot) + "/platform-tools/adb.exe");
        candidates.push_back(std::string(sdkRoot) + "/platform-tools/adb");
    }

    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData != nullptr && *localAppData != '\0') {
        candidates.push_back(std::string(localAppData) + "\\Android\\Sdk\\platform-tools\\adb.exe");
    }
    const char* programFiles = std::getenv("ProgramFiles");
    if (programFiles != nullptr && *programFiles != '\0') {
        candidates.push_back(std::string(programFiles) + "\\Android\\Sdk\\platform-tools\\adb.exe");
    }

    const char* pathEnv = std::getenv("PATH");
    if (pathEnv != nullptr && *pathEnv != '\0') {
        // Bounded, like the scrcpy scan in the GUI: whatever a machine's PATH
        // happens to hold, locating adb must not be able to spend the whole startup
        // on it. A single blocking probe cannot be interrupted from here - that is
        // what the filters in isUsablePathEntry are for - but a long tail of merely
        // slow entries is cut off. The candidates after this loop (next to the
        // executable, which is where an on-demand download lands) are still probed,
        // so giving up on PATH never loses a usable adb.
        const auto scanDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        std::string pathStr = pathEnv;
        std::size_t start = 0;
        while (start <= pathStr.size()) {
            std::size_t end = pathStr.find(';', start);
            std::string dir = pathStr.substr(start, end == std::string::npos ? std::string::npos : end - start);
            start = (end == std::string::npos) ? pathStr.size() + 1 : end + 1;
            // Skip entries that cannot hold adb and would stall the scan: an
            // unreachable network share costs ~21 seconds to probe (see
            // isUsablePathEntry).
            if (!dir.empty() && isUsablePathEntry(dir)) {
                candidates.push_back(dir + "\\adb.exe");
                candidates.push_back(dir + "/adb");
            }
            if (std::chrono::steady_clock::now() > scanDeadline) break;
        }
    }

    // A copy shipped next to the executable (and in the bundled scrcpy folder),
    // used as a fallback when no SDK / PATH adb is present.
    {
        const std::string dir = executableDir();
        candidates.push_back(dir + "\\adb.exe");
        candidates.push_back(dir + "\\scrcpy\\adb.exe");
        candidates.push_back(dir + "/adb");
        candidates.push_back(dir + "/scrcpy/adb");
    }

    for (const std::string& candidate : candidates) {
        // Never std::filesystem directly here: the candidates include every PATH
        // entry, and a PATH entry with non-ASCII characters holds ANSI bytes (not
        // UTF-8) on a non-UTF-8 locale, which makes path construction throw. This
        // file is built with exceptions, but the caller is not - and an escaping
        // throw there aborts the process (that is how scanning PATH crashed the
        // GUI on startup). See core/fileio.h.
        if (isRegularFileNoThrow(candidate)) return candidate;
    }
    return "";
}

std::string defaultDownloadDir() {
    const char* profile = std::getenv("USERPROFILE");
    if (profile != nullptr && *profile != '\0') return std::string(profile) + "\\Downloads";
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') return std::string(home) + "/Downloads";
    return ".";
}

}  // namespace adb::core
