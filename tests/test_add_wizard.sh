#!/bin/sh
# Drives the interactive `add` wizard via a real pseudo-terminal, which the
# other, sandboxed-but-non-tty shell tests can't do. See test_add_wizard.py.
set -u

SHIMBACK="$1"
if [ -z "$SHIMBACK" ] || [ ! -x "$SHIMBACK" ]; then
    echo "usage: $0 <path-to-shimback-binary>" 1>&2
    exit 2
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not found -- skipping the interactive wizard test (see README.md: only a" 1>&2
    echo "C11 compiler and CMake are required to build/run shimback itself; python3 is only" 1>&2
    echo "needed to exercise this one PTY-driven test)" 1>&2
    # 77 is the conventional "skipped" exit code (Automake's test harness,
    # and CMake's own SKIP_RETURN_CODE docs, both use it) -- CMakeLists.txt
    # sets this test's SKIP_RETURN_CODE to match, so ctest reports it as
    # skipped rather than failed. A plain `exit 1` here would otherwise
    # make a from-source build on a machine without python3 impossible to
    # pass, despite the README never listing it as a prerequisite.
    exit 77
fi

TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$TEST_DIR/test_add_wizard.py" "$SHIMBACK"
