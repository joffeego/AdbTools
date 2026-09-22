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

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

// -----------------------------------------------------------------------------
// isRegularFileNoThrow
//
// This exists because scanning PATH with std::filesystem aborted the GUI: a PATH
// entry with non-ASCII characters holds ANSI bytes (GBK on a Chinese Windows),
// libstdc++ builds a std::filesystem::path from narrow strings as UTF-8, and the
// conversion error that produces is fatal in a translation unit compiled with
// -fno-exceptions. These cases pin down the "must never throw" contract.
// -----------------------------------------------------------------------------

ADB_TEST(isRegularFileNoThrow_true_for_a_regular_file) {
    TempDir dir("adbtools-test-isfile-yes");
    const std::string target = dir.file("scrcpy.exe");
    ADB_CHECK(writeFileAtomic(target, "x"));
    ADB_CHECK(isRegularFileNoThrow(target));
}

ADB_TEST(isRegularFileNoThrow_false_for_a_directory) {
    TempDir dir("adbtools-test-isfile-dir");
    // The candidate list contains "<dir>/scrcpy", which names a *directory* when
    // scrcpy is unpacked; treating that as the executable made scrcpy fail to
    // launch with a confusing error.
    ADB_CHECK(!isRegularFileNoThrow(dir.path.string()));
    ADB_CHECK(!isRegularFileNoThrow("."));
}

ADB_TEST(isRegularFileNoThrow_false_for_missing_and_empty) {
    TempDir dir("adbtools-test-isfile-missing");
    ADB_CHECK(!isRegularFileNoThrow(dir.file("no-such-file.exe")));
    ADB_CHECK(!isRegularFileNoThrow(""));
}

ADB_TEST(isRegularFileNoThrow_survives_a_non_utf8_path) {
    // GBK bytes for a real directory name, exactly as they appear in the process
    // environment on a Chinese Windows. Constructing a std::filesystem::path from
    // this string throws, which is what aborted the app; the helper must simply
    // report "not a file".
    const std::string gbk =
        "C:\\Program Files (x86)\\Tencent\\\xCE\xA2\xD0\xC5web\xB0\xB2\xC8\xAB\xB9\xDC\xBC\xD2\\scrcpy.exe";
    bool threw = false;
    try {
        (void)isRegularFileNoThrow(gbk);
    } catch (...) {
        threw = true;
    }
    ADB_CHECK(!threw);
    ADB_CHECK(!isRegularFileNoThrow(gbk));
}

ADB_TEST(isRegularFileNoThrow_survives_embedded_nul_and_junk) {
    // Environment entries are attacker-adjacent input as far as this code is
    // concerned: whatever PATH contains must not be able to kill the process.
    std::string odd = "C:\\temp\\x";
    odd.push_back('\0');
    odd += "y\\scrcpy.exe";
    bool threw = false;
    try {
        (void)isRegularFileNoThrow(odd);
        (void)isRegularFileNoThrow(std::string("\xFF\xFE\xFD"));
        (void)isRegularFileNoThrow(std::string(4096, 'A'));
    } catch (...) {
        threw = true;
    }
    ADB_CHECK(!threw);
}

ADB_TEST(writeFileAtomic_survives_a_non_utf8_path) {
    // The same hazard applies to the settings/bookmark/command writers: they are
    // called from the GUI's -fno-exceptions code, so a throw here aborts the app
    // rather than failing the write.
    bool threw = false;
    bool ok = true;
    try {
        ok = writeFileAtomic("C:\\\xCE\xA2\xD0\xC5\\settings.txt", "x=1\n");
    } catch (...) {
        threw = true;
    }
    ADB_CHECK(!threw);
    ADB_CHECK(!ok);  // the directory does not exist, so this must fail cleanly
}

// -----------------------------------------------------------------------------
// replaceFileOver
//
// The adb updater replaces adb.exe while the adb server is running it, and Linux
// and Windows both refuse to overwrite a running executable - but Windows allows
// renaming one, so the update moves the old file aside and copies the new one in.
// These cases cover the plain path and both failure paths.
// -----------------------------------------------------------------------------

