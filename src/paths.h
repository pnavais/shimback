#ifndef SHIMBACK_PATHS_H
#define SHIMBACK_PATHS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h> /* mode_t */

/* All functions below return a newly allocated string; caller owns it (and,
 * per the project's memory convention, may simply leak it until process
 * exit in short-lived CLI command handlers -- see util.h). NULL is returned
 * only where explicitly documented. */

/* Allowlist, not a denylist, for a shim name: ASCII letters, digits, '.',
 * '_', '+', '-', and not the literal "shimback". A shim name ends up
 * interpolated verbatim into a `[shims.<name>]` TOML section header
 * (config.c's render_config) and into a `<name>-config.toml` filename
 * (split_config_filename, below), so anything outside this set risks
 * corrupting either -- most notably '#' (config.c's parser treats it as
 * a comment marker even inside a section header, silently truncating it
 * on the next load and making the *entire* config unloadable, not just
 * that one shim) and '/' or '..' (a path-traversal component that would
 * otherwise reach outside shimback's own directories once joined into a
 * path -- see split_config_filename). Lives here rather than in a
 * command-level file so config.c's config_load can enforce the same rule
 * on a hand-edited config.toml's own [shims.<name>] headers, and so
 * split_config_filename itself can enforce it defensively -- config.c
 * already depends on paths.c, so this is the only direction that doesn't
 * create a dependency cycle. */
bool is_valid_shim_name(const char *name);

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

/* "<name>-config.toml" -- the filename (not a path) a shim's split config
 * file must have, at whichever of the three split_config_all_paths
 * locations it lives in. Newly allocated. */
char *split_config_filename(const char *name);

/* Fills paths[0..2] with the three locations, newly allocated and in
 * resolve_split_config_path's priority order, where a "<name>-config.toml"
 * split config file for shim `name` may live: the shim's own symlink
 * directory (shim_bin_dir()), the real shimback binary's own directory
 * (dir_of(self_exe_path())), and the shimback config directory
 * (dir_of(config_file_path())). Does not check whether any of them
 * actually exist -- for callers (remove, uninstall --full, add's
 * non-split cleanup) that need to sweep every potential location rather
 * than just resolve the one that currently wins. */
void split_config_all_paths(const char *name, char *paths[3]);

/* Searches split_config_all_paths's three locations, in order, for an
 * existing "<name>-config.toml" and returns the first one found (newly
 * allocated), or NULL if none exist. The order is deliberately
 * most-specific-first: a copy living right next to the shim's own symlink
 * wins over one next to the shimback binary, which in turn wins over the
 * one in the shared config directory (where `add --split-config` always
 * writes it initially) -- so moving a copy to a more specific location is
 * how you override, without having to touch or delete the original. */
char *resolve_split_config_path(const char *name);

/* Lists every shimback-managed symlink directly inside shim_bin_dir() --
 * i.e. every real shim, whether or not it has a config.toml entry or a
 * split config file anywhere (`list`/`doctor` need this to notice a
 * split-only shim, or one with no configuration anywhere at all -- an
 * orphan). A dangling symlink, or one pointing at something other than
 * the running shimback binary, is not included (mirrors the same check
 * `remove`/`uninstall` use to decide a symlink is "ours"). Newly
 * allocated array of newly allocated names (not full paths); *out_count
 * receives its length (0/NULL if none, including when the directory
 * itself doesn't exist yet). */
char **list_shim_symlink_names(size_t *out_count);

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

/* Best-effort check that `path` is actually a shimback binary (any
 * version/build, any install location), by statically scanning its own
 * bytes for a fixed marker every build embeds -- never executes the
 * candidate. Shared by every command that needs a consistent answer to
 * "is this shimback's" for the same symlink/binary (see paths.c). Not
 * authenticated ownership proof; see paths.c for the full reasoning. */
bool looks_like_shimback_binary(const char *path);

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

/* Best-effort resolution for `add --force`: makes a path-shaped `arg`
 * (one containing '/') absolute without requiring it to exist yet -- a
 * relative path is joined with the current working directory, an already
 * absolute one is returned as-is. Purely lexical (no symlink resolution,
 * no collapsing of "." / ".."), unlike canonicalize(), which needs the
 * full path to already exist. Returns NULL for a bare name (no '/'):
 * there's no $PATH to search against something that doesn't exist
 * anywhere yet, so there's nothing meaningful to resolve it to. */
char *force_resolve_binary_arg(const char *arg);

/* mkdir -p equivalent. Returns true on success (including "already exists
 * as a directory"). */
bool mkdir_p(const char *dir);

/* Acquires/releases an exclusive advisory lock on a shim directory, so
 * add/remove/doctor fix can serialize their own check-then-mutate
 * sequence on a shim symlink against each other (see paths.c). Returns an
 * fd, or -1 on failure; release() is a no-op on -1. `shim_dir` must
 * already exist. */
int shim_dir_lock_acquire(const char *shim_dir);
void shim_dir_lock_release(int fd);

/* Removes the lock file shim_dir_lock_acquire() creates inside `shim_dir`,
 * if present. For uninstall's own cleanup, once every shim symlink is
 * gone, before it tries to rmdir() the now-supposedly-empty directory. */
void shim_dir_lock_file_remove(const char *shim_dir);

/* Joins two path components with a single '/', avoiding a double slash if
 * `a` already ends in one. Newly allocated; caller owns it. */
char *path_join(const char *a, const char *b);

/* Writes `data` (length `len`) to `path` atomically: via a securely-created
 * temp file (mkstemp, so it's immune to a pre-planted symlink at a
 * predictable "<path>.tmp.<pid>" name -- a plain fopen() would silently
 * follow such a symlink and write through it) in the same directory as
 * `path`, then rename() into place, so a reader never observes a partially-
 * written file. `mode` is always applied explicitly and unconditionally --
 * never left to whatever the umask happens to be, and not preserved from
 * whatever `path` previously had, so a call site's own chosen mode is
 * self-healing: anything that ever leaves `path` with a weaker mode (a
 * permissive umask on some other writer, a stray hand copy, ...) is
 * corrected back on the next call rather than silently perpetuated.
 * Returns false on any I/O failure, leaving `path` untouched. */
bool write_file_atomic(const char *path, const char *data, size_t len, mode_t mode);

#endif /* SHIMBACK_PATHS_H */
