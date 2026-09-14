# Whole-repository code review

## Scope and method

Reviewed the current tracked repository at `HEAD`, including `src/`, all tracked
tests/fixtures, `CMakeLists.txt`, and the relevant recent history. Generated
build artifacts were excluded from the code review. I examined security,
correctness/error handling, performance, architecture, and test coverage, then
ran a fresh CMake configure/build with `SHIMBACK_DEV_WARNINGS=ON` and the full
CTest suite.

Verification: the clean build succeeded and all 10 tests passed.

## Critical

### 1. Shell startup files are generated with an injection vulnerability

- **Location:** `src/shell.c:181-195`, `src/shell.c:483-490`
- **Problem:** Directory paths derived from `HOME`/`XDG_DATA_HOME` are inserted
  directly into shell code. The zsh/bash form places them inside double quotes
  without escaping `$`, backticks, backslashes, or double quotes; the fish form
  also emits the path as raw shell syntax.
- **Why it matters:** A path such as `.../data"; /usr/bin/touch /tmp/pwned; #`
  produces executable commands in the user's startup file. I reproduced this
  by setting `XDG_DATA_HOME` to that value, running `shimback init`, and
  sourcing the generated `.bashrc`; the injected command executed. This is
  arbitrary code execution on the next shell startup and can also affect the
  current shell if the file is re-sourced.
- **Fix:** Never concatenate untrusted path text into shell source. Escape it
  for the target shell (including quotes, backslashes, `$`, backticks, and
  command-substitution syntax), or use a shell-safe serialization strategy.
  Add regression tests that generate startup files with quotes, backslashes,
  `$`, and command-substitution characters and source them in a clean shell.

## Warning

### 1. `add` mutates the shim directory before validating/saving the configuration

- **Location:** `src/commands/add.c:147-169`, followed by config loading at
  `src/commands/add.c:171-177`
- **Problem:** The existing managed symlink is removed and recreated before the
  current config is successfully loaded and before the new config is saved.
- **Why it matters:** If the config is malformed, unreadable, or the save fails,
  `add` exits with an error but leaves a newly created or replaced symlink. I
  reproduced this with a readable config that fails validation: `add` returned
  status 1 while the new shim symlink remained on disk. The filesystem and
  configuration then describe different states, and a later invocation can
  unexpectedly dispatch through the orphaned shim.
- **Fix:** Load and validate the existing config, construct/save the new config,
  and only then create or replace the symlink; alternatively retain the old
  symlink and roll it back whenever any later operation fails. Ideally perform
  both changes through an explicit recovery/rollback path because they cannot
  be made one atomic filesystem transaction.

### 2. `waitpid` interruptions are ignored in process execution paths

- **Location:** `src/dispatch.c:168-179`, `src/dispatch.c:249-251`, and
  `src/commands/install.c:40-43`
- **Problem:** `waitpid` is called once and its return value is ignored. If a
  signal interrupts the wait, the status may be uninitialized or stale, while
  the child remains unreaped.
- **Why it matters:** A signal arriving while a wrapped command, install copy,
  or fallback is running can make the parent decode an invalid status and
  return the wrong exit code, and can leave a zombie child. This is an error
  path in normal Unix process behavior, not just a theoretical compiler issue.
- **Fix:** Retry `waitpid` when it returns `-1` with `errno == EINTR`, and treat
  any other failure as an explicit error. For example:

  ```c
  do {
      rc = waitpid(pid, &status, 0);
  } while (rc < 0 && errno == EINTR);
  if (rc < 0) { /* report/fail */ }
  ```

### 3. Fallback-policy trials buffer unbounded child output in memory

- **Location:** `src/dispatch.c:181-251`
- **Problem:** `run_captured` appends every byte written to stdout and stderr
  into `DynBuf` until the child exits, with no size limit.
- **Why it matters:** Any source command that emits a large stream and exits
  non-zero (including a misconfigured or compromised executable) causes the
  shim to retain the entire output before deciding whether to fall back. This
  can exhaust the process's memory and terminate the wrapper instead of
  running the fallback or returning the source result.
- **Fix:** Bound captured output and define overflow behavior. For example,
  retain only a configured maximum needed for heuristic matching while
  continuing to drain the pipes, or spool output to temporary files and replay
  only when required. Keep draining both descriptors to preserve the existing
  deadlock avoidance.

## Conclusion

The core dispatch and test suite are in good shape, and the clean build/test
verification passed. The shell-code injection must be fixed before release;
the `add` ordering and interrupted-child handling should also be addressed to
avoid inconsistent state and unreliable command results.

