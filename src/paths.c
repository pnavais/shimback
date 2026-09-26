#include "paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/platform.h"
#include "util.h"
#include "version.h"

char *path_join(const char *a, const char *b) {
    size_t alen = strlen(a);
    bool needs_sep = alen > 0 && a[alen - 1] != '/';
    size_t total = alen + (needs_sep ? 1 : 0) + strlen(b) + 1;
    char *result = xmalloc(total);
    snprintf(result, total, "%s%s%s", a, needs_sep ? "/" : "", b);
    return result;
}

static bool is_shim_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '+' || c == '-';
}

bool is_valid_shim_name(const char *name) {
    if (name[0] == '\0' || strcmp(name, "shimback") == 0) {
        return false;
    }
    for (const char *p = name; *p != '\0'; p++) {
        if (!is_shim_name_char(*p)) {
            return false;
        }
    }
    return true;
}

char *home_dir(void) {
    return plat_home_dir();
}

static bool path_looks_absolute(const char *path) {
#ifdef _WIN32
    /* Windows has no single leading-character convention for "absolute"
     * the way POSIX's leading '/' is: either a drive letter ("C:\" /
     * "C:/") or a UNC path ("\\server\share\..."). */
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
        return true;
    }
    return path[0] == '\\' && path[1] == '\\';
#else
    return path[0] == '/';
#endif
}

static char *xdg_env(const char *name) {
    const char *val = getenv(name);
    if (!val || val[0] == '\0' || !path_looks_absolute(val)) {
        return NULL;
    }
    return xstrdup(val);
}

char *xdg_config_home(void) {
    return xdg_env("XDG_CONFIG_HOME");
}

char *xdg_data_home(void) {
    return xdg_env("XDG_DATA_HOME");
}

static bool path_is_dir(const char *path);

#ifdef _WIN32
/* Windows-only fallback used when no XDG_*_HOME override is set. Prefers
 * the same ".config"/".local/share" (under %USERPROFILE%) layout macOS/
 * Linux already use -- deliberately, not %APPDATA%/%LOCALAPPDATA%, so a
 * config shared across WSL/Git-Bash/MSYS2 and native Windows (or just a
 * dotfiles repo already set up that way) keeps working without a separate
 * Windows-only location. If that "<base>/shimback" directory doesn't
 * exist yet, but %APPDATA%/%LOCALAPPDATA%'s does (an install made back
 * when that was the default), keeps using that instead -- so this default
 * doesn't strand an existing install. A fresh install, or one where
 * neither exists yet, gets the home-based default. */
static char *win_base_dir(const char *env_name, const char *home_leaf) {
    char *home = home_dir();
    char *home_base = path_join(home, home_leaf);
    free(home);

    char *home_shimback_dir = path_join(home_base, "shimback");
    bool home_exists = path_is_dir(home_shimback_dir);
    free(home_shimback_dir);

    if (!home_exists) {
        const char *appdata_env = getenv(env_name);
        if (appdata_env && appdata_env[0] != '\0') {
            char *appdata_shimback_dir = path_join(appdata_env, "shimback");
            bool appdata_exists = path_is_dir(appdata_shimback_dir);
            free(appdata_shimback_dir);
            if (appdata_exists) {
                free(home_base);
                return xstrdup(appdata_env);
            }
        }
    }
    return home_base;
}
#endif

static char *base_dir(char *(*xdg_fn)(void), const char *win_env_name,
                       const char *fallback_leaf) {
    char *base = xdg_fn();
    if (base) {
        return base;
    }
#ifdef _WIN32
    return win_base_dir(win_env_name, fallback_leaf);
#else
    (void)win_env_name;
    char *home = home_dir();
    char *result = path_join(home, fallback_leaf);
    free(home);
    return result;
#endif
}

char *config_file_path(void) {
    char *dir = base_dir(xdg_config_home, "APPDATA", ".config");
    char *shimback_dir = path_join(dir, "shimback");
    free(dir);
    char *result = path_join(shimback_dir, "config.toml");
    free(shimback_dir);
    return result;
}

