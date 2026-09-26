/* Note: this is a short-lived CLI command handler. Heap allocations here are
 * intentionally not freed before process exit -- the OS reclaims them, and
 * this is a standard, deliberate simplification for one-shot CLI tools. */
#include "commands.h"

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
#include "../suggest.h"
#include "../util.h"

/* Inline color markup: these bytes never appear in real output, so a format
 * string can be written as plain string-literal concatenation, e.g.
 *   pf(M_GREEN "%s" M_RESET "\n", path);
 * pf() swaps each marker for its ANSI escape (or drops it when color is off).
 * Only the *format string* is scanned, never the arguments, so a path or
 * argument that happens to contain one of these bytes can't inject color. */
#define M_RESET "\001"
#define M_DIM "\002"
#define M_GREEN "\003"
#define M_BLUE "\004"
#define M_YELLOW "\005"
#define M_CYAN "\006" /* bold cyan: shim names */
#define M_RED "\016"
#define M_MAGENTA "\017"
#define M_BOLD "\020"
#define M_HDR "\021"    /* bold yellow: section headers */
#define M_POLICY "\022" /* this shim's policy color, same as `list` */

#define TAG_OK "[" M_BOLD M_GREEN "ok" M_RESET "]"
#define TAG_WARN "[" M_BOLD M_YELLOW "warn" M_RESET "]"
#define TAG_FAIL "[" M_BOLD M_RED "fail" M_RESET "]"

#define LABEL_W 14

static const char *USAGE = "usage: shimback info <name>\n";

static bool g_colorize = false;
static const char *g_policy_color = "";
static int g_issues = 0;

static const char *markup_escape(char c) {
    switch (c) {
        case '\001': return ANSI_RESET;
        case '\002': return ANSI_DIM;
        case '\003': return ANSI_GREEN;
        case '\004': return ANSI_BLUE;
        case '\005': return ANSI_YELLOW;
        case '\006': return ANSI_BOLD ANSI_CYAN;
        case '\016': return ANSI_RED;
        case '\017': return ANSI_MAGENTA;
        case '\020': return ANSI_BOLD;
        case '\021': return ANSI_BOLD ANSI_YELLOW;
        case '\022': return g_policy_color;
        default: return NULL;
    }
}

static void pf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void pf(const char *fmt, ...) {
    DynBuf f;
    dynbuf_init(&f);
    for (const char *p = fmt; *p; p++) {
        const char *esc = markup_escape(*p);
        if (esc) {
            if (g_colorize) {
                dynbuf_append_str(&f, esc);
            }
        } else {
            dynbuf_append_char(&f, *p);
        }
    }
    va_list ap;
    va_start(ap, fmt);
    vprintf(dynbuf_cstr(&f), ap);
    va_end(ap);
    dynbuf_free(&f);
}

static void section(const char *title) {
    pf("\n" M_HDR "%s" M_RESET "\n", title);
}

static void label(const char *text) {
    pf("  " M_DIM "%-*s" M_RESET " ", LABEL_W, text);
}

/* Continuation lines of a multi-line value line up under the value column. */
static void indent(void) {
    pf("  %-*s ", LABEL_W, "");
}

static void issue(void) {
    g_issues++;
}

/* Quotes an argument the way a shell would need it, so args containing
 * spaces or metacharacters stay unambiguous when printed as a command line. */
static char *shell_word(const char *s) {
    if (*s != '\0' && strpbrk(s, " \t\n'\"\\$`*?<>|&;()[]{}!#~") == NULL) {
        return xstrdup(s);
    }
    DynBuf b;
    dynbuf_init(&b);
    dynbuf_append_char(&b, '\'');
    for (const char *p = s; *p; p++) {
        if (*p == '\'') {
            dynbuf_append_str(&b, "'\\''");
        } else {
            dynbuf_append_char(&b, *p);
        }
    }
    dynbuf_append_char(&b, '\'');
    char *out = xstrdup(dynbuf_cstr(&b));
    dynbuf_free(&b);
    return out;
}

static char *join_words(char *const *items, size_t count, const char *sep) {
    DynBuf b;
    dynbuf_init(&b);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            dynbuf_append_str(&b, sep);
        }
        char *w = shell_word(items[i]);
        dynbuf_append_str(&b, w);
        free(w);
    }
    char *out = xstrdup(dynbuf_cstr(&b));
    dynbuf_free(&b);
    return out;
}

/* Like join_words, but every item wrapped in double quotes (for patterns). */
static char *join_quoted(char *const *items, size_t count, const char *sep) {
    DynBuf b;
    dynbuf_init(&b);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            dynbuf_append_str(&b, sep);
        }
        dynbuf_append_char(&b, '"');
        dynbuf_append_str(&b, items[i]);
        dynbuf_append_char(&b, '"');
    }
    char *out = xstrdup(dynbuf_cstr(&b));
    dynbuf_free(&b);
    return out;
}

