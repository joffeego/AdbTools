// Tests for core/package.h - the third-party metadata parsers.
//
// These pin down two bugs that actually shipped:
//
//   1. A brace-counting JSON walk that tripped over GitHub's own URL templates
//      (".../events{/privacy}"), which made the updater attribute the *setup
//      installer's* digest to the portable zip and then refuse to install.
//   2. A platform-tools download URL assembled from the version number
//      ("..._r37.0.1-windows.zip"), which 404s because the real archive is
//      named "-win.zip".
#include "core/package.h"

#include <string>

#include "tests/test_main.h"

using namespace adb::core;

namespace {

// Trimmed-down copy of a real GitHub release payload. Note the uploader object
// before "assets" and the "starred_url"/"events_url" templates: those braces are
// exactly what broke the original parser.
const char* kReleaseJson =
    "{"
    "\"url\":\"https://api.github.com/repos/o/r/releases/1\","
    "\"uploader\":{"
    "\"login\":\"github-actions[bot]\","
    "\"starred_url\":\"https://api.github.com/users/x/starred{/owner}{/repo}\","
    "\"events_url\":\"https://api.github.com/users/x/events{/privacy}\","
    "\"followers_url\":\"https://api.github.com/users/x/followers\""
    "},"
    "\"tag_name\":\"v0.9.8\","
    "\"assets\":["
    "{"
    "\"url\":\"https://api.github.com/repos/o/r/releases/assets/1\","
    "\"name\":\"AdbFileBrowser-windows-x64.zip\","
    "\"content_type\":\"application/zip\","
    "\"size\":16126252,"
    "\"digest\":\"sha256:5e9d704c4760a30edd275c13741a62cedd8fcfda9d38795b5ed6774811d1aa0d\","
    "\"browser_download_url\":\"https://github.com/o/r/releases/download/v0.9.8/AdbFileBrowser-windows-x64.zip\""
    "},"
    "{"
    "\"name\":\"AdbTools-Setup-0.9.8.exe\","
    "\"digest\":\"sha256:9f40208fe56faafe18176863814a967a64a93a2642f43822793842f865628197\","
    "\"browser_download_url\":\"https://github.com/o/r/releases/download/v0.9.8/AdbTools-Setup-0.9.8.exe\""
    "}"
    "]"
    "}";

}  // namespace

ADB_TEST(jsonStringValue_reads_first_occurrence) {
    ADB_CHECK_EQ(jsonStringValue(kReleaseJson, "tag_name"), std::string("v0.9.8"));
    ADB_CHECK_EQ(jsonStringValue(kReleaseJson, "name"), std::string("AdbFileBrowser-windows-x64.zip"));
    // Absent key -> empty, not a crash or a partial match.
    ADB_CHECK_EQ(jsonStringValue(kReleaseJson, "no_such_key"), std::string(""));
    ADB_CHECK_EQ(jsonStringValue("", "name"), std::string(""));
}

ADB_TEST(jsonStringEnd_skips_escaped_quotes) {
    // "a\"b" rest  ->  bytes: 34 97 92 34 98 34 ...
    //                  index:  0  1  2  3  4  5
    // The escaped quote lives at 2..3, so the string really ends at index 5.
    // The assertion is expressed against the bytes rather than a hand-counted
    // literal, and it checks it landed on a quote so it cannot silently pass on
    // the wrong character.
    const std::string json = "\"a\\\"b\" rest";
    const std::size_t end = jsonStringEnd(json, 0);
    ADB_CHECK(end != std::string::npos);
    ADB_CHECK(end < json.size());
    ADB_CHECK_EQ(json[end], '"');
    // Everything between the quotes must be preserved verbatim, backslash and all.
    ADB_CHECK_EQ(json.substr(1, end - 1), std::string("a\\\"b"));

    // No closing quote at all.
    ADB_CHECK_EQ(jsonStringEnd("\"unterminated", 0), std::string::npos);
}

