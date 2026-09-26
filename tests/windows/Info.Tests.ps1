# Windows Pester counterpart to tests/test_info.sh (leaner: covers the
# banner/sections contract and the passthrough policy's own info.c output,
# added this session -- not a line-for-line port of every policy's diagram
# text, which the bash suite already covers identically on the shared
# info.c code).

Describe "info" {
    BeforeAll {
        . "$PSScriptRoot\Common.ps1"
        $script:Sandbox = New-ShimbackSandbox
        Enter-ShimbackSandbox $Sandbox
        $script:PS = Get-PowerShellExe
        $script:PrimaryArgs = Get-FixtureArgs "FakePrimary.ps1"
        $script:FallbackArgs = Get-FixtureArgs "FakeFallback.ps1"
    }

    AfterAll {
        Exit-ShimbackSandbox $Sandbox
        Remove-ShimbackSandbox $Sandbox
    }

    It "shows every section for a default exit-code shim, exits 0" {
        $a = @("add", "infotool", "-s", $PS, "-f", $PS, "--diagnostic")
        foreach ($x in $PrimaryArgs) { $a += @("--source-arg", $x) }
        foreach ($x in $FallbackArgs) { $a += @("--fallback-arg", $x) }
        Invoke-Shimback $a | Out-Null

        $r = Invoke-Shimback @("info", "infotool")
        $r.ExitCode | Should -Be 0
        $r.StdOut | Should -Match "policy"
        $r.StdOut | Should -Match "exit-code"
        $r.StdOut | Should -Match "Runs the source; falls back if it exits non-zero"
        $r.StdOut | Should -Match "on \(prints a note to stderr"
        $r.StdOut | Should -Match "2000 ms \(default\)"
        $r.StdOut | Should -Match "8 MiB \(default\)"
        $r.StdOut | Should -Match "typing 'infotool' runs this shim"
        $r.StdOut | Should -Match "no problems noticed"
    }

    It "shows the passthrough policy's own summary and rejects diagnostic in its output" {
        $a = @("add", "pttool", "-s", $PS, "-f", $PS, "--policy", "passthrough")
        foreach ($x in $PrimaryArgs) { $a += @("--source-arg", $x) }
        foreach ($x in $FallbackArgs) { $a += @("--fallback-arg", $x) }
        Invoke-Shimback $a | Out-Null

        $r = Invoke-Shimback @("info", "pttool")
        $r.ExitCode | Should -Be 0
        $r.StdOut | Should -Match "passthrough"
        $r.StdOut | Should -Match "no hidden trial run"
        $r.StdOut | Should -Match "not applicable -- this policy never hides anything"
        $r.StdOut | Should -Match "this policy runs its target directly, with live output"
    }

    It "an unknown shim name fails with a clear error" {
        # NOTE: README/man page both claim "info" gives a typo suggestion
        # like remove/doctor do, but cmd_info() never calls the suggestion
        # helper at all (confirmed by reading info.c) -- a pre-existing
        # doc/behavior mismatch found while writing this suite, unrelated
        # to the passthrough work, not fixed here. This asserts today's
        # real behavior so a future fix has to consciously update this
        # test rather than silently regress further.
        $r = Invoke-Shimback @("info", "totally-unconfigured-xyz")
        $r.ExitCode | Should -Not -Be 0
        $r.StdErr | Should -Match "no shim configured for 'totally-unconfigured-xyz'"
    }
}
