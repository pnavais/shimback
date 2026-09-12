#include "dispatch.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "paths.h"
#include "util.h"

static int decode_exit_code(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

static bool child_succeeded(int status) {
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Builds the argv to pass to exec: argv[0] = the shim's invoked name (so the
 * wrapped program sees the same identity the user typed), followed by the
 * original forwarded arguments. */
static char **build_argv(const char *name, int argc, char **argv) {
    char **fwd = xmalloc((size_t)(argc + 1) * sizeof(char *));
    fwd[0] = (char *)name;
    for (int i = 1; i < argc; i++) {
        fwd[i] = argv[i];
    }
    fwd[argc] = NULL;
    return fwd;
}

/* Runs `exe` with real inherited stdio (no capturing) -- used whenever a run
 * is meant to be the "real", user-visible attempt: the fallback, or the
 * source when it equals the fallback. */
static void run_inherited(const char *exe, char *const argv[], int *raw_status) {
    pid_t pid = fork();
    if (pid < 0) {
        die("fork failed: %s", strerror(errno));
    }
    if (pid == 0) {
        execv(exe, argv);
        fprintf(stderr, "shimback: exec %s: %s\n", exe, strerror(errno));
        _exit(127);
    }
    waitpid(pid, raw_status, 0);
}

/* Runs `exe` with stdout/stderr captured into buffers rather than streamed
 * live -- this is what makes a failed trial run invisible. Drains both
 * pipes via poll() rather than sequential reads, since the child may
 * interleave writes to both streams and a full pipe would otherwise
 * deadlock a sequential reader. */
static void run_captured(const char *exe, char *const argv[], DynBuf *out, DynBuf *err,
                          int *raw_status) {
    int out_pipe[2];
    int err_pipe[2];
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        die("failed to create pipes: %s", strerror(errno));
    }

    pid_t pid = fork();
    if (pid < 0) {
        die("fork failed: %s", strerror(errno));
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

    bool out_done = false;
    bool err_done = false;
    char chunk[4096];
    while (!out_done || !err_done) {
        struct pollfd fds[2];
        fds[0].fd = out_done ? -1 : out_pipe[0];
        fds[0].events = POLLIN;
        fds[1].fd = err_done ? -1 : err_pipe[0];
        fds[1].events = POLLIN;

        int rc = poll(fds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("poll failed: %s", strerror(errno));
        }

        if (!out_done && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(out_pipe[0], chunk, sizeof(chunk));
            if (n > 0) {
                dynbuf_append(out, chunk, (size_t)n);
            } else if (n == 0 || errno != EINTR) {
                out_done = true;
            }
        }
        if (!err_done && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(err_pipe[0], chunk, sizeof(chunk));
            if (n > 0) {
                dynbuf_append(err, chunk, (size_t)n);
            } else if (n == 0 || errno != EINTR) {
                err_done = true;
            }
        }
    }

    close(out_pipe[0]);
    close(err_pipe[0]);
    waitpid(pid, raw_status, 0);
}

static void replay(DynBuf *out, DynBuf *err) {
    fwrite(out->data, 1, out->len, stdout);
    fflush(stdout);
    fwrite(err->data, 1, err->len, stderr);
    fflush(stderr);
}

int dispatch_run(const char *shim_name, int argc, char **argv) {
    char *cfg_path = config_file_path();
    Config cfg;
    char errbuf[256];
    ConfigStatus cst = config_load(cfg_path, &cfg, errbuf, sizeof(errbuf));
    if (cst != CONFIG_OK) {
        die("failed to load config %s: %s", cfg_path, errbuf);
    }

    ShimEntry *entry = config_find(&cfg, shim_name);
    if (!entry) {
        fprintf(stderr, "shimback: no shim configured for '%s'\n", shim_name);
        return 127;
    }

    if (!is_executable_file(entry->fallback)) {
        fprintf(stderr, "shimback: fallback '%s' for '%s' not found or not executable\n",
                entry->fallback, shim_name);
        return 127;
    }
    char *resolved_fallback = canonicalize(entry->fallback);
    if (!resolved_fallback) {
        fprintf(stderr, "shimback: fallback '%s' for '%s' not found or not executable\n",
                entry->fallback, shim_name);
        return 127;
    }

    char *resolved_source = NULL;
    if (entry->source) {
        if (!is_executable_file(entry->source)) {
            fprintf(stderr, "shimback: source '%s' for '%s' not found or not executable\n",
                    entry->source, shim_name);
            return 127;
        }
        resolved_source = canonicalize(entry->source);
    } else {
        char *shim_dir = shim_bin_dir();
        char *self_exe = self_exe_path();
        resolved_source = path_search(shim_name, shim_dir, self_exe);
        free(shim_dir);
        free(self_exe);
    }
    if (!resolved_source) {
        fprintf(stderr, "shimback: no '%s' found on PATH to use as the source command\n",
                shim_name);
        return 127;
    }

    char **fwd_argv = build_argv(shim_name, argc, argv);

    if (strcmp(resolved_source, resolved_fallback) == 0) {
        warn("'%s': source and fallback resolve to the same binary; running it directly",
             shim_name);
        int status;
        run_inherited(resolved_source, fwd_argv, &status);
        return decode_exit_code(status);
    }

    DynBuf out;
    DynBuf err;
    dynbuf_init(&out);
    dynbuf_init(&err);
    int status;
    run_captured(resolved_source, fwd_argv, &out, &err, &status);

    if (child_succeeded(status)) {
        replay(&out, &err);
        dynbuf_free(&out);
        dynbuf_free(&err);
        return 0;
    }

    bool should_fallback;
    if (entry->policy == POLICY_EXIT_CODE) {
        should_fallback = true;
    } else if (entry->policy == POLICY_EXIT_CODE_MATCH) {
        should_fallback = false;
        int source_exit_code = decode_exit_code(status);
        for (size_t i = 0; i < entry->exit_code_count; i++) {
            if (entry->exit_codes[i] == source_exit_code) {
                should_fallback = true;
                break;
            }
        }
    } else {
        should_fallback = false;
        const char *stderr_text = dynbuf_cstr(&err);
        for (size_t i = 0; i < entry->error_pattern_count; i++) {
            if (str_casestr(stderr_text, entry->error_patterns[i]) != NULL) {
                should_fallback = true;
                break;
            }
        }
    }

    if (should_fallback) {
        dynbuf_free(&out);
        dynbuf_free(&err);
        if (entry->diagnostic) {
            warn("'%s' failed; falling back to %s", shim_name, resolved_fallback);
        }
        int fb_status;
        run_inherited(resolved_fallback, fwd_argv, &fb_status);
        return decode_exit_code(fb_status);
    }

    replay(&out, &err);
    int code = decode_exit_code(status);
    dynbuf_free(&out);
    dynbuf_free(&err);
    return code;
}
