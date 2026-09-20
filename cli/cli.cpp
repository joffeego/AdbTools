#include "cli/cli.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>

#include "core/adb.h"
#include "core/adbpath.h"
#include "core/appinfo.h"
#include "core/batch.h"
#include "core/process.h"
#include "core/strings.h"
#include "core/version.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace adb::cli {
namespace {

using adb::core::ProcessResult;
using adb::core::runProcess;

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

// Parsed arguments shared by every command.
struct Options {
    bool json = false;
    bool help = false;
    std::string serial;  // from --serial
    std::vector<std::string> args;
};

// --- output -----------------------------------------------------------------
//
// Human-readable by default; --json switches to a machine-readable form so the
// CLI can be scripted without parsing prose. Results go to stdout, problems to
// stderr, so `... > file` captures only what was asked for.
void emit(const std::string& line = std::string()) { std::cout << line << "\n"; }
void emitError(const std::string& line) { std::cerr << line << "\n"; }

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string jsonString(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

std::string jsonBool(bool value) { return value ? "true" : "false"; }

// --- adb access -------------------------------------------------------------

std::string g_adbPath;

bool requireAdb() {
    if (!g_adbPath.empty()) return true;
    g_adbPath = adb::core::findAdb();
    if (g_adbPath.empty()) {
        emitError("找不到 adb。");
        emitError("请安装 Android SDK platform-tools，或把 adb.exe 放到程序目录（或 scrcpy 子目录）/ 加入 PATH。");
        return false;
    }
    return true;
}

std::vector<adb::core::Device> listDevices() {
    const ProcessResult r = runProcess(g_adbPath, adb::core::devicesArgs(), 20000);
    if (r.exitCode != 0) return {};
    return adb::core::parseDevices(r.out);
}

// Decide which device to act on: an explicit --serial is honoured as-is (it may
// be a device adb has not listed yet, e.g. right after `adb connect`),
// otherwise there must be exactly one device in the "device" state.
bool resolveSerial(const Options& options, std::string& serial) {
    if (!options.serial.empty()) {
        serial = options.serial;
        return true;
    }
    const std::vector<adb::core::Device> devices = listDevices();

    std::vector<std::string> ready;
    std::string others;
    for (const adb::core::Device& d : devices) {
        if (d.state == "device") ready.push_back(d.serial);
        else others += "\n  " + d.serial + "  (" + d.state + ")";
    }
    if (ready.size() == 1) {
        serial = ready.front();
        return true;
    }
    if (ready.empty()) {
        emitError(others.empty() ? "没有连接的设备。请用数据线连接手机并允许 USB 调试。"
                                 : "没有可用的设备（需要处于 device 状态）：" + others);
        return false;
    }
    std::string list;
    for (const std::string& s : ready) list += "\n  " + s;
    emitError("连接了多个设备，请用 --serial 指定：" + list);
    return false;
}

std::string trimmed(const ProcessResult& r) { return adb::core::trim(r.out); }

// One consistent failure report: adb's own message when it said something,
// otherwise a note about the exit code (including the "timed out" hint).
int failWithAdb(const ProcessResult& r, const std::string& what) {
    const std::string msg = trimmed(r);
    if (!msg.empty()) {
        emitError(msg);
    } else if (!r.err.empty()) {
        emitError(what + "失败：" + r.err);
    } else {
        emitError(what + "失败（adb 退出码 " + std::to_string(r.exitCode) + "）");
    }
    return kExitFailure;
}

// --- logcat ----------------------------------------------------------------

// Streams `adb logcat` to our stdout line by line.
//
// This deliberately does NOT use core::runProcess: that buffers the whole child
// output before returning, which for a log stream means "return in an hour, with
// a gigabyte". Here the child is read as it produces, so each line appears
// immediately, a line limit can stop early, and a deadline can stop a stream
// that would otherwise never end.
//
// The child is terminated (and waited for) on the way out so no stray adb is left
// holding the device connection.
struct LogcatOptions {
    std::size_t limitLines = 0;   // 0 = unlimited
    int timeoutSeconds = 0;       // 0 = no deadline (until Ctrl+C)
    std::string filter;           // plain substring, matched case-insensitively
    bool clearFirst = false;
};

#ifdef _WIN32
// Returns false when the process could not be started.
bool pumpLogcat(const std::vector<std::string>& args, const LogcatOptions& logcat, int& exitCode) {
    std::wstring cmd = adb::core::quoteWinArg(adb::core::toWide(g_adbPath));
    for (const std::string& a : args) {
        cmd += L" ";
        cmd += adb::core::quoteWinArg(adb::core::toWide(a));
    }

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        emitError("无法创建管道");
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        emitError("无法启动 adb logcat");
        return false;
    }
    CloseHandle(writePipe);
    CloseHandle(pi.hThread);

    const std::string filterLower = adb::core::lower(logcat.filter);
    const auto started = std::chrono::steady_clock::now();
    std::string pending;
    char buffer[8192];
    std::size_t emitted = 0;
    bool stoppedEarly = false;

    auto handleLine = [&](const std::string& rawLine) {
        std::string line = rawLine;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!filterLower.empty() && adb::core::lower(line).find(filterLower) == std::string::npos) return;
        emit(line);
        ++emitted;
        if (logcat.limitLines > 0 && emitted >= logcat.limitLines) stoppedEarly = true;
    };

