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

# --- rewrite policy: an alias/argument-macro mechanism, not a fallback one.
# fake_fallback.sh (which just echoes its own args as "FALLBACK_RAN:$*") is
# reused here as the *source* -- rewrite never touches fallback at all, so
# any argv-echoing fixture works fine as its source. ---
"$SHIMBACK" add rwtool -s "$FAKE_FALLBACK" --policy rewrite \
    --rewrite "--full=-ltrah" --rewrite "all=ls" \
    --rewrite "backup=-c -z -f backup.tar.gz" --rewrite "--verbose=" >/dev/null
code=$?
assert_eq "rewrite: add succeeds without a fallback" "0" "$code"

RWTOOL="$(shim_path rwtool)"

out="$("$RWTOOL" -a foo)"
assert_eq "rewrite: unmatched args pass through unchanged" "FALLBACK_RAN:-a foo" "$out"

out="$("$RWTOOL" --full)"
assert_eq "rewrite: single-token replacement" "FALLBACK_RAN:-ltrah" "$out"

out="$("$RWTOOL" --full extra)"
assert_eq "rewrite: replacement mixed with a pass-through arg" "FALLBACK_RAN:-ltrah extra" "$out"

out="$("$RWTOOL" all)"
assert_eq "rewrite: alias example (all -> ls)" "FALLBACK_RAN:ls" "$out"

out="$("$RWTOOL" backup)"
assert_eq "rewrite: multi-token replacement expands into separate args" \
    "FALLBACK_RAN:-c -z -f backup.tar.gz" "$out"

out="$("$RWTOOL" --verbose keep-me)"
assert_eq "rewrite: empty replacement drops the matched arg" "FALLBACK_RAN:keep-me" "$out"

FAKE_FALLBACK_EXIT_CODE=9 "$RWTOOL" >/dev/null
code=$?
assert_eq "rewrite: source's real exit code surfaces directly (no capture, no retry)" "9" "$code"

assert_contains "rewrite: list shows no fallback configured" "$("$SHIMBACK" list)" "none"

# --- rewrite without --rewrite is rejected at add time ---
"$SHIMBACK" add badrewrite -s "$FAKE_FALLBACK" --policy rewrite >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add with policy rewrite and no --rewrite should have failed"
fi
assert_contains "rewrite: missing rule error" "$(cat "$SANDBOX/err")" \
    "requires at least one --rewrite"

# --- a malformed --rewrite (no '=') is rejected ---
"$SHIMBACK" add badrewrite2 -s "$FAKE_FALLBACK" --policy rewrite --rewrite "noequals" \
    >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add with a malformed --rewrite value should have failed"
fi
assert_contains "rewrite: malformed rule error" "$(cat "$SANDBOX/err")" "<from>=<to>"

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

# --- source == fallback is still rejected even with matching (including
# both empty) source_args/fallback_args -- truly indistinguishable ---
"$SHIMBACK" add sameargstool -s "$FAKE_PRIMARY" --source-arg "-x" \
    -f "$FAKE_PRIMARY" --fallback-arg "-x" >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add with source==fallback and identical extra args should have failed"
fi
assert_contains "same binary, same args: rejected" "$(cat "$SANDBOX/err")" "no-op shim"

# --- but source == fallback with DIFFERENT source_args/fallback_args is
# not a no-op -- allowed at add time, and dispatch treats them as genuinely
# different invocations rather than short-circuiting to just one run ---
"$SHIMBACK" add samebindiffargs -s "$FAKE_FALLBACK" --source-arg "--srcflag" \
    -f "$FAKE_FALLBACK" --fallback-arg "--fbflag" >/dev/null
code=$?
assert_eq "same binary, different args: add succeeds" "0" "$code"
SAMEBINDIFFARGS="$(shim_path samebindiffargs)"

out="$("$SAMEBINDIFFARGS" ownarg)"
assert_eq "same binary, different args: source's own args used on success" \
    "FALLBACK_RAN:--srcflag ownarg" "$out"

