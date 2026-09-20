// Tests for core/version.h.
//
// compareVersions decides whether the app offers an update, so its edge cases
// (different component counts, suffixes, garbage) are worth locking down.
#include "core/version.h"

#include <string>
#include <vector>

#include "tests/test_main.h"

using namespace adb::core;

ADB_TEST(parseVersionParts_splits_on_dots) {
    ADB_CHECK_EQ(parseVersionParts("0.9.8"), (std::vector<int>{0, 9, 8}));
    ADB_CHECK_EQ(parseVersionParts("37.0.1"), (std::vector<int>{37, 0, 1}));
    ADB_CHECK_EQ(parseVersionParts("4"), (std::vector<int>{4}));
}

ADB_TEST(parseVersionParts_treats_dash_and_underscore_as_separators) {
    ADB_CHECK_EQ(parseVersionParts("1-2-3"), (std::vector<int>{1, 2, 3}));
    ADB_CHECK_EQ(parseVersionParts("1_2"), (std::vector<int>{1, 2}));
}

ADB_TEST(parseVersionParts_stops_at_anything_else) {
    // A trailing space ends the version; the rest is not a version at all.
    ADB_CHECK_EQ(parseVersionParts("1.2 "), (std::vector<int>{1, 2}));
    ADB_CHECK_EQ(parseVersionParts("1.2-beta"), (std::vector<int>{1, 2}));
    ADB_CHECK_EQ(parseVersionParts(""), (std::vector<int>{}));
    ADB_CHECK_EQ(parseVersionParts("v"), (std::vector<int>{}));
    ADB_CHECK_EQ(parseVersionParts("."), (std::vector<int>{}));
}

ADB_TEST(compareVersions_orders_numerically_not_lexically) {
    // The classic trap: "0.9.10" < "0.9.9" as strings but not as versions.
    ADB_CHECK(compareVersions("0.9.9", "0.9.10") < 0);
    ADB_CHECK(compareVersions("0.9.10", "0.9.9") > 0);
    ADB_CHECK(compareVersions("0.10.0", "0.9.9") > 0);
    ADB_CHECK(compareVersions("1.0.0", "0.99.99") > 0);
}

ADB_TEST(compareVersions_equal_and_missing_components) {
    ADB_CHECK_EQ(compareVersions("1.2.3", "1.2.3"), 0);
    // A missing component counts as zero.
    ADB_CHECK_EQ(compareVersions("1.2", "1.2.0"), 0);
    ADB_CHECK_EQ(compareVersions("1.2.0", "1.2"), 0);
    ADB_CHECK(compareVersions("1.2", "1.2.1") < 0);
}

ADB_TEST(compareVersions_garbage_is_treated_as_zero) {
    // An unparsable current version must not look newer than a real one.
    ADB_CHECK(compareVersions("", "0.9.8") < 0);
    ADB_CHECK(compareVersions("unknown", "0.9.8") < 0);
    ADB_CHECK_EQ(compareVersions("", ""), 0);
}

ADB_TEST(adbShortVersion_reads_version_from_output) {
    const std::string out =
        "Android Debug Bridge version 1.0.41\n"
        "Version 37.0.0-14910828\n"
        "Installed as C:\\tools\\adb.exe\n";
    // The first version-looking number wins, and the word "version" is matched
    // case-insensitively so the lowercase line above is found as well.
    ADB_CHECK_EQ(adbShortVersion(out), std::string("1.0.41"));
}

ADB_TEST(adbShortVersion_reads_capitalised_version_line) {
    // Some adb builds only expose the platform-tools revision on this line.
    ADB_CHECK_EQ(adbShortVersion("Version 37.0.0-14910828\n"), std::string("37.0.0"));
    ADB_CHECK_EQ(adbShortVersion("Android Debug Bridge version 1.0.41\n"), std::string("1.0.41"));
}

ADB_TEST(adbShortVersion_without_version_returns_empty) {
    ADB_CHECK_EQ(adbShortVersion(""), std::string(""));
    ADB_CHECK_EQ(adbShortVersion("error: no devices found\n"), std::string(""));
    // "Version " present but with no digits after it.
    ADB_CHECK_EQ(adbShortVersion("Version unknown\n"), std::string(""));
}

ADB_TEST(adbShortVersion_handles_installed_as_line) {
    // Real adb output puts the interesting number on the second line.
    ADB_CHECK_EQ(adbShortVersion("Android Debug Bridge version 1.0.41\n"), std::string("1.0.41"));
}

ADB_TEST(scrcpyShortVersion_reads_version) {
    ADB_CHECK_EQ(scrcpyShortVersion("scrcpy 4.1 <https://github.com/Genymobile/scrcpy>"),
                 std::string("4.1"));
    ADB_CHECK_EQ(scrcpyShortVersion("scrcpy v3.1.2"), std::string("3.1.2"));
    ADB_CHECK_EQ(scrcpyShortVersion("scrcpy 4.1\n"), std::string("4.1"));
}

ADB_TEST(scrcpyShortVersion_absent_or_malformed) {
    ADB_CHECK_EQ(scrcpyShortVersion(""), std::string(""));
    ADB_CHECK_EQ(scrcpyShortVersion("error: not found"), std::string(""));
    // "scrcpy" without a following version.
    ADB_CHECK_EQ(scrcpyShortVersion("scrcpy"), std::string(""));
    ADB_CHECK_EQ(scrcpyShortVersion("scrcpy (unknown)"), std::string(""));
}
