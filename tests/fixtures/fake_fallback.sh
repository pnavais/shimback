#!/bin/sh
# Configurable fake "fallback" command for dispatch tests. Always prints a
# recognizable marker so tests can confirm it (and not the source) ran.
if [ -n "$FAKE_FALLBACK_EXIT_CODE" ]; then
    code=$FAKE_FALLBACK_EXIT_CODE
else
    code=0
fi
echo "FALLBACK_RAN:$*"
exit "$code"