---

## Response (2026-09-14)

All findings were re-verified against the current code before touching
anything (line numbers had drifted from the ones cited above, since several
other features landed between when this review was written and now).
Verification for every item below: `BUILD_TYPE=Debug just build` (clean,
`-Wall -Wextra -Werror`), `just clean && just test` (10/10 suites pass,
including two new regression tests added for this round), the reviewer's own
repro steps re-run by hand against the fixed binary, and a refreshed real
install (`~/.local/bin/shimback doctor` reports clean).

### Agreed and fixed

**Critical #1 — shell startup files generated with an injection
vulnerability.** Confirmed and fixed. `src/shell.c`'s `build_zsh_body`,
`build_bash_body`, and `build_fish_body` now write every directory as a
properly escaped shell/fish single-quoted literal (`append_sh_squoted` /
`append_fish_squoted`), instead of splicing the raw path into a
double-quoted (zsh/bash) or unquoted (fish) context. The matching parsers
(`parse_existing_dirs`, `parse_existing_fish_dirs`) were updated to read the
new quoted format back out, so idempotent re-runs (`add`/`init`/`install`
called again) still union directories correctly instead of just yielding no
directories. Re-ran the reviewer's exact repro
(`XDG_DATA_HOME='.../data"; touch /tmp/PWNED; #'`, then `shimback init`,
then sourcing the generated `.bashrc`) — the marker file is no longer
created. Also verified the same for zsh (`.zshrc`, including the
`zsh-defer` branch) and fish (a snippet with an embedded `'` and `(...)`,
fish's own command-substitution syntax). New regression test:
`tests/test_init.sh` (sets a metacharacter-laden `$XDG_DATA_HOME`, asserts
the generated files contain the safely-quoted form, then actually sources
the real `.zshrc`/`.bashrc` in the real shell and asserts no marker file
appears).

**Warning #1 — `add` mutates the shim directory before validating/saving
the configuration.** Confirmed and fixed. `src/commands/add.c`'s
`finish_add` now only does the read-only pre-flight checks (does something
already occupy the symlink path, and is it ours to replace) before touching
the config; the config is loaded, validated, the new entry built, and saved
*first*, and only once that succeeds does it `mkdir_p` the shim directory
and create/replace the symlink. Re-ran the reviewer's repro (a config that
parses but fails validation — `route-args` with no `route_args` — then
`add`) — `add` now fails with the same clear error and leaves no symlink
behind at all, vs. previously leaving an orphaned one. New regression test
in `tests/test_add_remove.sh`.

**Warning #2 — `waitpid` interruptions are ignored.** Confirmed and fixed.
Added `xwaitpid()` (`src/util.c`/`.h`), a `waitpid` wrapper that retries on
`EINTR`, and switched all four call sites to it: `dispatch.c`'s
`run_inherited` and `run_captured`, `install.c`'s `download_via_curl`, and
`edit.c`'s wait for the editor. Verified by running a slow shim/editor in
the background and bombarding it with `SIGWINCH` (30x over 1.5s) while it
was blocked in the wait — it still returned the correct exit code every
time and left no zombie process behind. Not covered by an automated
regression test: reliably forcing a `waitpid` EINTR race in CI is inherently
flaky (timing-dependent), so this was verified by hand rather than adding a
test likely to be a source of spurious CI failures later.

### Also fixed, after further discussion

**Warning #3 — fallback-policy trials buffer unbounded child output in
memory.** Fixed, via neither of the two options originally weighed in this
document. Discussion after the first round surfaced a better design than
either: `run_captured` now tracks both elapsed time and bytes captured for
a trial run; whichever of two configurable thresholds is hit first (default
2000ms / 8MiB) makes shimback give up on ever hiding or falling back on
that run, flush whatever's been captured so far straight to the real
stdout/stderr, and relay everything read from that point on live instead of
buffering it further. This bounds worst-case memory (the original concern)
*and* fixes a related, previously-undiscussed problem: a long-running or
unexpectedly chatty source used to buffer invisibly for its entire
lifetime, with no way to see its output until it exited — a genuine
functional gap for anything wrapping a dev server or a slow build, not just
a memory question. Unlike a hard cap that truncates, **no output is ever
lost** — every byte the command produces still reaches the user, split
between "replayed from the buffer" and "streamed live after the cutover."
The only thing that changes at the cutover point is that falling back is no
longer possible for that one invocation, since some of the source's real
output is already on screen.

