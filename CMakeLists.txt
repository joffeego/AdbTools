cmake_minimum_required(VERSION 3.14)
project(AdbFileBrowser LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use the vendored third-party sources under 3rd/EUI-NEO/3rd (no network fetch).
set(EUI_DEPS_MODE bundled CACHE STRING "" FORCE)
set(EUI_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(EUI_BUILD_USER_APPS OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(EUI_ENABLE_TRAY OFF CACHE BOOL "" FORCE)

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

add_executable(adb_browser main.cpp regex_wrap.cpp ${ADB_BROWSER_RC})
eui_neo_configure_app(adb_browser)

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
