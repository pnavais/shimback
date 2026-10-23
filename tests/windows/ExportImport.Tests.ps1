# Windows Pester counterpart to tests/test_export.sh and tests/test_import.sh
# (leaner): a round trip into a fresh config, --dry-run, conflicts,
# --replace, malformed archives, and how export reports its path.

Describe "export / import" {
    BeforeAll {
        . "$PSScriptRoot\Common.ps1"
        $script:Sandbox = New-ShimbackSandbox
        Enter-ShimbackSandbox $Sandbox
        $script:PS = Get-PowerShellExe
        $script:Backup = Join-Path $Sandbox.Root "backup.sz"

        # Two shims in the sandbox's own config: one in config.toml, one in
        # its own split config file.
        $a = @("add", "ctool", "-s", $PS, "--source-arg", "-NoProfile", "-f", $PS)
        (Invoke-Shimback $a).ExitCode | Should -Be 0
        $a = @("add", "stool", "-s", $PS, "--source-arg", "-NoLogo", "-f", $PS, "--split-config")
        (Invoke-Shimback $a).ExitCode | Should -Be 0
        $script:ExportResult = Invoke-Shimback @("export", "-o", $Backup, "-y")

        # A separate, empty config + shim directory to import into.
        function New-Target([string]$Name) {
            $root = Join-Path $Sandbox.Root $Name
            $t = [pscustomobject]@{
                Env     = @{ XDG_CONFIG_HOME = "$root\cfg"; XDG_DATA_HOME = "$root\data" }
                CfgDir  = "$root\cfg\shimback"
                ShimDir = "$root\data\shimback\bin"
            }
            New-Item -ItemType Directory -Force -Path "$root\cfg", "$root\data" | Out-Null
            $t
        }
    }

    AfterAll {
        Exit-ShimbackSandbox $Sandbox
        Remove-ShimbackSandbox $Sandbox
    }

    It "exports both shims and reports the archive as a quoted, backslash-only path" {
        $ExportResult.ExitCode | Should -Be 0
        Test-Path $Backup | Should -BeTrue
        $ExportResult.StdOut | Should -Match "2 shims, config.toml included"
        $ExportResult.StdOut | Should -Match ([regex]::Escape("`"$Backup`""))
    }

    It "round-trips into a fresh config: config.toml and split shims, with their shim links" {
        $t = New-Target "roundtrip"
        $r = Invoke-Shimback @("import", $Backup, "-y") -EnvOverrides $t.Env
        $r.ExitCode | Should -Be 0
        $r.StdOut | Should -Match "2 imported"
        $list = (Invoke-Shimback @("list") -EnvOverrides $t.Env).StdOut
        $list | Should -Match "ctool"
        $list | Should -Match "stool"
        Test-Path (Join-Path $t.CfgDir "stool-config.toml") | Should -BeTrue
        Test-Path (Join-Path $t.ShimDir "ctool.exe") | Should -BeTrue
        Test-Path (Join-Path $t.ShimDir "stool.exe") | Should -BeTrue
    }

    It "--dry-run reports what it would do and writes nothing" {
        $t = New-Target "dryrun"
        $r = Invoke-Shimback @("import", $Backup, "--dry-run") -EnvOverrides $t.Env
        $r.ExitCode | Should -Be 0
        $r.StdOut | Should -Match "ctool: would import"
        $r.StdOut | Should -Match "nothing written"
        Test-Path (Join-Path $t.CfgDir "config.toml") | Should -BeFalse
        Test-Path (Join-Path $t.CfgDir "stool-config.toml") | Should -BeFalse
    }

    It "leaves a differing local shim alone when the prompt gets no answer" {
        $t = New-Target "conflict"
        $a = @("add", "ctool", "-s", $PS, "--source-arg", "-NoExit", "-f", $PS)
        (Invoke-Shimback $a -EnvOverrides $t.Env).ExitCode | Should -Be 0
        $r = Invoke-Shimback @("import", $Backup) -EnvOverrides $t.Env
        $r.StdOut | Should -Match "Overwrite with the archived version\?"
        $r.StdOut | Should -Match "1 skipped"
        Get-Content -Raw (Join-Path $t.CfgDir "config.toml") | Should -Match "-NoExit"
    }

    It "--replace -y removes a shim that only exists locally" {
        $t = New-Target "replace"
        $a = @("add", "onlylocal", "-s", $PS, "--source-arg", "-NoExit", "-f", $PS)
        (Invoke-Shimback $a -EnvOverrides $t.Env).ExitCode | Should -Be 0
        $r = Invoke-Shimback @("import", $Backup, "--replace", "-y") -EnvOverrides $t.Env
        $r.ExitCode | Should -Be 0
        $r.StdOut | Should -Match "1 removed"
        $list = (Invoke-Shimback @("list") -EnvOverrides $t.Env).StdOut
        $list | Should -Not -Match "onlylocal"
        $list | Should -Match "ctool"
        Test-Path (Join-Path $t.ShimDir "onlylocal.exe") | Should -BeFalse
    }

    It "shows paths in error messages with backslashes only" {
        $missing = (Join-Path $Sandbox.Root "no\such.sz") -replace '\\', '/'
        $r = Invoke-Shimback @("import", $missing)
        $r.ExitCode | Should -Not -Be 0
        $r.StdErr | Should -Match ([regex]::Escape(($missing -replace '/', '\')))
        $r.StdErr | Should -Not -Match ([regex]::Escape("/no/such.sz"))
    }

    It "rejects a file that isn't a zip, changing nothing" {
        $t = New-Target "garbage"
        $garbage = Join-Path $Sandbox.Root "garbage.sz"
        [IO.File]::WriteAllBytes($garbage, [byte[]](1..64))
        $r = Invoke-Shimback @("import", $garbage, "-y") -EnvOverrides $t.Env
        $r.ExitCode | Should -Not -Be 0
        $r.StdErr | Should -Match "does not look like a valid zip"
        Test-Path (Join-Path $t.CfgDir "config.toml") | Should -BeFalse
    }

    It "rejects a zip with no manifest.toml" {
        $t = New-Target "nomanifest"
        $zipPath = Join-Path $Sandbox.Root "nomanifest.sz"
        Add-Type -AssemblyName System.IO.Compression
        $fs = [IO.File]::Open($zipPath, [IO.FileMode]::Create)
        $zip = [IO.Compression.ZipArchive]::new($fs, [IO.Compression.ZipArchiveMode]::Create)
        # Stored, not deflated: shimback only reads stored members, and this
        # test is about the missing manifest, not compression.
        $entry = $zip.CreateEntry("somefile.txt", [IO.Compression.CompressionLevel]::NoCompression)
        $w = [IO.StreamWriter]::new($entry.Open())
        $w.Write("hi")
        $w.Dispose()
        $zip.Dispose()
        $fs.Dispose()
        $r = Invoke-Shimback @("import", $zipPath, "-y") -EnvOverrides $t.Env
        $r.ExitCode | Should -Not -Be 0
        $r.StdErr | Should -Match "has no manifest.toml"
    }
}
