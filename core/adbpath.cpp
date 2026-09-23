#include "core/adbpath.h"

#include <cctype>
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
    // blocks for the SMB connect timeout (measured ~19 s), and adb is not installed
    // on network shares in practice - while a laptop off the office network hits
    // exactly this case on every launch.
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
        std::string pathStr = pathEnv;
        std::size_t start = 0;
        while (start <= pathStr.size()) {
            std::size_t end = pathStr.find(';', start);
            std::string dir = pathStr.substr(start, end == std::string::npos ? std::string::npos : end - start);
            start = (end == std::string::npos) ? pathStr.size() + 1 : end + 1;
            // Skip entries that cannot hold adb and would stall the scan: an
            // unreachable network share costs ~19 seconds to probe (see
            // isUsablePathEntry).
            if (!dir.empty() && isUsablePathEntry(dir)) {
                candidates.push_back(dir + "\\adb.exe");
                candidates.push_back(dir + "/adb");
            }
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
