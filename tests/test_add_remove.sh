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

# --- add's shell-startup-file PATH-update notices are silent by default,
# shown with --verbose, and shown by the config's own global default too ---
out="$("$SHIMBACK" add verbosetool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK")"
assert_not_contains "add: PATH notice hidden by default" "$out" "PATH updated"
assert_not_contains "add: restart notice hidden by default" "$out" "Restart your shell"

out="$("$SHIMBACK" add verbosetool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --verbose)"
assert_contains "add --verbose: PATH notice shown" "$out" "PATH updated"
assert_contains "add --verbose: restart notice shown" "$out" "Restart your shell"

CFG="$(config_file)"
{
    printf 'version = 1\nverbose = true\n'
    tail -n +2 "$CFG"
} >"$CFG.tmp" && mv "$CFG.tmp" "$CFG"

out="$("$SHIMBACK" add verbosetool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK")"
assert_contains "add: global verbose=true default shows the PATH notice" "$out" "PATH updated"
assert_contains "add: global verbose=true default shows the restart notice" "$out" \
    "Restart your shell"
assert_contains "add: global verbose=true persists across saves" "$(cat "$CFG")" "verbose = true"

# --- re-running add is idempotent: no duplicate marker block ---
"$SHIMBACK" add mytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
marker_count="$(count_occurrences '# >>> shimback >>>' "$ZSHRC")"
assert_eq "add: still exactly one marker block after re-add" "1" "$marker_count"

# --- the injected block is a plain export, with no zsh-defer indirection ---
assert_not_contains "add: PATH block doesn't use zsh-defer" "$(cat "$ZSHRC")" "zsh-defer"
assert_contains "add: PATH block is a plain export" "$(cat "$ZSHRC")" "export PATH="

# --- a block written by an older shimback (routed through zsh-defer) is
# rewritten as a plain export the next time add/init/install touches it ---
OLD_BLOCK_DIR="'$XDG_DATA_HOME/shimback/bin'"
printf '# >>> shimback >>>\nif command -v zsh-defer >/dev/null 2>&1; then\n    zsh-defer export PATH=%s:"$PATH"\nelse\n    export PATH=%s:"$PATH"\nfi\n# <<< shimback <<<\n' \
    "$OLD_BLOCK_DIR" "$OLD_BLOCK_DIR" >"$ZSHRC"
"$SHIMBACK" add mytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
assert_not_contains "add: an old zsh-defer block is migrated to a plain export" \
    "$(cat "$ZSHRC")" "zsh-defer"
assert_eq "add: the migrated block is still singular" "1" \
    "$(count_occurrences '# >>> shimback >>>' "$ZSHRC")"
assert_contains "add: the migrated block keeps the shim dir" "$(cat "$ZSHRC")" \
    "$XDG_DATA_HOME/shimback/bin"

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

# --- add must not leave an orphaned symlink behind when the existing
# config turns out to be invalid: the symlink/config mutation order used to
# be reversed, so a config that fails validation would still leave a fresh
# symlink on disk with no matching entry (see review.md) ---
CFG="$(config_file)"
CFG_BACKUP="$SANDBOX/config_backup.toml"
cp "$CFG" "$CFG_BACKUP"
{
    echo "version = 1"
    echo ""
    echo "[shims.brokenshim]"
    echo "fallback = \"$FAKE_FALLBACK\""
    echo "policy = \"route-args\""
} >"$CFG"

"$SHIMBACK" add orphantool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add: should fail when the existing config is invalid"
fi
assert_contains "add: reports the invalid existing config" "$(cat "$SANDBOX/err")" \
    "existing config"
if [ -e "$(shim_path orphantool)" ]; then
    fail "add: must not leave an orphaned symlink when the existing config was invalid"
fi

cp "$CFG_BACKUP" "$CFG"

# --- bare -s/-f command names resolve via $PATH, not just a raw stat, and
# get stored as the resolved absolute path (frozen, like an explicit path) ---
FIXTURES_DIR_REAL="$(cd "$(dirname "$FAKE_FALLBACK")" && pwd)"
OLD_PATH="$PATH"
export PATH="$FIXTURES_DIR_REAL:$PATH"