out="$(FAKE_FALLBACK_EXIT_CODE=1 "$SAMEBINDIFFARGS" ownarg)"
assert_eq "same binary, different args: fallback's own (different) args used on failure" \
    "FALLBACK_RAN:--fbflag ownarg" "$out"

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

# --- source_args: fixed args prepended before source runs, every time --
# lets a shim double as a regular alias (e.g. source "ls" + source_args
# ["-ltrah"]). FAKE_FALLBACK is reused as *source* here purely because it
# echoes its received argv, which is exactly what's needed to verify the
# extra args actually landed (fallback is FAKE_PRIMARY so it's never
# confused with what's being checked; source succeeds, so it never runs
# anyway). ---
"$SHIMBACK" add aliastool -s "$FAKE_FALLBACK" -f "$FAKE_PRIMARY" \
    --source-arg "--extra1" --source-arg "--extra2" >/dev/null
ALIASTOOL="$(shim_path aliastool)"
out="$("$ALIASTOOL" own-arg)"
assert_eq "source_args: prepended before source's own args" \
    "FALLBACK_RAN:--extra1 --extra2 own-arg" "$out"

# --- fallback_args: fixed args prepended before fallback runs, every time
# fallback actually runs ---
"$SHIMBACK" add aliasfb -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" \
    --fallback-arg "--fbextra" >/dev/null
ALIASFB="$(shim_path aliasfb)"
out="$(FAKE_EXIT_CODE=1 "$ALIASFB" own-arg)"
assert_eq "fallback_args: prepended before fallback's own args" \
    "FALLBACK_RAN:--fbextra own-arg" "$out"

# --- route-args: source_args apply when source is the one picked ---
"$SHIMBACK" add aliasroute1 -s "$FAKE_FALLBACK" -f "$FAKE_PRIMARY" \
    --policy route-args --route-arg special --source-arg "--srcflag" >/dev/null
ALIASROUTE1="$(shim_path aliasroute1)"
out="$("$ALIASROUTE1" other)"
assert_eq "route-args no-match: uses source_args" "FALLBACK_RAN:--srcflag other" "$out"

# --- route-args: fallback_args apply when fallback is the one picked ---
"$SHIMBACK" add aliasroute2 -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" \
    --policy route-args --route-arg special --fallback-arg "--fbflag" >/dev/null
ALIASROUTE2="$(shim_path aliasroute2)"
out="$("$ALIASROUTE2" special)"
assert_eq "route-args match: uses fallback_args" "FALLBACK_RAN:--fbflag special" "$out"

# --- rewrite: source_args are prepended before the (possibly rewritten)
# user args, and are never themselves subject to rewriting ---
"$SHIMBACK" add aliasrewrite -s "$FAKE_FALLBACK" --policy rewrite \
    --rewrite "all=ls" --source-arg "--fixed" >/dev/null
ALIASREWRITE="$(shim_path aliasrewrite)"
out="$("$ALIASREWRITE" all)"
assert_eq "rewrite: source_args prepended, then the rewritten arg" \
    "FALLBACK_RAN:--fixed ls" "$out"

# --- --fallback-arg without a fallback (only reachable under --policy
# rewrite, the one policy where -f/--fallback is optional) is discarded
# with a warning rather than silently kept around or a hard failure ---
out="$("$SHIMBACK" add aliasnofallback -s "$FAKE_FALLBACK" --policy rewrite \
    --rewrite "all=ls" --fallback-arg "--orphan" 2>&1)"
code=$?
assert_eq "fallback_args without fallback: add still succeeds" "0" "$code"
assert_contains "fallback_args without fallback: warns" "$out" \
    "--fallback-arg given without a fallback -- discarding it"
shim_section="$(awk '/^\[shims\.aliasnofallback\]/{f=1; next} /^\[shims\./{f=0} f' "$(config_file)")"
assert_not_contains "fallback_args without fallback: discarded, not stored" "$shim_section" \
    "fallback_args"

finish
