cmake_minimum_required(VERSION 3.21)

project(MiniCCServe
    VERSION 1.0.0
    DESCRIPTION "Mini static file and mock route server for debugging"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Unicode throughout: Win32 macros (ListView_*, LV_ITEM, ...) resolve to W.
# Windows 10+ (GetDpiForWindow, PerMonitorV2, activeCodePage UTF-8 manifest).
add_compile_definitions(UNICODE _UNICODE _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)

# MSVC: static CRT -> no Visual C++ Redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Version-stamped Win32 resources (icon, manifest, version info).
# Forward slashes: MSVC rc.exe treats backslash escapes in quoted paths.
set(RC_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/MiniCCServe.ico")
set(RC_MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest")
configure_file(res/app.rc.in res/app.rc @ONLY)

add_executable(MiniCCServe WIN32
    src/main.cpp
    src/gui.cpp
    src/http.cpp
    src/router.cpp
    src/config.cpp
    src/util.cpp
    vendor/cJSON/cJSON.c
    ${CMAKE_CURRENT_BINARY_DIR}/res/app.rc)

target_include_directories(MiniCCServe PRIVATE src vendor/cJSON)

target_link_libraries(MiniCCServe PRIVATE ws2_32 comctl32 ole32 shell32 iphlpapi user32 gdi32)

if(MSVC)
    target_compile_options(MiniCCServe PRIVATE /W4 /utf-8 /permissive-)
    # The manifest is embedded via app.rc; suppress the linker's own copy.
    target_link_options(MiniCCServe PRIVATE "/MANIFEST:NO")
    # cJSON is vendored third-party code; keep its warning level low.
    set_source_files_properties(vendor/cJSON/cJSON.c PROPERTIES COMPILE_OPTIONS "/W1;/utf-8")
else()
    target_compile_options(MiniCCServe PRIVATE -Wall -Wextra -pedantic)
    # Self-contained exe: statically link libc++/libunwind (llvm-mingw) so the
    # binary runs on machines without the toolchain's DLLs.
    target_link_options(MiniCCServe PRIVATE -static)
    set_source_files_properties(vendor/cJSON/cJSON.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

install(TARGETS MiniCCServe RUNTIME DESTINATION .)
