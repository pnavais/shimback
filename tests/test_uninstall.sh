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

# --- uninstall --full run from the INSTALLED binary itself (the real,
# common way someone actually runs it: `~/.local/bin/shimback uninstall
# --full`) must not die partway through. Every scenario above ran
# uninstall from a copy of the binary that survives the whole run ($SHIMBACK,
# never installed at $DEST) -- this is the one place self_exe_path()'s
# own canonicalize() call is resolving the same file uninstall is about to
# unlink, which is exactly the ordering this regression test guards:
# anything needing self_exe_path() (the split-config sweep, in
# particular) must run before the installed-binary removal, not after. ---
"$SHIMBACK" add selftool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null
"$BUNDLED_SHIMBACK" install --prefix "$PREFIX" >/dev/null
if [ ! -x "$DEST" ]; then
    fail "setup: expected an installed binary at $DEST for the self-uninstall test"
fi

out5="$("$DEST" uninstall --prefix "$PREFIX" --full)"
code5=$?
assert_eq "uninstall --full run from the installed binary itself: exits 0" "0" "$code5"
assert_not_contains "uninstall --full (self): no canonicalize failure" "$out5" \
    "failed to canonicalize executable path"
if [ -e "$DEST" ]; then
    fail "uninstall --full (self): installed binary should be gone"
fi
if [ -f "$CFG" ]; then
    fail "uninstall --full (self): config file should be removed"
fi

# --- uninstall leaves alone a live symlink in the shim directory that
# doesn't resolve to a real shimback binary (hand-placed by the user or
# another tool, despite the shim directory being shimback's by convention)
# -- it must not be silently deleted just for being a symlink in that
# directory ---
"$SHIMBACK" add mytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
FOREIGN_TARGET="$SANDBOX/not-shimback.sh"
printf '#!/bin/sh\necho not shimback\n' >"$FOREIGN_TARGET"
chmod +x "$FOREIGN_TARGET"
FOREIGN_LINK="$(shim_path foreigntool)"
ln -s "$FOREIGN_TARGET" "$FOREIGN_LINK"

out6="$("$SHIMBACK" uninstall --prefix "$PREFIX" 2>&1)"
assert_contains "uninstall: warns about the foreign symlink" "$out6" \
    "leaving $FOREIGN_LINK alone"
if [ ! -L "$FOREIGN_LINK" ]; then
    fail "uninstall: foreign symlink should NOT have been removed"
fi
if [ -e "$(shim_path mytool)" ]; then
    fail "uninstall: shimback's own symlink should still have been removed"
fi
rm -f "$FOREIGN_LINK" "$FOREIGN_TARGET"

# --- ownership verification must never *execute* the candidate to decide
# -- a malicious symlink target that would print a convincing fake
# "shimback " prefix (and do something else first) must never actually
# run, let alone be trusted because of what it printed (see review.md) ---
"$SHIMBACK" add mytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
PWNED_MARKER="$SANDBOX/pwned_marker"
rm -f "$PWNED_MARKER"
MALICIOUS_TARGET="$SANDBOX/malicious.sh"
printf '#!/bin/sh\ntouch "%s"\necho "shimback fake"\n' "$PWNED_MARKER" >"$MALICIOUS_TARGET"
chmod +x "$MALICIOUS_TARGET"
MALICIOUS_LINK="$(shim_path malicioustool)"
ln -s "$MALICIOUS_TARGET" "$MALICIOUS_LINK"

"$SHIMBACK" uninstall --prefix "$PREFIX" >/dev/null 2>&1
if [ -e "$PWNED_MARKER" ]; then
    fail "uninstall: ownership check must never execute the candidate file"
fi
if [ ! -L "$MALICIOUS_LINK" ]; then
    fail "uninstall: a symlink that would fake a 'shimback' identity if run should still survive"
fi
rm -f "$MALICIOUS_LINK" "$MALICIOUS_TARGET"

