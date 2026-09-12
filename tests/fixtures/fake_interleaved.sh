#!/bin/sh
# Writes >64KB of interleaved output to both stdout and stderr -- large
# enough to exceed a typical pipe buffer on both streams at once, so a naive
# "read stdout fully, then read stderr fully" capture implementation would
# deadlock (the child blocks writing to whichever pipe fills up first while
# nothing is draining it). Exits 0.
i=0
while [ "$i" -lt 2000 ]; do
    printf 'O%040d\n' "$i"
    printf 'E%040d\n' "$i" 1>&2
    i=$((i + 1))
done
exit 0
