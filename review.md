# Outstanding issues from final verification

## Verification status

The latest remediation fixes were verified against the current source. The
macOS Debug build succeeds with `-Wall -Wextra -Werror`, and all 12 CTest tests
pass. The previously reported Critical issues were not reproduced; uninstall
no longer executes candidate files, traversal through `remove` is rejected,
invalid symlink names no longer abort cleanup, and failed split-config updates
restore their prior contents.

## Warning (should address)

### 1. Static binary marker is forgeable

**Location:** `src/commands/uninstall.c:39-90`;
`src/version.h.in:6-15`

**Problem:** Uninstall treats any executable containing the public
`SHIMBACK_BINARY_MARKER` byte sequence as Shimback-owned.

**Why it matters:** A foreign executable can embed the same marker and be
incorrectly deleted by `uninstall --prefix`, or a foreign symlink target can be
misclassified as a Shimback shim and removed. This is no longer a code
execution issue, but it is not strong ownership proof.

**Fix:** Use trusted installation metadata, or record and verify a trusted
cryptographic hash/device identity. If the marker remains intentional, document
it as a best-effort identification hint rather than authenticated ownership.

### 2. Malformed scalar configuration values are accepted

**Location:** `src/config.c:418-425, 462-470, 569-581, 764-766`

**Problem:** Scalar parsers do not require the remainder of a value to contain
only permitted whitespace after parsing. For example:

```toml
fallback = "/bin/echo" garbage
```

is accepted instead of rejected.

**Why it matters:** Operator mistakes and malformed configuration can be
silently accepted and later normalized when Shimback rewrites the file, hiding
configuration corruption.

**Fix:** After parsing a quoted string or scalar, skip whitespace and require
`*cursor == '\0'`. Parse the top-level `version` with the same strict integer
validation used for other numeric fields.

### 3. Existing symlink replacement remains raceable

**Location:** `src/commands/add.c:237-249, 411-423`

**Problem:** `add` checks that an existing symlink is Shimback-managed, then
later creates a replacement and calls `rename()` over the path. The directory
entry is not held or revalidated between the ownership check and replacement.

**Why it matters:** A concurrent local process that can modify the shim
directory can replace the checked entry in between those operations. The later
`rename()` can overwrite a different file or symlink, causing data loss.

**Fix:** Enforce that the shim directory is trusted and owner-only writable,
or use descriptor-relative filesystem operations with revalidation immediately
before replacement. At minimum, reject or warn about insecure directory
ownership and permissions.

## Assessment

No remaining Critical issue was found, and the project is substantially safer.
The project is release-ready for the current threat model, but the three
warnings above remain worthwhile hardening and correctness improvements.

## Response

Findings 2 and 3 are agreed with and fixed. Finding 1 is defended -- see
below for the reasoning. Verified with a clean macOS Debug build
(`-Wall -Wextra -Werror`, zero warnings) plus a genuine non-root Docker build
on `ubuntu:24.04`, both 12/12 CTest, and reproduced each of findings 2 and 3
before fixing them.

### 1. Static binary marker is forgeable -- defended

Agreed that the marker isn't cryptographic proof of ownership -- it's a
fixed, public byte sequence readable with `strings` on any shimback binary,
so nothing stops a different file from embedding it. Not hardening this
further is a deliberate choice, though, not an oversight: the two ways to
strengthen it both land on the same problem. Recording trusted installation
metadata (a manifest, a hash) alongside the binary is exactly as forgeable
as the marker itself, since it would live in the same directory an attacker
would need write access to in order to plant a convincing marker forgery in
the first place -- and someone with that write access can already delete or
replace the target file directly, with no need to trick `uninstall` into
doing it for them. A real cryptographic identity check would close that
gap, but shimback is a single-user CLI tool with no privilege boundary to
defend, so that's disproportionate to the actual risk here.

Taking the fix you offered as an acceptable resolution ("document it as a
best-effort identification hint rather than authenticated ownership"):
expanded the comment on `looks_like_shimback_binary()` in `uninstall.c` to
say this explicitly, including the reasoning above, and added a short note
to the `uninstall` section of the README so it's visible without reading
the source.

### 2. Malformed scalar configuration values are accepted -- fixed

Agreed, and the same gap existed across every cursor-based value in
`parse_shim_entry_field()` -- not just the four locations cited, but also
`source_args`, `fallback_args`, `exit_codes`, `error_patterns`,
`route_args`, `source_route_args`, `fallback_route_args`, `rewrite_from`,
and `rewrite_to`. Each of those calls `parse_string_array()`/
`parse_int_array()`, which correctly stop at the closing `]` but, like the
quoted-string fields, never checked whether anything followed it.

Added a shared `no_trailing_garbage()` helper (`src/config.c`) -- skips
whitespace and requires end-of-string, since an inline comment is already
stripped before parsing starts -- and applied it after every cursor-based
parse in `parse_shim_entry_field()` and both `capture_limit` call sites.
Also replaced the top-level `version` key's unchecked `strtol()` with
`parse_nonneg_int()`, the same strict integer parser already used for
`capture_timeout_ms`, exactly as suggested.

Reproduced with `fallback = "/usr/bin/sed" garbage`, which used to load
successfully with " garbage" silently dropped; now rejected with `line N:
expected a string for 'fallback'`. Also verified for a trailing-garbage
array, `capture_limit`, and `version`, and that ordinary well-formed
configs still parse unchanged. New regression tests in
`tests/test_config_parser.c` covering a quoted-string field, `policy`, an
array field, `capture_limit` (both scopes), and `version`.

### 3. Existing symlink replacement remains raceable -- fixed

Agreed. Two changes in `add.c`, matching the "at minimum" bar from the
suggested fix:

- Right after confirming an existing symlink is shimback-owned, refuse to
  proceed if the shim directory is writable by anyone other than its
  owner (`chmod go-w` fixes it). This doesn't close the window by itself,
  but it removes the actual precondition the race needs -- another user
  able to write into the directory at all.
- Immediately before the final `rename()` that swaps the replacement
  symlink into place, re-verify `symlink_path` is still a symlink still
  resolving to the shimback binary, refusing the replacement (with
  rollback) if it changed. This shrinks the TOCTOU window from "however
  long the config save took" down to the handful of syscalls between the
  recheck and `rename()` itself -- not a full descriptor-relative
  rewrite, but as close as a plain `rename()` gets.

Verified the directory-permission check: made an existing shim's directory
group/other-writable, confirmed `add` on that shim now refuses with
"writable by more than just its owner" instead of proceeding, and that it
succeeds again once owner-only permissions are restored. New regression
test in `tests/test_add_remove.sh`.
