// Entry point for the unit tests. Each test_*.cpp registers its cases through
// the ADB_TEST macro; this file only runs them.
//
// It also implements two modes that the process tests use to check that children
// cannot outlive their parent - see replaceFileOver / adoptChildProcess:
//
//   --sleep <pidfile>   write our PID to <pidfile>, then sleep
//   --spawn <pidfile>   run "<self> --sleep <pidfile>" and wait for it
//
// "--spawn" blocks inside core::runProcess(), so the child it starts is adopted
// into that process's job object; killing the "--spawn" process must therefore
// take the "--sleep" child with it.
#include "tests/test_main.h"

#include <cstring>
#include <fstream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "core/process.h"

namespace {

#ifdef _WIN32
std::string ownPath() {
    wchar_t buffer[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    return adb::core::toUtf8(std::wstring(buffer, static_cast<std::size_t>(n)));
}

void writePidFile(const char* path) {
    std::ofstream out(path, std::ios::trunc);
    out << GetCurrentProcessId() << "\n";
    out.flush();
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // The pidfile is optional so that callers which only need "a process that
    // stays alive" (the replaceFileOver test) can use it too.
    if (argc > 1 && std::strcmp(argv[1], "--sleep") == 0) {
        if (argc > 2) writePidFile(argv[2]);
        Sleep(60000);
        return 0;
    }
    if (argc > 2 && std::strcmp(argv[1], "--spawn") == 0) {
        const std::string self = ownPath();
        if (self.empty()) return 1;
        // Blocks until the child exits, which is the point: this process is alive
        // and holds the job object for as long as the child runs.
        adb::core::runProcess(self, {"--sleep", argv[2]}, 0);
        return 0;
    }
#endif
    return adb::test::runAll();
}
