#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

# Captured before the fake zsh/bash below get prepended onto PATH -- needed
# later to actually source a generated rc file for real, not through a
# no-op stand-in.
REAL_ZSH="$(command -v zsh || true)"
REAL_BASH="$(command -v bash || true)"

# Fake zsh/bash/fish binaries so `init` reliably detects all three
# regardless of what's actually installed on the machine running the test
# (fish in particular is often absent).
FAKE_SHELLS_DIR="$SANDBOX/fake_shells"
mkdir -p "$FAKE_SHELLS_DIR"
for name in zsh bash fish; do
    printf '#!/bin/sh\nexit 0\n' >"$FAKE_SHELLS_DIR/$name"
    chmod +x "$FAKE_SHELLS_DIR/$name"
done
export PATH="$FAKE_SHELLS_DIR:$PATH"

out="$("$SHIMBACK" init)"
assert_contains "init: detects zsh" "$out" "Detected zsh"
assert_contains "init: detects bash" "$out" "Detected bash"
assert_contains "init: detects fish" "$out" "Detected fish"

assert_contains "init: zsh gets a PATH block" "$(cat "$HOME/.zshrc")" "# >>> shimback >>>"

if [ ! -f "$HOME/.bashrc" ]; then
    fail "init: expected ~/.bashrc to be created (none of the bash rc files existed)"
fi
assert_contains "init: bash gets a PATH block" "$(cat "$HOME/.bashrc")" "# >>> shimback >>>"

FISH_SNIPPET="$HOME/.config/fish/conf.d/shimback.fish"
if [ ! -f "$FISH_SNIPPET" ]; then
    fail "init: expected a fish conf.d snippet to be created"
fi
assert_contains "init: fish snippet sets PATH" "$(cat "$FISH_SNIPPET")" "set -gx PATH"
assert_contains "init: fish snippet includes the shim dir" "$(cat "$FISH_SNIPPET")" \
    "$XDG_DATA_HOME/shimback/bin"

# --- idempotent re-run: no duplicate blocks ---
"$SHIMBACK" init >/dev/null
zsh_count="$(count_occurrences '# >>> shimback >>>' "$HOME/.zshrc")"
bash_count="$(count_occurrences '# >>> shimback >>>' "$HOME/.bashrc")"
fish_dir_count="$(count_occurrences "$XDG_DATA_HOME/shimback/bin" "$FISH_SNIPPET")"
assert_eq "init: zsh marker stays singular on re-run" "1" "$zsh_count"
assert_eq "init: bash marker stays singular on re-run" "1" "$bash_count"
assert_eq "init: fish snippet's shim dir stays singular on re-run" "1" "$fish_dir_count"

# --- real zsh, with a stand-in zsh-defer that queues then drains in order (as
# the real one does once the rc file has finished): another tool's deferred
# PATH activation queued *before* our block must survive it. The queue-time
# expansion form clobbered it, since it froze "$PATH" before that entry ran ---
if [ -n "$REAL_ZSH" ]; then
    FAKE_DEFER='
_q=()
zsh-defer() {
  if [[ $1 == -c ]]; then _q+=("$2"); else _q+=("${(j: :)${(@q)@}}"); fi
}
_drain() { local c; for c in "${_q[@]}"; do eval "$c"; done }
'
    final_path="$("$REAL_ZSH" -f -c "$FAKE_DEFER
PATH=/usr/bin:/bin
zsh-defer -c 'export PATH=\"/other/tool/bin:\$PATH\"'
source '$HOME/.zshrc'
_drain
print -r -- \$PATH" 2>/dev/null)"
    assert_contains "init: a deferred entry queued earlier survives our deferred export" \
        "$final_path" "/other/tool/bin"
    case "$final_path" in
        "$XDG_DATA_HOME/shimback/bin:"*) ;;
        *) fail "init: our shim dir should end up first on PATH, got [$final_path]" ;;
    esac
fi

# --- shell-injection regression: a data-home directory containing shell
# metacharacters must be safely quoted in every generated rc file/snippet,
# and sourcing it for real must never execute the injected command ---
OLD_DATA_HOME="$XDG_DATA_HOME"
MARKER="$SANDBOX/PWNED"
export XDG_DATA_HOME="$SANDBOX/inj\"; touch $MARKER; #"

"$SHIMBACK" init >/dev/null

QUOTED_DIR="'$SANDBOX/inj\"; touch $MARKER; #/shimback/bin'"
assert_contains "init: injected dir is single-quoted in .zshrc" "$(cat "$HOME/.zshrc")" \
    "$QUOTED_DIR"
assert_contains "init: injected dir is single-quoted in .bashrc" "$(cat "$HOME/.bashrc")" \
    "$QUOTED_DIR"
assert_contains "init: injected dir is single-quoted in the fish snippet" \
    "$(cat "$HOME/.config/fish/conf.d/shimback.fish")" "$QUOTED_DIR"

if [ -n "$REAL_ZSH" ]; then
    "$REAL_ZSH" -c "source '$HOME/.zshrc'" >/dev/null 2>&1
    if [ -e "$MARKER" ]; then
        fail "init: sourcing the generated .zshrc executed injected shell code"
    fi
    # ...and the deferred branch, whose command string gets eval'd, must be
    # just as inert (a stand-in zsh-defer that evals -c strings immediately)
    "$REAL_ZSH" -f -c 'zsh-defer() { [[ $1 == -c ]] && eval "$2"; }; source "$HOME/.zshrc"' \
        >/dev/null 2>&1
    if [ -e "$MARKER" ]; then
        fail "init: the zsh-defer -c branch executed injected shell code"
    fi
fi
if [ -n "$REAL_BASH" ]; then
    "$REAL_BASH" -c "source '$HOME/.bashrc'" >/dev/null 2>&1
    if [ -e "$MARKER" ]; then
        fail "init: sourcing the generated .bashrc executed injected shell code"
    fi
fi

export XDG_DATA_HOME="$OLD_DATA_HOME"

finish
