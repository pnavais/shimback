# Shared Pester helpers for shimback's Windows test suite -- the native
# Pester counterpart to tests/test_common.sh. Dot-sourced from each
# *.Tests.ps1 file.
#
# Sandbox isolation covers config.toml and the shim directory (via
# XDG_CONFIG_HOME/XDG_DATA_HOME/HOME, exactly like the POSIX suite), which
# is everything `dispatch`/`add`/`remove`/`doctor`/`info`/`list` touch.
# It does NOT cover the PowerShell profile / cmd.exe AutoRun /
# HKCU\Environment\Path shell integration shimback's `add`/`init`/
# `install`/`uninstall` also touch -- shell.c resolves the PowerShell
# profile from the real Windows "Documents" special folder via
# SHGetKnownFolderPath, and the other two are real per-user registry
# state, none of which any env var override can redirect (confirmed the
# hard way during this port -- see windows-port.md Phase 5's addendum).
# Tests that need to exercise that real surface use
# Test-RealShellIntegrationAllowed/Backup-RealShellState below, which only
# run for real when explicitly opted into (always true in CI, off by
# default locally), and always snapshot + restore afterward regardless.

$script:RepoRoot = (Resolve-Path "$PSScriptRoot/../..").Path

function Get-ShimbackExe {
    if ($env:SHIMBACK_EXE) { return (Resolve-Path $env:SHIMBACK_EXE).Path }
    $candidate = Join-Path $script:RepoRoot "build-windows-x86_64\shimback.exe"
    if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
    throw "shimback.exe not found -- build it first (see justfile), or set `$env:SHIMBACK_EXE"
}

function Get-PowerShellExe {
    if (Test-Path "$PSHOME\powershell.exe") { return "$PSHOME\powershell.exe" }
    $pwsh = Get-Command pwsh -ErrorAction SilentlyContinue
    if ($pwsh) { return $pwsh.Source }
    throw "no powershell.exe/pwsh.exe found to run fixture scripts with"
}

function Get-FixturePath {
    param([Parameter(Mandatory)][string]$Name)
    Join-Path $PSScriptRoot "fixtures\$Name"
}

# Fixed args (source_args/fallback_args) that make a shim's source/fallback
# "powershell -NoProfile -File <fixture>.ps1" -- reuses shimback's own
# baked-in-args alias mechanism rather than needing compiled fixtures.
function Get-FixtureArgs {
    param([Parameter(Mandatory)][string]$Name)
    @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Get-FixturePath $Name))
}

function New-ShimbackSandbox {
    $sandboxRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("shimback-pester-" + [Guid]::NewGuid().ToString("N"))
    $homeDir = Join-Path $sandboxRoot "home"
    New-Item -ItemType Directory -Force -Path $homeDir | Out-Null
    [pscustomobject]@{
        Root       = $sandboxRoot
        Home       = $homeDir
        ConfigHome = Join-Path $homeDir ".config"
        DataHome   = Join-Path $homeDir ".local\share"
        SavedEnv   = $null
        ShellSnapshot = $null
    }
}

function Enter-ShimbackSandbox {
    param([Parameter(Mandatory)]$Sandbox)
    $Sandbox.SavedEnv = @{
        XDG_CONFIG_HOME = $env:XDG_CONFIG_HOME
        XDG_DATA_HOME   = $env:XDG_DATA_HOME
        HOME            = $env:HOME
        PATH            = $env:PATH
    }
    $env:XDG_CONFIG_HOME = $Sandbox.ConfigHome
    $env:XDG_DATA_HOME = $Sandbox.DataHome
    $env:HOME = $Sandbox.Home
    # Simulates a shell that actually sourced the injected PATH block --
    # the sandbox's own profile is written but never sourced by this
    # process (same workaround test_common.sh's own comment describes for
    # the bash suite). Needed for doctor's/info's "is this shim first on
    # PATH" check specifically -- that check (dir_on_path in doctor.c) is a
    # plain strcmp against $env:PATH's entries, with no separator
    # normalization, so this must match shim_bin_dir()'s own path_join
    # byte-for-byte: '/' before "shimback/bin", not Join-Path's '\'.
    $env:PATH = "$($Sandbox.DataHome)/shimback/bin;$env:PATH"
    # `add`/`init`/`install`/`uninstall` all call shell_ensure_path
    # unconditionally -- there is no env var that redirects the real
    # PowerShell profile / cmd.exe AutoRun / HKCU\Environment\Path lookups
    # away from the real machine (see this file's header comment), so ANY
    # sandboxed test that happens to call one of those commands still
    # mutates real, permanent, per-user state. Snapshotting here and
    # restoring in Exit-ShimbackSandbox makes every sandbox safe by
    # construction, regardless of whether a given test file's author
    # remembered that hazard -- found the hard way when a plain `add` in a
    # config/data-sandboxed Doctor test still overwrote the real PowerShell
    # profile with a temp-directory path (see windows-port.md's own
    # addendum on this).
    $Sandbox.ShellSnapshot = Backup-RealShellState
}

