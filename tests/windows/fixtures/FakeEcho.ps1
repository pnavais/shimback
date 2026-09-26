# Generic fixture that echoes its own invocation arguments -- PowerShell
# equivalent of tests/fixtures/fake_echo.sh, for tests that need to see
# exactly what argv a shim forwarded regardless of source/fallback role.
Write-Output ("ECHO_RAN:" + ($args -join " "))
$code = if ($env:FAKE_EXIT_CODE) { [int]$env:FAKE_EXIT_CODE } else { 0 }
exit $code