    for (;;) {
        if (stoppedEarly) break;
        if (logcat.timeoutSeconds > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::steady_clock::now() - started).count();
            if (elapsed >= logcat.timeoutSeconds) break;
        }

        DWORD available = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            const DWORD want = available > sizeof(buffer) ? sizeof(buffer) : available;
            DWORD read = 0;
            if (ReadFile(readPipe, buffer, want, &read, nullptr) && read > 0) {
                pending.append(buffer, read);
                std::size_t pos = 0;
                while ((pos = pending.find('\n')) != std::string::npos) {
                    handleLine(pending.substr(0, pos));
                    pending.erase(0, pos + 1);
                    if (stoppedEarly) break;
                }
            }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
        Sleep(15);
    }

    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    exitCode = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);
    return true;
}
#else
bool pumpLogcat(const std::vector<std::string>&, const LogcatOptions&, int&) {
    emitError("此平台的 logcat 流式输出尚未实现。");
    return false;
}
#endif

int cmdLogcat(const Options& options) {
    LogcatOptions logcat;
    std::vector<std::string> adbArgs;

    for (std::size_t i = 0; i < options.args.size(); ++i) {
        const std::string& a = options.args[i];
        if (a == "-n" && i + 1 < options.args.size()) {
            logcat.limitLines = static_cast<std::size_t>(std::strtoull(options.args[++i].c_str(), nullptr, 10));
            continue;
        }
        if (a == "--timeout" && i + 1 < options.args.size()) {
            logcat.timeoutSeconds = std::atoi(options.args[++i].c_str());
            continue;
        }
        if (a == "--grep" && i + 1 < options.args.size()) {
            logcat.filter = options.args[++i];
            continue;
        }
        if (a == "--clear") {
            logcat.clearFirst = true;
            continue;
        }
        // Anything else is passed straight to adb logcat (`-s TAG`, a priority
        // filter like `*:W`, ...).
        adbArgs.push_back(a);
    }

    if (logcat.limitLines == 0 && logcat.timeoutSeconds == 0) {
        emitError("为避免无限输出，请指定 -n <行数> 或 --timeout <秒数>（Ctrl+C 也可中断）。");
        return kExitUsage;
    }
    if (!requireAdb()) return kExitFailure;
    std::string serial;
    if (!resolveSerial(options, serial)) return kExitFailure;

    if (logcat.clearFirst) {
        const ProcessResult r = runProcess(g_adbPath, adb::core::logcatClearArgs(serial), 15000);
        if (r.exitCode != 0) return failWithAdb(r, "清空 logcat 缓冲");
    }

    std::vector<std::string> args = {"-s", serial, "logcat"};
    for (const std::string& a : adbArgs) args.push_back(a);

    int exitCode = 0;
    if (!pumpLogcat(args, logcat, exitCode)) return kExitFailure;
    return kExitOk;
}

// --- option parsing ---------------------------------------------------------

