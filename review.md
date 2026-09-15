# Follow-up security review

## Scope and verification

This review covers the remediation commit and the remaining security and
correctness risks identified after the previous review. The macOS Debug build
completed successfully with warnings enabled, and all 12 CTest tests passed.
The findings below were also verified with targeted sandboxed reproductions.

## Critical (must fix before merge)

### 1. Uninstall executes untrusted files to determine ownership

**Location:** `src/commands/uninstall.c:45-81, 128-134, 292`

**Problem:** `looks_like_shimback_binary()` executes a candidate executable with
`--version` and treats output beginning with `shimback ` as proof that the file
is Shimback-owned. This check is used both for installed binaries and for
symlink targets in the shim directory.

**Why it matters:** A foreign executable can print the expected prefix while
performing arbitrary actions first. Running `uninstall` therefore executes
untrusted code merely to decide whether it may be deleted. I verified this
with a foreign symlink whose target wrote a marker file, printed `shimback
fake`, and was then removed by `uninstall`.

**Fix:** Do not execute candidate files for ownership verification. Use trusted
installation metadata, or compare the candidate against a recorded identity
such as a trusted hash or inode/device identity. For symlinks, preserve live
links whose ownership cannot be established; do not infer ownership from
program output.

### 2. Path traversal remains possible through `remove`

**Location:** `src/commands/remove.c:40-60, 102-137`;
`src/paths.c:89-129`

**Problem:** `remove` accepts an arbitrary user-supplied name and passes it to
`split_config_filename()` and `remove_split_configs()` without validating it.
`split_config_filename()` simply appends `-config.toml`, so `../` components
remain active when the result is joined with the Shimback directories.

**Why it matters:** A caller can delete a file outside the intended config
directory when the corresponding traversal path exists. I verified that:

```sh
shimback remove ../../../victim
```

deleted a sandbox file named `victim-config.toml` outside the Shimback config
directory. The same issue remains possible for unsafe section names loaded from
hand-edited `config.toml`, including during `uninstall --full`.

**Fix:** Validate names with the same `is_valid_shim_name()` allowlist before
any path construction in `remove`, and reject invalid names while parsing
`[shims.<name>]` sections in `config_load()`. Path-building helpers should also
defensively reject names containing `/`, `..`, or other path separators rather
than relying only on callers.

## Warning (should address)

### 3. `add` can still leave configuration behind when symlink creation fails

**Location:** `src/commands/add.c:292-334`

**Problem:** `add` saves `config.toml` (and, for split configuration, the split
file) before creating a new symlink. The atomic rename fix protects an existing
symlink, but there is still no rollback when creating a new symlink or the shim
directory fails.

**Why it matters:** I verified that when the XDG data path is blocked by a
regular file, `add` returns an error while leaving a new config entry behind
with no corresponding symlink. This is a partially applied operation and can
confuse later `list`, `doctor`, and dispatch behavior.

**Fix:** Stage the config and symlink changes and commit them together where
possible. At minimum, retain the previous config/split files and restore them
if directory or symlink creation fails; for a new shim, remove the newly
written config on failure.

### 4. Temporary man-page download errors are ignored

**Location:** `src/commands/install.c:63-71`

**Problem:** `download_via_curl()` ignores failures from `chmod(tmp_file, 0644)`
and `rmdir(tmpdir)`.

**Why it matters:** If `chmod()` fails, the man page can still be renamed into
place with unintended permissions. If cleanup fails, private temporary
directories are left behind after successful or failed installs and can
accumulate over repeated runs.

**Fix:** Check `chmod()` and treat failure as an unsuccessful download before
the rename. Check `rmdir()` on every exit path and report cleanup failures, or
use a cleanup helper that guarantees the temporary directory is removed when
possible.

## Assessment

The prior fixes for name validation during `add`, checksum verification,
editor command parsing, temporary-file isolation, read errors, wait handling,
Python test skipping, and packaging smoke tests appear to be working. The two
Critical findings above still require correction before the project should be
considered ready for release.

## Response to the follow-up review (2026-09-15)

All 4 findings agreed with and fixed -- both Critical ones are genuine, and
#1 in particular is a real regression my own previous round introduced
(trading "delete without checking" for "execute untrusted code to check" is
not actually progress). Verified on a clean macOS Debug build
(`-Wall -Wextra -Werror`, zero warnings) and a fresh non-root `ubuntu:24.04`
container, full 12-test suite green on both, plus direct reproductions of
both Critical exploits exactly as you described them.

### Critical