static char *human_bytes(size_t bytes) {
    char buf[64];
    if (bytes != 0 && bytes % (1024UL * 1024 * 1024) == 0) {
        snprintf(buf, sizeof(buf), "%zu GiB", bytes / (1024UL * 1024 * 1024));
    } else if (bytes != 0 && bytes % (1024UL * 1024) == 0) {
        snprintf(buf, sizeof(buf), "%zu MiB", bytes / (1024UL * 1024));
    } else if (bytes != 0 && bytes % 1024UL == 0) {
        snprintf(buf, sizeof(buf), "%zu KiB", bytes / 1024UL);
    } else {
        snprintf(buf, sizeof(buf), "%zu bytes", bytes);
    }
    return xstrdup(buf);
}

/* ---- command health ---------------------------------------------------- */

typedef enum {
    CMD_OK,
    CMD_FORCED_MISSING, /* doesn't exist yet, but the shim was added with --force */
    CMD_MISSING,
    CMD_LOOP, /* resolves back to the shimback binary itself */
} CmdState;

static CmdState cmd_state(const char *path, const char *self_exe, bool force) {
    if (!is_executable_file(path)) {
        return force ? CMD_FORCED_MISSING : CMD_MISSING;
    }
    char *canon = canonicalize(path);
    bool loop = canon && strcmp(canon, self_exe) == 0;
    free(canon);
    return loop ? CMD_LOOP : CMD_OK;
}

static void print_cmd_tag(CmdState st) {
    switch (st) {
        case CMD_OK: pf(" " TAG_OK); break;
        case CMD_FORCED_MISSING:
            pf(" " TAG_WARN " " M_DIM "doesn't exist yet (added with --force)" M_RESET);
            break;
        case CMD_MISSING:
            pf(" " TAG_FAIL " " M_RED "missing or not executable" M_RESET);
            issue();
            break;
        case CMD_LOOP:
            pf(" " TAG_FAIL " " M_RED "resolves back to shimback itself (a loop)" M_RESET);
            issue();
            break;
    }
}

/* A command as it appears inside the flow diagram: path, its fixed args,
 * then a dim placeholder for whatever the shim was invoked with. */
static void print_cmdline(const char *path, char *const *args, size_t arg_count,
                          const char *tail, CmdState st) {
    if (path) {
        pf("%s", path);
    } else {
        pf(M_RED "(not found on $PATH)" M_RESET);
    }
    if (arg_count > 0) {
        char *joined = join_words(args, arg_count, " ");
        pf(" %s", joined);
        free(joined);
    }
    if (tail) {
        pf(" " M_DIM "%s" M_RESET, tail);
    }
    if (!path || st == CMD_MISSING || st == CMD_LOOP) {
        pf(" " M_RED "[!]" M_RESET);
    }
}

/* ---- policy descriptions ------------------------------------------------ */

static const char *policy_summary(Policy p) {
    switch (p) {
        case POLICY_EXIT_CODE: return "Runs the source; falls back if it exits non-zero.";
        case POLICY_HEURISTIC:
            return "Runs the source; falls back only if it fails AND its stderr matches a "
                   "configured pattern.";
        case POLICY_EXIT_CODE_MATCH:
            return "Runs the source; falls back only if it exits with one of the configured "
                   "codes.";
        case POLICY_ROUTE_ARGS:
            return "Picks the source or the fallback up front, from the invocation's arguments.";
        case POLICY_SPLIT_ARGS:
            return "Picks the source or the fallback up front, from a separate set of "
                   "arguments for each.";
        case POLICY_REWRITE:
            return "Rewrites matching arguments, then runs the source (no fallback).";
        case POLICY_ROUTE_MAP:
            return "Routes to any number of commands, based on the invocation's arguments.";
        case POLICY_PASSTHROUGH:
            return "Runs the source with its output visible the whole time (no hidden trial "
                   "run); falls back if it exits non-zero.";
    }
    return "";
}

static bool policy_uses_fallback(Policy p) {
    return p != POLICY_REWRITE && p != POLICY_ROUTE_MAP;
}

/* Policies that ever run the source as a hidden, captured trial run. */
static bool policy_uses_trial_run(Policy p) {
    return p == POLICY_EXIT_CODE || p == POLICY_HEURISTIC || p == POLICY_EXIT_CODE_MATCH ||
           p == POLICY_SPLIT_ARGS;
}

/* ---- symlink / PATH ------------------------------------------------------ */

static bool same_dir(const char *a, const char *b) {
    if (strcmp(a, b) == 0) {
        return true;
    }
    char *ca = canonicalize(a);
    char *cb = canonicalize(b);
    bool same = ca && cb && strcmp(ca, cb) == 0;
    free(ca);
    free(cb);
    return same;
}

