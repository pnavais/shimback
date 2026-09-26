# Writes a large, fast burst of stdout (comfortably more than any small
# test-configured --capture-limit) then fails -- PowerShell equivalent of
# tests/fixtures/fake_firehose.sh, exercising the size-based capture
# cutover in dispatch.c's run_captured.
$sw = [System.IO.StreamWriter]::new([Console]::OpenStandardOutput())
$sw.AutoFlush = $false
for ($i = 0; $i -lt 5000; $i++) {
    $sw.WriteLine("firehose-line-{0:D5}-padding-to-make-this-longer-than-it-needs-to-be" -f $i)
}
$sw.Flush()
exit 1
