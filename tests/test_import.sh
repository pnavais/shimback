#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

# --- round trip: config-backed + split-backed shims survive an export then
# import into a wiped-clean local state (standing in for a fresh machine --
# every shimback path resolves purely from $HOME/$XDG_*, so wiping those
# between export and import is equivalent to a second sandbox) ---
"$SHIMBACK" add ctool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
"$SHIMBACK" add stool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null
BACKUP="$SANDBOX/backup.sz"
"$SHIMBACK" export -o "$BACKUP" -y >/dev/null

wipe_local_state() {
    rm -rf "$XDG_CONFIG_HOME/shimback" "$XDG_DATA_HOME/shimback"
}

wipe_local_state
out="$("$SHIMBACK" import "$BACKUP")"
assert_contains "import: round-trip reports both shims imported" "$out" "2 imported"
full="$("$SHIMBACK" list --full)"
assert_contains "import: round-trip restores ctool (config.toml-backed)" "$full" "ctool"
assert_contains "import: round-trip restores stool (split-backed)" "$full" \
    "config: $(dirname "$(config_file)")/stool-config.toml"
if [ ! -L "$(shim_path ctool)" ]; then
    fail "import: round-trip did not recreate ctool's symlink"
fi

# --- dispatch actually works after import, not just the config bookkeeping ---
PATH_SAVE="$PATH"
PATH="$XDG_DATA_HOME/shimback/bin:$PATH"
out="$(FAKE_STDOUT="via-ctool" ctool)"
code=$?
PATH="$PATH_SAVE"
assert_eq "import: imported shim actually dispatches" "0" "$code"
assert_eq "import: imported shim runs its real source" "via-ctool" "$out"

# --- merge (the default): re-importing the same archive after adding an
# unrelated local shim leaves that shim untouched, and both archived shims
# come back as "unchanged" (identical content, not re-written) ---
"$SHIMBACK" add other -s "$FAKE_ECHO" -f "$FAKE_FALLBACK" >/dev/null
out="$("$SHIMBACK" import "$BACKUP")"
assert_contains "import: merge reports both shims unchanged" "$out" "2 unchanged"
assert_contains "import: merge leaves an unrelated local shim alone" "$("$SHIMBACK" list)" "other"

# --- conflict: a name that collides with different local content prompts;
# declining leaves the local version, accepting (or -y) overwrites it ---
"$SHIMBACK" add ctool -s "$FAKE_PRIMARY" -f "$FAKE_ECHO" >/dev/null
out="$(printf 'n\n' | "$SHIMBACK" import "$BACKUP")"
assert_contains "import: conflict prompts" "$out" "Overwrite with the archived version?"
assert_contains "import: declined conflict is reported as skipped" "$out" "1 skipped"
assert_contains "import: declined conflict leaves the local fallback" "$("$SHIMBACK" list)" \
    "fake_echo.sh"

out="$(printf 'y\n' | "$SHIMBACK" import "$BACKUP")"
assert_contains "import: accepted conflict is reported as updated" "$out" "1 updated"
assert_contains "import: accepted conflict applies the archived fallback" "$("$SHIMBACK" list)" \
    "fake_fallback.sh"

"$SHIMBACK" add ctool -s "$FAKE_PRIMARY" -f "$FAKE_ECHO" >/dev/null
out="$("$SHIMBACK" import -y "$BACKUP")"
assert_not_contains "import -y: no prompt text printed" "$out" "Overwrite with"
assert_contains "import -y: overwrites without asking" "$out" "1 updated"

# --- identical conflict: same name, byte-for-byte same config both sides
# -- never prompts, even with stdin closed ---
out="$("$SHIMBACK" import "$BACKUP" </dev/null)"
code=$?
assert_eq "import: identical content never prompts, succeeds with stdin closed" "0" "$code"
assert_contains "import: identical content reported as unchanged, not updated" "$out" \
    "2 unchanged"

# --- an "unchanged" (identical) shim still gets its symlink repaired if
# it's independently gone missing -- config.toml surviving while the shim
# directory itself got wiped is exactly this case. Still scoped to the
# archive's own names (confirmed by the "untouched" scenario further
# below), not a blanket doctor-fix sweep. ---
rm -f "$(shim_path ctool)"
out="$("$SHIMBACK" import "$BACKUP")"
assert_contains "import: still reports unchanged (config.toml content didn't change)" "$out" \
    "2 unchanged"
