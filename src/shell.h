#ifndef SHIMBACK_SHELL_H
#define SHIMBACK_SHELL_H

#include <stdbool.h>

typedef enum {
    SHELL_ZSH,
    SHELL_BASH,
    SHELL_FISH,
    SHELL_UNKNOWN,
} ShellKind;

/* Detects the current shell from $SHELL's basename. */
ShellKind detect_current_shell(void);

/* Whether `kind`'s binary is reachable on PATH (used by `init` to decide
 * which shells to configure). Meaningless for SHELL_UNKNOWN. */
bool shell_is_installed(ShellKind kind);

const char *shell_kind_name(ShellKind kind);

/* Ensures `shim_dir` is prepended to PATH for `kind`, via an idempotent
 * marker-block injection into that shell's usual startup file(s). For
 * SHELL_FISH, prints a note and touches nothing (unsupported in v0.1.0).
 * For SHELL_UNKNOWN, prints the line to add manually and touches nothing.
 * Returns false only on an actual I/O failure while writing.
 *
 * Equivalent to shell_ensure_path_tagged(kind, shim_dir, "shimback"). */
bool shell_ensure_path(ShellKind kind, const char *shim_dir);

/* Same as shell_ensure_path, but files its marker block under `tag` instead
 * of the fixed "shimback" tag -- lets a second, independently-managed PATH
 * entry (e.g. shimback's own install directory, injected by `install`)
 * coexist in the same startup file without colliding markers with the
 * shim-dir block. */
bool shell_ensure_path_tagged(ShellKind kind, const char *dir, const char *tag);

/* Removes the marker block tagged `tag` (if present) from `kind`'s usual
 * startup file(s) -- the inverse of shell_ensure_path_tagged, used by
 * `uninstall --full`. A no-op for SHELL_FISH/SHELL_UNKNOWN, since shimback
 * never writes a block for those. Returns false only on an actual I/O
 * failure while writing. */
bool shell_remove_path_tagged(ShellKind kind, const char *tag);

#endif /* SHIMBACK_SHELL_H */
