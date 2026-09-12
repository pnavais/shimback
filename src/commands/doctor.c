#include "commands.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../config.h"
#include "../paths.h"
#include "../util.h"

/* Set once at the start of cmd_doctor and read by the report_ok/report_fail/
 * check_* helpers below -- a single-shot CLI command has no concurrency to
 * worry about, so a file-scope flag is simpler than threading a colorize
 * parameter through every helper's signature. */
static bool g_colorize = false;

static void report_ok(const char *fmt, ...) {
    va_list ap;
    if (g_colorize) {
        printf("  [%s%sok%s]   ", ANSI_BOLD, ANSI_GREEN, ANSI_RESET);
    } else {
        printf("  [ok]   ");
    }
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void report_fail(int *issues, const char *fmt, ...) {
    va_list ap;
    if (g_colorize) {
        printf("  [%s%sfail%s] ", ANSI_BOLD, ANSI_RED, ANSI_RESET);
    } else {
        printf("  [fail] ");
    }
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    (*issues)++;
}

static void report_fixed(const char *fmt, ...) {
    va_list ap;
    if (g_colorize) {
        printf("  [%s%sfixed%s] ", ANSI_BOLD, ANSI_CYAN, ANSI_RESET);
    } else {
        printf("  [fixed] ");
    }
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* If <shim_dir>/<name> is missing entirely, or is a symlink whose target no
 * longer exists (dangling), recreates it pointing at `self_exe` -- the same
 * thing `add` does when it first creates a shim's symlink. Never touches a
 * path occupied by anything else (a plain file, a directory, or a symlink
 * that still resolves, even to a different-but-valid shimback binary
 * elsewhere) -- only genuinely dead or missing entries are "needed" fixes. */
static void fix_symlink_if_needed(const char *shim_dir, const char *name, const char *self_exe) {
    char *link_path = path_join(shim_dir, name);

    struct stat lst;
    int lst_rc = lstat(link_path, &lst);
    bool is_link = lst_rc == 0 && S_ISLNK(lst.st_mode);

    bool dangling = false;
    if (is_link) {
        struct stat st;
        dangling = stat(link_path, &st) != 0;
    }
    bool missing = lst_rc != 0;

    if (!missing && !dangling) {
        free(link_path);
        return;
    }

    if (is_link) {
        unlink(link_path); /* drop the dangling symlink before recreating it */
    }
    if (symlink(self_exe, link_path) == 0) {
        report_fixed("recreated %s symlink -> %s", missing ? "missing" : "dangling", self_exe);
    } else {
        warn("doctor fix: failed to recreate symlink for '%s': %s", name, strerror(errno));
    }
    free(link_path);
}

/* Interactively prompts for a replacement value for `field_label`
 * ("source" or "fallback") of shim `shim_name`, whose current value
 * `current_value` resolves back to shimback itself -- a cycle. Re-resolves
 * and re-checks each answer, looping until a non-cyclic value is given or
 * stdin runs out (EOF, e.g. non-interactive stdin, or Ctrl+D); Ctrl+C just
 * kills the whole process via the terminal's ordinary SIGINT handling, no
 * special signal code needed here. Returns a newly allocated resolved path,
 * or NULL if the prompt was never satisfied. */
static char *prompt_fix_cycle(const char *shim_name, const char *field_label,
                               const char *current_value, const char *self_exe) {
    printf("  %s '%s' for shim '%s' resolves back to the shimback binary itself (a cycle).\n",
           field_label, current_value, shim_name);

    char line[4096];
    for (;;) {
        printf("  Enter a corrected %s (Ctrl+C to abort): ", field_label);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n  Leaving '%s' as-is.\n", current_value);
            return NULL;
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        char *resolved = resolve_binary_arg(line);
        if (!resolved) {
            printf("  '%s' does not exist, is not executable, or isn't on $PATH -- try again.\n",
                   line);
            continue;
        }
        if (strcmp(resolved, self_exe) == 0) {
            printf("  '%s' still resolves back to the shimback binary itself -- try again.\n",
                   line);
            free(resolved);
            continue;
        }
        return resolved;
    }
}

/* If `entry`'s fallback (or, if explicit, its source) resolves to the
 * shimback binary itself -- a cycle, since dispatching through it would
 * just re-invoke this same shim forever -- interactively prompts for a
 * replacement (see prompt_fix_cycle) and updates `entry` in place. Returns
 * true if anything changed (the config needs saving). Leaves a value the
 * user didn't fix untouched -- the normal check_source/check_fallback pass
 * right after this still reports it as a failure. */
static bool fix_cycle_if_needed(ShimEntry *entry, const char *self_exe) {
    bool changed = false;

    if (entry->source) {
        char *resolved = canonicalize(entry->source);
        bool cyclic = resolved && strcmp(resolved, self_exe) == 0;
        free(resolved);
        if (cyclic) {
            char *fixed = prompt_fix_cycle(entry->name, "source", entry->source, self_exe);
            if (fixed) {
                report_fixed("source updated to %s", fixed);
                free(entry->source);
                entry->source = fixed;
                changed = true;
            }
        }
    }

    char *resolved_fb = canonicalize(entry->fallback);
    bool fb_cyclic = resolved_fb && strcmp(resolved_fb, self_exe) == 0;
    free(resolved_fb);
    if (fb_cyclic) {
        char *fixed = prompt_fix_cycle(entry->name, "fallback", entry->fallback, self_exe);
        if (fixed) {
            report_fixed("fallback updated to %s", fixed);
            free(entry->fallback);
            entry->fallback = fixed;
            changed = true;
        }
    }

    return changed;
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
 * executable file, reporting a failure and returning NULL otherwise. Also
 * reports (but doesn't return NULL for) a fallback that resolves back to
 * the shimback binary itself -- a cycle -- since the resolved path is still
 * useful to the caller's source==fallback check. */
static char *check_fallback(int *issues, const char *fallback, const char *self_exe) {
    if (!is_executable_file(fallback)) {
        report_fail(issues, "fallback '%s' does not exist or is not executable", fallback);
        return NULL;
    }
    char *resolved = canonicalize(fallback);
    if (resolved && strcmp(resolved, self_exe) == 0) {
        report_fail(issues,
                     "fallback '%s' resolves back to the shimback binary itself -- this shim "
                     "would loop forever if invoked (run `shimback doctor fix` to repair)",
                     fallback);
    } else {
        report_ok("fallback: %s", fallback);
    }
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
        if (resolved_source && strcmp(resolved_source, self_exe) == 0) {
            report_fail(issues,
                         "source '%s' resolves back to the shimback binary itself -- this shim "
                         "would loop forever if invoked (run `shimback doctor fix` to repair)",
                         e->source);
        } else {
            report_ok("source: %s", e->source);
        }
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
    bool fix_mode = false;
    if (argc > 1) {
        if (strcmp(argv[1], "fix") == 0) {
            fix_mode = true;
        } else {
            die("doctor: unexpected argument '%s' (did you mean `shimback doctor fix`?)",
                argv[1]);
        }
        if (argc > 2) {
            die("doctor: unexpected argument '%s'", argv[2]);
        }
    }

    int issues = 0;
    g_colorize = stdout_is_color();
    const char *hdr = g_colorize ? ANSI_BOLD ANSI_YELLOW : "";
    const char *name_color = g_colorize ? ANSI_BOLD ANSI_CYAN : "";
    const char *reset = g_colorize ? ANSI_RESET : "";

    char *cfg_path = config_file_path();
    Config cfg;
    char errbuf[256];
    ConfigStatus cst = config_load(cfg_path, &cfg, errbuf, sizeof(errbuf));
    printf("%sconfig:%s %s\n", hdr, reset, cfg_path);
    if (cst != CONFIG_OK) {
        report_fail(&issues, "%s", errbuf);
        printf("\n%sshimback doctor: %d issue(s) found%s\n", g_colorize ? ANSI_BOLD ANSI_RED : "",
               issues, reset);
        return 1;
    }
    report_ok("parsed successfully");
    printf("\n");

    char *shim_dir = shim_bin_dir();
    printf("%sshim directory:%s %s\n", hdr, reset, shim_dir);
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
    bool config_dirty = false;

    printf("%s%zu shim(s) configured:%s\n", hdr, cfg.count, reset);
    for (size_t i = 0; i < cfg.count; i++) {
        ShimEntry *e = &cfg.shims[i];
        printf("\n%s%s%s\n", name_color, e->name, reset);

        if (fix_mode) {
            fix_symlink_if_needed(shim_dir, e->name, self_exe);
            config_dirty = fix_cycle_if_needed(e, self_exe) || config_dirty;
        }
        check_symlink(&issues, shim_dir, e->name);
        char *resolved_fallback = check_fallback(&issues, e->fallback, self_exe);
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
        if (e->policy == POLICY_ROUTE_ARGS && e->route_arg_count == 0) {
            report_fail(&issues,
                         "policy is route-args but no --route-arg is configured -- this shim "
                         "will always run its source");
        }
    }
    printf("\n");

    if (config_dirty) {
        ConfigStatus save_st = config_save(&cfg, cfg_path, errbuf, sizeof(errbuf));
        if (save_st != CONFIG_OK) {
            warn("doctor fix: failed to save config: %s", errbuf);
        } else {
            printf("shimback doctor: saved config changes to %s\n\n", cfg_path);
        }
    }

    if (issues > 0) {
        printf("%sshimback doctor: %d issue(s) found%s\n", g_colorize ? ANSI_BOLD ANSI_RED : "",
               issues, reset);
    } else {
        printf("%sshimback doctor: all checks passed%s\n", g_colorize ? ANSI_BOLD ANSI_GREEN : "",
               reset);
    }
    return issues > 0 ? 1 : 0;
}
