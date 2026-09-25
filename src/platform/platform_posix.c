#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h> /* pid_t */
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
/* readlink("/proc/self/exe", ...) needs no extra headers beyond unistd.h. */
#else
#error "unsupported platform: plat_self_exe_path needs a platform-specific implementation"
#endif

#include "../util.h"

/* waitpid(2) for a specific `pid`, options 0, transparently retried when
 * interrupted by a signal (EINTR) -- every blocking wait for a child in this
 * codebase wants this: an unrelated signal arriving while waiting (e.g.
 * SIGWINCH on terminal resize) would otherwise make a bare waitpid() call
 * return early with *status left unset and the child still unreaped, so a
 * caller that didn't know to retry would decode a garbage exit code. Returns
 * whatever the underlying waitpid() call ultimately returns for any other
 * outcome (the pid, or -1 with errno set for a real failure).
 *
 * Declared here rather than in the universally-included util.h: it's a
 * POSIX process-management primitive (needs pid_t, which doesn't exist on
 * Windows at all), not a portable utility -- moved here, with an explicit
 * prototype in edit.c (its one remaining external caller, itself deferred
 * from the Windows build -- see windows-port.md), when util.h's own
 * unconditional `pid_t` dependency turned out to make that header
 * uncompilable on Windows regardless of whether anything there actually
 * called this. */
pid_t xwaitpid(pid_t pid, int *status) {
    pid_t rc;
    do {
        rc = waitpid(pid, status, 0);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

/* POSIX's exit-status decoding convention: a normal exit reports its own
 * status; a fatal-signal death is folded into 128+signum, matching what a
 * shell reports in $?. Applied uniformly by every function below so no
 * caller ever sees a raw wait status. */
static int decode_exit_status(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

int plat_run_inherited(const char *exe, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execv(exe, argv);
        fprintf(stderr, "shimback: exec %s: %s\n", exe, strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (xwaitpid(pid, &status) < 0) {
        return -1;
    }
    return decode_exit_status(status);
}

/* Milliseconds elapsed since `start` (CLOCK_MONOTONIC, so immune to wall-
 * clock adjustments -- NTP, DST, someone changing the system clock). */
static long elapsed_ms_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)((now.tv_sec - start->tv_sec) * 1000 +
                   (now.tv_nsec - start->tv_nsec) / 1000000);
}

int plat_run_captured(const char *exe, char *const argv[], DynBuf *out, DynBuf *err,
                       int timeout_ms, size_t limit_bytes, bool *committed_live) {
    int out_pipe[2];
    int err_pipe[2];
    if (pipe(out_pipe) != 0) {
        return -1;
    }
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        execv(exe, argv);
        fprintf(stderr, "shimback: exec %s: %s\n", exe, strerror(errno));
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    bool out_done = false;
    bool err_done = false;
    bool live = false;
    char chunk[4096];
    while (!out_done || !err_done) {
        struct pollfd fds[2];
        fds[0].fd = out_done ? -1 : out_pipe[0];
        fds[0].events = POLLIN;
        fds[1].fd = err_done ? -1 : err_pipe[0];
        fds[1].events = POLLIN;

        /* Once committed to live relay there's no more deadline to watch
         * for, so just block; until then, never wait past the deadline --
         * otherwise a source that's gone quiet (a server that isn't
         * chatty, just long-running) would never get re-checked against
         * timeout_ms, since poll() would only wake for data that never
         * comes. */
        int poll_timeout = -1;
        if (!live) {
            long remaining = timeout_ms - elapsed_ms_since(&start);
            poll_timeout = remaining > 0 ? (int)remaining : 0;
        }

        int rc = poll(fds, 2, poll_timeout);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(out_pipe[0]);
            close(err_pipe[0]);
            xwaitpid(pid, NULL);
            return -1;
        }

        if (!live && (elapsed_ms_since(&start) >= timeout_ms || out->len + err->len >= limit_bytes)) {
            fwrite(out->data, 1, out->len, stdout);
            fwrite(err->data, 1, err->len, stderr);
            fflush(stdout);
            fflush(stderr);
            live = true;
            *committed_live = true;
        }

        if (!out_done && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(out_pipe[0], chunk, sizeof(chunk));
            if (n > 0) {
                if (live) {
                    fwrite(chunk, 1, (size_t)n, stdout);
                    fflush(stdout);
                } else {
                    dynbuf_append(out, chunk, (size_t)n);
                }
            } else if (n == 0 || errno != EINTR) {
                out_done = true;
            }
        }
        if (!err_done && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(err_pipe[0], chunk, sizeof(chunk));
            if (n > 0) {
                if (live) {
                    fwrite(chunk, 1, (size_t)n, stderr);
                    fflush(stderr);
                } else {
                    dynbuf_append(err, chunk, (size_t)n);
                }
            } else if (n == 0 || errno != EINTR) {
                err_done = true;
            }
        }
    }

    close(out_pipe[0]);
    close(err_pipe[0]);
    int status = 0;
    if (xwaitpid(pid, &status) < 0) {
        return -1;
    }
    return decode_exit_status(status);
}

long plat_capture_stdout(const char *exe, char *const argv[], char *buf, size_t bufcap) {
    int fds[2];
    if (pipe(fds) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
        }
        execv(exe, argv);
        _exit(127);
    }
    close(fds[1]);
    size_t len = 0;
    ssize_t n;
    while (len < bufcap - 1 && (n = read(fds[0], buf + len, bufcap - 1 - len)) > 0) {
        len += (size_t)n;
    }
    close(fds[0]);
    int status = 0;
    if (xwaitpid(pid, &status) < 0) {
        return -1;
    }
    buf[len] = '\0';
    if (len == 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return (long)len;
}

char *plat_home_dir(void) {
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

#if defined(__APPLE__)
char *plat_self_exe_path(void) {
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size); /* always returns -1 here, sets size */
    char *buf = xmalloc(size);
    if (_NSGetExecutablePath(buf, &size) != 0) {
        die("failed to resolve the shimback executable's own path");
    }
    char *resolved = realpath(buf, NULL);
    free(buf);
    if (!resolved) {
        die("failed to canonicalize executable path: %s", strerror(errno));
    }
    return resolved;
}
#elif defined(__linux__)
char *plat_self_exe_path(void) {
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
    char *resolved = realpath(buf, NULL);
    free(buf);
    if (!resolved) {
        die("failed to canonicalize executable path: %s", strerror(errno));
    }
    return resolved;
}
#endif

char **plat_list_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        return NULL;
    }
    char **names = NULL;
    size_t count = 0;
    size_t cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (count + 1 >= cap) { /* +1: always keep room for the NULL terminator */
            cap = cap == 0 ? 8 : cap * 2;
            names = xrealloc(names, cap * sizeof(char *));
        }
        names[count++] = xstrdup(ent->d_name);
    }
    closedir(d);
    if (!names) {
        /* An empty (but openable) directory: xrealloc was never called
         * above, so allocate the single-element NULL-terminated array
         * plat_free_dir_entries and every caller's iteration expect. */
        names = xmalloc(sizeof(char *));
    }
    names[count] = NULL;
    return names;
}

