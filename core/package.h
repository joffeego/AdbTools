// Parsers for the third-party metadata the updater depends on:
//
//   * GitHub's release JSON (asset list, per-asset "digest")
//   * Google's SDK repository XML (platform-tools revision, archive URL and
//     checksum)
//
// All of it is pure string processing with no network access, which is exactly
// what makes it testable - and these parsers have already produced two real
// bugs (a brace-counting walk that tripped over GitHub's own URL templates, and
// a guessed platform-tools archive name that never existed). The tests in
// tests/test_package.cpp pin every one of those cases down.
#pragma once

#include <cstddef>
#include <string>

namespace adb::core {

// Value of the first `"key":"value"` occurrence. Returns "" when absent.
std::string jsonStringValue(const std::string& json, const std::string& key);

// End index of the JSON string opening at `openQuote`, or npos. Escape-aware.
std::size_t jsonStringEnd(const std::string& json, std::size_t openQuote);

// End index (closing brace) of the JSON object opening at `brace`, or npos if
// unbalanced. String-aware: braces inside string literals are ignored, because
// GitHub's JSON contains URL templates such as ".../events{/privacy}".
std::size_t jsonObjectEnd(const std::string& json, std::size_t brace);

// First GitHub release asset whose name ends in ".zip".
//
// Outputs the download URL, the asset name (row) and the sha256 published in
// the asset's own "digest" field (lowercase hex, "" when the release does not
// publish one). Returns false when the release carries no such asset - in which
// case the caller must NOT offer a self-update, since there is nothing to
// install.
bool findAppReleaseAsset(const std::string& json, std::string& url, std::string& row, std::string& sha);

// Download URL of the first "scrcpy-win64-*" asset. "" when absent.
std::string findWin64AssetUrl(const std::string& json);

// sha256 published in the release JSON for `url`, or "" when the asset is not
// found or carries no digest. Performs no network access: the caller falls back
// to the "<url>.sha256" sidecar itself.
std::string findAssetSha256(const std::string& releaseJson, const std::string& url);

// Text of the first <name>...</name> inside `block`, trimmed. "" when absent.
std::string xmlValue(const std::string& block, const std::string& name);

// platform-tools revision as "major.minor.micro" (e.g. "37.0.1"). "" when the
// package or its <revision> block is missing.
std::string parsePlatformToolsVersion(const std::string& xml);

// Absolute download URL and published checksum of the platform-tools archive
// for Windows. Both are cleared when no Windows archive is present.
//
// Reading the archive entry directly matters: the file is named
// "platform-tools_r<version>-win.zip" (not "-windows.zip"), so assembling the
// URL from the version number produces a 404.
void parsePlatformToolsWindowsArchive(const std::string& xml, std::string& url, std::string& checksum);

}  // namespace adb::core
