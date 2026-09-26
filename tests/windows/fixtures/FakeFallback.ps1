# Configurable fake "fallback" command for dispatch tests -- PowerShell
# equivalent of tests/fixtures/fake_fallback.sh. Always prints a recognizable
# marker so tests can confirm it (and not the source) ran.
$code = if ($env:FAKE_FALLBACK_EXIT_CODE) { [int]$env:FAKE_FALLBACK_EXIT_CODE } else { 0 }
Write-Output ("FALLBACK_RAN:" + ($args -join " "))
exit $code
