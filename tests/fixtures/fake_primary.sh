#!/bin/sh
# Configurable fake "source" command for dispatch tests.
# FAKE_EXIT_CODE: exit status to return (default 0)
# FAKE_STDOUT / FAKE_STDERR: literal text to print to each stream
if [ -n "$FAKE_EXIT_CODE" ]; then
    code=$FAKE_EXIT_CODE
else
    code=0
fi
if [ -n "$FAKE_STDOUT" ]; then
    printf '%s' "$FAKE_STDOUT"
fi
if [ -n "$FAKE_STDERR" ]; then
    printf '%s' "$FAKE_STDERR" 1>&2
fi
exit "$code"
