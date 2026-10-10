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
//
// This counts *bytes*, so it is only for ASCII-ish text (an error message, a
// command label). For anything the user typed - file names, quick-path aliases -
// use shortenColumns below: cutting a UTF-8 string at a byte offset splits a
// Chinese character in half and the tail renders as a replacement glyph.
std::string shorten(const std::string& s, int limit);

// --- display width (UTF-8) ---------------------------------------------------
//
// How many columns a string occupies when drawn in a fixed-width box: a CJK or
// emoji code point is two columns wide, everything else one. The file list's
// name column and the quick-path menu both need this to decide how much of a
// string still fits.
int displayColumns(const std::string& s);

// Truncate to at most `maxColumns` display columns, appending "…" when it does
// not fit. Never cuts a UTF-8 sequence in half. `maxColumns` below 2 disables
// shortening.
std::string shortenColumns(const std::string& s, int maxColumns);

// Same, but drops from the front: "/sdcard/DCIM/Camera" -> "…/DCIM/Camera".
// A device path is read from its end - the last component is the one that says
// where you are - so the quick-path dialog shows paths this way.
std::string shortenColumnsHead(const std::string& s, int maxColumns);

// --- searching text ----------------------------------------------------------
//
// A byte range inside a string: [start, end).
struct TextMatch {
    std::size_t start = 0;
    std::size_t end = 0;
};

// Every occurrence of `needle` in `haystack`, in order.
//
// Matches only start on a UTF-8 code point boundary: looking for "中" must not hit
// the middle of some other character's three bytes. Comparison is ASCII
// case-insensitive (the same rule as `lower` - bytes >= 0x80 are compared as-is),
// so "README" finds "readme" while "Ä" only finds "Ä".
//
// Stops after `maxMatches` hits so a one-letter query in a huge file cannot
// allocate without bound; the caller can tell it was cut off because the result is
// exactly `maxMatches` long.
std::vector<TextMatch> findMatches(const std::string& haystack, const std::string& needle,
                                   bool caseSensitive = false, std::size_t maxMatches = 5000);

// 1-based number of the line containing byte `offset` (for "第 N 行" style hints).
std::size_t lineNumberAt(const std::string& text, std::size_t offset);

// Human-readable byte count, e.g. 1536 -> "1.5 KB". Negative input -> "".
std::string formatSize(long long bytes);

// True for the three-letter English month abbreviations used by `ls -la`.
bool isMonthName(const std::string& s);

// Strict non-negative decimal parse: every character must be a digit (no sign,
// no whitespace, no trailing junk), otherwise 0. Used for the size and date
// columns of `ls -la`.
long long parseSize(const std::string& s);

}  // namespace adb::core