Both thresholds are configurable, with the same global-default-plus-per-shim-
override shape used for `verbose`/`force`: `capture_timeout_ms`/
`capture_limit` at the top level of `config.toml`, or per-shim via `add
--capture-timeout <ms>`/`--capture-limit <size>` (accepting a unit suffix —
`B`, `K`/`KB`, `KiB`, `M`/`MB`/`MiB`, `G`/`GB`/`GiB`, case-insensitive).

One narrower, honest trade-off: `heuristic` policy's `--error-pattern`
matching only ever sees what was captured before a cutover, if one happens
— in practice low-risk, since a real error message appears at/near the
point of failure, not buried after megabytes of unrelated output, and once
cut over, the whole fallback decision (content or exit-code based, whichever
policy) is skipped uniformly, not just heuristic's.

Verified: the reviewer's original memory concern (a firehose of output),
the newly-identified long-running-command case (time-based cutover, checked
against a real `SIGWINCH`-interrupt-style live test), the fast-fail case
still falling back exactly as before with the default thresholds (no
regression), and that no bytes are ever lost across a cutover in either
direction. New tests in `test_dispatch.sh` (size and time cutover, fast-fail
regression, CLI unit-parsing) and `test_config_parser.c` (`parse_size_bytes`
unit tests, global+per-shim config round-trip).

## Second deep review (independent verification) — 2026-09-14

### Scope and method

Reviewed the complete current C implementation under `src/`, the CLI/config/parser,
shell generation and PATH migration, path and executable resolution, dispatch and
fallback execution, install/uninstall/doctor/edit behavior, `CMakeLists.txt`, all
tracked tests and fixtures, and the release documentation. I independently
re-checked every finding and claim in the response above rather than treating the
response as evidence. I also inspected failure/permission paths and ran targeted
reproductions for the two state/permission issues below.

Verification: a fresh out-of-tree Debug configure/build with
`-DSHIMBACK_DEV_WARNINGS=ON` succeeded; `ctest --test-dir ... --output-on-failure`
passed all 10/10 tests. The targeted remove failure reproduction confirmed that
`remove` deletes the symlink and then fails to save the config. A targeted
`umask 000` run confirmed that newly generated `.zshrc` and `config.toml` are
created as world-writable (`0666`).

### Prior finding verdicts

- **Critical #1, shell startup injection:** **fixed and verified.** The zsh/bash
  and fish generators now quote paths, and the corresponding round-trip parsers
  understand the quoted form. The new `test_init.sh` coverage and the response's
  source/repro claims are consistent with the current implementation.
- **Warning #1, `add` ordering:** **fixed for failures before `config_save`.**
  Current `finish_add` performs the preflight, loads/updates/saves config, and
  only then creates the symlink. It still cannot make the config and symlink one
  atomic transaction: a later `mkdir_p`, unlink, or symlink failure leaves the
  saved config without its filesystem entry. That residual limitation is covered
  by the new finding below.
- **Warning #2, interrupted waits:** **the EINTR omission is fixed.** All four
  waits use `xwaitpid`, which retries `EINTR`. The callers still ignore a
  non-EINTR `xwaitpid` failure in the dispatch/install paths, but with their live
  child PID this is not an independently reproducible release blocker; it should
  still be reported rather than decoding an uninitialized status in hardened code.
- **Warning #3, unbounded capture:** **fixed and verified.** The current trial
  path has both time and byte cutovers, drains both pipes, replays buffered data,
  and relays subsequent data live. The dispatch tests cover size/time cutover and
  fast-fail fallback behavior.

### Critical (must fix before merge)

#### 1. Atomic writers can create world-writable startup/config files

- **Location:** `src/shell.c:87-101`, `src/config.c:933-956`, and
  `src/paths.c:167-192`
- **Problem:** All three atomic-write implementations create their predictable
  temporary file with `fopen(..., "wb")`, so the mode is `0666 & ~umask`. They
  then rename it over the destination, changing the destination's permissions.
- **Why it matters:** A valid `umask 000` makes a generated `.zshrc`, `.bashrc`,
  fish snippet, or config file world-writable. Another local user/process can
  then alter a shell startup file and obtain code execution when it is sourced;
  a config can also be modified to run an attacker-selected executable. Even
  with a normal umask, replacing an existing private file silently weakens its
  mode. The targeted run produced `-rw-rw-rw-` for both `.zshrc` and
  `config.toml`.
