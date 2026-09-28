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

namespace {

// True when every byte belongs to a well-formed UTF-8 sequence. The tests below
// use it to pin down the invariant that matters: truncation must never leave
// half a character behind (that is what turned Chinese file names into a
// replacement glyph).
bool isWellFormedUtf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        std::size_t length = 0;
        if (b < 0x80u) length = 1;
        else if (b >= 0xF0u && b <= 0xF7u) length = 4;
        else if (b >= 0xE0u) length = 3;
        else if (b >= 0xC0u) length = 2;
        if (length == 0 || i + length > s.size()) return false;
        for (std::size_t k = 1; k < length; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0u) != 0x80u) return false;
        }
        i += length;
    }
    return true;
}

}  // namespace

ADB_TEST(displayColumns_counts_wide_characters_as_two) {
    ADB_CHECK_EQ(displayColumns(""), 0);
    ADB_CHECK_EQ(displayColumns("abc"), 3);
    ADB_CHECK_EQ(displayColumns("a b"), 3);
    // CJK ideographs, kana, fullwidth punctuation and fullwidth Latin take two
    // columns; that is what decides whether text still fits in its box.
    ADB_CHECK_EQ(displayColumns("中文"), 4);
    ADB_CHECK_EQ(displayColumns("a中"), 3);
    ADB_CHECK_EQ(displayColumns("，。"), 4);
    ADB_CHECK_EQ(displayColumns("Ａ"), 2);
    ADB_CHECK_EQ(displayColumns("ひらがな"), 8);
    // Halfwidth katakana is a narrow cell, not a wide one.
    ADB_CHECK_EQ(displayColumns("ｱ"), 1);
}

ADB_TEST(displayColumns_survives_broken_utf8) {
    // A stray continuation byte and a sequence cut off by the end of the string
    // each count as one column and advance one byte, so a corrupt name can never
    // desynchronise the count for everything after it.
    ADB_CHECK_EQ(displayColumns("\xFF"), 1);
    ADB_CHECK_EQ(displayColumns("\xE4\xB8"), 2);   // lead byte + orphan tail
    ADB_CHECK_EQ(displayColumns("a\xFF" "b"), 3);
}

ADB_TEST(shortenColumns_keeps_what_fits) {
    ADB_CHECK_EQ(shortenColumns("Download", 20), std::string("Download"));
    ADB_CHECK_EQ(shortenColumns("Download", 8), std::string("Download"));  // exactly fits
    ADB_CHECK_EQ(shortenColumns("Download", 7), std::string("Downlo") + "…");
    // Below 2 there is no room for even the ellipsis, so nothing is shortened.
    ADB_CHECK_EQ(shortenColumns("Download", 1), std::string("Download"));
    ADB_CHECK_EQ(shortenColumns("Download", 0), std::string("Download"));
}

ADB_TEST(shortenColumns_never_splits_a_character) {
    const std::string zh = "微信图片文件夹";  // 7 characters, 14 columns
    ADB_CHECK_EQ(displayColumns(zh), 14);
    ADB_CHECK_EQ(shortenColumns(zh, 7), std::string("微信图") + "…");
    // An odd budget must not leave half a wide character behind.
    ADB_CHECK_EQ(shortenColumns(zh, 6), std::string("微信") + "…");
    ADB_CHECK_EQ(shortenColumns(zh, 14), zh);
    ADB_CHECK(displayColumns(shortenColumns(zh, 5)) <= 5);
}

ADB_TEST(shortenColumns_mixed_width_text_fits_the_budget) {
    const std::string mixed = "Camera 相机";  // 6 + 1 + 4 = 11 columns
    ADB_CHECK_EQ(displayColumns(mixed), 11);
    ADB_CHECK_EQ(shortenColumns(mixed, 8), std::string("Camera ") + "…");
    ADB_CHECK_EQ(shortenColumns(mixed, 11), mixed);

    // Whatever the budget, the result fits in it and stays valid UTF-8.
    const std::string sample = "Download/下载 目录📁";
    for (int limit = 2; limit <= 24; ++limit) {
        const std::string out = shortenColumns(sample, limit);
        ADB_CHECK(displayColumns(out) <= limit);
        ADB_CHECK(isWellFormedUtf8(out));
    }
}

ADB_TEST(shorten_is_byte_based_shortenColumns_is_not) {
    // The contrast that motivated the pair above: the byte-wise helper splits a
    // Chinese character when the offset lands mid-sequence, the column-wise one
    // cannot. Anything holding user-typed text must use the latter.
    const std::string zh = "微信图片";  // 4 characters, 12 bytes
    ADB_CHECK(!isWellFormedUtf8(shorten(zh, 8)));
    ADB_CHECK(isWellFormedUtf8(shortenColumns(zh, 8)));
}

ADB_TEST(shortenColumnsHead_keeps_the_tail_of_a_path) {
    ADB_CHECK_EQ(shortenColumnsHead("/sdcard/DCIM/Camera", 40), std::string("/sdcard/DCIM/Camera"));
    // 12 columns of room: the ellipsis plus "/DCIM/Camera".
    ADB_CHECK_EQ(shortenColumnsHead("/sdcard/DCIM/Camera", 13), std::string("…/DCIM/Camera"));
    ADB_CHECK_EQ(shortenColumnsHead("中文目录/下载", 7), std::string("…/下载"));
    ADB_CHECK_EQ(shortenColumnsHead("/sdcard/DCIM/Camera", 1), std::string("/sdcard/DCIM/Camera"));
    ADB_CHECK(isWellFormedUtf8(shortenColumnsHead("中文目录/下载", 6)));
}
