#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

# Simulate a shell that actually has the shim directory on PATH (the
# sandbox's own rc file is written but never sourced by this script).
export PATH="$XDG_DATA_HOME/shimback/bin:$PATH"

# --- exit-code (the default policy): every section is there ---
"$SHIMBACK" add infotool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --diagnostic >/dev/null
out="$("$SHIMBACK" info infotool)"
code=$?
assert_eq "info: exits 0 for a configured shim" "0" "$code"
assert_contains "info: banner names the shim and its policy" "$out" "shim: infotool (exit-code)"
assert_contains "info: shows the shim's symlink path" "$out" "$(shim_path infotool)"
assert_contains "info: shows the config file path" "$out" "$(config_file)"
assert_contains "info: says the entry lives in config.toml" "$out" \
    "the [shims.infotool] entry in config.toml"
assert_contains "info: explains what the policy does" "$out" \
    "Runs the source; falls back if it exits non-zero."
assert_contains "info: shows the source" "$out" "fake_primary.sh"
assert_contains "info: shows the fallback" "$out" "fake_fallback.sh"
assert_contains "info: shows the diagnostic flag" "$out" "on (prints a note to stderr"
assert_contains "info: shows the default trial-run timeout" "$out" "2000 ms (default)"
assert_contains "info: shows the default output limit" "$out" "8 MiB (default)"
assert_contains "info: confirms the shim is first on PATH" "$out" \
    "typing 'infotool' runs this shim"
assert_contains "info: has a flow diagram" "$out" "Flow"
assert_contains "info: diagram shows the source trial run" "$out" "run SOURCE"
assert_contains "info: diagram shows the fallback branch" "$out" "exit != 0"
assert_contains "info: diagram shows the fallback command" "$out" "run FALLBACK"
assert_contains "info: reports a clean bill of health" "$out" "no problems noticed"

# --- the whole output is plain 7-bit ASCII (no box-drawing, no arrows) and
# carries no stray markup bytes when piped ---
non_ascii="$(printf '%s' "$out" | LC_ALL=C tr -d '\n\t -~' | wc -c | tr -d ' ')"
assert_eq "info: output is pure printable ASCII when piped" "0" "$non_ascii"

# --- each policy explains itself and draws its own flow ---
"$SHIMBACK" add heurtool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --policy heuristic \
    --error-pattern "invalid option" --error-pattern "illegal option" >/dev/null
out="$("$SHIMBACK" info heurtool)"
assert_contains "info heuristic: lists the patterns" "$out" '"invalid option"'
assert_contains "info heuristic: diagram covers the no-match case" "$out" \
    "exit != 0, no pattern matches"

"$SHIMBACK" add codetool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --policy exit-code-match \
    --exit-code 2 --exit-code 64 >/dev/null
out="$("$SHIMBACK" info codetool)"
assert_contains "info exit-code-match: lists the codes" "$out" "2, 64"
assert_contains "info exit-code-match: diagram names the codes" "$out" "exit is one of: 2, 64"

"$SHIMBACK" add routetool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --policy route-args \
    --route-arg x --route-arg y --strip-matched-args >/dev/null
out="$("$SHIMBACK" info routetool)"
assert_contains "info route-args: diagram shows the trigger args" "$out" \
    "is any argument one of:  x | y ?"
assert_contains "info route-args: notes the runs-directly behavior" "$out" \
    "live output, no trial run, no retry"
assert_contains "info route-args: notes that the matched arg is stripped" "$out" \
    "the matched argument is removed before forwarding"
assert_contains "info route-args: no trial-run limits apply" "$out" \
    "the limits below don't apply"

"$SHIMBACK" add splittool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --policy split-args \
    --split-source-arg -1 --split-fallback-arg -1 --split-fallback-arg -2 >/dev/null
out="$("$SHIMBACK" info splittool)"
assert_contains "info split-args: diagram has the both-match rule" "$out" \
    "the side with MORE arguments wins"
assert_contains "info split-args: notes the trial-run case" "$out" \
    "neither side's arguments match"

"$SHIMBACK" add rwtool -s "$FAKE_PRIMARY" --policy rewrite --rewrite "all=ls" --rewrite "-v=" \
    >/dev/null
out="$("$SHIMBACK" info rwtool)"
assert_contains "info rewrite: shows a rewrite rule" "$out" "all -> ls"
assert_contains "info rewrite: an empty <to> is shown as dropped" "$out" "-v -> (dropped)"
assert_contains "info rewrite: fallback is reported as unused" "$out" \
    "(not used by this policy)"

# --- route-map: every route, the no-match default, and per-route args that
# can only be set by hand-editing the config (the last route block written
# is rmtool's own, so appending lands the key inside it) ---
"$SHIMBACK" add rmtool -s "$FAKE_PRIMARY" --policy route-map \
    --route "--v8=$FAKE_FALLBACK" --route "--v25=$FAKE_ECHO" --strip-matched-args >/dev/null