static void print_symlink(const char *link_path, const char *self_exe) {
    label("symlink");
    pf(M_GREEN "%s" M_RESET, link_path);

#ifdef _WIN32
    /* A Windows shim is a hard link, not a symlink -- there's no separate
     * "raw unresolved target" to read at all (unlike a symlink, a hard
     * link just *is* the same file, so "dead"/"missing" as concepts don't
     * carry over the same way either -- see windows-port.md Phase 3).
     * looks_like_shimback_binary() (which already checks
     * is_executable_file internally) is both the existence and validity
     * check in one, mirroring check_symlink's identical Windows branch in
     * doctor.c. */
    if (access(link_path, F_OK) != 0) {
        pf("  " TAG_FAIL " " M_RED "missing" M_RESET " " M_DIM
           "-- `shimback doctor fix` recreates it" M_RESET "\n");
        issue();
        return;
    }
    if (!looks_like_shimback_binary(link_path)) {
        pf("  " TAG_FAIL " " M_RED "exists but doesn't look like a shimback binary" M_RESET "\n");
        issue();
        return;
    }
    /* Unlike the POSIX branch, this doesn't distinguish "this exact
     * binary" from "a different shimback binary" -- doing that properly
     * needs a file-identity comparison (same volume + file index), not a
     * path-string one (canonicalize() on a hard link can legitimately
     * return any one of its several linked names, not necessarily
     * self_exe's own), and that's more machinery than this cosmetic
     * distinction is worth right now. */
    pf("  " TAG_OK "\n");
    label("");
    pf(M_DIM "(hard link to a shimback binary)" M_RESET "\n");
#else
    struct stat lst;
    if (lstat(link_path, &lst) != 0) {
        pf("  " TAG_FAIL " " M_RED "missing" M_RESET " " M_DIM
           "-- `shimback doctor fix` recreates it" M_RESET "\n");
        issue();
        return;
    }
    if (!S_ISLNK(lst.st_mode)) {
        pf("  " TAG_FAIL " " M_RED "exists but is not a symlink" M_RESET "\n");
        issue();
        return;
    }
    char *target = canonicalize(link_path);
    if (!target || !is_executable_file(target)) {
        char raw[PATH_MAX];
        ssize_t n = readlink(link_path, raw, sizeof(raw) - 1);
        raw[n > 0 ? n : 0] = '\0';
        pf("  " TAG_FAIL " " M_RED "dead" M_RESET " " M_DIM "-> %s (target is gone) -- "
           "`shimback doctor fix` recreates it" M_RESET "\n",
           raw);
        issue();
        free(target);
        return;
    }
    if (strcmp(target, self_exe) == 0) {
        pf("  " TAG_OK "\n");
        label("");
        pf(M_DIM "-> " M_RESET "%s" M_DIM "  (shimback binary)" M_RESET "\n", target);
    } else if (looks_like_shimback_binary(target)) {
        pf("  " TAG_OK "\n");
        label("");
        pf(M_DIM "-> " M_RESET "%s" M_DIM "  (a different shimback binary than this one)" M_RESET
                  "\n",
           target);
    } else {
        pf("  " TAG_FAIL " " M_RED "points at %s, which isn't shimback" M_RESET "\n", target);
        issue();
    }
    free(target);
#endif
}

/* The first executable `name` on $PATH, exactly as the shell would find it
 * -- NOT canonicalized, unlike path_search(): a shim is a symlink to the
 * shimback binary, so resolving symlinks here would make every shim look
 * like "the shimback binary" instead of like the shim. */
static char *first_on_path(const char *name) {
    const char *path_env = getenv("PATH");
    if (!path_env) {
        return NULL;
    }
    char *copy = xstrdup(path_env);
    char *result = NULL;
    char *save = NULL;
    for (char *dir = strtok_r(copy, PLAT_PATH_LIST_SEP, &save); dir && !result;
         dir = strtok_r(NULL, PLAT_PATH_LIST_SEP, &save)) {
        char *candidate = path_join(dir, name);
#ifdef _WIN32
        /* Same .exe-retry path_search() itself needs (see its own comment)
         * -- name is always the bare shim name here, and without this a
         * stat() on the extension-less candidate never matches the real
         * "<name>.exe" file, so every shim would always look unreachable
         * from $PATH on Windows regardless of its real PATH setup. */
        size_t name_len = strlen(name);
        bool already_has_exe = name_len > 4 && _stricmp(name + name_len - 4, ".exe") == 0;
        if (!is_executable_file(candidate) && !already_has_exe) {
            char *name_exe = shim_file_name(name);
            free(candidate);
            candidate = path_join(dir, name_exe);
            free(name_exe);
        }
#endif
        if (is_executable_file(candidate)) {
            result = candidate;
        } else {
            free(candidate);
        }
    }
    free(copy);
    return result;
}

static void print_path_resolution(const char *name, const char *shim_dir) {
    label("on $PATH");
    char *first = first_on_path(name);
    if (!first) {
        pf(TAG_WARN " " M_YELLOW "no '%s' is reachable from $PATH in this shell" M_RESET " "
           M_DIM "-- is the shim directory on $PATH?" M_RESET "\n",
           name);
        issue();
        return;
    }
    char *first_dir = dir_of(first);
    if (same_dir(first_dir, shim_dir)) {
        pf(TAG_OK " typing '" M_CYAN "%s" M_RESET "' runs this shim (first match on $PATH)\n",
           name);
    } else {
        pf(TAG_WARN " " M_YELLOW "typing '%s' currently runs %s instead -- the shim is "
           "bypassed" M_RESET "\n",
           name, first);
        issue();
    }
    free(first_dir);
    free(first);
}

/* ---- sections -------------------------------------------------------------- */

static void print_yes_no(bool yes) {
    if (yes) {
        pf(M_GREEN "yes" M_RESET "\n");
    } else {
        pf(M_DIM "no" M_RESET "\n");
    }
}

/* A rewrite rule's <to>; an empty one just drops the matched argument. */
static void print_rewrite_to(const char *to) {
    if (to[0]) {
        pf("%s\n", to);
    } else {
        pf(M_DIM "(dropped)" M_RESET "\n");
    }
}

