#!/bin/sh
# Shared helpers for shimback's black-box shell tests. Meant to be sourced,
# not executed directly: `. "$(dirname "$0")/test_common.sh" "$1"`.
# Sets up an isolated $HOME/$XDG_CONFIG_HOME/$XDG_DATA_HOME sandbox so tests
# never touch the developer's real shell rc files, and cleans it up on exit.

SHIMBACK="$1"
if [ -z "$SHIMBACK" ] || [ ! -x "$SHIMBACK" ]; then
    echo "usage: $0 <path-to-shimback-binary>" 1>&2
    exit 2
fi

TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
FIXTURES_DIR="$TEST_DIR/fixtures"
FAKE_PRIMARY="$FIXTURES_DIR/fake_primary.sh"
FAKE_FALLBACK="$FIXTURES_DIR/fake_fallback.sh"
FAKE_INTERLEAVED="$FIXTURES_DIR/fake_interleaved.sh"
chmod +x "$FAKE_PRIMARY" "$FAKE_FALLBACK" "$FAKE_INTERLEAVED"

SANDBOX="$(mktemp -d)"
export HOME="$SANDBOX/home"
export XDG_CONFIG_HOME="$SANDBOX/home/.config"
export XDG_DATA_HOME="$SANDBOX/home/.local/share"
export SHELL="/bin/zsh"
mkdir -p "$HOME"

cleanup() {
    rm -rf "$SANDBOX"
}
trap cleanup EXIT INT TERM

FAILURES=0

fail() {
    echo "FAIL: $*" 1>&2
    FAILURES=$((FAILURES + 1))
}

assert_eq() {
    # assert_eq <description> <expected> <actual>
    if [ "$2" != "$3" ]; then
        fail "$1: expected [$2], got [$3]"
    fi
}

assert_contains() {
    # assert_contains <description> <haystack> <needle>
    case "$2" in
        *"$3"*) ;;
        *) fail "$1: expected output to contain [$3], got [$2]" ;;
    esac
}

assert_not_contains() {
    # assert_not_contains <description> <haystack> <needle>
    case "$2" in
        *"$3"*) fail "$1: expected output NOT to contain [$3], but got [$2]" ;;
        *) ;;
    esac
}

shim_path() {
    echo "$XDG_DATA_HOME/shimback/bin/$1"
}

config_file() {
    echo "$XDG_CONFIG_HOME/shimback/config.toml"
}

# count_occurrences <needle> <file>
count_occurrences() {
    grep -o -F "$1" "$2" 2>/dev/null | wc -l | tr -d ' '
}

# run_with_timeout <seconds> <outfile> <cmd...>
# A portable (no GNU coreutils `timeout` dependency) watchdog: runs <cmd...>
# in the background, kills it if it hasn't finished within <seconds>.
run_with_timeout() {
    secs="$1"
    shift
    outfile="$1"
    shift
    "$@" >"$outfile" 2>&1 &
    cmd_pid=$!
    # Redirected to /dev/null: if the command finishes before the timeout,
    # the watchdog's `kill` may not land before its `sleep` child is
    # reparented on exit -- that lingering sleep must not keep holding this
    # script's own stdout/stderr open, or a pipe-reading caller (e.g. ctest)
    # would block for the rest of the timeout waiting for EOF.
    (
        sleep "$secs"
        kill -9 "$cmd_pid" 2>/dev/null
    ) >/dev/null 2>&1 &
    watchdog_pid=$!
    wait "$cmd_pid" 2>/dev/null
    status=$?
    kill "$watchdog_pid" 2>/dev/null
    wait "$watchdog_pid" 2>/dev/null
    return $status
}

finish() {
    if [ "$FAILURES" -gt 0 ]; then
        echo "$FAILURES assertion(s) failed" 1>&2
        exit 1
    fi
    echo "OK"
    exit 0
}