"$SHIMBACK" add pathtool -s "$(basename "$FAKE_PRIMARY")" -f "$(basename "$FAKE_FALLBACK")" \
    >/dev/null
code=$?
assert_eq "add: bare -s/-f names resolve via \$PATH" "0" "$code"

cfg_contents="$(cat "$(config_file)")"
assert_contains "add: resolved fallback stored as an absolute path" "$cfg_contents" \
    "fallback = \"$FAKE_FALLBACK\""
assert_contains "add: resolved source stored as an absolute path" "$cfg_contents" \
    "source = \"$FAKE_PRIMARY\""

export PATH="$OLD_PATH"

# --- a bare name not found anywhere on $PATH is rejected, mentioning $PATH ---
"$SHIMBACK" add badpathtool -s "$FAKE_PRIMARY" -f totally-not-a-real-command-xyz \
    >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "add: a fallback not found on \$PATH should fail"
fi
assert_contains "add: bad bare fallback error mentions \$PATH" "$(cat "$SANDBOX/err")" '$PATH'

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

# --- remove must not leave the symlink and config entry inconsistent when
# the config save fails: the symlink used to be unlinked before the save,
# so a failed save left the config still describing a shim whose symlink
# was already gone (see review.md) ---
"$SHIMBACK" add rmordertool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
RMORDER_LINK="$(shim_path rmordertool)"
if [ ! -e "$RMORDER_LINK" ]; then
    fail "remove-ordering setup: expected rmordertool's symlink to exist"
fi

CFG_DIR="$(dirname "$(config_file)")"
chmod 0500 "$CFG_DIR"
"$SHIMBACK" remove rmordertool >/dev/null 2>"$SANDBOX/err"
code=$?
chmod 0700 "$CFG_DIR"
if [ "$code" -eq 0 ]; then
    fail "remove: should fail when the config directory isn't writable"
fi
assert_contains "remove: reports the save failure" "$(cat "$SANDBOX/err")" "failed to save config"
if [ ! -e "$RMORDER_LINK" ]; then
    fail "remove: symlink must survive a failed config save, not be left orphaned"
fi
assert_contains "remove: config entry also survives a failed save" "$("$SHIMBACK" list)" \
    "rmordertool"

"$SHIMBACK" remove rmordertool >/dev/null

# --- remove must not claim success when the config entry is gone but its
# symlink could not actually be removed (see review.md) -- unlike the
# scenario above, here config_save succeeds (a different directory), only
# the symlink's own removal fails, so the failure has to be caught and
# reported separately rather than papered over by "it saved fine" ---
"$SHIMBACK" add rmsymlinkfail -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
RMSYMLINKFAIL_LINK="$(shim_path rmsymlinkfail)"
if [ ! -e "$RMSYMLINKFAIL_LINK" ]; then
    fail "remove-symlink-failure setup: expected rmsymlinkfail's symlink to exist"
fi

SHIM_DIR="$(dirname "$RMSYMLINKFAIL_LINK")"
chmod 0500 "$SHIM_DIR"
out="$("$SHIMBACK" remove rmsymlinkfail 2>&1)"
code=$?
chmod 0700 "$SHIM_DIR"
if [ "$code" -eq 0 ]; then
    fail "remove: should fail (not silently succeed) when the symlink can't be removed"
fi
assert_not_contains "remove: no false 'removed' success message" "$out" "removed 'rmsymlinkfail'"
assert_contains "remove: reports the partial removal" "$out" "partially removed"
assert_contains "remove: config entry gone -- symlink is now an orphan, not configured" \
    "$("$SHIMBACK" list)" "rmsymlinkfail  (orphaned symlink"
if [ ! -e "$RMSYMLINKFAIL_LINK" ]; then
    fail "remove: the un-removable symlink should still be there to match the failure reported"
fi
rm -f "$RMSYMLINKFAIL_LINK"

# --- add rolls back a brand-new shim's just-committed config entry if the
# shim directory can't actually be created afterward, rather than leaving
# a "configured" shim with no working symlink behind (see review.md) ---
OLD_DATA_HOME_RB="$XDG_DATA_HOME"
export XDG_DATA_HOME="$SANDBOX/blocked-data-home"
mkdir -p "$(dirname "$XDG_DATA_HOME")"
printf 'blocker' >"$XDG_DATA_HOME"