void plat_free_dir_entries(char **entries) {
    if (!entries) {
        return;
    }
    for (char **p = entries; *p; p++) {
        free(*p);
    }
    free(entries);
}

int plat_mkstemp(char *template_path) {
    return mkstemp(template_path);
}

char *plat_mkdtemp(char *template_path) {
    return mkdtemp(template_path);
}

bool plat_chmod(const char *path, int mode) {
    return chmod(path, (mode_t)mode) == 0;
}

bool plat_fchmod(int fd, int mode) {
    return fchmod(fd, (mode_t)mode) == 0;
}

bool plat_rename_replace(const char *from, const char *to) {
    return rename(from, to) == 0;
}

int plat_mkdir(const char *path) {
    return mkdir(path, 0755);
}

char *plat_getcwd(void) {
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) {
        return NULL;
    }
    return xstrdup(buf);
}

int plat_lockfile_open(const char *path, int *out_errno) {
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    int open_errno = errno; /* callers may free/reuse `path` before
                              * checking *out_errno; capture immediately. */
    if (fd < 0) {
        if (out_errno) {
            *out_errno = open_errno;
        }
        return -1;
    }
    if (flock(fd, LOCK_EX) != 0) {
        if (out_errno) {
            *out_errno = errno;
        }
        close(fd);
        return -1;
    }
    return fd;
}

void plat_lockfile_close(int handle) {
    if (handle >= 0) {
        flock(handle, LOCK_UN);
        close(handle);
    }
}

bool plat_isatty_stdin(void) {
    return isatty(STDIN_FILENO);
}

bool plat_isatty_stdout(void) {
    return isatty(STDOUT_FILENO);
}

bool plat_isatty_stderr(void) {
    return isatty(STDERR_FILENO);
}

bool plat_path_is_symlink(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

char *plat_realpath(const char *path) {
    /* realpath(path, NULL) is a POSIX.1-2008 extension that mallocs the
     * result buffer itself; supported on both macOS and Linux libc. */
    return realpath(path, NULL);
}

bool plat_link_create(const char *target, const char *link_path) {
    return symlink(target, link_path) == 0;
}

static struct termios g_saved_termios;
static bool g_have_saved_termios = false;
static bool g_raw_mode_active = false;

static void restore_at_exit(void) {
    plat_tty_raw_exit();
}

bool plat_tty_raw_enter(void) {
    if (g_raw_mode_active) {
        return true;
    }
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &raw) != 0) {
        return false;
    }
    if (!g_have_saved_termios) {
        g_saved_termios = raw;
        g_have_saved_termios = true;
        atexit(restore_at_exit);
    }
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(unsigned)(IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        return false;
    }
    g_raw_mode_active = true;
    return true;
}

void plat_tty_raw_exit(void) {
    if (!g_raw_mode_active || !g_have_saved_termios) {
        return;
    }
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
    g_raw_mode_active = false;
}

bool plat_stdin_byte_ready(int ms) {
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    return poll(&pfd, 1, ms) > 0;
}

bool plat_read_stdin_byte(char *out) {
    ssize_t n = read(STDIN_FILENO, out, 1);
    return n == 1;
}

/* --- Shell/PATH integration (Phase 5) -------------------------------- */
/* All five of these are Windows-only concepts (a "Documents" special
 * folder, parent-process detection in lieu of $SHELL, cmd.exe's AutoRun
 * registry hook, and the HKCU\Environment\Path persistent-PATH registry
 * value) -- see platform.h. shell.c's POSIX code paths never call any of
 * them; these exist only so every plat_* declaration has an implementation
 * on both platforms. */

char *plat_documents_dir(void) {
    return NULL;
}

char *plat_parent_process_name(void) {
    return NULL;
}

char *plat_win_autorun_get(void) {
    return xstrdup("");
}

bool plat_win_autorun_set(const char *value) {
    (void)value;
    return false;
}

bool plat_win_userenv_path_add(const char *dir) {
    (void)dir;
    return false;
}

bool plat_win_userenv_path_remove(const char *dir) {
    (void)dir;
    return false;
}

void plat_enable_vt_output(void) {
    /* No-op: a real POSIX terminal already interprets ANSI/VT escape
     * sequences natively, nothing to turn on. */
}
