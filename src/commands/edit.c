/* Note: this is a short-lived CLI command handler. Heap allocations here are
 * intentionally not freed before process exit -- the OS reclaims them, and
 * this is a standard, deliberate simplification for one-shot CLI tools. */
#include "commands.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../config.h"
#include "../paths.h"
#include "../util.h"

/* Tried in order when $EDITOR is unset or empty. */
static const char *const FALLBACK_EDITORS[] = {"nvim", "vim", "vi", "nano", "pico"};

static int decode_exit_code(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

int cmd_edit(int argc, char **argv) {
    if (argc > 1) {
        die("edit: unexpected argument '%s'", argv[1]);
    }

    char *cfg_path = config_file_path();
    char *cfg_dir = dir_of(cfg_path);
    if (!mkdir_p(cfg_dir)) {
        die("edit: failed to create config directory %s", cfg_dir);
    }
    free(cfg_dir);

    const char *editor_env = getenv("EDITOR");

    pid_t pid = fork();
    if (pid < 0) {
        die("fork failed: %s", strerror(errno));
    }
    if (pid == 0) {
        if (editor_env && editor_env[0] != '\0') {
            /* Run through a shell so a multi-word $EDITOR (e.g. "code
             * --wait") works: $EDITOR is deliberately left unquoted so the
             * shell word-splits it into a command plus its own flags,
             * while the config path ($1) stays quoted -- passed as a
             * distinct argument, not interpolated into the script text, so
             * it's safe regardless of what characters it contains. */
            execl("/bin/sh", "sh", "-c", "exec $EDITOR \"$1\"", "sh", cfg_path, (char *)NULL);
            fprintf(stderr, "shimback: failed to run $EDITOR ('%s'): %s\n", editor_env,
                    strerror(errno));
            _exit(127);
        }

        for (size_t i = 0; i < sizeof(FALLBACK_EDITORS) / sizeof(FALLBACK_EDITORS[0]); i++) {
            execlp(FALLBACK_EDITORS[i], FALLBACK_EDITORS[i], cfg_path, (char *)NULL);
            /* exec only returns on failure (not installed, or something
             * else wrong with it) -- try the next candidate either way. */
        }
        fprintf(stderr,
                "shimback: no editor found -- set $EDITOR, or install one of: nvim, vim, vi, "
                "nano, pico\n");
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        die("edit: failed to wait for editor: %s", strerror(errno));
    }
    int code = decode_exit_code(status);

    /* A quick sanity check, not a hard requirement: if the editor exited
     * cleanly but left the config unparseable, say so right away rather
     * than letting the next add/list/doctor run surface a confusing error
     * far removed from the edit that caused it. */
    if (code == 0) {
        Config cfg;
        char errbuf[256];
        ConfigStatus cst = config_load(cfg_path, &cfg, errbuf, sizeof(errbuf));
        if (cst == CONFIG_OK) {
            config_free(&cfg);
        } else {
            warn("edit: %s now fails to parse: %s", cfg_path, errbuf);
        }
    }

    return code;
}
