# Complete verification review

**Round:** 2

## Scope and verification

Reviewed the entire current repository: all tracked C sources and headers,
tests, CMake configuration, installer, shell integration, README/build
documentation, and the untracked `test.txt`. The tree is a small C11/POSIX
CLI project (18 implementation files, 12 CTest entries, and the installer).

The current macOS build completed successfully and `ctest --output-on-failure`
passed all 12/12 tests. No Critical findings were verified.

## Prior assessment verification

- **Strict configuration parsing: holds.** `config.c` now applies
  `no_trailing_garbage()` to quoted and array values, uses strict bounded
  integer parsing for `version` and timeout values, and `parse_size_bytes()`
  rejects `ERANGE`. The relevant regression coverage is present.
- **Shell marker matching: holds.** `shell.c` requires complete marker lines at
  line boundaries before editing or removing a block.
- **Ownership-policy consistency: holds.** `looks_like_shimback_binary()` is
  shared by list/doctor/add/remove/uninstall, and the documented limitation
  that the public marker is only a best-effort hint is reasonable for the
  stated single-user, no-privilege-boundary threat model.
- **`add` replacement race fix: holds for Shimback operations.** The
  `.shimback.lock` advisory lock is acquired before the replacement path's
  config work and released after `rename()`. The same lock is used by
  `remove` and the relevant `doctor fix` paths. It does not protect against a
  same-user process that deliberately ignores the lock, which is an explicit
  and reasonable scope limitation here.
- **Dangling-link uninstall defense: reasonable.** The repository documents
  the shim directory as Shimback-owned, and the code explicitly treats a
  dangling link as stale Shimback state while preserving live foreign links.
  That is a deliberate policy choice, not an unverified ownership claim.
- **Lock-file cleanup: holds.** `uninstall` removes `.shimback.lock` before
  attempting `rmdir()`.

## Warning (should address)

### 1. Lock-file cleanup can race a concurrent Shimback command

**Location:** `src/commands/uninstall.c:75-117`, `src/paths.c:505-543`

**Problem:** `uninstall` scans the shim directory, closes the directory, and
  then unconditionally unlinks `.shimback.lock` without acquiring that lock.
  A concurrent `add`, `remove`, or `doctor fix` can hold the lock while
  uninstall deletes the lock file, after which a new command can create a new
  lock inode and acquire it independently. The two commands are then no longer
  serialized.

**Why it matters:** Concurrent teardown can interleave with mutation. For
  example, uninstall may remove a symlink based on an earlier scan while a
  concurrent add is updating the same name, and deleting the lock inode makes
  the intended mutual exclusion unreliable for the remainder of the race.

**Fix:** Acquire the directory lock for uninstall's complete scan/removal and
  hold it until all symlink mutations are complete. Only unlink the lock file
  after releasing it, and preferably only after confirming no concurrent
  operation can recreate/use it; alternatively leave the lock file in place
  permanently and let the directory cleanup tolerate it.

### 2. Configuration and shell-file updates are not serialized across processes

**Location:** `src/commands/add.c:295-505`, `src/commands/remove.c:60-172`,
`src/shell.c:311-385, 713-738`

**Problem:** The new shim-directory lock covers only replacement-path symlink
  mutations. `add` and `remove` can concurrently load and atomically rewrite
  `config.toml`, and shell PATH updates similarly read/merge/write startup
  files without a lock.

**Why it matters:** Two simultaneous commands can both read the same old
  configuration and then atomically rename their independent snapshots into
  place, losing one command's shim or removal. Concurrent PATH updates can
  likewise lose one directory from the merged block. Atomic rename prevents
  partial files but does not prevent lost updates.

**Fix:** Use a lock covering each read-modify-write transaction (ideally a
  config-directory lock for config and a per-startup-file lock for shell
  integration), or implement conflict detection/retry before replacing the
  file. Extend the existing operation lock only if its lifetime and location
  are suitable for all relevant writers.

### 3. `install` can overwrite an existing binary without ownership validation

**Location:** `src/commands/install.c:214-226`, `src/paths.c:375-424`

**Problem:** `install --prefix DIR` always atomically renames the running
  binary over `DIR/bin/shimback`. Unlike `uninstall`, it does not verify that an
  existing destination is a Shimback binary or ask for confirmation.

**Why it matters:** A typo, shared prefix, or an intentionally existing
  unrelated executable named `shimback` is silently replaced. If the prefix is
  writable by another principal, the destination can also change between the
  caller's path construction and rename, causing an unintended overwrite.

**Fix:** Refuse an existing destination that fails
  `looks_like_shimback_binary()` unless an explicit replacement/upgrade option
  is supplied, and validate the prefix ownership/permissions or use a trusted
  installation directory. At minimum, document that `install` deliberately
  overwrites the destination and warn before doing so.

### 4. The untracked `test.txt` is unexplained repository content

**Location:** `test.txt:1` (untracked)

**Problem:** The repository contains an untracked file with the content
  `updated_test`, outside the documented source/test layout.

**Why it matters:** If accidentally included in a release or commit it adds
  unexplained project content; if it is a test artifact, its purpose and
  lifecycle are unclear.

**Fix:** Remove it if it is a local artifact, or add it intentionally with a
  meaningful name, tracked purpose, and corresponding test/documentation.

## Assessment

No Critical issues were found. The previously claimed fixes and defenses hold
as described. The remaining warnings are concurrency hardening and installer
overwrite-policy issues rather than failures reproduced by the current test
suite.
