# Windows Pester counterpart to tests/test_list.sh.

Describe "list" {
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

    It "plain list shows the compact table with no policy-specific detail" {
        $addArgs = @("add", "htool", "-s", $PS, "-f", $PS, "--policy", "heuristic",
            "--error-pattern", "invalid option")
        foreach ($a in $PrimaryArgs) { $addArgs += @("--source-arg", $a) }
        foreach ($a in $FallbackArgs) { $addArgs += @("--fallback-arg", $a) }
        Invoke-Shimback $addArgs | Out-Null

        $r = Invoke-Shimback @("list")
        $r.StdOut | Should -Match "NAME"
        $r.StdOut | Should -Not -Match "error patterns:"
    }

    It "--full shows one policy-specific detail block per configured policy" {
        $xArgs = @("add", "xtool", "-s", $PS, "-f", $PS, "--policy", "exit-code-match", "--exit-code", "2", "--exit-code", "3")
        foreach ($a in $PrimaryArgs) { $xArgs += @("--source-arg", $a) }
        foreach ($a in $FallbackArgs) { $xArgs += @("--fallback-arg", $a) }
        Invoke-Shimback $xArgs | Out-Null

        $ptArgs = @("add", "pttool", "-s", $PS, "-f", $PS, "--policy", "passthrough")
        foreach ($a in $PrimaryArgs) { $ptArgs += @("--source-arg", $a) }
        foreach ($a in $FallbackArgs) { $ptArgs += @("--fallback-arg", $a) }
        Invoke-Shimback $ptArgs | Out-Null

        $full = (Invoke-Shimback @("list", "--full")).StdOut
        $full | Should -Match "exit codes: 2, 3"
        # passthrough has nothing policy-specific to add beyond the symlink
        # line every shim gets -- same as plain exit-code (see info.c).
    }
}