char *shim_bin_dir(void) {
    char *dir = base_dir(xdg_data_home, "LOCALAPPDATA", ".local/share");
    char *shimback_dir = path_join(dir, "shimback");
    free(dir);
    char *result = path_join(shimback_dir, "bin");
    free(shimback_dir);
    return result;
}

char *split_config_filename(const char *name) {
    /* Defense in depth, not the primary check: every legitimate caller is
     * expected to have already validated `name` against
     * is_valid_shim_name() (add.c/remove.c on user input, config_load()
     * on a hand-edited config.toml's own section headers) before it ever
     * reaches here. A name containing '/' or '..' joined into a path
     * below would otherwise let something outside shimback's own
     * directories be read, written, or deleted -- so treat an unvalidated
     * name arriving here as a programming error (something upstream
     * forgot to validate), not a normal, recoverable failure. */
    if (!is_valid_shim_name(name)) {
        die("internal error: unsafe shim name '%s' reached split_config_filename", name);
    }
    size_t total = strlen(name) + strlen("-config.toml") + 1;
    char *result = xmalloc(total);
    snprintf(result, total, "%s-config.toml", name);
    return result;
}

void split_config_all_paths(const char *name, char *paths[3]) {
    char *filename = split_config_filename(name);

    char *shim_dir = shim_bin_dir();
    paths[0] = path_join(shim_dir, filename);
    free(shim_dir);

    char *self_exe = self_exe_path();
    char *bin_dir = dir_of(self_exe);
    paths[1] = path_join(bin_dir, filename);
    free(self_exe);
    free(bin_dir);

    char *cfg_path = config_file_path();
    char *cfg_dir = dir_of(cfg_path);
    paths[2] = path_join(cfg_dir, filename);
    free(cfg_path);
    free(cfg_dir);

    free(filename);
}

char *resolve_split_config_path(const char *name) {
    /* Unlike split_config_filename()'s own die() (a last-resort invariant
     * for names that were *supposed* to already be validated), `name` here
     * can legitimately be something nobody ever validated: list/doctor and
     * uninstall --full all resolve every name list_shim_symlink_names()
     * finds by scanning the shim directory, including a symlink somebody
     * created or renamed by hand with an unsafe name. An invalid name can
     * never have a legitimate split config file (add.c refuses to create
     * one for it), so "not found" is the correct, crash-free answer, not
     * an error. */
    if (!is_valid_shim_name(name)) {
        return NULL;
    }
    char *paths[3];
    split_config_all_paths(name, paths);
    char *found = NULL;
    for (int i = 0; i < 3; i++) {
        if (!found && access(paths[i], F_OK) == 0) {
            found = paths[i];
        } else {
            free(paths[i]);
        }
    }
    return found;
}

char **list_shim_symlink_names(size_t *out_count) {
    char *shim_dir = shim_bin_dir();

    char **names = NULL;
    size_t count = 0;
    size_t cap = 0;

    char **entries = plat_list_dir(shim_dir);
    for (char **e = entries; e && *e; e++) {
        char *entry_path = path_join(shim_dir, *e);
        /* Recognizes any shimback build/install location as ours, not
         * just this exact running binary's own path -- an exact
         * self_exe_path() match used to make a shim from a relocated or
         * pre-upgrade binary invisible to list/doctor even though
         * uninstall's own sweep (also built on this same check) would
         * still recognize and clean it up, giving inconsistent answers
         * about the same symlink depending which command asked (see
         * review.md). See is_shim_dir_entry's own comment for how this
         * recognition differs by platform (symlink+resolve on POSIX,
         * direct content-scan on Windows, since a hard link has nothing
         * separate to resolve). */
        if (is_shim_dir_entry(entry_path)) {
            if (count == cap) {
                cap = cap == 0 ? 8 : cap * 2;
                names = xrealloc(names, cap * sizeof(char *));
            }
            names[count++] = shim_name_from_file(*e);
        }
        free(entry_path);
    }
    plat_free_dir_entries(entries);

    free(shim_dir);
    *out_count = count;
    return names;
}

