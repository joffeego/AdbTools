#include "core/fileio.h"

#include <filesystem>
#include <fstream>
#include <string>

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
namespace {

#ifdef _WIN32
// Narrow -> wide without ever throwing, preferring UTF-8 (paths we built from
// wide APIs and handed on as UTF-8) and falling back to the ANSI code page
// (what the process environment block actually holds on a non-UTF-8 locale).
std::wstring toWideLenient(const std::string& s) {
    if (s.empty()) return std::wstring();
    for (UINT codePage : {CP_UTF8, CP_ACP}) {
        const int n = MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0) continue;
        std::wstring wide(static_cast<std::size_t>(n), L'\0');
        const int written =
            MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), &wide[0], n);
        if (written > 0) {
            wide.resize(static_cast<std::size_t>(written));
            return wide;
        }
    }
    return std::wstring();
}
#endif

}  // namespace

bool isRegularFileNoThrow(const std::string& path) {
    if (path.empty()) return false;
#ifdef _WIN32
    const std::wstring wide = toWideLenient(path);
    if (wide.empty()) return false;
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    try {
        std::error_code ec;
        return std::filesystem::is_regular_file(std::filesystem::path(path), ec) && !ec;
    } catch (...) {
        // Never let a filesystem error escape: callers may be compiled without
        // exception support, where it would abort the process.
        return false;
    }
#endif
}

bool writeFileAtomic(const std::string& path, const std::string& contents) {
    try {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path target(path);
        const fs::path parent = target.parent_path();
        const fs::path temp = parent / (target.filename().string() + ".tmp");

        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            out.flush();
            if (!out) {  // disk full / write error: do not touch the real file
                out.close();
                fs::remove(temp, ec);
                return false;
            }
        }

        fs::rename(temp, target, ec);
        if (ec) {
            // Some filesystems refuse to replace an existing file; retry after
            // removing it, but only now that the complete new file exists.
            ec.clear();
            fs::remove(target, ec);
            ec.clear();
            fs::rename(temp, target, ec);
            if (ec) {
                fs::remove(temp, ec);
                return false;
            }
        }
        return true;
    } catch (...) {
        // Constructing a std::filesystem::path from a std::string can throw, and
        // callers may be built without exception support (the GUI is), where an
        // escaping throw becomes std::terminate -> abort. Report failure instead.
        return false;
    }
}

}  // namespace adb::core