if [ ! -L "$(shim_path ctool)" ]; then
    fail "import: an unchanged shim's missing symlink should still have been recreated"
fi

# --- --replace: makes the archive the new source of truth -- local-only
# shims are removed (config AND symlink, not left as an orphan), the
# archived shims land exactly as the archive says. Starts from a clean,
# wiped state so exactly one shim ("onlylocal") is local-only. ---
wipe_local_state
"$SHIMBACK" import "$BACKUP" >/dev/null
"$SHIMBACK" add onlylocal -s "$FAKE_ECHO" -f "$FAKE_FALLBACK" >/dev/null
out="$(printf 'n\n' | "$SHIMBACK" import --replace "$BACKUP")"
assert_contains "import --replace, declined: local-only shim survives" "$("$SHIMBACK" list)" \
    "onlylocal"

out="$("$SHIMBACK" import --replace -y "$BACKUP")"
assert_contains "import --replace -y: reports the removal" "$out" "1 removed"
list_out="$("$SHIMBACK" list)"
assert_not_contains "import --replace -y: local-only shim's config is gone" "$list_out" \
    "onlylocal"
if [ -e "$(shim_path onlylocal)" ]; then
    fail "import --replace -y: local-only shim's symlink should be gone too, not an orphan"
fi
doctor_out="$("$SHIMBACK" doctor)"
assert_not_contains "import --replace -y: doctor sees no leftover orphan" "$doctor_out" \
    "onlylocal"

# --- --replace against an archive with zero shims refuses outright,
# changing nothing ---
EMPTY_CFG_DIR="$SANDBOX/empty-export"
mkdir -p "$EMPTY_CFG_DIR"
(cd "$EMPTY_CFG_DIR" && printf 'format_version = 1\nshimback_version = "0.0.0"\nhostname = "h"\nexported_at = "t"\nshim_count = 0\n' >manifest.toml && zip -0 -q empty.sz manifest.toml)
before="$("$SHIMBACK" list)"
"$SHIMBACK" import --replace -y "$EMPTY_CFG_DIR/empty.sz" >"$SANDBOX/err" 2>&1
code=$?
if [ "$code" -eq 0 ]; then
    fail "import --replace with zero shims in the archive should have refused"
fi
assert_contains "import --replace, zero shims: names the specific reason" "$(cat "$SANDBOX/err")" \
    "has no shims to import"
assert_eq "import --replace, zero shims: nothing changed" "$before" "$("$SHIMBACK" list)"

# --- corrupt/garbage input is rejected cleanly, nothing written ---
head -c 64 /dev/urandom >"$SANDBOX/garbage.sz"
before="$("$SHIMBACK" list)"
"$SHIMBACK" import "$SANDBOX/garbage.sz" >"$SANDBOX/err" 2>&1
code=$?
if [ "$code" -eq 0 ]; then
    fail "import of a garbage file should have failed"
fi
assert_contains "import: garbage file error names the problem" "$(cat "$SANDBOX/err")" \
    "does not look like a valid zip"
assert_eq "import: garbage file changed nothing" "$before" "$("$SHIMBACK" list)"

# --- a valid zip with no manifest.toml at all is rejected, not
# best-effort-imported ---
NOMANIFEST_DIR="$SANDBOX/no-manifest"
mkdir -p "$NOMANIFEST_DIR"
(cd "$NOMANIFEST_DIR" && echo "hi" >somefile.txt && zip -q nomanifest.sz somefile.txt)
"$SHIMBACK" import "$NOMANIFEST_DIR/nomanifest.sz" >"$SANDBOX/err" 2>&1
code=$?
if [ "$code" -eq 0 ]; then
    fail "import of an archive with no manifest.toml should have failed"
fi
assert_contains "import: no-manifest error names the problem" "$(cat "$SANDBOX/err")" \
    "has no manifest.toml"

# --- an unrecognized format_version is rejected, naming the version found ---
BADVER_DIR="$SANDBOX/bad-version"
mkdir -p "$BADVER_DIR"
(cd "$BADVER_DIR" && printf 'format_version = 2\n' >manifest.toml && zip -q badversion.sz manifest.toml)
"$SHIMBACK" import "$BADVER_DIR/badversion.sz" >"$SANDBOX/err" 2>&1
code=$?
if [ "$code" -eq 0 ]; then
    fail "import of an unrecognized format_version should have failed"
