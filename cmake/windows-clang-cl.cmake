# CMake toolchain file for building shimback on Windows with clang-cl +
# lld-link, targeting the MSVC ABI without a full Visual Studio install --
# see windows-port.md Phase 1 for the full toolchain discussion.
#
# Usage:
#   cmake -S . -B build-windows -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/windows-clang-cl.cmake \
#     -DSHIMBACK_MSVC_SYSROOT=<path to an xwin-splatted sysroot for one arch> \
#     -DSHIMBACK_WIN_TARGET=x86_64-pc-windows-msvc   # or aarch64-pc-windows-msvc
#
# Toolchain files are evaluated *before* project() -- deliberately, so that
# CMake's own internal "does this compiler/linker work" check (which runs
# as part of project() itself, before anything in the main CMakeLists.txt
# body executes) already has the right /I and /LIBPATH: flags. Setting
# these only *after* project() (e.g. via add_link_options in
# CMakeLists.txt) is too late for that internal check specifically, even
# though it's fine for every target built afterward -- this was found the
# hard way: the compiler-check step failed with "could not open
# 'kernel32.lib'" until the sysroot paths moved here.

set(CMAKE_SYSTEM_NAME Windows)

# CMake's own internal compiler-check try_compile (see the comment above)
# runs as a separate, nested cmake invocation that does NOT automatically
# forward arbitrary user -D cache variables -- only ones explicitly listed
# here. Without this, SHIMBACK_MSVC_SYSROOT/SHIMBACK_LLVM_BIN/
# SHIMBACK_WIN_TARGET are invisible inside that nested project even though
# they're set on the outer `cmake -S . -B ...` command line, and this same
# toolchain file (re-evaluated for the nested project too) then fails its
# own "is this defined" checks below.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
  SHIMBACK_MSVC_SYSROOT
  SHIMBACK_LLVM_BIN
  SHIMBACK_WIN_TARGET
)

if(NOT DEFINED SHIMBACK_WIN_TARGET)
  if(DEFINED ENV{SHIMBACK_WIN_TARGET})
    set(SHIMBACK_WIN_TARGET "$ENV{SHIMBACK_WIN_TARGET}")
  else()
    set(SHIMBACK_WIN_TARGET "x86_64-pc-windows-msvc")
  endif()
endif()
set(CMAKE_C_COMPILER_TARGET "${SHIMBACK_WIN_TARGET}" CACHE STRING "")
string(REGEX MATCH "^[^-]+" SHIMBACK_WIN_ARCH "${SHIMBACK_WIN_TARGET}")

# Looked up on PATH by default (that's where `winget install LLVM.LLVM`
# puts them); override with -DSHIMBACK_LLVM_BIN=<dir> if installed
# elsewhere or not on PATH for this shell.
if(NOT DEFINED SHIMBACK_LLVM_BIN)
  if(DEFINED ENV{SHIMBACK_LLVM_BIN})
    set(SHIMBACK_LLVM_BIN "$ENV{SHIMBACK_LLVM_BIN}")
  else()
    set(SHIMBACK_LLVM_BIN "")
  endif()
endif()
if(SHIMBACK_LLVM_BIN)
  set(CMAKE_C_COMPILER "${SHIMBACK_LLVM_BIN}/clang-cl.exe" CACHE FILEPATH "")
  set(CMAKE_LINKER "${SHIMBACK_LLVM_BIN}/lld-link.exe" CACHE FILEPATH "")
  set(CMAKE_RC_COMPILER "${SHIMBACK_LLVM_BIN}/llvm-rc.exe" CACHE FILEPATH "")
  set(CMAKE_MT "${SHIMBACK_LLVM_BIN}/llvm-mt.exe" CACHE FILEPATH "")
else()
  set(CMAKE_C_COMPILER clang-cl CACHE FILEPATH "")
  set(CMAKE_LINKER lld-link CACHE FILEPATH "")
endif()

