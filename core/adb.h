// adb command construction and output parsing.
//
// Pure functions only: they build the argument vectors that get handed to
// runProcess() and parse what comes back. Nothing here starts a process, so
// every one of them is unit tested against real adb output in tests/test_adb.cpp.
//
// Building commands in one place also removes a class of bug the old inline
// calls were prone to: quoting. The device-side shell interprets what it
// receives, so a path with a space, a quote or a "$" has to be quoted exactly
// once, and the quoting rule lives in core/paths.h (shellQuote).
#pragma once

#include <string>
#include <vector>

namespace adb::core {

struct Device {
    std::string serial;
    std::string state;  // "device", "unauthorized", "offline", ...
    std::string model;  // from `adb devices -l`, may be empty
    std::string product;
};

struct FsEntry {
    std::string name;
    bool isDir = false;
    bool isLink = false;
    bool isOther = false;  // not a regular file, directory or symlink
    long long size = 0;
    std::string perms;
    std::string date;
    std::string linkTarget;
};

struct ListingResult {
    std::vector<FsEntry> entries;
    bool writable = false;
};

// Arguments for `adb devices -l`. "-l" adds model/product/device properties; it
// is supported by every platform-tools release the app targets.
std::vector<std::string> devicesArgs();

// Parse `adb devices` / `adb devices -l` output.
//
// Tolerates the daemon startup chatter ("* daemon not running; starting now at
// tcp:5037", "* daemon started successfully") and the "List of devices attached"
// header, and ignores blank lines. Lines with no state field still produce a
// device entry (state left empty) rather than being dropped, so a malformed line
// shows up in the UI instead of silently disappearing.
std::vector<Device> parseDevices(const std::string& output);

// Parse `ls -la` / `ls -l` output into entries.
//
// Handles both date shapes real devices emit:
//   "Mon DD HH:MM"  (or "Mon DD YYYY")  -> 8 metadata columns
//   "YYYY-MM-DD HH:MM"                  -> 7 metadata columns
// Skips the "total N" line and the "." / ".." entries. For symlinks the
// "name -> target" form is split into name + linkTarget. Filenames containing
// spaces are reassembled from the trailing tokens.
std::vector<FsEntry> parseLsLa(const std::string& output);

// Shell command that lists `path` and reports whether it is writable, in one
// round trip: the sentinel keeps the two results apart, and the exit status is
// preserved so a failure is still a failure.
std::string buildListCommand(const std::string& path);

// Marker emitted by buildListCommand between the listing and the writable flag.
extern const char* const kWriteMarker;

// Split buildListCommand()'s output back into a listing and a writability flag.
// When the marker is missing (adb died mid-command, or the device echoed an
// error) the whole output is treated as the listing and writable stays false,
// which is the safe direction - the UI only enables destructive actions on a
// directory it knows is writable.
ListingResult parseListingOutput(const std::string& output);

// --- device-side operations -------------------------------------------------

// `adb -s <serial> pull <remote> <localDir>`
std::vector<std::string> pullArgs(const std::string& serial, const std::string& remote,
                                  const std::string& localDir);

// `adb -s <serial> push <localFile> <remoteDir>`
std::vector<std::string> pushArgs(const std::string& serial, const std::string& localFile,
                                  const std::string& remoteDir);

// `adb -s <serial> shell rm -rf <remote>`
std::vector<std::string> deleteArgs(const std::string& serial, const std::string& remote);

// `adb -s <serial> shell mkdir -p <remote>`
std::vector<std::string> makeDirArgs(const std::string& serial, const std::string& remote);

// `adb -s <serial> install -r <apk>`
std::vector<std::string> installApkArgs(const std::string& serial, const std::string& apkPath);

// `adb -s <serial> uninstall <package>`
std::vector<std::string> uninstallArgs(const std::string& serial, const std::string& package);

// `adb -s <serial> shell pm clear <package>`
std::vector<std::string> clearAppDataArgs(const std::string& serial, const std::string& package);

// `adb -s <serial> shell input keyevent 3` etc. is not used here; this is the
// screenshot hop: `adb -s <serial> exec-out screencap -p`
std::vector<std::string> screencapArgs(const std::string& serial);

// `adb -s <serial> logcat -c`
std::vector<std::string> logcatClearArgs(const std::string& serial);

// `adb connect <host:port>`
std::vector<std::string> connectArgs(const std::string& hostPort);

// `adb kill-server`
std::vector<std::string> killServerArgs();

// --- output helpers ---------------------------------------------------------

// Extract (package, label) pairs from `pm list packages` / `dumpsys` output.
// Accepts lines of the form "package:com.example.app" and ignores everything
// else, including adb's own error text.
std::vector<std::string> parsePackageList(const std::string& output);

}  // namespace adb::core
