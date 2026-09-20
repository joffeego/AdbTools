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
    core/fileio.cpp
    core/package.cpp
    core/paths.cpp
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

add_executable(adb_browser main.cpp regex_wrap.cpp ${ADBTOOLS_CORE_SOURCES} ${ADB_BROWSER_RC})
eui_neo_configure_app(adb_browser)

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
endif()
