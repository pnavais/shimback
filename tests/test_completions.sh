#!/bin/sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/test_common.sh" "$1"

# --- shimback completions <shell>: all four print a non-empty script and exit 0 ---
for shell in bash zsh fish powershell; do
    out="$("$SHIMBACK" completions "$shell")"
    code=$?
    assert_eq "completions $shell: exits 0" "0" "$code"
    if [ -z "$out" ]; then
        fail "completions $shell: expected non-empty output"
    fi
done

# --- an unrecognized shell name fails, non-zero exit, no output on stdout ---
out="$("$SHIMBACK" completions nushell 2>/dev/null)"
code=$?
if [ "$code" -eq 0 ]; then
    fail "completions nushell: should have failed"
fi
if [ -n "$out" ]; then
    fail "completions nushell: should not have printed anything to stdout"
fi

# --- completions with no shell argument, or an extra one, both fail ---
"$SHIMBACK" completions >/dev/null 2>&1
if [ $? -eq 0 ]; then
    fail "completions (no argument): should have failed"
fi
"$SHIMBACK" completions bash extra >/dev/null 2>&1
if [ $? -eq 0 ]; then
    fail "completions bash extra: should have failed"
fi

# --- __complete-names: no shims configured -> prints nothing, exits 0 ---
out="$("$SHIMBACK" __complete-names)"
code=$?
assert_eq "__complete-names (empty): exits 0" "0" "$code"
assert_eq "__complete-names (empty): prints nothing" "" "$out"

# --- __complete-names: a mix of config.toml- and split-config-backed shims,
# each name printed exactly once, one per line ---
"$SHIMBACK" add ctool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" >/dev/null
"$SHIMBACK" add stool -s "$FAKE_PRIMARY" -f "$FAKE_FALLBACK" --split-config >/dev/null

out="$("$SHIMBACK" __complete-names)"
code=$?
assert_eq "__complete-names: exits 0" "0" "$code"
assert_contains "__complete-names: lists the config.toml-backed shim" "$out" "ctool"
assert_contains "__complete-names: lists the split-config-backed shim" "$out" "stool"
line_count="$(printf '%s\n' "$out" | grep -c .)"
assert_eq "__complete-names: exactly 2 names, one per line" "2" "$line_count"

# --- hidden command: __complete-names never appears in --help, and is
# never offered as a "did you mean" suggestion for a typo'd command ---
help_out="$("$SHIMBACK" --help)"
assert_not_contains "--help: __complete-names is not listed" "$help_out" "__complete-names"

typo_out="$("$SHIMBACK" __complete-name 2>&1)"
assert_not_contains "typo of the hidden command: not suggested as a correction" \
    "$typo_out" "__complete-names"

# --- __complete-names degrades gracefully (no die(), still exit 0) on a
# config.toml that fails to parse ---
CFG="$(config_file)"
printf 'this is not valid toml {{{\n' >"$CFG"
out="$("$SHIMBACK" __complete-names)"
code=$?
assert_eq "__complete-names (malformed config): exits 0, not an error" "0" "$code"
assert_eq "__complete-names (malformed config): prints nothing" "" "$out"

# --- syntax validation, one shell/tool at a time, skipped (not failed)
# when that specific checker isn't installed -- most of this test's real
# value is the behavioral assertions above, which need none of these ---
if command -v bash >/dev/null 2>&1; then
    if "$SHIMBACK" completions bash | bash -n 2>"$SANDBOX/bash_syntax_err"; then
        :
    else
        fail "completions bash: bash -n reported a syntax error: $(cat "$SANDBOX/bash_syntax_err")"
    fi
    rm -f "$SANDBOX/bash_syntax_err"
else
    echo "bash not found -- skipping its syntax check" 1>&2
fi

if command -v shellcheck >/dev/null 2>&1; then
    if ! "$SHIMBACK" completions bash | shellcheck -s bash - >"$SANDBOX/shellcheck_out" 2>&1; then
        fail "completions bash: shellcheck reported an issue: $(cat "$SANDBOX/shellcheck_out")"
    fi
    rm -f "$SANDBOX/shellcheck_out"
else
    echo "shellcheck not found -- skipping the bash shellcheck pass" 1>&2
fi

if command -v zsh >/dev/null 2>&1; then
    if "$SHIMBACK" completions zsh | zsh -n 2>"$SANDBOX/zsh_syntax_err"; then
        :
    else
        fail "completions zsh: zsh -n reported a syntax error: $(cat "$SANDBOX/zsh_syntax_err")"
    fi
    rm -f "$SANDBOX/zsh_syntax_err"
else
    echo "zsh not found -- skipping its syntax check" 1>&2
fi

if command -v fish >/dev/null 2>&1; then
    fish_script="$SANDBOX/shimback_completion.fish"
    "$SHIMBACK" completions fish >"$fish_script"
    if ! fish --no-execute "$fish_script" >"$SANDBOX/fish_syntax_err" 2>&1; then
        fail "completions fish: fish --no-execute reported a syntax error: $(cat "$SANDBOX/fish_syntax_err")"
    fi
    rm -f "$SANDBOX/fish_syntax_err"
else
    echo "fish not found -- skipping its syntax check" 1>&2
fi

if command -v pwsh >/dev/null 2>&1; then
    ps_script="$SANDBOX/shimback_completion.ps1"
    "$SHIMBACK" completions powershell >"$ps_script"
    parse_out="$(pwsh -NoProfile -Command "
        \$parseErrors = \$null
        [System.Management.Automation.Language.Parser]::ParseFile('$ps_script', [ref]\$null, [ref]\$parseErrors) | Out-Null
        if (\$parseErrors.Count -gt 0) { \$parseErrors | ForEach-Object { Write-Output \$_.Message } }
    " 2>&1)"
    if [ -n "$parse_out" ]; then
        fail "completions powershell: parser reported an issue: $parse_out"
    fi
else
    echo "pwsh not found -- skipping its syntax check" 1>&2
fi

finish
