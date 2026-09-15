#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

# Simulate a shell that actually sourced the injected PATH block -- the
# sandbox's own .zshrc is written but never sourced by this test script.
export PATH="$XDG_DATA_HOME/shimback/bin:$PATH"

# --- doctor with nothing configured: healthy, exit 0 ---
out="$("$SHIMBACK" doctor)"
code=$?
assert_eq "doctor: exit 0 with no shims configured" "0" "$code"
assert_contains "doctor: reports no shims" "$out" "No shims configured."

# --- healthy shim: doctor passes ---
"$SHIMBACK" add mytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
out="$("$SHIMBACK" doctor)"
code=$?
assert_eq "doctor: exit 0 for a healthy shim" "0" "$code"
assert_contains "doctor: healthy shim reports ok symlink" "$out" "[ok]   symlink ->"
assert_contains "doctor: all checks passed" "$out" "all checks passed"

# --- dead symlink: doctor fails and says so ---
rm -f "$(shim_path mytool)"
out="$("$SHIMBACK" doctor)"
code=$?
if [ "$code" -eq 0 ]; then
    fail "doctor: should fail when a shim's symlink is missing"
fi
assert_contains "doctor: reports missing symlink" "$out" "no symlink at"

# recreate the symlink for the next checks
"$SHIMBACK" add mytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null

# --- doctor fix: recreates a missing symlink ---
MYTOOL_LINK="$(shim_path mytool)"
rm -f "$MYTOOL_LINK"
out="$("$SHIMBACK" doctor fix)"
assert_contains "doctor fix: reports recreating a missing symlink" "$out" \
    "[fixed] recreated missing symlink"
if [ ! -L "$MYTOOL_LINK" ]; then
    fail "doctor fix: expected mytool's symlink to be recreated"
fi
out2="$("$SHIMBACK" doctor)"
code2=$?
assert_eq "doctor: exit 0 after fix recreated the missing symlink" "0" "$code2"

# --- doctor fix: recreates a dangling symlink (target no longer exists) ---
rm -f "$MYTOOL_LINK"
ln -s "$SANDBOX/nonexistent-binary" "$MYTOOL_LINK"
out3="$("$SHIMBACK" doctor fix)"
assert_contains "doctor fix: reports recreating a dangling symlink" "$out3" \
    "[fixed] recreated dangling symlink"
new_target="$(readlink "$MYTOOL_LINK")"
assert_not_contains "doctor fix: dangling symlink no longer points at the missing target" \
    "$new_target" "nonexistent-binary"

# --- doctor fix: a no-op (no [fixed] line) once the symlink is healthy ---
out4="$("$SHIMBACK" doctor fix)"
assert_not_contains "doctor fix: nothing to fix once healthy" "$out4" "[fixed]"

# --- doctor: an unrecognized positional argument is a clear error ---
"$SHIMBACK" doctor bogus >/dev/null 2>"$SANDBOX/err"
code5=$?
if [ "$code5" -eq 0 ]; then
    fail "doctor bogus: should fail on an unrecognized argument"
fi

# --- fallback that has since disappeared: doctor fails ---
DRIFTED_FALLBACK="$SANDBOX/fallback-copy.sh"
cp "$FAKE_FALLBACK" "$DRIFTED_FALLBACK"
chmod +x "$DRIFTED_FALLBACK"
"$SHIMBACK" add drifted -s "$FAKE_PRIMARY" -f "$DRIFTED_FALLBACK" >/dev/null
rm -f "$DRIFTED_FALLBACK"

out="$("$SHIMBACK" doctor)"
code=$?
if [ "$code" -eq 0 ]; then
    fail "doctor: should fail when a fallback binary has disappeared"
fi
assert_contains "doctor: reports missing fallback" "$out" "does not exist or is not executable"

# --- add refuses a fallback/source that resolves back to shimback itself ---
"$SHIMBACK" add cycletool -s "$FAKE_PRIMARY" -f "$SHIMBACK" >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add: a fallback resolving to the shimback binary itself should be rejected"
fi
assert_contains "add: cycle error mentions the shimback binary" "$(cat "$SANDBOX/err")" \
    "resolves back to the shimback binary itself"
assert_not_contains "add: cyclic shim was not created" "$("$SHIMBACK" list)" "cycletool"