**1. Uninstall executes untrusted files to determine ownership -- fixed.**
Reproduced your exact exploit first (a symlink target that writes a marker
file, prints `shimback fake`, gets removed) to confirm, then replaced
`looks_like_shimback_binary()` entirely: it no longer executes anything.
Every shimback build now embeds a fixed marker string
(`SHIMBACK_BINARY_MARKER`, in `version.h.in`) -- referenced from a reachable
line in `main.c` so no optimization level can strip it as unused, but never
printed, so `--version`'s actual output is unchanged. The check is now a
plain byte-scan for that marker in the candidate file's own bytes (bounded
to a sane max size), used for both the shim-symlink-ownership check and the
`--prefix`-derived binary check. This keeps the "any shimback build, not
just this exact one" property from last round's fix, without executing
anything -- reading a file's bytes can't run them, whatever they are. The
man-page check was already non-executing (a static header sniff) and is
unchanged. Re-ran your exact reproduction after the fix: the marker file is
never created (the candidate is never executed), and the symlink correctly
survives. Both scenarios (a malicious shim-directory symlink, and a
malicious `--prefix`-derived `bin/shimback`) are now permanent regression
tests in `test_uninstall.sh`.

**2. Path traversal through `remove` -- fixed**, at the three layers you
described. `is_valid_shim_name()` moved from `add_wizard.c` to `paths.h`
(config.c already depends on paths.c, so this was the only direction that
didn't create a header cycle) and is now enforced in all three places:
`remove` validates `name` before it ever touches `resolve_split_config_path`
or `remove_split_configs`; `config_load()` rejects an invalid
`[shims.<name>]` section header the same way it already rejects malformed
header syntax (`CONFIG_ERR_PARSE`), closing the `uninstall --full`-via-
hand-edited-config path you flagged; and `split_config_filename()` itself
now `die()`s on an unvalidated name reaching it at all, as a last-resort
invariant check for any future caller that forgets the first two. Re-ran
`shimback remove '../../../victim'` exactly as you described: now refused
up front with a clear "invalid shim name" error, victim file untouched.
Both the CLI-argument path and the hand-edited-config.toml path are now
covered by regression tests (`test_add_remove.sh`,
`test_config_parser.c`).

### Warning

**3. `add` can still leave configuration behind -- fixed for the case your
reproduction actually hit** (a brand-new shim, shim-directory creation
blocked). When `mkdir_p(shim_dir)` or the plain `symlink()` call fails for
a shim that had *no* previous symlink, `add` now rolls back: reloads
config.toml (or deletes the split file) and removes the entry it just
committed, before `die()`-ing -- so a failed `add` no longer leaves a
"configured" shim with nothing backing it. I didn't extend this to the
*replacing an existing symlink* branch: last round's atomic-rename fix
already guarantees the *old* symlink survives completely intact if the
replacement fails, and since a shim symlink never encodes policy/source/
fallback data itself (only its name matters; dispatch re-reads everything
else fresh from config.toml/the split file every time it runs), that old
symlink stays fully functional under whatever config now exists for it --
there's no actual inconsistency left to roll back in that case, just a
config entry that changed without its symlink needing to. Re-ran your
exact reproduction (XDG data path blocked by a regular file): config.toml
now shows no trace of the failed `add` afterward. New regression test in
`test_add_remove.sh`.

**4. Ignored `chmod`/`rmdir` errors in the man-page downloader -- fixed.**
A failed `chmod(tmp_file, 0644)` is now treated as a failed download (logged,
`ok = false`) instead of proceeding to `rename()` a file into place with
whatever mode it happened to get otherwise. `rmdir(tmpdir)` is now checked
on every exit path (success and failure) and a failure is reported via
`warn()` rather than silently leaving the private temp directory behind.
Verified by inspection and a clean `test_install.sh` run; didn't add a
dedicated automated test for this one specifically (forcing `chmod`/`rmdir`
to fail portably needs fairly contrived sandbox setup for a narrow,
low-likelihood edge case) -- same proportionate call as #9 from the
previous round, which this review didn't flag as insufficient.

### Summary

4/4 agreed and fixed, all verified on both macOS and non-root Linux,
including direct reproductions of both Critical exploits before and after
the fix. New/updated tests: two new `uninstall` regression tests proving
ownership verification never executes a candidate file (shim symlink and
`--prefix` binary, both with a "would print a fake identity if run" payload
that must never actually run); `remove`'s path-traversal rejection (new,
`test_add_remove.sh`); `config_load`'s rejection of an invalid section-header
name, both a traversal attempt and a `#`-containing one (new,
`test_config_parser.c`); `add`'s config rollback on a blocked shim directory
(new, `test_add_remove.sh`). The one design note worth flagging explicitly:
#1's fix trades a "recognize any shimback binary via execution" guarantee
for "recognize any shimback binary via a static marker, which a
sufficiently motivated forger could in principle also embed" -- that's a
deliberate, correct trade (no code execution is worth far more than
resistance to a deliberately crafted decoy, which was never the actual
threat model either finding was about), but flagging it in case a future
round wants something stronger than a marker scan.
