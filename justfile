set windows-shell := ["cmd.exe", "/c"]

arch := arch()
os := os()
build_dir := "build-" + os + "-" + arch
build_type := env_var_or_default("BUILD_TYPE", "Release")

# Windows has no native MSVC/nmake generator available outside a VS
# Developer shell (plain `cmake -S . -B dir` picks NMake Makefiles by
# default there and then fails trying to run nmake.exe, which isn't on
# PATH in an ordinary shell) -- this project's own Windows toolchain is
# clang-cl/Ninja instead (see cmake/windows-clang-cl.cmake), same as CI.
# Needs SHIMBACK_MSVC_SYSROOT (an xwin-splatted sysroot) set, as an env
# var or via `just build windows_cmake_args=...`; the toolchain file's own
# FATAL_ERROR points this out clearly if it's missing.
windows_cmake_args := if os() == "windows" { "-G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/windows-clang-cl.cmake" } else { "" }

# List available recipes
default:
    @just --list

# Configure + compile shimback for the current OS/arch (autodetected: {{os}}-{{arch}})
build:
    @echo "shimback: building for {{os}}-{{arch}} in {{build_dir}} ({{build_type}})"
    cmake -S . -B {{build_dir}} -DCMAKE_BUILD_TYPE={{build_type}} {{windows_cmake_args}}
    cmake --build {{build_dir}}

# Build, then run the test suite
test: build
    ctest --test-dir {{build_dir}} --output-on-failure

# Install the built binary (defaults to /usr/local, override with `just install ~/.local`)
install prefix="/usr/local": build
    cmake --install {{build_dir}} --prefix {{prefix}}

# Remove build output for the current OS/arch
clean:
    cmake -E rm -rf {{build_dir}}

# Remove build output for every OS/arch
clean-all:
    rm -rf build-*
