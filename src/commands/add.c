/* Note: this is a short-lived CLI command handler. Heap allocations here are
 * intentionally not freed before process exit -- the OS reclaims them, and
 * this is a standard, deliberate simplification for one-shot CLI tools. */
#include "commands.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../config.h"
#include "../paths.h"
#include "../platform/platform.h"
#include "../shell.h"
#include "../tui.h"
#include "../util.h"
#include "add_wizard.h"

static const char *USAGE =
    "usage: shimback add <name> [-s <source>] [--source-arg <arg>]...\n"
    "                    -f <fallback> [--fallback-arg <arg>]...\n"
    "                    [--policy exit-code|heuristic|exit-code-match|route-args|rewrite|\n"
    "                              split-args|route-map]\n"
    "                    [--error-pattern <p>]... [--exit-code <code>]...\n"
    "                    [--route-arg <arg>]... [--strip-matched-args]\n"
    "                    [--split-source-arg <arg>]... [--split-fallback-arg <arg>]...\n"
    "                    [--rewrite <from>=<to>]... [--route <match>=<command>]...\n"
    "                    [--diagnostic] [--force] [-v|--verbose]\n"
    "                    [--capture-timeout <ms>] [--capture-limit <size>] [--split-config]\n"
    "-f/--fallback is required, except with --policy rewrite or route-map, where it's unused.\n"
    "--force allows source/fallback/a --route command to point at a path (not a bare name)\n"
    "that doesn't exist yet; doctor skips its existence check for whichever still doesn't.\n"
    "-v/--verbose prints the shell-startup-file PATH-update notices (silent by default,\n"
    "or per the config's own top-level `verbose` default).\n"
    "--capture-timeout/--capture-limit override, for this shim only, how long or how much\n"
    "output an invisible trial run may accumulate before shimback gives up on hiding it\n"
    "and streams it live instead (global defaults: 2000ms / 8MiB). --capture-limit accepts\n"
    "a unit suffix (B, K/KB, KiB, M/MB, MiB, G/GB, GiB, case-insensitive; no suffix = bytes).\n"
    "--split-config saves this shim's settings in their own <name>-config.toml file in the\n"
    "config directory, instead of as a [shims.<name>] entry inside config.toml -- move that\n"
    "file to the shimback binary's own directory, or the shim symlink's own directory, to\n"
    "override it from there instead. Omitting --split-config on a shim that currently has\n"
    "one removes it, folding the shim back into config.toml.\n"
    "--policy route-map generalizes route-args to any number of routes: the first --route\n"
    "whose <match> exactly equals one of the invocation's arguments runs <command> instead\n"
    "of source (with --strip-matched-args removing that one matched argument first, same as\n"
    "route-args). No match runs source, exactly like route-args' own source/fallback default.\n"
    "Each route's own extra fixed arguments aren't settable from this flag -- edit the\n"
    "resulting config.toml's `args = [...]` under that route's [[shims.<name>.routes]] block.\n"
    "Re-running add keeps those args for any route whose <match> and <command> are unchanged.\n";

#define OPT_STRIP_MATCHED_ARGS 1000
#define OPT_SOURCE_ARG 1001
#define OPT_FALLBACK_ARG 1002
#define OPT_FORCE 1003
#define OPT_SPLIT_SOURCE_ARG 1004
#define OPT_SPLIT_FALLBACK_ARG 1005
#define OPT_CAPTURE_TIMEOUT 1006
#define OPT_CAPTURE_LIMIT 1007
#define OPT_SPLIT_CONFIG 1008
#define OPT_ROUTE 1009

static void push_exit_code(int **arr, size_t *count, size_t *cap, int value) {
    if (*count == *cap) {
        *cap = *cap == 0 ? 4 : *cap * 2;
        *arr = xrealloc(*arr, *cap * sizeof(int));
    }
    (*arr)[(*count)++] = value;
}

/* Backs up `path` to `backup_path` if `path` currently exists, so a later
 * failure can restore exactly what was there before finish_add overwrites
 * it (config.toml itself, and/or a split config file being updated in
 * place). Returns true and does nothing if `path` doesn't exist yet --
 * there's nothing to preserve, and restore_from_backup() below knows to
 * treat "no backup" as "delete whatever got created" in that case. Only
 * fails when `path` exists but couldn't be copied. */
static bool backup_file_if_exists(const char *path, const char *backup_path) {
    if (access(path, F_OK) != 0) {
        return true;
    }
    return copy_file(path, backup_path);
}

