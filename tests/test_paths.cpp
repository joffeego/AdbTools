// Tests for core/paths.h - the device-side path helpers.
//
// joinPath feeds every adb command the app issues, and shellQuote is what keeps
// filenames with spaces or quotes from being re-interpreted by the device
// shell, so both are worth pinning down precisely.
#include "core/paths.h"

#include <string>

#include "tests/test_main.h"

using namespace adb::core;

ADB_TEST(shellQuote_wraps_in_single_quotes) {
    ADB_CHECK_EQ(shellQuote("/sdcard/a.txt"), std::string("'/sdcard/a.txt'"));
    ADB_CHECK_EQ(shellQuote(""), std::string("''"));
}

ADB_TEST(shellQuote_escapes_embedded_single_quote) {
    // The only character that cannot appear literally inside '...' is ' itself;
    // it has to be closed, escaped and reopened.
    ADB_CHECK_EQ(shellQuote("it's"), std::string("'it'\\''s'"));
    ADB_CHECK_EQ(shellQuote("'"), std::string("''\\'''"));
}

ADB_TEST(shellQuote_leaves_shell_metacharacters_inert) {
    // These are the characters that would otherwise be interpreted by the
    // device shell; all of them must end up inside the quotes.
    ADB_CHECK_EQ(shellQuote("a b;c"), std::string("'a b;c'"));
    ADB_CHECK_EQ(shellQuote("$(rm -rf /)"), std::string("'$(rm -rf /)'"));
    ADB_CHECK_EQ(shellQuote("a|b&c>d"), std::string("'a|b&c>d'"));
    ADB_CHECK_EQ(shellQuote("*.txt"), std::string("'*.txt'"));
    ADB_CHECK_EQ(shellQuote("中文 名.txt"), std::string("'中文 名.txt'"));
}

ADB_TEST(joinPath_handles_bases) {
    ADB_CHECK_EQ(joinPath("/sdcard", "a.txt"), std::string("/sdcard/a.txt"));
    ADB_CHECK_EQ(joinPath("/sdcard/", "a.txt"), std::string("/sdcard/a.txt"));
    // A "/" base must not produce "//name".
    ADB_CHECK_EQ(joinPath("/", "a.txt"), std::string("/a.txt"));
    // An empty base is treated like the root.
    ADB_CHECK_EQ(joinPath("", "a.txt"), std::string("/a.txt"));
}

ADB_TEST(parentPath_walks_up) {
    ADB_CHECK_EQ(parentPath("/sdcard/a.txt"), std::string("/sdcard"));
    ADB_CHECK_EQ(parentPath("/sdcard/dir/a.txt"), std::string("/sdcard/dir"));
    // Never returns an empty string: the root is its own parent.
    ADB_CHECK_EQ(parentPath("/sdcard"), std::string("/"));
    ADB_CHECK_EQ(parentPath("/"), std::string("/"));
    ADB_CHECK_EQ(parentPath(""), std::string("/"));
}

ADB_TEST(parentPath_ignores_trailing_slashes) {
    ADB_CHECK_EQ(parentPath("/sdcard/dir/"), std::string("/sdcard"));
    ADB_CHECK_EQ(parentPath("/sdcard///"), std::string("/"));
    ADB_CHECK_EQ(parentPath("relative"), std::string("/"));
}

ADB_TEST(splitPath_produces_components) {
    ADB_CHECK_EQ(splitPath("/sdcard/a.txt").size(), static_cast<std::size_t>(2));
    ADB_CHECK_EQ(splitPath("/sdcard/a.txt")[0], std::string("sdcard"));
    ADB_CHECK_EQ(splitPath("/sdcard/a.txt")[1], std::string("a.txt"));
    ADB_CHECK_EQ(splitPath("/").size(), static_cast<std::size_t>(0));
    ADB_CHECK_EQ(splitPath("").size(), static_cast<std::size_t>(0));
    // Double slashes and a trailing slash must not create empty components.
    ADB_CHECK_EQ(splitPath("/a//b/").size(), static_cast<std::size_t>(2));
    ADB_CHECK_EQ(splitPath("a/b")[0], std::string("a"));
}
