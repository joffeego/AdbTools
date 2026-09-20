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
#endif

}  // namespace adb::core
