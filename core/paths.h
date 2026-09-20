// Device-side (POSIX) path and shell-quoting helpers.
//
// These operate on paths *inside* the Android device, not on Windows paths, and
// have no dependency on the running app - hence their own module so they can be
// tested directly.
#pragma once

#include <string>
#include <vector>

namespace adb::core {

// Quote an argument for the device-side shell (`adb shell ...`). Wrapping in
// single quotes makes every character literal except a single quote itself.
std::string shellQuote(const std::string& s);

// Join a directory path and a child name, tolerating an empty and "/" base and
// a trailing slash on the base.
std::string joinPath(const std::string& base, const std::string& name);

// Parent of a device path. Never returns an empty string: "/" is its own parent.
std::string parentPath(const std::string& p);

// Split a device path into components, dropping empties and leading "/".
std::vector<std::string> splitPath(const std::string& p);

}  // namespace adb::core
