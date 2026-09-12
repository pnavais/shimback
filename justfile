arch := arch()
os := os()
build_dir := "build-" + os + "-" + arch
build_type := env_var_or_default("BUILD_TYPE", "Release")

# List available recipes
default:
    @just --list

# Configure + compile shimback for the current OS/arch (autodetected: {{os}}-{{arch}})
build:
    @echo "shimback: building for {{os}}-{{arch}} in {{build_dir}} ({{build_type}})"
    cmake -S . -B {{build_dir}} -DCMAKE_BUILD_TYPE={{build_type}}
    cmake --build {{build_dir}}

# Build, then run the test suite
test: build
    ctest --test-dir {{build_dir}} --output-on-failure

# Install the built binary (defaults to /usr/local, override with `just install ~/.local`)
install prefix="/usr/local": build
    cmake --install {{build_dir}} --prefix {{prefix}}

# Remove build output for the current OS/arch
clean:
    rm -rf {{build_dir}}

# Remove build output for every OS/arch
clean-all:
    rm -rf build-*
