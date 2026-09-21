# Fails the build when the executable imports a DLL that does not ship with
# Windows.
#
# Why this exists: the framework links libcurl whenever CMake's FindCURL
# succeeds, and MSYS2's cmake package drags libcurl into the MINGW64 tree. The
# release build therefore produced an exe importing libcurl-4.dll - not a Windows
# component and not shipped - so every install on a clean machine died with a
# "missing DLL" error, while local builds (no libcurl installed) were fine.
# CMakeLists.txt now disables the CURL search outright; this check is the
# backstop that turns any future accidental dependency into a build failure here
# rather than a bug report from a user.
#
# Invoked from CMakeLists.txt as a POST_BUILD step:
#   cmake -DOBJDUMP=<objdump> -DEXE=<exe> -P cmake/check_pe_imports.cmake

cmake_minimum_required(VERSION 3.14)

if(NOT DEFINED EXE OR NOT EXISTS "${EXE}")
    message(FATAL_ERROR "check_pe_imports: executable not found: '${EXE}'")
endif()

if(NOT DEFINED OBJDUMP OR NOT EXISTS "${OBJDUMP}")
    # MSVC toolchains have no objdump; the shipped builds are MinGW, so a
    # warning is enough here and the release workflow asserts this separately.
    message(WARNING
        "check_pe_imports: objdump not found, skipping the DLL import check for ${EXE}")
    return()
endif()

execute_process(
    COMMAND "${OBJDUMP}" -p "${EXE}"
    OUTPUT_VARIABLE dump
    ERROR_VARIABLE dumpError
    RESULT_VARIABLE dumpResult
)
if(NOT dumpResult EQUAL 0)
    message(FATAL_ERROR "check_pe_imports: objdump failed (${dumpResult}): ${dumpError}")
endif()

string(REGEX MATCHALL "DLL Name: [^\r\n]+" dllLines "${dump}")

# DLLs that ship with Windows and may legitimately be imported. Anything else has
# to be linked statically or shipped alongside the exe, because a user's machine
# cannot be assumed to have it.
set(allowedSystemDlls
    advapi32 bcrypt cfgmgr32 comctl32 comdlg32 crypt32 d3d11 dbghelp dnsapi
    dwmapi dxgi gdi32 glu32 hid imm32 iphlpapi kernel32 mf mfplat mfreadwrite
    msvcrt netapi32 ntdll ole32 oleaut32 opengl32 pdh propsys psapi rpcrt4
    secur32 setupapi shell32 shlwapi urlmon user32 userenv uxtheme version
    winhttp wininet winmm winspool ws2_32
    # Only relevant if the MSVC path is ever used; these come from the VC
    # redistributable, which the installer would have to ship.
    msvcp140 vcruntime140 vcruntime140_1
)

set(foreignDlls "")
foreach(line IN LISTS dllLines)
    string(REGEX REPLACE "^DLL Name: " "" name "${line}")
    string(STRIP "${name}" name)
    string(TOLOWER "${name}" lowered)

    # ucrtbase.dll and the api-ms-win-*/ext-ms-win-* API sets are OS components.
    if(lowered MATCHES "^(api-ms-win-|ext-ms-win-)" OR lowered STREQUAL "ucrtbase.dll")
        continue()
    endif()

    string(REGEX REPLACE "\\.dll$" "" stem "${lowered}")
    if(NOT stem IN_LIST allowedSystemDlls)
        list(APPEND foreignDlls "${name}")
    endif()
endforeach()

if(foreignDlls)
    list(REMOVE_DUPLICATES foreignDlls)
    list(SORT foreignDlls)
    string(REPLACE ";" ", " joined "${foreignDlls}")
    message(FATAL_ERROR
        "${EXE}\n"
        "  imports DLLs that are not part of Windows: ${joined}\n"
        "  On a machine without those DLLs the program fails to start with a\n"
        "  \"missing DLL\" error. Link the dependency statically, ship the DLL next\n"
        "  to the exe, or - only if it really is an OS component - add it to the\n"
        "  allowlist in cmake/check_pe_imports.cmake.")
endif()

list(LENGTH dllLines importCount)
message(STATUS "check_pe_imports: ${EXE} imports ${importCount} system DLLs, all accounted for")
