# Windows Pester counterpart to tests/test_doctor.sh -- Windows has no
# "dangling symlink" case at all (a hard link's file data is alive as long
# as its own directory entry exists, regardless of self_exe's own path --
# see doctor.c's own Windows branch of check_symlink/fix_symlink_if_needed),
# so this covers "missing" and "doesn't look like a shimback binary"
# instead of POSIX's missing/dangling pair.

Describe "doctor" {
    BeforeAll {
        . "$PSScriptRoot\Common.ps1"
        $script:Sandbox = New-ShimbackSandbox
        Enter-ShimbackSandbox $Sandbox
        $script:PS = Get-PowerShellExe
        $script:PrimaryArgs = Get-FixtureArgs "FakePrimary.ps1"
        $script:FallbackArgs = Get-FixtureArgs "FakeFallback.ps1"
        # Builds only the argument array (never calls Invoke-Shimback itself
        # -- a scriptblock invoked via `&` from a sibling It's own isolated
        # scope resolves *commands* against the caller's scope chain, not
        # the one it was defined in, so a helper that calls another function
        # here would fail to find it; building plain data has no such
        # problem). References $script:-scoped variables explicitly rather
        # than relying on .GetNewClosure() -- found the hard way that
        # GetNewClosure() only snapshots true *local* variables from its
        # immediately enclosing scope, silently leaving a bare `$PS`
        # (actually `$script:PS`) empty once invoked from a different It
        # block's own scope; an explicit `$script:` prefix always does a
        # live lookup instead, closure or not.
        $script:BasicShimArgs = {
            param([string]$Name, [string]$Fallback = $script:PS, [string[]]$ExtraArgs = @())
            $a = @("add", $Name, "-s", $script:PS, "-f", $Fallback) + $ExtraArgs
            foreach ($x in $script:PrimaryArgs) { $a += @("--source-arg", $x) }
            foreach ($x in $script:FallbackArgs) { $a += @("--fallback-arg", $x) }
            $a
        }
    }

    AfterAll {
        Exit-ShimbackSandbox $Sandbox
        Remove-ShimbackSandbox $Sandbox
    }

    It "reports healthy with nothing configured, exit 0" {
        $r = Invoke-Shimback @("doctor")
        $r.ExitCode | Should -Be 0
        $r.StdOut | Should -Match "No shims configured."
    }

    It "passes for a healthy shim" {
        Invoke-Shimback (& $script:BasicShimArgs "mytool") | Out-Null
        $r = Invoke-Shimback @("doctor")
        $r.ExitCode | Should -Be 0
        $r.StdOut | Should -Match "hard link to the shimback binary"
        $r.StdOut | Should -Match "all checks passed"
        Invoke-Shimback @("remove", "-y", "mytool") | Out-Null
    }

    It "fails and points at doctor fix when a shim is missing" {
        Invoke-Shimback (& $script:BasicShimArgs "misstool") | Out-Null
        $link = Get-ShimPath "misstool" $Sandbox
        Remove-Item -Force $link
        $r = Invoke-Shimback @("doctor")
        $r.ExitCode | Should -Not -Be 0
        $r.StdOut | Should -Match "no shim at"
        $r.StdOut | Should -Match "doctor fix.*to recreate it"
    }

    It "doctor fix recreates a missing shim" {
        $link = Get-ShimPath "misstool" $Sandbox
        $r = Invoke-Shimback @("doctor", "fix") -StdIn ""
        $r.StdOut | Should -Match "\[fixed\] recreated missing (symlink|shim)"
        Test-Path $link | Should -BeTrue
        (Invoke-Shimback @("doctor")).ExitCode | Should -Be 0
        Invoke-Shimback @("remove", "-y", "misstool") | Out-Null
    }

    It "fails when a shim's file doesn't look like a shimback binary" {
        Invoke-Shimback (& $script:BasicShimArgs "corrupttool") | Out-Null
        $link = Get-ShimPath "corrupttool" $Sandbox
        Set-Content -Path $link -Value "not a real binary"
        $r = Invoke-Shimback @("doctor")
        $r.ExitCode | Should -Not -Be 0
        $r.StdOut | Should -Match "doesn't look like a shimback binary"
        Invoke-Shimback @("remove", "-y", "corrupttool") | Out-Null
    }

    It "rejects an unrecognized positional argument" {
        $r = Invoke-Shimback @("doctor", "bogus")
        $r.ExitCode | Should -Not -Be 0
    }

    It "fails when a fallback binary has disappeared" {
        $driftedFallback = Join-Path $Sandbox.Root "fallback-copy.exe"
        Copy-Item (Get-ShimbackExe) $driftedFallback
        Invoke-Shimback (& $script:BasicShimArgs "drifted" $driftedFallback) | Out-Null
        Remove-Item -Force $driftedFallback
        $r = Invoke-Shimback @("doctor")
        $r.ExitCode | Should -Not -Be 0
        $r.StdOut | Should -Match "does not exist or is not executable"
        Invoke-Shimback @("remove", "-y", "drifted") | Out-Null
    }

    It "add refuses a source/fallback that resolves back to shimback itself" {
        $exe = Get-ShimbackExe
        $r = Invoke-Shimback @("add", "cycletool", "-s", $PS, "-f", $exe)
        $r.ExitCode | Should -Not -Be 0
        $r.StdErr | Should -Match "resolves back to the shimback binary itself"
        (Invoke-Shimback @("list")).StdOut | Should -Not -Match "cycletool"
    }

    It "doctor detects a cycle induced by hand-editing config.toml" {
        $exe = Get-ShimbackExe
        $cfg = Get-ConfigFile $Sandbox
        # TOML basic strings double each backslash once ('\' -> '\\') -- the
        # single-quoted '\\' replacement below really is 2 literal backslash
        # characters, matching config.c's own append_escaped_string.
        $srcEsc = $PrimaryArgs[4] -replace '\\', '\\'
        $psEsc = $PS -replace '\\', '\\'
        $exeEsc = $exe -replace '\\', '\\'
        New-Item -ItemType Directory -Force -Path (Split-Path $cfg) | Out-Null
        Set-Content -Path $cfg -Value @"
version = 1

[shims.cyc]
source = "$psEsc"
source_args = ["$($PrimaryArgs[0])", "$($PrimaryArgs[1])", "$($PrimaryArgs[2])", "$($PrimaryArgs[3])", "$srcEsc"]
fallback = "$exeEsc"
policy = "exit-code"
"@
        $shimDir = Join-Path $Sandbox.DataHome "shimback\bin"
        New-Item -ItemType Directory -Force -Path $shimDir | Out-Null
        Copy-Item $exe (Join-Path $shimDir "cyc.exe")

        $r = Invoke-Shimback @("doctor")
        $r.ExitCode | Should -Not -Be 0
        $r.StdOut | Should -Match "resolves back to the shimback binary itself"

        $fbEsc = ($FallbackArgs[4]) -replace '\\', '\\'
        $fix = Invoke-Shimback @("doctor", "fix") -StdIn "$($FallbackArgs[4])`n"
        $fix.StdOut | Should -Match "\[fixed\] fallback updated to"
        (Get-Content -Raw $cfg).Contains($fbEsc) | Should -BeTrue
        Invoke-Shimback @("remove", "-y", "cyc") | Out-Null
    }

    It "add --force allows a source/fallback path that doesn't exist yet" {
        $ghostSrc = Join-Path $Sandbox.Root "ghost-source.exe"
        $ghostFb = Join-Path $Sandbox.Root "ghost-fallback.exe"
        $r = Invoke-Shimback @("add", "ghosttool", "-s", $ghostSrc, "-f", $ghostFb, "--force")
        $r.ExitCode | Should -Be 0
        (Get-Content -Raw (Get-ConfigFile $Sandbox)) | Should -Match "force = true"

        $doc = Invoke-Shimback @("doctor")
        $doc.StdOut | Should -Match "added with --force; not currently on disk, so not checked"

        Copy-Item (Get-ShimbackExe) $ghostSrc
        Copy-Item (Get-ShimbackExe) $ghostFb
        $doc2 = Invoke-Shimback @("doctor")
        $doc2.StdOut | Should -Not -Match ([regex]::Escape("$ghostSrc (added with --force"))
        Invoke-Shimback @("remove", "-y", "ghosttool") | Out-Null
    }

    It "detects and removes an orphaned symlink (real shim, no config)" {
        Invoke-Shimback (& $script:BasicShimArgs "orphantool") | Out-Null
        $cfg = Get-ConfigFile $Sandbox
        $content = Get-Content -Raw $cfg
        $stripped = $content -replace '(?ms)^\[shims\.orphantool\].*?(?=^\[|\z)', ''
        Set-Content -NoNewline -Path $cfg -Value $stripped
        $link = Get-ShimPath "orphantool" $Sandbox
        Test-Path $link | Should -BeTrue

        $r = Invoke-Shimback @("doctor")
        $r.ExitCode | Should -Not -Be 0
        $r.StdOut | Should -Match "orphaned symlink"

        $declined = Invoke-Shimback @("doctor", "fix") -StdIn "n`n"
        $declined.StdOut | Should -Match "no configuration anywhere for it"
        Test-Path $link | Should -BeTrue

        $accepted = Invoke-Shimback @("doctor", "fix") -StdIn "y`n"
        $accepted.StdOut | Should -Match "\[fixed\] removed orphaned"
        Test-Path $link | Should -BeFalse
        (Invoke-Shimback @("doctor")).ExitCode | Should -Be 0
    }

    It "doctor fix -y removes an orphan without prompting" {
        Invoke-Shimback (& $script:BasicShimArgs "orphantool2") | Out-Null
        $cfg = Get-ConfigFile $Sandbox
        $content = Get-Content -Raw $cfg
        $stripped = $content -replace '(?ms)^\[shims\.orphantool2\].*?(?=^\[|\z)', ''
        Set-Content -NoNewline -Path $cfg -Value $stripped
        $link = Get-ShimPath "orphantool2" $Sandbox

        $r = Invoke-Shimback @("doctor", "fix", "-y") -StdIn ""
        $r.StdOut | Should -Match "\[fixed\] removed orphaned"
        $r.StdOut | Should -Not -Match "Remove the symlink"
        Test-Path $link | Should -BeFalse
    }

    It "doctor fix creates a missing shim directory and recreates its symlinks" {
        Invoke-Shimback (& $script:BasicShimArgs "dirtool") | Out-Null
        $shimDir = Join-Path $Sandbox.DataHome "shimback\bin"
        Remove-Item -Recurse -Force $shimDir
        $r = Invoke-Shimback @("doctor")
        $r.StdOut | Should -Match "doctor fix.*to create it"

        $fix = Invoke-Shimback @("doctor", "fix", "-y") -StdIn ""
        $fix.StdOut | Should -Match "\[fixed\] created shim directory"
        $fix.StdOut | Should -Not -Match "failed to acquire the shim directory lock"
        Test-Path (Get-ShimPath "dirtool" $Sandbox) | Should -BeTrue
        Invoke-Shimback @("remove", "-y", "dirtool") | Out-Null
    }
}
