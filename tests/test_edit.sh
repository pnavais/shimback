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

# --- $EDITOR is word-split (for multi-word editors like "code --wait"),
# never handed to a shell to interpret -- shell metacharacters and command
# substitution in it must be refused, not executed (see review.md) ---
PWNED_MARKER="$SANDBOX/pwned"
rm -f "$PWNED_MARKER"
EDITOR="true; touch $PWNED_MARKER" "$SHIMBACK" edit >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "edit: a \$EDITOR containing a shell command separator should be refused"
fi
if [ -e "$PWNED_MARKER" ]; then
    fail "edit: \$EDITOR's ';'-separated command must never actually run"
fi
assert_contains "edit: explains why the ';' \$EDITOR was refused" "$(cat "$SANDBOX/err")" \
    "doesn't look like a plain command"

EDITOR="\$(touch $PWNED_MARKER)" "$SHIMBACK" edit >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "edit: a \$EDITOR containing command substitution should be refused"
fi
if [ -e "$PWNED_MARKER" ]; then
    fail "edit: \$EDITOR's command substitution must never actually run"
fi
assert_contains "edit: explains why command substitution was refused" "$(cat "$SANDBOX/err")" \
    "command substitution isn't allowed"

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

# --- edit <name>: opens whichever file defines that shim -- config.toml for
# a regular shim, its own split file for a --split-config one ---
"$SHIMBACK" add plainshim -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
"$SHIMBACK" add splitshim -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null
SPLIT_FILE="$(dirname "$CFG")/splitshim-config.toml"

EDITOR="$FAKEBIN/myeditor" "$SHIMBACK" edit plainshim >/dev/null 2>"$SANDBOX/err"
code=$?
assert_eq "edit <name>: exits 0 for a config.toml shim" "0" "$code"
assert_contains "edit <name>: a config.toml shim opens config.toml" "$(cat "$CFG")" \
    "EDITED_BY:myeditor ARGS:$CFG"
assert_not_contains "edit <name>: ... and leaves the split file alone" "$(cat "$SPLIT_FILE")" \
    "EDITED_BY"

EDITOR="$FAKEBIN/myeditor" "$SHIMBACK" edit splitshim >/dev/null 2>"$SANDBOX/err"
code=$?
assert_eq "edit <name>: exits 0 for a split-config shim" "0" "$code"
assert_contains "edit <name>: a split-config shim opens its split file" "$(cat "$SPLIT_FILE")" \
    "EDITED_BY:myeditor ARGS:$SPLIT_FILE"
assert_eq "edit <name>: ... and config.toml only got the one earlier edit" "1" \
    "$(count_occurrences "EDITED_BY" "$CFG")"

# a copy moved next to the shim's own symlink wins, same as at dispatch time
MOVED_SPLIT="$(dirname "$(shim_path splitshim)")/splitshim-config.toml"
mv "$SPLIT_FILE" "$MOVED_SPLIT"
EDITOR="$FAKEBIN/myeditor" "$SHIMBACK" edit splitshim >/dev/null 2>"$SANDBOX/err"
assert_contains "edit <name>: follows a split file moved to the shim's own directory" \
    "$(cat "$MOVED_SPLIT")" "ARGS:$MOVED_SPLIT"

# --- edit <name> on an unknown shim: clear error, editor never runs ---
BEFORE="$(cat "$CFG")"
EDITOR="$FAKEBIN/myeditor" "$SHIMBACK" edit plainshimm >/dev/null 2>"$SANDBOX/err"
code=$?
assert_eq "edit <name>: an unconfigured name exits 1" "1" "$code"
assert_contains "edit <name>: says no shim is configured" "$(cat "$SANDBOX/err")" \
    "no shim configured for 'plainshimm'"
assert_eq "edit <name>: an unconfigured name doesn't run the editor" "$BEFORE" "$(cat "$CFG")"

# --- edit <name> rejects a name that isn't a valid shim name (no path
# traversal into split_config_filename) ---
EDITOR="$FAKEBIN/myeditor" "$SHIMBACK" edit ../../etc/passwd >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "edit <name>: a name with '/' should be rejected"
fi
assert_contains "edit <name>: explains the invalid name" "$(cat "$SANDBOX/err")" \
    "invalid shim name"

# --- edit <name> can still open a malformed split file (to fix it), and
# validates it as a split file afterwards, not as a config.toml ---
printf '[oops]\n' >"$MOVED_SPLIT"
EDITOR="$FAKEBIN/myeditor" "$SHIMBACK" edit splitshim >/dev/null 2>"$SANDBOX/err"
code=$?
assert_eq "edit <name>: opens a malformed split file" "0" "$code"
assert_contains "edit <name>: the malformed split file was the one opened" \
    "$(cat "$MOVED_SPLIT")" "EDITED_BY:myeditor"
assert_contains "edit <name>: warns the split file still doesn't parse" "$(cat "$SANDBOX/err")" \
    "$MOVED_SPLIT now fails to parse"

printf '#!/bin/sh\nprintf "fallback = \\"%s\\"\\n" "%s" > "$1"\n' "$FAKE_FALLBACK" "$FAKE_FALLBACK" \
    >"$FAKEBIN/goodsplit"
chmod +x "$FAKEBIN/goodsplit"
EDITOR="$FAKEBIN/goodsplit" "$SHIMBACK" edit splitshim >/dev/null 2>"$SANDBOX/err"
assert_not_contains "edit <name>: no warning once the split file parses again" \
    "$(cat "$SANDBOX/err")" "now fails to parse"

# --- more than one argument is a hard error ---
"$SHIMBACK" edit plainshim extra >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "edit: an unexpected extra argument should fail"
fi
assert_contains "edit: unexpected argument error" "$(cat "$SANDBOX/err")" "unexpected argument"

finish
