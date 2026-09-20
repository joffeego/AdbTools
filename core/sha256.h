// Checksum helpers used to verify downloaded update packages.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace adb::core {

// Parse a digest blob into (filename, lowercase hex) rows.
//
// Accepts both the sha256sum format
//     <64 hex>  <filename>
//     <64 hex> *<filename>
// and GitHub's release "digest" values ("sha256:<hex>"), and tolerates CRLF.
// Rows whose digest is not exactly 64 hex characters, or that carry no
// filename, are skipped.
std::vector<std::pair<std::string, std::string>> parseSha256Rows(const std::string& text);

#ifdef _WIN32
// Hex SHA-256 of a file, via CNG (bcrypt). On failure returns "" and fills
// `err`. Windows-only because it is only used by the updater.
std::string sha256FileHex(const std::wstring& path, std::string& err);

// Hex SHA-1 of a file. Google publishes SHA-1 (not SHA-256) digests for
// platform-tools in repository2-1.xml, so the adb updater verifies with this.
std::string sha1FileHex(const std::wstring& path, std::string& err);

// Generic variant; `algorithm` is a BCRYPT_*_ALGORITHM constant (L"SHA256").
std::string hashFileHex(const std::wstring& path, const wchar_t* algorithm, std::string& err);
#endif

}  // namespace adb::core
