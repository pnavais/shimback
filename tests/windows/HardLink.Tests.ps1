# Windows-only: verifies `add` creates a real hard link to the running
# shimback binary (same file, same volume), not a copy, when both are on
# the same drive -- and separately, that the documented copy-fallback (and
# its "restart your shell" reminder) actually fires for real when they
# aren't. No POSIX equivalent (see windows-port.md Phase 3): this is new
# surface Phase 8's own plan calls out by name.

Describe "hard links" {
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

    It "creates a real hard link (same file) when source and shim dir share a drive" {
        # Deliberately NOT reusing the shared $Sandbox: on a real CI runner
        # (confirmed this port's own windows-2025 jobs: checkout/%TEMP% on
        # D:, same as this dev machine) %TEMP% and shimback.exe's own build
        # output don't share a drive by default, which would make this test
        # always skip in exactly the environment it most needs to run in.
        # Building the sandbox explicitly next to the running exe instead
        # guarantees the same-drive shape everywhere.
        $exeDrive = (Get-Item (Get-ShimbackExe)).PSDrive.Root
        $guid = [Guid]::NewGuid().ToString("N")
        $sameDriveRoot = Join-Path $exeDrive "shimback-pester-samedrive-$guid"
        New-Item -ItemType Directory -Force -Path "$sameDriveRoot\home" | Out-Null
        $sameDriveSandbox = [pscustomobject]@{
            Root          = $sameDriveRoot
            Home          = "$sameDriveRoot\home"
            ConfigHome    = "$sameDriveRoot\home\.config"
            DataHome      = "$sameDriveRoot\home\.local\share"
            SavedEnv      = $null
            ShellSnapshot = $null
        }
        Enter-ShimbackSandbox $sameDriveSandbox
        try {
            $a = @("add", "linktool", "-s", $PS, "-f", $PS)
            foreach ($x in $PrimaryArgs) { $a += @("--source-arg", $x) }
            foreach ($x in $FallbackArgs) { $a += @("--fallback-arg", $x) }
            $r = Invoke-Shimback $a
            $r.ExitCode | Should -Be 0
            $r.StdErr | Should -Not -Match "created a copy"

            $link = Get-ShimPath "linktool" $sameDriveSandbox
            Test-Path $link | Should -BeTrue
            Test-SameHardLink $link (Get-ShimbackExe) | Should -BeTrue

            # A real hard link's size tracks the live binary's exactly (it
            # IS the same file) -- unlike a copy, which would go stale.
            (Get-Item $link).Length | Should -Be (Get-Item (Get-ShimbackExe)).Length
        }
        finally {
            Exit-ShimbackSandbox $sameDriveSandbox
            Remove-Item -Recurse -Force $sameDriveRoot -ErrorAction SilentlyContinue
        }
    }

    It "falls back to a copy across drives, with the restart-your-shell reminder" {
        # TEMPORARY diagnostics -- remove once the windows-arm64 CI behavior here is understood.
        Write-Host "DIAG exe path: $(Get-ShimbackExe)"
        Write-Host "DIAG exe drive name: $((Get-Item (Get-ShimbackExe)).PSDrive.Name)"
        Get-PSDrive -PSProvider FileSystem | ForEach-Object {
            Write-Host "DIAG PSDrive: Name=$($_.Name) Root=$($_.Root) Free=$($_.Free) Used=$($_.Used)"
        }

        $otherDrive = (Get-PSDrive -PSProvider FileSystem | Where-Object {
                $_.Name -ne (Get-Item (Get-ShimbackExe)).PSDrive.Name -and $_.Free -gt 10MB
            } | Select-Object -First 1).Name
        Write-Host "DIAG selected otherDrive: '$otherDrive'"
        if (-not $otherDrive) {
            Set-ItResult -Skipped -Because "no second writable drive available on this machine to force a cross-drive copy"
            return
        }

        # A different drive LETTER doesn't guarantee a different NTFS volume --
        # confirmed the hard way on a windows-11-arm hosted runner, which offers
        # a second FileSystem PSDrive that isn't actually a separate volume from
        # the exe's own drive, so CreateHardLinkW succeeds for real across them
        # and this whole test's premise (forcing the copy-fallback path) never
        # fires. shimback's own fallback is driven by whether CreateHardLinkW
        # itself succeeds, not by drive letter, so probe with that exact
        # mechanism (fsutil hardlink create) before trusting the letter.
        $probeSrc = Join-Path $env:TEMP ("shimback-hardlink-probe-" + [Guid]::NewGuid().ToString("N") + ".tmp")
        $probeDst = Join-Path "${otherDrive}:\" ("shimback-hardlink-probe-" + [Guid]::NewGuid().ToString("N") + ".tmp")
        Set-Content -Path $probeSrc -Value "probe"
        try {
            $probeOut = & fsutil hardlink create $probeDst $probeSrc 2>&1 | Out-String
            $sameVolume = ($LASTEXITCODE -eq 0)
            Write-Host "DIAG probeSrc: $probeSrc"
            Write-Host "DIAG probeDst: $probeDst"
            Write-Host "DIAG fsutil exit: $LASTEXITCODE output: $probeOut"
            Write-Host "DIAG sameVolume: $sameVolume"
        }
        finally {
            Remove-Item -Force $probeSrc -ErrorAction SilentlyContinue
            Remove-Item -Force $probeDst -ErrorAction SilentlyContinue
        }
        if ($sameVolume) {
            Set-ItResult -Skipped -Because "'$otherDrive' isn't actually a separate volume from the exe's own drive on this machine, so a real cross-volume drive isn't available to force the copy fallback"
            return
        }

        $crossDriveSandbox = Join-Path "${otherDrive}:\" ("shimback-pester-xdrive-" + [Guid]::NewGuid().ToString("N"))
        New-Item -ItemType Directory -Force -Path $crossDriveSandbox | Out-Null
        try {
            $savedData = $env:XDG_DATA_HOME
            $env:XDG_DATA_HOME = $crossDriveSandbox
            $a = @("add", "xdrivetool", "-s", $PS, "-f", $PS)
            foreach ($x in $PrimaryArgs) { $a += @("--source-arg", $x) }
            foreach ($x in $FallbackArgs) { $a += @("--fallback-arg", $x) }
            $r = Invoke-Shimback $a
            $r.ExitCode | Should -Be 0
            $r.StdErr | Should -Match "created a copy of shimback's binary"
            $r.StdErr | Should -Match "restart your shell"

            $link = "$crossDriveSandbox/shimback/bin/xdrivetool.exe"
            Test-Path $link | Should -BeTrue
            Test-SameHardLink $link (Get-ShimbackExe) | Should -BeFalse
        }
        finally {
            $env:XDG_DATA_HOME = $savedData
            Remove-Item -Recurse -Force $crossDriveSandbox -ErrorAction SilentlyContinue
        }
    }
}
