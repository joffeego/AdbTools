# CLI

One executable, two modes:

```
adb_browser.exe                     # GUI
adb_browser.exe <command> [args]    # CLI, then exit
adb_browser.exe --cli               # CLI help without opening a window
```

`app_main.cpp` is the router. It replaces the framework's own `main()` by
defining `EUI_APP_RUNNER_LIBRARY` on `3rd/EUI-NEO/core/app/glfw_app_main.cpp`
(see the comment in `CMakeLists.txt` — the macro has to be on *that* file, which
is the opposite of what it looks like at first).

## Console output on Windows

The binary is built for the **WINDOWS** subsystem, so it has no console of its
own. A CLI run therefore has to attach to its parent's console
(`AttachConsole(ATTACH_PARENT_PROCESS)`) or its output goes nowhere and the user
sees a program that "does nothing".

Two details that are easy to get wrong, and both are handled in
`app_main.cpp:attachParentConsole()`:

- **Only attach when stdout is not already redirected.** If the caller passed a
  pipe or a file (`adb_browser list /sdcard | findstr x`, `> out.txt`, or a
  parent process capturing output), the handle is already usable — re-pointing it
  at a console *steals* the output from the reader. The code checks
  `GetFileType(STD_OUTPUT_HANDLE)` first and leaves redirects alone.
- **Re-sync the C++ streams afterwards.** `freopen` rebinds the C `stdout`, but
  `std::cout` keeps its own buffer and stays pointed at the inherited handle.
  Without `std::ios::sync_with_stdio(false)` plus `clear()`, some output appears
  and the rest silently vanishes.

## Commands

| Command | Description |
| --- | --- |
| `devices` | List connected devices (serial, state, model) |
| `list <path>` | `ls -la` of a device directory, plus whether it is writable |
| `push <local...> <remoteDir>` | Upload files |
| `pull <remote...> <localDir>` | Download files |
| `rm <remote...>` | Delete files/directories |
| `mkdir <remote>` | Create a directory |
| `shell <command...>` | Run a shell command on the device |
| `install <apk>` | Install an APK (reinstall) |
| `uninstall <package>` | Uninstall an app |
| `packages` | List third-party package names |
| `screenshot [file]` | Capture the screen and save it locally |
| `logcat` | Stream logcat; requires `-n <lines>` or `--timeout <seconds>` |
| `version` | Show the app and adb versions |
| `help` | Usage |

### logcat

Streams as lines arrive rather than buffering, so `-n` / `--timeout` can stop it
and output appears immediately. It deliberately does not use `core::runProcess`:
that returns only after the child exits, which for a log stream means "in an hour,
with a gigabyte".

```bash
adb_browser logcat -n 100                    # first 100 lines, then exit
adb_browser logcat --timeout 10              # collect for 10 seconds
adb_browser logcat --timeout 5 --grep crash  # substring, case-insensitive
adb_browser logcat --clear -n 50             # clear the buffer first
adb_browser logcat --timeout 5 -s ActivityManager   # extra args go to adb logcat
```

`-n <lines>` or `--timeout <seconds>` is required: without one the command would
stream forever. Ctrl+C also works. Any unrecognised argument is passed straight
to `adb logcat`, so tag/priority filters (`-s TAG`, `*:W`) work as usual.

## Conventions

- **`--json`** switches output to machine-readable form. Results go to stdout,
  problems to stderr, so `... > file` captures only what was asked for.
- **`--serial <serial>`** picks the device. With exactly one ready device it is
  optional; with several, its absence is an error that names the candidates. It
  is honoured even when adb has not listed the device yet (useful right after
  `adb connect`).
- **Exit codes**: `0` success, `1` operation failed, `2` usage error. A command
  that handles several items (push/pull/rm) attempts **all** of them and reports
  `已完成 N，失败 M` — the same rule the GUI's batch operations follow, and the
  reason a failure stops being silent.
- **Quoting**: arguments are passed to adb as separate `argv` entries, so a path
  with spaces needs quoting by *your shell*, not by us. `shell` is the exception
  in the other direction: everything after the command name is joined into one
  string and handed to the device shell verbatim, so your quoting applies.

## How it is wired

```
app_main.cpp      mode switch + console handling + argv as UTF-8
cli/cli.cpp       argument parsing, one function per command, output formatting
core/*            everything the commands actually do (see core/README.md)
```

The CLI holds no adb knowledge of its own: it calls the `core::` command
builders and parsers, which is what makes `devices`, `list`, `push`, `pull`,
`rm`, `mkdir`, `install`, `uninstall`, `packages`, `screenshot` and `version`
testable and keeps the GUI and CLI from drifting apart.

## Tests

- **Unit tests** (`ctest -LE device`) cover the core logic without a device.
- **Device end-to-end tests** (`tests/device_tests.ps1`, `ctest -L device`) drive
  this CLI against a real phone: 42 checks covering the CLI contract (exit codes,
  `--json`), device/listing parsing, byte-for-byte push/pull round trips
  (including names with spaces), batch deletion with a genuine partial failure,
  the package list, and the screenshot path (it asserts the PNG signature).
  They self-skip with exit code 0 when no device is connected, and clean up the
  test directory on the device even when they fail.

  CI excludes them with `ctest -LE device` rather than relying on that skip, so a
  future failure cannot hide behind "no device connected".

## Known limitation: non-ASCII file names

`adb` itself mangles non-ASCII names on a system with a non-UTF-8 ANSI code page
(936 here): `adb push 中文名.txt` run *directly*, with this project out of the
picture, either fails with `remote couldn't create file: Is a directory` or
leaves a truncated name on the device (`中文.`), and Latin-1 names lose their last
byte (`café.txt` → `café.tx`). The CLI only forwards the argument, so this is an
`adb`/environment problem, not something the app can fix here — but the device
test reports what it observes instead of asserting success, so the behaviour is
recorded rather than hidden.

## Not implemented yet

- `pull`/`push` of whole directory trees recursively (adb handles it, but there
  is no progress reporting).
- `logcat` streaming, scrcpy mirroring, wireless pairing.
- The app's own update check/install (the download layer still lives in
  `main.cpp` next to the WinHTTP code).
