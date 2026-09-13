#include "commands.h"

#include <stddef.h>
#include <stdio.h>

#include "../paths.h"
#include "../shell.h"
#include "../util.h"

int cmd_init(int argc, char **argv) {
    if (argc > 1) {
        die("init: unexpected argument '%s'", argv[1]);
    }

    char *shim_dir = shim_bin_dir();
    if (!mkdir_p(shim_dir)) {
        die("init: failed to create shim directory %s", shim_dir);
    }

    ShellKind kinds[] = {SHELL_ZSH, SHELL_BASH, SHELL_FISH};
    bool any_installed = false;
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        if (shell_is_installed(kinds[i])) {
            any_installed = true;
            printf("Detected %s\n", shell_kind_name(kinds[i]));
            shell_ensure_path(kinds[i], shim_dir, true);
        }
    }

    if (!any_installed) {
        printf("No supported shells (zsh, bash, fish) detected on PATH.\n");
    }

    return 0;
}