- **Fix:** Create temporary files with `open(..., O_CREAT|O_EXCL|O_WRONLY,
  0600)` (or `mkstemp`), apply an explicit safe destination mode before rename
  (`0600` for config; a documented non-world-writable mode for generated shell
  files), and preserve an existing destination's mode where appropriate. Check
  the mode/ownership behavior in tests under permissive umasks.

### Warning (should address)

#### 1. Predictable temporary paths are vulnerable to symlink replacement

- **Location:** `src/shell.c:87-101`, `src/config.c:933-956`,
  `src/paths.c:167-192`, and `src/commands/install.c:28-47`
- **Problem:** Temporary names are formed as `<destination>.tmp.<pid>` and opened
  with `fopen(..., "wb")`, without exclusive creation. If that name already
  exists as a symlink, the write follows it before the final rename.
- **Why it matters:** A stale or locally planted temp symlink can cause `init`,
  `add`, `edit`, or `install` to overwrite an unrelated file the invoking user
  can write. The PID suffix is not a security boundary and is predictable enough
  for a local race; this is also an avoidable data-loss path after an interrupted
  invocation.
- **Fix:** Use `mkstemp`/`open(O_CREAT|O_EXCL)` in the destination directory,
  write through the returned descriptor/`FILE *`, and unlink that exact temporary
  file on failure. Prefer `fsync` before rename for the advertised atomic-write
  guarantee.

#### 2. `remove` can leave config and filesystem state inconsistent

- **Location:** `src/commands/remove.c:92-107`
- **Problem:** The managed symlink is unlinked before the updated config is
  saved. A config write failure therefore aborts after deleting the symlink.
- **Why it matters:** The command returns failure, but the configured shim no
  longer exists. I reproduced this by making the config directory non-writable:
  `remove` returned 1, reported the temp-file permission error, and the symlink
  was already gone while the config entry remained.
- **Fix:** Save the new config first, then remove the symlink, with a rollback
  path if symlink removal fails; or retain/recreate the old symlink whenever the
  later operation fails. The command should not report a failed removal after
  already performing its destructive half.

#### 3. Malformed numeric capture settings are silently accepted

- **Location:** `src/config.c:430-444` and `src/config.c:623-625`
- **Problem:** `capture_timeout_ms` is parsed with `strtol` without checking the
  end pointer, `errno`, range, or non-negative value. For example,
  `capture_timeout_ms = nope` is accepted as a successful config load and
  becomes zero; a shim then immediately cuts over to live output instead of
  applying the intended trial/fallback behavior.
- **Why it matters:** A typo or hand-edited malformed config changes dispatch
  semantics while `list`, `doctor`, and dispatch treat the file as valid. Large
  numeric values and negative values are also converted to `int` without a
  defined validation result, making behavior platform/compiler dependent.
- **Fix:** Parse through a checked helper using `errno`, the end pointer, and
  `INT_MAX`; reject trailing characters and negative values with a parse error.
  Apply the same validation to the per-shim value and add malformed/overflow
  parser tests.

### Suggestion (nice to have)

#### 1. Add failure-path regression coverage for the remaining transactional risks

- **Location:** `tests/` (no current test covers failed config save after a
  symlink mutation, failed symlink creation after a successful config save, or
  file-mode preservation under a permissive umask)
- **Problem:** The happy-path suite and the prior regression tests do not lock in
  the state guarantees relied on by `add`/`remove` and the atomic writers.
- **Why it matters:** These are precisely the paths that can leave a release
  installation inconsistent or weaken file permissions, and they can regress
  without affecting the current 10/10 result.
- **Fix:** Add sandboxed tests that force each write step to fail, assert the
  pre-existing config/symlink pair is preserved, and assert safe modes after
  `init`, `add`, and `install` under `umask 000`.

### 0.1.0 release recommendation

**Do not release 0.1.0 yet.** The clean build and all current tests pass, and all
three prior findings are substantially addressed, but the file-permission issue
can expose executable startup/config control to other local users and the
predictable temp-file design permits avoidable file overwrite/data-loss races.
Fix the Critical item before release; fix the transactional `remove` behavior and
checked numeric parsing before release as well. Re-run the full suite plus the
new failure-path tests after those changes.

### Review lead assessment

I agree with the independent findings and the **do not release 0.1.0** verdict.
The original shell-injection, `add` ordering, EINTR, and unbounded-capture issues
are addressed, but the remaining permission, temporary-file, removal-transaction,
and numeric-validation problems are real release risks. A clean Release build and
the full 10/10 test suite pass locally; a Debug build with `-Wall -Wextra -Werror`
also passes. These results do not clear the identified failure and permission paths.

