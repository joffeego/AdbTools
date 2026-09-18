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

add_executable(adb_browser main.cpp regex_wrap.cpp)
eui_neo_configure_app(adb_browser)

# regex_wrap.cpp uses std::regex, whose constructor throws on invalid patterns.
# The app template compiles with -fno-exceptions, so compile this one TU with
# exceptions enabled to isolate that behavior.
set_source_files_properties(regex_wrap.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")

# WinHTTP is used by the built-in adb/scrcpy update checker.
target_link_libraries(adb_browser PRIVATE winhttp)
