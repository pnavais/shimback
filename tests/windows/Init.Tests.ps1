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

            # Microsoft.PowerShell_profile.ps1 ($PROFILE.CurrentUserCurrentHost),
            # not the old profile.ps1 (CurrentUserAllHosts) -- see
            # shell.c's powershell_profile_path for why.
            $winPsProfile = "$([Environment]::GetFolderPath('MyDocuments'))\WindowsPowerShell\Microsoft.PowerShell_profile.ps1"
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

    It "install adds its own HKCU\Environment\Path entry; uninstall removes the binary" {
        # NOT a byte-for-byte "PATH restored to exactly $before" check: the
        # PowerShell-profile PATH block (and the HKCU\Environment\Path
        # entries mirrored from it) is a single machine-wide, shared,
        # tagged section -- by design, one union of every directory
        # shimback has ever added on this machine, not one scoped to a
        # particular install (see shell.c's remove_powershell, which reads
        # *every* directory currently listed in the tagged block and
        # removes each one). Backup/Restore-RealShellState around this test
        # is what actually guarantees real, pre-existing entries come back,
        # not `uninstall` itself.
        #
        # Also, uninstall deliberately does NOT always strip the shared
        # block on removal: if another real installation on this machine
        # still uses it (uninstall.c checks the PATH-recorded installation
        # list), it leaves the block in place rather than breaking that
        # other install -- confirmed for real on a dev machine that has its
        # own separate real install at ~/.local/bin. So this only asserts
        # what's actually guaranteed regardless of that: the binary itself
        # is gone, and the PATH entry is gone *unless* uninstall explicitly
        # said it kept the block for that reason.
        $snapshot = Backup-RealShellState
        try {
            $prefix = Join-Path $Sandbox.Root "install-prefix"

            $r1 = Invoke-Shimback @("install", "--prefix", $prefix)
            $r1.ExitCode | Should -Be 0
            $binPath = Join-Path $prefix "bin\shimback.exe"
            Test-Path $binPath | Should -BeTrue
            # "$prefix/bin", not Join-Path's "$prefix\bin" -- install.c's
            # own path_join always uses '/' for the join point it adds,
            # same convention as shim_bin_dir() (see Common.ps1's own note
            # on this for the PATH-prepend simulation).
            $afterInstall = (Get-ItemProperty -Path 'HKCU:\Environment' -Name Path).Path
            $afterInstall.Contains("$prefix/bin") | Should -BeTrue

            $r2 = Invoke-Shimback @("uninstall", "--prefix", $prefix)
            $r2.ExitCode | Should -Be 0
            Test-Path $binPath | Should -BeFalse

            $afterUninstall = (Get-ItemProperty -Path 'HKCU:\Environment' -Name Path -ErrorAction SilentlyContinue).Path
            $keptBlock = $r2.StdErr -match "left the PATH block in place"
            if (-not $keptBlock) {
                $afterUninstall.Contains("$prefix/bin") | Should -BeFalse
            }
        }
        finally {
            Restore-RealShellState $snapshot
        }
    }

    It "uninstall can remove itself even though Windows won't let a running process delete its own image" {
        # Real bug, reported by the user running `shimback uninstall` for
        # real on their own machine: `shimback uninstall` is normally
        # invoked as the exact binary it's trying to delete (e.g.
        # ~/.local/bin/shimback.exe uninstall), and Windows refuses to
        # delete a running process's own image file (DeleteFileW/unlink()
        # -> ERROR_ACCESS_DENIED) -- verified for real via a standalone
        # compiled test program. uninstall.c's orphan_running_binary works
        # around this with a rename (which Windows *does* allow on a
        # running file), freeing the original path. Invoke-Shimback always
        # runs the *test harness's* shimback.exe (see Get-ShimbackExe), not
        # whatever's installed at --prefix, so this specifically invokes
        # the *installed* copy against itself via Invoke-Exe to actually
        # exercise the failure -- the earlier "uninstall removes the
        # binary" test above never does, since it always deletes a file
        # that isn't the running process's own.
        $snapshot = Backup-RealShellState
        try {
            $prefix = Join-Path $Sandbox.Root "install-prefix-selfdelete"
            $r1 = Invoke-Shimback @("install", "--prefix", $prefix)
            $r1.ExitCode | Should -Be 0
            $binPath = Join-Path $prefix "bin\shimback.exe"
            Test-Path $binPath | Should -BeTrue

            $r2 = Invoke-Exe -Path $binPath -ExeArgs @("uninstall", "--prefix", $prefix)
            $r2.ExitCode | Should -Be 0
            Test-Path $binPath | Should -BeFalse
            Test-Path "$binPath.old" | Should -BeTrue
            $r2.StdOut | Should -Match "renamed it to"

            # The rename frees the original path, so the "any other
            # installation left?" scan further down correctly sees none --
            # unless a genuinely separate installation exists elsewhere on
            # this machine (e.g. a dev box's own real ~/.local/bin install,
            # same caveat as the test above), in which case the message is
            # fine as long as it isn't the original bug: misattributing
            # *this* just-freed path as if it were that other installation.
            if ($r2.StdErr -match "left the PATH block in place") {
                $r2.StdErr | Should -Not -Match ([regex]::Escape($binPath))
            }
        }
        finally {
            Restore-RealShellState $snapshot
        }
    }
}