---

## Response to second deep review (2026-09-14)

All four findings were re-verified against the current code before touching
anything (confirmed the exact `fopen(..., "wb")` pattern at all three cited
call sites, reproduced the `remove` failure and the `umask 000` world-writable
result exactly as described). Verification for every item below:
`BUILD_TYPE=Debug just build` (clean, `-Wall -Wextra -Werror`), `just clean &&
just test` (10/10 suites pass, including new regression tests for this round),
the reviewer's own repro steps re-run by hand against the fixed binary, and a
refreshed real install (`~/.local/bin/shimback doctor` reports clean).

### Agreed and fixed

**Critical #1 — atomic writers can create world-writable startup/config
files.** Confirmed and fixed. New shared `write_file_atomic()` (`paths.c`/
`.h`) replaces the three separate `fopen(tmp, "wb")` implementations in
`shell.c`, `config.c`, and (for its own narrower symlink-race issue, since it
already explicitly `chmod`'d — see below) `paths.c`'s `copy_file_mode`. It
uses `mkstemp` (always creates at `0600`, ignoring the umask) and then
`fchmod`s to the mode the caller asked for, applied unconditionally on every
save rather than only at first creation. `config.toml` gets `0600` (it
controls which executables a shim runs); generated shell rc files/fish
snippets get `0644` (ordinary, non-sensitive dotfile content). Re-ran the
reviewer's exact repro (`umask 000`, `add`, inspect `.zshrc` and
`config.toml`) — both are now `0644`/`0600` respectively, never
world-writable.

One design note worth recording: my first pass had `write_file_atomic`
*preserve* an existing destination's mode (matching the reviewer's "preserve
where appropriate" phrasing) rather than always enforcing the caller's mode.
That turned out to be the wrong call — a regression test caught it
propagating a stray `0644` (introduced by an unrelated `cp` elsewhere in the
test suite, itself not `-p`) forward indefinitely instead of correcting it.
Switched to always enforcing the given mode unconditionally: simpler, and
self-healing if anything ever leaves the file at a weaker mode, rather than
perpetuating drift. New regression test in `tests/test_add_remove.sh` (adds
under `umask 000`, asserts both files' permission strings).

**Warning #1 — predictable temporary paths are vulnerable to symlink
replacement.** Confirmed and fixed for every shimback-owned write.
`write_file_atomic`'s use of `mkstemp` (rather than a plain `fopen` on a
`<path>.tmp.<pid>` name) closes this for `shell.c` and `config.c` in the same
change as the mode fix above; `paths.c`'s `copy_file_mode` (already
`chmod`-ing explicitly, so not affected by the Critical finding, but sharing
the same vulnerable temp-file creation) got the same `mkstemp` treatment,
keeping its existing explicit `chmod`. `install.c`'s `download_via_curl` is
a narrower case: curl needs an actual path for `-o`, not a fd we could hand
it directly, so shimback now claims an exclusively-created, unguessable name
via `mkstemp` first (closing the "guess the pid in advance" attack the
reviewer described), then lets curl reopen that same path — which narrows
the window to "attacker unlinks and replaces this exact file in the instant
between our `mkstemp()` and curl's own `open()`" rather than closing it
outright. Documented as a known, accepted residual in the code comment:
fully closing it would mean piping curl's output through a `dup2`'d fd
instead of a path, more machinery than the risk (a downloaded, non-sensitive
man page) justifies right now. Not covered by a dedicated symlink-race
regression test — `mkstemp`'s exclusivity guarantee is a well-established
POSIX primitive, not shimback-specific behavior to lock in with a test, the
same reasoning applied to `xwaitpid`'s `EINTR` retry loop in the first
round.

**Warning #2 — `remove` can leave config and filesystem state
inconsistent.** Confirmed and fixed, mirroring the earlier `add` fix from
round one. `src/commands/remove.c` now only does the read-only check (is
there a shimback-managed symlink to remove) before touching anything;
`config_remove`+`config_save` happen first, and the symlink is only actually
unlinked once that succeeds. Re-ran the reviewer's repro (config directory
`chmod 0500`, then `remove`) — `remove` now fails with the same clear error
and leaves *both* the symlink and the config entry intact together, instead
of the previous "symlink gone, config entry still there" split state. New
regression test in `tests/test_add_remove.sh`.