static void append_unique_name(char ***names, size_t *count, size_t *cap, const char *name) {
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*names)[i], name) == 0) {
            return;
        }
    }
    if (*count == *cap) {
        *cap = *cap == 0 ? 8 : *cap * 2;
        *names = xrealloc(*names, *cap * sizeof(char *));
    }
    (*names)[(*count)++] = xstrdup(name);
}

static void scan_split_config_dir(const char *dir, char ***names, size_t *count, size_t *cap) {
    const char *suffix = "-config.toml";
    const size_t suffix_len = strlen(suffix);

    char **entries = plat_list_dir(dir);
    for (char **e = entries; e && *e; e++) {
        const char *filename = *e;
        size_t filename_len = strlen(filename);
        if (filename_len <= suffix_len ||
            strcmp(filename + filename_len - suffix_len, suffix) != 0) {
            continue;
        }

        size_t name_len = filename_len - suffix_len;
        char *name = xmalloc(name_len + 1);
        memcpy(name, filename, name_len);
        name[name_len] = '\0';
        if (is_valid_shim_name(name)) {
            char *path = path_join(dir, filename);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                append_unique_name(names, count, cap, name);
            }
            free(path);
        }
        free(name);
    }
    plat_free_dir_entries(entries);
}

char **list_split_config_names(size_t *out_count) {
    char **names = NULL;
    size_t count = 0;
    size_t cap = 0;

    char *shim_dir = shim_bin_dir();
    scan_split_config_dir(shim_dir, &names, &count, &cap);
    free(shim_dir);

    char *self_exe = self_exe_path();
    char *bin_dir = dir_of(self_exe);
    scan_split_config_dir(bin_dir, &names, &count, &cap);
    free(bin_dir);
    free(self_exe);

    char *cfg_path = config_file_path();
    char *cfg_dir = dir_of(cfg_path);
    scan_split_config_dir(cfg_dir, &names, &count, &cap);
    free(cfg_dir);
    free(cfg_path);

    *out_count = count;
    return names;
}

char *canonicalize(const char *path) {
    return plat_realpath(path);
}

char *self_exe_path(void) {
    return plat_self_exe_path();
}

char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return xstrdup(".");
    }
    if (slash == path) {
        return xstrdup("/");
    }
    size_t len = (size_t)(slash - path);
    char *result = xmalloc(len + 1);
    memcpy(result, path, len);
    result[len] = '\0';
    return result;
}

bool is_executable_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        return false;
    }
    return access(path, X_OK) == 0;
}

/* Bounds how much of a candidate file looks_like_shimback_binary will read
 * into memory -- shimback itself is a few MB at most; nothing legitimate
 * it would ever be asked to check is anywhere near this size, so this is
 * just a sanity bound against an implausibly huge file, not a real limit
 * in practice. */
#define SHIMBACK_BINARY_CHECK_MAX_SIZE (256 * 1024 * 1024)

/* Statically scans `path`'s own bytes for SHIMBACK_BINARY_MARKER -- a
 * fixed sequence every shimback build embeds (see main.c and
 * version.h.in) -- to confirm a file is actually a shimback binary (any
 * version/build of it, not just this exact one, and regardless of where
 * it's installed). Shared by every command that needs to recognize a
 * shim symlink or installed binary as shimback's own: list/doctor (via
 * list_shim_symlink_names, below), add/remove (deciding whether an
 * existing symlink is theirs to replace/remove), and uninstall (deciding
 * whether to delete bin_dest, built from user-controlled --prefix, or a
 * shim symlink target). All of them need the same answer to "is this
 * actually shimback's" for the same symlink -- using an exact match
 * against *this* running binary's own path in some of them and this
 * marker scan in others used to give different answers for a shim that
 * predates an upgrade or binary relocation (see review.md).
 *
 * Deliberately does NOT execute the candidate to ask it what it is (e.g.
 * `path --version`): a foreign executable placed at a shimback-owned
 * path can print whatever it likes -- including a convincing "shimback "
 * prefix -- while doing something else first, so running an untrusted
 * file just to decide whether to trust/delete it is itself a
 * code-execution risk, not a safety check (see review.md). A plain
 * byte-scan can still be fooled by a file that happens to embed the same
 * marker bytes, but reading them can never execute anything, which is
 * the actual property this needs.
 *
 * This is best-effort identification, not authenticated ownership proof
 * -- SHIMBACK_BINARY_MARKER is a fixed public byte sequence compiled into
 * every build (readable with `strings` on any shimback binary), so
 * nothing stops a different file from embedding the same bytes and being
 * misclassified as ours (see review.md). Deliberately not hardened
 * further than this: doing so would mean either trusting some other piece
 * of locally-writable state (an installed-binary manifest, a recorded
 * hash) that's exactly as forgeable by anything that can already write to
 * shimback's own directories, or verifying a real cryptographic identity,
 * which is disproportionate for a single-user CLI tool with no privilege
 * boundary to defend -- whoever could plant a convincing forgery here
 * already has write access to the same directory being managed, and so
 * could just delete or replace the file directly without needing this
 * check's cooperation at all. */