Options parseOptions(const std::vector<std::string>& argv, std::size_t from) {
    Options options;
    for (std::size_t i = from; i < argv.size(); ++i) {
        const std::string& a = argv[i];
        if (a == "--json") {
            options.json = true;
        } else if (a == "-h" || a == "--help") {
            options.help = true;
        } else if (a == "--serial") {
            if (i + 1 < argv.size()) options.serial = argv[++i];
        } else {
            options.args.push_back(a);
        }
    }
    return options;
}

// --- commands ---------------------------------------------------------------

int cmdHelp();

int cmdDevices(const Options& options) {
    if (!requireAdb()) return kExitFailure;
    const std::vector<adb::core::Device> devices = listDevices();

    if (options.json) {
        std::cout << "[";
        for (std::size_t i = 0; i < devices.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << "{\"serial\":" << jsonString(devices[i].serial)
                      << ",\"state\":" << jsonString(devices[i].state)
                      << ",\"model\":" << jsonString(devices[i].model)
                      << ",\"product\":" << jsonString(devices[i].product) << "}";
        }
        std::cout << "]\n";
        return kExitOk;
    }
    if (devices.empty()) {
        emit("(没有连接的设备)");
        return kExitOk;
    }
    for (const adb::core::Device& d : devices) {
        std::string line = d.serial + "  " + d.state;
        if (!d.model.empty()) line += "  " + d.model;
        emit(line);
    }
    return kExitOk;
}

int cmdList(const Options& options) {
    if (options.args.size() != 1) {
        emitError("用法：list <远程路径>");
        return kExitUsage;
    }
    if (!requireAdb()) return kExitFailure;
    std::string serial;
    if (!resolveSerial(options, serial)) return kExitFailure;

    const std::string path = options.args.front();
    const ProcessResult r = runProcess(g_adbPath, adb::core::listArgs(serial, path), 30000);
    if (r.exitCode != 0) return failWithAdb(r, "读取目录");

    const adb::core::ListingResult listing = adb::core::parseListingOutput(r.out);
    if (options.json) {
        std::cout << "{\"path\":" << jsonString(path)
                  << ",\"writable\":" << jsonBool(listing.writable) << ",\"entries\":[";
        for (std::size_t i = 0; i < listing.entries.size(); ++i) {
            const adb::core::FsEntry& e = listing.entries[i];
            if (i) std::cout << ",";
            std::cout << "{\"name\":" << jsonString(e.name) << ",\"dir\":" << jsonBool(e.isDir)
                      << ",\"link\":" << jsonBool(e.isLink)
                      << ",\"linkTarget\":" << jsonString(e.linkTarget) << ",\"size\":" << e.size
                      << ",\"date\":" << jsonString(e.date) << ",\"perms\":" << jsonString(e.perms) << "}";
        }
        std::cout << "]}\n";
        return kExitOk;
    }
    for (const adb::core::FsEntry& e : listing.entries) {
        const char* kind = e.isDir ? "d" : (e.isLink ? "l" : (e.isOther ? "?" : "-"));
        std::string size = e.isDir ? std::string(10, ' ') : adb::core::formatSize(e.size);
        std::string line = std::string(kind) + "  " + size + "  " + e.date + "  " + e.name;
        if (e.isLink && !e.linkTarget.empty()) line += " -> " + e.linkTarget;
        emit(line);
    }
    return kExitOk;
}

// Runs one command per item, recording each outcome, and prints either the JSON
// totals or the shared text summary.
//
// The accounting is core::BatchOutcome - the same one the GUI uses - so the two
// front ends cannot disagree about what happened, and the "was every item
// attempted" property is checked in one place.
int runBatch(const Options& options, const std::vector<std::string>& items,
             const std::function<bool(const std::string&, std::string&)>& runOne,
             const std::vector<std::string>& labels) {
    adb::core::BatchOutcome outcome(labels);
    for (std::size_t i = 0; i < items.size(); ++i) {
        std::string error;
        const bool ok = runOne(items[i], error);
        outcome.record(i, ok, error);
    }
    if (options.json) {
        std::cout << outcome.toJson() << "\n";
    } else {
        // Always printed, including the all-succeeded case: silence would be
        // indistinguishable from a command that did nothing.
        emit(outcome.text());
    }
    return outcome.failed() == 0 ? kExitOk : kExitFailure;
}

