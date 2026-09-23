// The application version, in one place.
//
// Both the GUI (window title, update check) and the CLI (--version) read this,
// so a release cannot ship with the two reporting different versions - which is
// exactly what happened when the value lived only in main.cpp.
//
// A release bumps this AND:
//   * adb_browser.rc      FILEVERSION / PRODUCTVERSION / FileVersion / ProductVersion
//   * adb_browser.manifest  assemblyIdentity version
//   * installer.iss       MyAppVersion
#pragma once

namespace adb::core {

// Keep in sync with the files listed above.
inline constexpr const char* kAppVersion = "0.10.10";

// GitHub repository used by the in-app update check ("owner/name").
inline constexpr const char* kAppUpdateRepo = "joffeego/AdbTools";

}  // namespace adb::core
