// Tests for core/adbpath.h - which PATH entries may be probed, and how long the
// adb lookup is allowed to take.
//
// These exist because of a startup bug that was invisible in the CLI and took ~21
// seconds in the GUI: scanning PATH for scrcpy touched an unreachable UNC entry,
// and the first touch of an unreachable server blocks for the whole TCP connect
// timeout. Measured with "\\192.0.2.201\share" (TEST-NET-1) on PATH:
//
//     GetFileAttributesW("\\\\192.0.2.201\\share\\scrcpy.exe")   ~21 s (first touch)
//     the same call again on that host                            ~0.1 ms
//     the same call on another unreachable host                   ~21 s
//
// so a machine with N unreachable PATH entries could spend N x 21 s before its
// window appeared. The filter under test is the fix; the timing assertion below is
// what keeps it fixed.
#include "core/adbpath.h"

#include <chrono>
#include <cstdlib>
#include <string>

#include "test_main.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

// Restores a PATH override when it goes out of scope, so a test can set PATH
// without leaking it into the cases that follow.
class ScopedPath {
public:
    explicit ScopedPath(const std::string& value) {
        const char* previous = std::getenv("PATH");
        if (previous != nullptr) saved_ = previous;
        _putenv_s("PATH", value.c_str());
    }
    ~ScopedPath() { _putenv_s("PATH", saved_.c_str()); }

    ScopedPath(const ScopedPath&) = delete;
    ScopedPath& operator=(const ScopedPath&) = delete;

private:
    std::string saved_;
};

}  // namespace

// The case from the bug report: an unreachable server on PATH must not be probed.
// 192.0.2.0/24 is TEST-NET-1, reserved and unroutable, so if this ever regresses
// the test blocks for ~21 s instead of failing fast.
ADB_TEST(isUsablePathEntry_rejects_unc_entries) {
    ADB_CHECK(!adb::core::isUsablePathEntry("\\\\192.0.2.201\\share"));
    ADB_CHECK(!adb::core::isUsablePathEntry("//192.0.2.201/share"));
    ADB_CHECK(!adb::core::isUsablePathEntry("\\\\server\\share\\platform-tools"));
    // Forward slashes on a UNC root count too - Windows accepts either separator.
    ADB_CHECK(!adb::core::isUsablePathEntry("\\\\server/share"));
    ADB_CHECK(!adb::core::isUsablePathEntry("//server\\share"));
    // The \\?\ and \\.\ device forms are UNC-shaped as well, and adb is never
    // installed behind them.
    ADB_CHECK(!adb::core::isUsablePathEntry("\\\\?\\UNC\\server\\share"));
}

ADB_TEST(isUsablePathEntry_rejects_empty) {
    ADB_CHECK(!adb::core::isUsablePathEntry(""));
}

// An ordinary local directory is still probed: the filter must not turn into a
// blanket refusal, or adb in PATH would silently stop being found.
ADB_TEST(isUsablePathEntry_accepts_local_directories) {
#ifdef _WIN32
    ADB_CHECK(adb::core::isUsablePathEntry("C:\\Windows\\System32"));
    ADB_CHECK(adb::core::isUsablePathEntry("C:/Windows/System32"));
#endif
    // A relative entry never names a network location.
    ADB_CHECK(adb::core::isUsablePathEntry("tools"));
    ADB_CHECK(adb::core::isUsablePathEntry("."));
}

// A drive letter with no drive behind it: probing "X:\adb.exe" is how a stale
// PATH stalls, and nothing can be there, so the entry is dropped before probing.
ADB_TEST(isUsablePathEntry_rejects_drive_letters_with_no_drive) {
#ifdef _WIN32
    bool checkedOne = false;
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        const std::string root = std::string(1, letter) + ":\\";
        const UINT type = GetDriveTypeW(std::wstring(root.begin(), root.end()).c_str());
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) {
            ADB_CHECK(!adb::core::isUsablePathEntry(std::string(1, letter) + ":\\"));
            checkedOne = true;
        }
    }
    // A machine with all 26 letters assigned cannot exist, so this is really an
    // assertion that the loop above ran rather than a machine-dependent check.
    ADB_CHECK(checkedOne);
#endif
}

// Mapped network drives are dropped as well: for a persistent mapping whose
// server is gone (the "red X" drive Explorer hangs on) the stat triggers a
// reconnect and stalls for the same ~21 s. A runner rarely has one, so this only
// asserts the rule when one is present.
ADB_TEST(isUsablePathEntry_rejects_mapped_network_drives) {
#ifdef _WIN32
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        const std::string root = std::string(1, letter) + ":\\";
        if (GetDriveTypeW(std::wstring(root.begin(), root.end()).c_str()) == DRIVE_REMOTE) {
            ADB_CHECK(!adb::core::isUsablePathEntry(root));
        }
    }
#endif
}

// The end-to-end version: with an unreachable UNC entry first on PATH, the adb
// lookup must return promptly instead of blocking on it.
//
// The assertion is "far below the ~21 s connect timeout" rather than an exact
// budget, so a loaded machine cannot make it flaky while a regression still fails.
ADB_TEST(findAdb_does_not_stall_on_an_unreachable_path_entry) {
    ScopedPath path("\\\\192.0.2.202\\share;C:\\definitely-not-a-real-dir-adbtools");
    const auto started = std::chrono::steady_clock::now();
    const std::string found = adb::core::findAdb();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    ADB_CHECK(seconds < 5.0);
    // Whatever it returns must not have come from the unroutable entry.
    ADB_CHECK(found.find("192.0.2.202") == std::string::npos);
}
