#!/bin/sh
# Generic fixture "editor" for test_edit.sh: appends a comment line (so the
# result stays valid, parseable TOML) identifying which name it was invoked
# as, plus every argument it received, to the last argument (the file being
# "edited"). Lets tests verify $EDITOR / the nvim-vim-vi-nano-pico fallback
# chain actually ran the expected command with the expected arguments,
# without needing a real interactive editor.
last=""
for a in "$@"; do
    last="$a"
done
# Parameter expansion, not `basename` -- this fixture is deliberately run
# with $PATH restricted to just the fake editors under test, so an external
# `basename` command might not be resolvable at all.
name="${0##*/}"
echo "# EDITED_BY:$name ARGS:$*" >> "$last"
