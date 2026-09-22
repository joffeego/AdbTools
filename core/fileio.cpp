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

bool replaceFileOver(const std::string& src, const std::string& dst, std::string& err) {
    try {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) return true;

        const std::string originalError = ec.message();
        const fs::path target(dst);
        const fs::path aside =
            target.parent_path() / (target.filename().string() + ".old");

        // Overwriting a running executable is refused, but renaming one is
        // allowed - so move the old file out of the way and create the new one
        // under the original name.
        std::error_code ecAside;
        fs::remove(aside, ecAside);  // a leftover from an earlier update
        ecAside.clear();
        fs::rename(target, aside, ecAside);
        if (ecAside) {
            err = "替换 " + target.filename().string() + " 失败：" + originalError;
            return false;
        }

        std::error_code ecCopy;
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ecCopy);
        if (ecCopy) {
            // Put the original back: leaving the directory without a working
            // adb.exe would be worse than the update not happening.
            std::error_code ecRestore;
            fs::rename(aside, target, ecRestore);
            err = "替换 " + target.filename().string() + " 失败：" + ecCopy.message();
            return false;
        }

        // Best effort. While the process that was using the old file is still
        // running this fails, which is fine - the file simply stays as
        // "<name>.old" until a later update tries again.
        std::error_code ecCleanup;
        fs::remove(aside, ecCleanup);
        return true;
    } catch (...) {
        err = "替换文件失败";
        return false;
    }
}

bool writeFileAtomic(const std::string& path, const std::string& contents) {    try {
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
