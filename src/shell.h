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

/* Parses "zsh"/"bash"/"fish" (case-sensitive, matching shell_kind_name's
 * output) into *out. Returns false, leaving *out untouched, for anything
 * else -- including "unknown", which is not a user-facing shell name. */
bool shell_kind_from_name(const char *name, ShellKind *out);

/* Ensures `dir` is prepended to PATH for `kind`. For SHELL_ZSH/SHELL_BASH,
 * via an idempotent marker-block injection into that shell's usual startup
 * file(s); if the block already exists (from an earlier add/init/install
 * call, possibly for a different directory), `dir` is unioned into it
 * rather than overwriting what's there -- so add, init, and install can run
 * in any order, each contributing its own directory, and end up sharing a
 * single block instead of one per caller. For SHELL_FISH, via a dedicated
 * `tag`.fish snippet dropped into fish's conf.d (auto-sourced at startup,
 * so no markers needed -- shimback owns the whole file), unioned the same
 * way. For SHELL_UNKNOWN, prints the line to add manually and touches
 * nothing (always -- that message is the only way the user finds out, so
 * it ignores `verbose`). Returns false only on an actual I/O failure while
 * writing.
 *
 * `verbose` gates only the informational "PATH updated in <file>" line
 * printed on success for SHELL_ZSH/SHELL_BASH/SHELL_FISH -- a failure still
 * always warns, regardless of `verbose`.
 *
 * Equivalent to shell_ensure_path_tagged(kind, dir, "shimback", verbose). */
bool shell_ensure_path(ShellKind kind, const char *dir, bool verbose);

/* Same as shell_ensure_path, but files its marker block (or, for
 * SHELL_FISH, its conf.d snippet) under `tag` instead of the fixed
 * "shimback" tag -- lets a second, independently-managed one (keyed on a
 * different directory set) coexist without colliding with the default. */
bool shell_ensure_path_tagged(ShellKind kind, const char *dir, const char *tag, bool verbose);

/* Removes the marker block (or, for SHELL_FISH, the conf.d snippet file)
 * tagged `tag`, if present, from `kind`'s usual startup file(s) -- the
 * inverse of shell_ensure_path_tagged, used by `uninstall --full`. A no-op
 * for SHELL_UNKNOWN, since shimback never writes anything for it. Returns
 * false only on an actual I/O failure while writing. */
bool shell_remove_path_tagged(ShellKind kind, const char *tag);

/* True if ~/.zshrc.local exists but has no `tag`-marked block while
 * ~/.zshrc does -- i.e. a block written back when ~/.zshrc.local either
 * didn't exist yet or shimback didn't yet prefer it. `shimback doctor`
 * surfaces this as a suggestion (not a failure), and `doctor fix` acts on
 * it via shell_zsh_migrate_block_to_local. */
bool shell_zsh_block_needs_migration(const char *tag);

/* Moves the `tag`-marked block from ~/.zshrc into ~/.zshrc.local, preserving
 * its directory list (each directory is re-unioned in via the same path
 * ensure_dir_in_block would take from a fresh add/init/install call).
 * Only meaningful when shell_zsh_block_needs_migration() is true; returns
 * false if there was no block in ~/.zshrc to move, or on an I/O failure. */
bool shell_zsh_migrate_block_to_local(const char *tag);

#endif /* SHIMBACK_SHELL_H */