"$SHIMBACK" add cycletool2 -s "$SHIMBACK" -f "$FAKE_FALLBACK" >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add: a source resolving to the shimback binary itself should be rejected"
fi
assert_contains "add: source-cycle error mentions the shimback binary" "$(cat "$SANDBOX/err")" \
    "resolves back to the shimback binary itself"

# --- doctor detects a cycle induced by hand-editing config.toml (add
# already refuses to create one going forward, so this is the only way one
# can end up in the config) ---
CFG="$(config_file)"
{
    echo "version = 1"
    echo ""
    echo "[shims.cyc]"
    echo "source = \"$FAKE_PRIMARY\""
    printf 'fallback = "%s"\n' "$SHIMBACK"
    echo "policy = \"exit-code\""
} >"$CFG"
CYC_LINK="$(shim_path cyc)"
ln -sf "$SHIMBACK" "$CYC_LINK"

out="$("$SHIMBACK" doctor)"
code=$?
if [ "$code" -eq 0 ]; then
    fail "doctor: should fail when a fallback resolves back to shimback itself"
fi
assert_contains "doctor: reports the fallback cycle" "$out" \
    "resolves back to the shimback binary itself"

# --- doctor fix: interactive prompt loop rejects a bad answer, accepts a good one ---
out2="$(printf 'not-a-real-command\n%s\n' "$FAKE_FALLBACK" | "$SHIMBACK" doctor fix)"
assert_contains "doctor fix: rejects an invalid replacement and re-prompts" "$out2" \
    "does not exist, is not executable, or isn't on \$PATH -- try again."
assert_contains "doctor fix: accepts a valid replacement" "$out2" "[fixed] fallback updated to"
assert_contains "doctor fix: config change gets saved" "$out2" "saved config changes to"
assert_contains "doctor fix: cycle is gone" "$(cat "$CFG")" \
    "fallback = \"$FAKE_FALLBACK\""

# --- doctor fix: EOF on stdin (non-interactive) gives up gracefully, no hang ---
{
    echo "version = 1"
    echo ""
    echo "[shims.cyc]"
    echo "source = \"$FAKE_PRIMARY\""
    printf 'fallback = "%s"\n' "$SHIMBACK"
    echo "policy = \"exit-code\""
} >"$CFG"

out3="$(run_with_timeout 5 "$SANDBOX/fix_eof_out" "$SHIMBACK" doctor fix </dev/null; \
    cat "$SANDBOX/fix_eof_out")"
code3=$?
if [ "$code3" -eq 137 ]; then
    fail "doctor fix: hung waiting for input on a closed stdin"
fi
assert_contains "doctor fix: gives up gracefully on EOF" "$out3" "Leaving '$SHIMBACK' as-is."
assert_contains "doctor fix: still reports the unresolved cycle" "$out3" \
    "resolves back to the shimback binary itself"

# --- doctor warns (but doesn't fail) when ~/.zshrc.local exists without our
# block while ~/.zshrc has it (from an earlier add, before .zshrc.local
# existed); doctor fix migrates it, after which the warning is gone. The
# config at this point still has an unrelated unresolved cycle (left by the
# EOF test above), so doctor's exit code is already non-zero regardless --
# the point here is that the warning doesn't change the issue *count*. ---
ZSHRC="$HOME/.zshrc"
ZSHRC_LOCAL="$HOME/.zshrc.local"
assert_contains "migration setup: block is currently in .zshrc" "$(cat "$ZSHRC")" \
    "# >>> shimback >>>"

issues_before="$("$SHIMBACK" doctor | grep -o '[0-9]* issue(s) found')"

: >"$ZSHRC_LOCAL"
out4="$("$SHIMBACK" doctor)"
assert_contains "doctor: warns about a block left in .zshrc" "$out4" \
    "[warn] ~/.zshrc.local exists, but the shimback PATH block is still in ~/.zshrc"
issues_after="$(echo "$out4" | grep -o '[0-9]* issue(s) found')"
assert_eq "doctor: the migration warning doesn't add to the issue count" \
    "$issues_before" "$issues_after"

"$SHIMBACK" doctor fix >/dev/null
assert_contains "doctor fix: block moved into .zshrc.local" "$(cat "$ZSHRC_LOCAL")" \
    "# >>> shimback >>>"
assert_not_contains "doctor fix: block gone from .zshrc" "$(cat "$ZSHRC")" "shimback"

