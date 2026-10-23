/* Note: this is a short-lived CLI command handler. Heap allocations here are
 * intentionally not freed before process exit -- the OS reclaims them, and
 * this is a standard, deliberate simplification for one-shot CLI tools. */
#include "commands.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../config.h"
#include "../paths.h"
#include "../platform/platform.h"
#include "../suggest.h"
#include "../util.h"

/* Tried in order when $EDITOR is unset or blank. On Windows, `edit` is
 * Microsoft Edit, which ships with Windows 11. */
#ifdef _WIN32
static const char *const FALLBACK_EDITORS[] = {"edit", "nvim", "vim", "vi", "nano"};
#else
static const char *const FALLBACK_EDITORS[] = {"nvim", "vim", "vi", "nano", "pico"};
#endif
#define FALLBACK_EDITOR_COUNT (sizeof(FALLBACK_EDITORS) / sizeof(FALLBACK_EDITORS[0]))

/* The runnable file a command word names: the word itself if it's a path,
 * else the first match on PATH (see path_search_command). NULL if neither. */
static char *resolve_command(const char *word) {
    for (const char *c = word; *c; c++) {
        if (is_path_sep(*c)) {
            return is_executable_file(word) ? xstrdup(word) : NULL;
        }
    }
    return path_search_command(word);
}

static bool is_blank(const char *s) {
    for (; *s; s++) {
        if (*s != ' ' && *s != '\t') {
            return false;
        }
    }
    return true;
}

/* Runs `editor_argv` (the editor and any flags of its own) on `target_path`
 * from the already-resolved `exe`, keeping the editor's own argv[0] (vim
 * picks vi-compatible mode from it). */
static int run_editor(const char *exe, StrVec *editor_argv, const char *target_path) {
    char **argv = xmalloc((editor_argv->count + 2) * sizeof(char *));
    for (size_t i = 0; i < editor_argv->count; i++) {
        argv[i] = editor_argv->items[i];
    }
#ifdef _WIN32
    /* config_file_path() can mix separators (C:\\Users\\x\\.config/...);
     * hand the editor a plain Windows path. */
    char *native = xstrdup(target_path);
    for (char *c = native; *c; c++) {
        if (*c == '/') {
            *c = '\\';
        }
    }
    argv[editor_argv->count] = native;
#else
    argv[editor_argv->count] = (char *)target_path;
#endif
    argv[editor_argv->count + 1] = NULL;
#ifdef _WIN32
    /* Windows runs a .bat/.cmd through cmd.exe, which acts on & | < > ^ in
     * an argument and expands %VAR% even inside quotes -- so an $EDITOR
     * like `code --wait&calc` would run calc. Refuse rather than escape:
     * % can't be escaped reliably. */
    const char *dot = strrchr(path_basename(exe), '.');
    if (dot && (_stricmp(dot, ".bat") == 0 || _stricmp(dot, ".cmd") == 0)) {
        /* argv[0] itself is replaced by `exe` on the command line. */
        for (size_t i = 0; argv[i]; i++) {
            const char *arg = i == 0 ? exe : argv[i];
            if (strpbrk(arg, "&|<>^%\"")) {
                warn("edit: refusing to pass '%s' to %s -- batch files run through cmd.exe, "
                     "which would interpret its & | < > ^ %% or \" characters",
                     arg, exe);
                return 127;
            }
        }
    }
#endif
    int code = plat_run_inherited(exe, argv);
    if (code < 0) {
        die("edit: failed to run %s: %s", exe, plat_strerror(errno));
    }
    return code;
}

/* Opens `target_path` in $EDITOR, or else the first fallback editor found.
 * Returns the editor's exit code (127 if no editor could be run at all). */