bool looks_like_shimback_binary(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    if (st.st_size < (off_t)SHIMBACK_BINARY_MARKER_LEN ||
        st.st_size > (off_t)SHIMBACK_BINARY_CHECK_MAX_SIZE) {
        return false;
    }
    if (!is_executable_file(path)) {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t size = (size_t)st.st_size;
    char *buf = xmalloc(size);
    size_t n = fread(buf, 1, size, f);
    fclose(f);

    bool found = false;
    if (n == size) {
        for (size_t i = 0; i + SHIMBACK_BINARY_MARKER_LEN <= n; i++) {
            if (memcmp(buf + i, SHIMBACK_BINARY_MARKER, SHIMBACK_BINARY_MARKER_LEN) == 0) {
                found = true;
                break;
            }
        }
    }
    free(buf);
    return found;
}

bool is_shim_dir_entry(const char *entry_path) {
#ifdef _WIN32
    return looks_like_shimback_binary(entry_path);
#else
    if (!plat_path_is_symlink(entry_path)) {
        return false;
    }
    char *resolved = canonicalize(entry_path);
    bool ok = resolved && looks_like_shimback_binary(resolved);
    free(resolved);
    return ok;
#endif
}

char *shim_file_name(const char *name) {
#ifdef _WIN32
    size_t len = strlen(name);
    char *result = xmalloc(len + 5); /* + ".exe" + NUL */
    memcpy(result, name, len);
    memcpy(result + len, ".exe", 5);
    return result;
#else
    return xstrdup(name);
#endif
}

char *shim_name_from_file(const char *filename) {
#ifdef _WIN32
    size_t len = strlen(filename);
    if (len > 4 && _stricmp(filename + len - 4, ".exe") == 0) {
        return xstrndup(filename, len - 4);
    }
    return xstrdup(filename);
#else
    return xstrdup(filename);
#endif
}

char *shimback_exe_name(void) {
    return shim_file_name("shimback");
}

void format_link_create_error(char *buf, size_t bufcap, const char *cmd_prefix,
                               const char *link_path, const char *target, int link_errno) {
#ifdef _WIN32
    if (link_errno == EXDEV) {
        snprintf(buf, bufcap,
                 "%s: cannot create a shim at %s -- it and shimback's own binary (%s) are on "
                 "different drives, and Windows hard links can't cross drives (unlike a POSIX "
                 "symlink). A plain copy was tried as a fallback and also failed (see the "
                 "warning above). Either run an installed copy of shimback from the same drive "
                 "as your shim directory (`shimback install --prefix <path>`, then add shims "
                 "via *that* copy, not this one), or set XDG_DATA_HOME to a directory on "
                 "shimback's own drive instead.",
                 cmd_prefix, link_path, target);
        return;
    }
#else
    (void)target;
#endif
    snprintf(buf, bufcap, "%s: failed to create shim link %s: %s", cmd_prefix, link_path,
             strerror(link_errno));
}

bool create_shim_link(const char *target, const char *link_path, const char *cmd_prefix) {
    if (plat_link_create(target, link_path)) {
        return true;
    }
    int link_errno = errno;
#ifdef _WIN32
    if (link_errno == EXDEV) {
        if (copy_executable(target, link_path)) {
            warn_colored(ANSI_YELLOW,
                         "%s: created a copy of shimback's binary at %s instead of a hard link "
                         "-- they're on different drives, and Windows hard links can't cross "
                         "drives. Keep in mind this copy won't automatically reflect a future "
                         "`shimback update`. If you don't see it yet, restart your shell (or "
                         "open a new terminal session) so any PATH changes are picked up.",
                         cmd_prefix, link_path);
            return true;
        }
        warn("%s: also failed to copy shimback's binary to %s as a fallback: %s", cmd_prefix,
             link_path, strerror(errno));
    }
#endif
    errno = link_errno; /* restore -- copy_executable's own failure path may have changed it,
                          * and the caller's format_link_create_error() call needs to see the
                          * *original* (here, primary) reason, not whatever the fallback's
                          * own failure left behind. */
    return false;
}

bool refresh_shim_link(const char *target, const char *link_path, const char *cmd_prefix) {
    size_t tmp_len = strlen(link_path) + 32;
    char *tmp_link = xmalloc(tmp_len);
    snprintf(tmp_link, tmp_len, "%s.tmp.%d", link_path, (int)getpid());

    if (!create_shim_link(target, tmp_link, cmd_prefix)) {
        int e = errno;
        free(tmp_link);
        errno = e;
        return false;
    }
    bool ok = plat_rename_replace(tmp_link, link_path);
    if (!ok) {
        int e = errno;
        unlink(tmp_link);
        errno = e;
    }
    free(tmp_link);
    return ok;
}

static bool copy_file_mode(const char *src, const char *dst, mode_t mode) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        return false;
    }

    char tmp[4160];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d.XXXXXX", dst, (int)getpid());
    /* plat_mkstemp both creates the file exclusively (immune to a
     * pre-planted symlink at this predictable-looking name -- a plain
     * fopen(tmp, "wb") would silently follow one) and fills in an
     * unguessable suffix, rather than relying on the pid alone. */
    int fd = plat_mkstemp(tmp);
    if (fd < 0) {
        fclose(in);
        return false;
    }
    FILE *out = fdopen(fd, "wb");
    if (!out) {
        close(fd);
        unlink(tmp);
        fclose(in);
        return false;
    }

    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    ok = ok && !ferror(in) && fflush(out) == 0;
    fclose(in);
    fclose(out);

    if (ok && !plat_chmod(tmp, (int)mode)) {
        ok = false;
    }
    if (ok && plat_rename_replace(tmp, dst)) {
        return true;
    }
    unlink(tmp);
    return false;
}

