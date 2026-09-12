#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

PREFIX="$SANDBOX/opt"
DEST="$PREFIX/bin/shimback"
MAN_DEST="$PREFIX/share/man/man1/shimback.1"
ZSHRC="$HOME/.zshrc"

# install's man-page step looks for a "shimback.1" bundled next to the
# *running* binary (how a release tarball ships it) before ever considering
# a network fetch. Run install through a sandboxed copy of $SHIMBACK with a
# real man page sitting beside it, so these tests exercise that local path
# deterministically instead of racing a real `curl` call against GitHub.
BUNDLE_DIR="$SANDBOX/bundle"
mkdir -p "$BUNDLE_DIR"
cp "$SHIMBACK" "$BUNDLE_DIR/shimback"
chmod +x "$BUNDLE_DIR/shimback"
cp "$TEST_DIR/../man/shimback.1" "$BUNDLE_DIR/shimback.1"
BUNDLED_SHIMBACK="$BUNDLE_DIR/shimback"

SHIM_DIR="$XDG_DATA_HOME/shimback/bin"
BIN_DIR="$PREFIX/bin"

# --- install copies the binary, the bundled man page, and merges both its
# own bin dir and the shim dir into a single PATH block ---
out="$("$BUNDLED_SHIMBACK" install --prefix "$PREFIX")"
code=$?
assert_eq "install: exits 0" "0" "$code"

if [ ! -x "$DEST" ]; then
    fail "install: expected an executable at $DEST"
fi
assert_contains "install: reports installed" "$out" "installed to $DEST"
assert_contains "install: PATH block injected" "$(cat "$ZSHRC")" "# >>> shimback >>>"
assert_contains "install: block includes its own bin dir" "$(cat "$ZSHRC")" "$BIN_DIR"
assert_contains "install: block includes the shim dir too, before any add" "$(cat "$ZSHRC")" \
    "$SHIM_DIR"

if [ ! -f "$MAN_DEST" ]; then
    fail "install: expected a man page at $MAN_DEST"
fi
assert_contains "install: reports man page installed" "$out" "man page installed to $MAN_DEST"
assert_not_contains "install: man page came from the bundle, not a download" "$out" "downloaded"

marker_count="$(count_occurrences '# >>> shimback >>>' "$ZSHRC")"
assert_eq "install: exactly one marker block (no separate shimback-bin block)" "1" "$marker_count"
assert_not_contains "install: no legacy shimback-bin block" "$(cat "$ZSHRC")" "shimback-bin"

# --- re-running install is idempotent: refreshes the copy, no duplicate marker ---
out2="$("$BUNDLED_SHIMBACK" install --prefix "$PREFIX")"
code2=$?
assert_eq "install: re-run exits 0" "0" "$code2"
assert_contains "install: re-run reports installed" "$out2" "installed to $DEST"
if [ ! -x "$DEST" ]; then
    fail "install: expected an executable at $DEST after re-install"
fi

marker_count2="$(count_occurrences '# >>> shimback >>>' "$ZSHRC")"
assert_eq "install: still exactly one marker block after re-install" "1" "$marker_count2"

# --- add merges the shim dir into the same block install already wrote,
# rather than creating a second one ---
"$SHIMBACK" add mytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
assert_contains "add: bin dir still present after add merges in" "$(cat "$ZSHRC")" "$BIN_DIR"
assert_contains "add: shim dir still present" "$(cat "$ZSHRC")" "$SHIM_DIR"
marker_count3="$(count_occurrences '# >>> shimback >>>' "$ZSHRC")"
assert_eq "add: still exactly one marker block after merging in" "1" "$marker_count3"

# --- default install only touches the current shell (zsh, per test_common.sh) ---
BASHRC="$HOME/.bashrc"
if [ -e "$BASHRC" ]; then
    fail "install (default): should not have touched $BASHRC"
fi

# --- --shell zsh,bash touches both, even though $SHELL is zsh ---
"$BUNDLED_SHIMBACK" install --prefix "$PREFIX" --shell zsh,bash >/dev/null
if [ ! -e "$BASHRC" ]; then
    fail "install --shell zsh,bash: expected $BASHRC to be created"
fi
assert_contains "install --shell: bashrc got the bin dir" "$(cat "$BASHRC")" "$BIN_DIR"
assert_contains "install --shell: bashrc got the shim dir" "$(cat "$BASHRC")" "$SHIM_DIR"

# --- --shell and --all are mutually exclusive ---
"$BUNDLED_SHIMBACK" install --prefix "$PREFIX" --shell zsh --all >/dev/null 2>"$SANDBOX/err"
code3=$?
if [ "$code3" -eq 0 ]; then
    fail "install --shell --all: should have failed (mutually exclusive)"
fi
assert_contains "install --shell --all: clear error" "$(cat "$SANDBOX/err")" \
    "mutually exclusive"

# --- an unknown --shell name fails before touching anything ---
FRESH_PREFIX="$SANDBOX/opt2"
"$BUNDLED_SHIMBACK" install --prefix "$FRESH_PREFIX" --shell tcsh >/dev/null 2>"$SANDBOX/err"
code4=$?
if [ "$code4" -eq 0 ]; then
    fail "install --shell tcsh: should have failed (unknown shell)"
fi
assert_contains "install --shell tcsh: clear error" "$(cat "$SANDBOX/err")" "unknown shell"
if [ -e "$FRESH_PREFIX/bin/shimback" ]; then
    fail "install --shell tcsh: should not have installed anything before failing"
fi

finish
