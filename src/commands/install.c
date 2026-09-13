/* Note: this is a short-lived CLI command handler. Heap allocations here are
 * intentionally not freed before process exit -- the OS reclaims them, and
 * this is a standard, deliberate simplification for one-shot CLI tools. */
#include "commands.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../paths.h"
#include "../shell.h"
#include "../util.h"
#include "version.h"

#define MAN_PAGE_NAME "shimback.1"

static const char *USAGE =
    "usage: shimback install [--prefix <dir>] [--shell <shell>[,<shell>]... | --all]\n";

/* Downloads `url` to `dest` via `curl` (its resolved absolute path), atomically
 * via a temp-file-plus-rename in `dest`'s own directory. Returns false on any
 * failure (curl missing/erroring, network down, non-2xx response with -f). */
static bool download_via_curl(const char *curl, const char *url, const char *dest) {
    char tmp[4160];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", dest, (int)getpid());

    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        execl(curl, curl, "-fsSL", url, "-o", tmp, (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (ok && rename(tmp, dest) == 0) {
        return true;
    }
    unlink(tmp);
    return false;
}

/* Installs the man page to <prefix>/share/man/man1/shimback.1: prefers a
 * copy bundled next to the running binary (how the release tarball ships
 * it), falling back to downloading it from the GitHub release matching the
 * running version if no local copy is found. Never fatal -- a missing man
 * page shouldn't fail `install`'s primary job of getting the binary in
 * place. */
static void install_man_page(const char *prefix, const char *self_exe) {
    char *man_dir = path_join(prefix, "share/man/man1");
    if (!mkdir_p(man_dir)) {
        warn("install: failed to create %s; skipping man page", man_dir);
        free(man_dir);
        return;
    }

    char *man_dest = path_join(man_dir, MAN_PAGE_NAME);
    char *self_dir = dir_of(self_exe);
    char *local_man = path_join(self_dir, MAN_PAGE_NAME);

    struct stat st;
    if (stat(local_man, &st) == 0 && S_ISREG(st.st_mode)) {
        if (copy_file(local_man, man_dest)) {
            printf("shimback: man page installed to %s\n", man_dest);
        } else {
            warn("install: failed to copy man page from %s to %s: %s", local_man, man_dest,
                 strerror(errno));
        }
    } else {
        char *curl = path_search("curl", NULL, NULL);
        if (!curl) {
            warn("install: no bundled man page found next to the binary, and 'curl' is not on "
                 "PATH -- skipping man page");
        } else {
            char url[256];
            snprintf(url, sizeof(url),
                     "https://github.com/pnavais/shimback/releases/download/v%s/%s",
                     SHIMBACK_VERSION, MAN_PAGE_NAME);
            if (download_via_curl(curl, url, man_dest)) {
                printf("shimback: man page downloaded and installed to %s\n", man_dest);
            } else {
                warn("install: failed to download man page from %s -- skipping", url);
            }
        }
    }

    free(local_man);
    free(self_dir);
    free(man_dest);
    free(man_dir);
}

/* Merges both the shim dir and this binary's own dir into the single
 * "shimback"-tagged PATH block for `kind` (shell_ensure_path unions its
 * directory into whatever's already there, so calling it twice combines
 * both without either clobbering the other). Also removes the old separate
 * "shimback-bin" block, a one-time migration for anyone who ran an earlier
 * version of `install` that kept the two directories in separate blocks. */
static void ensure_shell_path(ShellKind kind, const char *shim_dir, const char *bin_dir) {
    shell_ensure_path(kind, shim_dir, true);
    shell_ensure_path(kind, bin_dir, true);
    shell_remove_path_tagged(kind, "shimback-bin");
}

int cmd_install(int argc, char **argv) {
    const char *prefix_arg = NULL;
    const char *shell_arg = NULL;
    bool all_shells = false;

    static struct option long_opts[] = {
        {"prefix", required_argument, 0, 'p'},
        {"shell", required_argument, 0, 's'},
        {"all", no_argument, 0, 'a'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:s:a", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p': prefix_arg = optarg; break;
            case 's': shell_arg = optarg; break;
            case 'a': all_shells = true; break;
            default:
                fprintf(stderr, "%s", USAGE);
                return 1;
        }
    }
    if (optind < argc) {
        die("install: unexpected extra argument '%s'", argv[optind]);
    }
    if (shell_arg && all_shells) {
        die("install: --shell and --all are mutually exclusive");
    }

    /* Parse and validate --shell up front, before any filesystem writes --
     * a typo here shouldn't leave a half-finished install (binary copied,
     * then a die() on a bad shell name). */
    ShellKind selected_shells[8];
    size_t selected_count = 0;
    if (shell_arg) {
        char *copy = xstrdup(shell_arg);
        char *saveptr = NULL;
        char *tok = strtok_r(copy, ",", &saveptr);
        while (tok) {
            if (selected_count >= sizeof(selected_shells) / sizeof(selected_shells[0])) {
                die("install: too many --shell values");
            }
            if (!shell_kind_from_name(tok, &selected_shells[selected_count])) {
                die("install: unknown shell '%s' (expected zsh, bash, or fish)", tok);
            }
            selected_count++;
            tok = strtok_r(NULL, ",", &saveptr);
        }
        free(copy);
        if (selected_count == 0) {
            die("install: --shell requires at least one shell name");
        }
    }

    char *prefix;
    if (prefix_arg) {
        prefix = xstrdup(prefix_arg);
    } else {
        char *home = home_dir();
        prefix = path_join(home, ".local");
        free(home);
    }

    char *bin_dir = path_join(prefix, "bin");
    if (!mkdir_p(bin_dir)) {
        die("install: failed to create %s", bin_dir);
    }

    char *self_exe = self_exe_path();
    char *dest = path_join(bin_dir, "shimback");

    /* Always overwrite: this doubles as the upgrade path (re-run install
     * after building a newer shimback to refresh the installed copy), and a
     * same-content copy is a harmless no-op. */
    if (!copy_executable(self_exe, dest)) {
        die("install: failed to copy %s to %s: %s", self_exe, dest, strerror(errno));
    }
    printf("shimback: installed to %s\n", dest);

    install_man_page(prefix, self_exe);

    /* Ensures both PATH entries are set up even on a totally fresh install,
     * before any `add` has ever run: this binary's own location, and the
     * shim directory itself (normally add/init's job) -- redundant, and a
     * harmless no-op, if add/init already wrote it. Defaults to the current
     * shell only; --shell/--all broaden that, matching `init`'s "every
     * installed shell" semantics for --all. */
    char *shim_dir = shim_bin_dir();
    if (all_shells) {
        ShellKind kinds[] = {SHELL_ZSH, SHELL_BASH, SHELL_FISH};
        for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
            if (shell_is_installed(kinds[i])) {
                ensure_shell_path(kinds[i], shim_dir, bin_dir);
            }
        }
    } else if (selected_count > 0) {
        for (size_t i = 0; i < selected_count; i++) {
            ensure_shell_path(selected_shells[i], shim_dir, bin_dir);
        }
    } else {
        ensure_shell_path(detect_current_shell(), shim_dir, bin_dir);
    }
    free(shim_dir);
    printf("Restart your shell (or re-source its startup file) for the PATH change to take "
           "effect.\n");

    return 0;
}
