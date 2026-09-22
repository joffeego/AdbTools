// Cross-platform file helpers shared by the app and its tests.
#pragma once

#include <string>

namespace adb::core {

// Atomically replace `path` with `contents`.
//
// The app persists its settings, bookmarks, commands and recent paths next to
// the executable. Writing those with a plain truncating ofstream means a crash
// (or the power going out) mid-write leaves a half-written or empty file, and
// the user silently loses their configuration. Instead we write a sibling
// temporary file, flush it, then rename it over the target - a rename within one
// directory is atomic on NTFS, so readers only ever observe the old file or the
// complete new one.
//
// Returns false if the file could not be written, in which case the previous
// contents (if any) are untouched and no temp file is left behind. Callers keep
// running with their in-memory state either way.
bool writeFileAtomic(const std::string& path, const std::string& contents);

// True when `path` names an existing regular file. Never throws.
//
// This exists because the obvious spelling is a landmine on Windows:
//
//     std::filesystem::is_regular_file(candidate, ec)   // still throws!
//
// The std::string -> std::filesystem::path conversion happens *before* the call,
// and libstdc++ converts narrow strings as UTF-8. A PATH entry holding non-ASCII
// characters does not contain UTF-8 on a Chinese Windows - the environment block
// is in the ANSI code page (GBK) - so the conversion raises
// filesystem_error("Cannot convert character sequence: Illegal byte sequence").
// The error_code overload cannot help, because the argument is built first.
//
// That matters because the GUI is compiled with -fno-exceptions: an escaping
// throw becomes std::terminate -> abort(). Scanning PATH with std::filesystem
// therefore aborted the app whenever a non-ASCII PATH entry was reached - which
// is a very common PATH on Windows in this locale.
//
// This helper converts through the Win32 wide API instead (trying UTF-8, then
// the ANSI code page, which is what the environment actually uses), so a
// mismatched entry simply reports "not found" rather than killing the process.
bool isRegularFileNoThrow(const std::string& path);

// Copy `src` over `dst`, replacing `dst` if it exists. Never throws.
//
// The obvious std::filesystem::copy_file(..., overwrite_existing) fails when the
// destination is a *running executable*, and that is not a corner case here: the
// adb updater replaces adb.exe while the adb server (and any adb client that has
// not exited yet) is running it. Windows refuses to overwrite a running image,
// but it does allow renaming one, so on a sharing violation the existing file is
// moved aside to "<name>.old" and the copy is retried. That is the same trick the
// app's self-updater uses for adb_browser.exe.
//
// The moved-aside file is removed afterwards, best effort: while the process that
// held it is still alive the delete fails, which is harmless and must not fail
// the update.
//
// On failure `err` describes what went wrong, and the original file is put back if
// it had to be moved aside.
bool replaceFileOver(const std::string& src, const std::string& dst, std::string& err);

}  // namespace adb::core
