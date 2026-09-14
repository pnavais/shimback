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

### Not fixed — needs a decision before the next round

**Warning #3 — fallback-policy trials buffer unbounded child output in
memory.** Confirmed as described, **not fixed**. The straightforward-looking
fix (cap `DynBuf` growth per stream, keep draining past the cap to avoid the
deadlock) turns out to have a real correctness cost: `run_captured`'s output
also gets replayed verbatim whenever the trial run succeeds, or fails but
the policy decides not to fall back — both are cases where the *whole
buffer* is the actual output the user is supposed to see, "as if shimback
weren't there at all" (the project's own design promise). A hard cap would
silently truncate that output for any command that's simply chatty and
successful, which is a worse, more surprising failure mode than the
original memory-growth one for a personal, single-user CLI tool wrapping
ordinary commands. The reviewer's other suggested fix — spool to temp files
instead of memory, so size is bounded without sacrificing fidelity — doesn't
have that problem, but is a meaningfully bigger and riskier change to make
this close to a release (temp file lifecycle/cleanup across every exit path,
disk-full handling, replay-from-file instead of replay-from-buffer) than the
other three. Left as-is pending a decision on direction:
1. Hard memory cap with truncation risk on large successful/non-fallback output (small, fast, but a real behavior change).
2. Spool to temp files (no correctness compromise, larger diff, more surface for new bugs).
3. Leave as-is for 0.1.0 (the realistic exposure for a personal CLI wrapping normal commands is low) and revisit post-release.
