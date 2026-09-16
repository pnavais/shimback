# Follow-up review: remaining warnings

## Scope and verification

This review checks the latest remediation commit against the previous follow-up
findings. The macOS Debug build succeeds with `-Wall -Wextra -Werror`, and all
12 CTest tests pass. The findings below were independently reproduced in
sandboxed environments.

No remaining Critical security issue was found: uninstall no longer executes
candidate files, and traversal through `remove` is rejected.

## Warning (should address)

### 1. Invalid symlink names can abort `uninstall --full`

**Location:** `src/commands/uninstall.c:282-286`;
`src/paths.c:116-118`

**Problem:** `remove_shim_symlinks()` records every owned symlink name in
`removed_names`, but `uninstall --full` later passes those names to
`remove_split_configs()`. A manually created owned symlink with an invalid name
such as `bad#name` reaches `split_config_filename()`, which calls `die()`.

**Why it matters:** The uninstall operation becomes partial: the symlink is
removed, then the process exits before completing the config sweep and the rest
of the uninstall. I reproduced this with a symlink named `bad#name`; the link
was removed, but `uninstall --full` exited 1 with an unsafe-name error.

**Fix:** Validate names before adding them to `removed_names`, or skip invalid
names while sweeping split configurations and emit a warning. Cleanup should
never abort the entire uninstall because of a malformed filesystem entry.

### 2. Failed `add --split-config` can delete an existing split config

**Location:** `src/commands/add.c:317-345, 352-378`

**Problem:** When `add --split-config` updates an existing shim, it overwrites
the existing split file before creating the symlink. If shim-directory creation
or symlink creation then fails, rollback removes the new split file but does not
restore the previous contents.

**Why it matters:** A failed command can permanently destroy a previously valid
configuration. I reproduced this by creating an existing split config and
blocking the XDG data directory; `add` failed and the prior split config was
gone afterward.

**Fix:** Preserve the previous split file by renaming it to a rollback backup
or storing its contents before the update. Restore it if the filesystem phase
fails; only delete the backup after the symlink operation succeeds.

### 3. Temporary-directory cleanup is unchecked on `fork()` failure

**Location:** `src/commands/install.c:44-55`

**Problem:** If `fork()` fails after `mkdtemp()` succeeds, the code calls
`rmdir(tmpdir)` but ignores its return value and does not report a cleanup
failure.

**Why it matters:** A private temporary directory can be left behind silently.
This is a low-probability resource leak, but repeated failed installs could
accumulate artifacts.

**Fix:** Check and report the cleanup result, consistently with the other exit
paths:

```c
if (rmdir(tmpdir) != 0) {
    warn("install: failed to remove temporary directory %s: %s",
         tmpdir, strerror(errno));
}
```

## Assessment

The major security findings from the earlier rounds appear resolved, and the
full test suite is green. Fixing the first two warnings is recommended before
describing the project as fully release-ready; the third is minor hardening.

## Response

All three findings are agreed with and fixed. I reproduced each exploit/bug
first, confirmed the fix resolves it, added a permanent regression test for
each, and verified with a clean macOS Debug build (`-Wall -Wextra -Werror`,
zero warnings) plus a genuine non-root Docker build on `ubuntu:24.04` -- both
12/12 CTest.

### 1. Invalid symlink names can abort `uninstall --full` -- fixed

Agreed, and while investigating I found the same root cause also reaches
`list`/`doctor`: both call `resolve_shim_entry()` for every name
`collect_all_shim_names()` finds, which includes directory-scanned symlink
names from `list_shim_symlink_names()` -- the exact same unvalidated source
`uninstall --full` uses. A symlink named `bad#name` would have crashed
`shimback list`/`shimback doctor` too, not just `uninstall --full`.

