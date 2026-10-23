# Windows Pester counterpart to tests/test_edit.sh, plus what only Windows
# has: Windows command-line quoting in $EDITOR, PATHEXT lookup (a .cmd
# editor beside an extension-less decoy), and edit.exe first in the
# fallback order.

Describe "edit" {
    BeforeAll {
        . "$PSScriptRoot\Common.ps1"
        $script:Sandbox = New-ShimbackSandbox
        Enter-ShimbackSandbox $Sandbox
        $script:Cfg = Get-ConfigFile $Sandbox
        $script:FakeBin = Join-Path $Sandbox.Root "fake bin"
        New-Item -ItemType Directory -Force -Path $FakeBin | Out-Null
        $script:EmptyBin = Join-Path $Sandbox.Root "emptybin"
        New-Item -ItemType Directory -Force -Path $EmptyBin | Out-Null

        # A fake editor: appends "# EDITED_BY:<name> ARGS:<args>" to the file
        # it's given (its last argument) -- a TOML comment, so the config
        # stays parseable. cmd built-ins only, so a restricted PATH is fine.
        function New-FakeEditor([string]$Path, [string]$Name) {
            $lines = @(
                '@echo off',
                'setlocal',
                'set "all="',
                ':loop',
                'if "%~1"=="" goto done',
                'set "last=%~1"',
                'set "all=%all% %~1"',
                'shift',
                'goto loop',
                ':done',
                ">>""%last%"" echo # EDITED_BY:$Name ARGS:%all%"
            )
            Set-Content -Path $Path -Value $lines -Encoding ascii
        }

        function Invoke-Edit([string[]]$EditArgs = @(), [hashtable]$Env = @{}) {
            Invoke-Shimback (@("edit") + $EditArgs) -EnvOverrides $Env
        }

        function Reset-Config {
            Remove-Item -Force $Cfg -ErrorAction SilentlyContinue
        }
    }

    AfterAll {
        Exit-ShimbackSandbox $Sandbox
        Remove-ShimbackSandbox $Sandbox
    }

    AfterEach {
        Reset-Config
        Get-ChildItem $FakeBin | Remove-Item -Force
    }

    It "runs a quoted `$EDITOR path with spaces, passing its flags and a backslash-only config path" {
        $editor = Join-Path $FakeBin "my editor.cmd"
        New-FakeEditor $editor "spaced"
        $r = Invoke-Edit -Env @{ EDITOR = "`"$editor`" --some-flag" }
        $r.ExitCode | Should -Be 0
        $content = Get-Content -Raw $Cfg
        $content | Should -Match "EDITED_BY:spaced"
        $content | Should -Match "--some-flag"
        $content | Should -Match ([regex]::Escape(($Cfg -replace '/', '\')))
        $content | Should -Not -Match ([regex]::Escape("/shimback/config.toml"))
    }

    It "resolves a bare `$EDITOR through PATHEXT to the .cmd, not an extension-less file beside it" {
        New-FakeEditor (Join-Path $FakeBin "myed.cmd") "myed-cmd"
        Set-Content -Path (Join-Path $FakeBin "myed") -Value "#!/bin/sh`necho not a windows program"
        $r = Invoke-Edit -Env @{ EDITOR = "myed"; PATH = $FakeBin }
        $r.ExitCode | Should -Be 0
        Get-Content -Raw $Cfg | Should -Match "EDITED_BY:myed-cmd"
    }

    It "falls back to edit first, then nvim, when `$EDITOR is unset or blank" {
        New-FakeEditor (Join-Path $FakeBin "edit.cmd") "edit"
        New-FakeEditor (Join-Path $FakeBin "nvim.cmd") "nvim"
        $r = Invoke-Edit -Env @{ EDITOR = "   "; PATH = $FakeBin }
        $r.ExitCode | Should -Be 0
        Get-Content -Raw $Cfg | Should -Match "EDITED_BY:edit"

        Reset-Config
        Remove-Item (Join-Path $FakeBin "edit.cmd")
        $r = Invoke-Edit -Env @{ EDITOR = ""; PATH = $FakeBin }
        $r.ExitCode | Should -Be 0
        Get-Content -Raw $Cfg | Should -Match "EDITED_BY:nvim"
    }

    It "fails clearly when no editor is found, without creating the config" {
        $r = Invoke-Edit -Env @{ EDITOR = ""; PATH = $EmptyBin }
        $r.ExitCode | Should -Not -Be 0
        $r.StdErr | Should -Match "no editor found"
        $r.StdErr | Should -Match "edit, nvim, vim, vi, nano"
        Test-Path $Cfg | Should -BeFalse
    }

    It "reports an `$EDITOR command that doesn't exist" {
        $r = Invoke-Edit -Env @{ EDITOR = "no-such-editor --wait"; PATH = $EmptyBin }
        $r.ExitCode | Should -Not -Be 0
        $r.StdErr | Should -Match "'no-such-editor' not found or not executable"
    }

    It "refuses cmd.exe metacharacters for a .cmd editor instead of letting cmd.exe run them" {
        New-FakeEditor (Join-Path $FakeBin "myed.cmd") "myed"
        $marker = Join-Path $Sandbox.Root "pwned.txt"
        $path = "$FakeBin;$env:SystemRoot\System32"
        $r = Invoke-Edit -Env @{ EDITOR = "myed x&echo pwned>`"$marker`""; PATH = $path }
        $r.ExitCode | Should -Not -Be 0
        $r.StdErr | Should -Match "refusing to pass"
        Test-Path $marker | Should -BeFalse
        Test-Path $Cfg | Should -BeFalse
    }

    It "ignores leading whitespace in `$EDITOR" {
        New-FakeEditor (Join-Path $FakeBin "myed.cmd") "myed"
        $r = Invoke-Edit -Env @{ EDITOR = "   myed --flag"; PATH = $FakeBin }
        $r.ExitCode | Should -Be 0
        Get-Content -Raw $Cfg | Should -Match "EDITED_BY:myed"
    }

    It "warns when the edited config no longer parses, but still exits with the editor's code" {
        $bad = Join-Path $FakeBin "bad.cmd"
        Set-Content -Path $bad -Value @('@echo off', '>"%~1" echo not valid toml {{{') -Encoding ascii
        $r = Invoke-Edit -Env @{ EDITOR = "`"$bad`"" }
        $r.ExitCode | Should -Be 0
        $r.StdErr | Should -Match "now fails to parse"
    }

    It "opens a named shim's config file" {
        $editor = Join-Path $FakeBin "named.cmd"
        New-FakeEditor $editor "named"
        $ps = Get-PowerShellExe
        $a = @("add", "edtool", "-s", $ps, "-f", $ps)
        foreach ($x in (Get-FixtureArgs "FakePrimary.ps1")) { $a += @("--source-arg", $x) }
        foreach ($x in (Get-FixtureArgs "FakeFallback.ps1")) { $a += @("--fallback-arg", $x) }
        Invoke-Shimback $a | Out-Null
        $r = Invoke-Edit -EditArgs @("edtool") -Env @{ EDITOR = "`"$editor`"" }
        $r.ExitCode | Should -Be 0
        Get-Content -Raw $Cfg | Should -Match "EDITED_BY:named"
    }
}
