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

# --- installing again to the same prefix is refused with a warning (upgrading
# from a release is `update`'s job) and touches nothing: not the binary, not
# the PATH block ---
echo "sentinel" >>"$DEST" # any change to the installed copy would overwrite this
out2="$("$BUNDLED_SHIMBACK" install --prefix "$PREFIX" 2>&1)"
code2=$?
assert_eq "install: same-prefix re-run exits 0 (nothing to do)" "0" "$code2"
assert_contains "install: same-prefix re-run warns it's already installed" "$out2" \
    "already installed at $DEST"
assert_contains "install: the warning points at update" "$out2" "shimback update"
assert_contains "install: the warning mentions --force" "$out2" "--force"
assert_not_contains "install: same-prefix re-run doesn't reinstall" "$out2" "installed to $DEST"
assert_contains "install: same-prefix re-run left the installed binary alone" "$(cat "$DEST")" \
    "sentinel"

marker_count2="$(count_occurrences '# >>> shimback >>>' "$ZSHRC")"
assert_eq "install: still exactly one marker block after the refused re-install" "1" \
    "$marker_count2"

# --- --force overwrites the installation at the same prefix (a developer's
# "I just rebuilt it") without creating a second one ---
out2b="$("$BUNDLED_SHIMBACK" install --prefix "$PREFIX" --force 2>&1)"
code2b=$?
assert_eq "install --force: same-prefix overwrite exits 0" "0" "$code2b"
assert_contains "install --force: reports installed" "$out2b" "installed to $DEST"
assert_not_contains "install --force: overwrote the installed copy" "$(cat "$DEST")" "sentinel"
assert_eq "install --force: still exactly one marker block" "1" \
    "$(count_occurrences '# >>> shimback >>>' "$ZSHRC")"

# --- only one installation: a different prefix is refused, with or without
# --force, and nothing is created there ---
OTHER_PREFIX="$SANDBOX/other-prefix"
for extra in "" "--force"; do
    out2c="$("$BUNDLED_SHIMBACK" install --prefix "$OTHER_PREFIX" $extra 2>&1)"
    code2c=$?
    assert_eq "install $extra: a different prefix is refused" "1" "$code2c"
    assert_contains "install $extra: names the existing installation" "$out2c" "already installed at $DEST"
    assert_contains "install $extra: explains only one is allowed" "$out2c" \
        "only one installation is allowed"
    if [ -e "$OTHER_PREFIX" ]; then
        fail "install $extra: a refused install must not create the other prefix"
    fi
done
assert_eq "install: the refused installs left one marker block" "1" \
    "$(count_occurrences '# >>> shimback >>>' "$ZSHRC")"
assert_not_contains "install: the block never gained the other prefix" "$(cat "$ZSHRC")" \
    "$OTHER_PREFIX"

# --- a stale block entry (binary deleted by hand) isn't an installation ---
STALE_HOME_ZSHRC_BACKUP="$SANDBOX/zshrc.backup"
cp "$ZSHRC" "$STALE_HOME_ZSHRC_BACKUP"
mv "$DEST" "$DEST.gone"
out2d="$("$BUNDLED_SHIMBACK" install --prefix "$OTHER_PREFIX" 2>&1)"
code2d=$?
assert_eq "install: a stale block entry doesn't block installing elsewhere" "0" "$code2d"
rm -rf "$OTHER_PREFIX"
mv "$DEST.gone" "$DEST"
cp "$STALE_HOME_ZSHRC_BACKUP" "$ZSHRC"

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
"$BUNDLED_SHIMBACK" install --prefix "$PREFIX" --force --shell zsh,bash >/dev/null
if [ ! -e "$BASHRC" ]; then
    fail "install --shell zsh,bash: expected $BASHRC to be created"
fi
assert_contains "install --shell: bashrc got the bin dir" "$(cat "$BASHRC")" "$BIN_DIR"
assert_contains "install --shell: bashrc got the shim dir" "$(cat "$BASHRC")" "$SHIM_DIR"

# --- --shell fish writes its own conf.d snippet, not a marker block ---
FISH_SNIPPET="$HOME/.config/fish/conf.d/shimback.fish"
"$BUNDLED_SHIMBACK" install --prefix "$PREFIX" --force --shell fish >/dev/null
if [ ! -f "$FISH_SNIPPET" ]; then
    fail "install --shell fish: expected a conf.d snippet at $FISH_SNIPPET"
fi
assert_contains "install --shell fish: snippet got the bin dir" "$(cat "$FISH_SNIPPET")" "$BIN_DIR"
assert_contains "install --shell fish: snippet got the shim dir" "$(cat "$FISH_SNIPPET")" \
    "$SHIM_DIR"
assert_not_contains "install --shell fish: no marker syntax in the fish snippet" \
    "$(cat "$FISH_SNIPPET")" "# >>>"

# --- re-running install --shell fish doesn't duplicate directories ---
"$BUNDLED_SHIMBACK" install --prefix "$PREFIX" --force --shell fish >/dev/null
bin_dir_count="$(count_occurrences "$BIN_DIR" "$FISH_SNIPPET")"
assert_eq "install --shell fish: bin dir stays singular on re-run" "1" "$bin_dir_count"

# --- --shell and --all are mutually exclusive ---
"$BUNDLED_SHIMBACK" install --prefix "$PREFIX" --force --shell zsh --all >/dev/null 2>"$SANDBOX/err"
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
