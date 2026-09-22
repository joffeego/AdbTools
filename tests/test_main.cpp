// Entry point for the unit tests. Each test_*.cpp registers its cases through
// the ADB_TEST macro; this file only runs them.
#include "tests/test_main.h"

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    // "adbtools_tests.exe --sleep" starts a process that just sits there and does
    // nothing for a while. replaceFileOver()'s test needs a *running image* to
    // replace, because Windows refuses to overwrite one - which is exactly what
    // the adb updater hits when it replaces a running adb.exe. Merely holding a
    // file open is not equivalent: Windows can still replace an open file, but not
    // a mapped image.
    if (argc > 1 && std::strcmp(argv[1], "--sleep") == 0) {
        Sleep(60000);
        return 0;
    }
#endif
    return adb::test::runAll();
}