printf 'args = ["--enable-preview"]\n' >>"$(config_file)"
out="$("$SHIMBACK" info rmtool)"
assert_contains "info route-map: lists the first route" "$out" "--v8"
assert_contains "info route-map: lists the second route" "$out" "--v25"
assert_contains "info route-map: shows a route's own args" "$out" "with args: --enable-preview"
assert_contains "info route-map: diagram folds the route's args into its command line" "$out" \
    "--enable-preview <args>"
assert_contains "info route-map: diagram has the no-match default" "$out" "(no match)"
assert_contains "info route-map: says the first match wins" "$out" \
    "the first match wins"

# --- per-shim capture overrides are attributed to the shim ---
"$SHIMBACK" add captool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --capture-timeout 500 \
    --capture-limit 1MiB >/dev/null
out="$("$SHIMBACK" info captool)"
assert_contains "info: shows a per-shim timeout override" "$out" "500 ms (this shim's override)"
assert_contains "info: shows a per-shim limit override" "$out" "1 MiB (this shim's override)"

# --- a split-config shim points at its own file ---
"$SHIMBACK" add splitfile -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null
out="$("$SHIMBACK" info splitfile)"
assert_contains "info split file: shows the split file's path" "$out" "splitfile-config.toml"
assert_contains "info split file: says it's a split config file" "$out" "a split config file"
assert_not_contains "info split file: no shadowing warning when there's no config.toml entry" \
    "$out" "is ignored"

# --- ...and a stale config.toml entry that the split file overrides is flagged ---
printf 'source = "%s"\nfallback = "%s"\n' "$FAKE_PRIMARY" "$FAKE_FALLBACK" \
    >"$(dirname "$(config_file)")/infotool-config.toml"
out="$("$SHIMBACK" info infotool)"
assert_contains "info: flags a config.toml entry shadowed by a split file" "$out" \
    "ignored -- the split file wins"
rm -f "$(dirname "$(config_file)")/infotool-config.toml"

# --- a missing symlink is reported, with the fix ---
rm -f "$(shim_path captool)"
out="$("$SHIMBACK" info captool)"
assert_contains "info: reports a missing symlink" "$out" "[fail] missing"
assert_contains "info: points at doctor fix" "$out" "\`shimback doctor fix\` recreates it"
assert_contains "info: counts the problem" "$out" "problem(s) noticed"

# --- another binary earlier on PATH bypasses the shim: flagged ---
EARLIER="$SANDBOX/earlier"
mkdir -p "$EARLIER"
printf '#!/bin/sh\n' >"$EARLIER/heurtool"
chmod +x "$EARLIER/heurtool"
out="$(PATH="$EARLIER:$PATH" "$SHIMBACK" info heurtool)"
assert_contains "info: flags a binary that shadows the shim on PATH" "$out" \
    "currently runs $EARLIER/heurtool instead -- the shim is bypassed"

# --- errors ---
"$SHIMBACK" info nosuchtool >/dev/null 2>"$SANDBOX/err"
code=$?
assert_eq "info: an unconfigured name exits 1" "1" "$code"
assert_contains "info: says no shim is configured" "$(cat "$SANDBOX/err")" \
    "no shim configured for 'nosuchtool'"

"$SHIMBACK" info infotoo >/dev/null 2>"$SANDBOX/err"
assert_contains "info: suggests the closest name for a typo" "$(cat "$SANDBOX/err")" \
    "did you mean 'infotool'?"

ln -s "$SHIMBACK" "$(shim_path ghost)"
out="$("$SHIMBACK" info ghost 2>&1)"
code=$?
assert_eq "info: an orphaned symlink exits 1" "1" "$code"
assert_contains "info: explains the orphan" "$out" "has a shim symlink but no configuration"

"$SHIMBACK" info >/dev/null 2>"$SANDBOX/err"
code=$?
assert_eq "info: no name is a usage error" "1" "$code"
assert_contains "info: names the missing argument" "$(cat "$SANDBOX/err")" "missing shim name"

"$SHIMBACK" info infotool extra >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "info: an extra argument should fail"
fi
assert_contains "info: unexpected argument error" "$(cat "$SANDBOX/err")" "unexpected argument"

"$SHIMBACK" info ../etc/passwd >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "info: a name with '/' should be rejected"
fi
assert_contains "info: explains the invalid name" "$(cat "$SANDBOX/err")" "invalid shim name"

# --- read-only: info never writes to config.toml ---
before="$(cat "$(config_file)")"
"$SHIMBACK" info rmtool >/dev/null
"$SHIMBACK" info heurtool >/dev/null
assert_eq "info: leaves config.toml untouched" "$before" "$(cat "$(config_file)")"

finish