ADB_TEST(jsonObjectEnd_ignores_braces_inside_strings) {
    // A "}" inside a string value is data, not structure. The assertion checks
    // the returned index really is a closing brace and that it is the LAST
    // character, rather than trusting a hand-counted offset.
    const std::string object1 = "{\"a\":\"}{\"}";
    const std::size_t end1 = jsonObjectEnd(object1, 0);
    ADB_CHECK(end1 != std::string::npos);
    ADB_CHECK(end1 < object1.size());
    ADB_CHECK_EQ(object1[end1], '}');
    ADB_CHECK_EQ(end1, object1.size() - 1);

    // Braces inside keys AND values, plus an escaped quote - all of it must be
    // skipped so the object still ends at its own closing brace.
    const std::string object2 = "{\"events{/privacy}\":\"a{b}c\\\"}{\\\"\"}";
    const std::size_t end2 = jsonObjectEnd(object2, 0);
    ADB_CHECK(end2 != std::string::npos);
    ADB_CHECK_EQ(object2[end2], '}');
    ADB_CHECK_EQ(end2, object2.size() - 1);
}

ADB_TEST(jsonObjectEnd_handles_nesting_and_imbalance) {
    ADB_CHECK_EQ(jsonObjectEnd("{\"a\":{\"b\":1}}", 0), std::size_t(12));
    ADB_CHECK_EQ(jsonObjectEnd("{\"a\":1", 0), std::string::npos);
    ADB_CHECK_EQ(jsonObjectEnd("{\"a\":1}", 0), std::size_t(6));
}

ADB_TEST(findAppReleaseAsset_picks_the_zip_not_the_installer) {
    std::string url, row, sha;
    ADB_CHECK(findAppReleaseAsset(kReleaseJson, url, row, sha));
    ADB_CHECK_EQ(url, std::string("https://github.com/o/r/releases/download/v0.9.8/"
                                  "AdbFileBrowser-windows-x64.zip"));
    ADB_CHECK_EQ(row, std::string("AdbFileBrowser-windows-x64.zip"));
    // Must be the ZIP's digest, not the installer's. This is the assertion that
    // catches the brace-counting regression.
    ADB_CHECK_EQ(sha, std::string("5e9d704c4760a30edd275c13741a62cedd8fcfda9d38795b5ed6774811d1aa0d"));
}

ADB_TEST(findAppReleaseAsset_rejects_digest_of_the_wrong_asset) {
    std::string url, row, sha;
    ADB_CHECK(findAppReleaseAsset(kReleaseJson, url, row, sha));
    ADB_CHECK(sha != "9f40208fe56faafe18176863814a967a64a93a2642f43822793842f865628197");
}

ADB_TEST(findAppReleaseAsset_prefers_the_application_zip_over_the_tools_zip) {
    // Releases now carry two archives: the portable application package and the
    // optional adb/scrcpy tools package. Assets arrive in upload order, so the
    // updater must not simply take the first .zip - here the tools package comes
    // first and would otherwise be downloaded and rejected as "no
    // adb_browser.exe in the update package".
    const std::string json =
        "{"
        "\"assets\":["
        "{"
        "\"name\":\"AdbTools-tools-windows-x64.zip\","
        "\"digest\":\"sha256:" + std::string(64, 'a') + "\","
        "\"browser_download_url\":\"https://example.com/AdbTools-tools-windows-x64.zip\""
        "},"
        "{"
        "\"name\":\"AdbFileBrowser-windows-x64.zip\","
        "\"digest\":\"sha256:" + std::string(64, 'b') + "\","
        "\"browser_download_url\":\"https://example.com/AdbFileBrowser-windows-x64.zip\""
        "}"
        "]"
        "}";
    std::string url, row, sha;
    ADB_CHECK(findAppReleaseAsset(json, url, row, sha));
    ADB_CHECK_EQ(url, std::string("https://example.com/AdbFileBrowser-windows-x64.zip"));
    ADB_CHECK_EQ(row, std::string("AdbFileBrowser-windows-x64.zip"));
    // The digest must belong to the package that was selected, not the other one.
    ADB_CHECK_EQ(sha, std::string(64, 'b'));
}

