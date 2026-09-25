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

/* Lists valid shim names inferred from existing <name>-config.toml files in
 * any of the three split-config locations. Newly allocated array of newly
 * allocated names; *out_count receives its length. */
char **list_split_config_names(size_t *out_count);

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

/* True if `entry_path` (an existing filesystem entry, typically found
 * while scanning a shim directory) is one of shimback's own shim links --
 * created by `add`, recognized the same permissive way
 * looks_like_shimback_binary() already documents (any shimback build/
 * install location, not just an exact match against this running binary's
 * own path). POSIX: `entry_path` must be a symlink (deliberately stricter
 * than a bare content check -- only `add` ever creates one here, but any
 * other kind of file could coincidentally exist in the directory too)
 * whose resolved target passes looks_like_shimback_binary(). Windows:
 * shims are hard links, indistinguishable from an ordinary regular file
 * by any syntactic check at all -- the file's own content (the same
 * marker scan) is the only signal available, so `entry_path` is scanned
 * directly, with no resolution step first (a hard link's content already
 * *is* the shared file's content, nothing to follow). This asymmetry is
 * inherent, not a shortcut: Windows genuinely cannot distinguish "a hard
 * link to shimback" from "a plain file containing the same bytes", and
 * doesn't need to -- both are equally valid, functioning shims. */
bool is_shim_dir_entry(const char *entry_path);

/* The filename a shim named `name` should have on disk in the shim
 * directory (POSIX: `name` itself, unchanged; Windows: `name` + ".exe" --
 * cmd.exe/PowerShell only resolve a bare command name against
 * PATHEXT-listed extensions, so an extension-less shim would be invisible
 * to normal invocation there; see windows-port.md Phase 3). Newly
 * allocated. */
char *shim_file_name(const char *name);

/* The reverse of shim_file_name(): the logical shim name for a `filename`
 * found while scanning the shim directory (POSIX: `filename` itself;
 * Windows: `filename` with a trailing ".exe"/".EXE" stripped, if present --
 * symmetric with shim_file_name(), and case-insensitive to match Windows'
 * own filesystem semantics). Newly allocated. */
char *shim_name_from_file(const char *filename);

/* The filename shimback's own installed binary should have on disk
 * (Windows: "shimback.exe"; POSIX: "shimback", unchanged) -- install.c's
 * own destination, uninstall.c's and installation.c's lookup for an
 * existing install, and update.c's extracted release binary all need to
 * agree on this the same way every shim needs shim_file_name(). Just
 * shim_file_name("shimback") under the hood; a separate name only so a
 * call site reads "the shimback binary's own name" rather than the more
 * surprising "shim_file_name of shimback". Newly allocated. */
char *shimback_exe_name(void);

/* Formats a message describing why plat_link_create(target, link_path)
 * failed, given the errno captured immediately after it returned false,
 * into `buf` (at most `bufcap` bytes, always NUL-terminated), prefixed
 * with `cmd_prefix` (e.g. "add", "doctor fix") the way this codebase's
 * other error messages already are. Windows' EXDEV specifically gets a
 * real explanation instead of the CRT's own opaque string for it
 * ("Improper link") -- a hard link (unlike a POSIX symlink, which is just
 * a stored path and never cared what filesystem either side lived on)
 * can't cross drives. By the time this is called, create_shim_link()
 * below has already tried (and failed) to paper over exactly that case
 * with a plain copy, so reaching this for EXDEV specifically means both
 * the link *and* the copy fallback failed -- any other errno falls back
 * to a plain `strerror()`-based message, same as before either of these
 * existed. */
void format_link_create_error(char *buf, size_t bufcap, const char *cmd_prefix,
                               const char *link_path, const char *target, int link_errno);

/* Creates a shim link at `link_path` sharing shimback's own binary
 * (`target`) -- a hard link (plat_link_create()) by default, falling back
 * to a plain copy (copy_executable()) specifically when that fails with
 * EXDEV (Windows only: a hard link can't cross drives, but a copy has no
 * such restriction -- see windows-port.md Phase 6's addendum). Prints a
 * `warn_colored()` notice when the fallback is used, since a copy doesn't
 * share disk space with shimback's binary the way a link does and won't
 * automatically reflect a later `shimback update` either -- and, since
 * `doctor fix`/`update` (unlike `add`/`install`) don't otherwise print
 * any "restart your shell" reminder of their own, a reminder to do so is
 * folded into this same notice so all three callers get it. Returns true on
 * success (a real link, or a successful fallback copy). On failure,
 * `errno` reflects the *original* plat_link_create() failure (even if a
 * fallback copy was attempted and also failed -- that failure is reported
 * separately via a `warn()`), so a caller's own
 * format_link_create_error() call afterward still describes the primary
 * reason. */
bool create_shim_link(const char *target, const char *link_path, const char *cmd_prefix);

/* Atomically replaces the shim link already at `link_path` with a fresh
 * one to `target` -- builds the replacement at a temp name first via
 * create_shim_link() (hard link, falling back to a copy on EXDEV) and
 * swaps it into place with plat_rename_replace(), so this can only ever
 * fully succeed or fail before ever touching the existing link (never
 * leave `link_path` missing in between). For `shimback update` refreshing
 * a shim left stale by a Windows hard link surviving the binary's own
 * atomic replace (or by a copy-fallback shim, which never shares data
 * with shimback's binary at all -- see create_shim_link()'s own comment).
 * `cmd_prefix` is passed through to create_shim_link()'s own fallback
 * warning. Returns false on failure, `errno` reflecting whichever of
 * create_shim_link()/plat_rename_replace() actually failed. */
bool refresh_shim_link(const char *target, const char *link_path, const char *cmd_prefix);

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
