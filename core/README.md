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
| `adb.h/.cpp` | device/listing model, `adb` output parsers, every adb command line |
| `store.h/.cpp` | on-disk text formats: settings, bookmarks, quick commands, recent paths |
| `process.h/.cpp` | `runProcess` — the one place a child process is started (GUI + CLI) |
| `adbpath.h/.cpp` | locating this executable and the adb it should drive |
| `appinfo.h` | the version constant, so GUI and CLI cannot report different ones |

## Two categories, on purpose

Most of this directory is **pure logic with unit tests**. Two files are not, and
they are listed here so the exception does not get "fixed" by mistake:

- `process.h/.cpp` spawns processes. It has no unit tests; it is exercised by
  the CLI against a real device. It lives here (rather than in the GUI layer)
  because the GUI and the CLI must share exactly one implementation — this is
  where the process-handling bugs were, and two copies would mean fixing each
  one twice.
- `adbpath.h/.cpp` reads the environment and the filesystem to find adb. Same
  reasoning: the CLI has to resolve adb identically to the GUI.

Both compile without the UI framework, which is what the CI `core-purity` job
enforces.

## Tests

`tests/` holds one file per core module, run by `ctest` (or directly via
`build/adbtools_tests.exe`). There is no third-party framework: `tests/test_main.h`
is a small harness with `ADB_TEST` / `ADB_CHECK` / `ADB_CHECK_EQ`.

Fixtures are copied from real devices wherever possible — the `ls -la` samples,
the `adb devices -l` line and the `pm list packages` output in `tests/test_adb.cpp`
come from an actual Huawei ELS-AN00 — because the interesting cases are exactly
the ones a hand-written sample forgets (that device emits ISO dates, not the
month-name form most examples use).

When adding core code, add cases for the boundaries you were unsure about. The
hash tests are a good model: they check published digests, and the large-input
case is cross-checked against PowerShell's `Get-FileHash` so the updater's
verification cannot silently become a no-op.

## Deliberately still in main.cpp

Not everything worth testing is here yet, and some things belong in the GUI layer
permanently:

- `findAdb()` / `defaultDownloadDir()` — they read the process environment and
  the filesystem, so they are not pure.
- The file *paths* for settings/bookmarks/etc. (they depend on `executableDir()`),
  even though the formats themselves are in `store.h`.
- `runProcess()` — process spawning and handle plumbing is platform work, not
  pure logic.

## Next steps

The remaining pure logic that is still in `main.cpp` and worth moving:

- The batch step drivers (`pullBatchStep` / `pushBatchStep` / `deleteBatchStep`)
  currently mix "what to do for one item" with the async restart plumbing. Pulling
  out the per-item decision (which item, what counts as success, how the summary
  is worded) would make the batching testable — the last time it was changed it
  needed a temporary self-test hook injected into a build-only copy of `main.cpp`
  to verify against a device.

Once that exists, both a CLI and an end-to-end test driver become thin layers on
top instead of new copies of the logic.
