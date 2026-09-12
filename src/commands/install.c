/* Note: this is a short-lived CLI command handler. Heap allocations here are
 * intentionally not freed before process exit -- the OS reclaims them, and
 * this is a standard, deliberate simplification for one-shot CLI tools. */
#include "commands.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../paths.h"
#include "../shell.h"
#include "../util.h"

#define INSTALL_TAG "shimback-bin"

static const char *USAGE = "usage: shimback install [--prefix <dir>]\n";

int cmd_install(int argc, char **argv) {
    const char *prefix_arg = NULL;

    static struct option long_opts[] = {
        {"prefix", required_argument, 0, 'p'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p': prefix_arg = optarg; break;
            default:
                fprintf(stderr, "%s", USAGE);
                return 1;
        }
    }
    if (optind < argc) {
        die("install: unexpected extra argument '%s'", argv[optind]);
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

    ShellKind shell = detect_current_shell();
    shell_ensure_path_tagged(shell, bin_dir, INSTALL_TAG);
    printf("Restart your shell (or re-source its startup file) for the PATH change to take "
           "effect.\n");

    return 0;
}
