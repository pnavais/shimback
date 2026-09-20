#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

# `update` fetches a release tarball plus SHA256SUMS over curl. Point it at a
# local fake "release" directory through file:// (SHIMBACK_RELEASE_URL) so
# nothing here touches the network. Needs curl, tar, and a checksum tool to
# build that fake release; skip (77, mapped to SKIPPED by ctest) without them.
for tool in curl tar; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "$tool not found -- skipping the update test" 1>&2
        exit 77
    }
done
if command -v sha256sum >/dev/null 2>&1; then
    SUM_CMD="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
    SUM_CMD="shasum -a 256"
else
    echo "no sha256sum/shasum -- skipping the update test" 1>&2
    exit 77
fi

# The release asset for *this* machine, exactly as install.sh/update derive it.
case "$(uname -s)" in
    Darwin) OS=macos ;;
    Linux) OS=linux ;;
    *) echo "unsupported OS -- skipping" 1>&2; exit 77 ;;
esac
case "$(uname -m)" in
    x86_64 | amd64) ARCH=x86_64 ;;
    arm64 | aarch64) ARCH=arm64 ;;
    *) echo "unsupported arch -- skipping" 1>&2; exit 77 ;;
esac
PLATFORM="shimback-$OS-$ARCH"

PREFIX="$SANDBOX/opt"
DEST="$PREFIX/bin/shimback"
MAN_DEST="$PREFIX/share/man/man1/shimback.1"

BUNDLE_DIR="$SANDBOX/bundle"
mkdir -p "$BUNDLE_DIR"
cp "$SHIMBACK" "$BUNDLE_DIR/shimback"
chmod +x "$BUNDLE_DIR/shimback"
cp "$TEST_DIR/../man/shimback.1" "$BUNDLE_DIR/shimback.1"
BUNDLED_SHIMBACK="$BUNDLE_DIR/shimback"

# "Newer" builds are the real binary/man page with a few bytes appended:
# different contents (so update sees a change) but still carrying the marker
# every shimback embeds. They're only ever *shipped* and byte-compared, never
# executed by this test -- a modified signed binary can be refused by the OS.
NEW1_BIN="$SANDBOX/new1-shimback"
NEW2_BIN="$SANDBOX/new2-shimback"
cp "$SHIMBACK" "$NEW1_BIN" && printf 'newer-build-1' >>"$NEW1_BIN"
cp "$SHIMBACK" "$NEW2_BIN" && printf 'newer-build-2' >>"$NEW2_BIN"
NEW_MAN="$SANDBOX/new-shimback.1"
cp "$TEST_DIR/../man/shimback.1" "$NEW_MAN" && printf '.\\" refreshed-by-update\n' >>"$NEW_MAN"

RELEASE="$SANDBOX/release"
STAGE="$SANDBOX/stage"

# make_release <binary> <man page>: (re)builds the fake release directory.
make_release() {
    rm -rf "$RELEASE" "$STAGE"
    mkdir -p "$RELEASE" "$STAGE/$PLATFORM"
    cp "$1" "$STAGE/$PLATFORM/shimback"
    chmod +x "$STAGE/$PLATFORM/shimback"
    cp "$2" "$STAGE/$PLATFORM/shimback.1"
    tar -czf "$RELEASE/$PLATFORM.tar.gz" -C "$STAGE" "$PLATFORM"
    (cd "$RELEASE" && $SUM_CMD "$PLATFORM.tar.gz" >SHA256SUMS)
}

export SHIMBACK_RELEASE_URL="file://$RELEASE"
SCRATCH_TMP="$SANDBOX/tmp"
mkdir -p "$SCRATCH_TMP"
export TMPDIR="$SCRATCH_TMP"

# --- not installed: update has nothing to replace ---
make_release "$BUNDLE_DIR/shimback" "$BUNDLE_DIR/shimback.1"
out="$("$SHIMBACK" update 2>&1)"
code=$?
assert_eq "update: refuses when shimback isn't installed" "1" "$code"
assert_contains "update: says to install first" "$out" "isn't installed"

"$BUNDLED_SHIMBACK" install --prefix "$PREFIX" >/dev/null
if [ ! -x "$DEST" ]; then
    fail "setup: expected an installed binary at $DEST"
fi

# --- the release is identical to what's installed: nothing to do ---
out="$("$SHIMBACK" update 2>&1)"
code=$?
assert_eq "update: an identical release exits 0" "0" "$code"
assert_contains "update: warns when SHIMBACK_RELEASE_URL overrides the release location" "$out" \
    "using SHIMBACK_RELEASE_URL=file://$RELEASE"
assert_contains "update: the warning says checksums don't prove the mirror is trustworthy" "$out" \
    "only guard against corruption"
assert_contains "update: reports already up to date" "$out" "already up to date"
assert_contains "update: names the installed binary" "$out" "$DEST"
if ! cmp -s "$DEST" "$BUNDLE_DIR/shimback"; then
    fail "update: an identical release must leave the installed binary untouched"
fi

# --- --check reports a newer release without installing it ---
make_release "$NEW1_BIN" "$NEW_MAN"
out="$("$SHIMBACK" update --check 2>&1)"
code=$?
assert_eq "update --check: exits 0" "0" "$code"
assert_contains "update --check: reports an update is available" "$out" "an update is available"
if ! cmp -s "$DEST" "$BUNDLE_DIR/shimback"; then
    fail "update --check: must not modify the installed binary"