out5="$("$SHIMBACK" doctor)"
assert_not_contains "doctor: no more migration warning once moved" "$out5" \
    "zshrc.local exists"

# --- add --force: allows a path that doesn't exist yet ---
GHOST_SRC="$SANDBOX/ghost-source.sh"
GHOST_FB="$SANDBOX/ghost-fallback.sh"
out="$("$SHIMBACK" add ghosttool -s "$GHOST_SRC" -f "$GHOST_FB" --force)"
code=$?
assert_eq "add --force: succeeds even though source/fallback don't exist yet" "0" "$code"
assert_contains "add --force: stores force = true in config" "$(cat "$CFG")" "force = true"

# --- add --force: still requires a path, not a bare name ---
"$SHIMBACK" add ghosttool2 -s ghost-bare-name -f "$GHOST_FB" --force >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add --force: a bare source name with nothing to resolve against should be rejected"
fi
assert_contains "add --force: bare-name error explains why" "$(cat "$SANDBOX/err")" \
    "needs a path"

# --- add without --force still rejects a nonexistent path outright ---
"$SHIMBACK" add noforcetool -s "$SANDBOX/still-missing.sh" -f "$GHOST_FB" \
    >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add: a nonexistent source without --force should still be rejected"
fi
assert_contains "add: nonexistent-source error unchanged without --force" \
    "$(cat "$SANDBOX/err")" "does not exist, is not executable, or isn't on \$PATH"

# --- doctor: a forced-but-still-missing source/fallback is reported ok, not fail ---
out="$("$SHIMBACK" doctor)"
assert_contains "doctor: forced-missing source reported ok" "$out" \
    "[ok]   source: $GHOST_SRC (added with --force; not currently on disk, so not checked)"
assert_contains "doctor: forced-missing fallback reported ok" "$out" \
    "[ok]   fallback: $GHOST_FB (added with --force; not currently on disk, so not checked)"

# --- doctor: once the forced binary actually exists, the full normal check applies ---
cp "$FAKE_PRIMARY" "$GHOST_SRC"
cp "$FAKE_FALLBACK" "$GHOST_FB"
chmod +x "$GHOST_SRC" "$GHOST_FB"
out="$("$SHIMBACK" doctor)"
assert_contains "doctor: forced source now checked normally once it exists" "$out" \
    "[ok]   source: $GHOST_SRC"
assert_not_contains "doctor: no --force note once source actually exists" "$out" \
    "source: $GHOST_SRC (added with --force"
assert_contains "doctor: forced fallback now checked normally once it exists" "$out" \
    "[ok]   fallback: $GHOST_FB"
assert_not_contains "doctor: no --force note once fallback actually exists" "$out" \
    "fallback: $GHOST_FB (added with --force"

# --- clean slate before the scenarios below: the hand-edited config.toml
# truncation earlier in this file (`>"$CFG"`, staging the "cyc" cycle
# test) incidentally orphaned mytool/drifted -- invisible before this
# feature existed, since doctor never looked beyond config.toml -- and
# cyc's own cycle was deliberately left broken by the EOF test right
# after it. One `doctor fix` clears both: the piped line answers cyc's
# still-pending cycle prompt (it's first, in cfg order), and -y removes
# the orphans with no prompt of their own needed. ---
out0="$(printf '%s\n' "$FAKE_FALLBACK" | "$SHIMBACK" doctor fix -y)"
assert_contains "cleanup: cyc's leftover cycle gets fixed" "$out0" "[fixed] fallback updated to"
assert_contains "cleanup: mytool's leftover orphan gets removed" "$out0" \
    "[fixed] removed orphaned symlink"
assert_contains "cleanup: drifted's leftover orphan gets removed" "$out0" \
    "[fixed] removed orphaned symlink"
out0b="$("$SHIMBACK" doctor)"
assert_eq "cleanup: doctor is clean before the split-config/orphan scenarios" "0" "$?"
if echo "$out0b" | grep -q "orphaned\|resolves back to the shimback binary itself"; then
    fail "cleanup: doctor should be fully clean at this point: $out0b"
fi