**Warning #3 — malformed numeric capture settings are silently accepted.**
Confirmed and fixed. New `parse_nonneg_int()` helper in `config.c` (checks
the end pointer, `errno`/`ERANGE`, and the `[0, INT_MAX]` range) replaces the
bare `strtol(value_str, NULL, 10)` for `capture_timeout_ms`, both at the top
level and per-shim; a malformed value is now a clear `CONFIG_ERR_PARSE`
instead of silently becoming `0` (which, as the reviewer noted, would make a
shim cut over to live output immediately rather than ever trying to hide a
failure). Also hardened the CLI-side `add --capture-timeout` parsing in
`add.c`, which had the same missing `errno`/`INT_MAX` checks (an
overflowing value would have hit undefined-ish `(int)` truncation behavior).
`capture_limit`'s own parser (`parse_size_bytes`, added last round) already
rejected malformed input correctly, so it needed no change. `version`'s
parsing has the identical `strtol(..., NULL, 10)` shape but was deliberately
left as-is: it's round-tripped only, never read back to drive any decision,
so a malformed value has no behavioral consequence (unlike
`capture_timeout_ms`, which directly gates dispatch's capture cutover) —
fixing it would be cosmetic, not a real gap. New regression tests in
`tests/test_config_parser.c`: non-numeric, negative, and overflowing
`capture_timeout_ms` at both the global and per-shim level.

### Noted, not independently re-fixed (already-accepted residuals)

Two residual limitations the second review itself flagged as follow-on notes
on already-fixed items, not as new numbered findings:

- **`add`'s remaining non-atomicity**: a `mkdir_p`/`unlink`/`symlink` failure
  *after* a successful config save still leaves the config describing a shim
  with no working symlink. This was already true after round one's fix and
  is unavoidable without genuine cross-filesystem transactions (which round
  one's own response already called out as impractical for this tool). It's
  not a silent failure, though: `doctor` explicitly detects and reports it
  ("no symlink at ... re-run `shimback add`"), which is the accepted,
  visible fallback for the one failure mode that can't be made atomic.
- **A non-`EINTR` `xwaitpid` failure is still unreported** in the
  dispatch/install paths, decoding whatever `status` last held. The review
  itself characterizes this as "not an independently reproducible release
  blocker" — every call site waits on a PID it just forked itself, so a
  genuine non-`EINTR` failure (`ECHILD`, `EINVAL`, ...) essentially can't
  happen in practice here. Left as a documented, accepted gap rather than
  restructuring `run_inherited`/`run_captured`'s void-returning contracts to
  propagate an error that has no realistic trigger in this codebase's
  fork/wait usage.

### Verification summary

`umask 000` no longer produces a world-writable `.zshrc`/`config.toml`
(reviewer's exact repro re-run); a `chmod 0500` config directory makes
`remove` fail cleanly without touching the symlink (reviewer's exact repro
re-run); `capture_timeout_ms = nope`/negative/overflowing values are now
clear parse errors at both config levels and on the CLI; the
`download_via_curl` path (exercised with a fake `curl` standing in, since
this environment has no network access to GitHub) still installs the man
page correctly through the new `mkstemp`-based flow. All 10 test suites
pass (3 new regression tests added this round), man page/README untouched
(no user-facing behavior changed, only internal write-safety), real install
refreshed and `doctor` confirmed clean.

One unrelated finding made in passing while verifying against the real
installed environment: the very first (rate-limited, terminated-early)
code-reviewer subagent from earlier in this session had left
`~/.config/shimback` `chmod`'d to `0555` (mid-way through what was evidently
its own permission-failure repro) and a stray `x` test shim in the real
config, pointing at this repo's test fixtures. Both were artifacts of that
interrupted run, not anything the user did — cleaned up now
(`chmod 0700` restored, `shimback remove x`), verified with a clean
`shimback doctor` afterward.

## Final release assessment (2026-09-14)

The latest fixes address all previously identified release-blocking findings:
atomic writers now use exclusive temporary files and explicit modes, `remove`
preserves state when saving fails, and capture-timeout values are strictly
validated. I independently verified the relevant implementations and found no
new release-blocking issue. The documented residual window in `install` exists
between `mkstemp` and curl reopening the temporary path, but exploitation
requires write access to the destination directory; this is acceptable for
0.1.0 and is substantially safer than the former predictable-name behavior.

**Release recommendation: ready for 0.1.0**, subject to the normal release
packaging/version/tag checks. Verification: clean Release build and full CTest
suite passed 10/10; current Debug build with `-Wall -Wextra -Werror` also passed.
