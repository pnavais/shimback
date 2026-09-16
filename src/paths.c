#include "paths.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"
#include "version.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
/* readlink("/proc/self/exe", ...) needs no extra headers beyond unistd.h. */
#else
#error "unsupported platform: self_exe_path needs a platform-specific implementation"
#endif

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
    const char *val = getenv("HOME");
    if (val && val[0] != '\0') {
        return xstrdup(val);
    }
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0] != '\0') {
        return xstrdup(pw->pw_dir);
    }
    die("could not determine home directory (HOME is unset and no passwd entry found)");
    return NULL; /* unreachable */
}

static char *xdg_env(const char *name) {
    const char *val = getenv(name);
    if (!val || val[0] == '\0' || val[0] != '/') {
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

static char *base_dir(char *(*xdg_fn)(void), const char *fallback_leaf) {
    char *base = xdg_fn();
    if (base) {
        return base;
    }
    char *home = home_dir();
    char *result = path_join(home, fallback_leaf);
    free(home);
    return result;
}

char *config_file_path(void) {
    char *dir = base_dir(xdg_config_home, ".config");
    char *shimback_dir = path_join(dir, "shimback");
    free(dir);
    char *result = path_join(shimback_dir, "config.toml");
    free(shimback_dir);
    return result;
}

char *shim_bin_dir(void) {
    char *dir = base_dir(xdg_data_home, ".local/share");
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

    DIR *d = opendir(shim_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            char *entry_path = path_join(shim_dir, ent->d_name);
            struct stat lst;
            if (lstat(entry_path, &lst) == 0 && S_ISLNK(lst.st_mode)) {
                /* Recognizes any shimback build/install location as ours,
                 * not just this exact running binary's own path -- an
                 * exact self_exe_path() match used to make a shim from a
                 * relocated or pre-upgrade binary invisible to list/doctor
                 * even though uninstall's own sweep (also built on this
                 * same check) would still recognize and clean it up,
                 * giving inconsistent answers about the same symlink
                 * depending which command asked (see review.md). */
                char *resolved = canonicalize(entry_path);
                if (resolved && looks_like_shimback_binary(resolved)) {
                    if (count == cap) {
                        cap = cap == 0 ? 8 : cap * 2;
                        names = xrealloc(names, cap * sizeof(char *));
                    }
                    names[count++] = xstrdup(ent->d_name);
                }
                free(resolved);
            }
            free(entry_path);
        }
        closedir(d);
    }

    free(shim_dir);
    *out_count = count;
    return names;
}

char *canonicalize(const char *path) {
    /* realpath(path, NULL) is a POSIX.1-2008 extension that mallocs the
     * result buffer itself; supported on both macOS and Linux libc. */
    return realpath(path, NULL);
}

#if defined(__APPLE__)
char *self_exe_path(void) {
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size); /* always returns -1 here, sets size */
    char *buf = xmalloc(size);
    if (_NSGetExecutablePath(buf, &size) != 0) {
        die("failed to resolve the shimback executable's own path");
    }
    char *resolved = canonicalize(buf);
    free(buf);
    if (!resolved) {
        die("failed to canonicalize executable path: %s", strerror(errno));
    }
    return resolved;
}
#elif defined(__linux__)
char *self_exe_path(void) {
    size_t cap = 256;
    char *buf = xmalloc(cap);
    ssize_t n;
    for (;;) {
        n = readlink("/proc/self/exe", buf, cap);
        if (n < 0) {
            die("failed to read /proc/self/exe: %s", strerror(errno));
        }
        if ((size_t)n < cap) {
            break;
        }
        cap *= 2;
        buf = xrealloc(buf, cap);
    }
    buf[n] = '\0';
    char *resolved = canonicalize(buf);
    free(buf);
    if (!resolved) {
        die("failed to canonicalize executable path: %s", strerror(errno));
    }
    return resolved;
}
#endif

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

static bool copy_file_mode(const char *src, const char *dst, mode_t mode) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        return false;
    }

    char tmp[4160];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d.XXXXXX", dst, (int)getpid());
    /* mkstemp both creates the file exclusively (immune to a pre-planted
     * symlink at this predictable-looking name -- a plain fopen(tmp, "wb")
     * would silently follow one) and fills in an unguessable suffix,
     * rather than relying on the pid alone. */
    int fd = mkstemp(tmp);
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

    if (ok && chmod(tmp, mode) != 0) {
        ok = false;
    }
    if (ok && rename(tmp, dst) == 0) {
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

bool mkdir_p(const char *dir) {
    char *copy = xstrdup(dir);
    size_t len = strlen(copy);
    if (len == 0) {
        free(copy);
        return false;
    }
    /* Strip a trailing slash so we don't try to mkdir an empty final segment. */
    if (copy[len - 1] == '/') {
        copy[len - 1] = '\0';
    }
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                free(copy);
                return false;
            }
            if (!path_is_dir(copy)) {
                errno = ENOTDIR;
                free(copy);
                return false;
            }
            *p = '/';
        }
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
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
    int fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    int open_errno = errno; /* free() below isn't guaranteed not to touch
                              * errno, and callers (remove.c) distinguish
                              * ENOENT ("shim_dir doesn't exist, nothing to
                              * lock") from a real failure. */
    free(lock_path);
    if (fd < 0) {
        errno = open_errno;
        return -1;
    }
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Releases a lock acquired by shim_dir_lock_acquire(). Best-effort and
 * safe to call with -1 (a no-op) so callers don't need to track whether
 * acquisition actually succeeded before cleaning up. */
void shim_dir_lock_release(int fd) {
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
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
    char *dir = strtok_r(path_copy, ":", &saveptr);
    while (dir) {
        if (dir[0] != '\0' && !(exclude_dir && strcmp(dir, exclude_dir) == 0)) {
            char *candidate = path_join(dir, name);
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
        dir = strtok_r(NULL, ":", &saveptr);
    }
    free(path_copy);
    return result;
}

char *resolve_binary_arg(const char *arg) {
    if (is_executable_file(arg)) {
        return canonicalize(arg);
    }
    if (strchr(arg, '/') == NULL) {
        return path_search(arg, NULL, NULL);
    }
    return NULL;
}

char *force_resolve_binary_arg(const char *arg) {
    if (strchr(arg, '/') == NULL) {
        return NULL;
    }
    if (arg[0] == '/') {
        return xstrdup(arg);
    }
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
        return NULL;
    }
    return path_join(cwd, arg);
}

bool write_file_atomic(const char *path, const char *data, size_t len, mode_t mode) {
    char tmp[4160];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d.XXXXXX", path, (int)getpid());
    int fd = mkstemp(tmp);
    if (fd < 0) {
        return false;
    }
    if (fchmod(fd, mode) != 0) {
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
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}
