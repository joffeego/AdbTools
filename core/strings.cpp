#include "core/strings.h"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <sstream>
#include <system_error>

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