static int open_in_editor(const char *target_path) {
    const char *editor_env = plat_getenv("EDITOR");
    StrVec words;
    strvec_init(&words);

    if (editor_env && !is_blank(editor_env)) {
        /* Split into words (multi-word editors like "code --wait"), but
         * never through a shell: see plat_split_command. Running $EDITOR
         * via `sh -c` would execute any `;`, `&&` or `$(...)` it contains
         * before the editor even opened -- not something to do
         * unconditionally with whatever the environment happens to hold
         * (e.g. inherited across `sudo -E`, a CI job, or a container). */
        const char *why = NULL;
        if (!plat_split_command(editor_env, &words, &why)) {
            warn("$EDITOR ('%s') doesn't look like a plain command%s%s%s -- refusing to run it",
                 editor_env, why ? " (" : "", why ? why : "", why ? ")" : "");
            return 127;
        }
        char *exe = resolve_command(words.items[0]);
        if (!exe) {
            warn("$EDITOR ('%s'): '%s' not found or not executable", editor_env, words.items[0]);
            return 127;
        }
        return run_editor(exe, &words, target_path);
    }

    for (size_t i = 0; i < FALLBACK_EDITOR_COUNT; i++) {
        char *exe = resolve_command(FALLBACK_EDITORS[i]);
        if (exe) {
            strvec_push(&words, xstrdup(FALLBACK_EDITORS[i]));
            return run_editor(exe, &words, target_path);
        }
    }

    DynBuf names;
    dynbuf_init(&names);
    for (size_t i = 0; i < FALLBACK_EDITOR_COUNT; i++) {
        dynbuf_append_str(&names, i > 0 ? ", " : "");
        dynbuf_append_str(&names, FALLBACK_EDITORS[i]);
    }
    warn("no editor found -- set $EDITOR, or install one of: %s", dynbuf_cstr(&names));
    return 127;
}

/* The file that currently defines shim `name`: its split config file if it
 * has one (which always wins over config.toml -- see resolve_shim_entry),
 * else config.toml if it has an entry there. Deliberately doesn't go through
 * resolve_shim_entry, which dies on a malformed split file -- that's exactly
 * the file someone would want to open to fix it. Exits 1 if `name` isn't
 * configured anywhere. */
static char *locate_shim_config(const char *name, const char *cfg_path, bool *is_split) {
    if (!is_valid_shim_name(name)) {
        die("edit: invalid shim name '%s' -- names may only contain letters, digits, '.', '_', "
            "'+', and '-'",
            name);
    }

    char *split_path = resolve_split_config_path(name);
    if (split_path) {
        *is_split = true;
        return split_path;
    }

    Config cfg;
    char errbuf[256];
    ConfigStatus cst = config_load(cfg_path, &cfg, errbuf, sizeof(errbuf));
    if (cst != CONFIG_OK) {
        die("edit: %s (run `shimback edit` with no arguments to fix it)", errbuf);
    }
    if (config_find(&cfg, name)) {
        config_free(&cfg);
        *is_split = false;
        return xstrdup(cfg_path);
    }

    warn("edit: no shim configured for '%s'", name);
    if (cfg.count > 0) {
        const char **candidates = xmalloc(cfg.count * sizeof(char *));
        for (size_t i = 0; i < cfg.count; i++) {
            candidates[i] = cfg.shims[i].name;
        }
        char *hint = fuzzy_suggest(name, candidates, cfg.count);
        if (hint) {
            print_suggestion_hint(hint);
            free(hint);
        }
        free(candidates);
    }
    config_free(&cfg);
    exit(1);
}

int cmd_edit(int argc, char **argv) {
    if (argc > 2) {
        die("edit: unexpected argument '%s'", argv[2]);
    }
    const char *shim_name = argc == 2 ? argv[1] : NULL;

    char *cfg_path = config_file_path();
    bool target_is_split = false;
    char *target_path;
    if (shim_name) {
        target_path = locate_shim_config(shim_name, cfg_path, &target_is_split);
    } else {
        char *cfg_dir = dir_of(cfg_path);
        if (!mkdir_p(cfg_dir)) {
            die("edit: failed to create config directory %s", cfg_dir);
        }
        free(cfg_dir);
        target_path = cfg_path;
    }

    int code = open_in_editor(target_path);

    /* A quick sanity check, not a hard requirement: if the editor exited
     * cleanly but left the file unparseable, say so right away rather
     * than letting the next add/list/doctor run surface a confusing error
     * far removed from the edit that caused it. A split file is checked
     * the way it's actually loaded (bare key = value lines, no sections),
     * not as a config.toml. */
    if (code == 0) {
        char errbuf[256];
        ConfigStatus cst;
        if (target_is_split) {
            ShimEntry entry;
            memset(&entry, 0, sizeof(entry));
            entry.name = xstrdup(shim_name);
            entry.policy = POLICY_EXIT_CODE;
            cst = config_load_split(target_path, &entry, errbuf, sizeof(errbuf));
            shim_entry_free(&entry);
        } else {
            Config cfg;
            cst = config_load(target_path, &cfg, errbuf, sizeof(errbuf));
            if (cst == CONFIG_OK) {
                config_free(&cfg);
            }
        }
        if (cst != CONFIG_OK) {
            warn("edit: %s now fails to parse: %s", target_path, errbuf);
        }
    }

    return code;
}
