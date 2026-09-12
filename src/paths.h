#ifndef SHIMBACK_PATHS_H
#define SHIMBACK_PATHS_H

#include <stdbool.h>

/* All functions below return a newly allocated string; caller owns it (and,
 * per the project's memory convention, may simply leak it until process
 * exit in short-lived CLI command handlers -- see util.h). NULL is returned
 * only where explicitly documented. */

/* $HOME, or the POSIX passwd-database fallback if unset. Dies if neither
 * source is available -- there's no meaningful way to continue without it. */
char *home_dir(void);

/* $XDG_CONFIG_HOME / $XDG_DATA_HOME, treating unset, empty, or non-absolute
 * values as unset per the XDG Base Directory spec. Returns NULL if unset. */
char *xdg_config_home(void);
char *xdg_data_home(void);

/* <config dir>/shimback/config.toml */
char *config_file_path(void);

/* <data dir>/shimback/bin -- the directory whose symlinks make up the shims,
 * and what gets prepended to PATH. */
char *shim_bin_dir(void);

/* realpath(3) wrapper. Returns NULL if the path doesn't exist / can't be
 * resolved (errno is left as set by realpath). */
char *canonicalize(const char *path);

/* The canonical path of the currently running executable. Platform-specific
 * (macOS: _NSGetExecutablePath; Linux: /proc/self/exe). */
char *self_exe_path(void);

/* The directory portion of `path` (everything before the last '/'), or "."
 * if `path` has no '/'. Newly allocated. */
char *dir_of(const char *path);

bool is_executable_file(const char *path);

/* Copies `src` to `dst` (as an executable, mode 0755), atomically via a
 * temp-file-plus-rename in `dst`'s own directory. Returns false on any I/O
 * failure, leaving `dst` untouched. */
bool copy_executable(const char *src, const char *dst);

/* Same as copy_executable, but mode 0644 -- for non-executable content like
 * a man page. */
bool copy_file(const char *src, const char *dst);

/* Searches $PATH in order for the first executable named `name`, skipping
 * any directory equal to `exclude_dir` (may be NULL for no exclusion) and
 * any candidate that canonicalizes to `exclude_canonical` (may be NULL).
 * Returns NULL if nothing matches. */
char *path_search(const char *name, const char *exclude_dir,
                   const char *exclude_canonical);

/* Resolves a user-supplied command reference (a -s/-f argument, or a
 * doctor-fix replacement typed interactively) to an absolute, executable
 * path: if `arg` is already a valid executable file as given (relative to
 * the current directory, or absolute), that's canonicalized and returned.
 * If it contains no '/' -- a bare command name -- and doesn't resolve that
 * way, it's searched for on $PATH instead (unrestricted: this can resolve
 * to a shim symlink, or to shimback itself -- callers that care about
 * cycles must check the result against self_exe_path() themselves). Returns
 * NULL if neither works. */
char *resolve_binary_arg(const char *arg);

/* mkdir -p equivalent. Returns true on success (including "already exists
 * as a directory"). */
bool mkdir_p(const char *dir);

/* Joins two path components with a single '/', avoiding a double slash if
 * `a` already ends in one. Newly allocated; caller owns it. */
char *path_join(const char *a, const char *b);

#endif /* SHIMBACK_PATHS_H */
