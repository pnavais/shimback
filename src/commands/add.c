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
#include <unistd.h>

#include "../config.h"
#include "../paths.h"
#include "../shell.h"
#include "../util.h"

static const char *USAGE =
    "usage: shimback add <name> [-s <source>] -f <fallback>\n"
    "                    [--policy exit-code|heuristic|exit-code-match]\n"
    "                    [--error-pattern <p>]... [--exit-code <code>]...\n"
    "                    [--diagnostic]\n";

static void push_exit_code(int **arr, size_t *count, size_t *cap, int value) {
    if (*count == *cap) {
        *cap = *cap == 0 ? 4 : *cap * 2;
        *arr = xrealloc(*arr, *cap * sizeof(int));
    }
    (*arr)[(*count)++] = value;
}

int cmd_add(int argc, char **argv) {
    const char *source_arg = NULL;
    const char *fallback_arg = NULL;
    const char *policy_arg = "exit-code";
    bool diagnostic = false;
    StrVec patterns;
    strvec_init(&patterns);
    int *exit_codes = NULL;
    size_t exit_code_count = 0;
    size_t exit_code_cap = 0;

    static struct option long_opts[] = {
        {"source", required_argument, 0, 's'},
        {"fallback", required_argument, 0, 'f'},
        {"policy", required_argument, 0, 'p'},
        {"error-pattern", required_argument, 0, 'e'},
        {"exit-code", required_argument, 0, 'x'},
        {"diagnostic", no_argument, 0, 'd'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:f:p:e:x:d", long_opts, NULL)) != -1) {
        switch (opt) {
            case 's': source_arg = optarg; break;
            case 'f': fallback_arg = optarg; break;
            case 'p': policy_arg = optarg; break;
            case 'e': strvec_push(&patterns, xstrdup(optarg)); break;
            case 'x': {
                char *end;
                long v = strtol(optarg, &end, 10);
                if (*optarg == '\0' || *end != '\0' || v < 0 || v > 255) {
                    die("add: --exit-code must be an integer between 0 and 255 (got '%s')",
                        optarg);
                }
                push_exit_code(&exit_codes, &exit_code_count, &exit_code_cap, (int)v);
                break;
            }
            case 'd': diagnostic = true; break;
            default:
                fprintf(stderr, "%s", USAGE);
                return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "%s", USAGE);
        die("add: missing shim name");
    }
    const char *name = argv[optind++];
    if (optind < argc) {
        die("add: unexpected extra argument '%s'", argv[optind]);
    }

    if (name[0] == '\0' || strchr(name, '/') != NULL || strcmp(name, "shimback") == 0) {
        die("add: invalid shim name '%s'", name);
    }
    if (!fallback_arg) {
        fprintf(stderr, "%s", USAGE);
        die("add: -f/--fallback is required");
    }

    Policy policy;
    if (!policy_from_string(policy_arg, &policy)) {
        die("add: --policy must be \"exit-code\", \"heuristic\", or \"exit-code-match\"");
    }
    if (policy == POLICY_HEURISTIC && patterns.count == 0) {
        die("add: --policy heuristic requires at least one --error-pattern");
    }
    if (policy == POLICY_EXIT_CODE_MATCH && exit_code_count == 0) {
        die("add: --policy exit-code-match requires at least one --exit-code");
    }

    if (!is_executable_file(fallback_arg)) {
        die("add: fallback '%s' does not exist or is not executable", fallback_arg);
    }
    char *resolved_fallback = canonicalize(fallback_arg);
    if (!resolved_fallback) {
        die("add: fallback '%s' does not exist or is not executable", fallback_arg);
    }

    char *shim_dir = shim_bin_dir();

    char *resolved_source_for_check = NULL;
    if (source_arg) {
        if (!is_executable_file(source_arg)) {
            die("add: source '%s' does not exist or is not executable", source_arg);
        }
        resolved_source_for_check = canonicalize(source_arg);
    } else {
        resolved_source_for_check = path_search(name, shim_dir, NULL);
    }

    if (resolved_source_for_check && strcmp(resolved_source_for_check, resolved_fallback) == 0) {
        die("add: source and fallback both resolve to '%s' -- refusing to add a no-op shim",
            resolved_fallback);
    }

    if (!mkdir_p(shim_dir)) {
        die("add: failed to create shim directory %s", shim_dir);
    }

    char *self_exe = self_exe_path();
    char *symlink_path = path_join(shim_dir, name);

    struct stat st;
    if (lstat(symlink_path, &st) == 0) {
        if (!S_ISLNK(st.st_mode)) {
            die("add: %s already exists and is not a symlink; remove it manually first",
                symlink_path);
        }
        char *existing_resolved = canonicalize(symlink_path);
        bool shimback_owned = existing_resolved && strcmp(existing_resolved, self_exe) == 0;
        if (!shimback_owned) {
            die("add: %s already exists and is not a shimback-managed symlink; remove it "
                "manually first",
                symlink_path);
        }
        if (unlink(symlink_path) != 0) {
            die("add: failed to replace existing symlink %s: %s", symlink_path, strerror(errno));
        }
    }

    if (symlink(self_exe, symlink_path) != 0) {
        die("add: failed to create symlink %s: %s", symlink_path, strerror(errno));
    }

    char *cfg_path = config_file_path();
    Config cfg;
    char errbuf[256];
    ConfigStatus cst = config_load(cfg_path, &cfg, errbuf, sizeof(errbuf));
    if (cst != CONFIG_OK) {
        die("add: existing config at %s is invalid: %s", cfg_path, errbuf);
    }

    size_t idx = config_upsert(&cfg, name);
    ShimEntry *entry = &cfg.shims[idx];
    free(entry->source);
    entry->source = source_arg ? xstrdup(source_arg) : NULL;
    free(entry->fallback);
    entry->fallback = xstrdup(fallback_arg);
    entry->policy = policy;
    for (size_t i = 0; i < entry->error_pattern_count; i++) {
        free(entry->error_patterns[i]);
    }
    free(entry->error_patterns);
    entry->error_patterns = patterns.items; /* ownership transferred */
    entry->error_pattern_count = patterns.count;
    free(entry->exit_codes);
    entry->exit_codes = exit_codes; /* ownership transferred */
    entry->exit_code_count = exit_code_count;
    entry->diagnostic = diagnostic;

    ConfigStatus save_st = config_save(&cfg, cfg_path, errbuf, sizeof(errbuf));
    if (save_st != CONFIG_OK) {
        die("add: failed to save config: %s", errbuf);
    }

    printf("shimback: '%s' -> %s (fallback: %s, policy: %s)\n", name, symlink_path,
           resolved_fallback, policy_to_string(policy));

    ShellKind shell = detect_current_shell();
    shell_ensure_path(shell, shim_dir);
    printf("Restart your shell (or re-source its startup file) for the PATH change to take "
           "effect.\n");

    return 0;
}