bool copy_executable(const char *src, const char *dst) {
    return copy_file_mode(src, dst, 0755);
}

bool copy_file(const char *src, const char *dst) {
    return copy_file_mode(src, dst, 0644);
}

/* EEXIST from mkdir() only means "a filesystem entry already has this
 * name" -- not "it's a directory". Without this check, a path component
 * blocked by a regular file (or anything else non-directory) would be
 * silently treated as already set up, and the real failure would only
 * surface later, far from here, as a confusing ENOTDIR trying to create
 * something *inside* what everyone assumed was a directory. */
static bool path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* '/' everywhere; also '\' on Windows -- callers there don't all go through
 * this project's own path_join() (which only ever emits '/'), e.g. a path
 * arriving via an env var set by some other Windows tool, or built with
 * PowerShell's own Join-Path, uses '\' -- and mkdir_p's per-component split
 * below has to recognize those boundaries too, or a whole run of
 * backslash-joined, not-yet-existing directories gets treated as a single
 * unsplittable component and handed to plat_mkdir() in one shot, which
 * (unlike this function) never creates more than one missing level at a
 * time and fails with ENOENT -- found exactly this way, via a Pester test
 * whose sandbox path was built with Join-Path. */
static bool is_path_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

bool mkdir_p(const char *dir) {
    char *copy = xstrdup(dir);
    size_t len = strlen(copy);
    if (len == 0) {
        free(copy);
        return false;
    }
    /* Strip a trailing separator so we don't try to mkdir an empty final segment. */
    if (is_path_sep(copy[len - 1])) {
        copy[len - 1] = '\0';
    }
    /* A Windows drive-letter root ("C:", "D:", ...) always exists and can
     * never usefully be mkdir'd -- and _wmkdir/stat on it behave
     * inconsistently across Windows/CRT versions when given the bare "C:"
     * form with no trailing separator (found the hard way: _wmkdir("C:")
     * returned EACCES rather than EEXIST on one machine, while on another it
     * returned EEXIST but the immediately following stat("C:") still
     * reported it as not a directory) -- so skip straight past "C:/" rather
     * than treating the drive root as just another path component. A POSIX
     * absolute path always starts with '/', so this never fires there. */
    char *start = copy + 1;
    if (len >= 2 && copy[1] == ':') {
        start = copy + 2;
        if (is_path_sep(*start)) {
            start++;
        }
    }
    for (char *p = start; *p; p++) {
        if (is_path_sep(*p)) {
            char sep = *p;
            *p = '\0';
            if (plat_mkdir(copy) != 0 && errno != EEXIST) {
                free(copy);
                return false;
            }
            if (!path_is_dir(copy)) {
                errno = ENOTDIR;
                free(copy);
                return false;
            }
            *p = sep;
        }
    }
    if (plat_mkdir(copy) != 0 && errno != EEXIST) {
        free(copy);
        return false;
    }
    bool ok = path_is_dir(copy);
    if (!ok) {
        errno = ENOTDIR;
    }
    free(copy);
    return ok;
}

