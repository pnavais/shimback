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

# --- compact table: fallback shows just the binary name, not its full
# frozen path (source, by contrast, still shows the full path -- only
# fallback gets shortened here) ---
assert_contains "list: fallback shown as just the binary name" "$out" \
    "$(basename "$FAKE_FALLBACK")"
assert_not_contains "list: fallback's full path not shown in compact mode" "$out" \
    "$FAKE_FALLBACK"

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
# rtool (2), rwtool (1), and atool (2) = 7 -- for 13 total. On top of
# that: sed's own "source resolves to:" line (its source is left auto,
# the only one here) for 14, plus a "fallback:" line for every shim with
# an explicit fallback -- sed, htool, xtool, rtool, atool (rwtool has
# none; --policy rewrite never uses -f/--fallback) -- 5 more, for 19. ---
detail_line_count="$(printf '%s\n' "$full" | grep -c '^        ')"
assert_eq "list --full: symlink line for every shim, plus policy-specific extras" \
    "19" "$detail_line_count"
assert_contains "list --full: sed shows its own symlink line" "$full" \
    "symlink: $(shim_path sed)"
assert_contains "list --full: sed's auto source shows what it resolves to" "$full" \
    "source resolves to:"
assert_contains "list --full: shows the fallback's full frozen path" "$full" \
    "fallback: $FAKE_FALLBACK"

# --- ls --full is the same alias as list --full ---
assert_eq "ls --full is an alias for list --full" "$full" "$("$SHIMBACK" ls --full)"

# --- more than one positional argument still fails (one is now a valid
# name filter -- see below) ---
"$SHIMBACK" list --full extra1 extra2 >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "list --full extra1 extra2: should have failed on the second argument"
fi
assert_contains "list: unexpected argument error" "$(cat "$SANDBOX/err")" "unexpected argument"

# --- name filter: an optional <pattern> narrows the table to matching
# names -- literal, or a glob (*, ?, [...]), same subset route-map's own
# match values use. Six shims exist at this point: sed, htool, xtool,
# rtool, rwtool, atool. ---
out="$("$SHIMBACK" list sed)"
assert_contains "list <pattern>: exact literal name matches" "$out" "sed"
assert_not_contains "list <pattern>: exact literal name excludes others" "$out" "htool"

out="$("$SHIMBACK" list 'r*')"
assert_contains "list <pattern>: prefix glob matches rtool" "$out" "rtool"
assert_contains "list <pattern>: prefix glob matches rwtool" "$out" "rwtool"
assert_not_contains "list <pattern>: prefix glob excludes non-matching names" "$out" "sed"

out="$("$SHIMBACK" list '?tool')"
assert_contains "list <pattern>: '?' glob matches htool" "$out" "htool"
assert_contains "list <pattern>: '?' glob matches xtool" "$out" "xtool"
assert_not_contains "list <pattern>: '?' glob excludes a longer prefix (rwtool)" "$out" "rwtool"

# --- the filter works regardless of where --full appears relative to it
# (getopt's own permutation, not special-cased) ---
full_before="$("$SHIMBACK" list --full 'r*')"
full_after="$("$SHIMBACK" list 'r*' --full)"
assert_eq "list <pattern>: --full works before or after the pattern" "$full_before" "$full_after"
assert_contains "list --full <pattern>: still shows policy-specific detail" "$full_before" \
    "symlink:"

# --- no match: a distinct message from the genuinely-empty-config one,
# and --full's backups section (independent of the name filter) still
# runs regardless ---
out="$("$SHIMBACK" list 'nonexistent*')"
assert_eq "list <pattern>: no-match message names the pattern" \
    "No shims match 'nonexistent*'." "$out"
out="$("$SHIMBACK" list --full 'nonexistent*')"
assert_contains "list --full <pattern>: no-match message still shown with --full" "$out" \
    "No shims match 'nonexistent*'."

# --- ls <pattern> is the same alias as list <pattern> ---
assert_eq "ls <pattern> is an alias for list <pattern>" \
    "$("$SHIMBACK" list 'r*')" "$("$SHIMBACK" ls 'r*')"

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

# --- the name filter applies uniformly to orphaned symlinks too, not
# just config-backed entries -- orphantool has no config.toml entry at
# all at this point, yet still matches by name ---
out="$("$SHIMBACK" list 'orphan*')"
assert_contains "list <pattern>: matches an orphaned symlink by name" "$out" "orphantool"
out="$("$SHIMBACK" list 'notorphan*')"
assert_not_contains "list <pattern>: non-matching pattern excludes the orphan too" "$out" \
    "orphantool"

"$SHIMBACK" remove -y splittool >/dev/null
"$SHIMBACK" doctor fix -y >/dev/null

# --- a shim symlink pointing at a DIFFERENT (but still genuinely
# shimback) binary -- e.g. left behind by an older build, or one
# installed at a different location -- must still be recognized as a
# real (if unconfigured) shim, not silently invisible to `list`/`doctor`
# just because it doesn't resolve to *this* exact running binary's own
# path (see review.md) ---
OLD_BUILD_COPY="$SANDBOX/shimback-old-build-copy"
cp "$SHIMBACK" "$OLD_BUILD_COPY"
chmod +x "$OLD_BUILD_COPY"
RELOCATED_LINK="$(shim_path relocatedtool)"
ln -sf "$OLD_BUILD_COPY" "$RELOCATED_LINK"

out="$("$SHIMBACK" list)"
assert_contains "list: recognizes a shim pointing at a different shimback build" "$out" \
    "relocatedtool"
assert_contains "list: it's reported as orphaned (no config), not silently dropped" "$out" \
    "orphaned symlink -- no config.toml entry or split config file found"

"$SHIMBACK" doctor fix -y >/dev/null 2>&1
if [ -e "$RELOCATED_LINK" ]; then
    fail "doctor fix: the orphaned relocated-build symlink should have been offered for removal"
fi
rm -f "$OLD_BUILD_COPY"

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
