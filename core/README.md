# core/ — the app's pure logic

Everything in this directory is **plain C++17 with no GUI, no platform state and
no app globals**. That single rule is what makes the code testable, and it is
enforced by CI (`core-purity` job) rather than by good intentions.

## Why this exists

`main.cpp` used to hold everything: window layout, adb calls, update parsing and
file-system helpers, all inside one anonymous namespace. Two consequences:

1. **Nothing could be unit tested.** The only way to check a parser or a
   comparison was to run the GUI against a real device and watch.
2. **The parsers were already buggy.** While writing the tests for this code,
   three real defects surfaced immediately:
   - `parseSize()` accepted negative numbers (`from_chars` is signed by default),
     even though it parses the size column of `ls -la` and the app uses negative
     values internally as an "unknown" sentinel.
   - `adbShortVersion()` only matched a capitalised `"Version "`, so it missed
     `"Android Debug Bridge version 1.0.41"` — the format a real adb actually
     prints.
   - The updater's sha256-sidecar fallback could not be exercised at all, so the
     fact that it silently drops unattributable digests was never verified.

## Rules for anything added here

- No `#include "eui_neo.h"`, no `<windows.h>`, no GLFW, no `<regex>` (the app is
  built `-fno-exceptions`; exceptions are isolated in `regex_wrap.cpp`).
  Windows-specific hashing is the one documented exception, guarded by
  `#ifdef _WIN32` in `sha256.cpp`, because CNG has no portable equivalent.
- No access to `state`, `settings` or any other global. Pass what you need in.
- No threads, no UI, no process spawning. File I/O is allowed only where the
  header says so (`fileio.h`) and must not throw.
- If a function needs "app context" to make sense, it does not belong here yet —
  split it instead.

## Files

| File | Contents |
| --- | --- |
| `strings.h/.cpp` | `trim`, `splitWs`, `lower`, `shorten`, `formatSize`, `isMonthName`, `parseSize` |
| `paths.h/.cpp` | device-side paths: `shellQuote`, `joinPath`, `parentPath`, `splitPath` |
| `version.h/.cpp` | version parsing/comparison, `adbShortVersion`, `scrcpyShortVersion` |
| `package.h/.cpp` | GitHub release JSON and Google repository XML parsers |
| `sha256.h/.cpp` | sha256sum-sidecar parsing, file hashing (CNG) |
| `fileio.h/.cpp` | `writeFileAtomic` |

## Tests

`tests/` holds one file per core module, run by `ctest` (or directly via
`build/adbtools_tests.exe`). There is no third-party framework: `tests/test_main.h`
is a small harness with `ADB_TEST` / `ADB_CHECK` / `ADB_CHECK_EQ`.

When adding core code, add cases for the boundaries you were unsure about — the
existing tests are mostly the edge cases that the GUI never exercised.

## Not done yet

The next steps (see the maintenance guide's roadmap) are to move the remaining
pure logic here:

- `core/adb.h/.cpp` — the push / pull / delete / list command builders and their
  output parsing (currently still in `main.cpp`, interleaved with async plumbing)
- `core/store.h/.cpp` — settings / bookmarks / commands / recent-paths
  serialisation (the writers are already atomic; the keys and formats are not yet
  isolated)

Once those exist, both a CLI and an end-to-end test driver become thin layers on
top instead of new copies of the logic.