/* Undoes whatever finish_add wrote to `path`, using a prior
 * backup_file_if_exists() backup: if the backup exists, moves it back over
 * `path` (restoring the previous content of an existing file that was
 * updated); if it doesn't (because `path` didn't exist before this add),
 * removes whatever was just created at `path` instead. Either way, `path`
 * ends up exactly as it was before this finish_add call started. Applies
 * equally to a brand-new shim and to one being replaced/updated -- unlike
 * the shim symlink itself (which never encodes policy/source/fallback data,
 * so the atomic temp+rename swap already protects it), config.toml and a
 * split config file *are* the data, so a failure after either has already
 * been overwritten must restore the previous version, not just abandon
 * whatever's newest. Best-effort: a failure here is reported but never
 * fatal, since the caller is already on its way to die() over the original
 * failure. */
static void restore_from_backup(const char *path, const char *backup_path) {
    if (access(backup_path, F_OK) == 0) {
        /* plat_rename_replace(), not a bare rename(): `path` already
         * exists here (that's exactly the case this restores) -- plain
         * rename()/MoveFileW don't replace an existing destination on
         * Windows, see platform.h. */
        if (!plat_rename_replace(backup_path, path)) {
            warn("add: failed to restore %s from backup: %s", path, strerror(errno));
        }
    } else if (unlink(path) != 0 && errno != ENOENT) {
        warn("add: failed to remove %s while rolling back: %s", path, strerror(errno));
    }
}

/* Discards a backup once the operation it was guarding against has fully
 * succeeded. Best-effort: a leftover backup file is harmless clutter, not a
 * correctness problem. */
static void discard_backup(const char *backup_path) {
    if (unlink(backup_path) != 0 && errno != ENOENT) {
        warn("add: failed to remove rollback backup %s: %s", backup_path, strerror(errno));
    }
}

/* Restores config.toml and (if split_target_path is non-NULL) the split
 * config file from their backups, then dies with the given message --
 * shared by every failure point after either file has been overwritten, so
 * none of them can forget to roll back before exiting. */
static void rollback_and_die(const char *cfg_path, const char *cfg_backup_path,
                              const char *split_target_path, const char *split_backup_path,
                              const char *fmt, ...) {
    restore_from_backup(cfg_path, cfg_backup_path);
    if (split_target_path) {
        restore_from_backup(split_target_path, split_backup_path);
    }
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    die("%s", msg);
}

/* Everything from policy validation through creating the symlink and
 * writing the config entry -- shared by both the "everything was already
 * given on the command line" path and the "the interactive wizard filled
 * in what was missing" path, so the two can never drift apart. */
