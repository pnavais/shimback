# Windows Pester counterpart to tests/test_help.sh.

Describe "help" {
    BeforeAll {
        . "$PSScriptRoot\Common.ps1"
        $script:Sandbox = New-ShimbackSandbox
        Enter-ShimbackSandbox $Sandbox
    }

    AfterAll {
        Exit-ShimbackSandbox $Sandbox
        Remove-ShimbackSandbox $Sandbox
    }

    It "--help lists usage and every command, exits 0" {
        $r = Invoke-Shimback @("--help")
        $r.ExitCode | Should -Be 0
        $r.StdOut | Should -Match "USAGE:"
        $r.StdOut | Should -Match "COMMANDS:"
        foreach ($cmd in @("add", "remove", "init", "list", "doctor", "install", "uninstall", "edit", "info", "update")) {
            $r.StdOut | Should -Match $cmd
        }
    }

    It "-h is the same as --help" {
        $help = Invoke-Shimback @("--help")
        $h = Invoke-Shimback @("-h")
        $h.StdOut | Should -Be $help.StdOut
    }

    It "per-subcommand --help shows only that command, clap-rs style" {
        $r = Invoke-Shimback @("add", "--help")
        $r.ExitCode | Should -Be 0
        $r.StdOut | Should -Match ([regex]::Escape("shimback add <name>"))
        $r.StdOut | Should -Match "Create or update a shim"
        $r.StdOut | Should -Not -Match ([regex]::Escape("shimback remove ["))
    }

    It "aliases show their primary command's help" {
        $remove = Invoke-Shimback @("remove", "--help")
        $rm = Invoke-Shimback @("rm", "--help")
        $rm.StdOut | Should -Be $remove.StdOut
    }

    It "--help short-circuits no matter where it appears among a command's own args" {
        $r = Invoke-Shimback @("doctor", "fix", "-y", "-h")
        $r.ExitCode | Should -Be 0
        $r.StdOut | Should -Match ([regex]::Escape("shimback doctor [fix"))
    }

    It "an unrecognized command isn't rescued by a trailing --help" {
        $r = Invoke-Shimback @("bogus", "--help")
        $r.ExitCode | Should -Not -Be 0
        $r.StdErr | Should -Match "unknown command 'bogus'"
    }
}
