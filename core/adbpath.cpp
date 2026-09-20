#include "core/adbpath.h"

#include <cstdlib>
#include <filesystem>

#include "core/process.h"

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
            if (!dir.empty()) {
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
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) return candidate;
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
