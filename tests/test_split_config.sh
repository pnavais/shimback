#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

CFG_DIR="$(dirname "$(config_file)")"
BIN_DIR="$(cd "$(dirname "$SHIMBACK")" && pwd)"

# --- add --split-config writes <name>-config.toml to the config dir, with
# no [shims.x] section header, and no entry lands in config.toml itself ---
"$SHIMBACK" add splitcfg -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null

SPLIT_FILE="$CFG_DIR/splitcfg-config.toml"
if [ ! -f "$SPLIT_FILE" ]; then
    fail "add --split-config: expected a split file at $SPLIT_FILE"
fi
assert_not_contains "add --split-config: no entry in config.toml" \
    "$(cat "$(config_file)" 2>/dev/null)" "[shims.splitcfg]"
assert_not_contains "add --split-config: split file has no section header" \
    "$(cat "$SPLIT_FILE")" "[shims."
assert_contains "add --split-config: split file has fallback" "$(cat "$SPLIT_FILE")" \
    "fallback ="

SPLITCFG_LINK="$(shim_path splitcfg)"
if [ ! -L "$SPLITCFG_LINK" ]; then
    fail "add --split-config: expected a shim symlink like any other add"
fi

# --- and dispatch actually resolves and uses it, exactly like a
# config.toml-backed shim would ---
out="$(FAKE_EXIT_CODE=1 "$SPLITCFG_LINK" abc 2>"$SANDBOX/err")"
code=$?
assert_eq "split-config dispatch: falls back to exit 0" "0" "$code"
assert_contains "split-config dispatch: fallback ran" "$out" "FALLBACK_RAN:abc"

# --- moving the split file to the shim symlink's own directory still
# resolves it there (one of the three documented locations) ---
mv "$SPLIT_FILE" "$(dirname "$SPLITCFG_LINK")/splitcfg-config.toml"
out="$(FAKE_EXIT_CODE=1 "$SPLITCFG_LINK" xyz 2>"$SANDBOX/err")"
code=$?
assert_eq "split-config moved to symlink dir: still falls back" "0" "$code"
assert_contains "split-config moved to symlink dir: fallback ran" "$out" "FALLBACK_RAN:xyz"

# --- precedence: a copy next to the shim's own symlink wins over one in
# the shimback binary's own directory, which wins over the config dir ---
BIN_DIR_COPY="$BIN_DIR/splitcfg-config.toml"
printf 'source = "%s"\nfallback = "%s"\npolicy = "exit-code"\n' "$FAKE_PRIMARY" "$FAKE_ECHO" \
    >"$BIN_DIR_COPY"
out="$(FAKE_EXIT_CODE=1 "$SPLITCFG_LINK" 2>"$SANDBOX/err")"
assert_contains "split-config precedence: symlink dir copy still wins over binary dir" "$out" \
    "FALLBACK_RAN"
assert_not_contains "split-config precedence: binary dir copy shadowed" "$out" "ECHO_RAN"

rm -f "$(dirname "$SPLITCFG_LINK")/splitcfg-config.toml"
out="$(FAKE_EXIT_CODE=1 "$SPLITCFG_LINK" 2>"$SANDBOX/err")"
assert_contains "split-config precedence: binary dir copy now wins over config dir" "$out" \
    "ECHO_RAN"
rm -f "$BIN_DIR_COPY"

# --- remove sweeps the split file from wherever it currently lives, not
# just the default config-dir location ---
"$SHIMBACK" add sweeptool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null
SWEEP_LINK="$(shim_path sweeptool)"
SWEEP_SPLIT_CFG="$CFG_DIR/sweeptool-config.toml"
mv "$SWEEP_SPLIT_CFG" "$(dirname "$SWEEP_LINK")/sweeptool-config.toml"

"$SHIMBACK" remove -y sweeptool >/dev/null
if [ -e "$SWEEP_LINK" ]; then
    fail "remove: expected the symlink to be gone"
fi
if [ -e "$(dirname "$SWEEP_LINK")/sweeptool-config.toml" ]; then
    fail "remove: expected the moved split file to be swept too"
fi

# --- re-`add` without --split-config folds a split shim back into
# config.toml and removes the now-superseded split file ---
"$SHIMBACK" add toggletool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null
TOGGLE_SPLIT_CFG="$CFG_DIR/toggletool-config.toml"
if [ ! -f "$TOGGLE_SPLIT_CFG" ]; then
    fail "setup: expected a split file for toggletool"
fi

"$SHIMBACK" add toggletool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
assert_contains "add (un-split): entry now in config.toml" "$(cat "$(config_file)")" \
    "[shims.toggletool]"
if [ -e "$TOGGLE_SPLIT_CFG" ]; then
    fail "add (un-split): stale split file should have been removed"
fi

# --- and the other direction: re-`add --split-config` on a config.toml
# shim moves it out into its own file ---
"$SHIMBACK" add toggletool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null
assert_not_contains "add --split-config (re-split): entry removed from config.toml" \
    "$(cat "$(config_file)")" "[shims.toggletool]"
if [ ! -f "$TOGGLE_SPLIT_CFG" ]; then
    fail "add --split-config (re-split): expected the split file back"
fi

"$SHIMBACK" remove -y toggletool >/dev/null

# --- a failed `add --split-config` on an EXISTING split shim must not
# destroy the previous, still-valid split file: back it up before
# overwriting, and restore it if a later step (here, shim-directory
# creation) fails (see review.md) ---
"$SHIMBACK" add rollbacktool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null
ROLLBACK_SPLIT_CFG="$CFG_DIR/rollbacktool-config.toml"
if [ ! -f "$ROLLBACK_SPLIT_CFG" ]; then
    fail "setup: expected a split file for rollbacktool"
fi
ORIGINAL_SPLIT_CONTENT="$(cat "$ROLLBACK_SPLIT_CFG")"

SHIM_DIR="$XDG_DATA_HOME/shimback/bin"
rm -rf "$SHIM_DIR"
# A plain file where the shim directory should be makes mkdir_p() fail.
touch "$SHIM_DIR"

set +e
"$SHIMBACK" add rollbacktool -s "$FAKE_PRIMARY" -f "$FAKE_ECHO" --split-config >/dev/null 2>&1
rollback_code=$?
set -e
if [ "$rollback_code" -eq 0 ]; then
    fail "add --split-config: expected a non-zero exit when the shim directory is blocked"
fi

assert_eq "add --split-config (rollback): split file content unchanged" \
    "$ORIGINAL_SPLIT_CONTENT" "$(cat "$ROLLBACK_SPLIT_CFG" 2>/dev/null)"
if ls "$CFG_DIR"/*.rollback.* >/dev/null 2>&1; then
    fail "add --split-config (rollback): no rollback backup files should be left behind"
fi

rm -f "$SHIM_DIR"
mkdir -p "$SHIM_DIR"
"$SHIMBACK" remove -y rollbacktool >/dev/null

finish
