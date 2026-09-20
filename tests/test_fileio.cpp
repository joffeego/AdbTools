// Tests for the atomic file replacement used by every persistent store the app
// has (settings, bookmarks, commands, recent paths).
//
// The failure paths cannot be reached through the GUI, and they are the whole
// point of the helper: a write that fails must leave the previous file intact
// and must not litter the directory with temp files.
#include "core/fileio.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "tests/test_main.h"

using namespace adb::core;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    explicit TempDir(const char* name) {
        std::error_code ec;
        path = fs::temp_directory_path(ec) / name;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace, ec);
        fs::remove_all(path, ec);
    }
    std::string file(const char* name) const { return (path / name).string(); }
};

std::string readAll(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

ADB_TEST(writeFileAtomic_creates_new_file) {
    TempDir dir("adbtools-test-atomic-create");
    const std::string target = dir.file("settings.txt");
    ADB_CHECK(writeFileAtomic(target, "a=1\nb=2\n"));
    ADB_CHECK_EQ(readAll(target), std::string("a=1\nb=2\n"));
    // No .tmp left behind on success.
    ADB_CHECK(!fs::exists(target + ".tmp"));
}

ADB_TEST(writeFileAtomic_replaces_existing_content) {
    TempDir dir("adbtools-test-atomic-replace");
    const std::string target = dir.file("settings.txt");
    ADB_CHECK(writeFileAtomic(target, "old content that is longer\n"));
    ADB_CHECK(writeFileAtomic(target, "new\n"));
    // A truncating write would leave residue from the longer old content.
    ADB_CHECK_EQ(readAll(target), std::string("new\n"));
    ADB_CHECK(!fs::exists(target + ".tmp"));
}

ADB_TEST(writeFileAtomic_keeps_original_when_write_fails) {
    TempDir dir("adbtools-test-atomic-fail");
    const std::string target = dir.file("settings.txt");
    ADB_CHECK(writeFileAtomic(target, "original\n"));

    // A read-only target makes the rename fail; the previous content must
    // survive, because that is the entire reason for writing to a temp file.
    std::error_code ec;
    fs::permissions(target, fs::perms::owner_read, fs::perm_options::replace, ec);
    const bool ok = writeFileAtomic(target, "replacement\n");
    fs::permissions(target, fs::perms::owner_all, fs::perm_options::replace, ec);

    if (ok) {
        // On a filesystem that allows replacing a read-only file the write is
        // legitimate; then the new content must be there in full.
        ADB_CHECK_EQ(readAll(target), std::string("replacement\n"));
    } else {
        ADB_CHECK_EQ(readAll(target), std::string("original\n"));
    }
    ADB_CHECK(!fs::exists(target + ".tmp"));
}

ADB_TEST(writeFileAtomic_missing_directory_fails_without_throwing) {
    TempDir dir("adbtools-test-atomic-nodir");
    const std::string target = (dir.path / "no-such-dir" / "x.txt").string();
    ADB_CHECK(!writeFileAtomic(target, "x"));
    ADB_CHECK(!fs::exists(target));
}

ADB_TEST(writeFileAtomic_round_trips_binary_and_utf8) {
    TempDir dir("adbtools-test-atomic-bytes");
    const std::string target = dir.file("data.bin");
    std::string payload = "key=\xE4\xB8\xAD\xE6\x96\x87\r\nq=\"a\tb\"\n";
    payload.push_back('\0');          // embedded NUL must survive
    payload += "tail\n";
    ADB_CHECK(writeFileAtomic(target, payload));
    ADB_CHECK_EQ(readAll(target), payload);
}

ADB_TEST(writeFileAtomic_empty_content_truncates) {
    TempDir dir("adbtools-test-atomic-empty");
    const std::string target = dir.file("settings.txt");
    ADB_CHECK(writeFileAtomic(target, "something\n"));
    // Writing an empty string is a legitimate "clear everything" operation.
    ADB_CHECK(writeFileAtomic(target, ""));
    ADB_CHECK_EQ(readAll(target), std::string(""));
    ADB_CHECK(fs::exists(target));
}