fi
assert_not_contains "update --check: must not refresh the man page" "$(cat "$MAN_DEST")" \
    "refreshed-by-update"

# --- update, run from the INSTALLED binary itself (the real-world case):
# replaces its own file atomically, refreshes the man page ---
out="$("$DEST" update 2>&1)"
code=$?
assert_eq "update (run from the installed binary): exits 0" "0" "$code"
assert_contains "update: reports the update" "$out" "updated $DEST"
assert_contains "update: reports the checksum was verified" "$out" "Checksum verified"
if ! cmp -s "$DEST" "$NEW1_BIN"; then
    fail "update: the installed binary should now be byte-identical to the release's"
fi
assert_contains "update: the man page was refreshed too" "$(cat "$MAN_DEST")" \
    "refreshed-by-update"
assert_contains "update: reports the man page refresh" "$out" "man page refreshed"

# --- a second update finds nothing new (compares contents, not versions) ---
out="$("$SHIMBACK" update 2>&1)"
assert_contains "update: nothing new the second time" "$out" "already up to date"

# --- a tampered/corrupt download is refused and leaves the install alone ---
make_release "$NEW2_BIN" "$NEW_MAN"
printf '%064d  %s\n' 0 "$PLATFORM.tar.gz" >"$RELEASE/SHA256SUMS"
out="$("$SHIMBACK" update 2>&1)"
code=$?
assert_eq "update: a checksum mismatch fails" "1" "$code"
assert_contains "update: names the checksum mismatch" "$out" "checksum mismatch"
if ! cmp -s "$DEST" "$NEW1_BIN"; then
    fail "update: a checksum mismatch must leave the installed binary untouched"
fi

# --- no checksum entry for this platform: refused ---
printf '%064d  %s\n' 0 "some-other-asset.tar.gz" >"$RELEASE/SHA256SUMS"
out="$("$SHIMBACK" update 2>&1)"
code=$?
assert_eq "update: a missing checksum entry fails" "1" "$code"
assert_contains "update: explains the missing entry" "$out" "no checksum entry"

# --- SHA256SUMS missing entirely: refused (never install unverified) ---
rm -f "$RELEASE/SHA256SUMS"
out="$("$SHIMBACK" update 2>&1)"
code=$?
assert_eq "update: a missing SHA256SUMS fails" "1" "$code"
assert_contains "update: refuses an unverified download" "$out" "unverified"
if ! cmp -s "$DEST" "$NEW1_BIN"; then
    fail "update: an unverifiable download must leave the installed binary untouched"
fi

# --- the asset itself missing: fails cleanly ---
make_release "$NEW2_BIN" "$NEW_MAN"
rm -f "$RELEASE/$PLATFORM.tar.gz"
out="$("$SHIMBACK" update 2>&1)"
code=$?
assert_eq "update: a missing release asset fails" "1" "$code"
assert_contains "update: reports the failed download" "$out" "failed to download"

# --- a valid newer release applies (installs the second "newer" build) ---
make_release "$NEW2_BIN" "$NEW_MAN"
"$SHIMBACK" update >/dev/null 2>&1
if ! cmp -s "$DEST" "$NEW2_BIN"; then
    fail "update: a later valid release should replace the installed binary again"
fi

# --- transport hardening: with the default GitHub location, curl is limited to
# HTTPS (including through redirects) and no override warning is shown; with
# SHIMBACK_RELEASE_URL set it's left unrestricted. A fake curl records its
# arguments and fails, so nothing touches the network. ---
FAKE_CURL_DIR="$SANDBOX/fakecurl"
mkdir -p "$FAKE_CURL_DIR"
printf '#!/bin/sh\nfor a in "$@"; do printf "%%s\\n" "$a"; done >"%s/curl_args"\nexit 1\n' "$SANDBOX" \
    >"$FAKE_CURL_DIR/curl"
chmod +x "$FAKE_CURL_DIR/curl"

out="$(PATH="$FAKE_CURL_DIR:$PATH" SHIMBACK_RELEASE_URL= "$SHIMBACK" update 2>&1)"
code=$?
assert_eq "update (default URL): a failed download fails" "1" "$code"
default_args="$(cat "$SANDBOX/curl_args")"
assert_contains "update (default URL): curl is limited to https" "$default_args" "--proto"
assert_contains "update (default URL): ...for the request" "$default_args" "=https"
assert_contains "update (default URL): ...and for redirects" "$default_args" "--proto-redir"
assert_contains "update (default URL): uses the GitHub latest-release location" "$default_args" \
    "https://github.com/pnavais/shimback/releases/latest/download/$PLATFORM.tar.gz"
assert_not_contains "update (default URL): no override warning" "$out" "SHIMBACK_RELEASE_URL"

out="$(PATH="$FAKE_CURL_DIR:$PATH" "$SHIMBACK" update 2>&1)"
assert_not_contains "update (override): curl isn't restricted to https" \
    "$(cat "$SANDBOX/curl_args")" "--proto"

# --- bad arguments ---
"$SHIMBACK" update bogus >/dev/null 2>"$SANDBOX/err"
code=$?
if [ "$code" -eq 0 ]; then
    fail "update: an unexpected extra argument should fail"
fi
assert_contains "update: unexpected argument error" "$(cat "$SANDBOX/err")" "unexpected extra argument"

# --- the scratch directory is always cleaned up, success or failure ---
leftover="$(ls -A "$SCRATCH_TMP")"
assert_eq "update: leaves no temporary files behind" "" "$leftover"

finish