static void print_arg_list_row(const char *lbl, char *const *args, size_t count) {
    label(lbl);
    if (count == 0) {
        pf(M_DIM "(none)" M_RESET "\n");
        return;
    }
    char *joined = join_words(args, count, " ");
    pf("%s\n", joined);
    free(joined);
}

typedef struct {
    const ShimEntry *e;
    const char *name;
    const char *self_exe;
    char *source_resolved; /* what will actually run as source, or NULL if unresolved */
    CmdState source_state;
    CmdState fallback_state;
    int timeout_ms;      /* effective trial-run timeout (shim override, else global) */
    size_t limit_bytes;  /* effective trial-run output limit */
} View;

static void print_overview(const View *v) {
    const ShimEntry *e = v->e;
    section("Overview");
    label("policy");
    pf(M_POLICY "%s" M_RESET "\n", policy_to_string(e->policy));
    indent();
    pf(M_DIM "%s" M_RESET "\n", policy_summary(e->policy));
    label("diagnostic");
    if (e->diagnostic) {
        pf(M_GREEN "on" M_RESET " " M_DIM "(prints a note to stderr whenever a fallback or "
           "route is taken)" M_RESET "\n");
    } else if (e->policy == POLICY_PASSTHROUGH) {
        pf(M_DIM "off (not applicable -- this policy never hides anything to report on)"
           M_RESET "\n");
    } else {
        pf(M_DIM "off" M_RESET "\n");
    }
    label("force");
    if (e->force) {
        pf(M_GREEN "yes" M_RESET " " M_DIM "(source/fallback paths may not exist yet; `doctor` "
           "tolerates that)" M_RESET "\n");
    } else {
        pf(M_DIM "no" M_RESET "\n");
    }
}

static void print_locations(const View *v, ShimSource src, const char *split_path,
                            bool shadowed_entry, const char *cfg_path, const char *shim_dir) {
    section("Locations");
    char *shim_file = shim_file_name(v->name);
    char *link_path = path_join(shim_dir, shim_file);
    free(shim_file);
    print_symlink(link_path, v->self_exe);
    print_path_resolution(v->name, shim_dir);

    label("config");
    if (src == SHIM_SOURCE_SPLIT) {
        static const char *const WHERE[3] = {
            "next to the shim's symlink",
            "next to the shimback binary",
            "in the shimback config directory",
        };
        char *paths[3];
        split_config_all_paths(v->name, paths);
        const char *where = "";
        for (int i = 0; i < 3; i++) {
            if (strcmp(paths[i], split_path) == 0) {
                where = WHERE[i];
            }
        }
        pf(M_GREEN "%s" M_RESET "\n", split_path);
        indent();
        pf(M_DIM "a split config file, %s" M_RESET "\n", where);
        if (shadowed_entry) {
            indent();
            pf(TAG_WARN " " M_YELLOW "config.toml also has a [shims.%s] entry, but it's "
               "ignored -- the split file wins" M_RESET "\n",
               v->name);
            issue();
        }
    } else if (src == SHIM_SOURCE_CONFIG) {
        pf(M_GREEN "%s" M_RESET "\n", cfg_path);
        indent();
        pf(M_DIM "the [shims.%s] entry in config.toml" M_RESET "\n", v->name);
    } else {
        pf(TAG_FAIL " " M_RED "none" M_RESET " " M_DIM "-- no config.toml entry or split "
           "config file was found for this symlink" M_RESET "\n");
        issue();
    }
    free(link_path);
}

static void print_commands(const View *v) {
    const ShimEntry *e = v->e;
    section("Commands");

    label("source");
    if (e->source) {
        pf(M_GREEN "%s" M_RESET " " M_DIM "(explicit, frozen at add time)" M_RESET, e->source);
        print_cmd_tag(v->source_state);
        pf("\n");
    } else if (v->source_resolved) {
        pf(M_DIM "auto" M_RESET " -> " M_GREEN "%s" M_RESET, v->source_resolved);
        print_cmd_tag(v->source_state);
        pf("\n");
        indent();
        pf(M_DIM "resolved on every run: the first '%s' on $PATH outside the shim directory" M_RESET
                 "\n",
           v->name);
    } else {
        pf(M_DIM "auto" M_RESET " " TAG_FAIL " " M_RED "no '%s' found on $PATH to use as the "
           "source" M_RESET "\n",
           v->name);
        issue();
    }
    print_arg_list_row("source args", e->source_args, e->source_arg_count);

    label("fallback");
    if (e->fallback) {
        pf(M_BLUE "%s" M_RESET, e->fallback);
        print_cmd_tag(v->fallback_state);
        if (!policy_uses_fallback(e->policy)) {
            pf(" " M_DIM "(unused by this policy)" M_RESET);
        }
        pf("\n");
        print_arg_list_row("fallback args", e->fallback_args, e->fallback_arg_count);
    } else {
        pf(M_DIM "none" M_RESET);
        if (!policy_uses_fallback(e->policy)) {
            pf(" " M_DIM "(not used by this policy)" M_RESET);
        }
        pf("\n");
    }
}

