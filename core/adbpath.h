// Locating this executable and the adb it should drive.
#pragma once

#include <string>

namespace adb::core {

// Directory containing the running executable (no trailing separator).
// The app keeps its settings/bookmarks/commands there rather than in the CWD, so
// they survive being launched from a shortcut, a terminal or a file manager.
std::string executableDir();

// Find an adb binary, in this order:
//   1. $ANDROID_SDK_ROOT / $ANDROID_HOME /platform-tools
//   2. %LOCALAPPDATA%\Android\Sdk\platform-tools, then %ProgramFiles%\Android\Sdk
//   3. every directory on $PATH
//   4. next to the executable, then its scrcpy\ subdirectory (the copy the release
//      ships)
// Returns "" when nothing is found; callers report that rather than guessing.
std::string findAdb();

// True when a PATH entry is worth probing for adb/scrcpy.
//
// Not every entry is, because the first touch of an unreachable network location
// blocks for the whole TCP connect timeout. Measured on Windows 10 with
// "\\192.0.2.201\share" (TEST-NET-1, deliberately unroutable) on PATH:
//
//     GetFileAttributesW("\\\\192.0.2.201\\share\\adb.exe")   21026 ms   (first touch)
//     the same call on the same host afterwards                    0.1 ms   (SMB negative cache)
//     the same call on a different unreachable host            21040 ms
//
// So each distinct unreachable server costs ~21 s of dead startup, once per
// session, and that is what "the window takes forever to appear" looked like on a
// laptop whose office share / VPN was not reachable (see the 0.10.10 notes).
// Entries that cannot hold a binary are therefore skipped rather than probed:
//   - UNC ("\\server\share", "//server/share") - adb is not installed on shares in
//     practice, and this is the case the timeout above comes from;
//   - a drive letter with no drive behind it (DRIVE_NO_ROOT_DIR / DRIVE_UNKNOWN);
//   - a *network* drive letter (DRIVE_REMOTE). GetDriveTypeW answers that without
//     touching the network, but a persistent mapping whose server is gone (the
//     "red X" drive Explorer hangs on) makes the following stat attempt a
//     reconnect - the same ~21 s stall, once per mapped drive. A connected network
//     drive is skipped too: the tradeoff is deliberate, since startup latency on
//     every machine matters more than adb-on-a-mapped-drive, which
//     $ANDROID_SDK_ROOT and the copy next to the executable still cover.
bool isUsablePathEntry(const std::string& dir);

// Default download directory: $USERPROFILE\Downloads, else $HOME/Downloads,
// else ".".
std::string defaultDownloadDir();

}  // namespace adb::core
