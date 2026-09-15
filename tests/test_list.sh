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

# --- source_args/fallback_args are independent of policy: shown for a
# plain exit-code shim too (this is exactly what lets a shim double as a
# regular alias -- see README's "add" section) ---
"$SHIMBACK" add atool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" \
    --source-arg "-ltrah" --fallback-arg "-la" >/dev/null

full="$("$SHIMBACK" list --full)"
assert_contains "list --full: heuristic shows error patterns" "$full" \
    "error patterns: invalid option, illegal option"
assert_contains "list --full: exit-code-match shows exit codes" "$full" "exit codes: 2, 3"
assert_contains "list --full: route-args shows route args" "$full" "route args: special"
assert_contains "list --full: route-args shows strip matched args" "$full" \
    "strip matched args: true"
assert_contains "list --full: rewrite shows rewrite rules" "$full" \
    "rewrite rules: --full -> -ltrah"
assert_contains "list --full: source_args shown for a plain exit-code shim" "$full" \
    "source args: -ltrah"
assert_contains "list --full: fallback_args shown for a plain exit-code shim" "$full" \
    "fallback args: -la"

# --- list --full: every shim always contributes its own "symlink:" line
# (6 shims: sed, htool, xtool, rtool, rwtool, atool = 6), plus whatever
# policy-specific detail it has beyond that -- htool (1), xtool (1),
# rtool (2), rwtool (1), and atool (2) = 7 -- for 13 total. sed itself
# has nothing configured beyond the basics, so it contributes only its
# one "symlink:" line and nothing else. ---
detail_line_count="$(printf '%s\n' "$full" | grep -c '^        ')"
assert_eq "list --full: symlink line for every shim, plus policy-specific extras" \
    "13" "$detail_line_count"
assert_contains "list --full: sed shows its own symlink line" "$full" \
    "symlink: $(shim_path sed)"

# --- ls --full is the same alias as list --full ---
assert_eq "ls --full is an alias for list --full" "$full" "$("$SHIMBACK" ls --full)"

# --- an unexpected extra argument still fails ---
"$SHIMBACK" list --full extra >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "list --full extra: should have failed on the unexpected argument"
fi
assert_contains "list: unexpected argument error" "$(cat "$SANDBOX/err")" "unexpected argument"

# --- list shows a split-config shim (see test_split_config.sh) just like
# a config.toml one, plus its actual split file path under --full ---
"$SHIMBACK" add splittool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null
out="$("$SHIMBACK" list)"
assert_contains "list: shows a split-config shim in the compact table" "$out" "splittool"
full="$("$SHIMBACK" list --full)"
assert_contains "list --full: shows the split-config shim's own file path" "$full" \
    "config: $(dirname "$(config_file)")/splittool-config.toml"
assert_contains "list --full: shows the split-config shim's symlink path too" "$full" \
    "symlink: $(shim_path splittool)"

# --- list surfaces a real shim symlink with no configuration anywhere
# (its config.toml entry removed by hand, leaving the symlink behind) as
# an orphan, rather than silently omitting it -- see test_doctor.sh for
# `doctor fix`'s orphan-removal side of this feature ---
"$SHIMBACK" add orphantool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
CFG="$(config_file)"
awk '/^\[shims\.orphantool\]$/{skip=1;next} /^\[/{skip=0} !skip' "$CFG" >"$CFG.tmp" &&
    mv "$CFG.tmp" "$CFG"
assert_not_contains "setup: orphantool's entry removed from config.toml by hand" \
    "$(cat "$CFG")" "orphantool"

out="$("$SHIMBACK" list)"
assert_contains "list: surfaces an orphaned symlink" "$out" "orphantool"
assert_contains "list: explains why it's orphaned" "$out" \
    "orphaned symlink -- no config.toml entry or split config file found"
full="$("$SHIMBACK" list --full)"
assert_contains "list --full: shows an orphan's own symlink path too" "$full" \
    "symlink: $(shim_path orphantool)"

"$SHIMBACK" remove -y splittool >/dev/null
"$SHIMBACK" doctor fix -y >/dev/null

# --- empty config with --full is still just the empty-config message ---
"$SHIMBACK" remove -y sed >/dev/null
"$SHIMBACK" remove -y htool >/dev/null
"$SHIMBACK" remove -y xtool >/dev/null
"$SHIMBACK" remove -y rtool >/dev/null
"$SHIMBACK" remove -y rwtool >/dev/null
"$SHIMBACK" remove -y atool >/dev/null
out="$("$SHIMBACK" list --full)"
assert_eq "list --full: empty config message" "No shims configured." "$out"

finish
