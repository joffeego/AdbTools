// A stand-in for adb.exe, used only while testing the external-edit feature on a
// machine with no phone attached. It implements just enough of the adb command
// line for the app's real code paths to run unchanged:
//
//   fakeadb pull <remote> <localDir>   copy FAKE_PHONE_ROOT<remote> -> localDir
//   fakeadb push <localFile> <remote>  copy localFile -> FAKE_PHONE_ROOT<remote>
//   fakeadb devices -l                 print the fake device
//   fakeadb version                    print a plausible version
//   fakeadb shell cat <remote>         print the file
//   fakeadb shell "ls -la '<dir>' ..." print an `ls -la` listing of the directory
//
// The listing makes the whole file browser usable without a phone (the app's list
// view, the right-click menu, the text preview), not just the external-edit path.
//
// FAKE_PHONE_ROOT is the directory that plays the part of the phone's filesystem.
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string phoneRoot() {
    const char* root = std::getenv("FAKE_PHONE_ROOT");
    return root != nullptr ? root : ".";
}

// Strips the leading '/' so the phone path can be joined onto FAKE_PHONE_ROOT.
//
// u8path, not path: on Windows a fs::path built from a narrow string goes through the
// ANSI code page, so any Chinese file name arrives as invalid bytes and construction
// throws "Cannot convert character sequence" - the stub crashed on `pull` of
// /sdcard/拖拽测试文件.txt while the real adb handles such names fine.
static fs::path phonePath(const std::string& remote) {
    std::string relative = remote;
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    return fs::u8path(phoneRoot()) / fs::u8path(relative);
}

static int copyFile(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr, "fakeadb: %s\n", ec.message().c_str());
        return 1;
    }
    std::printf("1 file pushed/pulled\n");
    return 0;
}

// `ls -la <dir>` in the shape core/adb.cpp's parseLsLa() expects: permissions,
// links, owner, group, size, "Mon DD HH:MM", name. Returns false when the
// directory does not exist, so the caller can exit non-zero like the real one.
static bool printListing(const std::string& remote) {
    const fs::path dir = phonePath(remote);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::printf("ls: %s: No such file or directory\n", remote.c_str());
        return false;
    }
    std::vector<fs::directory_entry> entries;
    for (const auto& entry : fs::directory_iterator(dir, ec)) entries.push_back(entry);
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename().string() < b.path().filename().string();
    });

    std::printf("total %d\n", static_cast<int>(entries.size()) * 4);
    for (const auto& entry : entries) {
        const std::string name = entry.path().filename().string();
        std::error_code sizeError;
        const auto size = entry.is_directory(sizeError) ? 4096 : static_cast<long long>(fs::file_size(entry.path(), sizeError));
        const bool isDir = entry.is_directory(sizeError);
        const bool isLink = entry.is_symlink(sizeError);
        const char* perms = isLink ? "lrwxrwxrwx" : (isDir ? "drwxr-xr-x" : "-rw-r--r--");
        std::printf("%s 1 root root %8lld Jan  1 12:00 %s", perms, size, name.c_str());
        if (isLink) {
            std::error_code linkError;
            const fs::path target = fs::read_symlink(entry.path(), linkError);
            if (!linkError) std::printf(" -> %s", target.string().c_str());
        }
        std::printf("\n");
    }
    return true;
}

// Pull the single-quoted path out of `ls -la '<path>' 2>&1; ...` (the app's
// buildListCommand()). Returns an empty string when the shape is not recognised.
static std::string quotedPathAfterLs(const std::string& commandLine) {
    const std::string prefix = "ls -la '";
    if (commandLine.rfind(prefix, 0) != 0) return {};
    const std::size_t close = commandLine.find('\'', prefix.size());
    if (close == std::string::npos) return {};
    return commandLine.substr(prefix.size(), close - prefix.size());
}

