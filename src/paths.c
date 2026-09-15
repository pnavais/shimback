#include "paths.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

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
            *p = '/';
        }
    }
    bool ok = mkdir(copy, 0755) == 0 || errno == EEXIST;
    free(copy);
    return ok;
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