ADB_TEST(replaceFileOver_replaces_an_existing_file) {
    TempDir dir("adbtools-test-replace-basic");
    const std::string src = dir.file("new.exe");
    const std::string dst = dir.file("adb.exe");
    ADB_CHECK(writeFileAtomic(src, "new binary"));
    ADB_CHECK(writeFileAtomic(dst, "old binary"));
    std::string err;
    ADB_CHECK(replaceFileOver(src, dst, err));
    ADB_CHECK_EQ(err, std::string(""));
    ADB_CHECK_EQ(readAll(dst), std::string("new binary"));
    // No .old left behind when the old file was not in use.
    ADB_CHECK(!fs::exists(dst + ".old"));
}

ADB_TEST(replaceFileOver_creates_a_missing_target) {
    TempDir dir("adbtools-test-replace-create");
    const std::string src = dir.file("new.exe");
    const std::string dst = dir.file("adb.exe");
    ADB_CHECK(writeFileAtomic(src, "fresh"));
    std::string err;
    ADB_CHECK(replaceFileOver(src, dst, err));
    ADB_CHECK_EQ(readAll(dst), std::string("fresh"));
}

ADB_TEST(replaceFileOver_reports_a_missing_source) {
    TempDir dir("adbtools-test-replace-nosrc");
    const std::string dst = dir.file("adb.exe");
    ADB_CHECK(writeFileAtomic(dst, "original"));
    std::string err;
    ADB_CHECK(!replaceFileOver(dir.file("no-such-file"), dst, err));
    ADB_CHECK(!err.empty());
    // The target must survive a failed replace untouched.
    ADB_CHECK_EQ(readAll(dst), std::string("original"));
}

#ifdef _WIN32
// A running image is the one thing Windows will not let you overwrite, and that
// is precisely the adb updater's problem: it replaces adb.exe while the adb
// server is executing it, so the update used to fail with "file is in use". A
// merely open file is NOT equivalent - Windows happily replaces those (POSIX
// delete semantics), which is why this test starts a real process.
ADB_TEST(replaceFileOver_replaces_a_running_executable) {
    // Per-process directory name: if an earlier run was interrupted it can leave a
    // victim.exe running and therefore locked, and a fixed name would then make
    // this test fail on a *copy* error instead of testing what it is about.
    const std::string dirName =
        "adbtools-test-replace-running-" + std::to_string(GetCurrentProcessId());
    TempDir dir(dirName.c_str());
    const std::string victim = dir.file("victim.exe");
    const std::string src = dir.file("new.bin");

    // The test binary we are running right now makes a convenient victim: with
    // --sleep it stays alive until we kill it.
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    ADB_CHECK(n > 0 && n < MAX_PATH);
    std::error_code ec;
    fs::copy_file(fs::path(std::wstring(self, n)), fs::path(victim),
                  fs::copy_options::overwrite_existing, ec);
    ADB_CHECK(!ec);
    ADB_CHECK(writeFileAtomic(src, "new binary"));

    std::string cmd = "\"" + victim + "\" --sleep";
    std::string mutableCmd = cmd;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL launched = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ADB_CHECK(launched);
    if (!launched) return;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, 100);  // let it map its image

    // The plain overwrite really does fail while it runs - so the test is testing
    // what it claims to.
    std::error_code plainEc;
    fs::copy_file(src, victim, fs::copy_options::overwrite_existing, plainEc);
    const bool plainCopyFailed = static_cast<bool>(plainEc);

    std::string err;
    const bool replaced = replaceFileOver(src, victim, err);

    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);

    ADB_CHECK(plainCopyFailed);
    ADB_CHECK(replaced);
    ADB_CHECK_EQ(err, std::string(""));
    ADB_CHECK_EQ(readAll(victim), std::string("new binary"));

    // The moved-aside copy may still have been locked when cleanup ran, in which
    // case it is left as victim.exe.old on purpose; remove it now that the
    // process is gone.
    std::error_code cleanupEc;
    fs::remove(victim + ".old", cleanupEc);
}
#endif
