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

# --- --full also removes a fish conf.d snippet (faking fish onto $PATH so
# shell_is_installed detects it regardless of the test machine's setup) ---
FAKE_SHELLS_DIR="$SANDBOX/fake_shells"
mkdir -p "$FAKE_SHELLS_DIR"
printf '#!/bin/sh\nexit 0\n' >"$FAKE_SHELLS_DIR/fish"
chmod +x "$FAKE_SHELLS_DIR/fish"
export PATH="$FAKE_SHELLS_DIR:$PATH"

FISH_SNIPPET="$HOME/.config/fish/conf.d/shimback.fish"
"$BUNDLED_SHIMBACK" install --prefix "$PREFIX" --shell fish >/dev/null
if [ ! -f "$FISH_SNIPPET" ]; then
    fail "setup: expected a fish conf.d snippet at $FISH_SNIPPET"
fi

"$SHIMBACK" uninstall --prefix "$PREFIX" --full >/dev/null
if [ -e "$FISH_SNIPPET" ]; then
    fail "uninstall --full: fish conf.d snippet should be removed"
fi

# --- --full also sweeps split <name>-config.toml files (see
# test_split_config.sh for the feature itself), for both a shim that also
# has a config.toml entry and one that only ever existed via the split
# file (its symlink is still how uninstall discovers the name) ---
"$SHIMBACK" add splituninst -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null
SPLIT_CFG="$XDG_CONFIG_HOME/shimback/splituninst-config.toml"
if [ ! -f "$SPLIT_CFG" ]; then
    fail "setup: expected a split config file at $SPLIT_CFG"
fi
assert_not_contains "setup: split shim has no config.toml entry" "$(cat "$CFG" 2>/dev/null)" \
    "[shims.splituninst]"

"$SHIMBACK" uninstall --prefix "$PREFIX" --full >/dev/null
if [ -e "$SPLIT_CFG" ]; then
    fail "uninstall --full: split config file should be removed"
fi

finish