if(NOT DEFINED SHIMBACK_MSVC_SYSROOT)
  if(DEFINED ENV{SHIMBACK_MSVC_SYSROOT})
    set(SHIMBACK_MSVC_SYSROOT "$ENV{SHIMBACK_MSVC_SYSROOT}")
  else()
    message(FATAL_ERROR
      "Building for Windows with clang-cl needs an xwin-splatted MSVC/Windows SDK "
      "sysroot -- pass -DSHIMBACK_MSVC_SYSROOT=<path> (or set the SHIMBACK_MSVC_SYSROOT "
      "environment variable) pointing at the architecture-specific splat root (the "
      "directory directly containing VC/ and 'Windows Kits/'). See windows-port.md Phase 1.")
  endif()
endif()

# Sysroot layout, one splat per architecture (see windows-port.md Phase 1)
# -- xwin's own splat layout has changed across versions, confirmed by
# hitting both in practice, so both are supported here:
#   New (xwin >= 0.10, confirmed via a real GitHub Actions windows-2025
#   run -- see windows-port.md Phase 7's CI addendum): flat, no version
#   directory at all:
#     <sysroot>/crt/{include,lib/<arch>}
#     <sysroot>/sdk/{include,lib}/{ucrt,um,shared}[/<arch>]
#   Old (whatever xwin version produced this project's original local dev
#   sysroot, mimicking the official installer's own layout with a version
#   directory):
#     <sysroot>/VC/Tools/MSVC/<ver>/{include,lib/<arch>}
#     <sysroot>/Windows Kits/10/{Include,Lib}/<winkit-ver>/{ucrt,um,shared}[/<arch>]
if(EXISTS "${SHIMBACK_MSVC_SYSROOT}/crt/include")
  set(SHIMBACK_WIN_INCLUDE_DIRS
    "${SHIMBACK_MSVC_SYSROOT}/crt/include"
    "${SHIMBACK_MSVC_SYSROOT}/sdk/include/ucrt"
    "${SHIMBACK_MSVC_SYSROOT}/sdk/include/um"
    "${SHIMBACK_MSVC_SYSROOT}/sdk/include/shared"
    CACHE INTERNAL "")
  set(SHIMBACK_WIN_LIB_DIRS
    "${SHIMBACK_MSVC_SYSROOT}/crt/lib/${SHIMBACK_WIN_ARCH}"
    "${SHIMBACK_MSVC_SYSROOT}/sdk/lib/ucrt/${SHIMBACK_WIN_ARCH}"
    "${SHIMBACK_MSVC_SYSROOT}/sdk/lib/um/${SHIMBACK_WIN_ARCH}"
    CACHE INTERNAL "")
else()
  file(GLOB SHIMBACK_MSVC_VER_DIRS LIST_DIRECTORIES true "${SHIMBACK_MSVC_SYSROOT}/VC/Tools/MSVC/*")
  file(GLOB SHIMBACK_WINKIT_VER_DIRS LIST_DIRECTORIES true "${SHIMBACK_MSVC_SYSROOT}/Windows Kits/10/Include/*")
  list(LENGTH SHIMBACK_MSVC_VER_DIRS SHIMBACK_MSVC_VER_COUNT)
  list(LENGTH SHIMBACK_WINKIT_VER_DIRS SHIMBACK_WINKIT_VER_COUNT)
  if(NOT SHIMBACK_MSVC_VER_COUNT EQUAL 1 OR NOT SHIMBACK_WINKIT_VER_COUNT EQUAL 1)
    message(FATAL_ERROR
      "Expected either the new xwin layout ('${SHIMBACK_MSVC_SYSROOT}/crt/include') or "
      "exactly one MSVC version under '${SHIMBACK_MSVC_SYSROOT}/VC/Tools/MSVC' and one "
      "Windows Kit version under '${SHIMBACK_MSVC_SYSROOT}/Windows Kits/10/Include' -- "
      "found neither the former nor (${SHIMBACK_MSVC_VER_COUNT}, ${SHIMBACK_WINKIT_VER_COUNT}) "
      "of the latter. Re-splat the sysroot with xwin if it has stale/multiple versions mixed in.")
  endif()
  list(GET SHIMBACK_MSVC_VER_DIRS 0 SHIMBACK_MSVC_VER_DIR)
  list(GET SHIMBACK_WINKIT_VER_DIRS 0 SHIMBACK_WINKIT_VER_DIR)
  get_filename_component(SHIMBACK_WINKIT_VER "${SHIMBACK_WINKIT_VER_DIR}" NAME)

  set(SHIMBACK_WIN_INCLUDE_DIRS
    "${SHIMBACK_MSVC_VER_DIR}/include"
    "${SHIMBACK_MSVC_SYSROOT}/Windows Kits/10/Include/${SHIMBACK_WINKIT_VER}/ucrt"
    "${SHIMBACK_MSVC_SYSROOT}/Windows Kits/10/Include/${SHIMBACK_WINKIT_VER}/um"
    "${SHIMBACK_MSVC_SYSROOT}/Windows Kits/10/Include/${SHIMBACK_WINKIT_VER}/shared"
    CACHE INTERNAL "")
  set(SHIMBACK_WIN_LIB_DIRS
    "${SHIMBACK_MSVC_VER_DIR}/lib/${SHIMBACK_WIN_ARCH}"
    "${SHIMBACK_MSVC_SYSROOT}/Windows Kits/10/Lib/${SHIMBACK_WINKIT_VER}/ucrt/${SHIMBACK_WIN_ARCH}"
    "${SHIMBACK_MSVC_SYSROOT}/Windows Kits/10/Lib/${SHIMBACK_WINKIT_VER}/um/${SHIMBACK_WIN_ARCH}"
    CACHE INTERNAL "")
