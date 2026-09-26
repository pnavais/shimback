# Windows-only: PowerShell profile / cmd.exe AutoRun / HKCU\Environment\Path
# integration -- the "other genuinely new subsystem" Phase 8's own plan
# calls out by name (see windows-port.md Phase 5). None of this is
# sandboxable via env var overrides (shell.c resolves the real Windows
# "Documents" special folder and real per-user registry regardless -- a
# known, deliberate limitation, see windows-port.md Phase 5's addendum), so
# these specs only run for real when explicitly opted into via
# SHIMBACK_TEST_REAL_SHELL=1 (always set in CI, off by default locally) --
# and even then, always snapshot + restore the real state around themselves
# on top of Common.ps1's own blanket Enter/Exit-ShimbackSandbox protection,
# belt and braces given how easy this exact class of mistake turned out to
# be to make while writing this very suite (see windows-port.md).

Describe "shell integration" {
    BeforeAll {
        . "$PSScriptRoot\Common.ps1"
        $script:Sandbox = New-ShimbackSandbox
        Enter-ShimbackSandbox $Sandbox
        $script:PS = Get-PowerShellExe
    }

    AfterAll {
        Exit-ShimbackSandbox $Sandbox
        Remove-ShimbackSandbox $Sandbox
    }

    BeforeEach {
        if (-not (Test-RealShellIntegrationAllowed)) {
            Set-ItResult -Skipped -Because "real PowerShell profile/registry mutation only runs with `$env:SHIMBACK_TEST_REAL_SHELL=1 (set automatically in CI)"
        }
    }

    It "init writes an idempotent PowerShell profile block and cmd.exe AutoRun entry" {
        $snapshot = Backup-RealShellState
        try {
            $r1 = Invoke-Shimback @("init")
            $r1.ExitCode | Should -Be 0
            $r1.StdOut | Should -Match "Detected powershell"

            $winPsProfile = "$([Environment]::GetFolderPath('MyDocuments'))\WindowsPowerShell\profile.ps1"
            $content = Get-Content -Raw $winPsProfile
            $content | Should -Match ([regex]::Escape($Sandbox.DataHome))
            $markerCount = ([regex]::Matches($content, [regex]::Escape("# >>> shimback >>>"))).Count
            $markerCount | Should -Be 1

            $autoRun = (Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Command Processor' -Name AutoRun).AutoRun
            $autoRun | Should -Match ([regex]::Escape($Sandbox.DataHome))

            # Re-running init must not duplicate the block.
            Invoke-Shimback @("init") | Out-Null
            $content2 = Get-Content -Raw $winPsProfile
            $markerCount2 = ([regex]::Matches($content2, [regex]::Escape("# >>> shimback >>>"))).Count
            $markerCount2 | Should -Be 1
        }
        finally {
            Restore-RealShellState $snapshot
        }
    }

    It "install adds and uninstall removes its own HKCU\Environment\Path entry" {
        # NOT a byte-for-byte "PATH restored to exactly $before" check: the
        # PowerShell-profile PATH block (and the HKCU\Environment\Path
        # entries mirrored from it) is a single machine-wide, shared,
        # tagged section -- by design, one union of every directory
        # shimback has ever added on this machine, not one scoped to a
        # particular install (see shell.c's remove_powershell, which reads
        # *every* directory currently listed in the tagged block and
        # removes each one). `uninstall` therefore, correctly and by
        # design, clears the *entire* tagged section, including entries
        # this test never added itself -- confirmed directly the hard way
        # while writing this spec (see windows-port.md). Backup/Restore-
        # RealShellState around this test is what actually guarantees real,
        # pre-existing entries come back, not `uninstall` itself.
        $snapshot = Backup-RealShellState
        try {
            $prefix = Join-Path $Sandbox.Root "install-prefix"

            $r1 = Invoke-Shimback @("install", "--prefix", $prefix)
            $r1.ExitCode | Should -Be 0
            Test-Path (Join-Path $prefix "bin\shimback.exe") | Should -BeTrue
            # "$prefix/bin", not Join-Path's "$prefix\bin" -- install.c's
            # own path_join always uses '/' for the join point it adds,
            # same convention as shim_bin_dir() (see Common.ps1's own note
            # on this for the PATH-prepend simulation).
            $afterInstall = (Get-ItemProperty -Path 'HKCU:\Environment' -Name Path).Path
            $afterInstall.Contains("$prefix/bin") | Should -BeTrue

            $r2 = Invoke-Shimback @("uninstall", "--prefix", $prefix)
            $r2.ExitCode | Should -Be 0
            $afterUninstall = (Get-ItemProperty -Path 'HKCU:\Environment' -Name Path -ErrorAction SilentlyContinue).Path
            $afterUninstall.Contains("$prefix/bin") | Should -BeFalse
        }
        finally {
            Restore-RealShellState $snapshot
        }
    }
}
