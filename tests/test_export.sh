#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

backup_dir() {
    echo "$XDG_CONFIG_HOME/shimback/backups"
}

# set_backup_keys <backup_name-line-or-empty> <backup_override-line-or-empty> <backup_dir-line-or-empty>
# Rewrites config.toml's top-level backup_* keys, preserving version and
# every [shims.*] section untouched -- mirrors test_list.sh's own
# established awk-based config.toml surgery, just via grep -v/printf
# instead, since this needs to be idempotent across repeated calls.
set_backup_keys() {
    CFG="$(config_file)"
    rest="$(grep -v '^version = \|^backup_name = \|^backup_override = \|^backup_dir = ' "$CFG")"
    {
        printf 'version = 1\n'
        [ -n "$1" ] && printf '%s\n' "$1"
        [ -n "$2" ] && printf '%s\n' "$2"
        [ -n "$3" ] && printf '%s\n' "$3"
        printf '%s\n' "$rest"
    } >"$CFG.tmp" && mv "$CFG.tmp" "$CFG"
}

# --- nothing exportable: no config.toml on disk, no shims -> warns, exits
# 0, creates no file ---
out="$("$SHIMBACK" export 2>&1)"
code=$?
assert_eq "export: nothing exportable exits 0" "0" "$code"
assert_contains "export: nothing exportable warns" "$out" "nothing to export"
if [ -d "$(backup_dir)" ] && [ -n "$(ls -A "$(backup_dir)" 2>/dev/null)" ]; then
    fail "export: nothing exportable should not have created any file"
fi

# --- set up a config.toml-backed shim and a split-config-backed one ---
"$SHIMBACK" add ctool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
"$SHIMBACK" add stool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null

# --- default export (no -o): creates shimback_backup_<host>_<ts>.sz under
# the default backup dir, and prints the path it wrote ---
out="$("$SHIMBACK" export)"
code=$?
assert_eq "export: default export exits 0" "0" "$code"
FILE1="$(find "$(backup_dir)" -name 'shimback_backup_*.sz' 2>/dev/null | head -1)"
if [ -z "$FILE1" ]; then
    fail "export: default export did not create a file under $(backup_dir)"
fi
assert_contains "export: prints the path it wrote" "$out" "$FILE1"
assert_contains "export: reports the shim count" "$out" "2 shims"

# --- archive contents: config.toml (config-backed shim only), the
# split-backed shim's own file, and manifest.toml -- not a file for the
# config-backed shim, since that lives inside config.toml itself ---
listing="$(unzip -l "$FILE1")"
assert_contains "export: archive contains config.toml" "$listing" "config.toml"
assert_contains "export: archive contains the split-config shim's own file" "$listing" \
    "stool-config.toml"
assert_contains "export: archive contains manifest.toml" "$listing" "manifest.toml"
assert_not_contains "export: archive has no separate file for the config-backed shim" \
    "$listing" "ctool-config.toml"

manifest="$(unzip -p "$FILE1" manifest.toml)"
assert_contains "export: manifest records format_version" "$manifest" "format_version = 1"
assert_contains "export: manifest records shim_count" "$manifest" "shim_count = 2"

cfg_in_zip="$(unzip -p "$FILE1" config.toml)"
assert_contains "export: config.toml in the archive has ctool's section" "$cfg_in_zip" \
    "[shims.ctool]"
assert_not_contains "export: config.toml in the archive doesn't also have stool (split-only)" \
    "$cfg_in_zip" "[shims.stool]"

split_in_zip="$(unzip -p "$FILE1" stool-config.toml)"
assert_contains "export: the split file in the archive has stool's source" "$split_in_zip" \
    "$FAKE_PRIMARY"

# --- -o <existing dir>/ and -o <existing dir> (no trailing slash) both
# place the default name inside it ---
DIR1="$SANDBOX/exports1"
mkdir -p "$DIR1"
"$SHIMBACK" export -o "$DIR1/" >/dev/null
count1="$(find "$DIR1" -name '*.sz' | wc -l | tr -d ' ')"
assert_eq "export -o <dir>/: creates exactly one file inside it" "1" "$count1"

DIR2="$SANDBOX/exports2"
mkdir -p "$DIR2"
"$SHIMBACK" export -o "$DIR2" >/dev/null
count2="$(find "$DIR2" -name '*.sz' | wc -l | tr -d ' ')"
assert_eq "export -o <existing dir, no trailing slash>: creates exactly one file inside it" \
    "1" "$count2"

# --- -o <file>.sz uses that exact name ---
EXACT="$SANDBOX/exact-name.sz"
"$SHIMBACK" export -o "$EXACT" >/dev/null
if [ ! -f "$EXACT" ]; then
    fail "export -o <file>.sz: did not create the exact file requested"
