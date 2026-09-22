#include "core/process.h"

#include <chrono>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

#include "core/paths.h"

namespace adb::core {

#ifdef _WIN32

namespace {

// The job object every child process this app starts is placed in.
//
// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE means the OS terminates everything still in
// the job once the last handle to it closes - and the OS closes our handle when
// this process terminates, however it terminates. That is the whole point: a crash
// or "End task" cannot leave adb behind.
//
// Created once, on first use, and deliberately never closed: closing it would kill
// the children that are legitimately still running.
HANDLE childJobObject() {
    static HANDLE job = []() -> HANDLE {
        HANDLE created = CreateJobObjectW(nullptr, nullptr);
        if (created == nullptr) return nullptr;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits))) {
            CloseHandle(created);
            return nullptr;
        }
        return created;
    }();
    return job;
}

}  // namespace

bool adoptChildProcess(void* nativeHandle) {
    if (nativeHandle == nullptr) return false;
    HANDLE job = childJobObject();
    if (job == nullptr) return false;
    // Best effort: an ancestor job that forbids nesting (only possible before
    // Windows 8) makes this fail, and the child then simply runs as it did before.
    return AssignProcessToJobObject(job, static_cast<HANDLE>(nativeHandle)) != FALSE;
}

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring quoteWinArg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    bool needQuotes = false;
    for (wchar_t c : arg) {
        if (c == L' ' || c == L'\t' || c == L'"' || c == L'\n') { needQuotes = true; break; }
    }
    if (!needQuotes) return arg;
    std::wstring out = L"\"";
    int backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(static_cast<std::size_t>(backslashes * 2 + 1), L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(static_cast<std::size_t>(backslashes), L'\\');
            out.push_back(c);
            backslashes = 0;
        }
    }
    out.append(static_cast<std::size_t>(backslashes * 2), L'\\');
    out.push_back(L'"');
    return out;
}

ProcessResult runProcessWindows(const std::string& program,
                                const std::vector<std::string>& args,
                                int timeoutMs) {    std::wstring cmd = quoteWinArg(toWide(program));
    for (const std::string& a : args) {
        cmd += L" ";
        cmd += quoteWinArg(toWide(a));
    }

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return {-1, "", "CreatePipe failed"};
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = writePipe;   // merge stderr into stdout to avoid deadlock
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    BOOL created = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!created) {
        CloseHandle(readPipe);
        return {-1, "", "CreateProcessW failed (" + std::to_string(GetLastError()) + ")"};
    }
    // Put the child in this process's job so it cannot outlive us: adb starts a
    // server that would otherwise keep running (and keep adb.exe locked, which
    // blocks the adb updater - see adoptChildProcess).
    adoptChildProcess(pi.hProcess);
    CloseHandle(pi.hThread);

    std::string output;
    char buffer[8192];
    auto start = std::chrono::steady_clock::now();
    bool timedOut = false;
    for (;;) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 0);
        bool exited = (wait == WAIT_OBJECT_0);
        DWORD available = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD toRead = available > sizeof(buffer) ? sizeof(buffer) : available;
            DWORD read = 0;
            if (ReadFile(readPipe, buffer, toRead, &read, nullptr) && read > 0) {
                output.append(buffer, read);
            }
            continue;
        }
        if (exited) break;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start).count();
        if (timeoutMs > 0 && elapsed > timeoutMs) {
            // TerminateProcess only *requests* termination, so wait for it to
            // take effect before reading the exit code - otherwise
            // GetExitCodeProcess below can still report STILL_ACTIVE (259),
            // which callers would mistake for a real exit code.
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            timedOut = true;
            break;
        }
        Sleep(8);
    }
    // Drain whatever remains.
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
        DWORD toRead = available > sizeof(buffer) ? sizeof(buffer) : available;
        DWORD read = 0;
        if (!ReadFile(readPipe, buffer, toRead, &read, nullptr) || read == 0) break;
        output.append(buffer, read);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);
    return {static_cast<int>(code), output, timedOut ? "timed out" : ""};
}

#else

ProcessResult runProcessPosix(const std::string& program, const std::vector<std::string>& args) {
    std::string cmd = shellQuote(program);
    for (const std::string& a : args) {
        cmd += " ";
        cmd += shellQuote(a);
    }
    cmd += " 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {-1, "", "popen failed"};
    std::string output;
    char buffer[4096];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        output.append(buffer, n);
    }
    int rc = pclose(pipe);
    int exitCode = (WIFEXITED(rc) != 0) ? WEXITSTATUS(rc) : -1;
    return {exitCode, output, ""};
}

#endif

ProcessResult runProcess(const std::string& program,
                         const std::vector<std::string>& args,
                         int timeoutMs) {
#ifdef _WIN32
    return runProcessWindows(program, args, timeoutMs);
#else
    (void)timeoutMs;
    return runProcessPosix(program, args);
#endif
}

}  // namespace adb::core
