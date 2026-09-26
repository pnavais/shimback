#include "commands.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../config.h"
#include "../installation.h"
#include "../paths.h"
#include "../platform/platform.h"
#include "../shell.h"
#include "../util.h"

/* Set once at the start of cmd_doctor and read by the report_ok/report_fail/
 * check_* helpers below -- a single-shot CLI command has no concurrency to
 * worry about, so a file-scope flag is simpler than threading a colorize
 * parameter through every helper's signature. */
static bool g_colorize = false;

/* Matches shell.c's DEFAULT_TAG / uninstall.c's SHIM_DIR_TAG -- the single
 * marker tag add/init/install all share for the shim directory's PATH
 * block. */
#define SHIM_DIR_TAG "shimback"

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

/* Unlike report_fail, doesn't increment *issues or affect doctor's exit
 * code -- for suggestions that are worth surfacing but aren't a broken
 * setup (e.g. a PATH block that would be better off somewhere else). */
static void report_warn(const char *fmt, ...) {
    va_list ap;
    if (g_colorize) {
        printf("  [%s%swarn%s] ", ANSI_BOLD, ANSI_YELLOW, ANSI_RESET);
    } else {
        printf("  [warn] ");
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
    char *shim_file = shim_file_name(name);
    char *link_path = path_join(shim_dir, shim_file);
    free(shim_file);

    /* Held across the whole check-then-recreate sequence below, so a
     * concurrent `add`/`remove`/`doctor fix` racing the same symlink as
     * the same user can't land in between the check and the unlink()+
     * create that acts on it (see review.md). shim_dir is guaranteed
     * to exist here -- this is only ever reached for an already-known
     * shim entry. */
    int shim_lock_fd = shim_dir_lock_acquire(shim_dir);
    if (shim_lock_fd < 0) {
        warn("doctor fix: failed to acquire the shim directory lock on %s: %s -- skipping '%s'",
             shim_dir, strerror(errno), name);
        free(link_path);
        return;
    }

#ifdef _WIN32
    /* No "dangling" case on Windows at all (see windows-port.md Phase 3):
     * a hard link has nothing separate to go missing out from under it --
     * as long as this entry's own link exists, its file data is alive
     * regardless of what happens to self_exe's own path. "missing" is the
     * only repairable case there is to check for, and access() (which
     * would be wrong on POSIX below -- it follows symlinks, conflating
     * "missing" with "dangling") is exactly right here, since there's no
     * separate target-resolution step to distinguish it from at all. */
    bool missing = access(link_path, F_OK) != 0;
    bool dangling = false;
#else
    /* lstat(), not access()/stat(): this must NOT follow the symlink --
     * "missing" (the entry itself isn't there) and "dangling" (the entry
     * is a symlink, but its target is gone) are different repairable
     * cases with different messages, and access()/stat() alone can't tell
     * them apart from each other (both would just report "doesn't exist"
     * for either case, since both follow symlinks to their target). */
    struct stat lst;
    bool missing = lstat(link_path, &lst) != 0;
    bool is_link = !missing && S_ISLNK(lst.st_mode);
    bool dangling = false;
    if (is_link) {
        struct stat st;
        dangling = stat(link_path, &st) != 0;
    }
#endif

    if (!missing && !dangling) {
        shim_dir_lock_release(shim_lock_fd);
        free(link_path);
        return;
    }

#ifndef _WIN32
    if (is_link) {
        unlink(link_path); /* drop the dangling symlink before recreating it */
    }
#endif
    if (create_shim_link(self_exe, link_path, "doctor fix")) {
        report_fixed("recreated %s symlink %s -> %s", missing ? "missing" : "dangling", link_path,
                     self_exe);
    } else {
        int link_errno = errno;
        char link_err_msg[1024];
        format_link_create_error(link_err_msg, sizeof(link_err_msg), "doctor fix", link_path,
                                  self_exe, link_errno);
        warn("%s", link_err_msg);
    }
    shim_dir_lock_release(shim_lock_fd);
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

/* Interactively confirms removing an orphaned shim symlink -- a real
 * shimback-managed symlink with no configuration anywhere for it (no
 * config.toml entry, no split config file in any of its three potential
 * locations; see ShimSource/resolve_shim_entry). `auto_yes` (`doctor fix
 * -y`) skips the prompt outright. On EOF (non-interactive stdin, same as
 * prompt_fix_cycle above), treats it as "no" rather than hanging. */
static bool confirm_remove_orphan(const char *name, bool auto_yes) {
    if (auto_yes) {
        return true;
    }
    printf("  '%s' has a real shim symlink but no configuration anywhere for it (no "
           "config.toml entry, no split config file). Remove the symlink? [y/N] ",
           name);
    fflush(stdout);
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) {
        printf("\n  Leaving '%s' as-is.\n", name);
        return false;
    }
    return line[0] == 'y' || line[0] == 'Y';
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

    char *resolved_fb = entry->fallback ? canonicalize(entry->fallback) : NULL;
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
    char *tok = strtok_r(copy, PLAT_PATH_LIST_SEP, &saveptr);
    while (tok) {
        if (strcmp(tok, dir) == 0) {
            found = true;
            break;
        }
        tok = strtok_r(NULL, PLAT_PATH_LIST_SEP, &saveptr);
    }
    free(copy);
    return found;
}

/* Checks that <shim_dir>/<name> exists, is a symlink, and isn't dead (its
 * target still exists and is executable). Missing/dead symlinks are what
 * `doctor fix` repairs, so the hint pointing at it is left out in fix mode,
 * where a failure surviving the repair attempt means the repair itself
 * failed (already warned about separately). */
static void check_symlink(int *issues, const char *shim_dir, const char *name, bool fix_mode) {
    char *shim_file = shim_file_name(name);
    char *link_path = path_join(shim_dir, shim_file);
    free(shim_file);
    const char *fix_hint = fix_mode ? "" : " -- run `shimback doctor fix` to recreate it";

#ifdef _WIN32
    /* No separate "target" to resolve and check for a hard link -- its
     * content already *is* the target's content (see windows-port.md
     * Phase 3), so looks_like_shimback_binary() (which itself already
     * checks is_executable_file internally) is both the existence and the
     * validity check in one, unlike the POSIX branch's three separate
     * steps below. */
    if (access(link_path, F_OK) != 0) {
        report_fail(issues, "no shim at %s%s", link_path, fix_hint);
    } else if (!looks_like_shimback_binary(link_path)) {
        report_fail(issues, "%s exists but doesn't look like a shimback binary", link_path);
    } else {
        report_ok("shim %s (hard link to the shimback binary)", link_path);
    }
    free(link_path);
#else
    struct stat lst;
    if (lstat(link_path, &lst) != 0) {
        report_fail(issues, "no symlink at %s%s", link_path, fix_hint);
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
        report_fail(issues, "symlink %s is dead (target does not exist)%s", link_path, fix_hint);
        free(raw_target);
        free(link_path);
        return;
    }

    char *target = canonicalize(link_path);
    if (!target || !is_executable_file(target)) {
        report_fail(issues, "symlink %s -> %s, which is not executable", link_path,
                     target ? target : "?");
    } else {
        report_ok("symlink %s -> %s", link_path, target);
    }
    free(target);
    free(link_path);
#endif
}

/* Returns a newly allocated canonical path if `fallback` is a valid,
 * executable file, reporting a failure and returning NULL otherwise. Also
 * reports (but doesn't return NULL for) a fallback that resolves back to
 * the shimback binary itself -- a cycle -- since the resolved path is still
 * useful to the caller's source==fallback check. A NULL `fallback` (only
 * valid for POLICY_REWRITE and POLICY_ROUTE_MAP, neither of which ever
 * uses it) is reported as fine, not checked at all, and returns NULL. When
 * `force` is set (the shim was added with `add --force`) and `fallback`
 * still doesn't exist, that's reported as fine rather than a failure --
 * but only while it's actually still missing; once it exists, the full
 * normal check below applies regardless of force. */
static char *check_fallback(int *issues, const char *fallback, const char *self_exe, bool force) {
    if (!fallback) {
        report_ok("fallback: none (not used by this policy)");
        return NULL;
    }
    if (!is_executable_file(fallback)) {
        if (force) {
            report_ok("fallback: %s (added with --force; not currently on disk, so not checked)",
                        fallback);
            return xstrdup(fallback);
        }
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
 * be NULL if the fallback check above already failed. `force` behaves as in
 * check_fallback above: a still-missing source added with --force is
 * reported as fine rather than a failure. */
static void check_source(int *issues, const ShimEntry *e, const char *name, const char *shim_dir,
                          const char *self_exe, const char *resolved_fallback, bool force) {
    char *resolved_source = NULL;
    if (e->source) {
        if (!is_executable_file(e->source)) {
            if (force) {
                report_ok(
                    "source: %s (added with --force; not currently on disk, so not checked)",
                    e->source);
                resolved_source = xstrdup(e->source);
                goto cross_check;
            }
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

cross_check:
    if (resolved_fallback && resolved_source && strcmp(resolved_source, resolved_fallback) == 0 &&
        str_array_eq(e->source_args, e->source_arg_count, e->fallback_args,
                     e->fallback_arg_count)) {
        report_fail(issues,
                     "source and fallback currently resolve to the same binary (%s) with the "
                     "same arguments -- this shim is a no-op right now",
                     resolved_source);
    }
    free(resolved_source);
}

int cmd_doctor(int argc, char **argv) {
    bool fix_mode = false;
    bool auto_yes = false;
    if (argc > 1) {
        if (strcmp(argv[1], "fix") != 0) {
            die("doctor: unexpected argument '%s' (did you mean `shimback doctor fix`?)",
                argv[1]);
        }
        fix_mode = true;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
                auto_yes = true;
            } else {
                die("doctor: unexpected argument '%s'", argv[i]);
            }
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
    bool shim_dir_missing = access(shim_dir, F_OK) != 0;
    /* Same thing `init` does; without it every per-shim symlink repair below
     * would fail on the shim directory lock. Only worth doing when shims are
     * actually configured -- otherwise there's nothing to put in it. */
    if (shim_dir_missing && cfg.count > 0 && fix_mode) {
        if (mkdir_p(shim_dir)) {
            report_fixed("created shim directory %s", shim_dir);
            shim_dir_missing = false;
        } else {
            warn("doctor fix: failed to create shim directory %s: %s", shim_dir, strerror(errno));
        }
    }
    if (shim_dir_missing) {
        if (cfg.count > 0) {
            report_fail(&issues,
                        "directory does not exist, but %zu shim(s) are configured%s", cfg.count,
                        fix_mode ? "" : " -- run `shimback doctor fix` to create it");
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

    /* Only one installation is allowed (see `install`); the PATH block is
     * where one is recorded. More than one can only come from a version that
     * predates that rule, and a recorded directory with no binary in it is
     * a leftover. Both are worth surfacing but neither breaks anything, so
     * they're warnings -- they don't change doctor's exit code. */
    printf("%sinstallation:%s\n", hdr, reset);
    {
        Installation *installs = NULL;
        size_t install_count = find_installations(&installs);
        StrVec stale;
        strvec_init(&stale);
        find_stale_block_dirs(&stale);

        if (install_count == 1) {
            report_ok("installed at %s", installs[0].binary);
        } else if (install_count == 0 && stale.count == 0) {
            report_ok("not installed (no `shimback install` recorded in the PATH block)");
        }
        if (install_count > 1) {
            report_warn("%zu installations are recorded in the PATH block, but only one is "
                        "supported -- run `shimback uninstall --prefix <dir>` for each one you "
                        "don't want (or `shimback uninstall` for all of them), then "
                        "`shimback install` once:",
                        install_count);
            for (size_t i = 0; i < install_count; i++) {
                printf("           - %s\n", installs[i].binary);
            }
        }
        for (size_t i = 0; i < stale.count; i++) {
            report_warn("stale PATH entry %s -- no shimback binary there any more (harmless; "
                        "delete it from the `# >>> shimback >>>` block in your shell startup "
                        "file, or `shimback uninstall` then `shimback install` to rewrite it)",
                        stale.items[i]);
        }
        strvec_free(&stale);
        free_installations(installs, install_count);
    }
    printf("\n");

    if (detect_current_shell() == SHELL_ZSH && shell_zsh_block_needs_migration(SHIM_DIR_TAG)) {
        if (fix_mode) {
            if (shell_zsh_migrate_block_to_local(SHIM_DIR_TAG)) {
                report_fixed("moved the shimback PATH block from ~/.zshrc to ~/.zshrc.local");
            } else {
                warn("doctor fix: failed to migrate the PATH block to ~/.zshrc.local");
            }
        } else {
            report_warn(
                "~/.zshrc.local exists, but the shimback PATH block is still in ~/.zshrc -- "
                "most zsh setups source ~/.zshrc.local for machine-local overrides, so that's "
                "the more appropriate place for it now (run `shimback doctor fix` to move it)");
        }
        printf("\n");
    }

    size_t name_count = 0;
    char **names = collect_all_shim_names(&cfg, &name_count);

    if (name_count == 0) {
        printf("No shims configured.\n");
        return issues > 0 ? 1 : 0;
    }

    char *self_exe = self_exe_path();
    bool config_dirty = false;

    printf("%s%zu shim(s):%s\n", hdr, name_count, reset);
    for (size_t i = 0; i < name_count; i++) {
        const char *name = names[i];
        printf("\n%s%s%s\n", name_color, name, reset);

        ShimEntry *e = NULL;
        char *split_path = NULL;
        ShimSource source = resolve_shim_entry(&cfg, name, &e, &split_path);

        if (source == SHIM_SOURCE_ORPHAN) {
            if (fix_mode && confirm_remove_orphan(name, auto_yes)) {
                /* Held from here through the unlink() below -- deliberately
                 * NOT across confirm_remove_orphan() above, which can wait
                 * indefinitely on user input; holding the lock that long
                 * would block every other shimback command for no reason.
                 * Orphan status is re-verified fresh inside the lock
                 * before acting, since a concurrent `add` could have
                 * legitimately reclaimed this exact name while the prompt
                 * was waiting (see review.md). */
                char *shim_file = shim_file_name(name);
                char *link_path = path_join(shim_dir, shim_file);
                free(shim_file);
                int shim_lock_fd = shim_dir_lock_acquire(shim_dir);
                if (shim_lock_fd < 0) {
                    warn("doctor fix: failed to acquire the shim directory lock on %s: %s -- "
                         "skipping '%s'",
                         shim_dir, strerror(errno), name);
                    free(link_path);
                    continue;
                }
                char *recheck_split = resolve_split_config_path(name);
                bool still_orphan = !config_find(&cfg, name) && !recheck_split;
                free(recheck_split);
                if (!still_orphan) {
                    warn("doctor fix: '%s' is no longer orphaned (a config entry appeared since "
                         "it was checked) -- leaving its symlink alone",
                         name);
                    shim_dir_lock_release(shim_lock_fd);
                    free(link_path);
                    continue;
                }
                if (unlink(link_path) == 0) {
                    report_fixed("removed orphaned symlink (no configuration found for it)");
                } else {
                    warn("doctor fix: failed to remove orphaned symlink for '%s': %s", name,
                         strerror(errno));
                }
                shim_dir_lock_release(shim_lock_fd);
                free(link_path);
            } else {
                report_fail(&issues,
                             "orphaned symlink -- no config.toml entry or split config file "
                             "found (run `shimback doctor fix` to remove it, or `shimback "
                             "doctor fix -y` to remove every orphan without asking)");
            }
            continue;
        }

        if (source == SHIM_SOURCE_SPLIT) {
            report_ok("config: split file at %s", split_path);
        }

        if (fix_mode) {
            fix_symlink_if_needed(shim_dir, name, self_exe);
            bool entry_dirty = fix_cycle_if_needed(e, self_exe);
            if (entry_dirty) {
                if (source == SHIM_SOURCE_SPLIT) {
                    char split_errbuf[256];
                    ConfigStatus split_st =
                        config_save_split(e, split_path, split_errbuf, sizeof(split_errbuf));
                    if (split_st != CONFIG_OK) {
                        warn("doctor fix: failed to save split config for '%s': %s", name,
                             split_errbuf);
                    } else {
                        printf("shimback doctor: saved split config changes to %s\n", split_path);
                    }
                } else {
                    config_dirty = true;
                }
            }
        }
        check_symlink(&issues, shim_dir, name, fix_mode);
        char *resolved_fallback = check_fallback(&issues, e->fallback, self_exe, e->force);
        check_source(&issues, e, name, shim_dir, self_exe, resolved_fallback, e->force);
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
        if (e->policy == POLICY_SPLIT_ARGS &&
            (e->source_route_arg_count == 0 || e->fallback_route_arg_count == 0)) {
            report_fail(&issues,
                         "policy is split-args but needs at least one --split-source-arg and "
                         "one --split-fallback-arg -- this shim can never fully match either "
                         "side");
        }
        if (e->policy == POLICY_REWRITE && e->rewrite_from_count == 0) {
            report_fail(&issues,
                         "policy is rewrite but no --rewrite rule is configured -- this shim "
                         "will never rewrite anything");
        }
        if (e->policy == POLICY_ROUTE_MAP) {
            if (e->route_count == 0) {
                report_fail(&issues,
                             "policy is route-map but no --route is configured -- this shim "
                             "will always run its source");
            }
            for (size_t r = 0; r < e->route_count; r++) {
                const RouteEntry *route = &e->routes[r];
                if (!is_executable_file(route->command)) {
                    if (e->force) {
                        report_ok(
                            "route '%s': %s (added with --force; not currently on disk, so "
                            "not checked)",
                            route->match, route->command);
                    } else {
                        report_fail(&issues, "route '%s': %s not found or not executable",
                                    route->match, route->command);
                    }
                    continue;
                }
                char *resolved_route = canonicalize(route->command);
                if (resolved_route && strcmp(resolved_route, self_exe) == 0) {
                    report_fail(&issues,
                                 "route '%s' resolves back to the shimback binary itself -- "
                                 "would loop forever if triggered (run `shimback doctor fix` "
                                 "to repair)",
                                 route->match);
                } else {
                    report_ok("route '%s': %s", route->match, route->command);
                }
                free(resolved_route);
            }
        }
        if (e->policy == POLICY_PASSTHROUGH && e->diagnostic) {
            report_fail(&issues,
                         "policy is passthrough, which never captures or hides anything -- "
                         "diagnostic has nothing to report");
        }
        if (e->policy == POLICY_PASSTHROUGH && e->capture_timeout_set) {
            report_fail(&issues,
                         "policy is passthrough, which never captures output -- a capture "
                         "timeout has nothing to apply to");
        }
        if (e->policy == POLICY_PASSTHROUGH && e->capture_limit_set) {
            report_fail(&issues,
                         "policy is passthrough, which never captures output -- a capture "
                         "limit has nothing to apply to");
        }

        if (source == SHIM_SOURCE_SPLIT) {
            shim_entry_free(e);
            free(e);
        }
        free(split_path);
    }
    printf("\n");

    if (config_dirty) {
        /* Narrowly held around just this save, not doctor's whole
         * (potentially long, interactive) run from its initial
         * config_load above -- matching the same "lock right before the
         * actual mutation" approach already used for orphan removal, not
         * the wider "lock the whole operation" one add/remove use, since
         * doctor's own run can span an unbounded interactive prompt
         * (see review.md). This prevents a concurrent add/remove from
         * racing *this* save specifically; it doesn't fully close the
         * separate, narrower risk of doctor's own in-memory `cfg` having
         * gone stale relative to a change made by something else earlier
         * in a long interactive session -- an accepted, lower-priority
         * residual gap for a distinctly less frequent, human-supervised
         * operation than add/remove's much tighter loop. */
        int shim_lock_fd = shim_dir_lock_acquire(shim_dir);
        if (shim_lock_fd < 0) {
            warn("doctor fix: failed to acquire the shim directory lock on %s: %s -- saving "
                 "config anyway",
                 shim_dir, strerror(errno));
        }
        ConfigStatus save_st = config_save(&cfg, cfg_path, errbuf, sizeof(errbuf));
        shim_dir_lock_release(shim_lock_fd);
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