fi
assert_contains "import: bad format_version error names it" "$(cat "$SANDBOX/err")" \
    "format_version 2, expected 1"

# --- a DEFLATE-compressed member (plain `zip`'s default, not shimback
# export's own STORE-only output) is rejected, not silently misread ---
DEFLATE_DIR="$SANDBOX/deflated"
mkdir -p "$DEFLATE_DIR"
(cd "$DEFLATE_DIR" &&
    printf 'format_version = 1\nshimback_version = "0.0.0"\nhostname = "h"\nexported_at = "t"\nshim_count = 0\n' >manifest.toml &&
    dd if=/dev/zero of=big.txt bs=1024 count=4 2>/dev/null &&
    zip -q deflated.sz manifest.toml big.txt)
"$SHIMBACK" import "$DEFLATE_DIR/deflated.sz" >"$SANDBOX/err" 2>&1
code=$?
if [ "$code" -eq 0 ]; then
    fail "import of a DEFLATE-compressed archive should have failed"
fi
assert_contains "import: compressed entry error names the issue" "$(cat "$SANDBOX/err")" \
    "only uncompressed (STORE) entries are supported"

# --- split-vs-config.toml location transition, config.toml -> split ---
wipe_local_state
TRANS_DIR="$SANDBOX/transition"
mkdir -p "$TRANS_DIR"
"$SHIMBACK" add transtool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null
"$SHIMBACK" export -o "$TRANS_DIR/split-backed.sz" -y >/dev/null
wipe_local_state
"$SHIMBACK" add transtool -s "$FAKE_PRIMARY" -f "$FAKE_ECHO" >/dev/null # config.toml-backed, different content
"$SHIMBACK" import -y "$TRANS_DIR/split-backed.sz" >/dev/null
assert_not_contains "import: migrated to split -- no [shims.transtool] left in config.toml" \
    "$(cat "$(config_file)")" "[shims.transtool]"
if [ ! -f "$(dirname "$(config_file)")/transtool-config.toml" ]; then
    fail "import: migrated to split -- split file was not created"
fi

# --- split-vs-config.toml location transition, split -> config.toml ---
wipe_local_state
"$SHIMBACK" add transtool2 -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
"$SHIMBACK" export -o "$TRANS_DIR/config-backed.sz" -y >/dev/null
wipe_local_state
"$SHIMBACK" add transtool2 -s "$FAKE_PRIMARY" -f "$FAKE_ECHO" --split-config >/dev/null
"$SHIMBACK" import -y "$TRANS_DIR/config-backed.sz" >/dev/null
assert_contains "import: migrated to config.toml -- [shims.transtool2] now present" \
    "$(cat "$(config_file)")" "[shims.transtool2]"
if [ -f "$(dirname "$(config_file)")/transtool2-config.toml" ]; then
    fail "import: migrated to config.toml -- stale split file should be gone"
fi

# --- a broken source/fallback path is still imported, flagged in the
# summary, and left for `doctor` (not auto-suppressed via force) ---
wipe_local_state
GONE="$SANDBOX/will-vanish.sh"
cp "$FAKE_PRIMARY" "$GONE"
chmod +x "$GONE"
"$SHIMBACK" add brokentool -s "$GONE" -f "$FAKE_FALLBACK" >/dev/null
BROKEN_BACKUP="$SANDBOX/broken.sz"
"$SHIMBACK" export -o "$BROKEN_BACKUP" -y >/dev/null
rm -f "$GONE"
wipe_local_state
out="$("$SHIMBACK" import "$BROKEN_BACKUP")"
assert_contains "import: broken-path shim is still imported" "$out" "1 imported"
assert_contains "import: broken-path shim is flagged" "$out" "1 flagged"
doctor_out="$("$SHIMBACK" doctor)"
gone_line="$(printf '%s\n' "$doctor_out" | grep -F "$GONE")"
assert_contains "import: doctor (not fix) reports brokentool's own broken source" "$gone_line" \
    "$GONE"
assert_contains "import: that specific line is a real [fail], not force-suppressed" "$gone_line" \
    "fail"

