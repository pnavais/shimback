#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

# --- route-map: the motivating java17/java8/java25 example. Multiple
# argv-triggered routes to different commands, with source as the no-match
# default -- what route-args cannot express (more than one alternate). ---
"$SHIMBACK" add jtool -s "$FAKE_PRIMARY" --policy route-map \
    --route "--v8=$FAKE_FALLBACK" --route "--v25=$FAKE_ECHO" >/dev/null

JTOOL="$(shim_path jtool)"

out="$(FAKE_EXIT_CODE=5 FAKE_STDOUT="primary-out" FAKE_STDERR="primary-err" "$JTOOL" other 2>"$SANDBOX/err")"
code=$?
assert_eq "route-map no-match: runs source directly, real exit code surfaces" "5" "$code"
assert_eq "route-map no-match: source's real stdout surfaces live" "primary-out" "$out"
assert_eq "route-map no-match: source's real stderr surfaces live" "primary-err" "$(cat "$SANDBOX/err")"
assert_not_contains "route-map no-match: no route command ran" "$out" "FALLBACK_RAN"
assert_not_contains "route-map no-match: no route command ran" "$out" "ECHO_RAN"

out="$("$JTOOL" --v8 keep-me)"
code=$?
assert_eq "route-map --v8: routes to its command, exit 0" "0" "$code"
assert_contains "route-map --v8: ran with the matched arg kept (no strip configured)" "$out" \
    "FALLBACK_RAN:--v8 keep-me"

out="$("$JTOOL" --v25 keep-me)"
code=$?
assert_eq "route-map --v25: routes to its own, different command" "0" "$code"
assert_contains "route-map --v25: ran with the matched arg kept" "$out" "ECHO_RAN:--v25 keep-me"

# --- route-map with --strip-matched-args removes the matched token ---
"$SHIMBACK" add jstrip -s "$FAKE_PRIMARY" --policy route-map \
    --route "--v8=$FAKE_FALLBACK" --route "--v25=$FAKE_ECHO" --strip-matched-args >/dev/null
JSTRIP="$(shim_path jstrip)"

out="$("$JSTRIP" --v8 keep-me)"
assert_contains "route-map strip: ran without the matched token" "$out" "FALLBACK_RAN:keep-me"
assert_not_contains "route-map strip: matched token was removed" "$out" "--v8"

# --- route-map: no fallback is stored (unused by this policy, like rewrite) ---
assert_not_contains "route-map: no fallback key stored" "$("$SHIMBACK" list --verbose 2>/dev/null)" \
    "fallback ="
cfg="$(cat "$(config_file)")"
# Scope the check to jtool's own section, not the whole file (other shims
# added later in this script do have a fallback).
jtool_section="$(awk '/^\[shims\.jtool\]/{p=1;next}/^\[shims\./{p=0}p' "$(config_file)")"
assert_not_contains "route-map: jtool has no fallback line" "$jtool_section" "fallback ="
assert_contains "route-map: jtool's two routes are both stored" "$cfg" '[[shims.jtool.routes]]'
assert_contains "route-map: jtool route match --v8 stored" "$cfg" 'match = "--v8"'
assert_contains "route-map: jtool route match --v25 stored" "$cfg" 'match = "--v25"'

# --- route-map without --route is rejected at add time ---
"$SHIMBACK" add badroute -s "$FAKE_PRIMARY" --policy route-map >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add with policy route-map and no --route should have failed"
fi
assert_not_contains "route-map without routes: nothing written to config" \
    "$("$SHIMBACK" list)" "badroute"

# --- route-map with two identical <match>=<command> routes is rejected,
# and nothing is written to disk (validate_shim_entry's duplicate-route
# check, enforced before any disk write, not just on the next reload) ---
"$SHIMBACK" add dupe -s "$FAKE_PRIMARY" --policy route-map \
    --route "--v8=$FAKE_FALLBACK" --route "--v8=$FAKE_FALLBACK" >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add with two identical route-map routes should have failed"
fi
assert_not_contains "route-map duplicate routes: nothing written to config" \
    "$("$SHIMBACK" list)" "dupe"

# --- fallback is not required for route-map (unlike every policy except
# rewrite) -- omitting -f entirely must succeed ---
"$SHIMBACK" add nofallback -s "$FAKE_PRIMARY" --policy route-map \
    --route "--v8=$FAKE_FALLBACK" >/dev/null 2>"$SANDBOX/err"
code=$?
assert_eq "route-map without -f/--fallback: add succeeds" "0" "$code"

finish
