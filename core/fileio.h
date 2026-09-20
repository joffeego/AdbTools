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

}  // namespace adb::core
