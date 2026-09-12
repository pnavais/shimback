#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

# --- exit-code policy ---
"$SHIMBACK" add ectool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null

ECTOOL="$(shim_path ectool)"

out="$(FAKE_EXIT_CODE=0 FAKE_STDOUT="hello-out" FAKE_STDERR="hello-err" "$ECTOOL" 2>"$SANDBOX/err")"
code=$?
assert_eq "exit-code success: exit status" "0" "$code"
assert_eq "exit-code success: stdout" "hello-out" "$out"
assert_eq "exit-code success: stderr" "hello-err" "$(cat "$SANDBOX/err")"

out="$(FAKE_EXIT_CODE=1 FAKE_STDOUT="secret-out" FAKE_STDERR="secret-err" "$ECTOOL" abc 2>"$SANDBOX/err")"
code=$?
assert_eq "exit-code failure: falls back to exit 0" "0" "$code"
assert_contains "exit-code failure: fallback ran" "$out" "FALLBACK_RAN:abc"
assert_not_contains "exit-code failure: source stdout is invisible" "$out" "secret-out"
assert_not_contains "exit-code failure: source stderr is invisible" "$(cat "$SANDBOX/err")" "secret-err"

# --- heuristic policy ---
"$SHIMBACK" add htool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" \
    --policy heuristic --error-pattern "illegal option" >/dev/null

HTOOL="$(shim_path htool)"

out="$(FAKE_EXIT_CODE=1 FAKE_STDERR="fake_primary: Illegal option -- x" "$HTOOL" -x 2>"$SANDBOX/err")"
code=$?
assert_eq "heuristic match: falls back to exit 0" "0" "$code"
assert_contains "heuristic match: fallback ran" "$out" "FALLBACK_RAN:-x"
assert_not_contains "heuristic match: no diagnostic printed (not configured)" "$(cat "$SANDBOX/err")" "shimback:"

out="$(FAKE_EXIT_CODE=3 FAKE_STDOUT="partial-out" FAKE_STDERR="totally different error" "$HTOOL" 2>"$SANDBOX/err")"
code=$?
assert_eq "heuristic no-match: source's real exit code surfaces" "3" "$code"
assert_eq "heuristic no-match: source's real stdout surfaces" "partial-out" "$out"
assert_eq "heuristic no-match: source's real stderr surfaces" "totally different error" "$(cat "$SANDBOX/err")"
assert_not_contains "heuristic no-match: fallback did not run" "$out" "FALLBACK_RAN"

# --- exit-code-match policy ---
"$SHIMBACK" add xtool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" \
    --policy exit-code-match --exit-code 42 --exit-code 43 >/dev/null

XTOOL="$(shim_path xtool)"

out="$(FAKE_EXIT_CODE=42 FAKE_STDOUT="secret-out" FAKE_STDERR="secret-err" "$XTOOL" y 2>"$SANDBOX/err")"
code=$?
assert_eq "exit-code-match match: falls back to exit 0" "0" "$code"
assert_contains "exit-code-match match: fallback ran" "$out" "FALLBACK_RAN:y"
assert_not_contains "exit-code-match match: source stdout is invisible" "$out" "secret-out"

out="$(FAKE_EXIT_CODE=7 FAKE_STDOUT="partial-out" FAKE_STDERR="totally different error" "$XTOOL" 2>"$SANDBOX/err")"
code=$?
assert_eq "exit-code-match no-match: source's real exit code surfaces" "7" "$code"
assert_eq "exit-code-match no-match: source's real stdout surfaces" "partial-out" "$out"
assert_not_contains "exit-code-match no-match: fallback did not run" "$out" "FALLBACK_RAN"

# --- exit-code-match without --exit-code is rejected at add time ---
"$SHIMBACK" add badxtool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --policy exit-code-match \
    >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add with policy exit-code-match and no --exit-code should have failed"
fi
assert_not_contains "exit-code-match without codes: nothing written to config" \
    "$("$SHIMBACK" list)" "badxtool"

# --- route-args policy ---
"$SHIMBACK" add rtool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" \
    --policy route-args --route-arg "special" >/dev/null

RTOOL="$(shim_path rtool)"

