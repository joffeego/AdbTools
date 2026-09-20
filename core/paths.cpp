#include "core/paths.h"

namespace adb::core {

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string joinPath(const std::string& base, const std::string& name) {
    if (base.empty() || base == "/") return "/" + name;
    if (base.back() == '/') return base + name;
    return base + "/" + name;
}

std::string parentPath(const std::string& p) {
    if (p.empty() || p == "/") return "/";
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    std::size_t pos = s.rfind('/');
    if (pos == std::string::npos) return "/";
    if (pos == 0) return "/";
    return s.substr(0, pos);
}

std::vector<std::string> splitPath(const std::string& p) {
    std::vector<std::string> parts;
    if (p.empty() || p == "/") return parts;
    std::size_t start = (p[0] == '/') ? 1 : 0;
    std::string current;
    for (std::size_t i = start; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '/') {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        } else {
            current += p[i];
        }
    }
    return parts;
}

}  // namespace adb::core
