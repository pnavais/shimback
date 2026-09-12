#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

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
assert_contains "init: reports fish as unsupported" "$out" "not supported"

assert_contains "init: zsh gets a PATH block" "$(cat "$HOME/.zshrc")" "# >>> shimback >>>"

if [ ! -f "$HOME/.bashrc" ]; then
    fail "init: expected ~/.bashrc to be created (none of the bash rc files existed)"
fi
assert_contains "init: bash gets a PATH block" "$(cat "$HOME/.bashrc")" "# >>> shimback >>>"

if [ -f "$HOME/.config/fish/config.fish" ]; then
    fail "init: fish config should not be touched in v0.1.0"
fi

# --- idempotent re-run: no duplicate blocks ---
"$SHIMBACK" init >/dev/null
zsh_count="$(count_occurrences '# >>> shimback >>>' "$HOME/.zshrc")"
bash_count="$(count_occurrences '# >>> shimback >>>' "$HOME/.bashrc")"
assert_eq "init: zsh marker stays singular on re-run" "1" "$zsh_count"
assert_eq "init: bash marker stays singular on re-run" "1" "$bash_count"

finish
