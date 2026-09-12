#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

PREFIX="$SANDBOX/opt"
DEST="$PREFIX/bin/shimback"
MAN_DEST="$PREFIX/share/man/man1/shimback.1"
ZSHRC="$HOME/.zshrc"
CFG="$(config_file)"

# Same trick as test_install.sh: a sandboxed copy of the binary with a real
# man page bundled beside it, so `install` never needs the network.
BUNDLE_DIR="$SANDBOX/bundle"
mkdir -p "$BUNDLE_DIR"
cp "$SHIMBACK" "$BUNDLE_DIR/shimback"
chmod +x "$BUNDLE_DIR/shimback"
cp "$TEST_DIR/../man/shimback.1" "$BUNDLE_DIR/shimback.1"
BUNDLED_SHIMBACK="$BUNDLE_DIR/shimback"

# --- set up: a shim, an installed binary, and an installed man page ---
"$SHIMBACK" add mytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
"$BUNDLED_SHIMBACK" install --prefix "$PREFIX" >/dev/null

MYTOOL_LINK="$(shim_path mytool)"
if [ ! -L "$MYTOOL_LINK" ]; then
    fail "setup: expected a shim symlink at $MYTOOL_LINK"
fi
if [ ! -x "$DEST" ]; then
    fail "setup: expected an installed binary at $DEST"
fi
if [ ! -f "$MAN_DEST" ]; then
    fail "setup: expected an installed man page at $MAN_DEST"
fi
if [ ! -f "$CFG" ]; then
    fail "setup: expected a config file at $CFG"
fi

# --- default uninstall: removes shims, binary, and man page ---
out="$("$SHIMBACK" uninstall --prefix "$PREFIX")"
code=$?
assert_eq "uninstall: exits 0" "0" "$code"

if [ -e "$MYTOOL_LINK" ]; then
    fail "uninstall: shim symlink should be gone"
fi
if [ -e "$DEST" ]; then
    fail "uninstall: installed binary should be gone"
fi
if [ -e "$MAN_DEST" ]; then
    fail "uninstall: installed man page should be gone"
fi
assert_contains "uninstall: reports removed symlink(s)" "$out" "removed 1 shim symlink"

# --- default uninstall leaves config and PATH blocks alone ---
if [ ! -f "$CFG" ]; then
    fail "uninstall (default): config file should NOT be removed"
fi
assert_contains "uninstall (default): PATH block left alone" "$(cat "$ZSHRC")" \
    "# >>> shimback >>>"

# --- --full also clears config and PATH blocks ---
"$SHIMBACK" add mytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
"$BUNDLED_SHIMBACK" install --prefix "$PREFIX" >/dev/null

out2="$("$SHIMBACK" uninstall --prefix "$PREFIX" --full)"
code2=$?
assert_eq "uninstall --full: exits 0" "0" "$code2"

if [ -f "$CFG" ]; then
    fail "uninstall --full: config file should be removed"
fi
assert_not_contains "uninstall --full: PATH block removed" "$(cat "$ZSHRC")" \
    "# >>> shimback >>>"

# --- uninstalling again (nothing left) is a harmless no-op ---
"$SHIMBACK" uninstall --prefix "$PREFIX" --full >/dev/null
code3=$?
assert_eq "uninstall: idempotent re-run exits 0" "0" "$code3"

finish
