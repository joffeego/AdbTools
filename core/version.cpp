#include "core/version.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#include "core/strings.h"

namespace adb::core {

std::vector<int> parseVersionParts(const std::string& v) {
    std::vector<int> parts;
    std::string cur;
    for (char c : v) {
        if (c >= '0' && c <= '9') {
            cur += c;
        } else if (c == '.' || c == '-' || c == '_') {
            if (!cur.empty()) { parts.push_back(std::atoi(cur.c_str())); cur.clear(); }
        } else {
            break;  // stop at the first non-version character (e.g. a space)
        }
    }
    if (!cur.empty()) parts.push_back(std::atoi(cur.c_str()));
    return parts;
}

int compareVersions(const std::string& a, const std::string& b) {
    const std::vector<int> pa = parseVersionParts(a);
    const std::vector<int> pb = parseVersionParts(b);
    const std::size_t n = std::max(pa.size(), pb.size());
    for (std::size_t i = 0; i < n; ++i) {
        const int x = i < pa.size() ? pa[i] : 0;
        const int y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

std::string adbShortVersion(const std::string& out) {
    // Match "version " case-insensitively: real adb output contains both
    // "Android Debug Bridge version 1.0.41" (lowercase) and
    // "Version 37.0.0-14910828" (capitalized), and which one carries the
    // platform-tools revision differs between adb releases.
    static const std::string kNeedle = "version ";
    const std::string haystack = lower(out);
    std::size_t pos = 0;
    while ((pos = haystack.find(kNeedle, pos)) != std::string::npos) {
        const std::size_t start = pos + kNeedle.size();
        std::string v;
        for (std::size_t i = start; i < out.size(); ++i) {
            const char c = out[i];
            if ((c >= '0' && c <= '9') || c == '.') v += c;
            else break;
        }
        if (!v.empty()) return v;
        pos = start;
    }
    return "";
}

std::string scrcpyShortVersion(const std::string& out) {
    const std::size_t p = out.find("scrcpy");
    if (p == std::string::npos) return "";
    std::size_t i = p + 6;  // len("scrcpy")
    while (i < out.size()) {
        const char c = out[i];
        if (c == ' ' || c == '\t' || c == 'v' || c == 'V') ++i;
        else break;
    }
    std::string v;
    while (i < out.size()) {
        const char c = out[i];
        if ((c >= '0' && c <= '9') || c == '.') v += c;
        else break;
        ++i;
    }
    return v;
}

}  // namespace adb::core
