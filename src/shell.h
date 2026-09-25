#ifndef SHIMBACK_SHELL_H
#define SHIMBACK_SHELL_H

#include <stdbool.h>

#include "util.h"

typedef enum {
    SHELL_ZSH,
    SHELL_BASH,
    SHELL_FISH,
    SHELL_POWERSHELL,
    SHELL_CMD,
    SHELL_UNKNOWN,
} ShellKind;

/* Detects the current shell (POSIX: from $SHELL's basename; Windows: from
 * the immediate parent process's image name -- powershell.exe/pwsh.exe ->
 * SHELL_POWERSHELL, cmd.exe -> SHELL_CMD, anything else -> SHELL_UNKNOWN,
 * since Windows has no $SHELL-style env var). */
ShellKind detect_current_shell(void);

/* Whether `kind`'s binary is reachable on PATH (used by `init` to decide
 * which shells to configure). Meaningless for SHELL_UNKNOWN. For
 * SHELL_POWERSHELL, true if either edition (`pwsh` or `powershell`) is
 * found. */
bool shell_is_installed(ShellKind kind);

const char *shell_kind_name(ShellKind kind);

/* Every shell kind this platform can configure (POSIX: zsh, bash, fish;
 * Windows: PowerShell, cmd), for callers that want to sweep all of them
 * (install --all, init, uninstall, find_installations) without duplicating
 * the platform split themselves. Fills `out` (must have room for at least
 * 3 entries) and returns the count actually written. */
size_t shell_all_kinds(ShellKind *out);

/* Parses "zsh"/"bash"/"fish"/"powershell"/"cmd" (case-sensitive, matching
 * shell_kind_name's output) into *out. Returns false, leaving *out
 * untouched, for anything else -- including "unknown", which is not a
 * user-facing shell name. */
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
 * way. For SHELL_POWERSHELL, via the same idempotent marker-block strategy
 * injected into $PROFILE.CurrentUserAllHosts for every installed edition
 * (Windows PowerShell and/or PowerShell 7+, whichever are found on PATH --
 * both get it if both are installed, so it works regardless of which one
 * ends up running). For SHELL_CMD, via the same marker-block strategy
 * (using `rem` instead of `#`, cmd.exe's own comment syntax) merged into
 * the HKCU\Software\Microsoft\Command Processor\AutoRun registry string.
 * SHELL_POWERSHELL/SHELL_CMD also persist `dir` into HKCU\Environment\Path
 * as a fallback layer for contexts that don't load a profile/AutoRun (see
 * platform.h's plat_win_userenv_path_add) -- necessary for reachability
 * there, but not by itself sufficient for first-in-PATH ordering, which
 * the profile/AutoRun injection is what actually delivers. For
 * SHELL_UNKNOWN, prints the line to add manually and touches nothing
 * (always -- that message is the only way the user finds out, so it
 * ignores `verbose`). Returns false only on an actual I/O failure while
 * writing.
 *
 * `verbose` gates only the informational "PATH updated in <file>" line
 * printed on success for SHELL_ZSH/SHELL_BASH/SHELL_FISH/SHELL_POWERSHELL/
 * SHELL_CMD -- a failure still always warns, regardless of `verbose`.
 *
 * Equivalent to shell_ensure_path_tagged(kind, dir, "shimback", verbose). */
bool shell_ensure_path(ShellKind kind, const char *dir, bool verbose);

/* Same as shell_ensure_path, but files its marker block (or, for
 * SHELL_FISH, its conf.d snippet) under `tag` instead of the fixed
 * "shimback" tag -- lets a second, independently-managed one (keyed on a
 * different directory set) coexist without colliding with the default. */
bool shell_ensure_path_tagged(ShellKind kind, const char *dir, const char *tag, bool verbose);

/* Removes the marker block (or, for SHELL_FISH, the conf.d snippet file;
 * for SHELL_POWERSHELL, the block from every profile touched by
 * shell_ensure_path_tagged; for SHELL_CMD, the block from the AutoRun
 * registry string) tagged `tag`, if present, from `kind`'s usual startup
 * location(s) -- the inverse of shell_ensure_path_tagged, used by
 * `uninstall --full`. SHELL_POWERSHELL/SHELL_CMD also remove each of that
 * block's directories from HKCU\Environment\Path. A no-op for
 * SHELL_UNKNOWN, since shimback never writes anything for it. Returns
 * false only on an actual I/O failure while writing. */
bool shell_remove_path_tagged(ShellKind kind, const char *tag);

/* Appends to `out` every directory recorded in `kind`'s PATH block tagged
 * `tag`, across every location where that shell's block may live (zsh:
 * both ~/.zshrc.local and ~/.zshrc; bash: .bashrc/.bash_profile/.profile;
 * fish: its conf.d snippet; powershell: both editions' profiles; cmd: the
 * AutoRun registry string). Read-only, and best-effort: a shell with no
 * such block, or an unparseable one, just contributes nothing. Entries may
 * repeat across locations; callers dedupe. */
void shell_read_block_dirs(ShellKind kind, const char *tag, StrVec *out);

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
