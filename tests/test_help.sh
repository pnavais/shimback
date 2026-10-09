#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

# --- global --help / -h: full command list, exits 0 ---
out="$("$SHIMBACK" --help)"
code=$?
assert_eq "--help: exits 0" "0" "$code"
assert_contains "--help: has USAGE section" "$out" "USAGE:"
assert_contains "--help: has COMMANDS section" "$out" "COMMANDS:"
for cmd in add remove init list doctor install uninstall edit info update export completions; do
    assert_contains "--help: mentions '$cmd'" "$out" "$cmd"
done
assert_eq "-h is the same as --help" "$out" "$("$SHIMBACK" -h)"

# --- per-subcommand --help: just that command, clap-rs style ---
out="$("$SHIMBACK" add --help)"
code=$?
assert_eq "add --help: exits 0" "0" "$code"
assert_contains "add --help: shows add's own usage" "$out" "shimback add <name>"
assert_contains "add --help: shows add's description" "$out" "Create or update a shim"
assert_contains "add --help: shows add's example" "$out" "shimback add sed -f /usr/bin/sed"
assert_contains "add --help: points back to the full list" "$out" \
    "Run \`shimback --help\` to see every command."
assert_not_contains "add --help: doesn't include remove's own usage line" "$out" \
    "shimback remove ["
assert_not_contains "add --help: doesn't include doctor's own usage line" "$out" \
    "shimback doctor [fix"

# --- -h is the same as --help for a subcommand too ---
assert_eq "add -h is the same as add --help" "$out" "$("$SHIMBACK" add -h)"

# --- aliases show their primary command's help ---
assert_eq "rm --help shows remove's help" "$("$SHIMBACK" remove --help)" \
    "$("$SHIMBACK" rm --help)"
assert_eq "ls --help shows list's help" "$("$SHIMBACK" list --help)" \
    "$("$SHIMBACK" ls --help)"

# --- --help short-circuits no matter where it appears among the
# command's own arguments, even ones that would otherwise be incomplete
# or invalid -- matching clap-rs ---
out="$("$SHIMBACK" add -f /usr/bin/sed --help)"
code=$?
assert_eq "add -f ... --help: still exits 0" "0" "$code"
assert_contains "add -f ... --help: still shows add's help" "$out" "shimback add <name>"

out="$("$SHIMBACK" doctor fix -y -h)"
code=$?
assert_eq "doctor fix -y -h: still exits 0" "0" "$code"
assert_contains "doctor fix -y -h: shows doctor's help" "$out" "shimback doctor [fix"

# --- an unrecognized command isn't rescued by a trailing --help -- it's
# still an error, with the full command list as a hint (existing
# unknown-command behavior, unaffected by this feature) ---
"$SHIMBACK" bogus --help >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "bogus --help: an unrecognized command should still fail"
fi
assert_contains "bogus --help: still reports the unknown command" \
    "$(cat "$SANDBOX/err")" "unknown command 'bogus'"

# --- color: off when piped, forced on by CLICOLOR_FORCE, NO_COLOR wins ---
ESC="$(printf '\033')"
out="$(unset NO_COLOR CLICOLOR_FORCE; "$SHIMBACK" --help)"
assert_not_contains "--help piped: no color" "$out" "$ESC["
out="$(unset NO_COLOR; CLICOLOR_FORCE=1 "$SHIMBACK" --help)"
assert_contains "--help piped with CLICOLOR_FORCE=1: colored" "$out" "$ESC["
out="$(unset NO_COLOR; CLICOLOR_FORCE=0 "$SHIMBACK" --help)"
assert_not_contains "--help piped with CLICOLOR_FORCE=0: no color" "$out" "$ESC["
out="$(CLICOLOR_FORCE=1 NO_COLOR=1 "$SHIMBACK" --help)"
assert_not_contains "--help with NO_COLOR and CLICOLOR_FORCE: NO_COLOR wins" "$out" "$ESC["
err="$(unset NO_COLOR; CLICOLOR_FORCE=1 "$SHIMBACK" bogus 2>&1 >/dev/null)"
assert_contains "stderr messages honor CLICOLOR_FORCE too" "$err" "${ESC}[34mshimback:"

finish
