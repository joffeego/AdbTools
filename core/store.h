// Serialisation of the small text files the app keeps next to its executable:
// settings, bookmarks, quick commands and the per-device recent path.
//
// Only the text formats live here. The file *paths* stay in the GUI layer (they
// depend on executableDir()), and writing goes through core/fileio.h
// (writeFileAtomic) so a crash mid-save cannot destroy the previous contents.
//
// The formats are tab-separated with '\n' line endings, and parsing is
// deliberately forgiving: a malformed line is skipped rather than aborting the
// whole file, so one bad row cannot lose the user's entire configuration.
//
// Round-tripping is the property that matters, and it is what the tests check
// pair-wise: parse(serialise(x)) == x.
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace adb::core {

struct Bookmark {
    std::string name;
    std::string path;
};

struct CommandEntry {
    std::string name;
    bool shell = true;  // true = adb shell, false = host command
    std::string command;
};

// --- bookmarks --------------------------------------------------------------
// "name\tpath"
std::string serializeBookmarks(const std::vector<Bookmark>& bookmarks);
std::vector<Bookmark> parseBookmarks(const std::string& text);

// --- quick commands ---------------------------------------------------------
// "name\tshell|cmd\tcommand"
std::string serializeCommands(const std::vector<CommandEntry>& commands);
std::vector<CommandEntry> parseCommands(const std::string& text);

// --- per-device recent path -------------------------------------------------
// "serial\tpath"
std::string serializeLastPaths(const std::unordered_map<std::string, std::string>& paths);
std::unordered_map<std::string, std::string> parseLastPaths(const std::string& text);

// --- key=value settings -----------------------------------------------------
// "key=value", one per line. Unknown keys are ignored, which is what lets an
// older build read a file written by a newer one.
std::string serializeKeyValues(const std::vector<std::pair<std::string, std::string>>& values);
std::unordered_map<std::string, std::string> parseKeyValues(const std::string& text);

}  // namespace adb::core
