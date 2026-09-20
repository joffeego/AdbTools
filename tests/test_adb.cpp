// Tests for core/adb.h - command construction and adb output parsing.
//
// The fixtures are mostly copied verbatim from a real device (`adb devices -l`,
// `ls -la /sdcard/Download`) rather than invented, because the interesting cases
// are exactly the ones a hand-written sample forgets: the ISO date format this
// device emits, filenames with spaces, symlinks, and special files.
#include "core/adb.h"

#include <string>
#include <vector>

#include "tests/test_main.h"

using namespace adb::core;

namespace {

// Verbatim from `adb devices -l` on a Huawei ELS-AN00.
const char* kDevicesL =
    "List of devices attached\n"
    "UQG5T20616008457       device product:ELS-AN00 model:ELS_AN00 device:HWELS transport_id:1\n";

// Two devices, one not ready: what a user with a stale authorisation sees.
const char* kDevicesMixed =
    "List of devices attached\n"
    "emulator-5554          device product:sdk_gphone64_x86_64 model:sdk_gphone64_x86_64 device:emu64xa transport_id:1\n"
    "UQG5T20616008457       unauthorized usb:1-3 transport_id:2\n"
    "0123456789ABCDEF       offline usb:2-1 transport_id:3\n";

// Verbatim from `ls -la /sdcard/Download` (ISO date form), trimmed.
const char* kLsIsoDate =
    "total 14080\n"
    "drwxrwx--- 3 media_rw media_rw    3440 2025-12-29 03:11 .7934039b\n"
    "-rw-rw---- 1 media_rw media_rw      32 2025-03-16 17:01 .801ab.cuid\n"
    "-rw-rw---- 1 media_rw media_rw 1434590 2024-10-26 07:57 2. 矩阵高次幂的计算（例题解析）_1664539430387.pdf\n"
    "drwxrwx--- 2 media_rw media_rw    3440 2026-05-07 11:18 Browser\n";

// The classic `ls -l` form with a month name (8 metadata columns).
const char* kLsMonthDate =
    "total 24\n"
    "drwxr-xr-x 2 root   root      4096 Jan 12 09:30 dcim\n"
    "-rw-r--r-- 1 root   root       512 Dec  3  2024 notes.txt\n"
    "lrwxrwxrwx 1 root   root        21 Aug  8  2018 sdcard -> /storage/self/primary\n";

}  // namespace