/* Name of the advisory lock file shim_dir_lock_acquire() creates inside a
 * shim directory. Shared with uninstall.c, which needs to remove it as
 * part of its own cleanup -- otherwise this one regular file (never a
 * symlink, so none of the symlink-sweeping logic elsewhere ever touches
 * it) would be the one thing left behind, and rmdir() on the now
 * "non-empty" shim directory would quietly stop working. */
#define SHIM_DIR_LOCK_FILENAME ".shimback.lock"

/* Acquires an exclusive advisory lock (flock()) on a fixed lock file inside
 * `shim_dir`, blocking until it's available. `add`/`remove`/`doctor fix`
 * each hold this across their own check-then-mutate sequence on a shim
 * symlink (the ownership/dangling/orphan check, and the unlink()/rename()
 * that acts on it) -- without it, two shimback commands (or two runs of
 * the same one) running concurrently as the same user could interleave
 * between the check and the mutation, so the mutation ends up acting on
 * whatever a *different* concurrent command's check saw, not what this
 * one just verified (see review.md). Doesn't defend against a directory
 * writable by other users -- that's the separate, already-rejected
 * precondition checked before this is ever called -- only against two
 * same-user shimback invocations racing each other.
 *
 * Returns an fd to later pass to shim_dir_lock_release(), or -1 on
 * failure (the lock file couldn't be created/opened, or flock() itself
 * failed) -- the caller decides how fatal that is. `shim_dir` must
 * already exist. */
int shim_dir_lock_acquire(const char *shim_dir) {
    char *lock_path = path_join(shim_dir, SHIM_DIR_LOCK_FILENAME);
    int open_errno = 0;
    int fd = plat_lockfile_open(lock_path, &open_errno);
    free(lock_path);
    if (fd < 0) {
        /* callers (remove.c) distinguish ENOENT ("shim_dir doesn't exist,
         * nothing to lock") from a real failure. */
        errno = open_errno;
        return -1;
    }
    return fd;
}

/* Releases a lock acquired by shim_dir_lock_acquire(). Best-effort and
 * safe to call with -1 (a no-op) so callers don't need to track whether
 * acquisition actually succeeded before cleaning up. */
void shim_dir_lock_release(int fd) {
    plat_lockfile_close(fd);
}

/* Removes the lock file shim_dir_lock_acquire() creates inside `shim_dir`,
 * if present -- for a caller (uninstall) about to try to rmdir() that
 * directory once every shim symlink is gone; the lock file itself would
 * otherwise be the one thing left behind keeping it non-empty. Safe to
 * call whether or not a lock was ever taken (ENOENT is not an error). */
