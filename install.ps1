# shimback installer -- downloads the right prebuilt binary for this
# machine from GitHub Releases and hands it to `shimback install`, which
# does the rest (copies itself to a stable location, sets up PATH, installs
# the man page). Windows x86_64 and arm64. Usage:
#
#   irm https://raw.githubusercontent.com/pnavais/shimback/main/install.ps1 | iex
#
# Extra arguments (e.g. --prefix) can't be forwarded through a plain `iex`
# pipeline -- PowerShell has no equivalent of `sh -s --`. Use this form
# instead, which does support them:
#
#   & ([scriptblock]::Create((irm https://raw.githubusercontent.com/pnavais/shimback/main/install.ps1))) --prefix C:\tools
#
# Pin a specific release instead of the latest one via $env:SHIMBACK_VERSION:
#
#   $env:SHIMBACK_VERSION = "v0.1.0"; irm .../install.ps1 | iex

param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$InstallArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Repo = "pnavais/shimback"

function Use-Color {
    -not $env:NO_COLOR -and [Console]::IsOutputRedirected -eq $false
}

$script:Colorize = Use-Color

# ANSI colors, or empty strings when not colorizing -- same palette as
# install.sh.
if ($script:Colorize) {
    # [char]27, not `e: Windows PowerShell 5.1 has no `e escape.
    $esc = [char]27
    $script:Blue = "${esc}[1;34m"; $script:White = "${esc}[1;97m"; $script:Gold = "${esc}[38;5;220m"
    $script:Cyan = "${esc}[1;36m"; $script:Green = "${esc}[1;32m"; $script:Reset = "${esc}[0m"
} else {
    $script:Blue = ""; $script:White = ""; $script:Gold = ""
    $script:Cyan = ""; $script:Green = ""; $script:Reset = ""
}

function Write-Banner {
    # Single-quoted: the art's backticks and backslashes must stay literal
    # (in a double-quoted string ` is PowerShell's escape character).
    $art = @(
        @('__   __  ', '      _     _           _                _'),
        @('\ \  \ \ ', '  ___| |__ (_)_ __ ___ | |__   __ _  ___| | __'),
        @(' \ \  \ \', ' / __| ''_ \| | ''_ ` _ \| ''_ \ / _` |/ __| |/ /'),
        @(' / /  / /', ' \__ \ | | | | | | | | | |_) | (_| | (__|   <'),
        @('/_/  /_/ ', ' |___/_| |_|_|_| |_| |_|_.__/ \__,_|\___|_|\_\')
    )
    foreach ($line in $art) {
        Write-Host ($Blue + $line[0] + $White + $line[1] + $Reset)
    }
    Write-Host "$Gold  >> run a primary command, transparently fall back to another >>$Reset"
    Write-Host "$Gold  Copyright (c) 2026 pnavais -- MIT OR Apache-2.0$Reset"
    Write-Host ""
}

function Write-Step($text) {
    Write-Host "$Cyan$text$Reset..."
}

function Write-Ok($text) {
    Write-Host "  ${Green}[ok]${Reset} $text"
}

function Die($msg) {
    Write-Error "shimback-install: $msg" -ErrorAction Continue
    exit 1
}

function Write-Indented($text) {
    ($text -split "`r?`n") | ForEach-Object { Write-Host "  $_" }
}

# Reports THIS PROCESS's own architecture, not necessarily the underlying
# hardware's -- e.g. an x64 PowerShell running via Windows' x64-on-ARM64
# emulation layer reports "x86_64" here, not "arm64". Deliberately matches
# install.sh's own uname()-based detect_arch(), which has the equivalent
# behavior for a Rosetta-translated shell on Apple Silicon (uname -m
# reports the process's own architecture there too): install to whatever
# this installer is actually running as, not necessarily the fastest
# option the hardware could support.
function Get-WindowsArch {
    switch ([System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture) {
        "X64" { return "x86_64" }
        "Arm64" { return "arm64" }
        default {
            Die "unsupported architecture '$_' -- shimback ships Windows x86_64 and arm64 binaries only. Build from source instead: https://github.com/$Repo#building"
        }
    }
}

function Main {
    Write-Banner

    $arch = Get-WindowsArch
    $pkgName = "shimback-windows-$arch"
    $asset = "$pkgName.zip"
    $version = if ($env:SHIMBACK_VERSION) { $env:SHIMBACK_VERSION } else { "latest" }
    $baseUrl = if ($version -eq "latest") {
        "https://github.com/$Repo/releases/latest/download"
    } else {
        "https://github.com/$Repo/releases/download/$version"
    }
    $url = "$baseUrl/$asset"
    $sumsUrl = "$baseUrl/SHA256SUMS"

    $tmpDir = Join-Path $env:TEMP ("shimback-install." + [System.IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
    try {
        $assetPath = Join-Path $tmpDir $asset
        $sumsPath = Join-Path $tmpDir "SHA256SUMS"

        Write-Host "${Cyan}Downloading$Reset $asset ($version)..."
        try {
            Invoke-WebRequest -Uri $url -OutFile $assetPath -UseBasicParsing
        } catch {
            Die "failed to download $url -- check that a '$version' release exists for windows/$arch"
        }
        Write-Ok "Download complete"

        Write-Step "Verifying checksum"
        try {
            Invoke-WebRequest -Uri $sumsUrl -OutFile $sumsPath -UseBasicParsing
        } catch {
            Die "failed to download $sumsUrl -- refusing to install an unverified binary"
        }
        $expected = (Get-Content $sumsPath | Where-Object { $_ -match [regex]::Escape($asset) + '\s*$' } |
            Select-Object -First 1) -split '\s+' | Select-Object -First 1
        if (-not $expected) {
            Die "no checksum entry for $asset in SHA256SUMS -- refusing to install an unverified binary"
        }
        $actual = (Get-FileHash -Path $assetPath -Algorithm SHA256).Hash.ToLower()
        if ($actual -ne $expected) {
            Die "checksum mismatch for $asset (expected $expected, got $actual) -- refusing to install a possibly corrupted or tampered download"
        }
        Write-Ok "Checksum verified"

        Expand-Archive -LiteralPath $assetPath -DestinationPath $tmpDir -Force
        $bin = Join-Path $tmpDir "$pkgName\shimback.exe"
        if (-not (Test-Path $bin)) {
            Die "downloaded archive didn't contain an executable 'shimback.exe' binary"
        }

        Write-Step "Installing"
        # Captured (to indent it and spot "already installed" below), so
        # shimback can't see a console: CLICOLOR_FORCE keeps its colors when
        # ours are on.
        $savedForce = $env:CLICOLOR_FORCE
        $env:CLICOLOR_FORCE = if ($script:Colorize) { "1" } else { "0" }
        try {
            $installOutput = & $bin install @InstallArgs 2>&1 | Out-String
        } finally {
            $env:CLICOLOR_FORCE = $savedForce
        }
        $installExit = $LASTEXITCODE
        Write-Indented $installOutput.TrimEnd()
        if ($installExit -ne 0) {
            Die "bundled shimback install failed"
        }
        # `shimback install` refuses to install over an existing installation
        # and says so ("already installed ..."), exiting 0 -- don't announce
        # a completed installation when nothing was changed.
        if ($installOutput -match "already installed") {
            Write-Host "  ${Gold}[!]${Reset} Nothing changed -- to upgrade, run: shimback update"
        } else {
            Write-Ok "Installation complete"
        }
    } finally {
        Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue
    }
}

Main