out="$("$SHIMBACK" add rollbacktool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" 2>&1)"
code=$?
export XDG_DATA_HOME="$OLD_DATA_HOME_RB"
rm -f "$SANDBOX/blocked-data-home"
if [ "$code" -eq 0 ]; then
    fail "add: should fail when the shim directory can't be created"
fi
assert_contains "add: reports the directory failure" "$out" "failed to create shim directory"
assert_not_contains "add: rolls back the config entry on failure" \
    "$(cat "$(config_file)")" "rollbacktool"

# --- generated startup files and config.toml are never world-writable,
# even under a permissive umask -- the mode is always set explicitly
# rather than left to whatever fopen()+umask would have produced (see
# review.md) ---
OLD_UMASK="$(umask)"
umask 000
"$SHIMBACK" add umasktool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
umask "$OLD_UMASK"

zshrc_perms="$(ls -l "$ZSHRC" | cut -c1-10)"
cfg_perms="$(ls -l "$(config_file)" | cut -c1-10)"
assert_eq "add under umask 000: .zshrc is not world-writable" "-rw-r--r--" "$zshrc_perms"
assert_eq "add under umask 000: config.toml is not world-writable" "-rw-------" "$cfg_perms"
"$SHIMBACK" remove umasktool >/dev/null

# --- fuzzy "did you mean" hints: edit-distance based, no fzf (or any other
# external tool) involved, so these always run regardless of the test
# machine's setup ---
"$SHIMBACK" add typotool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null

out="$("$SHIMBACK" remove typotoo 2>&1)"
assert_contains "remove typo (dropped char): did-you-mean hint" "$out" "did you mean 'typotool'?"

out2="$("$SHIMBACK" doctr 2>&1)"
assert_contains "unknown command typo (dropped char): did-you-mean hint" "$out2" \
    "did you mean 'doctor'?"

# --- an inserted (not just dropped) character must be caught too -- a
# subsequence-only fuzzy match (e.g. `fzf --filter`) can't catch this shape
# at all, since "shed" is longer than "sed" and so can never be a
# subsequence of it ---
"$SHIMBACK" add sed -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
out3="$("$SHIMBACK" remove shed 2>&1)"
assert_contains "remove typo (inserted char): did-you-mean hint" "$out3" "did you mean 'sed'?"
"$SHIMBACK" remove sed >/dev/null

"$SHIMBACK" remove typotool >/dev/null

# --- a genuinely unrelated name gets no hint at all, just the plain error ---
out4="$("$SHIMBACK" remove nonexistent-shim-xyz 2>&1)"
code4=$?
assert_not_contains "remove: no hint for an unrelated name" "$out4" "did you mean"
if [ "$code4" -eq 0 ]; then
    fail "remove: removing a nonexistent shim should still fail"
fi

# --- the "did you mean" hint comes after the error, not before it. Uses
# head/tail rather than sed for the line extraction below -- this sandbox's
# config now has a shim literally named "sed", and if the real shimback
# shim directory also happens to be on this test-running shell's own
# ambient $PATH, a bare `sed` invocation here would be dispatched through
# that shim instead of the real sed(1). ---
"$SHIMBACK" add sed -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
out5="$("$SHIMBACK" remove shed 2>&1)"
error_line="$(printf '%s\n' "$out5" | head -n1)"
hint_line="$(printf '%s\n' "$out5" | tail -n1)"
assert_contains "did-you-mean: error line comes first" "$error_line" "no shim configured"
assert_contains "did-you-mean: hint line comes second" "$hint_line" "did you mean 'sed'?"
"$SHIMBACK" remove sed >/dev/null

# --- rm is an alias for remove ---
"$SHIMBACK" add rmtool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
"$SHIMBACK" rm rmtool >/dev/null
assert_not_contains "rm: removed like remove would" "$("$SHIMBACK" list)" "rmtool"

# --- -y auto-removes when the typo has exactly one unique close match,
# with no intermediate "not found, removing X instead" noise -- just the
# final removal confirmation, naming the shim that was actually removed ---
"$SHIMBACK" add sed -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
outy="$("$SHIMBACK" remove -y shed)"
assert_eq "remove -y: prints only the final removal confirmation" \
    "shimback: removed 'sed'" "$outy"
