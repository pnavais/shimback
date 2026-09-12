#include "commands.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../config.h"
#include "../paths.h"
#include "../util.h"

static void report_ok(const char *fmt, ...) {
    va_list ap;
    printf("  [ok]   ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void report_fail(int *issues, const char *fmt, ...) {
    va_list ap;
    printf("  [fail] ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    (*issues)++;
}

static bool dir_on_path(const char *dir) {
    const char *path_env = getenv("PATH");
    if (!path_env) {
        return false;
    }
    char *copy = xstrdup(path_env);
    bool found = false;
    char *saveptr = NULL;
    char *tok = strtok_r(copy, ":", &saveptr);
    while (tok) {
        if (strcmp(tok, dir) == 0) {
            found = true;
            break;
        }
        tok = strtok_r(NULL, ":", &saveptr);
    }
    free(copy);
    return found;
}

/* Checks that <shim_dir>/<name> exists, is a symlink, and isn't dead (its
 * target still exists and is executable). */
static void check_symlink(int *issues, const char *shim_dir, const char *name) {
    char *link_path = path_join(shim_dir, name);

    struct stat lst;
    if (lstat(link_path, &lst) != 0) {
        report_fail(issues, "no symlink at %s -- re-run `shimback add %s ...`", link_path, name);
        free(link_path);
        return;
    }
    if (!S_ISLNK(lst.st_mode)) {
        report_fail(issues, "%s exists but is not a symlink", link_path);
        free(link_path);
        return;
    }

    struct stat st;
    if (stat(link_path, &st) != 0) {
        char *raw_target = canonicalize(link_path);
        report_fail(issues, "symlink %s is dead (target does not exist)", link_path);
        free(raw_target);
        free(link_path);
        return;
    }

    char *target = canonicalize(link_path);
    if (!target || !is_executable_file(target)) {
        report_fail(issues, "symlink %s -> %s, which is not executable", link_path,
                     target ? target : "?");
    } else {
        report_ok("symlink -> %s", target);
    }
    free(target);
    free(link_path);
}

/* Returns a newly allocated canonical path if `fallback` is a valid,
 * executable file, reporting a failure and returning NULL otherwise. */
static char *check_fallback(int *issues, const char *fallback) {
    if (!is_executable_file(fallback)) {
        report_fail(issues, "fallback '%s' does not exist or is not executable", fallback);
        return NULL;
    }
    char *resolved = canonicalize(fallback);
    report_ok("fallback: %s", fallback);
    return resolved;
}

/* Mirrors dispatch_run's source resolution (see dispatch.c) so doctor
 * reports exactly what a real invocation would see. `resolved_fallback` may
 * be NULL if the fallback check above already failed. */
static void check_source(int *issues, const ShimEntry *e, const char *name, const char *shim_dir,
                          const char *self_exe, const char *resolved_fallback) {
    char *resolved_source = NULL;
    if (e->source) {
        if (!is_executable_file(e->source)) {
            report_fail(issues, "source '%s' does not exist or is not executable", e->source);
            return;
        }
        resolved_source = canonicalize(e->source);
        report_ok("source: %s", e->source);
    } else {
        resolved_source = path_search(name, shim_dir, self_exe);
        if (!resolved_source) {
            report_fail(issues, "source: auto, but no '%s' found on $PATH (besides this shim)",
                         name);
            return;
        }
        report_ok("source: auto -> %s", resolved_source);
    }

    if (resolved_fallback && resolved_source && strcmp(resolved_source, resolved_fallback) == 0) {
        report_fail(issues,
                     "source and fallback currently resolve to the same binary (%s) -- this "
                     "shim is a no-op right now",
                     resolved_source);
    }
    free(resolved_source);
}

int cmd_doctor(int argc, char **argv) {
    if (argc > 1) {
        die("doctor: unexpected argument '%s'", argv[1]);
    }

    int issues = 0;

    char *cfg_path = config_file_path();
    Config cfg;
    char errbuf[256];
    ConfigStatus cst = config_load(cfg_path, &cfg, errbuf, sizeof(errbuf));
    printf("config: %s\n", cfg_path);
    if (cst != CONFIG_OK) {
        report_fail(&issues, "%s", errbuf);
        printf("\nshimback doctor: %d issue(s) found\n", issues);
        return 1;
    }
    report_ok("parsed successfully");
    printf("\n");

    char *shim_dir = shim_bin_dir();
    printf("shim directory: %s\n", shim_dir);
    if (access(shim_dir, F_OK) != 0) {
        if (cfg.count > 0) {
            report_fail(&issues,
                        "directory does not exist, but %zu shim(s) are configured -- run "
                        "`shimback init`",
                        cfg.count);
        } else {
            report_ok("not created yet (no shims added, and `init` hasn't been run)");
        }
    } else if (dir_on_path(shim_dir)) {
        report_ok("on $PATH");
    } else {
        report_fail(&issues,
                     "not on $PATH in this shell -- restart your shell (or re-source its "
                     "startup file)");
    }
    printf("\n");

    if (cfg.count == 0) {
        printf("No shims configured.\n");
        return issues > 0 ? 1 : 0;
    }

    char *self_exe = self_exe_path();

    printf("%zu shim(s) configured:\n", cfg.count);
    for (size_t i = 0; i < cfg.count; i++) {
        ShimEntry *e = &cfg.shims[i];
        printf("\n%s\n", e->name);

        check_symlink(&issues, shim_dir, e->name);
        char *resolved_fallback = check_fallback(&issues, e->fallback);
        check_source(&issues, e, e->name, shim_dir, self_exe, resolved_fallback);
        free(resolved_fallback);

        if (e->policy == POLICY_HEURISTIC && e->error_pattern_count == 0) {
            report_fail(&issues,
                         "policy is heuristic but no --error-pattern is configured -- this shim "
                         "will never fall back");
        }
        if (e->policy == POLICY_EXIT_CODE_MATCH && e->exit_code_count == 0) {
            report_fail(&issues,
                         "policy is exit-code-match but no --exit-code is configured -- this "
                         "shim will never fall back");
        }
    }
    printf("\n");

    if (issues > 0) {
        printf("shimback doctor: %d issue(s) found\n", issues);
    } else {
        printf("shimback doctor: all checks passed\n");
    }
    return issues > 0 ? 1 : 0;
}
