# shimback

`shimback` is a small, dependency-free command-line shim: it wraps a command
name (e.g. `sed`) with a **source** binary to run and a **fallback** binary
to transparently retry with if the source doesn't work out. It was born out
of a very concrete annoyance: on a Nix-managed macOS system, GNU `sed` often
ends up ahead of BSD `/usr/bin/sed` on `PATH`, and scripts written for one
dialect's argument style (`sed -i ''` vs `sed -i`) break under the other.
`shimback` generalizes that "try one, fall back to the other" idea to any
pair of commands.

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
   all. (The `route-args` policy is the exception to this "try source,
   maybe fall back" shape — see below.)

A failed trial run of the source command is designed to be **invisible**:
its stdout and stderr are captured, not streamed live, and are discarded
entirely if a fallback is triggered — nothing about the failed attempt
reaches your terminal unless the shim explicitly opts into a one-line
diagnostic (see `diagnostic` below).

## Usage

```
shimback add <name> [-s <source>] -f <fallback>
                     [--policy exit-code|heuristic|exit-code-match|route-args]
                     [--error-pattern <p>]... [--exit-code <code>]...
                     [--route-arg <arg>]... [--strip-matched-args] [--diagnostic]
shimback remove <name>
shimback init
shimback list
shimback doctor
shimback install [--prefix <dir>]
shimback uninstall [--prefix <dir>] [--full]
shimback --help | --version
```

`--help` (and the usage printed on a missing/unknown command) is colored
like clap-rs's styled help when stdout is a terminal and `NO_COLOR` isn't
set: section headers bold yellow, commands and flags bold green,
placeholders (`<name>`, `<fallback>`, …) cyan.

### `add`

```sh
shimback add sed -f /usr/bin/sed
```

- `<name>` is the command name to shim (e.g. `sed`). It can't contain `/`
  and can't be `shimback` itself.
- `-s`/`--source` is optional. If given, it's the exact path to the primary
  binary, resolved once and frozen in the config — re-run `add` if that
  binary moves. If omitted, the source is resolved fresh from `PATH` on
  **every invocation**, skipping shimback's own shim directory (and
  anything that resolves back to the `shimback` binary itself), so it
  naturally follows whatever the "real" `<name>` on your system currently
  is.
- `-f`/`--fallback` is required: the path to the fallback binary.
- `add` refuses to create a shim where source and fallback resolve to the
  same binary (nothing would ever change), and never writes anything if
  validation fails.
