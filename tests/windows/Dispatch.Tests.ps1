# Windows Pester counterpart to tests/test_dispatch.sh -- mirrors its
# intent policy by policy (see windows-port.md Phase 8) rather than
# transliterating every assertion, plus two things with no POSIX
# equivalent: the .bat/.cmd CreateProcessW regression (see
# windows-port.md's own addendum) and capture-cutover timing widened for a
# spawned PowerShell process's much higher startup latency than a POSIX
# shell script's.

Describe "dispatch" {
    BeforeAll {
        . "$PSScriptRoot\Common.ps1"
        $script:Sandbox = New-ShimbackSandbox
        Enter-ShimbackSandbox $Sandbox
        $script:PrimaryArgs = Get-FixtureArgs "FakePrimary.ps1"
        $script:FallbackArgs = Get-FixtureArgs "FakeFallback.ps1"
        $script:EchoArgs = Get-FixtureArgs "FakeEcho.ps1"
        $script:PS = Get-PowerShellExe
    }

    AfterAll {
        Exit-ShimbackSandbox $Sandbox
        Remove-ShimbackSandbox $Sandbox
    }

    Context "exit-code (default policy)" {
        BeforeAll {
            Invoke-Shimback @("add", "ectool", "-s", $PS, "--source-arg", $PrimaryArgs[0],
                "--source-arg", $PrimaryArgs[1], "--source-arg", $PrimaryArgs[2],
                "--source-arg", $PrimaryArgs[3], "--source-arg", $PrimaryArgs[4],
                "-f", $PS, "--fallback-arg", $FallbackArgs[0], "--fallback-arg", $FallbackArgs[1],
                "--fallback-arg", $FallbackArgs[2], "--fallback-arg", $FallbackArgs[3],
                "--fallback-arg", $FallbackArgs[4]) | Out-Null
            $script:EcTool = Get-ShimPath "ectool" $Sandbox
        }

        It "passes through a successful source's exact stdout/stderr" {
            $r = Invoke-Exe $EcTool @() -EnvOverrides @{ FAKE_EXIT_CODE = "0"; FAKE_STDOUT = "hello-out"; FAKE_STDERR = "hello-err" }
            $r.ExitCode | Should -Be 0
            $r.StdOut | Should -Be "hello-out"
            $r.StdErr | Should -Be "hello-err"
        }

        It "falls back on failure, hiding the source's real output" {
            $r = Invoke-Exe $EcTool @("abc") -EnvOverrides @{ FAKE_EXIT_CODE = "1"; FAKE_STDOUT = "secret-out"; FAKE_STDERR = "secret-err" }
            $r.ExitCode | Should -Be 0
            $r.StdOut | Should -Match "FALLBACK_RAN:abc"
            $r.StdOut | Should -Not -Match "secret-out"
            $r.StdErr | Should -Not -Match "secret-err"
        }
    }

    Context "heuristic" {
        BeforeAll {
            $htoolArgs = @("add", "htool", "-s", $PS, "-f", $PS, "--policy", "heuristic",
                "--error-pattern", "illegal option")
            foreach ($a in $PrimaryArgs) { $htoolArgs += @("--source-arg", $a) }
            foreach ($a in $FallbackArgs) { $htoolArgs += @("--fallback-arg", $a) }
            Invoke-Shimback $htoolArgs | Out-Null
            $script:HTool = Get-ShimPath "htool" $Sandbox
        }

        It "falls back only when stderr matches the configured pattern" {
            $r = Invoke-Exe $HTool @("-x") -EnvOverrides @{ FAKE_EXIT_CODE = "1"; FAKE_STDERR = "fake_primary: Illegal option -- x" }
            $r.ExitCode | Should -Be 0
            $r.StdOut | Should -Match "FALLBACK_RAN:-x"
        }

        It "surfaces the real failure untouched when stderr doesn't match" {
            $r = Invoke-Exe $HTool @() -EnvOverrides @{ FAKE_EXIT_CODE = "3"; FAKE_STDOUT = "partial-out"; FAKE_STDERR = "totally different error" }
            $r.ExitCode | Should -Be 3
            $r.StdOut | Should -Be "partial-out"
            $r.StdOut | Should -Not -Match "FALLBACK_RAN"
        }
    }

    Context "exit-code-match" {
        BeforeAll {
            $xtoolArgs = @("add", "xtool", "-s", $PS, "-f", $PS, "--policy", "exit-code-match",
                "--exit-code", "42", "--exit-code", "43")
            foreach ($a in $PrimaryArgs) { $xtoolArgs += @("--source-arg", $a) }
            foreach ($a in $FallbackArgs) { $xtoolArgs += @("--fallback-arg", $a) }
            Invoke-Shimback $xtoolArgs | Out-Null
            $script:XTool = Get-ShimPath "xtool" $Sandbox
        }

        It "falls back only on a configured exit code" {
            $r = Invoke-Exe $XTool @("y") -EnvOverrides @{ FAKE_EXIT_CODE = "42"; FAKE_STDOUT = "secret-out" }
            $r.ExitCode | Should -Be 0
            $r.StdOut | Should -Match "FALLBACK_RAN:y"
        }

        It "surfaces an unconfigured exit code as-is" {
            $r = Invoke-Exe $XTool @() -EnvOverrides @{ FAKE_EXIT_CODE = "7"; FAKE_STDOUT = "partial-out" }
            $r.ExitCode | Should -Be 7
            $r.StdOut | Should -Be "partial-out"
        }

        It "add rejects exit-code-match with no --exit-code" {
            $r = Invoke-Shimback @("add", "badxtool", "-s", $PS, "-f", $PS, "--policy", "exit-code-match")
            $r.ExitCode | Should -Not -Be 0
        }
    }

    Context "route-args" {
        BeforeAll {
            $raArgs = @("add", "ratool", "-s", $PS, "-f", $PS, "--policy", "route-args", "--route-arg", "special")
            foreach ($a in $PrimaryArgs) { $raArgs += @("--source-arg", $a) }
            foreach ($a in $FallbackArgs) { $raArgs += @("--fallback-arg", $a) }
            Invoke-Shimback $raArgs | Out-Null
            $script:RaTool = Get-ShimPath "ratool" $Sandbox
        }

        It "runs source when no argument matches" {
            $r = Invoke-Exe $RaTool @("normal") -EnvOverrides @{ FAKE_STDOUT = "ran-source" }
            $r.StdOut | Should -Be "ran-source"
        }

        It "runs fallback directly (no trial run) when an argument matches" {
            $r = Invoke-Exe $RaTool @("special")
            $r.StdOut | Should -Match "FALLBACK_RAN:special"
        }
    }

    Context "rewrite" {
        BeforeAll {
            $rwArgs = @("add", "rwtool", "-s", $PS, "--policy", "rewrite", "--rewrite", "--full=-Alt")
            foreach ($a in $EchoArgs) { $rwArgs += @("--source-arg", $a) }
            Invoke-Shimback $rwArgs | Out-Null
            $script:RwTool = Get-ShimPath "rwtool" $Sandbox
        }

        It "rewrites a matching argument before running source, live, no fallback" {
            $r = Invoke-Exe $RwTool @("--full")
            $r.StdOut | Should -Match "ECHO_RAN:-Alt"
        }

        It "add rejects rewrite with no --rewrite rule" {
            $r = Invoke-Shimback @("add", "badrw", "-s", $PS, "--policy", "rewrite")
            $r.ExitCode | Should -Not -Be 0
        }
    }

    Context "route-map" {
        BeforeAll {
            # Routed to a plain, argument-less system exe (not the $PS
            # fixture pattern used elsewhere) -- a route's own baked-in args
            # aren't settable via --route itself (see README/man page), and
            # $PS with no args at all would launch an interactive shell and
            # hang the test run.
            $hostnameExe = "$env:WINDIR\System32\hostname.exe"
            $rmArgs = @("add", "rmtool", "-s", $PS, "--policy", "route-map",
                "--route", ("--v8=" + $hostnameExe), "--strip-matched-args")
            foreach ($a in $PrimaryArgs) { $rmArgs += @("--source-arg", $a) }
            Invoke-Shimback $rmArgs | Out-Null
            $script:RmTool = Get-ShimPath "rmtool" $Sandbox
        }

        It "runs source when no route matches" {
            $r = Invoke-Exe $RmTool @() -EnvOverrides @{ FAKE_STDOUT = "ran-source" }
            $r.StdOut | Should -Be "ran-source"
        }

        It "routes to the matched command, live, no fallback" {
            $r = Invoke-Exe $RmTool @("--v8")
            $r.StdOut.Trim() | Should -Be $env:COMPUTERNAME
        }
    }

    Context "passthrough" {
        BeforeAll {
            $ptArgs = @("add", "pttool", "-s", $PS, "-f", $PS, "--policy", "passthrough")
            foreach ($a in $PrimaryArgs) { $ptArgs += @("--source-arg", $a) }
            foreach ($a in $FallbackArgs) { $ptArgs += @("--fallback-arg", $a) }
            Invoke-Shimback $ptArgs | Out-Null
            $script:PtTool = Get-ShimPath "pttool" $Sandbox
        }

        It "passes through a successful source's output" {
            $r = Invoke-Exe $PtTool @() -EnvOverrides @{ FAKE_EXIT_CODE = "0"; FAKE_STDOUT = "hello-out" }
            $r.ExitCode | Should -Be 0
            $r.StdOut | Should -Be "hello-out"
        }

        It "falls back on failure WITHOUT hiding the source's own output" {
            $r = Invoke-Exe $PtTool @("abc") -EnvOverrides @{ FAKE_EXIT_CODE = "1"; FAKE_STDOUT = "visible-out" }
            $r.ExitCode | Should -Be 0
            $r.StdOut | Should -Match "visible-out"
            $r.StdOut | Should -Match "FALLBACK_RAN:abc"
        }

        It "add rejects passthrough combined with --diagnostic" {
            $cmdExe = "$env:WINDIR\System32\cmd.exe"
            $r = Invoke-Shimback @("add", "badptdiag", "-s", $PS, "-f", $cmdExe, "--policy", "passthrough", "--diagnostic")
            $r.ExitCode | Should -Not -Be 0
            $r.StdErr | Should -Match "never captures or hides anything"
        }

        It "add rejects passthrough combined with --capture-timeout" {
            $cmdExe = "$env:WINDIR\System32\cmd.exe"
            $r = Invoke-Shimback @("add", "badpttimeout", "-s", $PS, "-f", $cmdExe, "--policy", "passthrough", "--capture-timeout", "500")
            $r.ExitCode | Should -Not -Be 0
            $r.StdErr | Should -Match "capture timeout has nothing to apply to"
        }

        It "add rejects passthrough combined with --capture-limit" {
            $cmdExe = "$env:WINDIR\System32\cmd.exe"
            $r = Invoke-Shimback @("add", "badptlimit", "-s", $PS, "-f", $cmdExe, "--policy", "passthrough", "--capture-limit", "1KiB")
            $r.ExitCode | Should -Not -Be 0
            $r.StdErr | Should -Match "capture limit has nothing to apply to"
        }
    }

    Context "diagnostic" {
        It "prints a one-line note on stderr only when a fallback actually fires" {
            $diagArgs = @("add", "diagtool", "-s", $PS, "-f", $PS, "--diagnostic")
            foreach ($a in $PrimaryArgs) { $diagArgs += @("--source-arg", $a) }
            foreach ($a in $FallbackArgs) { $diagArgs += @("--fallback-arg", $a) }
            Invoke-Shimback $diagArgs | Out-Null
            $diagTool = Get-ShimPath "diagtool" $Sandbox

            $ok = Invoke-Exe $diagTool @() -EnvOverrides @{ FAKE_EXIT_CODE = "0" }
            $ok.StdErr | Should -Not -Match "shimback:"

            $fail = Invoke-Exe $diagTool @() -EnvOverrides @{ FAKE_EXIT_CODE = "1" }
            $fail.StdErr | Should -Match "failed; falling back to"
        }
    }

    Context "capture cutover" {
        It "gives up hiding a source producing more output than --capture-limit; nothing lost, no fallback" {
            $fhArgs = @("add", "capsize", "-s", $PS, "-f", $PS, "--capture-limit", "1KiB")
            foreach ($a in (Get-FixtureArgs "FakeFirehose.ps1")) { $fhArgs += @("--source-arg", $a) }
            foreach ($a in $FallbackArgs) { $fhArgs += @("--fallback-arg", $a) }
            Invoke-Shimback $fhArgs | Out-Null
            $capsize = Get-ShimPath "capsize" $Sandbox

            $r = Invoke-Exe $capsize @()
            $r.ExitCode | Should -Be 1
            ($r.StdOut -split "`n" | Where-Object { $_ -match "^firehose-line-" }).Count | Should -Be 5000
            $r.StdOut | Should -Not -Match "FALLBACK_RAN"
        }

        It "gives up hiding a source that runs longer than --capture-timeout; nothing lost, no fallback" {
            $slowArgs = @("add", "captime", "-s", $PS, "-f", $PS, "--capture-timeout", "400")
            foreach ($a in (Get-FixtureArgs "FakeSlow.ps1")) { $slowArgs += @("--source-arg", $a) }
            foreach ($a in $FallbackArgs) { $slowArgs += @("--fallback-arg", $a) }
            Invoke-Shimback $slowArgs | Out-Null
            $captime = Get-ShimPath "captime" $Sandbox

            $r = Invoke-Exe $captime @()
            $r.ExitCode | Should -Be 1
            $r.StdOut | Should -Match "slow-start"
            $r.StdOut | Should -Match "slow-end"
            $r.StdOut | Should -Not -Match "FALLBACK_RAN"
        }
    }

    Context ".bat/.cmd source and fallback (Windows-only regression)" {
        # CreateProcessW's implicit cmd.exe re-exec for a .bat/.cmd target
        # reads the *command line's first token* to decide what to run, not
        # lpApplicationName -- see windows-port.md's addendum under Phase 2
        # for the bug this guards against (every shim wrapping a .cmd tool,
        # e.g. npm.cmd, was silently broken before the fix).
        BeforeAll {
            $batDir = Join-Path $Sandbox.Root "batfixtures"
            New-Item -ItemType Directory -Force -Path $batDir | Out-Null
            $script:GoodBat = Join-Path $batDir "goodsrc.bat"
            $script:BadBat = Join-Path $batDir "badsrc.bat"
            $script:FbBat = Join-Path $batDir "fb.bat"
            Set-Content -Path $GoodBat -Value "@echo off`r`necho bat-source-ok %*`r`nexit /b 0`r`n"
            Set-Content -Path $BadBat -Value "@echo off`r`necho bat-source-fail %*`r`nexit /b 3`r`n"
            Set-Content -Path $FbBat -Value "@echo off`r`necho bat-fallback-ran %*`r`nexit /b 0`r`n"
        }

        It "runs a .bat source directly (not the shim's own invoked name)" {
            Invoke-Shimback @("add", "battool", "-s", $GoodBat, "-f", $FbBat) | Out-Null
            $battool = Get-ShimPath "battool" $Sandbox
            $r = Invoke-Exe $battool @("arg1", "arg2")
            $r.ExitCode | Should -Be 0
            $r.StdOut | Should -Match "bat-source-ok arg1 arg2"
        }

        It "falls back to a .bat fallback when a .bat source fails" {
            Invoke-Shimback @("add", "battool", "-s", $BadBat, "-f", $FbBat, "--force") | Out-Null
            $battool = Get-ShimPath "battool" $Sandbox
            $r = Invoke-Exe $battool @("x")
            $r.ExitCode | Should -Be 0
            $r.StdOut | Should -Match "bat-fallback-ran x"
        }

        It "passthrough also runs a .bat source/fallback correctly" {
            Invoke-Shimback @("add", "batpt", "-s", $BadBat, "-f", $FbBat, "--policy", "passthrough") | Out-Null
            $batpt = Get-ShimPath "batpt" $Sandbox
            $r = Invoke-Exe $batpt @("y")
            $r.ExitCode | Should -Be 0
            $r.StdOut | Should -Match "bat-source-fail y"
            $r.StdOut | Should -Match "bat-fallback-ran y"
        }
    }
}
