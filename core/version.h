// Version parsing/comparison and the version strings hidden inside tool output.
#pragma once

#include <string>
#include <vector>

namespace adb::core {

// Numeric components of a version string. Digits accumulate; '.', '-' and '_'
// separate; anything else (e.g. a trailing space) ends the parse.
std::vector<int> parseVersionParts(const std::string& v);

// -1 / 0 / 1 for a<b / a==b / a>b, comparing component-wise and treating a
// missing component as 0 ("1.2" == "1.2.0").
int compareVersions(const std::string& a, const std::string& b);

// First "<something> version <digits>" occurrence, case-insensitive on the word
// "version": real adb output contains both "Android Debug Bridge version 1.0.41"
// and "Version 37.0.0-14910828", and which line carries the meaningful number
// differs between adb releases. Returns "" when no version is found.
std::string adbShortVersion(const std::string& out);

// "scrcpy 4.1 <https://...>" -> "4.1". Returns "" when "scrcpy" is absent and
// when it is present but not followed by a version.
std::string scrcpyShortVersion(const std::string& out);

}  // namespace adb::core