ADB_TEST(findAppReleaseAsset_falls_back_to_the_first_zip_when_no_name_matches) {
    // Releases published before the app archive had its current name (and any
    // future rename) must keep working: without a name match, the first .zip wins,
    // as it did originally.
    const std::string json =
        "{"
        "\"assets\":["
        "{"
        "\"name\":\"bundle.zip\","
        "\"digest\":\"sha256:" + std::string(64, 'c') + "\","
        "\"browser_download_url\":\"https://example.com/bundle.zip\""
        "},"
        "{"
        "\"name\":\"other.zip\","
        "\"digest\":\"sha256:" + std::string(64, 'd') + "\","
        "\"browser_download_url\":\"https://example.com/other.zip\""
        "}"
        "]"
        "}";
    std::string url, row, sha;
    ADB_CHECK(findAppReleaseAsset(json, url, row, sha));
    ADB_CHECK_EQ(url, std::string("https://example.com/bundle.zip"));
    ADB_CHECK_EQ(sha, std::string(64, 'c'));
}

ADB_TEST(findAppReleaseAsset_ignores_a_tools_zip_without_the_app_zip) {
    // A release that carries only the optional tools package has no application
    // package to update from. Reporting that is better than picking the tools zip
    // and failing later with "no adb_browser.exe in the update package".
    const std::string json =
        "{"
        "\"assets\":[{"
        "\"name\":\"AdbTools-tools-windows-x64.zip\","
        "\"browser_download_url\":\"https://example.com/AdbTools-tools-windows-x64.zip\""
        "}]"
        "}";
    std::string url, row, sha;
    ADB_CHECK(!findAppReleaseAsset(json, url, row, sha));
    ADB_CHECK_EQ(url, std::string(""));
}

ADB_TEST(findAppReleaseAsset_survives_escaped_quotes_in_string_values) {
    // A release body or asset label containing an escaped quote must not cut the
    // asset object short: if it did, the parser would read the wrong URL and, in
    // the worst case, attribute one asset's digest to another.
    const std::string json =
        "{"
        "\"body\":\"release \\\"quoted\\\" notes with {braces}\","
        "\"assets\":[{"
        "\"label\":\"a \\\"quoted\\\" label\","
        "\"name\":\"app.zip\","
        "\"digest\":\"sha256:" + std::string("5e9d704c4760a30edd275c13741a62cedd8fcfda9d38795b5ed6774811d1aa0d") + "\","
        "\"browser_download_url\":\"https://example.com/app.zip\""
        "}]"
        "}";
    std::string url, row, sha;
    ADB_CHECK(findAppReleaseAsset(json, url, row, sha));
    ADB_CHECK_EQ(url, std::string("https://example.com/app.zip"));
    ADB_CHECK_EQ(row, std::string("app.zip"));
    ADB_CHECK_EQ(sha.size(), static_cast<std::size_t>(64));
    ADB_CHECK_EQ(sha, std::string("5e9d704c4760a30edd275c13741a62cedd8fcfda9d38795b5ed6774811d1aa0d"));
}

ADB_TEST(findAppReleaseAsset_without_zip_asset_returns_false) {
    // A release that only carries the installer (or nothing at all) must not be
    // offered as a self-update: there is no package to install.
    const std::string onlyInstaller =
        "{\"assets\":[{\"name\":\"Setup.exe\","
        "\"browser_download_url\":\"https://example.com/Setup.exe\"}]}";
    std::string url, row, sha;
    ADB_CHECK(!findAppReleaseAsset(onlyInstaller, url, row, sha));
    ADB_CHECK_EQ(url, std::string(""));

    ADB_CHECK(!findAppReleaseAsset("{\"assets\":[]}", url, row, sha));
    ADB_CHECK(!findAppReleaseAsset("{}", url, row, sha));
    ADB_CHECK(!findAppReleaseAsset("", url, row, sha));
}