function Exit-ShimbackSandbox {
    param([Parameter(Mandatory)]$Sandbox)
    $env:XDG_CONFIG_HOME = $Sandbox.SavedEnv.XDG_CONFIG_HOME
    $env:XDG_DATA_HOME = $Sandbox.SavedEnv.XDG_DATA_HOME
    $env:HOME = $Sandbox.SavedEnv.HOME
    $env:PATH = $Sandbox.SavedEnv.PATH
    if ($Sandbox.ShellSnapshot) {
        Restore-RealShellState $Sandbox.ShellSnapshot
    }
}

function Remove-ShimbackSandbox {
    param($Sandbox)
    if ($Sandbox -and (Test-Path $Sandbox.Root)) {
        Remove-Item -Recurse -Force $Sandbox.Root -ErrorAction SilentlyContinue
    }
}

function Get-ShimPath {
    param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)]$Sandbox)
    Join-Path $Sandbox.DataHome "shimback\bin\$Name.exe"
}

function Get-ConfigFile {
    param([Parameter(Mandatory)]$Sandbox)
    Join-Path $Sandbox.ConfigHome "shimback\config.toml"
}

# Runs an arbitrary exe (shimback.exe itself, or one of its own shim
# symlinks/copies) with stdout/stderr captured *separately* -- like the
# bash suite's own out/err/code triple, not PowerShell's merging `2>&1`.
# Uses ReadToEndAsync on both streams before WaitForExit, not a blocking
# ReadToEnd on one then the other -- avoids the exact pipe-buffer deadlock
# dispatch.c's own run_captured has to guard against for a chatty child
# (see the "large interleaved stdout/stderr" fixture/test).
function Invoke-Exe {
    param(
        [Parameter(Mandatory)][string]$Path,
        [string[]]$ExeArgs = @(),
        [hashtable]$EnvOverrides = @{},
        # Written to the child's stdin then closed (EOF) -- for doctor fix's
        # interactive prompts. $null (the default) redirects stdin to
        # nothing and closes it immediately, giving an instant EOF, same as
        # the bash suite's `exec </dev/null`/`</dev/null` redirections
        # (never inherits this process's own stdin, which would leave a
        # background test run stalled on a real console).
        [string]$StdIn = $null
    )
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Path
    foreach ($a in $ExeArgs) { $psi.ArgumentList.Add($a) }
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = $true
    $psi.UseShellExecute = $false
    foreach ($k in $EnvOverrides.Keys) { $psi.EnvironmentVariables[$k] = $EnvOverrides[$k] }
    $proc = [System.Diagnostics.Process]::Start($psi)
    if ($StdIn) { $proc.StandardInput.Write($StdIn) }
    $proc.StandardInput.Close()
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    $proc.WaitForExit()
    [pscustomobject]@{
        StdOut   = $stdoutTask.GetAwaiter().GetResult()
        StdErr   = $stderrTask.GetAwaiter().GetResult()
        ExitCode = $proc.ExitCode
    }
}

function Invoke-Shimback {
    param([string[]]$ShimbackArgs = @(), [hashtable]$EnvOverrides = @{}, [string]$StdIn = $null)
    Invoke-Exe -Path (Get-ShimbackExe) -ExeArgs $ShimbackArgs -EnvOverrides $EnvOverrides -StdIn $StdIn
}

