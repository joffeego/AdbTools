#include "core/store.h"

#include <sstream>

namespace adb::core {

namespace {

// Split a stored line into exactly `fields` tab-separated parts. Returns false
// when there are fewer parts than the format requires, which is how a truncated
// or foreign line gets rejected instead of producing a half-filled record.
bool splitFields(const std::string& line, std::size_t fields, std::vector<std::string>* out) {
    out->clear();
    std::size_t start = 0;
    for (std::size_t i = 0; i + 1 < fields; ++i) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos) return false;
        out->push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    // The final field takes the rest of the line, so a command or a path may
    // itself contain tabs without corrupting the record.
    out->push_back(line.substr(start));
    return true;
}

void forEachLine(const std::string& text, const std::function<void(const std::string&)>& fn) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) fn(line);
    }
}

}  // namespace

std::string serializeBookmarks(const std::vector<Bookmark>& bookmarks) {
    std::string text;
    for (const Bookmark& b : bookmarks) {
        text += b.name + "\t" + b.path + "\n";
    }
    return text;
}

std::vector<Bookmark> parseBookmarks(const std::string& text) {
    std::vector<Bookmark> bookmarks;
    std::vector<std::string> fields;
    forEachLine(text, [&](const std::string& line) {
        if (!splitFields(line, 2, &fields)) return;
        Bookmark b;
        b.name = fields[0];
        b.path = fields[1];
        if (!b.path.empty()) bookmarks.push_back(std::move(b));
    });
    return bookmarks;
}

std::string serializeCommands(const std::vector<CommandEntry>& commands) {
    std::string text;
    for (const CommandEntry& c : commands) {
        text += c.name + "\t" + (c.shell ? "shell" : "cmd") + "\t" + c.command + "\n";
    }
    return text;
}

std::vector<CommandEntry> parseCommands(const std::string& text) {
    std::vector<CommandEntry> commands;
    std::vector<std::string> fields;
    forEachLine(text, [&](const std::string& line) {
        if (!splitFields(line, 3, &fields)) return;
        CommandEntry c;
        c.name = fields[0];
        c.shell = (fields[1] == "shell");
        c.command = fields[2];
        if (!c.name.empty() && !c.command.empty()) commands.push_back(std::move(c));
    });
    return commands;
}

std::string serializeLastPaths(const std::unordered_map<std::string, std::string>& paths) {
    std::string text;
    for (const auto& kv : paths) {
        text += kv.first + "\t" + kv.second + "\n";
    }
    return text;
}

std::unordered_map<std::string, std::string> parseLastPaths(const std::string& text) {
    std::unordered_map<std::string, std::string> paths;
    std::vector<std::string> fields;
    forEachLine(text, [&](const std::string& line) {
        if (!splitFields(line, 2, &fields)) return;
        if (fields[0].empty() || fields[1].empty()) return;
        paths[fields[0]] = fields[1];
    });
    return paths;
}

std::string serializeKeyValues(const std::vector<std::pair<std::string, std::string>>& values) {
    std::string text;
    for (const auto& kv : values) {
        text += kv.first + "=" + kv.second + "\n";
    }
    return text;
}

std::unordered_map<std::string, std::string> parseKeyValues(const std::string& text) {
    std::unordered_map<std::string, std::string> values;
    forEachLine(text, [&](const std::string& line) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) return;
        // The value keeps any further '=' characters; only the first one splits.
        values[line.substr(0, eq)] = line.substr(eq + 1);
    });
    return values;
}

}  // namespace adb::core
