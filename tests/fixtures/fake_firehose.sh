#!/bin/sh
# Writes a large, fast burst of stdout (comfortably more than any small
# test-configured --capture-limit) then fails -- used to exercise the
# size-based capture cutover in dispatch.c's run_captured.
i=0
while [ "$i" -lt 5000 ]; do
    printf 'firehose-line-%05d-padding-to-make-this-longer-than-it-needs-to-be\n' "$i"
    i=$((i + 1))
done
exit 1
