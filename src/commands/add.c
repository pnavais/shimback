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
#include "../tui.h"
#include "../util.h"
#include "add_wizard.h"

static const char *USAGE =
    "usage: shimback add <name> [-s <source>] -f <fallback>\n"
    "                    [--policy exit-code|heuristic|exit-code-match|route-args|rewrite]\n"
    "                    [--error-pattern <p>]... [--exit-code <code>]...\n"
    "                    [--route-arg <arg>]... [--strip-matched-args]\n"
    "                    [--rewrite <from>=<to>]... [--diagnostic]\n"
    "-f/--fallback is required, except with --policy rewrite, where it's unused.\n";

#define OPT_STRIP_MATCHED_ARGS 1000

static void push_exit_code(int **arr, size_t *count, size_t *cap, int value) {
    if (*count == *cap) {
        *cap = *cap == 0 ? 4 : *cap * 2;
        *arr = xrealloc(*arr, *cap * sizeof(int));
    }
    (*arr)[(*count)++] = value;
}

/* Everything from policy validation through creating the symlink and
 * writing the config entry -- shared by both the "everything was already
 * given on the command line" path and the "the interactive wizard filled
 * in what was missing" path, so the two can never drift apart. */
static int finish_add(const char *name, const char *source_arg, const char *fallback_arg,
                       Policy policy, StrVec *patterns, int *exit_codes, size_t exit_code_count,
                       StrVec *route_args, bool strip_matched_args, StrVec *rewrite_from,
                       StrVec *rewrite_to, bool diagnostic) {
    if (name[0] == '\0' || strchr(name, '/') != NULL || strcmp(name, "shimback") == 0) {
        die("add: invalid shim name '%s'", name);
    }

    char *self_exe = self_exe_path();

    char *resolved_fallback = NULL;
    if (fallback_arg) {
        resolved_fallback = resolve_binary_arg(fallback_arg);
        if (!resolved_fallback) {
            die("add: fallback '%s' does not exist, is not executable, or isn't on $PATH",
                fallback_arg);
        }
        if (strcmp(resolved_fallback, self_exe) == 0) {
            die("add: fallback '%s' resolves back to the shimback binary itself -- that would "
                "loop forever if this shim were ever invoked (did it resolve via $PATH to "
                "another shim, or to this one?)",
                fallback_arg);
        }
    }

    char *shim_dir = shim_bin_dir();

    char *resolved_source_for_check = NULL;
    if (source_arg) {
        resolved_source_for_check = resolve_binary_arg(source_arg);
        if (!resolved_source_for_check) {
            die("add: source '%s' does not exist, is not executable, or isn't on $PATH",
                source_arg);
        }
        if (strcmp(resolved_source_for_check, self_exe) == 0) {
            die("add: source '%s' resolves back to the shimback binary itself -- that would "
                "loop forever if this shim were ever invoked (did it resolve via $PATH to "
                "another shim, or to this one?)",
                source_arg);
        }
    } else {
        resolved_source_for_check = path_search(name, shim_dir, self_exe);
    }

    if (resolved_source_for_check && resolved_fallback &&
        strcmp(resolved_source_for_check, resolved_fallback) == 0) {
        die("add: source and fallback both resolve to '%s' -- refusing to add a no-op shim",
            resolved_fallback);
    }

    if (!mkdir_p(shim_dir)) {
        die("add: failed to create shim directory %s", shim_dir);
    }

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
    /* Store the resolved (canonicalized, and PATH-searched if bare) form,
     * not the raw argument -- this is what "resolved once and frozen in the
     * config" (see README) actually means, and it's what dispatch/doctor
     * already assume: a stable absolute path, not a bare name they'd have
     * to re-search $PATH for themselves. */
    entry->source = source_arg ? xstrdup(resolved_source_for_check) : NULL;
    free(entry->fallback);
    entry->fallback = resolved_fallback ? xstrdup(resolved_fallback) : NULL;
    entry->policy = policy;
    for (size_t i = 0; i < entry->error_pattern_count; i++) {
        free(entry->error_patterns[i]);
    }
    free(entry->error_patterns);
    entry->error_patterns = patterns->items; /* ownership transferred */
    entry->error_pattern_count = patterns->count;
    free(entry->exit_codes);
    entry->exit_codes = exit_codes; /* ownership transferred */
    entry->exit_code_count = exit_code_count;
    for (size_t i = 0; i < entry->route_arg_count; i++) {
        free(entry->route_args[i]);
    }
    free(entry->route_args);
    entry->route_args = route_args->items; /* ownership transferred */
    entry->route_arg_count = route_args->count;
    entry->strip_matched_args = strip_matched_args;
    for (size_t i = 0; i < entry->rewrite_from_count; i++) {
        free(entry->rewrite_from[i]);
    }
    free(entry->rewrite_from);
    entry->rewrite_from = rewrite_from->items; /* ownership transferred */
    entry->rewrite_from_count = rewrite_from->count;
    for (size_t i = 0; i < entry->rewrite_to_count; i++) {
        free(entry->rewrite_to[i]);
    }
    free(entry->rewrite_to);
    entry->rewrite_to = rewrite_to->items; /* ownership transferred */
    entry->rewrite_to_count = rewrite_to->count;
    entry->diagnostic = diagnostic;

    ConfigStatus save_st = config_save(&cfg, cfg_path, errbuf, sizeof(errbuf));
    if (save_st != CONFIG_OK) {
        die("add: failed to save config: %s", errbuf);
    }

    printf("shimback: '%s' -> %s (fallback: %s, policy: %s)\n", name, symlink_path,
           resolved_fallback ? resolved_fallback : "none", policy_to_string(policy));

    ShellKind shell = detect_current_shell();
    shell_ensure_path(shell, shim_dir);
    printf("Restart your shell (or re-source its startup file) for the PATH change to take "
           "effect.\n");

    return 0;
}