assert_not_contains "remove -y: gone from config" "$("$SHIMBACK" list)" "sed"

# --- --yes is the long form of -y ---
"$SHIMBACK" add sed -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
"$SHIMBACK" remove --yes shed >/dev/null
assert_not_contains "remove --yes: gone from config" "$("$SHIMBACK" list)" "sed"

# --- -y does NOT auto-remove when two configured names tie for closest
# (an ambiguous typo target) -- falls back to just the plain hint ---
"$SHIMBACK" add sed -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
"$SHIMBACK" add sad -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
outtie="$("$SHIMBACK" remove -y sxd 2>&1)"
code_tie=$?
if [ "$code_tie" -eq 0 ]; then
    fail "remove -y: an ambiguous tie should not auto-remove anything"
fi
assert_not_contains "remove -y: no auto-pick message on a tie" "$outtie" "removing closest match"
assert_contains "remove -y: sed still configured after an ambiguous tie" \
    "$("$SHIMBACK" list)" "sed"
assert_contains "remove -y: sad still configured after an ambiguous tie" \
    "$("$SHIMBACK" list)" "sad"
"$SHIMBACK" remove sed >/dev/null
"$SHIMBACK" remove sad >/dev/null

# --- -y with a genuinely unrelated name still just fails, same as without -y ---
"$SHIMBACK" remove -y totally-unrelated-name-xyz >/dev/null 2>"$SANDBOX/err"
code_unrelated=$?
if [ "$code_unrelated" -eq 0 ]; then
    fail "remove -y: an unrelated name should still fail"
fi
assert_contains "remove -y: plain error for an unrelated name" "$(cat "$SANDBOX/err")" \
    "no shim configured"

# --- when ~/.zshrc.local exists, the PATH block goes there instead of
# ~/.zshrc (most zsh setups source .zshrc.local from .zshrc for
# machine-local overrides kept out of a dotfiles repo) ---
ZSHRC_LOCAL="$HOME/.zshrc.local"
zshrc_before_local="$(cat "$ZSHRC")"
: >"$ZSHRC_LOCAL"

"$SHIMBACK" add localtool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
assert_contains "add: with .zshrc.local present, PATH block goes there" \
    "$(cat "$ZSHRC_LOCAL")" "# >>> shimback >>>"
assert_eq "add: .zshrc itself is untouched once .zshrc.local exists" \
    "$zshrc_before_local" "$(cat "$ZSHRC")"

marker_count="$(count_occurrences '# >>> shimback >>>' "$ZSHRC_LOCAL")"
assert_eq "add: exactly one marker block in .zshrc.local" "1" "$marker_count"

"$SHIMBACK" uninstall --full >/dev/null 2>&1
assert_not_contains "uninstall --full: PATH block removed from .zshrc.local" \
    "$(cat "$ZSHRC_LOCAL")" "shimback"

rm -f "$ZSHRC_LOCAL"

# --- remove refuses a path-traversal name before it ever reaches path
# construction -- a bare "../../../victim" must never let a file outside
# shimback's own directories be deleted, however the traversal happens to
# resolve on this machine (see review.md) ---
VICTIM="$SANDBOX/victim-config.toml"
echo "sensitive data, not shimback's to touch" >"$VICTIM"
out="$("$SHIMBACK" remove '../../../victim' 2>&1)"
code=$?
if [ "$code" -eq 0 ]; then
    fail "remove: a path-traversal name should be rejected, not succeed"
fi
assert_contains "remove: rejects a path-traversal name" "$out" "invalid shim name"
if [ ! -e "$VICTIM" ]; then
    fail "remove: a file outside shimback's own directories must never be touched"
fi
rm -f "$VICTIM"

# --- add refuses to replace an existing shim symlink when the shim
# directory is writable by more than its owner -- that's the precondition
# a concurrent process would need to race the ownership check against the
# later rename() (see review.md); with owner-only permissions restored,
# the same replace succeeds normally again ---
"$SHIMBACK" add racetool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
RACETOOL_LINK="$(shim_path racetool)"
SHIM_DIR="$(dirname "$RACETOOL_LINK")"
chmod go+w "$SHIM_DIR"

