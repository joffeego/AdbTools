"""Add a UTF-8 BOM to the PowerShell scripts that contain non-ASCII text.

Windows PowerShell 5.1 reads a .ps1 as ANSI (the system code page) unless it
starts with a UTF-8 BOM. With Chinese strings inside, that mangles the text badly
enough to break the parser - which is exactly what happened to
tests/device_tests.ps1 before this ran.

Run this after editing such a script.
"""
import io
import os

TARGETS = ["tests/device_tests.ps1", "tests/run_device_tests.ps1"]
BOM = b"\xef\xbb\xbf"


def main() -> int:
    for path in TARGETS:
        if not os.path.exists(path):
            print("skip (missing): %s" % path)
            continue
        with io.open(path, "rb") as fh:
            data = fh.read()
        if data.startswith(BOM):
            print("already has a BOM: %s" % path)
            continue
        with io.open(path, "wb") as fh:
            fh.write(BOM + data)
        print("added a UTF-8 BOM: %s" % path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