// The app shell-quotes device paths ("cat '/sdcard/x.txt'"), so strip the quotes
// the way the real shell would.
static std::string unquote(const std::string& value) {
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

static int runFakeAdb(std::vector<std::string> args) {

    // Skip the "-s <serial>" selector and operation flags the app passes.
    std::size_t i = 0;
    while (i < args.size() && args[i] == "-s") i += 2;
    if (i >= args.size()) return 0;
    const std::string command = args[i++];
    if (command == "-L") return 0;  // adb's own server flags

    if (command == "pull" && i + 1 < args.size()) {
        const fs::path from = phonePath(args[i]);
        fs::path to = fs::path(args[i + 1]);
        // adb pull takes a directory and keeps the file name.
        if (fs::is_directory(to)) to /= from.filename();
        return copyFile(from, to);
    }
    if (command == "push" && i + 1 < args.size()) {
        const fs::path from = args[i];
        fs::path to = phonePath(args[i + 1]);
        // adb push to a directory keeps the local file's name (the app always pushes
        // to a directory, so without this the copy lands on the directory itself and
        // fails with "Invalid argument" - which is a fake-adb bug, not an app one).
        std::error_code dirError;
        if (fs::is_directory(to, dirError)) to /= from.filename();
        return copyFile(from, to);
    }
    if (command == "devices") {
        std::printf("List of devices attached\nFAKEDEVICE001\tdevice product:fake model:Fake_Phone device:fake\n");
        return 0;
    }
    if (command == "version") {
        std::printf("Android Debug Bridge version 1.0.41\nVersion 37.0.1-fake\n");
        return 0;
    }
    if (command == "shell") {
        // "cat <path>" is all the app needs from a shell in this test.
        if (i + 1 < args.size() && args[i] == "cat") {
            std::ifstream in(phonePath(unquote(args[i + 1])), std::ios::binary);
            if (!in) return 1;
            std::printf("%s", std::string((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>())
                                 .c_str());
            return 0;
        }
        // The listing command is a compound shell line; only its `ls -la '<path>'`
        // part matters here. The app expects the listing, then the write marker,
        // then a 1/0 writability flag (core/adb.cpp: parseListingOutput).
        if (i < args.size()) {
            const std::string path = quotedPathAfterLs(args[i]);
            if (!path.empty()) {
                // The real command is `ls ... 2>&1; echo MARKER; test -w ...; exit $__r`,
                // so a missing directory ends with a non-zero status and the error text
                // is already on stdout (2>&1). Mimic exactly that, or the CLI would look
                // like it silently succeeded on a path that does not exist.
                const bool ok = printListing(path);
                std::printf("__WRITE__\n1\n");
                return ok ? 0 : 2;
            }
        }
        // Path auto-completion asks the other way round: separate arguments
        // ("shell ls -la '/sdcard'"), so handle that shape too.
        if (i < args.size() && args[i] == "ls") {
            std::size_t j = i + 1;
            while (j < args.size() && !args[j].empty() && args[j][0] == '-') ++j;
            if (j < args.size()) printListing(unquote(args[j]));
            return 0;
        }
        // The rest of the CLI's shell commands, so the whole command set can be
        // exercised without a phone (see cli/README.md and the AI integration guide).
        // `shell <command...>`: the CLI joins every argument into ONE string and hands
        // it to the device shell, so "echo hello" arrives as a single argv entry.
        if (i < args.size() && args[i].rfind("echo ", 0) == 0) {
            std::printf("%s\n", args[i].substr(5).c_str());
            return 0;
        }
        if (i < args.size() && args[i] == "echo") {
            for (std::size_t j = i + 1; j < args.size(); ++j) {
                std::printf("%s%s", j > i + 1 ? " " : "", unquote(args[j]).c_str());
            }
            std::printf("\n");
            return 0;
        }
        if (i < args.size() && args[i] == "mkdir") {
            std::size_t j = i + 1;
            while (j < args.size() && !args[j].empty() && args[j][0] == '-') ++j;
            if (j < args.size()) {
                std::error_code ec;
                fs::create_directories(phonePath(unquote(args[j])), ec);
                if (ec) { std::fprintf(stderr, "mkdir failed: %s\n", ec.message().c_str()); return 1; }
            }
            return 0;
        }
        if (i < args.size() && args[i] == "rm") {
            std::size_t j = i + 1;
            while (j < args.size() && !args[j].empty() && args[j][0] == '-') ++j;
            if (j < args.size()) {
                std::error_code ec;
                fs::remove_all(phonePath(unquote(args[j])), ec);
                if (ec) { std::fprintf(stderr, "rm failed: %s\n", ec.message().c_str()); return 1; }
            }
            return 0;
        }
        if (i < args.size() && args[i] == "pm") {
            // `pm list packages -3` - three stand-in third-party packages, sorted the
            // way a real device's list would be (the CLI sorts anyway).
            if (i + 2 < args.size() && args[i + 1] == "list" && args[i + 2] == "packages") {
                std::printf("package:com.example.alpha\npackage:com.example.beta\npackage:org.fake.gamma\n");
            }
            return 0;
        }
        // `screencap -p > '/sdcard/x.png'`: the redirect and the command are ONE
        // argument, so this is a prefix match, not an equality check.
        if (i < args.size() && args[i].rfind("screencap", 0) == 0) {
            // `screencap -p > '/sdcard/x.png'`: the redirect is inside this one argument.
            const std::string line = args[i];
            const std::size_t arrow = line.find('>');
            if (arrow != std::string::npos) {
                std::string target = line.substr(arrow + 1);
                while (!target.empty() && (target.front() == ' ' || target.front() == '\t')) target.erase(target.begin());
                const fs::path file = phonePath(unquote(target));
                std::error_code ec;
                fs::create_directories(file.parent_path(), ec);
                // A real 1x1 PNG: the CLI (and the device tests) check the signature.
                static const unsigned char kPng[] = {
                    0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R',
                    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15,
                    0xC4, 0x89, 0x00, 0x00, 0x00, 0x0A, 'I', 'D', 'A', 'T', 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00,
                    0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 'I', 'E', 'N', 'D',
                    0xAE, 0x42, 0x60, 0x82};
                std::ofstream out(file, std::ios::binary);
                if (!out) return 1;
                out.write(reinterpret_cast<const char*>(kPng), sizeof(kPng));
                return 0;
            }
            return 0;
        }
        return 0;
    }
    if (command == "logcat") {
        // `-c`/`--clear` prints nothing, like the real thing.
        for (std::size_t j = i; j < args.size(); ++j) {
            if (args[j] == "-c" || args[j] == "--clear") return 0;
        }
        // Opt-in continuous stream (FAKE_LOGCAT_STREAM=1). It exists so the app's reader
        // loop, its line cap and its stop path can be exercised against a stream that does
        // not end on its own - three fixed lines could never reproduce "the app stays slow
        // after closing logcat". Off by default because the test suite runs this command
        // and expects it to return.
        if (std::getenv("FAKE_LOGCAT_STREAM") != nullptr) {
            long long counter = 0;
            for (;;) {
                const int level = static_cast<int>(counter % 3);
                const char* tag = level == 0 ? "I" : (level == 1 ? "W" : "E");
                std::printf("01-01 12:%02d:%02d.%03d  1234  5678 %s FakeTag : fake logcat line %lld\n",
                            static_cast<int>((counter / 600) % 60),
                            static_cast<int>((counter / 10) % 60),
                            static_cast<int>((counter * 100) % 1000), tag, counter);
                std::fflush(stdout);
                ++counter;
            }
        }
        std::printf("01-01 12:00:00.000  1234  5678 I FakeTag : fake logcat line 1\n");
        std::printf("01-01 12:00:00.100  1234  5678 W FakeTag : fake logcat line 2\n");
        std::printf("01-01 12:00:00.200  1234  5678 E FakeTag : fake logcat line 3\n");
        return 0;
    }
    return 0;
}

// Windows hands a program its command line in the ANSI code page, so the UTF-8 names the
// app passes (any Chinese file name) arrived as invalid bytes and std::filesystem threw
// "Cannot convert character sequence" - the stub died on `pull /sdcard/拖拽测试文件.txt`
// while the real adb handles such names fine.
//
// Read the *wide* command line instead. Deliberately not wmain: that would require
// building with -municode, and the documented build command (see cli/AI-INTEGRATION.md)
// has no such flag - anyone following it must still get a working adb.
#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>

int main() {
    int argc = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    if (wide != nullptr) {
        args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
        for (int i = 1; i < argc; ++i) {
            const int need =
                WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string text;
            if (need > 1) {
                text.resize(static_cast<std::size_t>(need - 1));
                WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), need, nullptr, nullptr);
            }
            args.push_back(std::move(text));
        }
        LocalFree(wide);
    }
    return runFakeAdb(std::move(args));
}
#else
int main(int argc, char** argv) {
    return runFakeAdb(std::vector<std::string>(argv + 1, argv + argc));
}
#endif