static void print_policy_settings(const View *v) {
    const ShimEntry *e = v->e;
    section("Policy settings");

    switch (e->policy) {
        case POLICY_EXIT_CODE:
            label("falls back on");
            pf("any non-zero exit from the source\n");
            break;
        case POLICY_HEURISTIC:
            label("error patterns");
            pf(M_DIM "(case-insensitive substrings of the source's stderr)" M_RESET "\n");
            for (size_t i = 0; i < e->error_pattern_count; i++) {
                indent();
                pf(M_YELLOW "\"%s\"" M_RESET "\n", e->error_patterns[i]);
            }
            break;
        case POLICY_EXIT_CODE_MATCH: {
            label("exit codes");
            for (size_t i = 0; i < e->exit_code_count; i++) {
                pf("%s" M_YELLOW "%d" M_RESET, i > 0 ? ", " : "", e->exit_codes[i]);
            }
            pf("\n");
            break;
        }
        case POLICY_ROUTE_ARGS:
            print_arg_list_row("route args", e->route_args, e->route_arg_count);
            label("strip matched");
            print_yes_no(e->strip_matched_args);
            break;
        case POLICY_SPLIT_ARGS:
            print_arg_list_row("source match", e->source_route_args, e->source_route_arg_count);
            print_arg_list_row("fallback match", e->fallback_route_args,
                               e->fallback_route_arg_count);
            indent();
            pf(M_DIM "a side wins only if ALL of its own arguments are present" M_RESET "\n");
            label("strip matched");
            print_yes_no(e->strip_matched_args);
            break;
        case POLICY_REWRITE:
            label("rewrite rules");
            pf(M_DIM "(each argument that equals <from> is replaced with <to>)" M_RESET "\n");
            for (size_t i = 0; i < e->rewrite_from_count; i++) {
                indent();
                pf(M_YELLOW "%s" M_RESET " -> ", e->rewrite_from[i]);
                print_rewrite_to(e->rewrite_to[i]);
            }
            break;
        case POLICY_ROUTE_MAP: {
            label("routes");
            pf(M_DIM "(checked in order; the first match wins)" M_RESET "\n");
            size_t mw = 0;
            for (size_t i = 0; i < e->route_count; i++) {
                size_t l = strlen(e->routes[i].match);
                if (l > mw) {
                    mw = l;
                }
            }
            for (size_t i = 0; i < e->route_count; i++) {
                const RouteEntry *r = &e->routes[i];
                CmdState st = cmd_state(r->command, v->self_exe, e->force);
                indent();
                pf(M_DIM "%zu." M_RESET " " M_YELLOW "%-*s" M_RESET " -> " M_BLUE "%s" M_RESET,
                   i + 1, (int)mw, r->match, r->command);
                print_cmd_tag(st);
                pf("\n");
                if (r->arg_count > 0) {
                    char *joined = join_words(r->args, r->arg_count, " ");
                    indent();
                    pf("   %-*s   " M_DIM "with args:" M_RESET " %s\n", (int)mw, "", joined);
                    free(joined);
                }
            }
            label("strip matched");
            print_yes_no(e->strip_matched_args);
            break;
        }
        case POLICY_PASSTHROUGH:
            label("falls back on");
            pf("any non-zero exit from the source " M_DIM "(same as exit-code, but source and "
               "fallback both run with live output instead of a hidden trial run)" M_RESET "\n");
            break;
    }
}

static void print_trial_run(const View *v, const Config *cfg) {
    const ShimEntry *e = v->e;
    section("Trial run");
    if (!policy_uses_trial_run(e->policy)) {
        label("captured");
        pf("no " M_DIM "-- this policy runs its target directly, with live output (the limits "
           "below don't apply)" M_RESET "\n");
        return;
    }
    if (e->policy == POLICY_SPLIT_ARGS) {
        label("used when");
        pf("neither side's arguments match " M_DIM "(otherwise the chosen side runs directly)"
           M_RESET "\n");
    }

    int timeout = e->capture_timeout_set ? e->capture_timeout_ms : cfg->capture_timeout_ms;
    const char *timeout_origin =
        e->capture_timeout_set
            ? "this shim's override"
            : (cfg->capture_timeout_ms != SHIMBACK_DEFAULT_CAPTURE_TIMEOUT_MS ? "global config"
                                                                                : "default");
    size_t limit = e->capture_limit_set ? e->capture_limit_bytes : cfg->capture_limit_bytes;
    const char *limit_origin =
        e->capture_limit_set
            ? "this shim's override"
            : (cfg->capture_limit_bytes != SHIMBACK_DEFAULT_CAPTURE_LIMIT_BYTES ? "global config"
                                                                                  : "default");
    char *limit_h = human_bytes(limit);

    label("timeout");
    pf("%d ms " M_DIM "(%s)" M_RESET "\n", timeout, timeout_origin);
    label("output limit");
    pf("%s " M_DIM "(%s)" M_RESET "\n", limit_h, limit_origin);
    indent();
    pf(M_DIM "past either one the source's output is streamed live instead, and it can no "
       "longer fall back" M_RESET "\n");
    free(limit_h);
}

/* ---- the flow diagram -------------------------------------------------------- */

#define RAIL "    " M_DIM "|" M_RESET

/* One branch of a decision: `+-- <cond> --> ` (or `` `-- `` for the last one),
 * with every condition padded to `width` so the arrows line up. The action
 * text is printed by the caller right after. `branch_cont` indents a second
 * action line to the same column (keeping the rail going unless last). */
