// Tests for core/strings.h - the string helpers the file list and the updater
// both rely on, including the ones that format sizes and parse `ls -la` data.
#include "core/strings.h"

#include <string>
#include <vector>

#include "tests/test_main.h"

using namespace adb::core;

ADB_TEST(trim_removes_surrounding_whitespace) {
    ADB_CHECK_EQ(trim("  hello  "), std::string("hello"));
    ADB_CHECK_EQ(trim("\t\r\n x \n"), std::string("x"));
}

ADB_TEST(trim_keeps_inner_whitespace_and_handles_empty) {
    ADB_CHECK_EQ(trim("a  b"), std::string("a  b"));
    ADB_CHECK_EQ(trim(""), std::string(""));
    ADB_CHECK_EQ(trim("    "), std::string(""));
}

ADB_TEST(splitWs_splits_on_any_whitespace_run) {
    ADB_CHECK_EQ(splitWs("  a\tb\n c ").size(), static_cast<std::size_t>(3));
    ADB_CHECK_EQ(ADB_AT(splitWs("a  b"), 1), std::string("b"));
    ADB_CHECK_EQ(splitWs("").size(), static_cast<std::size_t>(0));
    ADB_CHECK_EQ(splitWs("   ").size(), static_cast<std::size_t>(0));
}

ADB_TEST(lower_is_ascii_only) {
    ADB_CHECK_EQ(lower("AbC-123"), std::string("abc-123"));
    // UTF-8 bytes must survive untouched (this is Chinese text in the UI).
    const std::string utf8 = "\xE4\xB8\xAD\xE6\x96\x87";  // 中文
    ADB_CHECK_EQ(lower(utf8), utf8);
}

ADB_TEST(shorten_boundaries) {
    ADB_CHECK_EQ(shorten("abcdef", 10), std::string("abcdef"));   // fits
    ADB_CHECK_EQ(shorten("abcdef", 6), std::string("abcdef"));    // exactly fits
    ADB_CHECK_EQ(shorten("abcdefghij", 7), std::string("abcd..."));
    // limit < 4 disables shortening entirely.
    ADB_CHECK_EQ(shorten("abcdef", 3), std::string("abcdef"));
    ADB_CHECK_EQ(shorten("abcdef", 0), std::string("abcdef"));
}

ADB_TEST(formatSize_bytes_and_units) {
    ADB_CHECK_EQ(formatSize(0), std::string("0 B"));
    ADB_CHECK_EQ(formatSize(512), std::string("512 B"));
    ADB_CHECK_EQ(formatSize(1024), std::string("1.0 KB"));
    ADB_CHECK_EQ(formatSize(1536), std::string("1.5 KB"));
    ADB_CHECK_EQ(formatSize(1024LL * 1024), std::string("1.0 MB"));
    ADB_CHECK_EQ(formatSize(1024LL * 1024 * 1024), std::string("1.0 GB"));
}

ADB_TEST(formatSize_negative_is_empty) {
    // A negative size is not a size; the UI shows a dash instead.
    ADB_CHECK_EQ(formatSize(-1), std::string(""));
    ADB_CHECK_EQ(formatSize(-123456), std::string(""));
}

ADB_TEST(isMonthName_matches_ls_output) {
    ADB_CHECK(isMonthName("Jan"));
    ADB_CHECK(isMonthName("Dec"));
    ADB_CHECK(!isMonthName("jan"));   // ls always prints capitalized
    ADB_CHECK(!isMonthName("January"));
    ADB_CHECK(!isMonthName(""));
    ADB_CHECK(!isMonthName("Xyz"));
}

ADB_TEST(parseSize_is_strict) {
    ADB_CHECK_EQ(parseSize("0"), 0LL);
    ADB_CHECK_EQ(parseSize("12345"), 12345LL);
    ADB_CHECK_EQ(parseSize("9223372036854775807"), 9223372036854775807LL);  // LLONG_MAX
    // Anything that is not purely digits must be rejected, not partially parsed.
    ADB_CHECK_EQ(parseSize("12a"), 0LL);
    ADB_CHECK_EQ(parseSize(" 12"), 0LL);
    ADB_CHECK_EQ(parseSize("+12"), 0LL);
    ADB_CHECK_EQ(parseSize("-12"), 0LL);
    ADB_CHECK_EQ(parseSize(""), 0LL);
    ADB_CHECK_EQ(parseSize("1.5"), 0LL);
}