out="$(FAKE_EXIT_CODE=5 FAKE_STDOUT="primary-out" FAKE_STDERR="primary-err" "$RTOOL" other 2>"$SANDBOX/err")"
code=$?
assert_eq "route-args no-match: runs source directly, real exit code surfaces" "5" "$code"
assert_eq "route-args no-match: source's real stdout surfaces live (no capture)" "primary-out" "$out"
assert_eq "route-args no-match: source's real stderr surfaces live (no capture)" "primary-err" \
    "$(cat "$SANDBOX/err")"
assert_not_contains "route-args no-match: fallback did not run" "$out" "FALLBACK_RAN"

out="$("$RTOOL" special)"
code=$?
assert_eq "route-args match: routes to fallback, exit 0" "0" "$code"
assert_contains "route-args match: fallback ran with the matched arg kept" "$out" "FALLBACK_RAN:special"

# --- route-args with --strip-matched-args removes the matched arg ---
"$SHIMBACK" add striptool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" \
    --policy route-args --route-arg "special" --strip-matched-args >/dev/null
STRIPTOOL="$(shim_path striptool)"

out="$("$STRIPTOOL" special keep-me)"
assert_contains "route-args strip: fallback ran without the matched arg" "$out" "FALLBACK_RAN:keep-me"
assert_not_contains "route-args strip: matched arg was removed from forwarded args" "$out" "special"

# --- route-args diagnostic opt-in only fires when routed to fallback ---
"$SHIMBACK" add drtool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" \
    --policy route-args --route-arg "special" --diagnostic >/dev/null
DRTOOL="$(shim_path drtool)"
"$DRTOOL" special >/dev/null 2>"$SANDBOX/err"
assert_contains "route-args diagnostic: prints a note when routed to fallback" \
    "$(cat "$SANDBOX/err")" "shimback:"
"$DRTOOL" other >/dev/null 2>"$SANDBOX/err"
assert_eq "route-args diagnostic: silent when routed to source" "" "$(cat "$SANDBOX/err")"

# --- route-args without --route-arg is rejected at add time ---
"$SHIMBACK" add badroute -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --policy route-args \
    >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add with policy route-args and no --route-arg should have failed"
fi
assert_not_contains "route-args without route args: nothing written to config" \
    "$("$SHIMBACK" list)" "badroute"

# --- diagnostic opt-in ---
"$SHIMBACK" add dtool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --diagnostic >/dev/null
DTOOL="$(shim_path dtool)"
FAKE_EXIT_CODE=1 "$DTOOL" >/dev/null 2>"$SANDBOX/err"
assert_contains "diagnostic opt-in: prints a fallback note" "$(cat "$SANDBOX/err")" "shimback:"

# --- unconfigured shim ---
ln -sf "$SHIMBACK" "$(shim_path nope)"
out="$("$(shim_path nope)" 2>"$SANDBOX/err")"
code=$?
assert_eq "unconfigured shim: exit 127" "127" "$code"
assert_contains "unconfigured shim: clear error" "$(cat "$SANDBOX/err")" "no shim configured"

# --- source == fallback is rejected at add time ---
"$SHIMBACK" add sametool -s "$FAKE_PRIMARY" -f "$FAKE_PRIMARY" >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add with source==fallback should have failed"
fi
listing="$("$SHIMBACK" list)"
assert_not_contains "source==fallback: nothing written to config" "$listing" "sametool"

# --- large interleaved stdout/stderr: no pipe-capture deadlock ---
"$SHIMBACK" add bigtool -s "$FAKE_INTERLEAVED" -f "$FAKE_FALLBACK" >/dev/null
BIGTOOL="$(shim_path bigtool)"
if run_with_timeout 10 "$SANDBOX/big_out" "$BIGTOOL"; then
    big_code=0
else
    big_code=$?
fi
if [ "$big_code" -eq 137 ]; then
    fail "large interleaved output: capture appears to have deadlocked (killed by watchdog)"
else
    assert_eq "large interleaved output: exit 0" "0" "$big_code"
    got_lines="$(wc -l <"$SANDBOX/big_out" | tr -d ' ')"
    assert_eq "large interleaved output: all 4000 lines replayed" "4000" "$got_lines"
fi

finish
