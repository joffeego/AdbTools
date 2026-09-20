// Entry point: one executable, two modes.
//
//   adb_browser.exe                 -> GUI
//   adb_browser.exe <command> ...   -> CLI, then exit
//   adb_browser.exe --cli           -> CLI help without opening a window
//
// The framework's own main() lives in core/app/glfw_app_main.cpp guarded by
// `#ifndef EUI_APP_RUNNER_LIBRARY`, and CMake defines that macro for this target
// so its main() is excluded and this one is used instead. That is why the
// vendored framework files need no local edits.
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "cli/cli.h"

// Runs the GUI event loop. Defined at global scope in
// core/app/glfw_app_main.cpp (its `namespace app { ... }` block closes well
// before this function), so it must be declared at global scope here too.
int eui_app_run();

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW

namespace {

// The executable is built for the WINDOWS subsystem, so it has no console of its
// own and a CLI run has to make its output reach the caller. What stdout actually
// is depends on how we were launched, and each case needs different handling:
//
//   * a pipe or a disk file - the caller is capturing us (`... | findstr`,
//     `> out.txt`, a parent reading our output). Usable as-is; re-pointing it at
//     a console would STEAL the output from them.
//   * a console handle we own - launched from a shell with nothing redirected.
//     Usable as-is; attaching again opens a SECOND console handle and sends the
//     output somewhere the waiting shell is not printing.
//   * an unusable handle (null, or FILE_TYPE_UNKNOWN) - a GUI-subsystem process
//     launched without a console inherits nothing useful. Attach to the parent's
//     console and point the streams at it.
//
// "A console we own" is determined with GetConsoleMode: it succeeds only when the
// handle refers to a console this process is attached to. A console handle
// inherited from a parent that we are NOT attached to fails it, which is the case
// that used to swallow output.
//
// std::cout must be re-synced after any redirect: freopen rebinds the C stdio
// streams only, and the C++ streams keep their own buffer on the old handle.
void attachParentConsole() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    bool usable = false;
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        const DWORD type = GetFileType(out);
        if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK) {
            usable = true;
        } else if (type == FILE_TYPE_CHAR) {
            DWORD mode = 0;
            usable = (GetConsoleMode(out, &mode) != 0);
        }
    }
    if (usable) return;

    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0) {
        // No console to attach to (double-clicked from Explorer, or a GUI parent)
        // and stdout is unusable. There is nothing to write to and nobody who
        // could have read it; the exit code still works.
        return;
    }

    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    freopen_s(&dummy, "CONIN$", "r", stdin);

    std::ios::sync_with_stdio(false);
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();
}

// argv as UTF-8. The wide command line is converted explicitly because the
// active code page is not UTF-8 on most systems, and device paths are UTF-8.
std::vector<std::string> commandLineArgs() {
    int argc = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    if (wide == nullptr) return args;
    for (int i = 0; i < argc; ++i) {
        const int need = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<std::size_t>(need > 0 ? need - 1 : 0), '\0');
        if (need > 1) {
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, &utf8[0], need, nullptr, nullptr);
        }
        args.push_back(std::move(utf8));
    }
    LocalFree(wide);
    return args;
}

}  // namespace
#endif  // _WIN32

int main(int argc, char** argv) {
#ifdef _WIN32
    std::vector<std::string> args = commandLineArgs();
#else
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
#endif

    // args[0] is the program path; the CLI only looks at what follows.
    std::vector<std::string> command(args.begin() + (args.empty() ? 0 : 1), args.end());

    if (adb::cli::wantsCli(command)) {
        // "--cli" is a mode hint for this entry point, not a command argument:
        // it exists so a caller can ask for the CLI (e.g. to read the help text)
        // without supplying a command. Strip it before the parser sees it.
        command.erase(std::remove(command.begin(), command.end(), std::string("--cli")), command.end());
#ifdef _WIN32
        attachParentConsole();
#endif
        return adb::cli::run(command);
    }
    return eui_app_run();
}
