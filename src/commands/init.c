#include "commands.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "../config.h"
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

    /* A malformed config.toml shouldn't block `init` from wiring up PATH
     * at all -- see install.c's identical reasoning for this same lookup. */
    char *cfg_path_for_mise = config_file_path();
    Config cfg_for_mise;
    char mise_errbuf[256];
    MiseIntegrationMode mise_mode = MISE_AUTO;
    if (config_load(cfg_path_for_mise, &cfg_for_mise, mise_errbuf, sizeof(mise_errbuf)) ==
        CONFIG_OK) {
        mise_mode = cfg_for_mise.mise_integration_set
                        ? (cfg_for_mise.mise_integration ? MISE_ON : MISE_OFF)
                        : MISE_AUTO;
    }
    free(cfg_path_for_mise);

    ShellKind kinds[3];
    size_t kind_count = shell_all_kinds(kinds);
    bool any_installed = false;
    for (size_t i = 0; i < kind_count; i++) {
        if (shell_is_installed(kinds[i])) {
            any_installed = true;
            printf("Detected %s\n", shell_kind_name(kinds[i]));
            shell_ensure_path(kinds[i], shim_dir, true, mise_mode);
        }
    }

    if (!any_installed) {
        printf("No supported shells (zsh, bash, fish) detected on PATH.\n");
    }

    return 0;
}