# --- a new shim's symlink path already occupied by a non-shimback file:
# config is still written, the symlink is explicitly reported as not
# created, and the foreign file is left untouched ---
wipe_local_state
"$SHIMBACK" add occtool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
OCC_BACKUP="$SANDBOX/occ.sz"
"$SHIMBACK" export -o "$OCC_BACKUP" -y >/dev/null
wipe_local_state
mkdir -p "$(dirname "$(shim_path occtool)")"
printf 'not shimback' >"$(shim_path occtool)"
out="$("$SHIMBACK" import "$OCC_BACKUP" 2>&1)"
assert_contains "import: occupied symlink path is reported, not silently skipped" "$out" \
    "occupied by a non-shimback file"
assert_eq "import: the foreign file at the symlink path is left untouched" "not shimback" \
    "$(cat "$(shim_path occtool)")"
assert_contains "import: config is still written even though the symlink wasn't" \
    "$(cat "$(config_file)")" "[shims.occtool]"

# --- same, but the occupying entry is a symlink that resolves fine, just
# not to anything shimback -- not only a plain file ---
wipe_local_state
"$SHIMBACK" add occtool2 -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
OCC2_BACKUP="$SANDBOX/occ2.sz"
"$SHIMBACK" export -o "$OCC2_BACKUP" -y >/dev/null
wipe_local_state
mkdir -p "$(dirname "$(shim_path occtool2)")"
ln -s /bin/true "$(shim_path occtool2)"
out="$("$SHIMBACK" import "$OCC2_BACKUP" 2>&1)"
assert_contains "import: a foreign (but resolving) symlink is also reported as occupied" "$out" \
    "occupied by a non-shimback file"
if [ "$(readlink "$(shim_path occtool2)")" != "/bin/true" ]; then
    fail "import: the foreign symlink itself should have been left alone"
fi

# --- a local shim the import never touches keeps its existing symlink
# exactly as-is (the scoped per-imported-shim call list ran, not a
# blanket doctor-fix-style sweep). "untouched" is added (and broken) only
# AFTER the archive is built, so it's genuinely absent from it -- not just
# classified IDENTICAL and skipped for that unrelated reason. ---
wipe_local_state
"$SHIMBACK" add irrelevant -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
IRRELEVANT_BACKUP="$SANDBOX/irrelevant.sz"
"$SHIMBACK" export -o "$IRRELEVANT_BACKUP" -y >/dev/null
"$SHIMBACK" add untouched -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
UNTOUCHED_LINK="$(shim_path untouched)"
unlink "$UNTOUCHED_LINK" # make it dangling -- doctor fix would recreate this, import must not
"$SHIMBACK" import "$IRRELEVANT_BACKUP" >/dev/null
if [ -e "$UNTOUCHED_LINK" ]; then
    fail "import: recreated a dangling symlink for a shim it never imported"
fi

# --- --dry-run: reports the plan, writes nothing, holds no lock ---
wipe_local_state
"$SHIMBACK" add drytool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
DRY_BACKUP="$SANDBOX/dry.sz"
"$SHIMBACK" export -o "$DRY_BACKUP" -y >/dev/null
"$SHIMBACK" add drytool -s "$FAKE_PRIMARY" -f "$FAKE_ECHO" >/dev/null # now a conflict
before_cfg="$(cat "$(config_file)")"
out="$("$SHIMBACK" import --dry-run "$DRY_BACKUP")"
assert_contains "import --dry-run: reports the plan" "$out" "would ask"
assert_contains "import --dry-run: writes nothing" "$out" "nothing written"
assert_eq "import --dry-run: config.toml is byte-for-byte unchanged" "$before_cfg" \
    "$(cat "$(config_file)")"
# A fully wiped state proves --dry-run never reaches Phase 2 at all: no
# shim directory (only mkdir_p'd there), no lock file, no config.toml --
# a direct check, not just a same-process sequential proxy for one.
wipe_local_state
"$SHIMBACK" import --dry-run "$DRY_BACKUP" >/dev/null
if [ -e "$(dirname "$(shim_path drytool)")" ]; then
    fail "import --dry-run: should never create the shim directory (that only happens in phase 2)"
fi
if [ -e "$(config_file)" ]; then
    fail "import --dry-run: should never create config.toml from a wiped state"
fi

# --- missing archive path / unexpected extra argument ---
"$SHIMBACK" import >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "import with no archive path should have failed"
fi
assert_contains "import: missing archive path error" "$(cat "$SANDBOX/err")" "missing archive path"

"$SHIMBACK" import "$BACKUP" extra >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "import with an unexpected extra argument should have failed"
fi
assert_contains "import: unexpected argument error" "$(cat "$SANDBOX/err")" "unexpected extra argument"

finish
