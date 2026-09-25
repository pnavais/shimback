cmake_minimum_required(VERSION 3.16)

# Must be set before project() to take effect: lets CMAKE_MSVC_RUNTIME_LIBRARY
# (set below, Windows-only) control /MT vs /MD for clang-cl. A no-op for the
# GCC/Clang-native macOS/Linux builds, which don't have an MSVC-style runtime
# to select in the first place.
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SHIMBACK_VERSION LIMIT_COUNT 1)

project(shimback LANGUAGES C VERSION ${SHIMBACK_VERSION})

# clang-cl (targeting the MSVC ABI) is detected this way rather than by
# CMAKE_C_COMPILER_ID alone, which is just "Clang" either way -- see
# windows-port.md's toolchain discussion for why clang-cl was chosen over
# MinGW-w64/plain MSVC. Not just WIN32: cross-compiling *to* Windows from
# another host is a real, intended use (see SHIMBACK_MSVC_SYSROOT below),
# so this must not assume the host itself is Windows.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
  set(SHIMBACK_TARGET_WINDOWS TRUE)
else()
  set(SHIMBACK_TARGET_WINDOWS FALSE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# With extensions off, -std=c11 is strict ISO C -- glibc (unlike macOS's
# libc, which exposes these regardless) hides POSIX-only declarations
# (strtok_r, clock_gettime/CLOCK_MONOTONIC, ...) behind a feature-test
# macro in that mode. Without it, strtok_r in particular isn't just a
# warning: it's implicitly declared as returning int, silently truncating
# the real pointer it returns on any 64-bit platform. POSIX.1-2008 covers
# everything this project uses.
#
# Explicitly defining _POSIX_C_SOURCE has the opposite effect on macOS's
# own libc: it *restricts* visibility to strict POSIX, hiding Darwin/BSD
# extensions (mkdtemp among them) that are normally exposed by default.
# _DARWIN_C_SOURCE restores those on top of the same strict POSIX baseline
# -- a no-op on Linux, where glibc doesn't look at it at all.
#
# _XOPEN_SOURCE=700 (the XSI/SUSv4 counterpart to POSIX.1-2008) is needed
# alongside _POSIX_C_SOURCE on glibc for realpath()'s prototype specifically
# -- glibc's own feature-test-macro logic doesn't expose it from
# _POSIX_C_SOURCE alone, unlike strtok_r/clock_gettime/etc. above.
#
# None of this applies on Windows -- these are POSIX feature-test macros,
# meaningless (and, for _POSIX_C_SOURCE specifically, actively wrong) against
# the Microsoft UCRT headers clang-cl compiles against there.
if(NOT SHIMBACK_TARGET_WINDOWS)
  add_compile_definitions(_POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

option(SHIMBACK_DEV_WARNINGS "Build with -Wall -Wextra -Werror" ${CMAKE_BUILD_TYPE_IS_DEBUG})
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(SHIMBACK_DEV_WARNINGS ON)
endif()

# --- Windows (clang-cl) toolchain wiring ------------------------------------
# See windows-port.md Phase 1. Sysroot/target-triple detection (from
# SHIMBACK_MSVC_SYSROOT, an xwin-splatted MSVC/Windows SDK tree -- see
# https://github.com/Jake-Shadle/xwin) lives in cmake/windows-clang-cl.cmake,
# a toolchain file evaluated *before* project() -- required so CMake's own
# internal compiler-check try-compile (which runs as part of project()
# itself) already has the right /I and /LIBPATH: flags; setting them only
# here, after project(), is too late for that one internal check even
# though it would be fine for every target built afterward. That file also
# defines SHIMBACK_WIN_INCLUDE_DIRS, read below for the win32-compat getopt
# shim's own include path.
if(SHIMBACK_TARGET_WINDOWS)
  # CMAKE_MSVC_RUNTIME_LIBRARY (/MT, always the release static CRT) is set
  # in the toolchain file, not here -- see its own comment for why it has
  # to be set that early.

  # The sysroot's headers are already exempted from clang's pedantic
  # warnings via -imsvc (system-header treatment), not per-warning
  # suppression here -- see the toolchain file's own comment on
  # SHIMBACK_WIN_C_FLAGS_INIT for the (long) list of warning categories
  # that would otherwise fire on Microsoft's own headers.

  # Force-included into every translation unit -- see the header's own
  # comment for why this (S_ISREG/S_ISDIR) needs /FI rather than the
  # win32-compat include-path trick getopt.h/unistd.h use.
  add_compile_options("/FI${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32-compat/shimback_prelude.h")
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
  @ONLY
)

add_executable(shimback
  src/main.c
  src/cli.c
  src/config.c
  src/dispatch.c
  src/shell.c
  src/installation.c
  src/sha256.c
  src/paths.c
  src/suggest.c
  src/tui.c
  src/util.c
  src/commands/add.c
  src/commands/add_wizard.c
  src/commands/remove.c
  src/commands/init.c
  src/commands/list.c
  src/commands/doctor.c
  src/commands/install.c
  src/commands/uninstall.c
  src/commands/info.c
  src/commands/update.c
)

if(SHIMBACK_TARGET_WINDOWS)
  target_sources(shimback PRIVATE src/platform/platform_win32.c src/platform/getopt_win32.c)
else()
  target_sources(shimback PRIVATE src/platform/platform_posix.c)
  # edit.c uses wordexp() (no Windows equivalent) and an exec-fails-in-child
  # fallback-chain idiom that needs a real PATH-search-first redesign for
  # Windows, not a mechanical port -- deliberately deferred, see
  # windows-port.md's Phase 0 notes. Excluded from the Windows build
  # entirely rather than partially ported.
  target_sources(shimback PRIVATE src/commands/edit.c)
endif()

target_include_directories(shimback PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

if(SHIMBACK_TARGET_WINDOWS)
  # Only for Windows, and only this one directory: a bare `#include
  # <getopt.h>` in the existing source files (unchanged) resolves to the
  # vendored one here since the UCRT headers have no getopt of their own --
  # see win32-compat/getopt.h's own comment. Never added on macOS/Linux, so
  # it can never shadow a real system <getopt.h> there. (The sysroot's own
  # include/lib paths don't need repeating here -- the toolchain file
  # already applies them globally via CMAKE_C_FLAGS_INIT/
  # CMAKE_EXE_LINKER_FLAGS_INIT, per its own top-of-file comment.)
  target_include_directories(shimback PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32-compat")
  # advapi32: registry access (cmd.exe AutoRun, HKCU\Environment\Path --
  # Phase 5). user32: SendMessageTimeoutW, broadcasting a PATH change to
  # already-running processes (Phase 5). psapi/kernel32's Toolhelp32
  # snapshot functions (parent-process detection, also Phase 5) are already
  # available via kernel32, linked by default.
  target_link_libraries(shimback PRIVATE shell32 ole32 advapi32 user32)
endif()

if(SHIMBACK_DEV_WARNINGS)
  target_compile_options(shimback PRIVATE -Wall -Wextra -Werror)
endif()

install(TARGETS shimback RUNTIME DESTINATION bin)
install(FILES man/shimback.1 DESTINATION share/man/man1)

enable_testing()

add_executable(test_config_parser
  tests/test_config_parser.c
  src/config.c
  src/paths.c
  src/util.c
)

if(SHIMBACK_TARGET_WINDOWS)
  target_sources(test_config_parser PRIVATE src/platform/platform_win32.c src/platform/getopt_win32.c)
else()
  target_sources(test_config_parser PRIVATE src/platform/platform_posix.c)
endif()

target_include_directories(test_config_parser PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

if(SHIMBACK_TARGET_WINDOWS)
  target_include_directories(test_config_parser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32-compat")
  target_link_libraries(test_config_parser PRIVATE shell32 ole32 advapi32 user32)
endif()
add_test(NAME test_config_parser COMMAND test_config_parser)

add_test(NAME test_dispatch
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dispatch.sh $<TARGET_FILE:shimback>)
add_test(NAME test_add_remove
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_add_remove.sh $<TARGET_FILE:shimback>)
add_test(NAME test_init
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_init.sh $<TARGET_FILE:shimback>)
add_test(NAME test_doctor
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_doctor.sh $<TARGET_FILE:shimback>)
add_test(NAME test_install
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install.sh $<TARGET_FILE:shimback>)
add_test(NAME test_uninstall
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_uninstall.sh $<TARGET_FILE:shimback>)
add_test(NAME test_add_wizard
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_add_wizard.sh $<TARGET_FILE:shimback>)
# This is the one test that needs python3 (to drive the interactive wizard
# through a real pseudo-terminal) -- not something the README lists as a
# build/run prerequisite for shimback itself. Rather than fail the whole
# suite on a python3-less machine, the script exits 77 (the conventional
# "skipped" code) when it's missing, and ctest reports this test as
# skipped, not failed, when it sees that same code back.
set_tests_properties(test_add_wizard PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME test_list
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_list.sh $<TARGET_FILE:shimback>)
add_test(NAME test_edit
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_edit.sh $<TARGET_FILE:shimback>)
add_test(NAME test_split_config
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_split_config.sh $<TARGET_FILE:shimback>)
add_test(NAME test_route_map
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_route_map.sh $<TARGET_FILE:shimback>)
add_test(NAME test_info
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_info.sh $<TARGET_FILE:shimback>)
add_test(NAME test_update
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update.sh $<TARGET_FILE:shimback>)
# Exits 77 (skipped, not failed) where curl/tar/a checksum tool are missing.
set_tests_properties(test_update PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME test_help
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_help.sh $<TARGET_FILE:shimback>)