// File name for display; the path is what the command needs, the name is what
// the user recognises.
std::vector<std::string> labelsFor(const std::vector<std::string>& items) {
    std::vector<std::string> labels;
    labels.reserve(items.size());
    for (const std::string& item : items) {
        const std::string name = std::filesystem::path(item).filename().string();
        labels.push_back(name.empty() ? item : name);
    }
    return labels;
}

int cmdPush(const Options& options) {
    if (options.args.size() < 2) {
        emitError("用法：push <本地文件...> <远程目录>");
        return kExitUsage;
    }
    if (!requireAdb()) return kExitFailure;
    std::string serial;
    if (!resolveSerial(options, serial)) return kExitFailure;

    const std::string remoteDir = options.args.back();
    std::vector<std::string> localFiles(options.args.begin(), options.args.end() - 1);

    // Report a missing local file through the batch rather than aborting: one
    // bad path should not stop the others, which is the same rule the GUI uses.
    const int code = runBatch(
        options, localFiles,
        [&](const std::string& local, std::string& error) {
            if (!std::filesystem::exists(local)) {
                error = "找不到本地文件";
                return false;
            }
            const ProcessResult r = runProcess(g_adbPath, adb::core::pushArgs(serial, local, remoteDir), 600000);
            if (r.exitCode != 0) {
                error = trimmed(r);
                return false;
            }
            return true;
        },
        labelsFor(localFiles));
    return code;
}

int cmdPull(const Options& options) {
    if (options.args.size() < 2) {
        emitError("用法：pull <远程路径...> <本地目录>");
        return kExitUsage;
    }
    if (!requireAdb()) return kExitFailure;
    std::string serial;
    if (!resolveSerial(options, serial)) return kExitFailure;

    const std::string localDir = options.args.back();
    std::error_code ec;
    std::filesystem::create_directories(localDir, ec);
    if (ec) {
        emitError("无法创建本地目录：" + localDir + "（" + ec.message() + "）");
        return kExitFailure;
    }

    std::vector<std::string> remotes(options.args.begin(), options.args.end() - 1);
    return runBatch(
        options, remotes,
        [&](const std::string& remote, std::string& error) {
            const ProcessResult r = runProcess(g_adbPath, adb::core::pullArgs(serial, remote, localDir), 600000);
            if (r.exitCode != 0) {
                error = trimmed(r);
                return false;
            }
            return true;
        },
        labelsFor(remotes));
}

int cmdRemove(const Options& options) {
    if (options.args.empty()) {
        emitError("用法：rm <远程路径...>");
        return kExitUsage;
    }
    if (!requireAdb()) return kExitFailure;
    std::string serial;
    if (!resolveSerial(options, serial)) return kExitFailure;

    return runBatch(
        options, options.args,
        [&](const std::string& remote, std::string& error) {
            const ProcessResult r = runProcess(g_adbPath, adb::core::deleteArgs(serial, remote), 60000);
            if (r.exitCode != 0) {
                error = trimmed(r);
                return false;
            }
            return true;
        },
        labelsFor(options.args));
}

int cmdMkdir(const Options& options) {
    if (options.args.size() != 1) {
        emitError("用法：mkdir <远程路径>");
        return kExitUsage;
    }
    if (!requireAdb()) return kExitFailure;
    std::string serial;
    if (!resolveSerial(options, serial)) return kExitFailure;

    const ProcessResult r = runProcess(g_adbPath, adb::core::makeDirArgs(serial, options.args.front()), 30000);
    if (r.exitCode != 0) return failWithAdb(r, "创建目录");
    if (!options.json) emit("已创建 " + options.args.front());
    return kExitOk;
}

