#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

MYTOOL_LINK="$(shim_path mytool)"
ZSHRC="$HOME/.zshrc"

# --- add creates a symlink, a config entry, and a PATH block ---
"$SHIMBACK" add mytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null

if [ ! -L "$MYTOOL_LINK" ]; then
    fail "add: expected a symlink at $MYTOOL_LINK"
fi
assert_contains "add: config has the shim section" "$(cat "$(config_file)")" "[shims.mytool]"
assert_contains "add: PATH block injected into .zshrc" "$(cat "$ZSHRC")" "# >>> shimback >>>"
assert_eq "ls is an alias for list" "$("$SHIMBACK" list)" "$("$SHIMBACK" ls)"

marker_count="$(count_occurrences '# >>> shimback >>>' "$ZSHRC")"
assert_eq "add: exactly one marker block after first add" "1" "$marker_count"

# --- re-running add is idempotent: no duplicate marker block ---
"$SHIMBACK" add mytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
marker_count="$(count_occurrences '# >>> shimback >>>' "$ZSHRC")"
assert_eq "add: still exactly one marker block after re-add" "1" "$marker_count"

# --- changing the shim dir updates the block in place, still singular ---
OLD_DATA_HOME="$XDG_DATA_HOME"
export XDG_DATA_HOME="$SANDBOX/home/.local/share-alt"
"$SHIMBACK" add mytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
marker_count="$(count_occurrences '# >>> shimback >>>' "$ZSHRC")"
assert_eq "add: marker block still singular after shim dir change" "1" "$marker_count"
assert_contains "add: PATH block updated to new shim dir" "$(cat "$ZSHRC")" "$XDG_DATA_HOME/shimback/bin"
assert_not_contains "add: old shim dir no longer referenced" "$(cat "$ZSHRC")" "$OLD_DATA_HOME/shimback/bin"
export XDG_DATA_HOME="$OLD_DATA_HOME"

# --- source == fallback: rejected, nothing written ---
before_listing="$("$SHIMBACK" list)"
"$SHIMBACK" add badtool -s "$FAKE_PRIMARY" -f "$FAKE_PRIMARY" >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add with source==fallback should fail"
fi
after_listing="$("$SHIMBACK" list)"
assert_eq "add rejection: config unchanged" "$before_listing" "$after_listing"
if [ -e "$(shim_path badtool)" ]; then
    fail "add rejection: no symlink should have been created for badtool"
fi

# --- remove drops the symlink and config entry, leaves PATH block alone ---
zshrc_before_remove="$(cat "$ZSHRC")"
"$SHIMBACK" remove mytool >/dev/null

if [ -e "$MYTOOL_LINK" ]; then
    fail "remove: symlink should be gone"
fi
assert_not_contains "remove: config entry gone" "$("$SHIMBACK" list)" "mytool"
assert_eq "remove: .zshrc PATH block untouched" "$zshrc_before_remove" "$(cat "$ZSHRC")"

# --- removing a nonexistent shim fails ---
"$SHIMBACK" remove mytool >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "remove: removing an already-removed shim should fail"
fi

finish
