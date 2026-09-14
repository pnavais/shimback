#include "commands.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../config.h"
#include "../paths.h"
#include "../suggest.h"
#include "../util.h"

static const char *USAGE = "usage: shimback remove [-y] <name>\n";

int cmd_remove(int argc, char **argv) {
    bool auto_yes = false;

    static struct option long_opts[] = {
        {"yes", no_argument, 0, 'y'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "y", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'y': auto_yes = true; break;
            default:
                fprintf(stderr, "%s", USAGE);
                return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "%s", USAGE);
        die("remove: missing shim name");
    }
    const char *name = argv[optind++];
    if (optind < argc) {
        die("remove: unexpected extra argument '%s'", argv[optind]);
    }

    char *cfg_path = config_file_path();
    Config cfg;
    char errbuf[256];
    ConfigStatus cst = config_load(cfg_path, &cfg, errbuf, sizeof(errbuf));
    if (cst != CONFIG_OK) {
        die("remove: %s", errbuf);
    }

    if (!config_find(&cfg, name)) {
        const char **candidates = NULL;
        if (cfg.count > 0) {
            candidates = xmalloc(cfg.count * sizeof(char *));
            for (size_t i = 0; i < cfg.count; i++) {
                candidates[i] = cfg.shims[i].name;
            }
        }

        /* -y only ever auto-applies a suggestion that's the UNIQUE closest
         * match (see fuzzy_suggest_unique) -- if two configured names are
         * equally plausible typo targets, picking one without asking would
         * be too risky for a destructive action. */
        if (auto_yes && candidates) {
            char *unique = fuzzy_suggest_unique(name, candidates, cfg.count);
            if (unique) {
                name = unique;
            }
        }

        if (!config_find(&cfg, name)) {
            fprintf(stderr, "shimback: remove: no shim configured for '%s'\n", name);
            if (candidates) {
                char *hint = fuzzy_suggest(name, candidates, cfg.count);
                if (hint) {
                    print_suggestion_hint(hint);
                    free(hint);
                }
            }
            free(candidates);
            exit(1);
        }
        free(candidates);
    }

    char *shim_dir = shim_bin_dir();
    char *symlink_path = path_join(shim_dir, name);
    char *self_exe = self_exe_path();

    /* Read-only pre-flight: figure out whether there's a shimback-managed
     * symlink to remove, without touching it yet. The config is saved
     * first (below), and only once that succeeds is the symlink actually
     * unlinked -- so a failed save never leaves the symlink gone while the
     * config still describes the shim as configured. */
    bool have_managed_symlink = false;
    struct stat st;
    if (lstat(symlink_path, &st) == 0 && S_ISLNK(st.st_mode)) {
        char *resolved = canonicalize(symlink_path);
        if (resolved && strcmp(resolved, self_exe) == 0) {
            have_managed_symlink = true;
        } else {
            warn("%s is not a shimback-managed symlink; leaving it alone", symlink_path);
        }
    }

    config_remove(&cfg, name);
    ConfigStatus save_st = config_save(&cfg, cfg_path, errbuf, sizeof(errbuf));
    if (save_st != CONFIG_OK) {
        die("remove: failed to save config: %s", errbuf);
    }

    if (have_managed_symlink && unlink(symlink_path) != 0) {
        warn("failed to remove symlink %s: %s", symlink_path, strerror(errno));
    }

    bool colorize = stdout_is_color();
    printf("shimback: %sremoved '%s'%s\n", colorize ? ANSI_GREEN : "", name,
           colorize ? ANSI_RESET : "");
    return 0;
}