out="$("$SHIMBACK" add racetool -s "$FAKE_PRIMARY" -f "$FAKE_ECHO" 2>&1)"
code=$?
chmod go-w "$SHIM_DIR"
if [ "$code" -eq 0 ]; then
    fail "add: replacing a shim in a group/other-writable directory should be refused"
fi
assert_contains "add: refuses to replace a shim in an insecure directory" "$out" \
    "writable by more than just its owner"

out2="$("$SHIMBACK" add racetool -s "$FAKE_PRIMARY" -f "$FAKE_ECHO" 2>&1)"
code2=$?
assert_eq "add: the same replace succeeds once the directory is owner-only again" "0" "$code2"

# --- shell marker matching only recognizes a marker line that occupies a
# complete line by itself -- unrelated content that happens to contain the
# exact marker text mid-line (a comment, a quoted string) must never be
# mistaken for shimback's own managed block and edited/removed (see
# review.md) ---
printf '# a comment mentioning # >>> shimback >>> mid-sentence, not a real marker\necho "unrelated line with # <<< shimback <<< embedded too"\nexport MY_OWN_VAR=1\n' \
    >"$ZSHRC"
fake_marker_content_before="$(cat "$ZSHRC")"

"$SHIMBACK" add markertest -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null

assert_contains "add: unrelated mid-line fake markers are left untouched" "$(cat "$ZSHRC")" \
    "$fake_marker_content_before"
assert_contains "add: a real marker block was still added correctly" "$(cat "$ZSHRC")" \
    "# >>> shimback >>>"

# The real regression this guards against: find_block() must locate
# shimback's *own* real block when a later command searches for it, not
# get thrown off by the fake mid-line text -- so `uninstall --full`
# (which removes the PATH block) must remove exactly the real block and
# leave the fake content alone, not fail to find anything (leaving a
# stale block) or, worse, mistake the fake lines for the start of a
# block and mangle them.
"$SHIMBACK" uninstall --full >/dev/null 2>&1
# A plain substring check can't tell a real standalone marker *line* apart
# from the fake mid-line text that also happens to contain the same bytes
# on purpose (that's the whole point of this fixture) -- grep -x for an
# exact full-line match is what actually distinguishes them.
if grep -qxF '# >>> shimback >>>' "$ZSHRC"; then
    fail "uninstall --full: the real marker block should be gone (a standalone marker line is \
still present)"
fi
assert_contains "uninstall --full: the fake mid-line content still survives untouched" \
    "$(cat "$ZSHRC")" "$fake_marker_content_before"

# --- add/remove/doctor fix hold a directory lock across their own
# check-then-mutate sequence on a shim symlink, so a concurrent shimback
# command racing the same symlink as the same user can't interleave with
# it (see review.md). Verified against a real concurrent holder, not just
# "doesn't crash": if `add` genuinely blocks on the lock, replacing an
# existing shim while something else holds it must take at least as long
# as the holder keeps it, not succeed instantly regardless. Uses Python's
# fcntl.flock(), which is the same underlying kernel primitive as the C
# side's flock() -- interoperable regardless of language. ---
if command -v python3 >/dev/null 2>&1; then
    "$SHIMBACK" add locktest -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
    LOCK_FILE="$(dirname "$(shim_path locktest)")/.shimback.lock"

    python3 -c "
import fcntl, time
f = open('$LOCK_FILE', 'a')
fcntl.flock(f, fcntl.LOCK_EX)
time.sleep(2)
" &
    HOLDER_PID=$!
    sleep 1 # let the holder actually acquire the lock before we race it

    start=$(date +%s)
    "$SHIMBACK" add locktest -s "$FAKE_PRIMARY" -f "$FAKE_ECHO" >/dev/null 2>&1
    replace_code=$?
    end=$(date +%s)
    wait "$HOLDER_PID"

    elapsed=$((end - start))
    if [ "$elapsed" -lt 1 ]; then
        fail "add: should have blocked on the shim directory lock held by a concurrent \
process (took only ${elapsed}s)"
    fi
    assert_eq "add: the replace still succeeds once the lock is released" "0" "$replace_code"

    "$SHIMBACK" remove -y locktest >/dev/null
fi

finish