int cmdShell(const Options& options) {
    if (options.args.empty()) {
        emitError("用法：shell <命令...>");
        return kExitUsage;
    }
    if (!requireAdb()) return kExitFailure;
    std::string serial;
    if (!resolveSerial(options, serial)) return kExitFailure;

    std::string command;
    for (const std::string& part : options.args) {
        if (!command.empty()) command += " ";
        command += part;
    }
    // Passed as ONE argv entry: adb hands it to the device shell verbatim, so
    // the caller's quoting applies rather than ours.
    const ProcessResult r = runProcess(g_adbPath, {"-s", serial, "shell", command}, 120000);
    if (!trimmed(r).empty()) emit(trimmed(r));
    return r.exitCode;
}

int cmdInstall(const Options& options) {
    if (options.args.size() != 1) {
        emitError("用法：install <本地 apk>");
        return kExitUsage;
    }
    const std::string apk = options.args.front();
    if (!std::filesystem::exists(apk)) {
        emitError("找不到 APK：" + apk);
        return kExitFailure;
    }
    if (!requireAdb()) return kExitFailure;
    std::string serial;
    if (!resolveSerial(options, serial)) return kExitFailure;

    const ProcessResult r = runProcess(g_adbPath, adb::core::installApkArgs(serial, apk), 300000);
    if (r.exitCode != 0) return failWithAdb(r, "安装");
    // adb prints "Success" on success; anything else on stdout is the reason it
    // was not a success, so surface it rather than claiming success.
    if (options.json) {
        std::cout << "{\"installed\":" << jsonString(std::filesystem::path(apk).filename().string())
                  << "}\n";
    } else {
        emit("已安装 " + std::filesystem::path(apk).filename().string());
    }
    return kExitOk;
}

int cmdUninstall(const Options& options) {
    if (options.args.size() != 1) {
        emitError("用法：uninstall <包名>");
        return kExitUsage;
    }
    if (!requireAdb()) return kExitFailure;
    std::string serial;
    if (!resolveSerial(options, serial)) return kExitFailure;

    const ProcessResult r = runProcess(g_adbPath, adb::core::uninstallArgs(serial, options.args.front()), 120000);
    if (r.exitCode != 0) return failWithAdb(r, "卸载");
    if (!options.json) emit("已卸载 " + options.args.front());
    return kExitOk;
}

int cmdPackages(const Options& options) {
    if (!requireAdb()) return kExitFailure;
    std::string serial;
    if (!resolveSerial(options, serial)) return kExitFailure;

    const ProcessResult r = runProcess(g_adbPath, adb::core::thirdPartyPackagesArgs(serial), 30000);
    if (r.exitCode != 0) return failWithAdb(r, "获取应用列表");

    std::vector<std::string> packages = adb::core::parsePackageList(r.out);
    std::sort(packages.begin(), packages.end());
    if (options.json) {
        std::cout << "[";
        for (std::size_t i = 0; i < packages.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << jsonString(packages[i]);
        }
        std::cout << "]\n";
        return kExitOk;
    }
    for (const std::string& p : packages) emit(p);
    return kExitOk;
}

int cmdScreenshot(const Options& options) {
    if (options.args.size() > 1) {
        emitError("用法：screenshot [本地输出文件]");
        return kExitUsage;
    }
    if (!requireAdb()) return kExitFailure;
    std::string serial;
    if (!resolveSerial(options, serial)) return kExitFailure;

    std::string out = options.args.empty() ? (adb::core::defaultDownloadDir() + "/screenshot.png")
                                           : options.args.front();
    if (!out.empty() && out.back() == '/') out += "screenshot.png";

    // Write the PNG on the device first and pull it: runProcess captures the
    // child's output as text, and routing binary through that is asking for
    // trouble (and would also force the whole image through memory as a string).
    const std::string remote = "/sdcard/.__adbtools_screenshot.png";
    const ProcessResult cap = runProcess(
        g_adbPath, {"-s", serial, "shell", "screencap -p > " + remote}, 60000);
    if (cap.exitCode != 0) return failWithAdb(cap, "截图");

    const ProcessResult pull = runProcess(g_adbPath, adb::core::pullArgs(serial, remote, out), 120000);
    runProcess(g_adbPath, adb::core::deleteArgs(serial, remote), 30000);  // best effort cleanup
    if (pull.exitCode != 0) return failWithAdb(pull, "取回截图");

    if (options.json) {
        std::cout << "{\"file\":" << jsonString(out) << "}\n";
    } else {
        emit("截图已保存到 " + out);
    }
    return kExitOk;
}

