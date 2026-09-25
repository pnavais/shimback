# shimback installer -- downloads the right prebuilt binary for this
# machine from GitHub Releases and hands it to `shimback install`, which
# does the rest (copies itself to a stable location, sets up PATH, installs
# the man page). Windows x86_64 only -- see windows-port.md's "Resolved
# decisions" for why ARM64 isn't built yet. Usage:
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

function Write-Banner {
    if ($script:Colorize) {
        $blue = "`e[1;34m"; $white = "`e[1;97m"; $gold = "`e[38;5;220m"; $reset = "`e[0m"
    } else {
        $blue = ""; $white = ""; $gold = ""; $reset = ""
    }
    Write-Host "${blue}__   __  ${white}      _     _           _                _${reset}"
    Write-Host "${blue}\ \  \ \ ${white}  ___| |__ (_)_ __ ___ | |__   __ _  ___| | __${reset}"
    Write-Host "${blue} \ \  \ \${white} / __| '_ \| | '_ ``_ \| '_ \ / _`` |/ __| |/ /${reset}"
    Write-Host "${blue} / /  / /${white} \__ \ | | | | | | | | | |_) | (_| | (__|   <${reset}"
    Write-Host "${blue}/_/  /_/ ${white} |___/_| |_|_|_| |_| |_|_.__/ \__,_|\___|_|\_\${reset}"
    Write-Host "${gold}  >> run a primary command, transparently fall back to another >>${reset}"
    Write-Host "${gold}  Copyright (c) 2026 pnavais -- MIT OR Apache-2.0${reset}"
    Write-Host ""
}

function Die($msg) {
    Write-Error "shimback-install: $msg" -ErrorAction Continue
    exit 1
}

function Write-Indented($text) {
    ($text -split "`r?`n") | ForEach-Object { Write-Host "  $_" }
}

function Main {
    Write-Banner

    $asset = "shimback-windows-x86_64.zip"
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

        Write-Host "Downloading $asset ($version)..."
        try {
            Invoke-WebRequest -Uri $url -OutFile $assetPath -UseBasicParsing
        } catch {
            Die "failed to download $url -- check that a '$version' release exists for windows/x86_64"
        }
        Write-Host "  [ok] Download complete"

        Write-Host "Verifying checksum..."
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
        Write-Host "  [ok] Checksum verified"

        Expand-Archive -LiteralPath $assetPath -DestinationPath $tmpDir -Force
        $bin = Join-Path $tmpDir "shimback-windows-x86_64\shimback.exe"
        if (-not (Test-Path $bin)) {
            Die "downloaded archive didn't contain an executable 'shimback.exe' binary"
        }

        Write-Host "Installing..."
        $installOutput = & $bin install @InstallArgs 2>&1 | Out-String
        $installExit = $LASTEXITCODE
        Write-Indented $installOutput.TrimEnd()
        if ($installExit -ne 0) {
            Die "bundled shimback install failed"
        }
        # `shimback install` refuses to install over an existing installation
        # and says so ("already installed ..."), exiting 0 -- don't announce
        # a completed installation when nothing was changed.
        if ($installOutput -match "already installed") {
            Write-Host "  [!] Nothing changed -- to upgrade, run: shimback update"
        } else {
            Write-Host "  [ok] Installation complete"
        }
    } finally {
        Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue
    }
}

Main