# A separate, ungated Describe: unlike the real-mutation specs above, this
# doesn't need SHIMBACK_TEST_REAL_SHELL -- generating the block still
# touches the real profile file (Enter-ShimbackSandbox can't redirect that,
# same reason as above), but Enter/Exit-ShimbackSandbox's own automatic
# Backup/Restore-RealShellState already protects it, the same way every
# other Describe in this suite that happens to call add/install is already
# protected, so there's no need to skip it locally too.
Describe "PowerShell PATH idempotency" {
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

    It "generates a guarded prepend that doesn't duplicate an already-present directory" {
        # Real bug, found by the user asking a clarifying question about
        # how PowerShell actually gets its PATH: a brand-new shell already
        # inherits the shim directory from HKCU\Environment\Path (or from
        # whatever process launched it) *before* $PROFILE ever runs, so an
        # unconditional `$env:Path = '<dir>' + ';' + $env:Path` -- the form
        # used through v0.1.0's own first re-release -- always added a
        # second, redundant copy every single session (a third with a
        # second PowerShell edition also touched, more with every nested
        # shell-launching-shell). Harmless -- the shim directory still won
        # the front of PATH either way -- but real, verified PATH-list
        # pollution. This exercises the fix directly against the real
        # generated PowerShell code, not just shimback's own file-content
        # bookkeeping.
        $a = @("add", "idemtool", "-s", $PS, "-f", $PS)
        foreach ($x in $PrimaryArgs) { $a += @("--source-arg", $x) }
        foreach ($x in $FallbackArgs) { $a += @("--fallback-arg", $x) }
        Invoke-Shimback $a | Out-Null

        $docs = [Environment]::GetFolderPath('MyDocuments')
        $profilePath = "$docs\PowerShell\Microsoft.PowerShell_profile.ps1"
        $content = Get-Content -Raw $profilePath
        $content | Should -Match "-notcontains"

        $blockStart = $content.IndexOf("# >>> shimback >>>")
        $blockEnd = $content.IndexOf("# <<< shimback <<<") + "# <<< shimback <<<".Length
        $blockCode = $content.Substring($blockStart, $blockEnd - $blockStart)
        $shimDir = "$($Sandbox.DataHome)/shimback/bin"

        # Directory already present before the block runs -> must not be duplicated.
        $already = & $PS -NoProfile -Command "`$env:Path = '$shimDir;C:\Windows'`n$blockCode`n(`$env:Path -split ';' | Where-Object { `$_ -eq '$shimDir' }).Count"
        $already | Should -Be 1

        # Directory not present yet -> still gets added (exactly once).
        $missing = & $PS -NoProfile -Command "`$env:Path = 'C:\Windows'`n$blockCode`n(`$env:Path -split ';' | Where-Object { `$_ -eq '$shimDir' }).Count"
        $missing | Should -Be 1
    }

    It "upgrades an old-format block (unconditional prepend) on the next add, preserving its directories" {
        $docs = [Environment]::GetFolderPath('MyDocuments')
        $profilePath = "$docs\PowerShell\Microsoft.PowerShell_profile.ps1"
        New-Item -ItemType Directory -Force -Path (Split-Path $profilePath) | Out-Null
        Set-Content -Path $profilePath -Value "# >>> shimback >>>`n`$env:Path = 'C:\OldFormatDir\bin' + ';' + `$env:Path`n# <<< shimback <<<`n"

        $a = @("add", "migtool", "-s", $PS, "-f", $PS)
        foreach ($x in $PrimaryArgs) { $a += @("--source-arg", $x) }
        foreach ($x in $FallbackArgs) { $a += @("--fallback-arg", $x) }
        $r = Invoke-Shimback $a
        $r.ExitCode | Should -Be 0

        $content = Get-Content -Raw $profilePath
        $content | Should -Match "-notcontains"
        $content.Contains("C:\OldFormatDir\bin") | Should -BeTrue
        ([regex]::Matches($content, [regex]::Escape("# >>> shimback >>>"))).Count | Should -Be 1
    }
}
