# Assessment: "Complete verification review"

**Round:** 2

Round covering 4 findings, all fixed (one of them a genuine race in the
locking mechanism this exact loop introduced last round). Verified with a
clean macOS Debug build (`-Wall -Wextra -Werror`, zero warnings) and a
non-root Docker build on `ubuntu:24.04`, both 12/12 CTest.

## 1. Lock-file cleanup can race a concurrent Shimback command — fixed

Agreed, and a sharp catch on the previous round's own fix. `uninstall`'s
symlink sweep wasn't participating in the locking scheme at all, and then
unconditionally deleted the shared `.shimback.lock` file without holding
it first -- if a concurrent `add`/`remove`/`doctor fix` was mid-operation,
uninstall could yank the lock file out from under it.

`remove_shim_symlinks()` (`src/commands/uninstall.c`) now acquires the
same `shim_dir_lock` before its scan and holds it through the whole
scan-and-removal, bringing uninstall into the same serialization as
everyone else -- closing the larger gap this finding was actually about.

One residual, narrower gap, documented in place rather than silently left:
`flock()` locks the *inode*, not the path, so releasing the lock and then
unlinking the lock file still leaves an unavoidable few-syscall window
where a fresh acquirer elsewhere could open/lock a brand-new inode at the
same path before the unlink runs. Making that fully airtight would mean
never deleting the lock file at all, and teaching every directory-
emptiness assumption elsewhere (including existing tests) to tolerate a
permanent stray file -- disproportionate given how narrow the actual
window is on a single-user CLI tool with no adversarial concurrent user.

## 2. Configuration and shell-file updates are not serialized across processes — fixed (primary case), partially scoped (rest)

Agreed the scope was too narrow. Widened the same `shim_dir_lock` in
`add.c` and `remove.c` to cover the *entire* operation -- config.toml load
through mutate, save, symlink create/replace, and the shell PATH update --
not just the symlink-swap step it originally protected. This directly
closes the primary, highest-frequency lost-update case: two concurrent
`add`/`remove` calls can no longer each load the same stale config.toml
and have one silently discard the other's change.

`doctor fix`'s own final `config_save()` (after its scan/fix loop) is now
also wrapped in the same lock, narrowly around just that save rather than
doctor's whole run -- doctor can sit on an interactive confirmation prompt
for an unbounded time, and holding the lock across that would block every
other shimback command for no good reason. This closes the concurrent-
save race for doctor's own final write, but doesn't fully close a
narrower, separate risk: if something else changes config.toml earlier
during a long interactive doctor session, doctor's own in-memory copy can
still go stale before its own final save. Accepted as lower-priority,
given doctor is a distinctly less frequent, human-supervised operation
than add/remove's much tighter loop.

Explicitly out of scope for this round: `install`/`init`'s own shell-file
writes aren't covered by this lock. They're one-time/infrequent setup
operations with no natural shim-directory context to lock against (a
fresh `init` may run before the shim directory even exists), and are far
less likely to race each other or routine add/remove usage in practice.

Caught during implementation: moving the lock to cover config load
required also moving `mkdir_p(shim_dir)` earlier in `add.c` (before lock
acquisition, since a brand-new shim's directory doesn't exist yet) --
verified this doesn't reintroduce the "dangling symlink with no config
entry" risk the original ordering protected against, since creating an
*empty* directory encodes no shim state; only the symlink itself still
waits until the config is safely saved.

Also caught and fixed during implementation (not part of the original
finding): widening `remove.c`'s lock to cover config load meant moving
`shim_dir`'s computation earlier, but `symlink_path` must NOT move with
it -- `-y`/`--yes` can reassign `name` to a corrected match after a typo
*after* that point, and building `symlink_path` from the pre-correction
name would check and unlink the wrong shim's symlink entirely (caught by
the existing `remove --yes` typo-correction test, which failed until
fixed).

## 3. `install` can overwrite an existing binary without ownership validation — fixed

Agreed, real asymmetry with uninstall's own caution. `install` now refuses
when something already exists at the destination path and it doesn't pass
`looks_like_shimback_binary()`, exactly mirroring uninstall's existing
check. Costs nothing for the normal cases: a fresh install has nothing at
the destination yet, and re-running install to upgrade an existing
shimback binary still passes, since that binary already embeds the
marker.

Reproduced first: `install --prefix` over a directory containing an
unrelated executable named `shimback` used to silently overwrite it.
After the fix it's refused with a clear error, and the file is left
untouched; the normal upgrade-in-place path (re-running install over an
already-installed shimback binary) still works.

## 4. The untracked `test.txt` is unexplained repository content — fixed

Agreed. Confirmed via `git log --all -- test.txt` that it was never
tracked in git history (no commits reference it), so removing it is
lossless. Deleted.