void shim_dir_lock_file_remove(const char *shim_dir) {
    char *lock_path = path_join(shim_dir, SHIM_DIR_LOCK_FILENAME);
    unlink(lock_path);
    free(lock_path);
}

char *path_search(const char *name, const char *exclude_dir,
                   const char *exclude_canonical) {
    const char *path_env = getenv("PATH");
    if (!path_env) {
        return NULL;
    }
    char *path_copy = xstrdup(path_env);
    char *result = NULL;
    char *saveptr = NULL;
    char *dir = strtok_r(path_copy, PLAT_PATH_LIST_SEP, &saveptr);
    while (dir) {
        if (dir[0] != '\0' && !(exclude_dir && strcmp(dir, exclude_dir) == 0)) {
            char *candidate = path_join(dir, name);
#ifdef _WIN32
            /* Windows resolves a bare, extension-less command name against
             * %PATHEXT% (cmd.exe/CreateProcess both do this); `name` here
             * is always passed bare by every caller (curl, pwsh,
             * powershell, ...), so without this a stat() on the
             * extension-less `candidate` above never matches the real
             * "<name>.exe" file and every such search silently fails to
             * find anything at all. Only .exe is handled -- the one
             * extension every binary this codebase looks for actually
             * has -- not the full %PATHEXT% list. */
            size_t name_len = strlen(name);
            bool already_has_exe = name_len > 4 && _stricmp(name + name_len - 4, ".exe") == 0;
            if (!is_executable_file(candidate) && !already_has_exe) {
                char *name_exe = shim_file_name(name); /* appends ".exe" */
                free(candidate);
                candidate = path_join(dir, name_exe);
                free(name_exe);
            }
#endif
            if (is_executable_file(candidate)) {
                char *resolved = canonicalize(candidate);
                if (resolved) {
                    if (exclude_canonical && strcmp(resolved, exclude_canonical) == 0) {
                        free(resolved);
                    } else {
                        free(candidate);
                        result = resolved;
                        break;
                    }
                }
            }
            free(candidate);
        }
        dir = strtok_r(NULL, PLAT_PATH_LIST_SEP, &saveptr);
    }
    free(path_copy);
    return result;
}

/* True if any character of `s` is a path separator (see is_path_sep) --
 * distinguishes "looks like a path" (however it's spelled) from "looks
 * like a bare command name to search $PATH for", the same way a plain
 * `strchr(s, '/')` did before Windows backslash paths needed recognizing
 * too. */
static bool has_path_sep(const char *s) {
    for (; *s; s++) {
        if (is_path_sep(*s)) {
            return true;
        }
    }
    return false;
}

char *resolve_binary_arg(const char *arg) {
    if (is_executable_file(arg)) {
        return canonicalize(arg);
    }
    if (!has_path_sep(arg)) {
        return path_search(arg, NULL, NULL);
    }
    return NULL;
}

char *force_resolve_binary_arg(const char *arg) {
    if (!has_path_sep(arg)) {
        return NULL;
    }
    if (path_looks_absolute(arg)) {
        return xstrdup(arg);
    }
    char *cwd = plat_getcwd();
    if (!cwd) {
        return NULL;
    }
    char *result = path_join(cwd, arg);
    free(cwd);
    return result;
}

bool write_file_atomic(const char *path, const char *data, size_t len, mode_t mode) {
    char tmp[4160];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d.XXXXXX", path, (int)getpid());
    int fd = plat_mkstemp(tmp);
    if (fd < 0) {
        return false;
    }
    if (!plat_fchmod(fd, (int)mode)) {
        close(fd);
        unlink(tmp);
        return false;
    }
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(tmp);
        return false;
    }

    size_t written = fwrite(data, 1, len, f);
    bool ok = written == len && fflush(f) == 0;
    fclose(f);
    if (!ok) {
        unlink(tmp);
        return false;
    }
    if (!plat_rename_replace(tmp, path)) {
        unlink(tmp);
        return false;
    }
    return true;
}
