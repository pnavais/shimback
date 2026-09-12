#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

# --- plain list: no policy-specific detail lines, even for a shim with some ---
"$SHIMBACK" add sed -f "$FAKE_FALLBACK" >/dev/null
"$SHIMBACK" add htool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" \
    --policy heuristic --error-pattern "invalid option" --error-pattern "illegal option" \
    >/dev/null

out="$("$SHIMBACK" list)"
assert_contains "list: shows the compact table" "$out" "NAME"
assert_not_contains "list: no detail lines without --full" "$out" "error patterns:"

# --- list --full: shows policy-specific details, one per configured policy ---
"$SHIMBACK" add xtool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" \
    --policy exit-code-match --exit-code 2 --exit-code 3 >/dev/null
"$SHIMBACK" add rtool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" \
    --policy route-args --route-arg special --strip-matched-args >/dev/null
"$SHIMBACK" add rwtool -s "$FAKE_PRIMARY" --policy rewrite --rewrite "--full=-ltrah" >/dev/null

full="$("$SHIMBACK" list --full)"
assert_contains "list --full: heuristic shows error patterns" "$full" \
    "error patterns: invalid option, illegal option"
assert_contains "list --full: exit-code-match shows exit codes" "$full" "exit codes: 2, 3"
assert_contains "list --full: route-args shows route args" "$full" "route args: special"
assert_contains "list --full: route-args shows strip matched args" "$full" \
    "strip matched args: true"
assert_contains "list --full: rewrite shows rewrite rules" "$full" \
    "rewrite rules: --full -> -ltrah"

# --- list --full: a plain exit-code shim (sed) gets no extra lines at all --
# every detail line is indented 8 spaces, and the only shims with anything to
# show are htool (1 line), xtool (1), rtool (2), and rwtool (1) = 5 total, so
# if sed contributed one too this count would be 6. ---
detail_line_count="$(printf '%s\n' "$full" | grep -c '^        ')"
assert_eq "list --full: exit-code policy contributes no detail lines" "5" "$detail_line_count"

# --- ls --full is the same alias as list --full ---
assert_eq "ls --full is an alias for list --full" "$full" "$("$SHIMBACK" ls --full)"

# --- an unexpected extra argument still fails ---
"$SHIMBACK" list --full extra >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "list --full extra: should have failed on the unexpected argument"
fi
assert_contains "list: unexpected argument error" "$(cat "$SANDBOX/err")" "unexpected argument"

# --- empty config with --full is still just the empty-config message ---
"$SHIMBACK" remove sed >/dev/null
"$SHIMBACK" remove htool >/dev/null
"$SHIMBACK" remove xtool >/dev/null
"$SHIMBACK" remove rtool >/dev/null
"$SHIMBACK" remove rwtool >/dev/null
out="$("$SHIMBACK" list --full)"
assert_eq "list --full: empty config message" "No shims configured." "$out"

finish