Rather than weaken `split_config_filename()`'s own `die()` (that stays a
correct last-resort invariant check for the callers that *are* supposed to
validate first -- `add.c`, `remove.c`, `config_load()`), I fixed it at the
two entry points that legitimately receive unvalidated, directory-derived
names: `resolve_split_config_path()` (`src/paths.c`) now returns `NULL` for
an invalid name -- correct, since an invalid name can never have a
legitimate split file (`add` refuses to create one), so "not found" is the
right answer, not a crash. `remove_split_configs()` (`src/config.c`) now
warns and returns `0` for an invalid name instead of reaching the `die()`.
This also fixes the `list`/`doctor` exposure, not just the reported
`uninstall --full` case.

Reproduced: `ln -s "$SHIMBACK" "$SHIMDIR/bad#name"` then `uninstall --full`
used to exit 1 partway through, leaving config.toml and PATH blocks
untouched. After the fix it exits 0, prints `skipping split-config cleanup
for invalid shim name 'bad#name'`, and completes the full sweep. New
regression test in `tests/test_uninstall.sh`.

### 2. Failed `add --split-config` can delete an existing split config -- fixed

Agreed. The bug wasn't specific to the symlink-replacement path -- it was
that `config_save_split()`/`config_save()` overwrite their target files
unconditionally, before the filesystem phase (`mkdir_p`/symlink) that can
still fail afterward, and the old rollback only ever undid a *brand-new*
entry, never a pre-existing file's prior content.

Replaced that with a general backup/restore mechanism in `add.c`:
`backup_file_if_exists()` copies whatever's currently at a path (a no-op if
nothing's there yet) before it gets overwritten; `restore_from_backup()`
either restores that backup (an existing file that was updated) or deletes
what was just created (a file that didn't exist before) -- so it correctly
unwinds both "updating an existing shim" and "creating a brand-new one" with
the same logic. This now covers config.toml itself and the split file
uniformly, and every failure point from the config save through the symlink
step (`rollback_and_die()`) restores both before dying. Backups are
discarded once everything succeeds. As a side effect this also fixes the
analogous (unreported) case for a plain, non-split shim being updated: its
config.toml entry is now restored too if a later step fails, not just the
split-file case.

I also moved the "sweep a stale split file left over from an earlier
`add --split-config`" step (for a *non*-split add) to run only after
everything else has succeeded, instead of right after the config save --
otherwise a stale split file could be swept away by an `add` that itself
went on to fail, deconfiguring the shim entirely.

Reproduced exactly as described: created a split-config shim, blocked the
shim directory (a plain file in its place, so `mkdir_p` fails), then ran
`add --split-config` again with a different fallback -- the split file used
to end up permanently overwritten with the new (never-applied) data. After
the fix it's byte-for-byte unchanged, and no `.rollback.*` files are left
behind. New regression test in `tests/test_split_config.sh`.

### 3. Temporary-directory cleanup is unchecked on `fork()` failure -- fixed

Agreed, applied exactly as suggested -- the `fork() < 0` branch in
`download_via_curl()` now checks and warns on a failed `rmdir()`, matching
the other two exit paths in the same function.

### Unrelated: Linux build broke under a newer toolchain

While running the full verification pipeline, a fresh pull of `ubuntu:24.04`
(gcc 13.3.0, glibc 2.39) failed to build at all -- confirmed this reproduces
on the already-reviewed commit too, so it's environment drift, not a
regression from this round's changes. Two issues, both fixed:

- `realpath()` (`src/paths.c`) is no longer visible under `-std=c11
  -D_POSIX_C_SOURCE=200809L` on this glibc; it also needs `_XOPEN_SOURCE=700`
  defined alongside it. Added in `CMakeLists.txt`.
- `add_wizard.c` had several `snprintf()` calls formatting a 4096-byte input
  buffer's contents into a 256-byte error message with a bare `%s`, which
  newer gcc's `-Wformat-truncation` correctly flags as possibly truncating.
  Bounded each with `%.190s` (still room for the surrounding literal text
  within 256 bytes) so it's provably safe rather than just quieter.

Both verified fixed under the same non-root `ubuntu:24.04` Docker build used
for the rest of this round's verification.