int cmd_add(int argc, char **argv) {
    const char *source_arg = NULL;
    const char *fallback_arg = NULL;
    const char *policy_arg = "exit-code";
    bool diagnostic = false;
    bool strip_matched_args = false;
    StrVec patterns;
    strvec_init(&patterns);
    StrVec route_args;
    strvec_init(&route_args);
    StrVec rewrite_from;
    strvec_init(&rewrite_from);
    StrVec rewrite_to;
    strvec_init(&rewrite_to);
    int *exit_codes = NULL;
    size_t exit_code_count = 0;
    size_t exit_code_cap = 0;

    static struct option long_opts[] = {
        {"source", required_argument, 0, 's'},
        {"fallback", required_argument, 0, 'f'},
        {"policy", required_argument, 0, 'p'},
        {"error-pattern", required_argument, 0, 'e'},
        {"exit-code", required_argument, 0, 'x'},
        {"route-arg", required_argument, 0, 'r'},
        {"strip-matched-args", no_argument, 0, OPT_STRIP_MATCHED_ARGS},
        {"rewrite", required_argument, 0, 'w'},
        {"diagnostic", no_argument, 0, 'd'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:f:p:e:x:r:w:d", long_opts, NULL)) != -1) {
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
            case 'r': strvec_push(&route_args, xstrdup(optarg)); break;
            case OPT_STRIP_MATCHED_ARGS: strip_matched_args = true; break;
            case 'w': {
                const char *eq = strchr(optarg, '=');
                if (!eq || eq == optarg) {
                    die("add: --rewrite must be '<from>=<to>' (got '%s')", optarg);
                }
                strvec_push(&rewrite_from, xstrndup(optarg, (size_t)(eq - optarg)));
                strvec_push(&rewrite_to, xstrdup(eq + 1));
                break;
            }
            case 'd': diagnostic = true; break;
            default:
                fprintf(stderr, "%s", USAGE);
                return 1;
        }
    }

    const char *name = (optind < argc) ? argv[optind++] : NULL;
    if (optind < argc) {
        die("add: unexpected extra argument '%s'", argv[optind]);
    }
    /* A name that was actually given but malformed is a hard error either
     * way (not "missing", so never wizard-eligible) -- finish_add() also
     * re-checks this (harmless for this path, the actual check for a
     * wizard-supplied name). */
    if (name && (name[0] == '\0' || strchr(name, '/') != NULL || strcmp(name, "shimback") == 0)) {
        die("add: invalid shim name '%s'", name);
    }

    Policy policy;
    if (!policy_from_string(policy_arg, &policy)) {
        die("add: --policy must be \"exit-code\", \"heuristic\", \"exit-code-match\", "
            "\"route-args\", or \"rewrite\"");
    }

    bool missing_name = (name == NULL);
    bool missing_fallback = (!fallback_arg && policy != POLICY_REWRITE);
    bool missing_patterns = (policy == POLICY_HEURISTIC && patterns.count == 0);
    bool missing_exit_codes = (policy == POLICY_EXIT_CODE_MATCH && exit_code_count == 0);
    bool missing_route_args = (policy == POLICY_ROUTE_ARGS && route_args.count == 0);
    bool missing_rewrite = (policy == POLICY_REWRITE && rewrite_from.count == 0);
    bool something_missing = missing_name || missing_fallback || missing_patterns ||
                              missing_exit_codes || missing_route_args || missing_rewrite;

    if (something_missing && !tui_supported()) {
        if (missing_name) {
            fprintf(stderr, "%s", USAGE);
            die("add: missing shim name");
        }
        if (missing_fallback) {
            fprintf(stderr, "%s", USAGE);
            die("add: -f/--fallback is required (except with --policy rewrite)");
        }
        if (missing_patterns) {
            die("add: --policy heuristic requires at least one --error-pattern");
        }
        if (missing_exit_codes) {
            die("add: --policy exit-code-match requires at least one --exit-code");
        }
        if (missing_route_args) {
            die("add: --policy route-args requires at least one --route-arg");
        }
        die("add: --policy rewrite requires at least one --rewrite <from>=<to>");
    }

    if (something_missing) {
        WizardSeed seed = {
            .name = name,
            .source_arg = source_arg,
            .fallback_arg = fallback_arg,
            .policy = policy,
            .patterns = &patterns,
            .exit_codes = exit_codes,
            .exit_code_count = exit_code_count,
            .route_args = &route_args,
            .strip_matched_args = strip_matched_args,
            .rewrite_from = &rewrite_from,
            .rewrite_to = &rewrite_to,
            .diagnostic = diagnostic,
        };
        WizardResult result;
        if (!run_add_wizard(&seed, &result)) {
            printf("Aborted -- no shim was created.\n");
            return 1;
        }
        return finish_add(result.name, result.source_arg, result.fallback_arg, result.policy,
                           &result.patterns, result.exit_codes, result.exit_code_count,
                           &result.route_args, result.strip_matched_args, &result.rewrite_from,
                           &result.rewrite_to, result.diagnostic);
    }

    return finish_add(name, source_arg, fallback_arg, policy, &patterns, exit_codes,
                       exit_code_count, &route_args, strip_matched_args, &rewrite_from,
                       &rewrite_to, diagnostic);
}