endif()

# -imsvc, not /I: marks these as *system* headers, which makes clang
# exempt them from diagnostics entirely (unlike /I, which puts them on
# equal footing with this project's own code for warning purposes). The
# Windows SDK/MSVC headers trip a long list of clang's pedantic warnings
# that have nothing to do with this project's own code -- reserved
# identifiers, __int64 as a language extension, wchar_t-vs-C++-keyword,
# sal.h appearing in two include roots, and more -- found by hitting each
# one in turn with /I before switching to this, the standard fix for
# exactly this class of problem in clang-cl + xwin setups.
set(SHIMBACK_WIN_C_FLAGS_INIT "")
foreach(_incdir ${SHIMBACK_WIN_INCLUDE_DIRS})
  string(APPEND SHIMBACK_WIN_C_FLAGS_INIT " -imsvc \"${_incdir}\"")
endforeach()
set(SHIMBACK_WIN_LINKER_FLAGS_INIT "")
foreach(_libdir ${SHIMBACK_WIN_LIB_DIRS})
  string(APPEND SHIMBACK_WIN_LINKER_FLAGS_INIT " \"/LIBPATH:${_libdir}\"")
endforeach()

# The "_INIT" suffix is what makes these apply to CMake's own internal
# compiler-check try-compile during project() -- see the comment at the top
# of this file. CMakeLists.txt itself is still free to add more via
# target_compile_options/target_link_options for things that don't need to
# exist that early (warnings, /MT, etc.).
set(CMAKE_C_FLAGS_INIT "${SHIMBACK_WIN_C_FLAGS_INIT}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${SHIMBACK_WIN_LINKER_FLAGS_INIT}")

# Always the release static CRT (/MT via libcmt.lib), even for a
# CMAKE_BUILD_TYPE=Debug shimback build -- deliberate, not just the /MT-
# for-a-single-self-contained-exe choice explained in windows-port.md Phase
# 1: this xwin-splatted sysroot only has the release CRT import libs
# (libcmt.lib, msvcrt.lib), not the debug ones (libcmtd.lib, msvcrtd.lib,
# which xwin doesn't fetch by default) -- CMake's usual per-config default
# of /MTd for a Debug build would fail to link here regardless of the /MT-
# vs-/MD preference. Set here, in the toolchain file, rather than only in
# CMakeLists.txt, so the internal compiler-check try_compile during
# project() sees it too -- that one doesn't inherit settings CMakeLists.txt
# applies after project() runs, and without this it failed the exact same
# way, trying to link against a debug CRT lib that isn't there.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
