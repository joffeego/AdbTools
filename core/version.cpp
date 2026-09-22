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
    // adb's version output carries two different numbers:
    //
    //     Android Debug Bridge version 1.0.41      <- adb *protocol* version
    //     Version 37.0.1-15733141                  <- platform-tools *revision*
    //
    // The updater compares this value against the revision published in Google's
    // repository2-1.xml, so the revision line is the one that matters. Returning
    // the protocol version (as this did) compared 1.0.41 against 37.0.1, which is
    // always "older" - so the app offered an adb update on every single check,
    // even when platform-tools was already current, and following through meant
    // re-downloading 8 MB and replacing a perfectly good adb (which then failed,
    // because a running adb.exe cannot be overwritten - see copyFileOver).
    //
    // Prefer a line that *begins* with "Version " (the revision), and fall back to
    // the first "version <x.y.z>" anywhere in the text so older adb builds, which
    // only print the protocol line, still report something usable.
    auto leadingNumber = [](const std::string& s) {
        std::string v;
        for (char c : s) {
            if ((c >= '0' && c <= '9') || c == '.') {
                v += c;
            } else {
                break;
            }
        }
        return v;
    };

    std::string fallback;
    std::size_t lineStart = 0;
    while (lineStart <= out.size()) {
        std::size_t lineEnd = out.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = out.size();
        const std::string line = lower(trim(out.substr(lineStart, lineEnd - lineStart)));

        static const std::string kVersionPrefix = "version ";
        if (line.rfind(kVersionPrefix, 0) == 0) {
            const std::string v = leadingNumber(line.substr(kVersionPrefix.size()));
            if (!v.empty()) return v;
        } else if (fallback.empty()) {
            const std::size_t p = line.find(kVersionPrefix);
            if (p != std::string::npos) {
                fallback = leadingNumber(line.substr(p + kVersionPrefix.size()));
            }
        }

        if (lineEnd == out.size()) break;
        lineStart = lineEnd + 1;
    }
    return fallback;
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
