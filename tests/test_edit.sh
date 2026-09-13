#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

FAKE_EDITOR_SRC="$FIXTURES_DIR/fake_editor.sh"
chmod +x "$FAKE_EDITOR_SRC"

FAKEBIN="$SANDBOX/fakebin"
mkdir -p "$FAKEBIN"

CFG="$(config_file)"

# --- $EDITOR (a single bare command) is used when set ---
cp "$FAKE_EDITOR_SRC" "$FAKEBIN/myeditor"
chmod +x "$FAKEBIN/myeditor"
EDITOR="$FAKEBIN/myeditor" "$SHIMBACK" edit >/dev/null 2>"$SANDBOX/err"
code=$?
assert_eq "edit: exits 0 when \$EDITOR succeeds" "0" "$code"
assert_contains "edit: \$EDITOR was invoked on the config file" "$(cat "$CFG")" \
    "EDITED_BY:myeditor"
assert_contains "edit: config file path was passed as the argument" "$(cat "$CFG")" "$CFG"
rm -f "$CFG"

# --- a multi-word $EDITOR (command + flags) is word-split, not treated as
# one literal command name ---
EDITOR="$FAKEBIN/myeditor --some-flag" "$SHIMBACK" edit >/dev/null 2>"$SANDBOX/err"
code=$?
assert_eq "edit: exits 0 with a multi-word \$EDITOR" "0" "$code"
assert_contains "edit: multi-word \$EDITOR's flag reached the editor" "$(cat "$CFG")" \
    "--some-flag"
rm -f "$CFG"

# --- unset/empty $EDITOR falls back to nvim/vim/vi/nano/pico, in that
# order -- with fake "vim" and fake "nano" both on PATH, vim (earlier in
# the order) wins ---
cp "$FAKE_EDITOR_SRC" "$FAKEBIN/vim"
chmod +x "$FAKEBIN/vim"
cp "$FAKE_EDITOR_SRC" "$FAKEBIN/nano"
chmod +x "$FAKEBIN/nano"
EDITOR= PATH="$FAKEBIN" "$SHIMBACK" edit >/dev/null 2>"$SANDBOX/err"
code=$?
assert_eq "edit: exits 0 via the fallback chain" "0" "$code"
assert_contains "edit: fallback chain picked vim over nano" "$(cat "$CFG")" "EDITED_BY:vim"
rm -f "$CFG" "$FAKEBIN/vim"

# --- with only a fake "nano" (no nvim/vim/vi) on PATH, nano is picked ---
EDITOR= PATH="$FAKEBIN" "$SHIMBACK" edit >/dev/null 2>"$SANDBOX/err"
code=$?
assert_eq "edit: exits 0 via the fallback chain (nano only)" "0" "$code"
assert_contains "edit: fallback chain picked nano" "$(cat "$CFG")" "EDITED_BY:nano"
rm -f "$CFG" "$FAKEBIN/nano"

# --- no editor at all, anywhere: fails clearly, nothing written ---
EMPTY_BIN="$SANDBOX/emptybin"
mkdir -p "$EMPTY_BIN"
EDITOR= PATH="$EMPTY_BIN" "$SHIMBACK" edit >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "edit: should fail when no editor is available anywhere"
fi
assert_contains "edit: clear error when no editor is found" "$(cat "$SANDBOX/err")" \
    "no editor found"
if [ -e "$CFG" ]; then
    fail "edit: no config file should be created when no editor ever ran"
fi

# --- post-edit validation: editor exits 0 but leaves invalid TOML behind --
# shimback warns about it right away rather than staying silent ---
printf '#!/bin/sh\necho "not valid toml {{{" > "$1"\n' >"$FAKEBIN/badeditor"
chmod +x "$FAKEBIN/badeditor"
EDITOR="$FAKEBIN/badeditor" "$SHIMBACK" edit >/dev/null 2>"$SANDBOX/err"
code=$?
assert_eq "edit: exits 0 even though the result doesn't parse (the editor itself succeeded)" \
    "0" "$code"
assert_contains "edit: warns when the edited config no longer parses" "$(cat "$SANDBOX/err")" \
    "now fails to parse"
rm -f "$CFG"

# --- an unexpected extra argument is a hard error, same as other
# no-argument commands ---
"$SHIMBACK" edit bogus >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "edit: an unexpected extra argument should fail"
fi
assert_contains "edit: unexpected argument error" "$(cat "$SANDBOX/err")" "unexpected argument"

finish