ADB_TEST(parseDevices_reads_serial_state_and_model) {
    const std::vector<Device> devices = parseDevices(kDevicesL);
    ADB_CHECK_EQ(devices.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(ADB_AT(devices, 0).serial, std::string("UQG5T20616008457"));
    ADB_CHECK_EQ(ADB_AT(devices, 0).state, std::string("device"));
    ADB_CHECK_EQ(ADB_AT(devices, 0).model, std::string("ELS_AN00"));
    ADB_CHECK_EQ(ADB_AT(devices, 0).product, std::string("ELS-AN00"));
}

ADB_TEST(parseDevices_skips_header_and_keeps_problem_states) {
    const std::vector<Device> devices = parseDevices(kDevicesMixed);
    ADB_CHECK_EQ(devices.size(), static_cast<std::size_t>(3));
    ADB_CHECK_EQ(ADB_AT(devices, 0).state, std::string("device"));
    // A device the user has not authorised must still be listed, otherwise the
    // UI cannot tell them why nothing is selectable.
    ADB_CHECK_EQ(ADB_AT(devices, 1).state, std::string("unauthorized"));
    ADB_CHECK_EQ(ADB_AT(devices, 1).serial, std::string("UQG5T20616008457"));
    ADB_CHECK_EQ(ADB_AT(devices, 1).model, std::string(""));  // -l omitted the property
    ADB_CHECK_EQ(ADB_AT(devices, 2).state, std::string("offline"));
}

ADB_TEST(parseDevices_ignores_daemon_chatter) {
    const std::string out =
        "* daemon not running; starting now at tcp:5037\n"
        "* daemon started successfully\n"
        "List of devices attached\n"
        "ABC123\tdevice\n";
    const std::vector<Device> devices = parseDevices(out);
    ADB_CHECK_EQ(devices.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(ADB_AT(devices, 0).serial, std::string("ABC123"));
}

ADB_TEST(parseDevices_empty_and_blank_output) {
    ADB_CHECK_EQ(parseDevices("").size(), static_cast<std::size_t>(0));
    ADB_CHECK_EQ(parseDevices("List of devices attached\n\n").size(), static_cast<std::size_t>(0));
    // No devices, but adb still succeeded.
    ADB_CHECK_EQ(parseDevices("List of devices attached\n").size(), static_cast<std::size_t>(0));
}

ADB_TEST(parseDevices_keeps_line_without_state) {
    // A truncated/garbled line is surfaced with an empty state rather than being
    // dropped, so the problem is visible instead of the device vanishing.
    const std::vector<Device> devices = parseDevices("List of devices attached\nABC123\n");
    ADB_CHECK_EQ(devices.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(ADB_AT(devices, 0).serial, std::string("ABC123"));
    ADB_CHECK_EQ(ADB_AT(devices, 0).state, std::string(""));
}

ADB_TEST(parseLsLa_reads_iso_dates_and_skips_total) {
    const std::vector<FsEntry> entries = parseLsLa(kLsIsoDate);
    ADB_CHECK_EQ(entries.size(), static_cast<std::size_t>(4));  // "total" line skipped
    ADB_CHECK_EQ(ADB_AT(entries, 0).name, std::string(".7934039b"));
    ADB_CHECK(ADB_AT(entries, 0).isDir);
    ADB_CHECK(!ADB_AT(entries, 0).isLink);
    ADB_CHECK_EQ(ADB_AT(entries, 0).date, std::string("2025-12-29 03:11"));
    ADB_CHECK_EQ(ADB_AT(entries, 1).size, 32LL);
    ADB_CHECK(!ADB_AT(entries, 1).isDir);
    ADB_CHECK_EQ(ADB_AT(entries, 2).name,
                 std::string("2. 矩阵高次幂的计算（例题解析）_1664539430387.pdf"));
    ADB_CHECK_EQ(ADB_AT(entries, 2).size, 1434590LL);
    ADB_CHECK_EQ(ADB_AT(entries, 3).name, std::string("Browser"));
    ADB_CHECK(ADB_AT(entries, 3).isDir);
}

ADB_TEST(parseLsLa_reads_month_dates_and_year_only) {
    const std::vector<FsEntry> entries = parseLsLa(kLsMonthDate);
    ADB_CHECK_EQ(entries.size(), static_cast<std::size_t>(3));
    ADB_CHECK_EQ(ADB_AT(entries, 0).name, std::string("dcim"));
    ADB_CHECK_EQ(ADB_AT(entries, 0).date, std::string("Jan 12 09:30"));
    // "Dec  3  2024" - the double space collapses into the date as well.
    ADB_CHECK_EQ(ADB_AT(entries, 1).name, std::string("notes.txt"));
    ADB_CHECK_EQ(ADB_AT(entries, 1).date, std::string("Dec 3 2024"));
    ADB_CHECK_EQ(ADB_AT(entries, 1).size, 512LL);
}

ADB_TEST(parseLsLa_splits_symlink_target) {
    const std::vector<FsEntry> entries = parseLsLa(kLsMonthDate);
    // The size is asserted before anything is read: binding a reference to
    // entries[2] when the parser produced fewer entries would read past the end,
    // which is how this file previously corrupted the heap on CI instead of
    // failing.
    ADB_CHECK_EQ(entries.size(), static_cast<std::size_t>(3));
    const FsEntry& link = ADB_AT(entries, 2);
    ADB_CHECK(link.isLink);
    ADB_CHECK(!link.isDir);
    ADB_CHECK_EQ(link.name, std::string("sdcard"));
    ADB_CHECK_EQ(link.linkTarget, std::string("/storage/self/primary"));
}

ADB_TEST(parseLsLa_reassembles_names_with_spaces) {
    // The ISO fixture already covers a spaced name; check the month-date branch
    // too, since that is the one with an extra column.
    const std::string out = "-rw-r--r-- 1 root root 100 Jan 12 09:30 my report 2024 final.txt\n";
    const std::vector<FsEntry> entries = parseLsLa(out);
    ADB_CHECK_EQ(entries.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(ADB_AT(entries, 0).name, std::string("my report 2024 final.txt"));
}

ADB_TEST(parseLsLa_marks_special_files_as_other) {
    const std::string out =
        "srwxrwxrwx 1 root root  0 2025-01-01 00:00 socket\n"
        "crw-rw-rw- 1 root root  1 2025-01-01 00:00 null\n"
        "brw-rw---- 1 root root  1 2025-01-01 00:00 sda\n"
        "prw-r--r-- 1 root root  0 2025-01-01 00:00 pipe\n";
    const std::vector<FsEntry> entries = parseLsLa(out);
    ADB_CHECK_EQ(entries.size(), static_cast<std::size_t>(4));
    for (const FsEntry& e : entries) {
        ADB_CHECK(e.isOther);
        ADB_CHECK(!e.isDir);
        ADB_CHECK(!e.isLink);
    }
}

ADB_TEST(parseLsLa_skips_dot_entries_and_junk) {
    const std::string out =
        "total 8\n"
        "drwxr-xr-x 2 root root 4096 2025-01-01 00:00 .\n"
        "drwxr-xr-x 2 root root 4096 2025-01-01 00:00 ..\n"
        "short line\n"
        "\n"
        "-rw-r--r-- 1 root root 10 2025-01-01 00:00 keep.txt\n";
    const std::vector<FsEntry> entries = parseLsLa(out);
    ADB_CHECK_EQ(entries.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(ADB_AT(entries, 0).name, std::string("keep.txt"));
}

ADB_TEST(parseLsLa_handles_crlf_from_windows_adb) {
    // adb on Windows has been known to hand back CRLF; a stray '\r' would end up
    // inside the filename and break every downstream path join.
    const std::string out = "-rw-r--r-- 1 root root 10 2025-01-01 00:00 keep.txt\r\n";
    const std::vector<FsEntry> entries = parseLsLa(out);
    ADB_CHECK_EQ(entries.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(ADB_AT(entries, 0).name, std::string("keep.txt"));
}

ADB_TEST(parseLsLa_error_text_produces_no_entries) {
    // This is what `ls -la` prints for a path that does not exist, and it must
    // not be mistaken for a listing.
    const std::string out = "ls: /nope: No such file or directory\n";
    ADB_CHECK_EQ(parseLsLa(out).size(), static_cast<std::size_t>(0));
}

ADB_TEST(buildListCommand_quotes_the_path_once) {
    const std::string cmd = buildListCommand("/sdcard/My Files");
    // The device shell receives the path quoted; the sentinel separates the
    // listing from the writability check and the exit status is preserved.
    ADB_CHECK(cmd.find("ls -la '/sdcard/My Files' 2>&1") != std::string::npos);
    ADB_CHECK(cmd.find("test -w '/sdcard/My Files'") != std::string::npos);
    ADB_CHECK(cmd.find(kWriteMarker) != std::string::npos);
    ADB_CHECK(cmd.find("exit $__r") != std::string::npos);
}

ADB_TEST(buildListCommand_contains_no_shell_metacharacter_escape_hatch) {
    // A path crafted to break out of the quoting must not: the command may
    // contain the characters, but only inside single quotes.
    const std::string cmd = buildListCommand("/sdcard/a'; rm -rf /; echo '");
    ADB_CHECK(cmd.find("'/sdcard/a'\\''; rm -rf /; echo '\\'''") != std::string::npos);
}

ADB_TEST(parseListingOutput_splits_listing_and_writable) {
    const std::string out =
        std::string("total 4\n"
                    "drwxr-xr-x 2 root root 4096 2025-01-01 00:00 sub\n") +
        kWriteMarker + "\n1\n";
    const ListingResult result = parseListingOutput(out);
    ADB_CHECK_EQ(result.entries.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(ADB_AT(result.entries, 0).name, std::string("sub"));
    ADB_CHECK(result.writable);
}

ADB_TEST(parseListingOutput_read_only_directory) {
    const std::string out = std::string("-rw-r--r-- 1 root root 1 2025-01-01 00:00 f\n") +
                            kWriteMarker + "\n0\n";
    const ListingResult result = parseListingOutput(out);
    ADB_CHECK_EQ(result.entries.size(), static_cast<std::size_t>(1));
    ADB_CHECK(!result.writable);
}

ADB_TEST(parseListingOutput_without_marker_is_not_writable) {
    // If adb died half-way the marker never arrives; treating the whole output
    // as a listing and reporting "not writable" is the safe direction, because
    // the UI only enables destructive actions on a directory it knows is
    // writable.
    const ListingResult result = parseListingOutput("error: device offline\n");
    ADB_CHECK_EQ(result.entries.size(), static_cast<std::size_t>(0));
    ADB_CHECK(!result.writable);
}

ADB_TEST(command_builders_quote_device_paths) {
    ADB_CHECK_EQ(deleteArgs("S1", "/sdcard/My Dir").size(), static_cast<std::size_t>(6));
    ADB_CHECK_EQ(ADB_AT(deleteArgs("S1", "/sdcard/My Dir"), 5), std::string("'/sdcard/My Dir'"));
    ADB_CHECK_EQ(ADB_AT(makeDirArgs("S1", "/sdcard/a b"), 5), std::string("'/sdcard/a b'"));
}

ADB_TEST(command_builders_shape) {
    // Each builder is assigned to a named vector and its size asserted before
    // any index is touched: indexing a temporary's element directly is what made
    // an earlier version of this test able to crash instead of reporting a
    // failure (it took down the whole run on CI, where the assert fired as
    // std::bad_alloc with no indication of which case it was).
    const std::vector<std::string> pull = pullArgs("S1", "/sdcard/a.txt", "C:\\out");
    ADB_CHECK_EQ(pull.size(), static_cast<std::size_t>(5));
    if (pull.size() == 5) {
        ADB_CHECK_EQ(ADB_AT(pull, 0), std::string("-s"));
        ADB_CHECK_EQ(ADB_AT(pull, 1), std::string("S1"));
        ADB_CHECK_EQ(ADB_AT(pull, 2), std::string("pull"));
        ADB_CHECK_EQ(ADB_AT(pull, 3), std::string("/sdcard/a.txt"));
        // Local paths are NOT shell-quoted: they go through CreateProcess as
        // separate argv entries via runProcess, which quotes them itself.
        ADB_CHECK_EQ(ADB_AT(pull, 4), std::string("C:\\out"));
    }

    const std::vector<std::string> push = pushArgs("S1", "C:\\my file.apk", "/sdcard/My Dir");
    ADB_CHECK_EQ(push.size(), static_cast<std::size_t>(5));
    if (push.size() == 5) {
        ADB_CHECK_EQ(ADB_AT(push, 2), std::string("push"));
        ADB_CHECK_EQ(ADB_AT(push, 3), std::string("C:\\my file.apk"));
        // The remote side is a directory argument, not a shell snippet: adb
        // quotes it itself, so it must not arrive pre-quoted.
        ADB_CHECK_EQ(ADB_AT(push, 4), std::string("/sdcard/My Dir"));
    }

    const std::vector<std::string> install = installApkArgs("S1", "C:\\a.apk");
    ADB_CHECK_EQ(install.size(), static_cast<std::size_t>(5));
    if (install.size() == 5) {
        ADB_CHECK_EQ(ADB_AT(install, 2), std::string("install"));
        ADB_CHECK_EQ(ADB_AT(install, 3), std::string("-r"));
        ADB_CHECK_EQ(ADB_AT(install, 4), std::string("C:\\a.apk"));
    }

    const std::vector<std::string> uninstall = uninstallArgs("S1", "com.x");
    ADB_CHECK_EQ(uninstall.size(), static_cast<std::size_t>(4));
    if (uninstall.size() == 4) {
        ADB_CHECK_EQ(ADB_AT(uninstall, 2), std::string("uninstall"));
        ADB_CHECK_EQ(ADB_AT(uninstall, 3), std::string("com.x"));
    }

    const std::vector<std::string> clear = clearAppDataArgs("S1", "com.x");
    ADB_CHECK_EQ(clear.size(), static_cast<std::size_t>(6));
    if (clear.size() == 6) {
        ADB_CHECK_EQ(ADB_AT(clear, 2), std::string("shell"));
        ADB_CHECK_EQ(ADB_AT(clear, 3), std::string("pm"));
        ADB_CHECK_EQ(ADB_AT(clear, 4), std::string("clear"));
        ADB_CHECK_EQ(ADB_AT(clear, 5), std::string("com.x"));
    }

    const std::vector<std::string> screencap = screencapArgs("S1");
    ADB_CHECK_EQ(screencap.size(), static_cast<std::size_t>(5));
    if (screencap.size() == 5) {
        ADB_CHECK_EQ(ADB_AT(screencap, 2), std::string("exec-out"));
        ADB_CHECK_EQ(ADB_AT(screencap, 3), std::string("screencap"));
        ADB_CHECK_EQ(ADB_AT(screencap, 4), std::string("-p"));
    }

    const std::vector<std::string> logcatClear = logcatClearArgs("S1");
    ADB_CHECK_EQ(logcatClear.size(), static_cast<std::size_t>(4));
    if (logcatClear.size() == 4) {
        ADB_CHECK_EQ(ADB_AT(logcatClear, 2), std::string("logcat"));
        ADB_CHECK_EQ(ADB_AT(logcatClear, 3), std::string("-c"));
    }

    const std::vector<std::string> connect = connectArgs("192.168.1.5:5555");
    ADB_CHECK_EQ(connect.size(), static_cast<std::size_t>(2));
    if (connect.size() == 2) {
        ADB_CHECK_EQ(ADB_AT(connect, 0), std::string("connect"));
        ADB_CHECK_EQ(ADB_AT(connect, 1), std::string("192.168.1.5:5555"));
    }

    const std::vector<std::string> kill = killServerArgs();
    ADB_CHECK_EQ(kill.size(), static_cast<std::size_t>(1));
    if (kill.size() == 1) {
        ADB_CHECK_EQ(ADB_AT(kill, 0), std::string("kill-server"));
    }
}

ADB_TEST(devicesArgs_requests_long_format) {
    const std::vector<std::string> args = devicesArgs();
    ADB_CHECK_EQ(args.size(), static_cast<std::size_t>(2));
    ADB_CHECK_EQ(ADB_AT(args, 0), std::string("devices"));
    ADB_CHECK_EQ(ADB_AT(args, 1), std::string("-l"));
}

ADB_TEST(parsePackageList_reads_package_lines_only) {
    const std::string out =
        "package:com.huawei.security.hsdr\n"
        "package:com.fenqile.fenqile\n"
        "error: closed\n"
        "\n"
        "package:com.android.cts.priv.ctsshim versionCode=29\n";
    const std::vector<std::string> packages = parsePackageList(out);
    ADB_CHECK_EQ(packages.size(), static_cast<std::size_t>(3));
    ADB_CHECK_EQ(ADB_AT(packages, 0), std::string("com.huawei.security.hsdr"));
    ADB_CHECK_EQ(ADB_AT(packages, 1), std::string("com.fenqile.fenqile"));
    // A trailing property is not part of the package name.
    ADB_CHECK_EQ(ADB_AT(packages, 2), std::string("com.android.cts.priv.ctsshim"));
}

ADB_TEST(parsePackageList_empty) {
    ADB_CHECK_EQ(parsePackageList("").size(), static_cast<std::size_t>(0));
    ADB_CHECK_EQ(parsePackageList("error: device offline\n").size(), static_cast<std::size_t>(0));
}
