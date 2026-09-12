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
    echo "python3 is required to drive the interactive wizard test" 1>&2
    exit 1
fi

TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$TEST_DIR/test_add_wizard.py" "$SHIMBACK"
