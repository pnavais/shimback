# Configurable fake "source" command for dispatch tests -- PowerShell
# equivalent of tests/fixtures/fake_primary.sh, same env-var contract:
#   FAKE_EXIT_CODE: exit status to return (default 0)
#   FAKE_STDOUT / FAKE_STDERR: literal text to print to each stream, with no
#   added newline (matching the POSIX fixture's `printf '%s'`, not `echo`),
#   so byte-for-byte string assertions behave the same on both platforms.
$code = if ($env:FAKE_EXIT_CODE) { [int]$env:FAKE_EXIT_CODE } else { 0 }
if ($env:FAKE_STDOUT) { [Console]::Out.Write($env:FAKE_STDOUT) }
if ($env:FAKE_STDERR) { [Console]::Error.Write($env:FAKE_STDERR) }
exit $code
