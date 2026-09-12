#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

PREFIX="$SANDBOX/opt"
DEST="$PREFIX/bin/shimback"
ZSHRC="$HOME/.zshrc"

# --- install copies the binary and injects a PATH block for it ---
out="$("$SHIMBACK" install --prefix "$PREFIX")"
code=$?
assert_eq "install: exits 0" "0" "$code"

if [ ! -x "$DEST" ]; then
    fail "install: expected an executable at $DEST"
fi
assert_contains "install: reports installed" "$out" "installed to $DEST"
assert_contains "install: PATH block injected" "$(cat "$ZSHRC")" "# >>> shimback-bin >>>"

marker_count="$(count_occurrences '# >>> shimback-bin >>>' "$ZSHRC")"
assert_eq "install: exactly one shimback-bin marker block" "1" "$marker_count"

# --- re-running install is idempotent: refreshes the copy, no duplicate marker ---
out2="$("$SHIMBACK" install --prefix "$PREFIX")"
code2=$?
assert_eq "install: re-run exits 0" "0" "$code2"
assert_contains "install: re-run reports installed" "$out2" "installed to $DEST"
if [ ! -x "$DEST" ]; then
    fail "install: expected an executable at $DEST after re-install"
fi

marker_count2="$(count_occurrences '# >>> shimback-bin >>>' "$ZSHRC")"
assert_eq "install: still exactly one marker block after re-install" "1" "$marker_count2"

# --- the shim-dir block (managed by add/init) stays separate from the bin block ---
"$SHIMBACK" add mytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
assert_contains "install: shim-dir block still present alongside the bin block" \
    "$(cat "$ZSHRC")" "# >>> shimback >>>"
assert_contains "install: bin block still present alongside the shim-dir block" \
    "$(cat "$ZSHRC")" "# >>> shimback-bin >>>"

finish
