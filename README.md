# shimback

[![Build](https://github.com/pnavais/shimback/actions/workflows/release.yml/badge.svg)](https://github.com/pnavais/shimback/actions/workflows/release.yml) [![Version](https://img.shields.io/github/v/release/pnavais/shimback?label=version)](https://github.com/pnavais/shimback/releases/latest)

![shimback banner](assets/banner.png)

`shimback` is a small, dependency-free command-line shim: it wraps a command
name (e.g. `sed`) with a **source** binary to run and a **fallback** binary
to transparently retry with if the source doesn't work out. It was born out
of a very concrete annoyance: on a Nix-managed macOS system, GNU `sed` often
ends up ahead of BSD `/usr/bin/sed` on `PATH`, and scripts written for one
dialect's argument style (`sed -i ''` vs `sed -i`) break under the other.
`shimback` generalizes that "try one, fall back to the other" idea to any
pair of commands.

## Contents

- [Installation](#installation)
- [How it works](#how-it-works)
- [Usage](#usage)
- [Fallback policies](#fallback-policies)
- [Configuration](#configuration)
- [Exit codes](#exit-codes)
- [Building](#building)
- [Platform support](#platform-support)
- [License](#license)

## Installation

**Linux / macOS / WSL:**

```sh
curl -fsSL https://raw.githubusercontent.com/pnavais/shimback/main/install.sh | sh
```

**PowerShell (Windows):**

```powershell
irm https://raw.githubusercontent.com/pnavais/shimback/main/install.ps1 | iex
```

Downloads the right prebuilt binary for your machine from the
[latest release](https://github.com/pnavais/shimback/releases/latest) and
hands it to `shimback install` (see [`install`](#install), below), which
copies itself to a stable location, sets up `PATH`, and installs the man
page (Windows: no man page, see [Platform support](#platform-support)).
Extra arguments are forwarded as-is, e.g. to pick a prefix:

```sh
curl -fsSL https://raw.githubusercontent.com/pnavais/shimback/main/install.sh | sh -s -- --prefix ~/.local
```

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/pnavais/shimback/main/install.ps1))) --prefix C:\tools
```

(PowerShell's `iex` alone can't forward arguments to a piped script the
way `sh -s --` can — the `scriptblock`/`&` form above is the equivalent.)

If shimback is already installed, that command changes nothing (`install`
allows a single installation) — upgrade with [`shimback update`](#update).

Or download a prebuilt binary directly from the
[latest release](https://github.com/pnavais/shimback/releases/latest):

| Platform | Download |
|---|---|
| macOS (Apple Silicon) | [shimback-macos-arm64.tar.gz](https://github.com/pnavais/shimback/releases/latest/download/shimback-macos-arm64.tar.gz) |
| macOS (Intel) | [shimback-macos-x86_64.tar.gz](https://github.com/pnavais/shimback/releases/latest/download/shimback-macos-x86_64.tar.gz) |
| Linux (x86_64) | [shimback-linux-x86_64.tar.gz](https://github.com/pnavais/shimback/releases/latest/download/shimback-linux-x86_64.tar.gz) |
| Linux (arm64) | [shimback-linux-arm64.tar.gz](https://github.com/pnavais/shimback/releases/latest/download/shimback-linux-arm64.tar.gz) |
| Windows (x86_64) | [shimback-windows-x86_64.zip](https://github.com/pnavais/shimback/releases/latest/download/shimback-windows-x86_64.zip) |

Each archive contains the `shimback` binary, the license files, and this
README (macOS/Linux also get the man page; Windows doesn't ship one, see
[Platform support](#platform-support)). Extract it and either run
`./shimback install` (`.\shimback.exe install` on Windows — same as the
one-liners above) or place the binary wherever you like on `PATH`
yourself — see [Building](#building) for the caveat on doing that
manually. Building from source works the same way on any other platform;
see [Building](#building) below.

## How it works

1. `shimback add <name> ...` creates a symlink named `<name>` pointing at the
   `shimback` binary itself, inside a dedicated shim directory that's
   prepended to your `PATH`.
2. When you run `<name>`, the OS finds that symlink first. `shimback`
   inspects `argv[0]`, sees it was invoked as `<name>` rather than
   `shimback`, looks up `<name>`'s configuration, and runs the **source**
   command with your arguments.
3. If the source command fails (exact condition depends on the shim's
   **policy** — see below), `shimback` transparently re-runs the same
   arguments against the **fallback** command instead. If the source
   succeeds, its output is passed through as if `shimback` weren't there at
   all. (`route-args`, `split-args`, `rewrite`, and `route-map` are
   exceptions to this "try source, maybe fall back" shape, and `passthrough`
   is an exception to the "invisible trial run" below — see below.)

A failed trial run of the source command is designed to be **invisible**:
its stdout and stderr are captured, not streamed live, and are discarded
entirely if a fallback is triggered — nothing about the failed attempt
reaches your terminal unless the shim explicitly opts into a one-line
diagnostic (see `diagnostic` below).

This is built for **fast-failing commands** — a bad flag, a missing file, a
GNU/BSD mismatch — which almost always fail in well under a second with a
small amount of output. Capturing has to hold everything in memory until the
source exits, since there's no way to know whether to show or hide it any
sooner than that; to keep that bounded, shimback gives up on hiding/falling
back for a given run if the trial takes too long or produces too much
output — whichever happens first — and switches to relaying the rest live
instead. Nothing already produced is ever lost (the switch just flushes what
was captured so far and streams the rest as it arrives); the only thing
that's lost is the *opportunity* to fall back for that one invocation, since
some of the source's real output is already on screen by then. This means a
long-running command (a dev server, a slow build) or one that unexpectedly
emits a lot of output still behaves reasonably — you just see it live,
un-hidden, rather than staring at nothing until it eventually exits. The two
thresholds (2 seconds, 8MiB by default) are configurable, globally and per
shim — see `--capture-timeout`/`--capture-limit` below.

## Usage

```
shimback add <name> [-s <source>] [--source-arg <arg>]...
                     -f <fallback> [--fallback-arg <arg>]...
                     [--policy exit-code|heuristic|exit-code-match|route-args|rewrite|split-args|route-map|passthrough]
                     [--error-pattern <p>]... [--exit-code <code>]...
                     [--route-arg <arg>]... [--strip-matched-args]
                     [--split-source-arg <arg>]... [--split-fallback-arg <arg>]...
                     [--rewrite <from>=<to>]... [--route <match>=<command>]...
                     [--diagnostic] [--force] [-v|--verbose]
                     [--capture-timeout <ms>] [--capture-limit <size>] [--split-config]
shimback remove [-y] <name>
shimback init
shimback list [--full]
shimback doctor [fix [-y|--yes]]
shimback install [--prefix <dir>] [--force] [--shell <shell>[,<shell>]... | --all]
shimback update [--check]
shimback uninstall [--prefix <dir>] [--full]
shimback edit [<name>]
shimback info <name>
shimback --help | --version
```

`--help` (and the usage printed on a missing/unknown command) uses styled,
colored help when stdout is a terminal and `NO_COLOR` isn't set: section
headers bold yellow, commands and flags bold green,
placeholders (`<name>`, `<fallback>`, …) cyan. Every command also has its
own scoped help — `shimback add --help` (or `-h`) shows just `add`'s usage
and description instead of the full list, the same way clap-rs's
generated subcommand help does; it works no matter where `-h`/`--help`
appears among that command's own arguments (`shimback doctor fix -y -h`
works the same as `shimback doctor --help`), and an alias (`rm`, `ls`)
shows its target command's help.

### `add`

```sh
shimback add sed -f /usr/bin/sed
```

- `<name>` is the command name to shim (e.g. `sed`). It can't contain `/`
  and can't be `shimback` itself.
- `-s`/`--source` is optional. If given, it's resolved **once**, at `add`
  time, and frozen in the config as an absolute path — re-run `add` if that
  binary moves. If omitted, the source is resolved fresh from `PATH` on
  **every invocation**, skipping shimback's own shim directory (and
  anything that resolves back to the `shimback` binary itself), so it
  naturally follows whatever the "real" `<name>` on your system currently
  is.
- `-f`/`--fallback` is required (except with `--policy rewrite` or
  `--policy route-map`, neither of which ever uses it), and is resolved the
  same way `-s` is: once, at `add` time, frozen as an absolute path.
- Both `-s` and `-f` accept either a path (`/usr/local/bin/eza`,
  `./eza`) or a bare command name (`eza`) — a bare name with no `/` is
  looked up on `PATH` (skipping nothing, unlike auto-resolved `-s`) exactly
  once, the same way a shell would find it, and that resolved path is what
  gets stored.
- `--source-arg <arg>`/`--fallback-arg <arg>` (each repeatable) attach
  fixed, baked-in arguments to source/fallback, independent of policy and
  independent of each other — whichever one actually runs gets its own
  extra arguments prepended *before* whatever the shim was invoked with,
  every time. This is what lets a shim double as a regular alias with
  flags built in, on top of whatever its policy already does:

  ```sh
  shimback add cools -s /bin/ls --source-arg -l --source-arg -t \
      --source-arg -r --source-arg -a --source-arg -h -f /usr/local/bin/eza
  ```

  Running `cools` runs `ls -l -t -r -a -h`; `cools somedir` runs
  `ls -l -t -r -a -h somedir` — the same shape as a shell alias like
  `alias cools='ls -ltrah'`, just resolved once at `add` time like
  everything else here. Under `--policy rewrite`, `source_args` land
  before the (possibly rewritten) invocation arguments, and are never
  themselves subject to rewriting. `--fallback-arg` has nothing to attach
  to without a fallback (only possible under `--policy rewrite` or
  `--policy route-map`, the two policies where `-f`/`--fallback` is
  optional) — given without one, it's
  discarded with a yellow warning rather than kept around uselessly or
  treated as a hard failure. The [wizard](#interactive-wizard) goes a step
  further and just never offers the fallback-args page at all when
  fallback is left blank.
- `add` refuses to create a shim where source and fallback resolve to the
  same binary *with the same extra arguments* (nothing would ever behave
  differently), and never writes anything if validation fails. Same binary
  with **different** `--source-arg`/`--fallback-arg` is fine — that's two
  distinct invocations of one command, not a no-op.
- Because a bare `-s`/`-f` name is looked up on `PATH` with nothing
  excluded, it can resolve to another shim's symlink — and every shim
  symlink points at the same `shimback` binary, so that's indistinguishable
  from pointing at `shimback` directly. `add` refuses this too: a source or
  fallback that resolves back to the `shimback` binary itself would loop
  forever the moment the shim actually ran. `shimback doctor` checks for
  this as well (in case a cycle ever ends up in a hand-edited config), and
  `shimback doctor fix` repairs it by prompting for a replacement — see
  below.
- `--force` allows `-s`/`-f` to name a **path** (something containing `/`) that
  doesn't exist yet, instead of dying with "does not exist, is not
  executable, or isn't on $PATH" — useful for configuring a shim ahead of
  installing the thing it points at. It's stored per-shim in `config.toml`
  (`force = true`) and, once set, `shimback doctor` skips its existence
  check for whichever of source/fallback is *still* missing, reporting it
  as fine rather than broken — as soon as either one actually exists,
  `doctor` goes back to checking it normally (cycle check included), `force`
  or not. `force` only affects that one `doctor` check: dispatch always
  resolves and checks source/fallback for real every time the shim
  actually runs, regardless of `force`. Since there's nothing to search
  `PATH` for something that doesn't exist anywhere yet, `--force` requires
  a path, not a bare command name.
- `add` also ensures the shim directory is on `PATH`, by injecting an
  idempotent, clearly marked block into your current shell's startup file
  (detected from `$SHELL`) — or, on fish, a dedicated snippet file instead
  (see `init`, below, for why). Re-running `add` never duplicates this.
  On zsh, if `~/.zshrc.local` exists, the block goes there instead of
  `~/.zshrc` — most zsh setups source it for machine-local overrides kept
  out of a dotfiles repo, so that's the more appropriate place for it; a
  later `uninstall` checks both files, regardless of which one
  currently exists. If [`zsh-defer`](https://github.com/romkatv/zsh-defer)
  is available, the injected block routes its `export PATH=` through it
  too — otherwise tools like `mise` or `direnv` that defer their own
  PATH-mutating activation (for faster prompt startup) would clobber the
  shim dir's position on `PATH` after the rc file finishes sourcing,
  regardless of where the shimback block sits in the file. It's queued as
  `zsh-defer -c 'export PATH="<dirs>:$PATH"'`, so `$PATH` is expanded when
  the deferred command actually runs rather than when it's queued —
  otherwise it would overwrite whatever those tools' own deferred
  activation added in between. Without `zsh-defer`, it's a plain `export`.
- The confirmation line printed on success (`shimback: 'name' -> path
  (fallback: ..., policy: ...)`) is colored when stdout is a terminal and
  `NO_COLOR` isn't set, matching `list`'s conventions. The shell-startup-file
  notices right after it (`zsh: PATH updated in ...`, `Restart your shell
  ...`) are **silent by default** — pass `-v`/`--verbose` to print them for
  this one invocation, or set `verbose = true` at the top level of
  `config.toml` (see [Configuration](#configuration)) to make that the
  default for every `add`. `-v`/`--verbose` only ever turns them on; it
  can't override a config default of `true` back to silent for one call.
- `--capture-timeout <ms>` and `--capture-limit <size>` override, for this
  shim only, the two thresholds that decide when a trial run gives up on
  being hideable (see [How it works](#how-it-works)): default 2000ms /
  8MiB, both also configurable as global defaults at the top level of
  `config.toml`. A shim-level value always wins over the global one.
  `--capture-limit` accepts a plain byte count or a size with a
  case-insensitive unit suffix — `B` (or nothing) for bytes, `K`/`KB` for
  decimal kilobytes (×1000), `KiB` for binary kibibytes (×1024), and
  likewise `M`/`MB`/`MiB` and `G`/`GB`/`GiB` — so `--capture-limit 8MiB`,
  `--capture-limit 8192KiB`, and `--capture-limit 8388608` all mean the
  same thing.
- `--split-config` saves this shim's settings in their own file instead of
  as a `[shims.<name>]` entry inside `config.toml` — see
  [Splitting a shim's config into its own file](#splitting-a-shims-config-into-its-own-file)
  below.

#### Splitting a shim's config into its own file

By default, every shim's settings live as a `[shims.<name>]` table inside
the single shared `config.toml`. If that file is getting unwieldy — many
shims, or one with a lot of policy-specific configuration — `add
--split-config` instead writes just that shim's settings, as bare
`key = value` lines with no section header, to a file named
`<name>-config.toml`:

```sh
shimback add sed -f /usr/bin/sed --split-config
```

This writes `sed-config.toml` into shimback's config directory (next to
`config.toml` itself) and, unlike a normal `add`, leaves no
`[shims.sed]` entry in `config.toml` at all — the split file is now the
*only* place `sed`'s settings live.

You can freely move that file to either of two other locations, and
shimback still finds it — every shim's effective config is resolved by
checking, in this order, for a `<name>-config.toml`:

1. next to the shim's own symlink (the shim directory reported by
   `shimback doctor`, e.g. `~/.local/share/shimback/bin/sed-config.toml`);
2. next to the `shimback` binary itself (e.g.
   `~/.local/bin/sed-config.toml`);
3. in the shared config directory, where `add --split-config` writes it
   initially (e.g. `~/.config/shimback/sed-config.toml`).

The first one found wins outright — it's used *instead of* `config.toml`,
not merged with it, so moving a copy to a more specific location (say, to
travel alongside the symlink itself) is enough to override the one
`add --split-config` originally wrote, with nothing to delete. A
`<name>-config.toml` can also exist with no `[shims.<name>]` entry in
`config.toml` at all, and vice versa; whichever a shim currently has wins,
independent of how it got there.

`remove` and `uninstall --full` both sweep a shim's split file from every
one of those three locations, not just wherever it currently resolves
from — so nothing is ever left behind by moving one around. Re-running
`add` on a shim *without* `--split-config` folds it back into
`config.toml`, removing whichever split file(s) it finds; re-running it
*with* `--split-config` does the reverse, removing the `config.toml`
entry. The [interactive wizard](#interactive-wizard) asks about this too,
as its very last page.

#### Interactive wizard

Run `add` at an interactive terminal with required information missing
(no name at all, or a name but no fallback, or a policy that needs
`--error-pattern`/`--exit-code`/`--route-arg`/(`--split-source-arg` and
`--split-fallback-arg`)/`--rewrite`/`--route` and doesn't have it) and,
instead of failing, a small step-by-step wizard walks you through filling
it in: name, policy (picked from a list), source, source's extra args,
fallback, fallback's extra args, then whatever the chosen policy still
needs (for `split-args`, that's two required list pages, one per side; for
`route-map`, that's one or more `<match>=<command>` route pairs), then the
diagnostic flag, and finally whether to
[split its config into its own file](#splitting-a-shims-config-into-its-own-file).
The two extra-args pages are optional list pages, unlike
the policy-specific ones (which, like `--route-arg`/`--split-source-arg`/
`--split-fallback-arg` themselves, need at least one entry to finish) —
blank Enter finishes an optional list with zero items just as happily as
with several — and the fallback-args page is skipped entirely (not just
left blank) when fallback itself was left blank, since there'd be nothing
for it to attach to. Whatever was already given on the command line (e.g.
`shimback add mytool -s /bin/ls`) is skipped straight past — the wizard
starts right at the first page that's actually missing (`fallback`, in
that example) — but every earlier page, including the ones filled in from
the command line, is still reachable and editable.

- **Enter** confirms the current page and moves to the next; on an optional
  field (source, or fallback under `--policy rewrite`/`--policy route-map`),
  pressing it with nothing typed just skips that field.
- **Left/Right arrows** move between pages. Left always goes back one page.
  Right only moves forward through pages you've already committed in this
  session — it can't skip ahead into territory you haven't reached yet
  ("forth" means the last page you'd gotten to, not further).
- Changing the **policy** after having already gone further resets
  everything after it (source, its extra args, fallback, its extra args,
  and whatever that policy's own page had collected), since a different
  policy needs different follow-up pages — you just re-enter them.
- **Esc** or **Ctrl-C** aborts at any point; nothing is written (no
  symlink, no config entry) unless the wizard runs all the way through.

### `remove` (alias: `rm`)

```sh
shimback remove sed
```

Removes the symlink and the config entry for `<name>` — including a
[split `<name>-config.toml` file](#splitting-a-shims-config-into-its-own-file),
if the shim has one, swept from all three of its potential locations. It
does **not** touch the PATH injection in your shell's startup file, since
other shims (or a future `add`) may still need it.

If `<name>` doesn't match any configured shim, shimback prints the error
first, then, on its own line right after, a `did you mean 'X'?` hint (in
yellow, when stderr is a terminal and `NO_COLOR` isn't set) for whichever
configured shim name is closest by edit (Levenshtein) distance — the
minimum number of single-character insertions, deletions, or
substitutions needed to turn one string into the other — as long as that
distance is small relative to the name's length (roughly one typo per
three characters), so an unrelated name never gets suggested. The same
hint appears for an unrecognized top-level command (e.g. `shimback dctor`
→ `did you mean 'doctor'?`). No external tool is involved, and it catches
any single-character typo shape — a dropped, inserted, or substituted
letter (`shed` → `sed`, not just `doctr` → `doctor`) — unlike a
subsequence-only fuzzy match (e.g. `fzf --filter`), which can only catch a
typo that's literally containable, in order, within the real name.

Pass `-y`/`--yes` to skip the hint and act on it automatically — but only
when it's *unambiguous*: if exactly one configured name is the closest
match, that one gets removed instead, with the usual `removed 'X'`
confirmation (in green, under the same terminal/`NO_COLOR` conditions)
naming whichever shim actually got removed; if two or more configured
names tie for closest, `-y` doesn't guess between them — it falls back to
the plain error and hint, exactly as without the flag.

```sh
shimback remove -y shed   # -> "removed 'sed'"
```

### `init`

```sh
shimback init
```

Detects installed shells (zsh, bash, and fish) and injects the PATH setup
into each one — useful for setting things up across every shell you have
installed, rather than just your current one. For zsh/bash that's the same
marker-block injection `add` does (see above); for fish, which has its own
different-enough PATH mechanism, it's a dedicated, shimback-owned snippet
dropped into `~/.config/fish/conf.d/shimback.fish` (auto-sourced by fish at
startup, so no editing of `config.fish` itself is needed):

```fish
# Managed by shimback -- changes here will be overwritten.
set -gx PATH /path/to/shim/dir $PATH
```

Like the zsh/bash block, this snippet is unioned rather than overwritten
across repeated `add`/`init`/`install` calls, and removed outright (the
whole file) by `uninstall --full`.

### `list` (alias: `ls`)

```sh
shimback list
```

Prints every shim's name, source (or `auto`), fallback, policy, and
diagnostic flag as a column-aligned table. The fallback column shows just
its binary name, not the full path -- `--full` (below) prints the full
path back out, alongside everything else the compact columns leave out.
This includes a
[split-config](#splitting-a-shims-config-into-its-own-file) shim exactly
like a config.toml one (its own file's path shown as a `config:` line
under `--full`, so you always know exactly where it currently lives),
and a real shim symlink with no configuration anywhere for it at all --
an **orphan**, shown with an explanatory message in place of its row's
usual columns instead of being silently left out (see `doctor`, below,
for removing one):

```
NAME  SOURCE                   FALLBACK  POLICY     DIAGNOSTIC
sed   auto                     sed       exit-code  false
awk   /opt/homebrew/bin/gawk   awk       heuristic  true
```

When stdout is a terminal (and [`NO_COLOR`](https://no-color.org/) isn't
set): shim names are bold cyan; an explicit source is green and `auto` is
dimmed; the fallback name is blue; the policy column is colored by kind
(`heuristic` yellow, `exit-code-match` magenta, `route-args` cyan,
`rewrite` green, `split-args` red, `route-map` blue, `passthrough` bold
magenta, `exit-code` uncolored as the baseline); `false`
diagnostics are dimmed and `true` ones are green; column headers are bold
yellow. Piping the output (e.g. to a file or another command) disables
color automatically.

`shimback list --full` (or `ls --full`) additionally prints whatever the
table's columns leave out, as extra indented lines right under a shim's
row: the shim symlink's own path (always shown, for every shim -- an
orphan's row gets this too, even though it has no other detail to show),
its [split config file](#splitting-a-shims-config-into-its-own-file)'s
path if it has one, what an `auto` source currently resolves to on
`$PATH` (or that nothing does), the fallback's own full path (when one is
configured -- unused-by-policy shims like `rewrite`/`route-map` without
`-f` show none of this), `source_args`/`fallback_args` (see `add`, above
— independent of policy, so these can show up for any shim), then
whatever's policy-specific — `error_patterns` for `heuristic`, the
configured codes for `exit-code-match`, `route_args` and
`strip_matched_args` for `route-args`,
`source_route_args`/`fallback_route_args`/`strip_matched_args` for
`split-args`, each `<from> -> <to>` pair for `rewrite`, and, for
`route-map`, every route's own resolved command (plus its baked-in
`args`, if any -- each route can point at a genuinely different binary,
which `--full` shows in full, not just its `<match>` token). A shim with
none of the policy-specific extras configured still gets its symlink
line, just nothing more:

```
NAME   SOURCE     FALLBACK  POLICY           DIAGNOSTIC
sed    auto       sed       exit-code        false
        symlink: /Users/you/.local/share/shimback/bin/sed
        source resolves to: /usr/bin/sed
        fallback: /usr/bin/sed
awk    /opt/.../gawk   awk  heuristic        true
        symlink: /Users/you/.local/share/shimback/bin/awk
        fallback: /usr/bin/awk
        error patterns: invalid option, illegal option
ll     /bin/ls    eza       exit-code        false
        symlink: /Users/you/.local/share/shimback/bin/ll
        fallback: /usr/local/bin/eza
        source args: -l, -t, -r, -a, -h
        fallback args: -la
java   /opt/java17/bin/java  none  route-map  false
        symlink: /Users/you/.local/share/shimback/bin/java
        route '--v8': /opt/java8/bin/java
        route '--v25': /opt/java25/bin/java
        route '--preview': /opt/java25/bin/java --enable-preview
        strip matched args: false
```

### `doctor`

```sh
shimback doctor [fix [-y|--yes]]
```

Checks the health of your whole shimback setup and reports any problems:
whether the shim directory exists and is actually on `$PATH`, whether the
config file parses, and, for every real shim — whether it's backed by a
config.toml entry or its own
[split config file](#splitting-a-shims-config-into-its-own-file) — whether
its symlink exists and isn't dead, whether its fallback (and, if explicit,
its source) still exist and are executable — for `route-map`, every
route's `command` gets this same check — and whether its policy is
fully configured (e.g. `heuristic` with no `--error-pattern`, or
`exit-code-match` with no `--exit-code`, can never fall back). A real shim
symlink with **no** configuration anywhere for it — an orphan, typically
left behind after its config.toml entry or split file was deleted by hand
— is reported too, rather than silently skipped (see below for removing
one). Exits `0` if everything checks out, `1` otherwise — safe to run in
CI or a shell startup hook. Section headers and shim names are bold
yellow/cyan, `[ok]`/`[fail]` are green/red, and the closing summary line
is green or red, when stdout is a terminal.

`shimback doctor fix` repairs what it safely can before reporting: a missing
shim directory (with shims still configured) is created first, the same
thing `init` does, so there's no need to run `init` and then `doctor fix`;
then any shim whose symlink is missing entirely, or is a *dangling* symlink (its target no
longer exists — e.g. because the binary it pointed at, back when `add` ran,
has since moved or been cleaned up), gets its symlink recreated pointing at
the currently running `shimback` binary, the same way `add` creates it in
the first place. It never touches a symlink that still resolves (even to a
different-but-valid `shimback` binary elsewhere) or a path occupied by
anything other than a symlink — only genuinely dead or missing entries.

It also checks every shim's source (if explicit) and fallback (and, for
`route-map`, every route's `command`) for a **cycle**: a value that
resolves back to the `shimback` binary itself (see `add`, above). `add`
refuses to create one, so the only way a cycle ends up in the config is by
hand-editing it. When `fix` finds one, it prompts
interactively for a replacement, right there in the terminal — and, for a
split-config shim, saves the fix back to its own split file, never to
config.toml:

```
cyc
  fallback '/Users/you/.local/bin/shimback' for shim 'cyc' resolves back to the shimback binary itself (a cycle).
  Enter a corrected fallback (Ctrl+C to abort):
```

Whatever you type is resolved and re-checked the same way `add` would; an
invalid answer, or another cycle, just re-prompts — it keeps asking until
you give it something that works, or until you press **Ctrl+C** to abort
the whole `doctor fix` run (the normal way a terminal handles that signal;
nothing shimback-specific). If stdin isn't interactive (e.g. run from a
script) and hits EOF before you've answered, it gives up on that one entry
and leaves it as-is — `doctor fix` never hangs waiting for input that can't
arrive.

For an **orphan** (a real shim symlink with no config.toml entry or split
config file anywhere for it), `fix` prompts before removing its symlink,
the same shape as the cycle prompt above:

```
mytool
  'mytool' has a real shim symlink but no configuration anywhere for it (no
  config.toml entry, no split config file). Remove the symlink? [y/N]
```

Pass `-y`/`--yes` (`shimback doctor fix -y`) to remove every orphan found
without asking — useful for a script or CI job where nothing can answer an
interactive prompt. It only affects orphan removal; a cycle still always
prompts (or gives up on EOF, as above) regardless of `-y`.

`doctor` also reports on the [installation](#install) recorded in the `PATH`
block: which binary it's installed at, or that shimback isn't installed. Two
things get a `[warn]` (which, like the stray-block warning below, doesn't
change `doctor`'s exit code): more than one installation recorded (only
possible from a version that predates `install`'s single-installation rule),
and a recorded directory with no shimback binary in it any more (a stale
entry, harmless). Both come with what to do about them.

`doctor` also checks for a stray PATH block: if `~/.zshrc.local` exists but
the shimback block is still sitting in `~/.zshrc` (written by an `add`/
`init`/`install` that ran before `~/.zshrc.local` existed, or before
shimback preferred it — see `add`, above), it's flagged as a `[warn]`, not
a `[fail]` — it doesn't affect `doctor`'s exit code, since PATH still works
fine either way. `doctor fix` acts on it by moving the block over.

### `install`

```sh
shimback install [--prefix <dir>] [--force] [--shell <shell>[,<shell>]... | --all]
```

Copies the running `shimback` binary to `<prefix>/bin/shimback` (default
prefix: `~/.local`) and ensures both `<prefix>/bin` **and** the shim
directory are on `PATH`, using the same idempotent marker-block injection
`add`/`init` use for the shim directory — and merged into that *same*
block, not a second one: whichever of `add`/`init`/`install` runs unions
its own directory into whatever's already there, in any order, so you end
up with one `# >>> shimback >>>` block listing every directory shimback
needs, however many of these commands you've run. This means a completely
fresh `install`, before ever running `add`, still leaves you with a working
`PATH`. This is the easiest way to get `shimback` itself onto a **stable**
location: `add` freezes the path of whatever binary is currently running
into each shim's symlink (see below), so running it straight out of a build
directory means every shim breaks the next time that directory is cleaned
or rebuilt. It also migrates away an old separate `shimback-bin` block if
v0.1.0 ever left you with one.

**Only one installation is allowed.** `install` looks for an existing one
(a directory recorded in the `PATH` block that really holds a shimback
binary — a stale entry whose binary was deleted doesn't count) before
writing anything:

- Already installed at this same prefix: it prints a yellow warning and does
  nothing (exit `0`). To *upgrade* from a release, use
  [`update`](#update); to overwrite the installed copy with the binary you're
  running — e.g. after a local rebuild — pass `--force`.
- Already installed at a *different* prefix: it refuses (exit `1`), naming
  the existing one; run `shimback uninstall` first to move it. `--force`
  never creates a second installation: it only overwrites the one at the same
  `--prefix`.

Since `--force` is also how you re-run `install` for the same prefix, it's
how you add `PATH` setup for another shell after the first install (e.g.
`shimback install --force --shell bash`).

By default, `install` only sets up `PATH` for your **current** shell (like
`add` does), not every shell you have. Pass `--shell` with a comma-separated
list (`--shell zsh,bash,fish`) to target specific shells regardless of which
you're running, or `--all` for every shell `init` would detect as installed.

`install` also installs this man page to `<prefix>/share/man/man1/shimback.1`.
It first looks for a `shimback.1` bundled next to the running binary — how
each [release](https://github.com/pnavais/shimback/releases) tarball ships
it, so the common "download a release, run install" path never touches the
network for this. If it can't find one there (e.g. built from source
directly), it falls back to downloading it from the GitHub release matching
the running version, via `curl` — one of only two places shimback shells out
to something it didn't build (the other is [`update`](#update)), and the
reason it isn't unconditionally "dependency-free": if `curl` isn't on `PATH`,
or the download fails, this step is skipped with a warning and `install`
still succeeds at its main job of getting the binary in place. `install`
never checks GitHub for a newer release — that's `update`.

### `update`

```sh
shimback update
shimback update --check
shimback update -y   # or --yes: don't prompt (see below)
```

Replaces the installed `shimback` with the latest GitHub release. It finds
the installation from the `PATH` block (so it needs a prior `install`),
downloads this platform's release archive and its `SHA256SUMS`, verifies the
archive's SHA-256 against it (refusing anything unverifiable or mismatched,
and never touching the installed binary in that case), then swaps in the new
binary — atomically, at the same path — and refreshes the man page alongside
it (Windows: no man page, see [Platform support](#platform-support)). It
compares the *contents* of the downloaded binary with the installed one
rather than version numbers, so a re-tagged release is picked up too, and an
identical one reports "already up to date". `--check` only reports whether
an update is available.

On macOS/Linux every shim symlink keeps working automatically, since a
symlink always resolves to whatever's at the target path *now*. On Windows,
a shim can be left pointing at the *old* binary (a hard link survives the
swap, or the shim is a plain copy to begin with — see [`add`](#add)):
`update` detects this by comparing each shim's own contents against the
newly installed binary, lists any that are now stale, and offers to refresh
them (recreating each one the same way `add` would, hard link first, falling
back to a copy). `-y`/`--yes` skips that prompt — for unattended/scripted
runs, same as `doctor fix -y`.

Like `install`'s man page fallback, this shells out to `curl` (and, on
macOS/Linux, `tar`; Windows uses PowerShell's `Expand-Archive` instead,
already built in) — a dependency-free C binary can't do HTTPS itself. The
checksum is computed natively, so `shasum`/`sha256sum` aren't needed.

**Trust model.** The default location is GitHub's `latest/download` URL,
fetched with `curl` restricted to HTTPS (including through redirects), so the
trust anchor is HTTPS to `github.com` — the same one `install.sh` relies on.
The `SHA256SUMS` comes from the same place as the archive, so the checksum
catches corruption, truncation, and a mismatched archive/checksum pair; it
does **not** authenticate the location itself (releases aren't signed). Set
`SHIMBACK_RELEASE_URL` to fetch from somewhere else (a mirror, or a
`file://` directory of release assets); that's an explicit decision to trust
that location, and `update` says so with a warning each time it's used.

### `uninstall`

```sh
shimback uninstall [--prefix <dir>] [--full]
```

Removes every symlink `shimback` created in the shim directory (dangling or
not — nothing else should ever live there), the installed `shimback` binary
and its man page, and the `PATH` marker block in shell startup files. The
installation is found from that block, so a custom `--prefix` used at
`install` time doesn't need repeating; pass `--prefix` only to override the
lookup (with none found and none given, it falls back to the default,
`~/.local`). The config file is left alone by default — that's your data,
and a future `add`/`init`/`install` just picks up where things left off; pass
`--full` to also delete it and sweep every shim's
[split `<name>-config.toml` file](#splitting-a-shims-config-into-its-own-file)
(including one that only ever existed that way, with no `config.toml`
entry at all) — a complete teardown. The one time the `PATH` block is kept:
if another installation (only possible from a version that predates the
single-installation rule) is still recorded in it. Safe to re-run: nothing
left to remove is just reported as already gone.

`uninstall` identifies its own binary and shim symlink targets by
statically checking for a marker embedded in every shimback build — a
best-effort identification hint, not cryptographic proof of ownership.
It never executes a candidate file to ask what it is.

### `edit`

```sh
shimback edit
shimback edit sed
```

Opens `config.toml` in `$EDITOR`, or, if that's unset (or empty), the first
of `nvim`, `vim`, `vi`, `nano`, `pico` found on `PATH` — in that order.
Fails with a clear error if `$EDITOR` isn't set and none of those five are
found either. A multi-word `$EDITOR` (e.g. `EDITOR="code --wait"`) works as
expected — it's run through a shell so its own flags are honored, not
treated as part of a single literal command name. The config's directory is
created first if it doesn't exist yet, so editing works even before the
first `add`. Once the editor exits, if it exited successfully but the file
it left behind no longer parses, shimback warns about that right away
(without failing the command) rather than letting the next `add`/`list`/
`doctor` surface a confusing error far removed from the edit that caused it.

Given a shim's name (`shimback edit sed`), it opens whichever file actually
defines that shim instead: its
[split config file](#splitting-a-shims-config-into-its-own-file) if it has
one (the same one dispatch would use, wherever it currently lives among the
three locations), otherwise `config.toml`. A name that isn't configured
anywhere fails with a "no shim configured" error (plus a typo suggestion if
one's close), rather than opening a file that has nothing to do with it.
A malformed split file can still be opened this way to fix it.

### `info`

```sh
shimback info java
```

Prints everything shimback knows about one shim, in sections, ending with an
ASCII diagram of how an invocation actually flows through it (colored when
stdout is a terminal, plain ASCII otherwise -- no box-drawing characters, so
it survives any terminal or log file):

```
shim: java (route-map)

Overview
  policy         route-map
                 Routes to any number of commands, based on the invocation's arguments.
  diagnostic     off
  force          no

Locations
  symlink        /Users/you/.local/share/shimback/bin/java  [ok]
                 -> /Users/you/.local/bin/shimback  (shimback binary)
  on $PATH       [ok] typing 'java' runs this shim (first match on $PATH)
  config         /Users/you/.config/shimback/config.toml
                 the [shims.java] entry in config.toml

Commands
  source         /opt/java17/bin/java (explicit, frozen at add time) [ok]
  source args    (none)
  fallback       none (not used by this policy)

Policy settings
  routes         (checked in order; the first match wins)
                 1. --v8      -> /opt/java8/bin/java [ok]
                 2. --v25     -> /opt/java25/bin/java [ok]
                 3. --preview -> /opt/java25/bin/java [ok]
                                with args: --enable-preview
  strip matched  yes

Trial run
  captured       no -- this policy runs its target directly, with live output (the limits below don't apply)

Flow
  $ java <args>
    |
    |  the shell finds the shim's symlink first on $PATH
    v
  /Users/you/.local/share/shimback/bin/java (symlink)
    |
    |  which is just shimback, started under the name 'java'
    v
  shimback  policy: route-map
    |
    v
  find the first route whose match equals one of the arguments:
    |
    +-- --v8       --> /opt/java8/bin/java <args>
    +-- --v25      --> /opt/java25/bin/java <args>
    +-- --preview  --> /opt/java25/bin/java --enable-preview <args>
    |
    `-- (no match) --> SOURCE /opt/java17/bin/java <args>

  (the chosen one runs directly: live output, no trial run, no retry)
  (the matched argument is removed before forwarding)

no problems noticed -- `shimback doctor` runs the full set of checks
```

- **Overview**: the policy and, in plain words, what it does; the
  `diagnostic` and `force` flags.
- **Locations**: the shim's symlink and whether it's healthy (present, not
  dead, actually pointing at shimback); the config file that defines it --
  `config.toml`, or its own [split file](#splitting-a-shims-config-into-its-own-file)
  and which of the three locations that one lives in; a warning when a stale
  `config.toml` entry is being ignored because a split file wins; and whether
  typing the shim's name really runs the shim, or another binary earlier on
  `$PATH` bypasses it.
- **Commands**: the source (explicit, or `auto` and what it currently
  resolves to) and the fallback, each with its fixed arguments and a health
  tag (`[ok]`, `[warn]` for a `--force`d path that doesn't exist yet, `[fail]`
  for a missing or looping one).
- **Policy settings**: whatever drives the policy -- error patterns, exit
  codes, route arguments, rewrite rules, or every route of a `route-map`
  (with each route's own `args`) -- plus `strip matched`.
- **Trial run**: the effective capture timeout and output limit, and whether
  each comes from a per-shim override, the global config, or the default;
  for policies that run their target directly it says so instead.
- **Flow**: the diagram, drawn from this shim's real resolved paths,
  arguments, routes, and thresholds.

A closing line counts the `[fail]`/`[warn]` items. `info` is read-only and
informational (its exit status is `0` for any shim it can describe); use
[`doctor`](#doctor) for the full health check. An unknown name fails with a
typo suggestion, and a symlink with no configuration behind it (an orphan) is
reported as such and exits `1`.

## Fallback policies

- **`exit-code`** (the default): fall back whenever the source command
  exits non-zero.
- **`heuristic`**: only fall back when the source's stderr matches one of
  the shim's configured `--error-pattern` values (case-insensitive
  substring match) — useful when a non-zero exit can mean several different
  things and you only want to retry on a specific kind of failure (e.g. an
  argument-style mismatch between GNU and BSD tools):

  ```sh
  shimback add sed -f /usr/bin/sed \
      --policy heuristic \
      --error-pattern "invalid option" \
      --error-pattern "illegal option"
  ```

  If the source fails and its stderr doesn't match any pattern, that's
  treated as a genuine failure: the real stdout/stderr and exit code are
  surfaced normally, and the fallback is **not** run.
- **`exit-code-match`**: like `heuristic`, but keyed on the source's exact
  exit code instead of its stderr text — only fall back when it exits with
  one of the shim's configured `--exit-code` values (repeatable, each
  `0`-`255`):

  ```sh
  shimback add grep -f /usr/bin/grep \
      --policy exit-code-match \
      --exit-code 2
  ```

  Any other non-zero exit (e.g. grep's own "no match" exit `1`) surfaces
  as-is, with the fallback **not** run.
- **`route-args`**: not a fallback-on-failure policy at all. Instead of
  running the source and reacting to how it went, `shimback` looks at the
  invocation's arguments *before running anything*: if any of them exactly
  match one of the shim's configured `--route-arg` values, it runs the
  **fallback**; otherwise it runs the **source**. Only the chosen one ever
  runs, directly, with live/inherited stdio (no invisible trial run, no
  captured output, no retry if it fails) — useful for a command whose
  behavior you want to switch on a flag rather than on failure:

  ```sh
  shimback add cools -s /bin/ls -f /usr/local/bin/eza \
      --policy route-args \
      --route-arg x
  ```

  Running `cools` normally runs `ls`; running `cools x` runs `eza x`
  instead. Pass `--strip-matched-args` to drop the matched argument(s)
  before forwarding the rest — with it set, `cools x` above would run
  `eza` with no arguments at all.
- **`split-args`**: also decides up front from the arguments alone rather
  than reacting to failure, but two-sided: source and fallback each get
  their *own* set of discriminating args (`--split-source-arg` /
  `--split-fallback-arg`, each repeatable, at least one of each required).
  A side only wins when *every one* of its own args is present in the
  invocation — a full match, not just "any one of them." If both sides
  fully match at once (one side's args are a subset of the other's), the
  side needing *more* matched args — the more discriminating one — wins;
  an exact tie in count favors source:

  ```sh
  shimback add release -s ./build-debug -f ./build-release \
      --policy split-args \
      --split-source-arg --debug --split-source-arg -g \
      --split-fallback-arg --debug --split-fallback-arg -g --split-fallback-arg -O3
  ```

  Passing `--debug -g` matches source's two args exactly (and doesn't fully
  match fallback's three) — source runs. Passing `--debug -g -O3` fully
  matches *both* — fallback's three matched args outweigh source's two, so
  fallback runs instead. If neither side fully matches (e.g. just `-g`
  alone), there's nothing to decide up front, so `split-args` falls back to
  the plain `exit-code` behavior instead: run source, and fall back on any
  failure. Whichever side actually wins runs directly, with live/inherited
  stdio (no invisible trial run) — the "neither matched" case is the only
  one that goes through the normal captured trial run. `--strip-matched-args`
  works the same way it does for `route-args`, removing whichever side's
  own args actually matched before forwarding the rest.
- **`rewrite`**: not a fallback-on-failure policy either, and unlike every
  other policy, **`-f`/`--fallback` is optional and never used** — this is
  an alias/argument-macro mechanism, not a retry mechanism. Each
  `--rewrite <from>=<to>` rule (repeatable) is checked against every
  argument the shim was invoked with; a match is replaced with `<to>`
  before running the **source**, live/inherited, no capture, no retry:

  ```sh
  shimback add tmux -s /usr/bin/tmux --policy rewrite --rewrite "all=ls"
  shimback add ls -s /bin/ls --policy rewrite --rewrite "--full=-ltrah"
  ```

  Running `tmux all` runs `tmux ls` instead; running `ls --full` runs
  `ls -ltrah` instead. A `<to>` containing spaces expands into multiple
  forwarded arguments (e.g. `--rewrite "backup=-c -z -f backup.tar.gz"`);
  an empty `<to>` (e.g. `--rewrite "-v="`) just drops the matched argument.
  Arguments that don't match any rule pass through unchanged.
- **`route-map`**: `route-args` generalized from a single fallback to any
  number of them. Like `rewrite`, **`-f`/`--fallback` is optional and
  never used**. Each `--route <match>=<command>` rule (repeatable) is
  checked, in order, against the invocation's arguments; the first one
  whose `<match>` exactly equals one of them runs `<command>` instead of
  the **source**, live/inherited, no capture, no retry. No match at all
  runs the source, exactly like `route-args`' own no-match case. Unlike
  `route-args`, the same command can appear in more than one route — the
  motivating case is a version-selecting shim:

  ```sh
  shimback add java -s /opt/java17/bin/java \
      --policy route-map \
      --route "--v8=/opt/java8/bin/java" \
      --route "--v25=/opt/java25/bin/java"
  ```

  Running `java` normally runs `java17`; `java --v8 Foo.java` runs `java8`
  instead; `java --v25 Foo.java` runs `java25` instead. `--strip-matched-args`
  works the same way it does for `route-args`, removing whichever route's
  matched token before forwarding the rest. A route can also carry its own
  fixed arguments — not settable via `--route` itself (its `<match>=<command>`
  shape has no room for a third field without ambiguous shell quoting), but
  editable directly in `config.toml`'s `args = [...]` under that route's
  `[[shims.<name>.routes]]` block — which is what lets the *same* command be
  reused across two routes with different behavior, e.g. a `--preview` route
  that also runs `java25`, but with an extra `--enable-preview` flag baked
  in. `add` rejects two routes that would resolve to an identical
  `(command, args)` pair — the second could never fire — both at `add` time
  and on every config load.
- **`passthrough`**: falls back on any non-zero exit, exactly like
  `exit-code`, but skips the invisible trial run entirely — the source runs
  with live/inherited stdio from the start, so its output (success *or*
  failure) streams to your terminal in real time instead of being held back
  for a possible replay:

  ```sh
  shimback add terraform -s /opt/terraform-1.5/bin/terraform \
      -f /opt/terraform-1.9/bin/terraform \
      --policy passthrough
  ```

  This trades away the "invisible" property every other fallback-on-failure
  policy has: by the time fallback runs, the source's own failure output has
  already been seen. Useful when you'd rather watch a slow or chatty command
  run live than wait for it to finish before anything shows up. Because
  nothing is ever captured or hidden here, `--diagnostic` and
  `--capture-timeout`/`--capture-limit` are rejected if set alongside this
  policy — there's nothing for any of them to apply to.

> **Note:** `exit-code` is deliberately the least precise policy (any
> failure triggers a retry) and needs no extra configuration. `heuristic`,
> `exit-code-match`, `route-args`, `split-args`, `rewrite`, and `route-map`
> are more targeted — each requires at least one `--error-pattern` /
> `--exit-code` / `--route-arg` / (`--split-source-arg` and
> `--split-fallback-arg`) / `--rewrite` / `--route` respectively, enforced
> both at `add` time and on every config load, so a shim can never silently
> end up in a state where it's configured to be selective but has nothing
> to select on (`shimback doctor` also flags this if the config is
> hand-edited into that state). The first three policies only ever affect
> *failed* runs — a source that exits `0` always has its output passed
> through untouched, regardless of policy. `route-args`, `rewrite`, and
> `route-map` are unconditional exceptions: all three decide what to run
> (or how to rewrite it) up front from the arguments alone, never looking
> at the exit code at all. `split-args` is a conditional exception — up
> front from the arguments when one side fully matches, exit-code-like
> otherwise. `passthrough` is exit-code-like in *when* it falls back, but
> unlike every other policy here, its trial run is never captured or hidden
> in the first place — see its own entry above.

### Diagnostics

By default, falling back leaves no trace. Pass `--diagnostic` at `add` time
(or set `diagnostic = true` in the config) to print a single line to stderr
whenever that shim falls back:

```
shimback: 'sed' failed; falling back to /usr/bin/sed
```

Not available for `--policy passthrough`: nothing is ever hidden under that
policy, so there's nothing for a diagnostic to report — `add`/config
validation rejects the combination.

## Configuration

Config lives at `$XDG_CONFIG_HOME/shimback/config.toml`, falling back to
`$HOME/.config/shimback/config.toml` if `XDG_CONFIG_HOME` is unset — this is
honored even on macOS, not overridden by platform-native paths. The shim
symlinks themselves live at `$XDG_DATA_HOME/shimback/bin` (fallback
`$HOME/.local/share/shimback/bin`). Any individual shim can instead live in
its own `<name>-config.toml` file — see
[Splitting a shim's config into its own file](#splitting-a-shims-config-into-its-own-file).

A top-level `verbose = true` sets the default for `add`'s shell-startup-file
PATH-update notices (see `add`, above); omitted or `false` keeps them silent
unless `-v`/`--verbose` is passed on that particular `add`.

Top-level `capture_timeout_ms` and `capture_limit` set the global defaults
for the trial-run capture cutover (see
[How it works](#how-it-works)) — omitted, they default to `2000` and
`"8MiB"`. A shim can override either with its own `capture_timeout_ms`/
`capture_limit` (see `add`'s `--capture-timeout`/`--capture-limit`, above).
`capture_limit` accepts the same unit suffixes there or here — always
stored back as a plain byte count once resolved, same as `--capture-limit`
freezes into a byte count at `add` time.

```toml
version = 1
capture_timeout_ms = 2000
capture_limit = "8MiB"

[shims.sed]
fallback = "/usr/bin/sed"
policy = "exit-code"

[shims.awk]
source = "/opt/homebrew/bin/gawk"
fallback = "/usr/bin/awk"
policy = "heuristic"
error_patterns = ["invalid option", "illegal option", "unrecognized option"]
diagnostic = true
capture_timeout_ms = 500
capture_limit = "1MiB"

[shims.grep]
fallback = "/usr/bin/grep"
policy = "exit-code-match"
exit_codes = [2]

[shims.cools]
source = "/bin/ls"
fallback = "/usr/local/bin/eza"
policy = "route-args"
route_args = ["x"]
strip_matched_args = true

[shims.release]
source = "./build-debug"
fallback = "./build-release"
policy = "split-args"
source_route_args = ["--debug", "-g"]
fallback_route_args = ["--debug", "-g", "-O3"]

[shims.ls]
source = "/bin/ls"
policy = "rewrite"
rewrite_from = ["--full"]
rewrite_to = ["-ltrah"]

[shims.ll]
source = "/bin/ls"
source_args = ["-l", "-t", "-r", "-a", "-h"]
fallback = "/usr/local/bin/eza"
fallback_args = ["-la"]
policy = "exit-code"

[shims.java]
source = "/opt/java17/bin/java"
policy = "route-map"

[[shims.java.routes]]
match = "--v8"
command = "/opt/java8/bin/java"

[[shims.java.routes]]
match = "--v25"
command = "/opt/java25/bin/java"

[[shims.java.routes]]
match = "--preview"
command = "/opt/java25/bin/java"
args = ["--enable-preview"]
```

The file is managed by `add`/`remove`, but is plain, hand-editable TOML (a
small subset — inline tables and multi-line strings aren't supported, and
the only array-of-tables shape recognized is `[[shims.<name>.routes]]`,
`route-map`'s own per-route blocks, which must come right after that
shim's `[shims.<name>]` section). A route's `args` is optional and behaves
like `source_args`/`fallback_args`: fixed arguments always prepended
ahead of whatever the shim was actually invoked with, whenever that route
fires.

## Exit codes

- `127` — the shim, source, or fallback couldn't be found or isn't
  executable.
- `1` — a CLI or config validation error.
- Anything else is passed straight through from whichever of source/
  fallback actually ran (or `128 + signal` if it was killed by a signal).

## Building

Requires a C11 compiler and CMake ≥ 3.16. No external dependencies to build
or run shims (`curl`/`tar` are only ever shelled out to by `update`, and by `install` as a
fallback when it can't find a man page bundled next to itself — see
`install` and `update` above).

On Windows, building from source needs clang-cl/lld-link and an
MSVC/Windows SDK sysroot (via [`xwin`](https://github.com/Jake-Shadle/xwin),
no full Visual Studio install required) rather than "any C11 compiler" —
see [`cmake/windows-clang-cl.cmake`](cmake/windows-clang-cl.cmake) for the
exact toolchain setup and usage. The prebuilt release binary (above)
doesn't need any of this.

With [`just`](https://github.com/casey/just) installed, `just build`
autodetects your OS/arch and builds into `build-<os>-<arch>/`:

```sh
just build
just test               # optional
just install ~/.local   # optional; defaults to /usr/local
```

Or drive CMake directly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # optional
```

The resulting `shimback` binary is self-contained, but **place it somewhere
permanent before running `add`**: each shim's symlink is frozen to point at
wherever the binary was running from at `add` time, so a binary left in a
build directory will strand every shim once that directory is removed or
rebuilt. `shimback install` (see above) handles this for you (after a local
rebuild, `shimback install --force` refreshes the installed copy), or install it
anywhere else on `PATH` yourself (e.g. via `cmake --install build`, which
also installs [`man/shimback.1`](man/shimback.1) to `<prefix>/share/man/man1`).

## Platform support

macOS and Linux (x86_64 and arm64), and Windows (x86_64) for v0.1.0.
Windows shims are hard links rather than symlinks (falling back to a
plain copy when that's not possible, e.g. across drives — see
[`add`](#add)), PATH integration covers PowerShell and cmd.exe, the
interactive add wizard uses the native console API, and no man page ships
there (`shimback --help`/`doctor` output is the fallback, same as
everywhere else this project treats `--help` as authoritative). A native
Pester test suite ([`tests/windows/`](tests/windows)) covers this platform
alongside the POSIX shell suite in [`tests/`](tests). Windows ARM64 isn't
built yet.

## License

MIT OR Apache-2.0.
