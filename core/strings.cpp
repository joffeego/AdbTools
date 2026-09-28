#include "core/strings.h"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace adb::core {

std::string trim(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string token;
    while (in >> token) out.push_back(token);
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string shorten(const std::string& s, int limit) {
    if (limit < 4 || static_cast<int>(s.size()) <= limit) return s;
    return s.substr(0, static_cast<std::size_t>(limit - 3)) + "...";
}

namespace {

// One UTF-8 code point: its value and how many bytes it occupies.
struct CodePoint {
    unsigned int value = 0;
    std::size_t bytes = 1;
};

// Decode the sequence starting at `s[i]`. A malformed or truncated sequence is
// reported as a single byte holding the raw value, so a corrupt string still
// advances one byte at a time instead of desynchronising or looping forever.
CodePoint decodeCodePoint(const std::string& s, std::size_t i) {
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    CodePoint cp;
    cp.value = lead;

    std::size_t length = 1;
    if (lead >= 0xF0u) {
        length = 4;
        cp.value = lead & 0x07u;
    } else if (lead >= 0xE0u) {
        length = 3;
        cp.value = lead & 0x0Fu;
    } else if (lead >= 0xC0u) {
        length = 2;
        cp.value = lead & 0x1Fu;
    } else {
        return cp;  // ASCII, or a stray continuation byte
    }
    if (i + length > s.size()) return cp;  // truncated at the end of the string

    for (std::size_t k = 1; k < length; ++k) {
        const unsigned char next = static_cast<unsigned char>(s[i + k]);
        if ((next & 0xC0u) != 0x80u) return cp;  // not a continuation byte
        cp.value = (cp.value << 6) | (next & 0x3Fu);
    }
    cp.bytes = length;
    return cp;
}

// East Asian Wide / Fullwidth, plus the emoji blocks - the code points Windows
// draws in a double-width cell. Borderline ranges are counted wide on purpose:
// over-reporting costs one character of space, under-reporting would push text
// out of its box, which is the bug this exists to prevent.
bool isWideCodePoint(unsigned int cp) {
    return (cp >= 0x1100u && cp <= 0x115Fu) ||   // Hangul Jamo
           (cp >= 0x2E80u && cp <= 0x303Eu) ||   // CJK radicals, Kangxi, punctuation
           (cp >= 0x3041u && cp <= 0x33FFu) ||   // kana, Bopomofo, Hangul compat, CJK compat
           (cp >= 0x3400u && cp <= 0x4DBFu) ||   // CJK ext A
           (cp >= 0x4E00u && cp <= 0x9FFFu) ||   // CJK unified ideographs
           (cp >= 0xA000u && cp <= 0xA4CFu) ||   // Yi
           (cp >= 0xAC00u && cp <= 0xD7A3u) ||   // Hangul syllables
           (cp >= 0xF900u && cp <= 0xFAFFu) ||   // CJK compatibility ideographs
           (cp >= 0xFE10u && cp <= 0xFE19u) ||   // vertical forms
           (cp >= 0xFE30u && cp <= 0xFE6Fu) ||   // CJK compatibility forms
           (cp >= 0xFF00u && cp <= 0xFF60u) ||   // fullwidth forms
           (cp >= 0xFFE0u && cp <= 0xFFE6u) ||   // fullwidth signs
           (cp >= 0x1F300u && cp <= 0x1F64Fu) || // emoji
           (cp >= 0x1F680u && cp <= 0x1F6FFu) ||
           (cp >= 0x1F900u && cp <= 0x1F9FFu) ||
           (cp >= 0x1FA70u && cp <= 0x1FAFFu) ||
           (cp >= 0x20000u && cp <= 0x3FFFDu);   // CJK ext B and beyond
}

int codePointColumns(const CodePoint& cp) { return isWideCodePoint(cp.value) ? 2 : 1; }

// U+2026, "…". One column wide, unlike the three dots it replaces.
constexpr char kEllipsis[] = "\xE2\x80\xA6";

}  // namespace

int displayColumns(const std::string& s) {
    int columns = 0;
    for (std::size_t i = 0; i < s.size();) {
        const CodePoint cp = decodeCodePoint(s, i);
        columns += codePointColumns(cp);
        i += cp.bytes;
    }
    return columns;
}

std::string shortenColumns(const std::string& s, int maxColumns) {
    if (maxColumns < 2 || displayColumns(s) <= maxColumns) return s;

    const int budget = maxColumns - 1;  // the ellipsis takes one column
    std::string out;
    int used = 0;
    for (std::size_t i = 0; i < s.size();) {
        const CodePoint cp = decodeCodePoint(s, i);
        const int width = codePointColumns(cp);
        if (used + width > budget) break;
        out.append(s, i, cp.bytes);
        used += width;
        i += cp.bytes;
    }
    return out + kEllipsis;
}

std::string shortenColumnsHead(const std::string& s, int maxColumns) {
    if (maxColumns < 2 || displayColumns(s) <= maxColumns) return s;

    const int budget = maxColumns - 1;
    std::vector<std::pair<std::size_t, int>> units;  // code point offset -> width
    for (std::size_t i = 0; i < s.size();) {
        const CodePoint cp = decodeCodePoint(s, i);
        units.emplace_back(i, codePointColumns(cp));
        i += cp.bytes;
    }

    std::size_t start = s.size();
    int used = 0;
    for (std::size_t k = units.size(); k-- > 0;) {
        if (used + units[k].second > budget) break;
        used += units[k].second;
        start = units[k].first;
    }
    return std::string(kEllipsis) + s.substr(start);
}

std::string formatSize(long long bytes) {
    if (bytes < 0) return "";
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char buf[64]{};
    if (unit == 0) {
        std::snprintf(buf, sizeof(buf), "%lld B", bytes);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
    }
    return buf;
}

bool isMonthName(const std::string& s) {
    static const char* kMonths[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (s.size() != 3) return false;
    for (const char* m : kMonths) if (s == m) return true;
    return false;
}

long long parseSize(const std::string& s) {
    // A leading '-' must not be accepted: this parses the size column of `ls -la`
    // output, where a negative byte count is meaningless. Negatives are also how
    // the app represents "unknown" progress internally, so letting them through
    // here could silently turn a parsed size into a sentinel value.
    if (s.empty() || s[0] < '0' || s[0] > '9') return 0;

    long long value = 0;
    const char* begin = s.c_str();
    const char* end = begin + s.size();
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end) return 0;
    return value;
}

}  // namespace adb::core