static void branch(bool last, size_t width, const char *cond) {
    pf("    " M_DIM "%s--" M_RESET " " M_YELLOW "%-*s" M_RESET " " M_DIM "-->" M_RESET " ",
       last ? "`" : "+", (int)width, cond);
}

static void branch_cont(bool last, size_t width) {
    pf("    " M_DIM "%s" M_RESET "%*s", last ? " " : "|", (int)(8 + width), "");
}

static size_t max_len(const char *const *strs, size_t n) {
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        size_t l = strlen(strs[i]);
        if (l > w) {
            w = l;
        }
    }
    return w;
}

static void flow_head(const View *v, const char *shim_dir, const char *policy) {
    char *shim_file = shim_file_name(v->name);
    char *link_path = path_join(shim_dir, shim_file);
    free(shim_file);
    pf("  " M_DIM "$" M_RESET " " M_CYAN "%s" M_RESET " " M_DIM "<args>" M_RESET "\n", v->name);
    pf(RAIL "\n");
#ifdef _WIN32
    pf(RAIL "  " M_DIM "the shell finds the shim's hard link first on $PATH" M_RESET "\n");
    pf("    " M_DIM "v" M_RESET "\n");
    pf("  " M_GREEN "%s" M_RESET " " M_DIM "(hard link)" M_RESET "\n", link_path);
#else
    pf(RAIL "  " M_DIM "the shell finds the shim's symlink first on $PATH" M_RESET "\n");
    pf("    " M_DIM "v" M_RESET "\n");
    pf("  " M_GREEN "%s" M_RESET " " M_DIM "(symlink)" M_RESET "\n", link_path);
#endif
    pf(RAIL "\n");
    pf(RAIL "  " M_DIM "which is just shimback, started under the name '%s'" M_RESET "\n",
       v->name);
    pf("    " M_DIM "v" M_RESET "\n");
    pf("  " M_BOLD "shimback" M_RESET "  " M_DIM "policy:" M_RESET " " M_POLICY "%s" M_RESET "\n",
       policy);
    free(link_path);
}

/* `run SOURCE   <cmd> <args>` as a boxed step, used by most policies. */
static void run_source_line(const View *v, const char *src_path, const char *tail) {
    pf("  " M_GREEN "run SOURCE" M_RESET "   ");
    print_cmdline(src_path, v->e->source_args, v->e->source_arg_count, tail, v->source_state);
    pf("\n");
}

static void run_fallback_text(const View *v, const char *tail) {
    pf(M_BLUE "run FALLBACK" M_RESET " ");
    print_cmdline(v->e->fallback, v->e->fallback_args, v->e->fallback_arg_count, tail,
                  v->fallback_state);
    pf("\n");
}

