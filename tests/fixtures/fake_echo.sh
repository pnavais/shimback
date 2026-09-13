#!/bin/sh
# Generic fixture that echoes its own invocation arguments -- for tests that
# need to see exactly what argv a shim forwarded regardless of whether this
# script is playing the source or fallback role (unlike fake_fallback.sh,
# whose "FALLBACK_RAN" marker implies a specific role). Exits with
# $FAKE_EXIT_CODE if set (default 0).
echo "ECHO_RAN:$*"
if [ -n "$FAKE_EXIT_CODE" ]; then
    exit "$FAKE_EXIT_CODE"
fi
exit 0
