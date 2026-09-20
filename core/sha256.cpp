#include "core/sha256.h"

#include <cctype>

#include "core/strings.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace adb::core {

std::vector<std::pair<std::string, std::string>> parseSha256Rows(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> rows;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;

        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("sha256:", 0) == 0) line = trim(line.substr(7));
        if (line.size() < 64) continue;

        std::string hex;
        hex.reserve(64);
        std::size_t i = 0;
        for (; i < line.size() && hex.size() < 64; ++i) {
            const char c = line[i];
            if (std::isxdigit(static_cast<unsigned char>(c))) {
                hex += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            } else {
                break;
            }
        }
        if (hex.size() != 64) continue;

        std::string name = trim(line.substr(i));
        if (!name.empty() && (name[0] == '*' || name[0] == ' ')) name = trim(name.substr(1));
        if (name.empty()) continue;
        rows.emplace_back(name, hex);
    }
    return rows;
}

#ifdef _WIN32
std::string hashFileHex(const std::wstring& path, const wchar_t* algorithm, std::string& err) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    auto cleanup = [&] {
        if (hash != nullptr) BCryptDestroyHash(hash);
        if (alg != nullptr) BCryptCloseAlgorithmProvider(alg, 0);
    };

    if (BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, 0) < 0) {
        err = "无法初始化哈希算法";
        cleanup();
        return result;
    }
    DWORD objectSize = 0;
    DWORD cb = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                          sizeof(objectSize), &cb, 0) < 0) {
        err = "无法读取哈希属性";
        cleanup();
        return result;
    }
    DWORD digestSize = 0;
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digestSize),
                          sizeof(digestSize), &cb, 0) < 0) {
        err = "无法读取哈希长度";
        cleanup();
        return result;
    }
    std::vector<unsigned char> object(objectSize);
    if (BCryptCreateHash(alg, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) {
        err = "无法创建哈希上下文";
        cleanup();
        return result;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        err = "无法读取下载的文件";
        cleanup();
        return result;
    }
    std::vector<unsigned char> buf(65536);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr)) {
            err = "读取下载的文件失败";
            break;
        }
        if (read == 0) break;
        if (BCryptHashData(hash, buf.data(), read, 0) < 0) {
            err = "哈希计算失败";
            break;
        }
    }
    CloseHandle(file);
    if (!err.empty()) {
        cleanup();
        return result;
    }

    std::vector<unsigned char> digest(digestSize);
    if (BCryptFinishHash(hash, digest.data(), digestSize, 0) < 0) {
        err = "哈希计算失败";
        cleanup();
        return result;
    }
    cleanup();

    static const char* kHex = "0123456789abcdef";
    result.reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
        result += kHex[byte >> 4];
        result += kHex[byte & 0x0F];
    }
    return result;
}

std::string sha256FileHex(const std::wstring& path, std::string& err) {
    return hashFileHex(path, BCRYPT_SHA256_ALGORITHM, err);
}

std::string sha1FileHex(const std::wstring& path, std::string& err) {
    return hashFileHex(path, BCRYPT_SHA1_ALGORITHM, err);
}
#endif  // _WIN32

}  // namespace adb::core
