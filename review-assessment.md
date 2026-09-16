# Assessment: "Final verification review"

Round covering 6 findings. 5 fixed, 1 defended. Verified with a clean macOS
Debug build (`-Wall -Wextra -Werror`, zero warnings) and a non-root Docker
build on `ubuntu:24.04`, both 12/12 CTest, including three new regression
tests (one of them a genuine concurrent-process lock contention test, not
just a no-crash check).

## 1. `add` replacement remains vulnerable to a final TOCTOU race — fixed

Agreed. The permission check added in a previous round narrowed the window
but, as flagged, didn't close it against a same-user concurrent process.
Added a real fix: `shim_dir_lock_acquire()`/`shim_dir_lock_release()`
(`src/paths.c`) take an exclusive `flock()` on a `.shimback.lock` file
inside the shim directory. `add` now holds this lock from the initial
ownership check through the final `rename()` whenever it's replacing an
existing symlink, so a concurrent `add`/`remove`/`doctor fix` racing the
same symlink as the same user is serialized against it instead of
interleaving. `remove` and `doctor fix` hold the same lock around their own
check-then-unlink sequences (see finding 3, same underlying mechanism).

This closes the race for concurrent *shimback* operations specifically; it
doesn't (and can't) defend against a process bypassing the lock entirely,
which is a different, out-of-scope threat for a single-user CLI tool with
no privilege boundary.

Caught and fixed during implementation: the new `.shimback.lock` file is a
regular file living inside the shim directory, so it was the one thing
left behind keeping `uninstall`'s directory-cleanup `rmdir()` from
succeeding — added `shim_dir_lock_file_remove()` for uninstall to clean it
up first.

Verified with a genuine concurrent-process test (new, in
`tests/test_add_remove.sh`): a background Python process holds the same
lock via `fcntl.flock()` (the same kernel primitive as the C side's
`flock()`) for 2 seconds; a concurrent `add` replacing the same shim is
timed and confirmed to actually block for that duration rather than
completing instantly, then succeeds once the lock is released.

## 2. `uninstall` treats every dangling symlink as Shimback-owned — defended

Not a bug. The shim directory's own documented contract (README: "nothing
else should ever live there") makes this the correct default, not an
inconsistency with the live-foreign-symlink handling. The two cases aren't
comparable: a live symlink can be positively checked against something
concrete (its target's own bytes via the marker scan), giving real
evidence either way. A dangling symlink has no target left to check at
all — there is no equivalent way to positively prove it *isn't*
shimback's. Any policy for that case is necessarily a default made in the
absence of evidence, and given the directory's own contract, defaulting to
"probably a stale shim" is the choice that actually matches it.

Strengthened the in-code comment on `remove_shim_symlinks` (`uninstall.c`)
to state this reasoning explicitly, so a future reviewer sees the
justification inline rather than needing to reconstruct it.

## 3. `remove` and `doctor fix` have check-then-unlink races — fixed

Agreed, same root cause and same fix as finding 1: both now hold
`shim_dir_lock_acquire()`'s lock across their check-then-mutate sequence.

- `remove.c`: acquires the lock right after computing `symlink_path`
  (tolerating `ENOENT` — nothing to lock if the shim directory doesn't
  exist), holds it through the ownership check and the `unlink()`.
- `doctor.c`'s `fix_symlink_if_needed()` (missing/dangling symlink
  recreation): acquires the lock for the whole check-then-recreate
  sequence.
- `doctor.c`'s orphan-removal path: acquires the lock *after* the
  (potentially indefinitely long, interactive) confirmation prompt, not
  across it — holding a lock across an unbounded wait for user input would
  block every other shimback command for no good reason. Instead, orphan
  status is **re-verified fresh inside the lock**, immediately before the
  `unlink()`, since a concurrent `add` could have legitimately reclaimed
  that exact name while the prompt was waiting; if so, the removal is
  skipped with a warning instead of proceeding on stale information.

Verified via the full test suite (all three lock call sites exercised by
existing add/remove/doctor tests) plus the new concurrent-lock test from
finding 1, which exercises the same underlying mechanism `remove` and
`doctor fix` now share.

## 4. Size parsing accepts overflowing integers — fixed

Agreed, clear bug. `parse_size_bytes()` (`src/util.c`) now sets `errno = 0`
before `strtoull()` and rejects `ERANGE`, exactly as suggested — the
overflow-clamped `ULLONG_MAX` value used to slip past the
multiplier-overflow check below it (`ULLONG_MAX > SIZE_MAX / 1` is false on
a 64-bit system where they're equal).

Reproduced first: `capture_limit = "18446744073709551616"` (one past
`ULLONG_MAX`) used to be silently accepted. After the fix it's rejected
with a clear parse error. New regression tests in both
`tests/test_config_parser.c`'s `test_parse_size_bytes()` (direct,
isolated) and its config-loading tests (per-shim and global
`capture_limit`).

## 5. Shell marker matching can overwrite unrelated startup-file content — fixed

Agreed, real bug. `find_block()` (`src/shell.c`) used a plain `strstr()`
that matched the marker text anywhere in the file, including mid-line
inside unrelated content. Added `find_marker_line()`, which only accepts a
match immediately preceded by start-of-string-or-newline and immediately
followed by newline-or-end-of-string — i.e. the marker must occupy a
complete line by itself, matching how it's actually written.

Reproduced first: a `.zshrc` with a comment mentioning
`# >>> shimback >>>` mid-sentence and a string containing
`# <<< shimback <<<` used to be misidentified as shimback's own block
(the *first* fake occurrence as the start marker, the *second* as the end
marker) — putting both fake lines at risk of being edited or deleted on
the next PATH update. After the fix, `add` correctly appends a real,
separate block leaving the fake content untouched, and a later
`uninstall --full` correctly finds and removes only the real block. New
regression test in `tests/test_add_remove.sh`, using exactly this fixture.

## 6. Symlink ownership is inconsistent after binary relocation or upgrade — fixed

Agreed. `list`/`remove`/`add` recognized a shim symlink only via an exact
match against *this* running binary's own resolved path
(`self_exe_path()`), while `uninstall` used the marker-based scan (any
shimback build, any location) — giving different answers for the same
symlink depending which command was asked, exactly as described.

Unified on the marker-based check everywhere. `looks_like_shimback_binary()`
moved from `uninstall.c` (`static`) to `paths.c` (public, declared in
`paths.h`) so every command can share it. Updated:

- `paths.c`'s `list_shim_symlink_names()` — the function both `list` and
  `doctor` build their shim discovery on — now uses it instead of an
  exact `self_exe` match.
- `add.c`'s ownership check (deciding whether an existing symlink is safe
  to replace) **and** its revalidation-immediately-before-`rename()` check
  (added in a previous round) — both switched together, since leaving one
  on the old exact-match policy while the other used the new one would
  have made `add` refuse its own just-verified symlink.
- `remove.c`'s ownership check.

`doctor.c`'s other `self_exe` comparisons (source/fallback resolving back
to the shimback binary — loop prevention) are a different concern
entirely and were deliberately left unchanged; they're not part of this
finding.

Reproduced first: a shim symlink pointing at a *copy* of the shimback
binary (simulating a relocated/pre-upgrade build) used to be completely
invisible to `list`/`doctor` — not even shown as an orphan — while
`uninstall --full` would still clean it up, exactly the inconsistency
described. After the fix, `list`/`doctor` correctly surface it as an
orphaned (unconfigured) shim. New regression test in `tests/test_list.sh`.
