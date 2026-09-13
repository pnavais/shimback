# shimback

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

- [How it works](#how-it-works)
- [Usage](#usage)
- [Fallback policies](#fallback-policies)
- [Configuration](#configuration)
- [Exit codes](#exit-codes)
- [Building](#building)
- [Platform support](#platform-support)
- [License](#license)

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
   all. (`route-args`, `split-args`, and `rewrite` are exceptions to this
   "try source, maybe fall back" shape — see below.)

A failed trial run of the source command is designed to be **invisible**:
its stdout and stderr are captured, not streamed live, and are discarded
entirely if a fallback is triggered — nothing about the failed attempt
reaches your terminal unless the shim explicitly opts into a one-line
diagnostic (see `diagnostic` below).

## Usage

```
shimback add <name> [-s <source>] [--source-arg <arg>]...
                     -f <fallback> [--fallback-arg <arg>]...
                     [--policy exit-code|heuristic|exit-code-match|route-args|rewrite|split-args]
                     [--error-pattern <p>]... [--exit-code <code>]...
                     [--route-arg <arg>]... [--strip-matched-args]
                     [--split-source-arg <arg>]... [--split-fallback-arg <arg>]...
                     [--rewrite <from>=<to>]... [--diagnostic] [--force] [-v|--verbose]
shimback remove [-y] <name>
shimback init
shimback list [--full]
shimback doctor [fix]
shimback install [--prefix <dir>] [--shell <shell>[,<shell>]... | --all]
shimback uninstall [--prefix <dir>] [--full]
shimback edit
shimback --help | --version
```

`--help` (and the usage printed on a missing/unknown command) uses styled,
colored help when stdout is a terminal and `NO_COLOR` isn't set: section
headers bold yellow, commands and flags bold green,
placeholders (`<name>`, `<fallback>`, …) cyan.

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
- `-f`/`--fallback` is required (except with `--policy rewrite`, which never
  uses it), and is resolved the same way `-s` is: once, at `add` time,
  frozen as an absolute path.
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
  to without a fallback (only possible under `--policy rewrite`, the one
  policy where `-f`/`--fallback` is optional) — given without one, it's
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
  later `uninstall --full` checks both files, regardless of which one
  currently exists. If [`zsh-defer`](https://github.com/romkatv/zsh-defer)
  is available, the injected block routes its `export PATH=` through it
  too — otherwise tools like `mise` or `direnv` that defer their own
  PATH-mutating activation (for faster prompt startup) would clobber the
  shim dir's position on `PATH` after the rc file finishes sourcing,
  regardless of where the shimback block sits in the file.
- The confirmation line printed on success (`shimback: 'name' -> path
  (fallback: ..., policy: ...)`) is colored when stdout is a terminal and
  `NO_COLOR` isn't set, matching `list`'s conventions. The shell-startup-file
  notices right after it (`zsh: PATH updated in ...`, `Restart your shell
  ...`) are **silent by default** — pass `-v`/`--verbose` to print them for
  this one invocation, or set `verbose = true` at the top level of
  `config.toml` (see [Configuration](#configuration)) to make that the
  default for every `add`. `-v`/`--verbose` only ever turns them on; it
  can't override a config default of `true` back to silent for one call.

#### Interactive wizard

Run `add` at an interactive terminal with required information missing
(no name at all, or a name but no fallback, or a policy that needs
`--error-pattern`/`--exit-code`/`--route-arg`/(`--split-source-arg` and
`--split-fallback-arg`)/`--rewrite` and doesn't have it) and, instead of
failing, a small step-by-step wizard walks you through filling it in:
name, policy (picked from a list), source, source's extra args, fallback,
fallback's extra args, then whatever the chosen policy still needs (for
`split-args`, that's two required list pages, one per side), then the
diagnostic flag. The two extra-args pages are optional list pages, unlike
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
  field (source, or fallback under `--policy rewrite`), pressing it with
  nothing typed just skips that field.
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

Removes the symlink and the config entry for `<name>`. It does **not**
touch the PATH injection in your shell's startup file, since other shims
(or a future `add`) may still need it.

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
`rewrite` green, `split-args` red, `exit-code` uncolored as the baseline); `false`
diagnostics are dimmed and `true` ones are green; column headers are bold
yellow. Piping the output (e.g. to a file or another command) disables
color automatically.

`shimback list --full` (or `ls --full`) additionally prints whatever the
table's columns leave out, as extra indented lines right under a shim's
row: `source_args`/`fallback_args` (see `add`, above — independent of
policy, so these can show up for any shim), then whatever's
policy-specific — `error_patterns` for `heuristic`, the configured codes
for `exit-code-match`, `route_args` and `strip_matched_args` for
`route-args`, `source_route_args`/`fallback_route_args`/`strip_matched_args`
for `split-args`, and each `<from> -> <to>` pair for `rewrite`. A shim with
none of the above configured has nothing extra to show and gets no
additional lines:

```
NAME   SOURCE     FALLBACK      POLICY           DIAGNOSTIC
sed    auto       /usr/bin/sed  exit-code        false
awk    /opt/.../gawk   /usr/bin/awk  heuristic   true
        error patterns: invalid option, illegal option
ll     /bin/ls    /usr/local/bin/eza  exit-code  false
        source args: -l, -t, -r, -a, -h
        fallback args: -la
```

### `doctor`

```sh
shimback doctor [fix]
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

`shimback doctor fix` repairs what it safely can before reporting: any shim
whose symlink is missing entirely, or is a *dangling* symlink (its target no
longer exists — e.g. because the binary it pointed at, back when `add` ran,
has since moved or been cleaned up), gets its symlink recreated pointing at
the currently running `shimback` binary, the same way `add` creates it in
the first place. It never touches a symlink that still resolves (even to a
different-but-valid `shimback` binary elsewhere) or a path occupied by
anything other than a symlink — only genuinely dead or missing entries.

It also checks every shim's source (if explicit) and fallback for a
**cycle**: a value that resolves back to the `shimback` binary itself (see
`add`, above). `add` refuses to create one, so the only way a cycle ends up
in the config is by hand-editing it. When `fix` finds one, it prompts
interactively for a replacement, right there in the terminal:

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

`doctor` also checks for a stray PATH block: if `~/.zshrc.local` exists but
the shimback block is still sitting in `~/.zshrc` (written by an `add`/
`init`/`install` that ran before `~/.zshrc.local` existed, or before
shimback preferred it — see `add`, above), it's flagged as a `[warn]`, not
a `[fail]` — it doesn't affect `doctor`'s exit code, since PATH still works
fine either way. `doctor fix` acts on it by moving the block over.

### `install`

```sh
shimback install [--prefix <dir>] [--shell <shell>[,<shell>]... | --all]
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
or rebuilt. Re-running `install` (e.g. after building a newer version)
simply refreshes the installed copy, and migrates away an old separate
`shimback-bin` block if v0.1.0 ever left you with one.

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

### `edit`

```sh
shimback edit
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

> **Note:** `exit-code` is deliberately the least precise policy (any
> failure triggers a retry) and needs no extra configuration. `heuristic`,
> `exit-code-match`, `route-args`, `split-args`, and `rewrite` are more
> targeted — each requires at least one `--error-pattern` / `--exit-code` /
> `--route-arg` / (`--split-source-arg` and `--split-fallback-arg`) /
> `--rewrite` respectively, enforced both at `add` time and on every config
> load, so a shim can never silently end up in a state where it's
> configured to be selective but has nothing to select on (`shimback
> doctor` also flags this if the config is hand-edited into that state).
> The first three policies only ever affect *failed* runs — a source that
> exits `0` always has its output passed through untouched, regardless of
> policy. `route-args` and `rewrite` are unconditional exceptions: both
> decide what to run (or how to rewrite it) up front from the arguments
> alone, never looking at the exit code at all. `split-args` is a
> conditional exception — up front from the arguments when one side fully
> matches, exit-code-like otherwise.

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

A top-level `verbose = true` sets the default for `add`'s shell-startup-file
PATH-update notices (see `add`, above); omitted or `false` keeps them silent
unless `-v`/`--verbose` is passed on that particular `add`.

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
