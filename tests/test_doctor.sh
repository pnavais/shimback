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

finish
