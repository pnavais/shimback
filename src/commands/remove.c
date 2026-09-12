#include "commands.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../config.h"
#include "../paths.h"
#include "../suggest.h"
#include "../util.h"

int cmd_remove(int argc, char **argv) {
    if (argc < 2) {
        die("remove: missing shim name (usage: shimback remove <name>)");
    }
    const char *name = argv[1];
    if (argc > 2) {
        die("remove: unexpected extra argument '%s'", argv[2]);
    }

    char *cfg_path = config_file_path();
    Config cfg;
    char errbuf[256];
    ConfigStatus cst = config_load(cfg_path, &cfg, errbuf, sizeof(errbuf));
    if (cst != CONFIG_OK) {
        die("remove: %s", errbuf);
    }

    if (!config_find(&cfg, name)) {
        fprintf(stderr, "shimback: remove: no shim configured for '%s'\n", name);
        if (cfg.count > 0) {
            const char **candidates = xmalloc(cfg.count * sizeof(char *));
            for (size_t i = 0; i < cfg.count; i++) {
                candidates[i] = cfg.shims[i].name;
            }
            char *suggestion = fuzzy_suggest(name, candidates, cfg.count);
            free(candidates);
            if (suggestion) {
                print_suggestion_hint(suggestion);
                free(suggestion);
            }
        }
        exit(1);
    }

    char *shim_dir = shim_bin_dir();
    char *symlink_path = path_join(shim_dir, name);
    char *self_exe = self_exe_path();

    struct stat st;
    if (lstat(symlink_path, &st) == 0 && S_ISLNK(st.st_mode)) {
        char *resolved = canonicalize(symlink_path);
        if (resolved && strcmp(resolved, self_exe) == 0) {
            if (unlink(symlink_path) != 0) {
                warn("failed to remove symlink %s: %s", symlink_path, strerror(errno));
            }
        } else {
            warn("%s is not a shimback-managed symlink; leaving it alone", symlink_path);
        }
    }

    config_remove(&cfg, name);
    ConfigStatus save_st = config_save(&cfg, cfg_path, errbuf, sizeof(errbuf));
    if (save_st != CONFIG_OK) {
        die("remove: failed to save config: %s", errbuf);
    }

    printf("shimback: removed '%s'\n", name);
    return 0;
}
