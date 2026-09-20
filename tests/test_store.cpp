// Tests for core/store.h - the on-disk text formats.
//
// These files hold the user's settings, bookmarks, quick commands and recent
// paths. The property that matters is the round trip, so most cases are written
// pair-wise: parse(serialize(x)) must give back exactly x. The parsing is also
// deliberately forgiving (a malformed line is skipped, not fatal), and that is
// checked too, because losing one bookmark must never lose the whole file.
#include "core/store.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tests/test_main.h"

using namespace adb::core;

ADB_TEST(bookmarks_round_trip) {
    const std::vector<Bookmark> in = {
        {"Download", "/sdcard/Download"},
        {"相机", "/sdcard/DCIM/Camera"},
        {"weird", "/sdcard/a b/c\td"},
    };
    const std::vector<Bookmark> out = parseBookmarks(serializeBookmarks(in));
    ADB_CHECK_EQ(out.size(), in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        ADB_CHECK_EQ(out[i].name, in[i].name);
        ADB_CHECK_EQ(out[i].path, in[i].path);
    }
}

ADB_TEST(bookmarks_skip_malformed_lines_but_keep_the_rest) {
    // A line with no tab is dropped; the good lines around it must survive.
    const std::string text =
        "good\t/sdcard/good\n"
        "no tab here\n"
        "\n"
        "also good\t/sdcard/also\n";
    const std::vector<Bookmark> out = parseBookmarks(text);
    ADB_CHECK_EQ(out.size(), static_cast<std::size_t>(2));
    ADB_CHECK_EQ(out[0].name, std::string("good"));
    ADB_CHECK_EQ(out[1].name, std::string("also good"));
}

ADB_TEST(bookmarks_drop_empty_path) {
    // A bookmark pointing nowhere is useless and used to be rejected on load.
    ADB_CHECK_EQ(parseBookmarks("name\t\n").size(), static_cast<std::size_t>(0));
    ADB_CHECK_EQ(parseBookmarks("").size(), static_cast<std::size_t>(0));
}

ADB_TEST(bookmarks_tolerate_crlf) {
    const std::vector<Bookmark> out = parseBookmarks("Download\t/sdcard/Download\r\n");
    ADB_CHECK_EQ(out.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(out[0].path, std::string("/sdcard/Download"));
}

ADB_TEST(bookmarks_keep_tabs_inside_the_path) {
    // Paths on the device can contain a tab; only the first tab splits.
    const std::vector<Bookmark> out = parseBookmarks("n\t/sdcard/a\tb\n");
    ADB_CHECK_EQ(out.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(out[0].path, std::string("/sdcard/a\tb"));
}

ADB_TEST(commands_round_trip) {
    const std::vector<CommandEntry> in = {
        {"重启", true, "reboot"},
        {"查看内存", true, "cat /proc/meminfo"},
        {"打开目录", false, "explorer ."},
    };
    const std::vector<CommandEntry> out = parseCommands(serializeCommands(in));
    ADB_CHECK_EQ(out.size(), in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        ADB_CHECK_EQ(out[i].name, in[i].name);
        ADB_CHECK_EQ(out[i].shell, in[i].shell);
        ADB_CHECK_EQ(out[i].command, in[i].command);
    }
}

ADB_TEST(commands_shell_flag_defaults_to_host_when_not_shell) {
    // Only the literal "shell" means adb shell; anything else is a host command.
    const std::vector<CommandEntry> out = parseCommands("n\tcmd\techo hi\n");
    ADB_CHECK_EQ(out.size(), static_cast<std::size_t>(1));
    ADB_CHECK(!out[0].shell);
    ADB_CHECK_EQ(out[0].command, std::string("echo hi"));
}

ADB_TEST(commands_allow_pipes_and_tabs_in_the_command) {
    const std::vector<CommandEntry> out = parseCommands("n\tshell\tls | grep a\tb\n");
    ADB_CHECK_EQ(out.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(out[0].command, std::string("ls | grep a\tb"));
}

ADB_TEST(commands_skip_malformed_lines) {
    const std::string text =
        "onlyonetab\tvalue\n"          // needs two tabs
        "n\tshell\tcmd\n"              // good
        "\t\tsomething\n"              // empty name -> dropped
        "name\tshell\t\n";             // empty command -> dropped
    const std::vector<CommandEntry> out = parseCommands(text);
    ADB_CHECK_EQ(out.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(out[0].name, std::string("n"));
}

ADB_TEST(last_paths_round_trip) {
    std::unordered_map<std::string, std::string> in = {
        {"UQG5T20616008457", "/sdcard/Download"},
        {"emulator-5554", "/"},
    };
    const auto out = parseLastPaths(serializeLastPaths(in));
    ADB_CHECK_EQ(out.size(), in.size());
    ADB_CHECK_EQ(out.at("UQG5T20616008457"), std::string("/sdcard/Download"));
    ADB_CHECK_EQ(out.at("emulator-5554"), std::string("/"));
}

ADB_TEST(last_paths_skip_incomplete_rows) {
    const auto out = parseLastPaths("serial-only\n\t/sdcard\nok\t/storage\n");
    ADB_CHECK_EQ(out.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(out.at("ok"), std::string("/storage"));
}

ADB_TEST(key_values_round_trip) {
    const std::vector<std::pair<std::string, std::string>> in = {
        {"fontFamily", "Maple Mono NF CN"},
        {"uiScale", "0.800000"},
        {"selectedDevice", "UQG5T20616008457"},
        {"accent", "0.25,0.545,0.945"},
        {"empty", ""},
    };
    const auto out = parseKeyValues(serializeKeyValues(in));
    ADB_CHECK_EQ(out.size(), in.size());
    for (const auto& kv : in) {
        ADB_CHECK_EQ(out.at(kv.first), kv.second);
    }
}

ADB_TEST(key_values_split_on_first_equals_only) {
    // A value may itself contain '=' (e.g. a command line or a query string).
    const auto out = parseKeyValues("cmd=adb shell setprop a=b\n");
    ADB_CHECK_EQ(out.at("cmd"), std::string("adb shell setprop a=b"));
}

ADB_TEST(key_values_ignore_unknown_and_malformed_lines) {
    const auto out = parseKeyValues(
        "known=1\n"
        "novalue\n"          // no '=' -> dropped
        "=orphan\n"          // empty key -> dropped
        "\n"
        "another=2\n");
    ADB_CHECK_EQ(out.size(), static_cast<std::size_t>(2));
    ADB_CHECK(out.count("known") == 1);
    ADB_CHECK(out.count("another") == 1);
    // The point of ignoring unknown keys: an older build must still be able to
    // read a settings file written by a newer one.
    ADB_CHECK(out.count("novalue") == 0);
}

ADB_TEST(key_values_handles_empty_and_crlf) {
    ADB_CHECK_EQ(parseKeyValues("").size(), static_cast<std::size_t>(0));
    const auto out = parseKeyValues("a=1\r\nb=2\r\n");
    ADB_CHECK_EQ(out.size(), static_cast<std::size_t>(2));
    ADB_CHECK_EQ(out.at("b"), std::string("2"));
}

ADB_TEST(key_values_preserve_utf8_and_spaces) {
    const auto out = parseKeyValues("fontFamily=微软雅黑\npath=C:\\Program Files\\x\n");
    ADB_CHECK_EQ(out.at("fontFamily"), std::string("微软雅黑"));
    ADB_CHECK_EQ(out.at("path"), std::string("C:\\Program Files\\x"));
}