ADB_TEST(findAppReleaseAsset_zip_without_digest_reports_empty_sha) {
    const std::string json =
        "{\"assets\":[{\"name\":\"app.zip\","
        "\"browser_download_url\":\"https://example.com/app.zip\"}]}";
    std::string url, row, sha;
    ADB_CHECK(findAppReleaseAsset(json, url, row, sha));
    ADB_CHECK_EQ(url, std::string("https://example.com/app.zip"));
    ADB_CHECK_EQ(row, std::string("app.zip"));
    // No digest published -> the caller must fall back to the .sha256 sidecar
    // and treat the download as unverified only if that is missing too.
    ADB_CHECK_EQ(sha, std::string(""));
}

ADB_TEST(findAppReleaseAsset_ignores_non_sha256_digest_format) {
    const std::string json =
        "{\"assets\":[{\"name\":\"app.zip\",\"digest\":\"sha512:deadbeef\","
        "\"browser_download_url\":\"https://example.com/app.zip\"}]}";
    std::string url, row, sha;
    ADB_CHECK(findAppReleaseAsset(json, url, row, sha));
    ADB_CHECK_EQ(sha, std::string(""));
}

ADB_TEST(findWin64AssetUrl_locates_scrcpy_asset) {
    const std::string json =
        "{\"assets\":["
        "{\"name\":\"scrcpy-win64-v4.1.zip\","
        "\"browser_download_url\":\"https://github.com/Genymobile/scrcpy/releases/download/v4.1/scrcpy-win64-v4.1.zip\"},"
        "{\"name\":\"scrcpy-server-v4.1\","
        "\"browser_download_url\":\"https://github.com/Genymobile/scrcpy/releases/download/v4.1/scrcpy-server-v4.1\"}"
        "]}";
    ADB_CHECK_EQ(findWin64AssetUrl(json),
                 std::string("https://github.com/Genymobile/scrcpy/releases/download/v4.1/"
                             "scrcpy-win64-v4.1.zip"));
}

ADB_TEST(findWin64AssetUrl_absent) {
    ADB_CHECK_EQ(findWin64AssetUrl("{}"), std::string(""));
    ADB_CHECK_EQ(findWin64AssetUrl("{\"assets\":[]}"), std::string(""));
    ADB_CHECK_EQ(findWin64AssetUrl(""), std::string(""));
}

ADB_TEST(findAssetSha256_matches_only_the_requested_url) {
    const std::string zipUrl = "https://github.com/o/r/releases/download/v0.9.8/"
                               "AdbFileBrowser-windows-x64.zip";
    ADB_CHECK_EQ(findAssetSha256(kReleaseJson, zipUrl),
                 std::string("5e9d704c4760a30edd275c13741a62cedd8fcfda9d38795b5ed6774811d1aa0d"));
    // A different URL (the installer) must not borrow the zip's digest.
    ADB_CHECK_EQ(findAssetSha256(kReleaseJson, "https://github.com/o/r/releases/download/v0.9.8/"
                                               "AdbTools-Setup-0.9.8.exe"),
                 std::string(""));
    ADB_CHECK_EQ(findAssetSha256(kReleaseJson, ""), std::string(""));
}

ADB_TEST(xmlValue_extracts_tag_text) {
    ADB_CHECK_EQ(xmlValue("<major>37</major>", "major"), std::string("37"));
    ADB_CHECK_EQ(xmlValue("<a><b>x</b></a>", "b"), std::string("x"));
    ADB_CHECK_EQ(xmlValue("<a>  spaced  </a>", "a"), std::string("spaced"));
    ADB_CHECK_EQ(xmlValue("<a>1</a>", "missing"), std::string(""));
    ADB_CHECK_EQ(xmlValue("<a>unclosed", "a"), std::string(""));
}