- `add` also ensures the shim directory is on `PATH`, by injecting an
  idempotent, clearly marked block into your current shell's startup file
  (detected from `$SHELL`). Re-running `add` never duplicates this block.
  On zsh, if [`zsh-defer`](https://github.com/romkatv/zsh-defer) is
  available, the injected block routes its `export PATH=` through it too —
  otherwise tools like `mise` or `direnv` that defer their own PATH-mutating
  activation (for faster prompt startup) would clobber the shim dir's
  position on `PATH` after the rc file finishes sourcing, regardless of
  where the shimback block sits in the file.

### `remove`

```sh
shimback remove sed
```

Removes the symlink and the config entry for `<name>`. It does **not**
touch the PATH injection in your shell's startup file, since other shims
(or a future `add`) may still need it.

### `init`

```sh
shimback init
```

Detects installed shells (zsh, bash, and fish) and injects the PATH block
into each one's startup file(s) — useful for setting things up across every
shell you have installed, rather than just your current one. As of v0.1.0,
**fish is detected but not automatically configured** (its PATH mechanism,
`fish_add_path`/`config.fish`, is different enough from an `export PATH=`
line that it's out of scope for now); `init` will print the manual command
to run instead.

### `list` (alias: `ls`)

```sh
shimback list
```

Prints every configured shim's name, source (or `auto`), fallback, policy,
and diagnostic flag as a column-aligned table:

```
NAME  SOURCE                   FALLBACK      POLICY     DIAGNOSTIC
sed   auto                     /usr/bin/sed  exit-code  false
awk   /opt/homebrew/bin/gawk   /usr/bin/awk  heuristic  true
```

When stdout is a terminal (and [`NO_COLOR`](https://no-color.org/) isn't
set): shim names are bold cyan; an explicit source is green and `auto` is
dimmed; the fallback path is blue; the policy column is colored by kind
(`heuristic` yellow, `exit-code-match` magenta, `route-args` cyan,
`exit-code` uncolored as the baseline); `false` diagnostics are dimmed and
`true` ones are green; column headers are bold yellow. Piping the output
(e.g. to a file or another command) disables color automatically.

### `doctor`

```sh
shimback doctor
```

Checks the health of your whole shimback setup and reports any problems:
whether the shim directory exists and is actually on `$PATH`, whether the
config file parses, and, for each configured shim, whether its symlink
exists and isn't dead, whether its fallback (and, if explicit, its source)
still exist and are executable, and whether its policy is fully configured
(e.g. `heuristic` with no `--error-pattern`, or `exit-code-match` with no
`--exit-code`, can never fall back). Exits `0` if everything checks out,
`1` otherwise — safe to run in CI or a shell startup hook. Section headers
and shim names are bold yellow/cyan, `[ok]`/`[fail]` are green/red, and the
closing summary line is green or red, when stdout is a terminal.

### `install`

```sh
shimback install [--prefix <dir>]
```

Copies the running `shimback` binary to `<prefix>/bin/shimback` (default
prefix: `~/.local`) and ensures `<prefix>/bin` is on `PATH`, via the same
kind of idempotent, marker-block injection `add`/`init` use for the shim
directory (under its own tag, so the two blocks coexist). This is the
easiest way to get `shimback` itself onto a **stable** location: `add`
freezes the path of whatever binary is currently running into each shim's
symlink (see below), so running it straight out of a build directory means
every shim breaks the next time that directory is cleaned or rebuilt.
Re-running `install` (e.g. after building a newer version) simply refreshes
the installed copy.

`install` also installs this man page to `<prefix>/share/man/man1/shimback.1`.
It first looks for a `shimback.1` bundled next to the running binary — how
each [release](https://github.com/pnavais/shimback/releases) tarball ships
it, so the common "download a release, run install" path never touches the
network for this. If it can't find one there (e.g. built from source
directly), it falls back to downloading it from the GitHub release matching
the running version, via `curl` — the one place shimback shells out to
something it didn't build, and the reason it isn't unconditionally
"dependency-free": if `curl` isn't on `PATH`, or the download fails, this
step is skipped with a warning and `install` still succeeds at its main
job of getting the binary in place.

### `uninstall`

```sh
shimback uninstall [--prefix <dir>] [--full]
```

Removes every symlink `shimback` created in the shim directory (dangling or
not — nothing else should ever live there), the `<prefix>/bin/shimback`
binary `install` placed (default prefix: `~/.local`, matching `install`),
and the man page installed alongside it. By default the config file and the
`PATH` marker blocks in shell startup files are left alone, so a future
`add`/`init` just picks up where things left off; pass `--full` to also
delete the config file and remove those `PATH` blocks — a complete teardown.
Safe to re-run: nothing left to remove is just reported as already gone.

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
  shimback add cagao -s /bin/ls -f /usr/local/bin/eza \
      --policy route-args \
      --route-arg x
  ```

  Running `cagao` normally runs `ls`; running `cagao x` runs `eza x`
  instead. Pass `--strip-matched-args` to drop the matched argument(s)
  before forwarding the rest — with it set, `cagao x` above would run
  `eza` with no arguments at all.

> **Note:** `exit-code` is deliberately the least precise policy (any
> failure triggers a retry) and needs no extra configuration. `heuristic`,
> `exit-code-match`, and `route-args` are more targeted — each requires at
> least one `--error-pattern` / `--exit-code` / `--route-arg` respectively,
> enforced both at `add` time and on every config load, so a shim can never
> silently end up in a state where it's configured to be selective but has
> nothing to select on (`shimback doctor` also flags this if the config is
> hand-edited into that state). The first three policies only ever affect
> *failed* runs — a source that exits `0` always has its output passed
> through untouched, regardless of policy — `route-args` is the exception,
> deciding source vs. fallback up front from the arguments alone.

### Diagnostics

By default, falling back leaves no trace. Pass `--diagnostic` at `add` time
(or set `diagnostic = true` in the config) to print a single line to stderr
whenever that shim falls back:

```
shimback: 'sed' failed; falling back to /usr/bin/sed
```

## Configuration

Config lives at `$XDG_CONFIG_HOME/shimback/config.toml`, falling back to
`$HOME/.config/shimback/config.toml` if `XDG_CONFIG_HOME` is unset — this is
honored even on macOS, not overridden by platform-native paths. The shim
symlinks themselves live at `$XDG_DATA_HOME/shimback/bin` (fallback
`$HOME/.local/share/shimback/bin`).

```toml
version = 1

[shims.sed]
fallback = "/usr/bin/sed"
policy = "exit-code"

[shims.awk]
source = "/opt/homebrew/bin/gawk"
fallback = "/usr/bin/awk"
policy = "heuristic"
error_patterns = ["invalid option", "illegal option", "unrecognized option"]
diagnostic = true

[shims.grep]
fallback = "/usr/bin/grep"
policy = "exit-code-match"
exit_codes = [2]

[shims.cagao]
source = "/bin/ls"
fallback = "/usr/local/bin/eza"
policy = "route-args"
route_args = ["x"]
strip_matched_args = true
```

The file is managed by `add`/`remove`, but is plain, hand-editable TOML (a
small subset — no arrays of tables, inline tables, or multi-line strings).

## Exit codes

- `127` — the shim, source, or fallback couldn't be found or isn't
  executable.
- `1` — a CLI or config validation error.
- Anything else is passed straight through from whichever of source/
  fallback actually ran (or `128 + signal` if it was killed by a signal).

## Building

Requires a C11 compiler and CMake ≥ 3.16. No external dependencies to build
or run shims (`curl` is only ever shelled out to by `install`, and only as a
fallback when it can't find a man page bundled next to itself — see
`install` above).

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
rebuilt. `shimback install` (see above) handles this for you, or install it
anywhere else on `PATH` yourself (e.g. via `cmake --install build`, which
also installs [`man/shimback.1`](man/shimback.1) to `<prefix>/share/man/man1`).

## Platform support

macOS and Linux (x86_64 and arm64) for v0.1.0. Windows is a long-term goal
but isn't supported yet — the current implementation relies on POSIX
symlinks, `fork`/`exec`, and Unix-style shell startup files throughout.

## License

MIT OR Apache-2.0.