static void print_flow(const View *v, const char *shim_dir) {
    const ShimEntry *e = v->e;
    section("Flow");
    flow_head(v, shim_dir, policy_to_string(e->policy));
    pf(RAIL "\n");
    pf("    " M_DIM "v" M_RESET "\n");

    const char *src_path = e->source ? e->source : v->source_resolved;
    const char *strip_note = "<args>";
    const char *direct_note = "(the chosen one runs directly: live output, no trial run, no retry)";
    const char *strip_footnote =
        e->strip_matched_args ? "(the matched argument is removed before forwarding)" : NULL;

    switch (e->policy) {
        case POLICY_EXIT_CODE:
        case POLICY_HEURISTIC:
        case POLICY_EXIT_CODE_MATCH: {
            run_source_line(v, src_path, "<args>");
            pf(RAIL "         " M_DIM "(a trial run: its output is captured, not shown)" M_RESET
                    "\n");
            pf(RAIL "\n");

            /* Conditions, padded to a common width. */
            char exit_codes[128] = "";
            if (e->policy == POLICY_EXIT_CODE_MATCH) {
                size_t off = 0;
                for (size_t i = 0; i < e->exit_code_count && off < sizeof(exit_codes) - 12; i++) {
                    off += (size_t)snprintf(exit_codes + off, sizeof(exit_codes) - off, "%s%d",
                                            i > 0 ? ", " : "", e->exit_codes[i]);
                }
            }
            char cond_fb[192];
            if (e->policy == POLICY_EXIT_CODE) {
                snprintf(cond_fb, sizeof(cond_fb), "exit != 0");
            } else if (e->policy == POLICY_HEURISTIC) {
                snprintf(cond_fb, sizeof(cond_fb), "exit != 0, and stderr matches");
            } else {
                snprintf(cond_fb, sizeof(cond_fb), "exit is one of: %s", exit_codes);
            }
            const char *cond_ok = "exit 0";
            const char *cond_other = e->policy == POLICY_HEURISTIC
                                          ? "exit != 0, no pattern matches"
                                          : "any other non-zero exit";
            const char *conds[3] = {cond_ok, cond_fb, cond_other};
            size_t w = max_len(conds, e->policy == POLICY_EXIT_CODE ? 2 : 3);

            bool has_other = e->policy != POLICY_EXIT_CODE;
            branch(false, w, cond_ok);
            pf("its output is shown as-is, then exit 0\n");
            pf(RAIL "\n");
            branch(!has_other, w, cond_fb);
            if (e->policy == POLICY_HEURISTIC) {
                char *pats = join_quoted(e->error_patterns, e->error_pattern_count, " | ");
                pf(M_YELLOW "%s" M_RESET "\n", pats);
                free(pats);
                branch_cont(false, w);
            }
            pf("discard its output, ");
            run_fallback_text(v, "<args>");
            if (has_other) {
                pf(RAIL "\n");
                branch(true, w, cond_other);
                pf("the source's real output and exit code are surfaced (no fallback)\n");
            }
            char *limit_h = human_bytes(v->limit_bytes);
            pf("\n  " M_DIM "(past %d ms or %s of output, the trial run is streamed live instead\n"
               "   and can no longer fall back)" M_RESET "\n",
               v->timeout_ms, limit_h);
            free(limit_h);
            break;
        }
        case POLICY_ROUTE_ARGS: {
            char *ra = join_words(e->route_args, e->route_arg_count, " | ");
            pf("  is any argument one of:  " M_YELLOW "%s" M_RESET " ?\n", ra);
            free(ra);
            pf(RAIL "\n");
            branch(false, 3, "yes");
            run_fallback_text(v, strip_note);
            pf(RAIL "\n");
            branch(true, 3, "no");
            pf(M_GREEN "run SOURCE" M_RESET "   ");
            print_cmdline(src_path, e->source_args, e->source_arg_count, "<args>",
                          v->source_state);
            pf("\n\n  " M_DIM "%s" M_RESET "\n", direct_note);
            if (strip_footnote) {
                pf("  " M_DIM "%s" M_RESET "\n", strip_footnote);
            }
            break;
        }
        case POLICY_SPLIT_ARGS: {
            char *sa = join_words(e->source_route_args, e->source_route_arg_count, " ");
            char *fa = join_words(e->fallback_route_args, e->fallback_route_arg_count, " ");
            pf("  which side has ALL of its own arguments present?\n");
            pf(RAIL "    source's:   " M_YELLOW "%s" M_RESET "\n", sa);
            pf(RAIL "    fallback's: " M_YELLOW "%s" M_RESET "\n", fa);
            free(sa);
            free(fa);
            pf(RAIL "\n");
            const char *conds[4] = {"only source's", "only fallback's", "both", "neither"};
            size_t w = max_len(conds, 4);
            branch(false, w, conds[0]);
            pf(M_GREEN "run SOURCE" M_RESET "   ");
            print_cmdline(src_path, e->source_args, e->source_arg_count, strip_note,
                          v->source_state);
            pf("\n");
            branch(false, w, conds[1]);
            run_fallback_text(v, strip_note);
            branch(false, w, conds[2]);
            pf("the side with MORE arguments wins " M_DIM "(a tie goes to source)" M_RESET "\n");
            branch(true, w, conds[3]);
            pf("like exit-code: " M_GREEN "SOURCE" M_RESET " as a trial run, and on any "
               "failure " M_BLUE "FALLBACK" M_RESET "\n");
            pf("\n  " M_DIM "(a chosen side runs directly: live output, no trial run, no "
               "retry)" M_RESET "\n");
            if (strip_footnote) {
                pf("  " M_DIM "%s" M_RESET "\n", strip_footnote);
            }
            break;
        }
        case POLICY_REWRITE: {
            pf("  rewrite each argument that equals a rule's <from>:\n");
            size_t w = max_len((const char *const *)e->rewrite_from, e->rewrite_from_count);
            for (size_t i = 0; i < e->rewrite_from_count; i++) {
                pf(RAIL "    " M_YELLOW "%-*s" M_RESET " " M_DIM "->" M_RESET " ", (int)w,
                   e->rewrite_from[i]);
                print_rewrite_to(e->rewrite_to[i]);
            }
            pf(RAIL "\n");
            pf("    " M_DIM "v" M_RESET "\n");
            run_source_line(v, src_path, "<rewritten args>");
            pf("\n  " M_DIM "(runs directly: live output, no trial run, no retry)" M_RESET "\n");
            break;
        }
        case POLICY_PASSTHROUGH: {
            run_source_line(v, src_path, "<args>");
            pf(RAIL "         " M_DIM "(live output the whole time -- not captured)" M_RESET
                    "\n");
            pf(RAIL "\n");
            size_t w = max_len((const char *const[]){"exit 0", "exit != 0"}, 2);
            branch(false, w, "exit 0");
            pf("its output is already shown; exit 0\n");
            pf(RAIL "\n");
            branch(true, w, "exit != 0");
            run_fallback_text(v, "<args>");
            pf("\n  " M_DIM "(fallback also runs with live output -- the source's own failure\n"
               "   output has already been seen by the time it runs)" M_RESET "\n");
            break;
        }
        case POLICY_ROUTE_MAP: {
            pf("  find the first route whose match equals one of the arguments:\n");
            pf(RAIL "\n");
            size_t w = strlen("(no match)");
            for (size_t i = 0; i < e->route_count; i++) {
                size_t l = strlen(e->routes[i].match);
                if (l > w) {
                    w = l;
                }
            }
            for (size_t i = 0; i < e->route_count; i++) {
                const RouteEntry *r = &e->routes[i];
                CmdState st = cmd_state(r->command, v->self_exe, e->force);
                branch(false, w, r->match);
                print_cmdline(r->command, r->args, r->arg_count, strip_note, st);
                pf("\n");
            }
            pf(RAIL "\n");
            branch(true, w, "(no match)");
            pf(M_GREEN "SOURCE" M_RESET " ");
            print_cmdline(src_path, e->source_args, e->source_arg_count, "<args>",
                          v->source_state);
            pf("\n\n  " M_DIM "%s" M_RESET "\n", direct_note);
            if (strip_footnote) {
                pf("  " M_DIM "%s" M_RESET "\n", strip_footnote);
            }
            break;
        }
    }
}