# --- doctor/doctor fix consider a split-config shim too (see
# test_split_config.sh), and a cycle fix on one saves back to its own
# split file, not config.toml. `add` already refuses to create a cyclic
# shim going forward (see above), so -- same trick as the config.toml
# "cyc" case above -- the cycle has to be hand-edited in afterward. ---
"$SHIMBACK" add splitcyc -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null
SPLIT_FILE="$(dirname "$CFG")/splitcyc-config.toml"
{
    printf 'source = "%s"\n' "$FAKE_PRIMARY"
    printf 'fallback = "%s"\n' "$SHIMBACK"
    printf 'policy = "exit-code"\n'
} >"$SPLIT_FILE"

out="$("$SHIMBACK" doctor)"
assert_contains "doctor: sees a split-config shim at all" "$out" "splitcyc"
assert_contains "doctor: shows the split shim's own file path" "$out" \
    "config: split file at $SPLIT_FILE"
assert_contains "doctor: reports the split shim's cycle" "$out" \
    "resolves back to the shimback binary itself"

out2="$(printf '%s\n' "$FAKE_FALLBACK" | "$SHIMBACK" doctor fix)"
assert_contains "doctor fix: fixes the split shim's cycle" "$out2" "[fixed] fallback updated to"
assert_contains "doctor fix: saves back to the split file, not config.toml" "$out2" \
    "saved split config changes to $SPLIT_FILE"
assert_contains "doctor fix: split file actually updated" "$(cat "$SPLIT_FILE")" \
    "fallback = \"$FAKE_FALLBACK\""
assert_not_contains "doctor fix: config.toml untouched by the split shim's fix" \
    "$(cat "$CFG")" "splitcyc"
"$SHIMBACK" remove -y splitcyc >/dev/null

# --- an orphaned shim (real symlink, no config anywhere) is reported as
# an issue by default, not silently skipped ---
"$SHIMBACK" add orphantool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
awk '/^\[shims\.orphantool\]$/{skip=1;next} /^\[/{skip=0} !skip' "$CFG" >"$CFG.tmp" &&
    mv "$CFG.tmp" "$CFG"
ORPHAN_LINK="$(shim_path orphantool)"
if [ ! -L "$ORPHAN_LINK" ]; then
    fail "setup: expected orphantool's symlink to still exist"
fi

out="$("$SHIMBACK" doctor)"
code=$?
if [ "$code" -eq 0 ]; then
    fail "doctor: should fail when an orphaned symlink is present"
fi
assert_contains "doctor: reports the orphaned symlink" "$out" \
    "orphaned symlink -- no config.toml entry or split config file found"

# --- doctor fix: prompts before removing an orphan, rejects a "no" ---
out2="$(printf 'n\n' | "$SHIMBACK" doctor fix)"
assert_contains "doctor fix: prompts about the orphan" "$out2" \
    "has a real shim symlink but no configuration anywhere for it"
if [ ! -L "$ORPHAN_LINK" ]; then
    fail "doctor fix: declining the prompt should leave the orphan's symlink alone"
fi

# --- doctor fix: accepts a "y" and removes it ---
out3="$(printf 'y\n' | "$SHIMBACK" doctor fix)"
assert_contains "doctor fix: removes the orphan on 'y'" "$out3" \
    "[fixed] removed orphaned symlink"
if [ -e "$ORPHAN_LINK" ]; then
    fail "doctor fix: orphan's symlink should be gone after confirming"
fi

out4="$("$SHIMBACK" doctor)"
code4=$?
assert_eq "doctor: exit 0 once the orphan is gone" "0" "$code4"

# --- doctor fix -y: removes an orphan without prompting ---
"$SHIMBACK" add orphantool2 -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
awk '/^\[shims\.orphantool2\]$/{skip=1;next} /^\[/{skip=0} !skip' "$CFG" >"$CFG.tmp" &&
    mv "$CFG.tmp" "$CFG"
ORPHAN2_LINK="$(shim_path orphantool2)"

out5="$(run_with_timeout 5 "$SANDBOX/fix_y_out" "$SHIMBACK" doctor fix -y </dev/null; \
    cat "$SANDBOX/fix_y_out")"
code5=$?
if [ "$code5" -eq 137 ]; then
    fail "doctor fix -y: hung waiting for input on a closed stdin"
fi
assert_contains "doctor fix -y: removes the orphan without a prompt" "$out5" \
    "[fixed] removed orphaned symlink"
assert_not_contains "doctor fix -y: never printed the confirmation prompt" "$out5" \
    "Remove the symlink?"
if [ -e "$ORPHAN2_LINK" ]; then
    fail "doctor fix -y: orphan's symlink should be gone"
fi

finish