namespace {

// Trimmed-down copy of Google's repository2-1.xml platform-tools entry. The
// linux archive comes first on purpose: an implementation that just takes the
// first <archive> would hand back the wrong file.
const char* kRepoXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<sdk-repository>"
    "<remotePackage path=\"platform-tools\">"
    "<type-details xsi:type=\"generic:genericDetailsType\"/>"
    "<revision><major>37</major><minor>0</minor><micro>1</micro></revision>"
    "<display-name>Android SDK Platform-Tools</display-name>"
    "<archives>"
    "<archive><complete><size>9054187</size>"
    "<checksum>477254aa5f903c15cf51001717bdf347fb6b53e0</checksum>"
    "<url>platform-tools_r37.0.1-linux.zip</url></complete><host-os>linux</host-os></archive>"
    "<archive><complete><size>16110554</size>"
    "<checksum>6ae73f4de6452dc57e62ec02b68eed92a4c21661</checksum>"
    "<url>platform-tools_r37.0.1-darwin.zip</url></complete><host-os>macosx</host-os></archive>"
    "<archive><complete><size>8044989</size>"
    "<checksum>e03e78b1d80b396f1c3358e31251cb31740e1110</checksum>"
    "<url>platform-tools_r37.0.1-win.zip</url></complete><host-os>windows</host-os></archive>"
    "</archives>"
    "</remotePackage>"
    "<remotePackage path=\"emulator\"><revision><major>1</major></revision></remotePackage>"
    "</sdk-repository>";

}  // namespace

ADB_TEST(parsePlatformToolsVersion_reads_revision) {
    ADB_CHECK_EQ(parsePlatformToolsVersion(kRepoXml), std::string("37.0.1"));
}

ADB_TEST(parsePlatformToolsVersion_missing_pieces) {
    ADB_CHECK_EQ(parsePlatformToolsVersion(""), std::string(""));
    ADB_CHECK_EQ(parsePlatformToolsVersion("<sdk-repository/>"), std::string(""));
    // Package present but with no revision block.
    ADB_CHECK_EQ(parsePlatformToolsVersion(
                     "<remotePackage path=\"platform-tools\"></remotePackage>"),
                 std::string(""));
    // major only.
    ADB_CHECK_EQ(parsePlatformToolsVersion(
                     "<remotePackage path=\"platform-tools\"><revision><major>5</major></revision>"
                     "</remotePackage>"),
                 std::string("5"));
}

ADB_TEST(parsePlatformToolsWindowsArchive_picks_windows_entry) {
    std::string url, checksum;
    parsePlatformToolsWindowsArchive(kRepoXml, url, checksum);
    // Must be "-win.zip". Assembling "-windows.zip" from the version is the bug
    // this test exists to prevent from coming back.
    ADB_CHECK_EQ(url, std::string("https://dl.google.com/android/repository/"
                                  "platform-tools_r37.0.1-win.zip"));
    ADB_CHECK_EQ(checksum, std::string("e03e78b1d80b396f1c3358e31251cb31740e1110"));
    ADB_CHECK_EQ(checksum.size(), static_cast<std::size_t>(40));  // SHA-1, not SHA-256
}

ADB_TEST(parsePlatformToolsWindowsArchive_without_windows_entry) {
    const std::string noWindows =
        "<remotePackage path=\"platform-tools\"><archives>"
        "<archive><complete><url>platform-tools_r37.0.1-linux.zip</url></complete>"
        "<host-os>linux</host-os></archive>"
        "</archives></remotePackage>";
    std::string url, checksum;
    parsePlatformToolsWindowsArchive(noWindows, url, checksum);
    ADB_CHECK_EQ(url, std::string(""));
    ADB_CHECK_EQ(checksum, std::string(""));
}

ADB_TEST(parsePlatformToolsWindowsArchive_clears_outputs_first) {
    // Callers reuse the out-params across refreshes; a stale URL must never
    // survive a failed parse.
    std::string url = "stale";
    std::string checksum = "stale";
    parsePlatformToolsWindowsArchive("", url, checksum);
    ADB_CHECK_EQ(url, std::string(""));
    ADB_CHECK_EQ(checksum, std::string(""));
}

ADB_TEST(parsePlatformToolsWindowsArchive_ignores_other_packages) {
    // platform-tools absent, but another package ships a windows archive.
    const std::string other =
        "<remotePackage path=\"emulator\"><archives>"
        "<archive><complete><url>emulator-win.zip</url></complete>"
        "<host-os>windows</host-os></archive>"
        "</archives></remotePackage>";
    std::string url, checksum;
    parsePlatformToolsWindowsArchive(other, url, checksum);
    ADB_CHECK_EQ(url, std::string(""));
}