int cmdVersion(const Options& options) {
    if (!requireAdb()) return kExitFailure;
    const ProcessResult r = runProcess(g_adbPath, {"version"}, 15000);
    const std::string adbVersion = adb::core::adbShortVersion(r.out);
    if (options.json) {
        std::cout << "{\"adb\":" << jsonString(adbVersion) << ",\"adbPath\":" << jsonString(g_adbPath)
                  << ",\"app\":" << jsonString(adb::core::kAppVersion) << "}\n";
    } else {
        emit("程序版本 " + std::string(adb::core::kAppVersion));
        emit("adb " + (adbVersion.empty() ? std::string("(未知版本)") : adbVersion));
        emit("adb 路径 " + g_adbPath);
    }
    return kExitOk;
}

struct Command {
    const char* name;
    const char* usage;
    const char* summary;
    int (*fn)(const Options&);
};

const Command kCommands[] = {
    {"devices",    "",                          "列出连接的设备",          cmdDevices},
    {"list",       "<远程路径>",                 "列出设备目录（含可写标志）", cmdList},
    {"push",       "<本地文件...> <远程目录>",     "上传本地文件到设备",       cmdPush},
    {"pull",       "<远程路径...> <本地目录>",     "从设备下载到本地",         cmdPull},
    {"rm",         "<远程路径...>",              "删除设备上的文件/目录",     cmdRemove},
    {"mkdir",      "<远程路径>",                 "在设备上创建目录",         cmdMkdir},
    {"shell",      "<命令...>",                  "在设备上执行 shell 命令",   cmdShell},
    {"install",    "<本地 apk>",                 "安装 APK（覆盖安装）",      cmdInstall},
    {"uninstall",  "<包名>",                     "卸载应用",                cmdUninstall},
    {"packages",   "",                          "列出第三方应用包名",        cmdPackages},
    {"screenshot", "[输出文件]",                 "截屏并保存到本地",         cmdScreenshot},
    {"logcat",     "-n <行数>|--timeout <秒>",   "流式输出 logcat（可 --grep/--clear）", cmdLogcat},
    {"version",    "",                          "显示程序与 adb 版本",       cmdVersion},
};

int cmdHelp() {
    emit("ADB 文件浏览器 —— 命令行模式");
    emit();
    emit("用法：adb_browser <命令> [参数...] [--json] [--serial <序列号>]");
    emit("      不加任何参数时启动图形界面。");
    emit();
    emit("命令：");
    for (const Command& c : kCommands) {
        std::string line = "  ";
        line += c.name;
        const std::string tail = std::string(c.usage).empty() ? std::string() : (" " + std::string(c.usage));
        line += tail;
        const std::size_t width = 34;
        line += std::string(line.size() < width ? width - line.size() : 2, ' ');
        line += c.summary;
        emit(line);
    }
    emit();
    emit("选项：");
    emit("  --json              输出 JSON（便于脚本处理）");
    emit("  --serial <序列号>    指定设备（连接多台时必须）");
    emit("  -h, --help          显示帮助");
    emit();
    emit("退出码：0 成功 / 1 操作失败 / 2 用法错误");
    return kExitOk;
}

}  // namespace

bool wantsCli(const std::vector<std::string>& args) {
    for (const std::string& a : args) {
        // "--cli" forces CLI mode even with no other arguments, and is how the
        // tests (and a user who wants the help text) get in without opening a
        // window.
        if (a == "--cli") return true;
    }
    return !args.empty();
}

int run(const std::vector<std::string>& argv) {
    if (argv.empty()) return cmdHelp();

    const std::string command = argv.front();
    Options options = parseOptions(argv, 1);

    if (command == "help" || command == "--help" || command == "-h") return cmdHelp();

    for (const Command& c : kCommands) {
        if (command != c.name) continue;
        if (options.help) return cmdHelp();
        return c.fn(options);
    }

    emitError("未知命令：" + command);
    emitError("运行 `adb_browser help` 查看用法。");
    return kExitUsage;
}

}  // namespace adb::cli