fi

# --- -o <nonexistent dir>/ prompts to create it; declining aborts with a
# non-zero exit and no file/directory created, -y auto-creates ---
NONE="$SANDBOX/does-not-exist-yet"
printf 'n\n' | "$SHIMBACK" export -o "$NONE/" >/dev/null 2>&1
code=$?
if [ "$code" -eq 0 ]; then
    fail "export -o <nonexistent>/, declined: should have failed"
fi
if [ -e "$NONE" ]; then
    fail "export -o <nonexistent>/, declined: directory should not have been created"
fi

"$SHIMBACK" export -o "$NONE/" -y >/dev/null
if [ ! -d "$NONE" ]; then
    fail "export -o <nonexistent>/ -y: directory should have been auto-created"
fi
count3="$(find "$NONE" -name '*.sz' | wc -l | tr -d ' ')"
assert_eq "export -o <nonexistent>/ -y: creates exactly one file inside it" "1" "$count3"

# --- collision: exporting to the same exact path twice without --override
# adds a sequence number instead of overwriting ---
STATIC_DIR="$SANDBOX/static"
mkdir -p "$STATIC_DIR"
"$SHIMBACK" export -o "$STATIC_DIR/fixed.sz" >/dev/null
"$SHIMBACK" export -o "$STATIC_DIR/fixed.sz" >/dev/null
if [ ! -f "$STATIC_DIR/fixed.sz" ] || [ ! -f "$STATIC_DIR/fixed_1.sz" ]; then
    fail "export: second export to the same path should create fixed_1.sz, not overwrite"
fi

# --- --override overwrites the original in place instead of
# sequence-numbering ---
"$SHIMBACK" export -o "$STATIC_DIR/fixed.sz" --override >/dev/null
if [ -f "$STATIC_DIR/fixed_2.sz" ]; then
    fail "export --override: should overwrite fixed.sz in place, not create fixed_2.sz"
fi

# --- backup_name template: both <hostname> and <timestamp> get substituted ---
set_backup_keys 'backup_name = "custom-<hostname>-<timestamp>-name"' "" ""
CUSTOM_DIR="$SANDBOX/customname"
mkdir -p "$CUSTOM_DIR"
"$SHIMBACK" export -o "$CUSTOM_DIR/" >/dev/null
CUSTOM_FILE="$(find "$CUSTOM_DIR" -name 'custom-*-name.sz' 2>/dev/null | head -1)"
if [ -z "$CUSTOM_FILE" ]; then
    fail "export: backup_name template placeholders were not substituted"
fi
case "$CUSTOM_FILE" in
    *'<hostname>'* | *'<timestamp>'*)
        fail "export: backup_name placeholders left unsubstituted in $CUSTOM_FILE" ;;
    *) ;;
esac

# --- backup_override = true in config.toml forces override even without
# --override on the command line ---
set_backup_keys 'backup_name = "fixed-override-test"' 'backup_override = true' ""
OVERRIDE_DIR="$SANDBOX/override-cfg"
mkdir -p "$OVERRIDE_DIR"
"$SHIMBACK" export -o "$OVERRIDE_DIR/" >/dev/null
"$SHIMBACK" export -o "$OVERRIDE_DIR/" >/dev/null
if [ -f "$OVERRIDE_DIR/fixed-override-test_1.sz" ]; then
    fail "export: config backup_override=true should force override, not sequence-number"
fi
if [ ! -f "$OVERRIDE_DIR/fixed-override-test.sz" ]; then
    fail "export: config backup_override=true -- expected file not found"
fi

# --- backup_dir in config.toml changes the default (no -o) location, and
# `list --full`'s BACKUPS section reflects it ---
CUSTOM_BACKUP_DIR="$SANDBOX/custom-backup-location"
set_backup_keys "" "" "backup_dir = \"$CUSTOM_BACKUP_DIR\""
"$SHIMBACK" export >/dev/null
count_custom="$(find "$CUSTOM_BACKUP_DIR" -name '*.sz' 2>/dev/null | wc -l | tr -d ' ')"
assert_eq "export: honors config backup_dir as the default location" "1" "$count_custom"

full="$("$SHIMBACK" list --full)"
assert_contains "list --full: shows a BACKUPS section for the configured backup_dir" "$full" \
    "backups ($CUSTOM_BACKUP_DIR)"
assert_contains "list --full: BACKUPS section has a NAME/SIZE/DATE header" "$full" "NAME"

# --- list --full shows nothing extra when the backup dir is empty/missing ---
set_backup_keys "" "" "backup_dir = \"$SANDBOX/no-backups-here\""
full="$("$SHIMBACK" list --full)"
assert_not_contains "list --full: no BACKUPS section when the backup dir has nothing" "$full" \
    "backups ("

finish