static int finish_add(const char *name, const char *source_arg, StrVec *source_args,
                       const char *fallback_arg, StrVec *fallback_args, Policy policy,
                       StrVec *patterns, int *exit_codes, size_t exit_code_count,
                       StrVec *route_args, bool strip_matched_args, StrVec *split_source_args,
                       StrVec *split_fallback_args, StrVec *rewrite_from, StrVec *rewrite_to,
                       StrVec *route_match, StrVec *route_command, bool diagnostic, bool force,
                       bool verbose, bool capture_timeout_set, int capture_timeout_ms,
                       bool capture_limit_set, size_t capture_limit_bytes, bool split_config) {
    if (!is_valid_shim_name(name)) {
        die("add: invalid shim name '%s' -- names may only contain letters, digits, '.', '_', "
            "'+', and '-'",
            name);
    }

    char *self_exe = self_exe_path();

    char *resolved_fallback = NULL;
    if (fallback_arg) {
        resolved_fallback = resolve_binary_arg(fallback_arg);
        if (!resolved_fallback && force) {
            resolved_fallback = force_resolve_binary_arg(fallback_arg);
        }
        if (!resolved_fallback) {
            if (force) {
                die("add: --force still needs a path for fallback (containing '/'), not a bare "
                    "name -- there's nothing to resolve '%s' against if it doesn't exist "
                    "anywhere yet",
                    fallback_arg);
            }
            die("add: fallback '%s' does not exist, is not executable, or isn't on $PATH",
                fallback_arg);
        }
        if (strcmp(resolved_fallback, self_exe) == 0) {
            die("add: fallback '%s' resolves back to the shimback binary itself -- that would "
                "loop forever if this shim were ever invoked (did it resolve via $PATH to "
                "another shim, or to this one?)",
                fallback_arg);
        }
    } else if (fallback_args->count > 0) {
        /* --fallback-arg is meaningless without a fallback to attach it to
         * (only reachable at all with --policy rewrite, the one policy
         * where -f/--fallback is optional) -- warn and discard rather than
         * silently keep dead config around or hard-fail over something
         * this easy to just drop. */
        bool colorize = stderr_is_color();
        fprintf(stderr,
                "%sshimback: --fallback-arg given without a fallback -- discarding it%s\n",
                colorize ? ANSI_YELLOW : "", colorize ? ANSI_RESET : "");
        strvec_free(fallback_args);
        strvec_init(fallback_args);
    }

    char *shim_dir = shim_bin_dir();

    char *resolved_source_for_check = NULL;
    if (source_arg) {
        resolved_source_for_check = resolve_binary_arg(source_arg);
        if (!resolved_source_for_check && force) {
            resolved_source_for_check = force_resolve_binary_arg(source_arg);
        }
        if (!resolved_source_for_check) {
            if (force) {
                die("add: --force still needs a path for source (containing '/'), not a bare "
                    "name -- there's nothing to resolve '%s' against if it doesn't exist "
                    "anywhere yet",
                    source_arg);
            }
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

    /* Each route's command gets the same resolution treatment source/
     * fallback already get above -- it shouldn't be exempt from the
     * existence/executable checks everything else gets just because
     * there can be more than one of them (see review.md's own note, from
     * the split-config work, about not letting a new kind of target skip
     * checks an old one already has to pass). --route <match>=<command>
     * always pushes one match and one command together (see cmd_add and
     * the wizard's route flow), so the two lists staying the same length
     * is a construction invariant, not something callers need to enforce
     * -- checked here anyway as a cheap defense against that invariant
     * ever accidentally breaking. */
    if (route_match->count != route_command->count) {
        die("add: internal error: %zu route match(es) but %zu route command(s)",
            route_match->count, route_command->count);
    }
    size_t route_count = route_match->count;
    RouteEntry *resolved_routes = route_count > 0 ? xmalloc(route_count * sizeof(RouteEntry)) : NULL;
    for (size_t i = 0; i < route_count; i++) {
        const char *route_command_arg = route_command->items[i];
        char *resolved_route_command = resolve_binary_arg(route_command_arg);
        if (!resolved_route_command && force) {
            resolved_route_command = force_resolve_binary_arg(route_command_arg);
        }
        if (!resolved_route_command) {
            if (force) {
                die("add: --force still needs a path for --route's command (containing '/'), "
                    "not a bare name -- there's nothing to resolve '%s' against if it doesn't "
                    "exist anywhere yet",
                    route_command_arg);
            }
            die("add: --route command '%s' does not exist, is not executable, or isn't on "
                "$PATH",
                route_command_arg);
        }
        if (strcmp(resolved_route_command, self_exe) == 0) {
            die("add: --route command '%s' resolves back to the shimback binary itself -- "
                "that would loop forever if this route were ever triggered",
                route_command_arg);
        }
        resolved_routes[i].match = xstrdup(route_match->items[i]);
        resolved_routes[i].command = resolved_route_command;
        resolved_routes[i].args = NULL;
        resolved_routes[i].arg_count = 0;
    }

    /* Same binary alone isn't a no-op if source_args/fallback_args make
     * them behave differently (e.g. source "ls" -la vs. fallback "ls" -lh)
     * -- only reject when they'd be truly indistinguishable. */
    if (resolved_source_for_check && resolved_fallback &&
        strcmp(resolved_source_for_check, resolved_fallback) == 0 &&
        str_array_eq(source_args->items, source_args->count, fallback_args->items,
                     fallback_args->count)) {
        die("add: source and fallback both resolve to '%s' with the same arguments -- refusing "
            "to add a no-op shim",
            resolved_fallback);
    }

    /* Read-only pre-flight: if something already occupies where the shim
     * link would go and it isn't ours to replace, fail before touching the
     * config at all -- no point loading/saving it only to then refuse the
     * filesystem half of the change. Doesn't itself write anything yet;
     * see below for why the actual link create/replace waits until after
     * the config is safely saved.
     *
     * shim_file_name() appends ".exe" on Windows -- shims are hard links
     * there (see windows-port.md Phase 3), and cmd.exe/PowerShell only
     * resolve a bare command name against a PATHEXT-listed extension. */
    char *shim_file = shim_file_name(name);
    char *symlink_path = path_join(shim_dir, shim_file);
    free(shim_file);
    bool replacing_existing_symlink = false;
    if (access(symlink_path, F_OK) == 0) {
        /* is_shim_dir_entry() recognizes any shimback build/install
         * location as ours, not just this exact running binary's own path
         * -- an exact self_exe match here used to refuse to update a shim
         * created by an older or relocated shimback binary even though
         * it's still genuinely shimback's, and disagreed with uninstall's
         * own (marker-based) recognition of the very same link (see
         * review.md). On Windows this is also the *only* signal available
         * at all (a hard link has no distinct file type to check, unlike
         * a POSIX symlink) -- see is_shim_dir_entry's own comment. */
        if (!is_shim_dir_entry(symlink_path)) {
            die("add: %s already exists and is not a shimback-managed shim; remove it "
                "manually first",
                symlink_path);
        }
#ifndef _WIN32
        /* This ownership check and the eventual rename() that replaces
         * symlink_path (see below) aren't atomic with each other -- config
         * I/O runs in between, which takes measurable time. A directory
         * that anyone besides its owner can write into would let a
         * concurrent process swap symlink_path for something else in that
         * window, so the later rename() would silently replace whatever
         * got swapped in, not what was actually just checked (see
         * review.md). Refusing outright when the directory isn't
         * owner-only-writable is the "at minimum" bar for this: it can't
         * close the window by itself (a single-writer directory still has
         * one), but it rules out the actual precondition the race needs
         * -- another user able to write here at all. shim_dir always
         * exists at this point, since symlink_path (inside it) was just
         * found. POSIX-only: Windows' default per-user directory ACLs
         * already aren't other-writable the way a misconfigured POSIX
         * directory can be, and `st_mode`'s group/other bits aren't a
         * meaningful signal there in the first place (UCRT synthesizes
         * them from a single read-only attribute, not real ACL state) --
         * a real equivalent would need an ACL-aware check, not a `stat()`
         * bit-mask one; left as a known gap, not silently assumed safe. */
        struct stat dir_st;
        if (stat(shim_dir, &dir_st) == 0 && (dir_st.st_mode & (S_IWGRP | S_IWOTH))) {
            die("add: %s is writable by more than just its owner, which would let a "
                "concurrent process race the replacement of an existing shim symlink -- "
                "restrict its permissions first (e.g. chmod go-w %s)",
                shim_dir, shim_dir);
        }
#endif
        replacing_existing_symlink = true;
    }

    /* Ensures shim_dir exists before we try to lock it -- for a brand-new
     * shim this is the earliest point it actually needs to exist on
     * disk. Doing this now (before we even know whether this ends up
     * creating a new shim or replacing one) is safe on its own: an empty
     * directory encodes no shim state, unlike the *symlink* itself,
     * which still waits until the config is safely saved below -- so
     * this doesn't reintroduce the "dangling symlink with no config
     * entry" risk that ordering was originally protecting against. */
    if (!mkdir_p(shim_dir)) {
        die("add: failed to create shim directory %s", shim_dir);
    }

    /* Held for this whole operation -- config load through the symlink
     * create/replace and the shell PATH update -- not just the symlink
     * replace path as originally scoped. Two concurrent `add`/`remove`
     * calls could otherwise each load the same old config.toml, mutate
     * their own in-memory copy, and atomically save it, with the second
     * save silently discarding whatever the first one added or removed
     * (a lost update) -- and the same for the shell startup file's own
     * read-merge-write cycle in shell_ensure_path (see review.md). This
     * reuses the same lock the original TOCTOU fix introduced, widened
     * to serialize the whole operation against a concurrent
     * add/remove/doctor fix, not just the one narrow symlink-swap race
     * that motivated it first. Released at the very end, on the success
     * path (or early by any die()/rollback_and_die() in between, via
     * ordinary process exit). */
    int shim_lock_fd = shim_dir_lock_acquire(shim_dir);
    if (shim_lock_fd < 0) {
        die("add: failed to acquire the shim directory lock on %s: %s", shim_dir,
            strerror(errno));
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
    for (size_t i = 0; i < entry->source_arg_count; i++) {
        free(entry->source_args[i]);
    }
    free(entry->source_args);
    entry->source_args = source_args->items; /* ownership transferred */
    entry->source_arg_count = source_args->count;
    free(entry->fallback);
    entry->fallback = resolved_fallback ? xstrdup(resolved_fallback) : NULL;
    for (size_t i = 0; i < entry->fallback_arg_count; i++) {
        free(entry->fallback_args[i]);
    }
    free(entry->fallback_args);
    entry->fallback_args = fallback_args->items; /* ownership transferred */
    entry->fallback_arg_count = fallback_args->count;
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
    for (size_t i = 0; i < entry->source_route_arg_count; i++) {
        free(entry->source_route_args[i]);
    }
    free(entry->source_route_args);
    entry->source_route_args = split_source_args->items; /* ownership transferred */
    entry->source_route_arg_count = split_source_args->count;
    for (size_t i = 0; i < entry->fallback_route_arg_count; i++) {
        free(entry->fallback_route_args[i]);
    }
    free(entry->fallback_route_args);
    entry->fallback_route_args = split_fallback_args->items; /* ownership transferred */
    entry->fallback_route_arg_count = split_fallback_args->count;
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
    /* --route can't express per-route args, so a hand-edited `args = [...]`
     * on an existing route would otherwise be silently lost on every
     * re-add. Carry them over to the new route with the same match and
     * resolved command; each old route's args are claimed at most once. */
    for (size_t i = 0; i < route_count; i++) {
        for (size_t j = 0; j < entry->route_count; j++) {
            RouteEntry *old_route = &entry->routes[j];
            if (old_route->arg_count == 0 || strcmp(old_route->match, resolved_routes[i].match) != 0 ||
                strcmp(old_route->command, resolved_routes[i].command) != 0) {
                continue;
            }
            resolved_routes[i].args = old_route->args;
            resolved_routes[i].arg_count = old_route->arg_count;
            old_route->args = NULL;
            old_route->arg_count = 0;
            break;
        }
    }
    for (size_t i = 0; i < entry->route_count; i++) {
        RouteEntry *old_route = &entry->routes[i];
        free(old_route->match);
        free(old_route->command);
        for (size_t j = 0; j < old_route->arg_count; j++) {
            free(old_route->args[j]);
        }
        free(old_route->args);
    }
    free(entry->routes);
    entry->routes = resolved_routes; /* ownership transferred */
    entry->route_count = route_count;
    entry->diagnostic = diagnostic;
    entry->force = force;
    entry->capture_timeout_set = capture_timeout_set;
    entry->capture_timeout_ms = capture_timeout_ms;
    entry->capture_limit_set = capture_limit_set;
    entry->capture_limit_bytes = capture_limit_bytes;

    /* The same per-entry validation config_load/config_load_split run on
     * every entry they parse -- checked here too, before anything below
     * touches disk, so a mistake `add`'s own flag-specific checks above
     * don't happen to catch (e.g. two identical --route entries) is
     * rejected right now with a clear error, instead of being silently
     * written to config.toml and only discovered the next time something
     * else reloads it (see review.md-style reasoning: exactly this kind
     * of gap is worth closing generally, not per-policy). */
    if (validate_shim_entry(entry, errbuf, sizeof(errbuf)) != CONFIG_OK) {
        die("add: %s", errbuf);
    }

    /* --verbose only ever turns this invocation's verbosity *on*; the
     * config's own `verbose` default is what controls it when the flag
     * isn't given. */
    bool effective_verbose = verbose || cfg.verbose;

    /* --split-config: write this shim's data to its own <name>-config.toml
     * in the config directory instead of config.toml, then drop it from
     * cfg (in memory only, so far) so the config_save right below doesn't
     * also leave a shadow copy of it there. Before overwriting either file,
     * back up whatever's currently there (a no-op if it doesn't exist yet)
     * so a failure anywhere below -- including in config_save/mkdir_p/the
     * symlink step, all of which run after these writes -- can restore the
     * previous, still-valid content instead of leaving it destroyed by a
     * command that itself reported failure (see review.md). */
    char *split_target_path = NULL;
    char *split_backup_path = NULL;
    if (split_config) {
        char *cfg_dir = dir_of(cfg_path);
        char *split_filename = split_config_filename(name);
        split_target_path = path_join(cfg_dir, split_filename);
        free(cfg_dir);
        free(split_filename);

        size_t split_backup_len = strlen(split_target_path) + 32;
        split_backup_path = xmalloc(split_backup_len);
        snprintf(split_backup_path, split_backup_len, "%s.rollback.%d", split_target_path,
                 (int)getpid());
        if (!backup_file_if_exists(split_target_path, split_backup_path)) {
            die("add: failed to back up existing split config %s before updating it",
                split_target_path);
        }

        ConfigStatus split_save_st =
            config_save_split(entry, split_target_path, errbuf, sizeof(errbuf));
        if (split_save_st != CONFIG_OK) {
            restore_from_backup(split_target_path, split_backup_path);
            die("add: failed to save split config: %s", errbuf);
        }
        config_remove(&cfg, name);
    }

    char *cfg_backup_path;
    {
        size_t cfg_backup_len = strlen(cfg_path) + 32;
        cfg_backup_path = xmalloc(cfg_backup_len);
        snprintf(cfg_backup_path, cfg_backup_len, "%s.rollback.%d", cfg_path, (int)getpid());
        if (!backup_file_if_exists(cfg_path, cfg_backup_path)) {
            if (split_target_path) {
                restore_from_backup(split_target_path, split_backup_path);
            }
            die("add: failed to back up existing config %s before updating it", cfg_path);
        }
    }

    ConfigStatus save_st = config_save(&cfg, cfg_path, errbuf, sizeof(errbuf));
    if (save_st != CONFIG_OK) {
        rollback_and_die(cfg_path, cfg_backup_path, split_target_path, split_backup_path,
                          "add: failed to save config: %s", errbuf);
    }

    /* shim_dir itself was already created earlier (before the lock was
     * even acquired) -- what matters is that we only get here, to touch
     * the *symlink*, once the config is safely saved: a failure above
     * (an unreadable/invalid existing config, or a failed save) must
     * never leave a dangling symlink behind with no matching config
     * entry, which used to happen when the symlink was created first. */
    if (replacing_existing_symlink) {
        /* Build the replacement at a temp name first and atomically rename
         * it over the old one, rather than unlink-then-create:
         * plat_rename_replace() (POSIX rename(); Windows MoveFileExW with
         * MOVEFILE_REPLACE_EXISTING -- a bare rename()/MoveFileW there
         * does *not* replace an existing destination, see platform.h) is
         * atomic, so this can only ever fully succeed (new link in place)
         * or fail before ever touching the existing one (old link still
         * intact) -- never the unlink-succeeded-but-create-failed gap in
         * between that would otherwise leave the name pointing at nothing
         * at all. The link itself never encodes policy/source/fallback
         * data, so its own survival doesn't need a rollback -- but the
         * config data it now resolves to does, which is exactly what the
         * backups above and below restore. */
        size_t tmp_len = strlen(symlink_path) + 32;
        char *tmp_link = xmalloc(tmp_len);
        snprintf(tmp_link, tmp_len, "%s.tmp.%d", symlink_path, (int)getpid());
        if (!create_shim_link(self_exe, tmp_link, "add")) {
            int link_errno = errno;
            char link_err_msg[1024];
            format_link_create_error(link_err_msg, sizeof(link_err_msg), "add", tmp_link,
                                      self_exe, link_errno);
            rollback_and_die(cfg_path, cfg_backup_path, split_target_path, split_backup_path, "%s",
                              link_err_msg);
        }
        /* Revalidate right before the swap: this is as close as a plain
         * rename() gets to closing the TOCTOU window from the ownership
         * check above, shrinking it from "however long config I/O took"
         * down to the handful of syscalls between here and rename()
         * itself (see review.md). */
        if (!is_shim_dir_entry(symlink_path)) {
            unlink(tmp_link);
            rollback_and_die(cfg_path, cfg_backup_path, split_target_path, split_backup_path,
                              "add: %s changed since it was checked; refusing to replace it",
                              symlink_path);
        }
        if (!plat_rename_replace(tmp_link, symlink_path)) {
            unlink(tmp_link);
            rollback_and_die(cfg_path, cfg_backup_path, split_target_path, split_backup_path,
                              "add: failed to replace existing symlink %s: %s", symlink_path,
                              strerror(errno));
        }
    } else if (!create_shim_link(self_exe, symlink_path, "add")) {
        int link_errno = errno;
        char link_err_msg[1024];
        format_link_create_error(link_err_msg, sizeof(link_err_msg), "add", symlink_path,
                                  self_exe, link_errno);
        rollback_and_die(cfg_path, cfg_backup_path, split_target_path, split_backup_path, "%s",
                          link_err_msg);
    }

    /* Everything succeeded -- the new config is durable and the symlink is
     * live, so the backups are no longer needed. Only now, with the split
     * file (if any) safely in place, is it also safe to sweep away a stale
     * split file left over from an earlier `add --split-config` for this
     * same name when this add *isn't* split this time: doing this sweep
     * any earlier (right after config_save, as before) would permanently
     * discard that leftover file even if a later step -- mkdir_p, the
     * symlink itself -- went on to fail. Best-effort: nothing to clean up
     * is the common case, not an error. */
    discard_backup(cfg_backup_path);
    if (split_target_path) {
        discard_backup(split_backup_path);
    } else {
        remove_split_configs(name);
    }

    bool colorize = stdout_is_color();
    const char *reset = colorize ? ANSI_RESET : "";
    const char *name_color = colorize ? ANSI_BOLD ANSI_CYAN : "";
    const char *path_color = colorize ? ANSI_GREEN : "";
    const char *fallback_color = colorize ? (resolved_fallback ? ANSI_BLUE : ANSI_DIM) : "";
    const char *pc = policy_color(policy);
    const char *policy_color_str = (colorize && pc) ? pc : "";

    printf("shimback: '%s%s%s' -> %s%s%s (fallback: %s%s%s, policy: %s%s%s)\n", name_color, name,
           reset, path_color, symlink_path, reset, fallback_color,
           resolved_fallback ? resolved_fallback : "none", reset, policy_color_str,
           policy_to_string(policy), reset);
    if (split_config) {
        printf("shimback: config for '%s%s%s' saved to %s%s%s\n", name_color, name, reset,
               path_color, split_target_path, reset);
    }

    ShellKind shell = detect_current_shell();
    shell_ensure_path(shell, shim_dir, effective_verbose);
    if (effective_verbose) {
        printf("Restart your shell (or re-source its startup file) for the PATH change to take "
               "effect.\n");
    }
    shim_dir_lock_release(shim_lock_fd);

    return 0;
}

int cmd_add(int argc, char **argv) {
    const char *source_arg = NULL;
    const char *fallback_arg = NULL;
    const char *policy_arg = "exit-code";
    bool diagnostic = false;
    bool strip_matched_args = false;
    bool force = false;
    bool verbose = false;
    bool capture_timeout_set = false;
    int capture_timeout_ms = 0;
    bool capture_limit_set = false;
    size_t capture_limit_bytes = 0;
    bool split_config = false;
    StrVec source_args;
    strvec_init(&source_args);
    StrVec fallback_args;
    strvec_init(&fallback_args);
    StrVec patterns;
    strvec_init(&patterns);
    StrVec route_args;
    strvec_init(&route_args);
    StrVec split_source_args;
    strvec_init(&split_source_args);
    StrVec split_fallback_args;
    strvec_init(&split_fallback_args);
    StrVec rewrite_from;
    strvec_init(&rewrite_from);
    StrVec rewrite_to;
    strvec_init(&rewrite_to);
    StrVec route_match;
    strvec_init(&route_match);
    StrVec route_command;
    strvec_init(&route_command);
    int *exit_codes = NULL;
    size_t exit_code_count = 0;
    size_t exit_code_cap = 0;

    static struct option long_opts[] = {
        {"source", required_argument, 0, 's'},
        {"source-arg", required_argument, 0, OPT_SOURCE_ARG},
        {"fallback", required_argument, 0, 'f'},
        {"fallback-arg", required_argument, 0, OPT_FALLBACK_ARG},
        {"policy", required_argument, 0, 'p'},
        {"error-pattern", required_argument, 0, 'e'},
        {"exit-code", required_argument, 0, 'x'},
        {"route-arg", required_argument, 0, 'r'},
        {"strip-matched-args", no_argument, 0, OPT_STRIP_MATCHED_ARGS},
        {"split-source-arg", required_argument, 0, OPT_SPLIT_SOURCE_ARG},
        {"split-fallback-arg", required_argument, 0, OPT_SPLIT_FALLBACK_ARG},
        {"rewrite", required_argument, 0, 'w'},
        {"route", required_argument, 0, OPT_ROUTE},
        {"diagnostic", no_argument, 0, 'd'},
        {"force", no_argument, 0, OPT_FORCE},
        {"verbose", no_argument, 0, 'v'},
        {"capture-timeout", required_argument, 0, OPT_CAPTURE_TIMEOUT},
        {"capture-limit", required_argument, 0, OPT_CAPTURE_LIMIT},
        {"split-config", no_argument, 0, OPT_SPLIT_CONFIG},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:f:p:e:x:r:w:dv", long_opts, NULL)) != -1) {
        switch (opt) {
            case 's': source_arg = optarg; break;
            case OPT_SOURCE_ARG: strvec_push(&source_args, xstrdup(optarg)); break;
            case 'f': fallback_arg = optarg; break;
            case OPT_FALLBACK_ARG: strvec_push(&fallback_args, xstrdup(optarg)); break;
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
            case OPT_SPLIT_SOURCE_ARG: strvec_push(&split_source_args, xstrdup(optarg)); break;
            case OPT_SPLIT_FALLBACK_ARG:
                strvec_push(&split_fallback_args, xstrdup(optarg));
                break;
            case 'w': {
                const char *eq = strchr(optarg, '=');
                if (!eq || eq == optarg) {
                    die("add: --rewrite must be '<from>=<to>' (got '%s')", optarg);
                }
                strvec_push(&rewrite_from, xstrndup(optarg, (size_t)(eq - optarg)));
                strvec_push(&rewrite_to, xstrdup(eq + 1));
                break;
            }
            case OPT_ROUTE: {
                const char *eq = strchr(optarg, '=');
                if (!eq || eq == optarg || eq[1] == '\0') {
                    die("add: --route must be '<match>=<command>' (got '%s')", optarg);
                }
                strvec_push(&route_match, xstrndup(optarg, (size_t)(eq - optarg)));
                strvec_push(&route_command, xstrdup(eq + 1));
                break;
            }
            case 'd': diagnostic = true; break;
            case OPT_FORCE: force = true; break;
            case 'v': verbose = true; break;
            case OPT_CAPTURE_TIMEOUT: {
                errno = 0;
                char *end;
                long v = strtol(optarg, &end, 10);
                if (*optarg == '\0' || *end != '\0' || errno == ERANGE || v < 0 ||
                    v > INT_MAX) {
                    die("add: --capture-timeout must be a non-negative integer of milliseconds "
                        "(got '%s')",
                        optarg);
                }
                capture_timeout_set = true;
                capture_timeout_ms = (int)v;
                break;
            }
            case OPT_CAPTURE_LIMIT: {
                if (!parse_size_bytes(optarg, &capture_limit_bytes)) {
                    die("add: --capture-limit must be a size like \"8MiB\" or a plain byte count "
                        "(got '%s')",
                        optarg);
                }
                capture_limit_set = true;
                break;
            }
            case OPT_SPLIT_CONFIG: split_config = true; break;
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
    if (name && !is_valid_shim_name(name)) {
        die("add: invalid shim name '%s' -- names may only contain letters, digits, '.', '_', "
            "'+', and '-'",
            name);
    }

    Policy policy;
    if (!policy_from_string(policy_arg, &policy)) {
        die("add: --policy must be \"exit-code\", \"heuristic\", \"exit-code-match\", "
            "\"route-args\", \"rewrite\", \"split-args\", or \"route-map\"");
    }

    bool missing_name = (name == NULL);
    bool missing_fallback =
        (!fallback_arg && policy != POLICY_REWRITE && policy != POLICY_ROUTE_MAP);
    bool missing_patterns = (policy == POLICY_HEURISTIC && patterns.count == 0);
    bool missing_exit_codes = (policy == POLICY_EXIT_CODE_MATCH && exit_code_count == 0);
    bool missing_route_args = (policy == POLICY_ROUTE_ARGS && route_args.count == 0);
    bool missing_split_args = (policy == POLICY_SPLIT_ARGS &&
                                (split_source_args.count == 0 || split_fallback_args.count == 0));
    bool missing_rewrite = (policy == POLICY_REWRITE && rewrite_from.count == 0);
    bool missing_routes = (policy == POLICY_ROUTE_MAP && route_match.count == 0);
    bool something_missing = missing_name || missing_fallback || missing_patterns ||
                              missing_exit_codes || missing_route_args || missing_split_args ||
                              missing_rewrite || missing_routes;

    if (something_missing && !tui_supported()) {
        if (missing_name) {
            fprintf(stderr, "%s", USAGE);
            die("add: missing shim name");
        }
        if (missing_fallback) {
            fprintf(stderr, "%s", USAGE);
            die("add: -f/--fallback is required (except with --policy rewrite or route-map)");
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
        if (missing_split_args) {
            die("add: --policy split-args requires at least one --split-source-arg and one "
                "--split-fallback-arg");
        }
        if (missing_routes) {
            die("add: --policy route-map requires at least one --route <match>=<command>");
        }
        die("add: --policy rewrite requires at least one --rewrite <from>=<to>");
    }

    if (something_missing) {
        WizardSeed seed = {
            .name = name,
            .source_arg = source_arg,
            .source_args = &source_args,
            .fallback_arg = fallback_arg,
            .fallback_args = &fallback_args,
            .policy = policy,
            .patterns = &patterns,
            .exit_codes = exit_codes,
            .exit_code_count = exit_code_count,
            .route_args = &route_args,
            .strip_matched_args = strip_matched_args,
            .split_source_args = &split_source_args,
            .split_fallback_args = &split_fallback_args,
            .rewrite_from = &rewrite_from,
            .rewrite_to = &rewrite_to,
            .route_match = &route_match,
            .route_command = &route_command,
            .diagnostic = diagnostic,
            .split_config = split_config,
        };
        WizardResult result;
        if (!run_add_wizard(&seed, &result)) {
            printf("Aborted -- no shim was created.\n");
            return 1;
        }
        return finish_add(result.name, result.source_arg, &result.source_args,
                           result.fallback_arg, &result.fallback_args, result.policy,
                           &result.patterns, result.exit_codes, result.exit_code_count,
                           &result.route_args, result.strip_matched_args,
                           &result.split_source_args, &result.split_fallback_args,
                           &result.rewrite_from, &result.rewrite_to, &result.route_match,
                           &result.route_command, result.diagnostic, force, verbose,
                           capture_timeout_set, capture_timeout_ms, capture_limit_set,
                           capture_limit_bytes, result.split_config);
    }

    return finish_add(name, source_arg, &source_args, fallback_arg, &fallback_args, policy,
                       &patterns, exit_codes, exit_code_count, &route_args, strip_matched_args,
                       &split_source_args, &split_fallback_args, &rewrite_from, &rewrite_to,
                       &route_match, &route_command, diagnostic, force, verbose,
                       capture_timeout_set, capture_timeout_ms, capture_limit_set,
                       capture_limit_bytes, split_config);
}
