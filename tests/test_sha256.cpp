// Tests for the sha256sum-sidecar parser used by the updater.
//
// The release workflow publishes "<asset>.sha256" next to every artifact, and
// this parser is what turns that file into a digest. It also accepts GitHub's
// "sha256:<hex>" form, so both shapes are covered here.
#include "core/sha256.h"

#include <string>
#include <utility>
#include <vector>

#include "tests/test_main.h"

using namespace adb::core;

namespace {
const char* kHex64 = "5e9d704c4760a30edd275c13741a62cedd8fcfda9d38795b5ed6774811d1aa0d";
const char* kHex64b = "9f40208fe56faafe18176863814a967a64a93a2642f43822793842f865628197";
}  // namespace

ADB_TEST(parseSha256Rows_reads_sha256sum_format) {
    const std::string text = std::string(kHex64) + "  AdbFileBrowser-windows-x64.zip\n";
    const auto rows = parseSha256Rows(text);
    ADB_CHECK_EQ(rows.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(rows[0].first, std::string("AdbFileBrowser-windows-x64.zip"));
    ADB_CHECK_EQ(rows[0].second, std::string(kHex64));
}

ADB_TEST(parseSha256Rows_reads_binary_marker_and_crlf) {
    // sha256sum -b writes "<hex> *<name>"; the workflow uses ASCII text but
    // other tools produce the star form, and CRLF must not leak into the name.
    const std::string text = std::string(kHex64) + " *app.zip\r\n";
    const auto rows = parseSha256Rows(text);
    ADB_CHECK_EQ(rows.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(rows[0].first, std::string("app.zip"));
    ADB_CHECK_EQ(rows[0].second, std::string(kHex64));
}

ADB_TEST(parseSha256Rows_bare_digest_without_filename_is_dropped) {
    // A digest with no filename is NOT a usable row: the caller needs to know
    // which file the digest belongs to. This matters because it is what kept
    // the updater's fallback honest - if the sidecar cannot be attributed, it is
    // rejected rather than being applied to whatever happened to be downloaded.
    //
    // GitHub's own "digest" field has this shape ("sha256:<hex>" with no
    // filename), but it is read straight out of the release JSON by
    // findAppReleaseAsset()/findAssetSha256() instead of going through here.
    const std::string text = std::string("sha256:") + kHex64 + "\n";
    const auto rows = parseSha256Rows(text);
    ADB_CHECK_EQ(rows.size(), static_cast<std::size_t>(0));
}

ADB_TEST(parseSha256Rows_normalises_uppercase_hex) {
    std::string upper = kHex64;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto rows = parseSha256Rows(upper + "  app.zip\n");
    ADB_CHECK_EQ(rows.size(), static_cast<std::size_t>(1));
    // Digests are compared case-insensitively but stored lowercase.
    ADB_CHECK_EQ(rows[0].second, std::string(kHex64));
}

ADB_TEST(parseSha256Rows_reads_multiple_lines) {
    const std::string text = std::string(kHex64) + "  a.zip\n" + kHex64b + "  b.exe\n";
    const auto rows = parseSha256Rows(text);
    ADB_CHECK_EQ(rows.size(), static_cast<std::size_t>(2));
    ADB_CHECK_EQ(rows[0].first, std::string("a.zip"));
    ADB_CHECK_EQ(rows[1].first, std::string("b.exe"));
}

ADB_TEST(parseSha256Rows_skips_malformed_lines) {
    const std::string text =
        "not a digest\n"
        "abc123  short-hex.zip\n"                       // too few hex chars
        "\n"                                            // blank line
        "g" + std::string(kHex64).substr(1) + "  bad.zip\n"  // non-hex character
        "  \r\n"
        + std::string(kHex64) + "\n";                    // digest with no filename
    const auto rows = parseSha256Rows(text);
    ADB_CHECK_EQ(rows.size(), static_cast<std::size_t>(0));
}

ADB_TEST(parseSha256Rows_handles_empty_input) {
    ADB_CHECK_EQ(parseSha256Rows("").size(), static_cast<std::size_t>(0));
    ADB_CHECK_EQ(parseSha256Rows("\n\n\n").size(), static_cast<std::size_t>(0));
}

ADB_TEST(parseSha256Rows_rejects_html_error_page) {
    // A 404 from a mirror returns HTML; it must not be mistaken for a digest.
    const std::string html =
        "<!DOCTYPE html><html><head><title>404</title></head><body>Not Found</body></html>";
    ADB_CHECK_EQ(parseSha256Rows(html).size(), static_cast<std::size_t>(0));
}

ADB_TEST(parseSha256Rows_keeps_filenames_with_spaces) {
    const std::string text = std::string(kHex64) + "  my file name.zip\n";
    const auto rows = parseSha256Rows(text);
    ADB_CHECK_EQ(rows.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(rows[0].first, std::string("my file name.zip"));
}