# --- uninstall --prefix pointing at a location with an unrelated file
# named "shimback" (e.g. a typo'd --prefix, or one shared with another
# project) leaves it alone rather than deleting it outright ---
FOREIGN_PREFIX="$SANDBOX/foreign-prefix"
mkdir -p "$FOREIGN_PREFIX/bin" "$FOREIGN_PREFIX/share/man/man1"
printf '#!/bin/sh\necho not shimback either\n' >"$FOREIGN_PREFIX/bin/shimback"
chmod +x "$FOREIGN_PREFIX/bin/shimback"
printf '.TH SOMETHING-ELSE 1\n' >"$FOREIGN_PREFIX/share/man/man1/shimback.1"

out7="$("$SHIMBACK" uninstall --prefix "$FOREIGN_PREFIX" 2>&1)"
assert_contains "uninstall: refuses to delete an unrelated bin/shimback" "$out7" \
    "doesn't look like a shimback installed binary"
assert_contains "uninstall: refuses to delete an unrelated man page" "$out7" \
    "doesn't look like a shimback man page"
if [ ! -f "$FOREIGN_PREFIX/bin/shimback" ]; then
    fail "uninstall --prefix: unrelated binary should NOT have been removed"
fi
if [ ! -f "$FOREIGN_PREFIX/share/man/man1/shimback.1" ]; then
    fail "uninstall --prefix: unrelated man page should NOT have been removed"
fi

# --- same as the malicious-symlink case above, but for --prefix's own
# bin/shimback: a fake binary that would print "shimback ..." if actually
# run must never get the chance to -- and must still be correctly refused
# ---
PWNED_MARKER2="$SANDBOX/pwned_marker2"
rm -f "$PWNED_MARKER2"
MALICIOUS_PREFIX="$SANDBOX/malicious-prefix"
mkdir -p "$MALICIOUS_PREFIX/bin"
printf '#!/bin/sh\ntouch "%s"\necho "shimback 99.99.99"\n' "$PWNED_MARKER2" \
    >"$MALICIOUS_PREFIX/bin/shimback"
chmod +x "$MALICIOUS_PREFIX/bin/shimback"

"$SHIMBACK" uninstall --prefix "$MALICIOUS_PREFIX" >/dev/null 2>&1
if [ -e "$PWNED_MARKER2" ]; then
    fail "uninstall --prefix: must never execute bin/shimback to check its identity"
fi
if [ ! -f "$MALICIOUS_PREFIX/bin/shimback" ]; then
    fail "uninstall --prefix: a binary that would fake its identity if run should still survive"
fi

# --- a genuinely-owned shim symlink with a name outside `add`'s own
# allowlist (hand-created with `ln -s`, or predating that restriction)
# must not abort uninstall --full partway through: the symlink itself is
# still safely removed (its *target* really is the shimback binary), but
# the invalid name must be skipped -- with a warning, not a die() -- when
# --full goes on to sweep split configs, so the rest of the cleanup (the
# config file, PATH blocks) still completes (see review.md) ---
"$SHIMBACK" add mytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
BAD_NAME_LINK="$(shim_path 'bad#name')"
ln -s "$SHIMBACK" "$BAD_NAME_LINK"

out8="$("$SHIMBACK" uninstall --prefix "$PREFIX" --full 2>&1)"
code8=$?
assert_eq "uninstall --full: still exits 0 despite an invalidly-named owned symlink" "0" "$code8"
assert_contains "uninstall --full: warns about the invalid name instead of dying" "$out8" \
    "skipping split-config cleanup for invalid shim name 'bad#name'"
if [ -e "$BAD_NAME_LINK" ]; then
    fail "uninstall --full: the invalidly-named owned symlink should still have been removed"
fi
if [ -f "$CFG" ]; then
    fail "uninstall --full: config file should still be removed despite the invalid name"
fi

finish
