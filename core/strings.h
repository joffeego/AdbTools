// Small, dependency-free string helpers shared by the app and its tests.
//
// These used to live in main.cpp's anonymous namespace, which made them
// impossible to unit test without linking the whole GUI. They are pure
// functions with no state, so they were the obvious first thing to extract.
#pragma once

#include <string>
#include <vector>

namespace adb::core {

std::string trim(const std::string& s);

// Split on runs of whitespace (std::istream >> semantics).
std::vector<std::string> splitWs(const std::string& s);

// ASCII lowercase copy. Bytes >= 0x80 are left untouched (the app's data is
// UTF-8 and must not be mangled).
std::string lower(std::string s);

// Shorten to `limit` characters, appending "..." when it does not fit.
// `limit` below 4 disables shortening.
std::string shorten(const std::string& s, int limit);

// Human-readable byte count, e.g. 1536 -> "1.5 KB". Negative input -> "".
std::string formatSize(long long bytes);

// True for the three-letter English month abbreviations used by `ls -la`.
bool isMonthName(const std::string& s);

// Strict non-negative decimal parse: every character must be a digit (no sign,
// no whitespace, no trailing junk), otherwise 0. Used for the size and date
// columns of `ls -la`.
long long parseSize(const std::string& s);

}  // namespace adb::core
