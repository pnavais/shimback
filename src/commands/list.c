#include "commands.h"

#include <stdio.h>

#include "../config.h"
#include "../paths.h"
#include "../util.h"

int cmd_list(int argc, char **argv) {
    if (argc > 1) {
        die("list: unexpected argument '%s'", argv[1]);
    }

    char *cfg_path = config_file_path();
    Config cfg;
    char errbuf[256];
    ConfigStatus cst = config_load(cfg_path, &cfg, errbuf, sizeof(errbuf));
    if (cst != CONFIG_OK) {
        die("list: %s", errbuf);
    }

    if (cfg.count == 0) {
        printf("No shims configured.\n");
        return 0;
    }

    for (size_t i = 0; i < cfg.count; i++) {
        ShimEntry *e = &cfg.shims[i];
        printf("%-12s source=%-28s fallback=%-28s policy=%-10s diagnostic=%s\n", e->name,
               e->source ? e->source : "auto", e->fallback, policy_to_string(e->policy),
               e->diagnostic ? "true" : "false");
    }
    return 0;
}
