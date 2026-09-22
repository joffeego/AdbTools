// Subprocess execution - the one place the app starts a child process.
//
// Extracted from main.cpp so the GUI and the CLI share exactly one
// implementation. That matters more than it looks: this function is where the
// process-handling bugs lived (a timeout that read the exit code before the
// process was gone, and pipe draining that could hold the loop open), and having
// two copies would mean fixing each of them twice.
//
// Kept out of the "pure logic" category on purpose - it spawns processes, so it
// has no unit tests; it is exercised through the CLI and the device tests
// instead. See core/README.md.
#pragma once

#include <string>
#include <vector>

namespace adb::core {

struct ProcessResult {
    int exitCode = -1;
    std::string out;
    std::string err;
};

// Run `program` with `args` and capture its merged stdout+stderr.
//
// Arguments are passed as separate argv entries (not pasted into a shell), so a
// path with spaces or quotes needs no quoting by the caller.
//
// On Windows the child is created with CREATE_NO_WINDOW, so calling this from a
// console process does not flash a window. A timeout terminates the child and
// waits for it to actually exit before reading the exit code - otherwise
// GetExitCodeProcess can still report STILL_ACTIVE (259), which callers would
// mistake for a real result. When the timeout fires, `err` is set to
// "timed out".
ProcessResult runProcess(const std::string& program,
                         const std::vector<std::string>& args,
                         int timeoutMs = 60000);

#ifdef _WIN32
// UTF-8 <-> UTF-16 conversion for the Windows APIs.
std::wstring toWide(const std::string& s);
std::string toUtf8(const std::wstring& w);

// Quote one argument for a Windows command line (CommandLineToArgvW rules).
std::wstring quoteWinArg(const std::wstring& arg);

// Put a freshly created child process into this process's job object, so Windows
// terminates it when this process exits - however it exits, including a crash or
// being killed from Task Manager.
//
// Why this is needed: adb is a client/server program, and a client that finds no
// server starts one, which then outlives the client. If the app is killed while
// such a child is starting (or while `adb logcat` runs, or while scrcpy mirrors),
// those processes are orphaned. They are not merely untidy: a running adb.exe
// cannot be overwritten, so they keep the adb updater from replacing it, and they
// hold a device connection open. Measured before this existed: eighteen adb
// processes were still alive nine hours after the app that started them died.
//
// `nativeHandle` is a Win32 process HANDLE (passed as void* so this header stays
// platform-neutral). Returns false if the child could not be adopted, which is not
// fatal - the process simply runs to completion on its own as it used to. The
// usual reason is an ancestor job that forbids nesting (pre-Windows 8).
bool adoptChildProcess(void* nativeHandle);
#endif

}  // namespace adb::core
