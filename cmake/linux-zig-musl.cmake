# CMake toolchain file for building a statically-linked musl variant of
# shimback on Linux, using Zig's bundled musl sysroot (zig cc) instead of
# the host's own glibc toolchain.
#
# Usage:
#   cmake -S . -B build-musl \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/linux-zig-musl.cmake \
#     -DSHIMBACK_ZIG_BIN=<dir containing the zig binary> \
#     -DSHIMBACK_ZIG_TARGET=x86_64-linux-musl   # or aarch64-linux-musl
#
# Zig over musl-tools/apt: `zig cc` bundles its own versioned musl sysroot
# (headers and libs), the same "fetch our own pinned toolchain rather than
# rely on whatever the OS package manager happens to carry" choice already
# made for Windows (see windows-clang-cl.cmake's own xwin-splatted sysroot).
# In CI this still runs natively (Linux host building a Linux-musl target,
# same arch, just a different libc) -- the binary is run and tested
# (`ctest`) on the same runner that built it, unlike a genuine cross-arch
# cross-compile. CMAKE_SYSTEM_NAME is still set to Linux explicitly below,
# though, confirmed necessary the hard way when first testing this
# toolchain file from a macOS host (for local iteration, before trusting
# it to CI): without it, CMake assumes the *host's* OS and injects
# macOS-specific default flags (-arch, -isysroot) that zig cc rejects
# outright for a Linux target. Even on a real Linux CI host this is the
# technically-correct thing for a toolchain file to declare -- it
# shouldn't rely on "happens to already match the host" to get the right
# default flags.
#
# Toolchain files are evaluated *before* project() -- same reason as
# windows-clang-cl.cmake: CMake's own internal compiler-check try_compile
# (part of project() itself) needs CMAKE_C_COMPILER already pointed at the
# right thing, not just targets added afterward.

set(CMAKE_SYSTEM_NAME Linux)

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
  SHIMBACK_ZIG_BIN
  SHIMBACK_ZIG_TARGET
)

if(NOT DEFINED SHIMBACK_ZIG_TARGET)
  if(DEFINED ENV{SHIMBACK_ZIG_TARGET})
    set(SHIMBACK_ZIG_TARGET "$ENV{SHIMBACK_ZIG_TARGET}")
  else()
    set(SHIMBACK_ZIG_TARGET "x86_64-linux-musl")
  endif()
endif()

if(NOT DEFINED SHIMBACK_ZIG_BIN)
  if(DEFINED ENV{SHIMBACK_ZIG_BIN})
    set(SHIMBACK_ZIG_BIN "$ENV{SHIMBACK_ZIG_BIN}")
  else()
    message(FATAL_ERROR
      "Building the musl variant needs Zig -- pass -DSHIMBACK_ZIG_BIN=<dir> "
      "(or set the SHIMBACK_ZIG_BIN environment variable) pointing at the "
      "directory containing the 'zig' binary.")
  endif()
endif()

# `zig cc -target ... "$@"` isn't a single executable CMake can point
# CMAKE_C_COMPILER at directly (unlike clang-cl) -- a tiny wrapper script is
# the standard fix for this exact "zig cc + CMake" combination. Written
# into the build directory (available even this early -- CMAKE_BINARY_DIR
# is set from the `-B` argument before any toolchain-file code runs) so a
# fresh build dir always regenerates it, rather than a stale copy living
# outside the build tree and drifting from SHIMBACK_ZIG_TARGET.
set(SHIMBACK_ZIGCC_WRAPPER "${CMAKE_BINARY_DIR}/zigcc-${SHIMBACK_ZIG_TARGET}.sh")
file(WRITE "${SHIMBACK_ZIGCC_WRAPPER}"
  "#!/bin/sh\nexec \"${SHIMBACK_ZIG_BIN}/zig\" cc -target ${SHIMBACK_ZIG_TARGET} \"$@\"\n")
execute_process(COMMAND chmod +x "${SHIMBACK_ZIGCC_WRAPPER}")

set(CMAKE_C_COMPILER "${SHIMBACK_ZIGCC_WRAPPER}" CACHE FILEPATH "")

# Static linking is the entire point of this variant (a single binary with
# zero runtime dependencies, portable to any Linux regardless of its own
# glibc/musl version) -- kept as a linker flag here rather than baked into
# the wrapper script itself, matching windows-clang-cl.cmake's own split
# between compile-time flags (CMAKE_C_FLAGS_INIT) and link-time ones
# (CMAKE_EXE_LINKER_FLAGS_INIT). The "_INIT" suffix matters for the same
# reason documented there: it must be visible to CMake's own internal
# compiler-check try_compile during project(), not just to targets added
# afterward.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
