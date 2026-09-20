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

// Default download directory: $USERPROFILE\Downloads, else $HOME/Downloads,
// else ".".
std::string defaultDownloadDir();

}  // namespace adb::core
