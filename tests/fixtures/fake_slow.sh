#!/bin/sh
# Prints a marker, sleeps past any small test-configured --capture-timeout,
# prints another marker, then fails -- used to exercise the time-based
# capture cutover in dispatch.c's run_captured.
echo "slow-start"
sleep 0.4
echo "slow-end"
exit 1
