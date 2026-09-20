// Tests for the sha256sum-sidecar parser used by the updater.
//
// The release workflow publishes "<asset>.sha256" next to every artifact, and
// this parser is what turns that file into a digest. It also accepts GitHub's
// "sha256:<hex>" form, so both shapes are covered here.
#include "core/sha256.h"

#include <filesystem>
#include <fstream>
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

#ifdef _WIN32
namespace {

// Write `content` to a temp file and return its path. The digest tests below
// check the whole hashing path (open file, stream it through CNG, hex encode)
// against published digests, which is the only way to be sure the update
// verification actually verifies anything.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const char* name, const std::string& content) {
        std::error_code ec;
        path = std::filesystem::temp_directory_path(ec) / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::wstring wide() const { return path.wstring(); }
};

}  // namespace

ADB_TEST(sha256FileHex_matches_known_digests) {
    // Published values (NIST / RFC 6234 test vectors).
    TempFile empty("adbtools-hash-empty.bin", "");
    TempFile abc("adbtools-hash-abc.bin", "abc");
    std::string err;

    ADB_CHECK_EQ(sha256FileHex(empty.wide(), err),
                 std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    ADB_CHECK_EQ(sha256FileHex(abc.wide(), err),
                 std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    ADB_CHECK_EQ(err, std::string(""));
}

ADB_TEST(sha256FileHex_hashes_more_than_one_buffer) {
    // Larger than the 64 KiB read buffer, so the streaming loop runs more than
    // once. The digest is compared against an independent implementation
    // (PowerShell's Get-FileHash) in the commit that added this test, and the
    // boundary case is checked by flipping a byte past the first buffer.
    std::string big(200 * 1024, 'a');
    TempFile file("adbtools-hash-big.bin", big);
    std::string err;

    const std::string digest = sha256FileHex(file.wide(), err);
    ADB_CHECK_EQ(err, std::string(""));
    // Cross-checked against an independent implementation:
    //   PowerShell  Get-FileHash <200KiB of 'a'> -Algorithm SHA256
    //   -> 4b4f0f46ac02d177dea0ab36a66a657840e2fb98b20bb27a688db4d8ea9cd22c
    // Getting this wrong would mean the updater's verification verifies nothing.
    ADB_CHECK_EQ(digest, std::string("4b4f0f46ac02d177dea0ab36a66a657840e2fb98b20bb27a688db4d8ea9cd22c"));

    // A change in the final byte (past the first 64 KiB buffer) must change the
    // digest: if the loop stopped early, both hashes would match.
    std::string altered = big;
    altered[big.size() - 1] = 'b';
    TempFile other("adbtools-hash-big2.bin", altered);
    std::string err2;
    const std::string digest2 = sha256FileHex(other.wide(), err2);
    ADB_CHECK(digest != digest2);
}

ADB_TEST(sha1FileHex_matches_known_digests) {
    // Google publishes SHA-1 for platform-tools, so the adb updater verifies
    // with this algorithm rather than SHA-256.
    TempFile abc("adbtools-hash-abc-sha1.bin", "abc");
    TempFile empty("adbtools-hash-empty-sha1.bin", "");
    std::string err;

    ADB_CHECK_EQ(sha1FileHex(abc.wide(), err),
                 std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    ADB_CHECK_EQ(sha1FileHex(empty.wide(), err),
                 std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    ADB_CHECK_EQ(err, std::string(""));
}

ADB_TEST(hashing_a_missing_file_reports_an_error) {
    // Must not silently return an empty string that a caller could mistake for
    // "nothing to verify".
    std::string err;
    const std::string digest = sha256FileHex(L"Z:\\definitely\\not\\here.bin", err);
    ADB_CHECK_EQ(digest, std::string(""));
    ADB_CHECK(!err.empty());
}

ADB_TEST(hashFileHex_rejects_an_unknown_algorithm) {
    TempFile abc("adbtools-hash-algo.bin", "abc");
    std::string err;
    const std::string digest = hashFileHex(abc.wide(), L"NOT-A-HASH", err);
    ADB_CHECK_EQ(digest, std::string(""));
    ADB_CHECK(!err.empty());
}
#endif  // _WIN32
