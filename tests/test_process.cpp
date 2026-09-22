// Tests for core/process.h - the job object that stops children outliving us.
//
// adb is a client/server program: a client that finds no server starts one, and
// that server keeps running after the client exits. When the app was killed while
// one of its adb children was starting, the child (and the server it spawned) were
// left behind. They are not just untidy - a running adb.exe cannot be overwritten,
// so the leftovers blocked the adb updater, and they held device connections open.
//
// The test process cannot test this against itself (the job only fires when the
// process *holding* it dies), so it starts a child that owns a grandchild, kills
// that child, and checks the grandchild is gone.
#include "core/process.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "tests/test_main.h"

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
std::string ownPath() {
    wchar_t buffer[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    return adb::core::toUtf8(std::wstring(buffer, static_cast<std::size_t>(n)));
}

// Reads the PID the grandchild wrote, or 0 if it has not appeared yet.
DWORD readPidFile(const std::string& path) {
    std::ifstream in(path);
    long long pid = 0;
    in >> pid;
    return static_cast<DWORD>(pid);
}

bool processIsRunning(DWORD pid) {
    if (pid == 0) return false;
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (handle == nullptr) return false;  // already gone
    const DWORD state = WaitForSingleObject(handle, 0);
    CloseHandle(handle);
    return state == WAIT_TIMEOUT;  // still running
}

bool waitForExit(DWORD pid, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!processIsRunning(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !processIsRunning(pid);
}
#endif

}  // namespace

#ifdef _WIN32
ADB_TEST(a_force_killed_parent_takes_its_children_with_it) {
    const fs::path dir = fs::temp_directory_path() / "adbtools-test-child-job";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string pidFile = (dir / "child.pid").string();

    const std::string self = ownPath();
    ADB_CHECK(!self.empty());
    if (self.empty()) return;

    // `--spawn` runs `<self> --sleep <pidfile>` through core::runProcess, which
    // adopts the child into its job, and then waits for it.
    std::string cmd = "\"" + self + "\" --spawn \"" + pidFile + "\"";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION spawner{};
    const BOOL launched = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &spawner);
    ADB_CHECK(launched);
    if (!launched) return;
    CloseHandle(spawner.hThread);

    // Wait for the grandchild to announce itself.
    DWORD childPid = 0;
    for (int i = 0; i < 200 && childPid == 0; ++i) {
        childPid = readPidFile(pidFile);
        if (childPid == 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ADB_CHECK(childPid != 0);
    ADB_CHECK(processIsRunning(childPid));  // the test would be vacuous otherwise

    // Kill the parent the way Task Manager or a crash would: no clean-up code runs
    // in it, so only the job object can save the grandchild.
    TerminateProcess(spawner.hProcess, 1);
    WaitForSingleObject(spawner.hProcess, 5000);
    CloseHandle(spawner.hProcess);

    ADB_CHECK(waitForExit(childPid, 10000));

    fs::remove_all(dir, ec);
}
#endif