# Opt-in gate for tests that mutate the *real* PowerShell profile / cmd.exe
# AutoRun / HKCU\Environment\Path -- see this file's own header comment.
# CI sets SHIMBACK_TEST_REAL_SHELL=1 itself (ephemeral runner, nothing to
# preserve); a local run stays off by default.
function Test-RealShellIntegrationAllowed {
    return $env:SHIMBACK_TEST_REAL_SHELL -eq "1"
}

# Snapshots every piece of real, non-sandboxable state shimback's shell
# integration can touch, so a test that opts into mutating it for real can
# restore exactly what was there before -- regardless of CI's ephemeral-
# runner safety, so a local opt-in run (or a CI runner image that changes
# to persist user profiles some day) never leaves real state behind.
function Backup-RealShellState {
    $winPsProfile = "$([Environment]::GetFolderPath('MyDocuments'))\WindowsPowerShell\profile.ps1"
    $psProfile = "$([Environment]::GetFolderPath('MyDocuments'))\PowerShell\profile.ps1"
    $autoRun = (Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Command Processor' `
            -Name AutoRun -ErrorAction SilentlyContinue).AutoRun
    $userPath = (Get-ItemProperty -Path 'HKCU:\Environment' -Name Path -ErrorAction SilentlyContinue).Path
    [pscustomobject]@{
        WinPsProfilePath    = $winPsProfile
        WinPsProfileExisted = Test-Path $winPsProfile
        WinPsProfileContent = if (Test-Path $winPsProfile) { Get-Content -Raw $winPsProfile } else { $null }
        PsProfilePath       = $psProfile
        PsProfileExisted    = Test-Path $psProfile
        PsProfileContent    = if (Test-Path $psProfile) { Get-Content -Raw $psProfile } else { $null }
        AutoRun             = $autoRun
        UserPath            = $userPath
    }
}

function Restore-RealShellState {
    param([Parameter(Mandatory)]$Snapshot)
    if ($Snapshot.WinPsProfileExisted) {
        New-Item -ItemType Directory -Force -Path (Split-Path $Snapshot.WinPsProfilePath) | Out-Null
        Set-Content -NoNewline -Path $Snapshot.WinPsProfilePath -Value $Snapshot.WinPsProfileContent
    }
    elseif (Test-Path $Snapshot.WinPsProfilePath) {
        Remove-Item -Force $Snapshot.WinPsProfilePath
    }
    if ($Snapshot.PsProfileExisted) {
        New-Item -ItemType Directory -Force -Path (Split-Path $Snapshot.PsProfilePath) | Out-Null
        Set-Content -NoNewline -Path $Snapshot.PsProfilePath -Value $Snapshot.PsProfileContent
    }
    elseif (Test-Path $Snapshot.PsProfilePath) {
        Remove-Item -Force $Snapshot.PsProfilePath
    }
    if ($null -ne $Snapshot.AutoRun) {
        Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Command Processor' -Name AutoRun -Value $Snapshot.AutoRun
    }
    else {
        Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Command Processor' -Name AutoRun -ErrorAction SilentlyContinue
    }
    if ($null -ne $Snapshot.UserPath) {
        Set-ItemProperty -Path 'HKCU:\Environment' -Name Path -Value $Snapshot.UserPath
    }
    else {
        Remove-ItemProperty -Path 'HKCU:\Environment' -Name Path -ErrorAction SilentlyContinue
    }
}

# True if two paths are hard-linked to the same file (same file ID on the
# same volume) -- shells out to fsutil, which lists every path hard-linked
# to a file's underlying data, rather than P/Invoking
# GetFileInformationByHandle directly (fsutil is the documented, supported
# CLI surface for exactly this question, and needs no elevation to query).
function Test-SameHardLink {
    param([Parameter(Mandatory)][string]$PathA, [Parameter(Mandatory)][string]$PathB)
    $linksOfA = fsutil hardlink list $PathA 2>$null
    if (-not $linksOfA) { return $false }
    $root = Split-Path -Qualifier $PathA
    $bRelative = (Resolve-Path $PathB).Path.Substring($root.Length)
    return ($linksOfA -contains $bRelative) -or ((Resolve-Path $PathA).Path -eq (Resolve-Path $PathB).Path)
}