/* ---- entry point ----------------------------------------------------------- */

int cmd_info(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "shimback: info: missing shim name\n%s", USAGE);
        return 1;
    }
    if (argc > 2) {
        die("info: unexpected argument '%s'", argv[2]);
    }
    const char *name = argv[1];
    if (!is_valid_shim_name(name)) {
        die("info: invalid shim name '%s' -- names may only contain letters, digits, '.', '_', "
            "'+', and '-'",
            name);
    }

    char *cfg_path = config_file_path();
    Config cfg;
    char errbuf[256];
    ConfigStatus cst = config_load(cfg_path, &cfg, errbuf, sizeof(errbuf));
    if (cst != CONFIG_OK) {
        die("info: %s", errbuf);
    }

    char *shim_dir = shim_bin_dir();
    char *self_exe = self_exe_path();
    char *shim_file = shim_file_name(name);
    char *link_path = path_join(shim_dir, shim_file);
    free(shim_file);

    ShimEntry *entry = NULL;
    char *split_path = NULL;
    ShimSource src = resolve_shim_entry(&cfg, name, &entry, &split_path);

    g_colorize = stdout_is_color();

    if (src == SHIM_SOURCE_ORPHAN) {
        if (access(link_path, F_OK) != 0) {
            fprintf(stderr, "shimback: info: no shim configured for '%s'\n", name);
            size_t count = 0;
            char **names = collect_all_shim_names(&cfg, &count);
            if (count > 0) {
                char *hint = fuzzy_suggest(name, (const char *const *)names, count);
                if (hint) {
                    print_suggestion_hint(hint);
                    free(hint);
                }
            }
            return 1;
        }
        /* A real symlink with nothing configured for it: show what there is
         * to show, then fail, since there's no shim to describe. */
        View v = {0};
        v.name = name;
        v.self_exe = self_exe;
        pf(M_HDR "shim:" M_RESET " " M_CYAN "%s" M_RESET " " M_DIM "(orphaned)" M_RESET "\n",
           name);
        print_locations(&v, src, NULL, false, cfg_path, shim_dir);
        pf("\n" M_RED "'%s' has a shim symlink but no configuration -- run `shimback doctor "
           "fix` to remove it, or `shimback add %s ...` to configure it." M_RESET "\n",
           name, name);
        return 1;
    }

    const ShimEntry *e = entry;
    /* NULL for exit-code, the uncolored baseline -- must stay a real string,
     * or pf() would print the M_POLICY marker byte literally. */
    const char *pcolor = policy_color(e->policy);
    g_policy_color = pcolor ? pcolor : "";

    View v = {0};
    v.e = e;
    v.name = name;
    v.self_exe = self_exe;
    if (e->source) {
        v.source_state = cmd_state(e->source, self_exe, e->force);
        v.source_resolved = is_executable_file(e->source) ? canonicalize(e->source) : NULL;
    } else {
        v.source_resolved = path_search(name, shim_dir, self_exe);
        v.source_state = v.source_resolved ? CMD_OK : CMD_MISSING;
    }
    if (e->fallback) {
        v.fallback_state = cmd_state(e->fallback, self_exe, e->force);
    }
    v.timeout_ms = e->capture_timeout_set ? e->capture_timeout_ms : cfg.capture_timeout_ms;
    v.limit_bytes = e->capture_limit_set ? e->capture_limit_bytes : cfg.capture_limit_bytes;

    bool shadowed_entry = src == SHIM_SOURCE_SPLIT && config_find(&cfg, name) != NULL;

    pf(M_HDR "shim:" M_RESET " " M_CYAN "%s" M_RESET " " M_DIM "(" M_RESET M_POLICY "%s" M_RESET
             M_DIM ")" M_RESET "\n",
       name, policy_to_string(e->policy));

    print_overview(&v);
    print_locations(&v, src, split_path, shadowed_entry, cfg_path, shim_dir);
    print_commands(&v);
    print_policy_settings(&v);
    print_trial_run(&v, &cfg);
    print_flow(&v, shim_dir);

    pf("\n");
    if (g_issues == 0) {
        pf(M_BOLD M_GREEN "no problems noticed" M_RESET " " M_DIM "-- `shimback doctor` runs the "
           "full set of checks" M_RESET "\n");
    } else {
        pf(M_BOLD M_RED "%d problem(s) noticed" M_RESET " " M_DIM "-- see the [fail]/[warn] "
           "lines above; `shimback doctor` has the full set of checks" M_RESET "\n",
           g_issues);
    }

    if (src == SHIM_SOURCE_SPLIT) {
        shim_entry_free(entry);
        free(entry);
    }
    return 0;
}
