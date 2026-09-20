cmake_minimum_required(VERSION 3.14)
project(AdbFileBrowser LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Use the vendored third-party sources under 3rd/EUI-NEO/3rd (no network fetch).
set(EUI_DEPS_MODE bundled CACHE STRING "" FORCE)
set(EUI_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(EUI_BUILD_USER_APPS OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_TRAY OFF CACHE BOOL "" FORCE)

# -----------------------------------------------------------------------------
# Core: the app's pure logic, with no dependency on the GUI framework.
#
# These files were extracted from main.cpp's anonymous namespace so the logic
# can be unit tested (and, later, driven from a CLI). They must never include
# eui_neo.h, touch the app's global state, or perform I/O beyond what their own
# header documents - that is what keeps them testable.
# -----------------------------------------------------------------------------
set(ADBTOOLS_CORE_SOURCES
    core/adb.cpp
    core/adbpath.cpp
    core/fileio.cpp
    core/package.cpp
    core/paths.cpp
    core/process.cpp
    core/sha256.cpp
    core/store.cpp
    core/strings.cpp
    core/version.cpp
)

# Include the repository root so core headers are found as "core/...".
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(3rd/EUI-NEO)

# Windows resource script: embeds VS_VERSION_INFO (company/product/version
# metadata) and the application manifest. Compiled by windres on MinGW and
# rc.exe on MSVC; both understand a literal .rc source. CMake's own RC
# preprocessing is disabled because MSVC's rc.exe cannot cope with the
# UTF-8 BOM CMake would add.
if(WIN32)
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_RC_SOURCE_FILE_EXTENSIONS "")
    set(ADB_BROWSER_RC adb_browser.rc)
else()
    set(ADB_BROWSER_RC "")
endif()

# One executable, two modes: app_main.cpp routes to the CLI when arguments are
# present and to the GUI otherwise. EUI_APP_RUNNER_LIBRARY makes the framework's
# own main() in core/app/glfw_app_main.cpp compile out, so no vendored file needs
# editing for this.
add_executable(adb_browser
    app_main.cpp
    main.cpp
    cli/cli.cpp
    regex_wrap.cpp
    ${ADBTOOLS_CORE_SOURCES}
    ${ADB_BROWSER_RC}
)
eui_neo_configure_app(adb_browser)

# The framework's app entry (core/app/glfw_app_main.cpp) ends with
#
#     #ifndef EUI_APP_RUNNER_LIBRARY
#     int main() { return eui_app_run(); }
#     #endif
#
# so defining the macro on THAT file compiles the framework's main() out and
# leaves eui_app_run() available for app_main.cpp to call. Defining it on
# app_main.cpp instead (or on the whole target) does nothing useful: the first
# leaves two main()s, the second also removes eui_app_run().
set(ADBTOOLS_FRAMEWORK_ENTRY "${CMAKE_CURRENT_SOURCE_DIR}/3rd/EUI-NEO/core/app/glfw_app_main.cpp")
if(EXISTS "${ADBTOOLS_FRAMEWORK_ENTRY}")
    set_source_files_properties("${ADBTOOLS_FRAMEWORK_ENTRY}" PROPERTIES
        COMPILE_DEFINITIONS "EUI_APP_RUNNER_LIBRARY=1")
else()
    message(FATAL_ERROR
        "The framework app entry point moved: ${ADBTOOLS_FRAMEWORK_ENTRY} not found. "
        "app_main.cpp relies on defining EUI_APP_RUNNER_LIBRARY there to replace main().")
endif()

# cli/ is compiled like core/ (normal C++), not with the app template's
# -fno-exceptions: it uses std::filesystem and std::string operations whose
# failures are exceptions, and a CLI that terminate()s on a bad path is useless.
set_source_files_properties(cli/cli.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions;-fno-rtti")

# The app template compiles with -fno-exceptions -fno-rtti and -Os, which suits
# the GUI but is deliberately NOT applied to the core or the tests: they are
# compiled with the project's normal flags so they behave like ordinary C++
# (and so a std::filesystem failure is an error_code, not a terminate()).
set_source_files_properties(${ADBTOOLS_CORE_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-fexceptions;-fno-rtti"
)

# regex_wrap.cpp uses std::regex, whose constructor throws on invalid patterns.
# The app template compiles with -fno-exceptions, so compile this one TU with
# exceptions enabled to isolate that behavior.
set_source_files_properties(regex_wrap.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")

# WinHTTP drives the built-in adb/scrcpy/app update checker; bcrypt (CNG)
# provides the SHA-256/SHA-1 verification of the downloaded packages.
target_link_libraries(adb_browser PRIVATE winhttp bcrypt)

# Harden the produced PE. MinGW does not enable these by default, and a
# non-ASLR binary both looks suspicious to antivirus heuristics and is a real
# security weakness. Kept MinGW-only so the MSVC path is unaffected.
if(MINGW)
    target_link_options(adb_browser PRIVATE
        -Wl,--dynamicbase
        -Wl,--nxcompat
        -Wl,--high-entropy-va
    )
endif()

# -----------------------------------------------------------------------------
# Unit tests for the core logic. No third-party test framework: tests/test_main.h
# is a ~100 line harness, which keeps the build dependency-free and lets the same
# files run under plain `ctest`.
# -----------------------------------------------------------------------------
option(ADBTOOLS_BUILD_TESTS "Build the core unit tests" ON)

if(ADBTOOLS_BUILD_TESTS)
    enable_testing()

    add_executable(adbtools_tests
        ${ADBTOOLS_CORE_SOURCES}
        tests/test_main.cpp
        tests/test_adb.cpp
        tests/test_fileio.cpp
        tests/test_package.cpp
        tests/test_paths.cpp
        tests/test_sha256.cpp
        tests/test_store.cpp
        tests/test_strings.cpp
        tests/test_version.cpp
    )
    # The core sources are compiled twice on purpose: the app target has the
    # framework's flags applied to it, and the test target must stay free of
    # them (and must not link the GUI).
    if(WIN32)
        target_link_libraries(adbtools_tests PRIVATE bcrypt)
    endif()

    add_test(NAME core COMMAND adbtools_tests)
    set_tests_properties(core PROPERTIES LABELS "unit")

    # -------------------------------------------------------------------------
    # Device end-to-end tests: drive the real executable's CLI against a real
    # Android device. These cover what the unit tests cannot - that the commands,
    # the adb invocation and the parsers work together on a phone.
    #
    # They self-skip (exit 0) when no device is connected, so `ctest` stays green
    # on a machine without one. CI excludes them explicitly with
    # `ctest -LE device` because a runner has no device and the skip would
    # otherwise hide a genuine failure.
    # -------------------------------------------------------------------------
    find_program(ADBTOOLS_PWSH NAMES pwsh powershell)
    if(WIN32 AND ADBTOOLS_PWSH)
        # Windows PowerShell 5.1 reads a .ps1 without a UTF-8 BOM as ANSI, which
        # mangles the Chinese text in this script badly enough to break its
        # parser. Fail loudly at configure time rather than letting the test fail
        # with a confusing syntax error; scripts/fix_ps1_bom.py repairs it.
        set(ADBTOOLS_DEVICE_TEST "${CMAKE_CURRENT_SOURCE_DIR}/tests/device_tests.ps1")
        file(READ "${ADBTOOLS_DEVICE_TEST}" ADBTOOLS_DEVICE_TEST_HEAD LIMIT 3 HEX)
        if(NOT ADBTOOLS_DEVICE_TEST_HEAD STREQUAL "efbbbf")
            message(FATAL_ERROR
                "tests/device_tests.ps1 lost its UTF-8 BOM. Run "
                "`python scripts/fix_ps1_bom.py` - without it PowerShell 5.1 reads "
                "the file as ANSI and the Chinese strings break the parser.")
        endif()

        add_test(NAME device
            COMMAND "${ADBTOOLS_PWSH}" -NoProfile -ExecutionPolicy Bypass
                    -File "${ADBTOOLS_DEVICE_TEST}"
                    -Bin "${CMAKE_CURRENT_BINARY_DIR}/adb_browser.exe")
        set_tests_properties(device PROPERTIES
            LABELS "device"
            TIMEOUT 600)
    endif()
endif()
