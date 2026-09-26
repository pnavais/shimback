# Prints a marker, sleeps past any small test-configured --capture-timeout,
# prints another marker, then fails -- PowerShell equivalent of
# tests/fixtures/fake_slow.sh, exercising the time-based capture cutover in
# dispatch.c's run_captured. The sleep is longer than the POSIX fixture's
# (1.5s vs 0.4s) to leave comfortable margin over a spawned PowerShell
# process's own startup latency, which is much higher than a POSIX shell
# script's.
Write-Output "slow-start"
[Console]::Out.Flush()
Start-Sleep -Milliseconds 1500
Write-Output "slow-end"
exit 1
