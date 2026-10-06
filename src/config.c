#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "paths.h"
#include "platform/platform.h"
#include "util.h"

void config_init(Config *cfg) {
    cfg->version = 1;
    cfg->shims = NULL;
    cfg->count = 0;
    cfg->cap = 0;
    cfg->verbose = false;
    cfg->capture_timeout_ms = SHIMBACK_DEFAULT_CAPTURE_TIMEOUT_MS;
    cfg->capture_limit_bytes = SHIMBACK_DEFAULT_CAPTURE_LIMIT_BYTES;
    cfg->backup_dir = NULL;
    cfg->backup_name_pattern = NULL;
    cfg->backup_override = false;
    cfg->mise_integration_set = false;
    cfg->mise_integration = false;
}

/* Everything about each policy that's a fixed property of the policy
 * itself, in enum order. */
static const struct {
    const char *name;
    const char *color; /* see policy_color */
    const char *description;
    bool uses_fallback;
    bool uses_trial_run;
} POLICY_INFO[POLICY__COUNT] = {
    [POLICY_EXIT_CODE] = {"exit-code", NULL,
                          "Fall back whenever source exits non-zero (the default).", true, true},
    [POLICY_HEURISTIC] = {"heuristic", ANSI_YELLOW,
                          "Fall back only when source's stderr matches a configured error "
                          "pattern.",
                          true, true},
    [POLICY_EXIT_CODE_MATCH] = {"exit-code-match", ANSI_MAGENTA,
                                "Fall back only when source exits with one of the configured "
                                "codes.",
                                true, true},
    [POLICY_ROUTE_ARGS] = {"route-args", ANSI_CYAN,
                           "Pick source or fallback up front, based on the invocation's "
                           "arguments.",
                           true, false},
    [POLICY_REWRITE] = {"rewrite", ANSI_GREEN,
                        "Always run source, rewriting matched arguments first; no fallback used.",
                        false, false},
    [POLICY_SPLIT_ARGS] = {"split-args", ANSI_RED,
                           "Pick source or fallback up front, from each side's own "
                           "most-discriminating args.",
                           true, true},
    [POLICY_ROUTE_MAP] = {"route-map", ANSI_BLUE,
                          "Route to any number of other commands, based on the invocation's "
                          "arguments.",
                          false, false},
    [POLICY_PASSTHROUGH] = {"passthrough", ANSI_BOLD ANSI_MAGENTA,
                            "Like exit-code, but never hides source's output -- no diagnostic "
                            "or capture limits.",
                            true, false},
};

/* An out-of-range value reads as the default policy. */
static Policy valid_policy(Policy p) {
    return (unsigned)p < POLICY__COUNT ? p : POLICY_EXIT_CODE;
}

const char *policy_to_string(Policy p) {
    return POLICY_INFO[valid_policy(p)].name;
}

bool policy_from_string(const char *s, Policy *out) {
    for (int i = 0; i < POLICY__COUNT; i++) {
        if (strcmp(s, POLICY_INFO[i].name) == 0) {
            *out = (Policy)i;
            return true;
        }
    }
    return false;
}

const char *policy_color(Policy p) {
    return POLICY_INFO[valid_policy(p)].color;
}

const char *policy_description(Policy p) {
    return POLICY_INFO[valid_policy(p)].description;
}

bool policy_uses_fallback(Policy p) {
    return POLICY_INFO[valid_policy(p)].uses_fallback;
}

bool policy_uses_trial_run(Policy p) {
    return POLICY_INFO[valid_policy(p)].uses_trial_run;
}

char *policy_names_list(void) {
    DynBuf buf;
    dynbuf_init(&buf);
    for (int i = 0; i < POLICY__COUNT; i++) {
        if (i > 0) {
            dynbuf_append_str(&buf, i == POLICY__COUNT - 1 ? ", or " : ", ");
        }
        dynbuf_append_char(&buf, '"');
        dynbuf_append_str(&buf, POLICY_INFO[i].name);
        dynbuf_append_char(&buf, '"');
    }
    char *result = xstrdup(dynbuf_cstr(&buf));
    dynbuf_free(&buf);
    return result;
}

ShimEntry *config_find(Config *cfg, const char *name) {
    for (size_t i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->shims[i].name, name) == 0) {
            return &cfg->shims[i];
        }
    }
    return NULL;
}

size_t config_upsert(Config *cfg, const char *name) {
    for (size_t i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->shims[i].name, name) == 0) {
            return i;
        }
    }
    if (cfg->count == cfg->cap) {
        size_t new_cap = cfg->cap == 0 ? 4 : cfg->cap * 2;
        cfg->shims = xrealloc(cfg->shims, new_cap * sizeof(ShimEntry));
        cfg->cap = new_cap;
    }
    ShimEntry *entry = &cfg->shims[cfg->count];
    memset(entry, 0, sizeof(*entry));
    entry->name = xstrdup(name);
    entry->policy = POLICY_EXIT_CODE;
    return cfg->count++;
}

bool config_remove(Config *cfg, const char *name) {
    for (size_t i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->shims[i].name, name) == 0) {
            shim_entry_free(&cfg->shims[i]);
            memmove(&cfg->shims[i], &cfg->shims[i + 1],
                    (cfg->count - i - 1) * sizeof(ShimEntry));
            cfg->count--;
            return true;
        }
    }
    return false;
}

void route_entry_free(RouteEntry *route) {
    free(route->match);
    free(route->command);
    str_array_free(route->args, route->arg_count);
}

void shim_entry_free(ShimEntry *entry) {
    free(entry->name);
    free(entry->source);
    free(entry->fallback);
    str_array_free(entry->source_args, entry->source_arg_count);
    str_array_free(entry->fallback_args, entry->fallback_arg_count);
    str_array_free(entry->error_patterns, entry->error_pattern_count);
    free(entry->exit_codes);
    str_array_free(entry->route_args, entry->route_arg_count);
    str_array_free(entry->source_route_args, entry->source_route_arg_count);
    str_array_free(entry->fallback_route_args, entry->fallback_route_arg_count);
    str_array_free(entry->rewrite_from, entry->rewrite_from_count);
    str_array_free(entry->rewrite_to, entry->rewrite_to_count);
    for (size_t i = 0; i < entry->route_count; i++) {
        route_entry_free(&entry->routes[i]);
    }
    free(entry->routes);
    memset(entry, 0, sizeof(*entry));
}

void config_free(Config *cfg) {
    for (size_t i = 0; i < cfg->count; i++) {
        shim_entry_free(&cfg->shims[i]);
    }
    free(cfg->shims);
    cfg->shims = NULL;
    cfg->count = 0;
    cfg->cap = 0;
    free(cfg->backup_dir);
    cfg->backup_dir = NULL;
    free(cfg->backup_name_pattern);
    cfg->backup_name_pattern = NULL;
}

/* ---- Parsing ---- */

/* Trims leading/trailing ASCII whitespace in place by adjusting *start
 * and writing a new NUL. */
static void trim(char **start) {
    char *s = *start;
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        e--;
    }
    *e = '\0';
    *start = s;
}

/* Splits off the next line of a NUL-terminated buffer in place: writes a
 * NUL over its '\n', advances *cursor past it, and returns the line, or
 * NULL once the buffer is exhausted. Unlike strtok_r, empty lines are
 * returned too, so a line counter stays in step with the physical file. */
static char *next_line(char **cursor) {
    char *line = *cursor;
    if (line == NULL) {
        return NULL;
    }
    char *nl = strchr(line, '\n');
    if (nl) {
        *nl = '\0';
        *cursor = nl + 1;
    } else {
        *cursor = NULL;
    }
    return line;
}

/* Truncates `line` at the first '#' that occurs outside a quoted string.
 * Inside quotes a backslash escapes the next character, matching
 * parse_quoted_string, so "C:\\" ends at its last quote. */
static void strip_trailing_comment(char *line) {
    bool in_quotes = false;
    for (char *p = line; *p != '\0'; p++) {
        if (in_quotes && *p == '\\' && p[1] != '\0') {
            p++;
        } else if (*p == '"') {
            in_quotes = !in_quotes;
        } else if (*p == '#' && !in_quotes) {
            *p = '\0';
            return;
        }
    }
}

/* The next line of `*cursor` (see next_line) with its comment stripped and
 * whitespace trimmed, skipping blank ones; counts every physical line read
 * into *line_no. NULL once the buffer is exhausted. */
static char *next_content_line(char **cursor, int *line_no) {
    char *line;
    while ((line = next_line(cursor)) != NULL) {
        (*line_no)++;
        strip_trailing_comment(line);
        trim(&line);
        if (*line != '\0') {
            return line;
        }
    }
    return NULL;
}

/* Splits a `key = value` line in place at its first '=', trimming both
 * sides. False if there is no '='. */
static bool split_key_value(char *line, char **key, char **value) {
    char *eq = strchr(line, '=');
    if (!eq) {
        return false;
    }
    *eq = '\0';
    *key = line;
    *value = eq + 1;
    trim(key);
    trim(value);
    return true;
}

/* Parses a quoted string starting at *cursor (which must point at the
 * opening '"'), advancing *cursor past the closing '"'. Returns a newly
 * allocated, unescaped string, or NULL on malformed input. */
static char *parse_quoted_string(const char **cursor) {
    const char *p = *cursor;
    if (*p != '"') {
        return NULL;
    }
    p++;
    DynBuf buf;
    dynbuf_init(&buf);
    while (*p != '\0' && *p != '"') {
        if (*p == '\\' && p[1] != '\0') {
            p++;
            char c;
            switch (*p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                default: c = *p; break;
            }
            dynbuf_append_char(&buf, c);
            p++;
        } else {
            dynbuf_append_char(&buf, *p);
            p++;
        }
    }
    if (*p != '"') {
        dynbuf_free(&buf);
        return NULL;
    }
    p++;
    char *result = xstrdup(dynbuf_cstr(&buf));
    dynbuf_free(&buf);
    *cursor = p;
    return result;
}

static void skip_ws(const char **cursor) {
    while (isspace((unsigned char)**cursor)) {
        (*cursor)++;
    }
}

/* Parses a `["a", "b"]` string array starting at *cursor (at the opening
 * '['). Populates `out` (must be strvec_init'd). Returns false on malformed
 * input. */
static bool parse_string_array(const char **cursor, StrVec *out) {
    const char *p = *cursor;
    if (*p != '[') {
        return false;
    }
    p++;
    skip_ws(&p);
    if (*p == ']') {
        p++;
        *cursor = p;
        return true;
    }
    for (;;) {
        skip_ws(&p);
        char *item = parse_quoted_string(&p);
        if (!item) {
            return false;
        }
        strvec_push(out, item);
        skip_ws(&p);
        if (*p == ',') {
            p++;
            skip_ws(&p);
            if (*p == ']') {
                p++;
                break;
            }
            continue;
        }
        if (*p == ']') {
            p++;
            break;
        }
        return false;
    }
    *cursor = p;
    return true;
}

/* Parses a `[75, 77]` integer array starting at *cursor (at the opening
 * '['). On success, out and out_count receive a newly allocated array
 * (NULL/0 if empty); the caller owns it. Returns false (leaving *out
 * untouched) on malformed input or a value outside 0-255. */
static bool parse_int_array(const char **cursor, int **out, size_t *out_count) {
    const char *p = *cursor;
    if (*p != '[') {
        return false;
    }
    p++;
    skip_ws(&p);

    int *items = NULL;
    size_t count = 0;
    size_t cap = 0;

    if (*p == ']') {
        p++;
        *cursor = p;
        *out = items;
        *out_count = count;
        return true;
    }

    for (;;) {
        skip_ws(&p);
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v > 255) {
            free(items);
            return false;
        }
        if (count == cap) {
            cap = cap == 0 ? 4 : cap * 2;
            items = xrealloc(items, cap * sizeof(int));
        }
        items[count++] = (int)v;
        p = end;
        skip_ws(&p);
        if (*p == ',') {
            p++;
            skip_ws(&p);
            if (*p == ']') {
                p++;
                break;
            }
            continue;
        }
        if (*p == ']') {
            p++;
            break;
        }
        free(items);
        return false;
    }
    *cursor = p;
    *out = items;
    *out_count = count;
    return true;
}

/* Reads the whole (already-opened) `f` into a newly allocated, NUL-terminated
 * buffer, closing it either way. Shared by config_load and
 * config_load_split, whose only difference is how a missing file is
 * handled (empty-but-valid vs. an error) -- both, so, do their own plat_fopen()
 * and ENOENT-handling before calling this. */
static ConfigStatus read_file_into_buffer(FILE *f, const char *path, char **out, char *errbuf,
                                           size_t errbuf_size) {
    size_t n = 0;
    long size = 0;
    switch (read_open_file(f, out, &n, &size)) {
        case FILE_READ_OK:
            return CONFIG_OK;
        case FILE_READ_SEEK_FAILED:
            snprintf(errbuf, errbuf_size, "cannot read %s: %s", path, plat_strerror(errno));
            return CONFIG_ERR_IO;
        case FILE_READ_SHORT:
        default:
            snprintf(errbuf, errbuf_size, "cannot read %s: incomplete read (got %zu of %ld bytes)",
                     path, n, size);
            return CONFIG_ERR_IO;
    }
}

/* After a value's own syntax (a quoted string, an array, ...) has already
 * been parsed and *cursor advanced past it, requires only trailing
 * whitespace remains -- an inline comment is already stripped before
 * parsing ever starts (see strip_trailing_comment), so in practice this
 * just means end of the line. Without this, something like
 * `fallback = "/bin/echo" garbage` is silently accepted with the trailing
 * garbage simply ignored -- and then silently dropped for good the next
 * time shimback rewrites the file, hiding what was actually a malformed
 * line instead of rejecting it (see review.md). */
static bool no_trailing_garbage(const char *cursor) {
    skip_ws(&cursor);
    return *cursor == '\0';
}

/* Parses one already-split `key`/`value_str` pair (value_str still raw,
 * untrimmed-of-quotes TOML syntax) into `entry`. Shared by config_load's
 * per-[shims.x]-line handling and config_load_split (a split file's every
 * line is one of these, with no section header involved) so the two can
 * never drift apart on what a shim's fields mean. */
/* `key = "..."`: replaces *dst with the parsed string. */
static ConfigStatus parse_str_field(const char *value_str, char **dst, const char *key,
                                    int line_no, char *errbuf, size_t errbuf_size) {
    const char *cursor = value_str;
    char *v = parse_quoted_string(&cursor);
    if (!v || !no_trailing_garbage(cursor)) {
        free(v);
        snprintf(errbuf, errbuf_size, "line %d: expected a string for '%s'", line_no, key);
        return CONFIG_ERR_PARSE;
    }
    free(*dst);
    *dst = v;
    return CONFIG_OK;
}

/* `key = ["a", ...]`: replaces the array at *dst (of *dst_count items). */
static ConfigStatus parse_str_array_field(const char *value_str, char ***dst,
                                          size_t *dst_count, const char *key, int line_no,
                                          char *errbuf, size_t errbuf_size) {
    const char *cursor = value_str;
    StrVec vec;
    strvec_init(&vec);
    if (!parse_string_array(&cursor, &vec) || !no_trailing_garbage(cursor)) {
        strvec_free(&vec);
        snprintf(errbuf, errbuf_size, "line %d: malformed '%s' array", line_no, key);
        return CONFIG_ERR_PARSE;
    }
    str_array_free(*dst, *dst_count);
    *dst = vec.items;
    *dst_count = vec.count;
    return CONFIG_OK;
}

/* `key = true|false`. */
static ConfigStatus parse_bool_field(const char *value_str, bool *dst, const char *key,
                                     int line_no, char *errbuf, size_t errbuf_size) {
    if (strcmp(value_str, "true") == 0) {
        *dst = true;
    } else if (strcmp(value_str, "false") == 0) {
        *dst = false;
    } else {
        snprintf(errbuf, errbuf_size, "line %d: '%s' must be true or false", line_no, key);
        return CONFIG_ERR_PARSE;
    }
    return CONFIG_OK;
}

static ConfigStatus parse_shim_entry_field(ShimEntry *entry, const char *key, char *value_str,
                                            int line_no, char *errbuf, size_t errbuf_size) {
    const char *cursor = value_str;

    if (strcmp(key, "source") == 0) {
        return parse_str_field(value_str, &entry->source, key, line_no, errbuf,
                               errbuf_size);
    } else if (strcmp(key, "fallback") == 0) {
        return parse_str_field(value_str, &entry->fallback, key, line_no, errbuf,
                               errbuf_size);
    } else if (strcmp(key, "source_args") == 0) {
        return parse_str_array_field(value_str, &entry->source_args,
                                     &entry->source_arg_count, key, line_no, errbuf,
                                     errbuf_size);
    } else if (strcmp(key, "fallback_args") == 0) {
        return parse_str_array_field(value_str, &entry->fallback_args,
                                     &entry->fallback_arg_count, key, line_no, errbuf,
                                     errbuf_size);
    } else if (strcmp(key, "policy") == 0) {
        char *v = parse_quoted_string(&cursor);
        if (!v || !no_trailing_garbage(cursor) || !policy_from_string(v, &entry->policy)) {
            char *names = policy_names_list();
            snprintf(errbuf, errbuf_size, "line %d: 'policy' must be %s", line_no, names);
            free(names);
            free(v);
            return CONFIG_ERR_PARSE;
        }
        free(v);
    } else if (strcmp(key, "exit_codes") == 0) {
        int *items = NULL;
        size_t count = 0;
        if (!parse_int_array(&cursor, &items, &count) || !no_trailing_garbage(cursor)) {
            free(items);
            snprintf(errbuf, errbuf_size, "line %d: malformed 'exit_codes' array (values "
                                           "must be integers 0-255)",
                     line_no);
            return CONFIG_ERR_PARSE;
        }
        free(entry->exit_codes);
        entry->exit_codes = items;
        entry->exit_code_count = count;
    } else if (strcmp(key, "error_patterns") == 0) {
        return parse_str_array_field(value_str, &entry->error_patterns,
                                     &entry->error_pattern_count, key, line_no, errbuf,
                                     errbuf_size);
    } else if (strcmp(key, "route_args") == 0) {
        return parse_str_array_field(value_str, &entry->route_args,
                                     &entry->route_arg_count, key, line_no, errbuf,
                                     errbuf_size);
    } else if (strcmp(key, "source_route_args") == 0) {
        return parse_str_array_field(value_str, &entry->source_route_args,
                                     &entry->source_route_arg_count, key, line_no, errbuf,
                                     errbuf_size);
    } else if (strcmp(key, "fallback_route_args") == 0) {
        return parse_str_array_field(value_str, &entry->fallback_route_args,
                                     &entry->fallback_route_arg_count, key, line_no, errbuf,
                                     errbuf_size);
    } else if (strcmp(key, "diagnostic") == 0) {
        return parse_bool_field(value_str, &entry->diagnostic, key, line_no, errbuf,
                                errbuf_size);
    } else if (strcmp(key, "force") == 0) {
        return parse_bool_field(value_str, &entry->force, key, line_no, errbuf,
                                errbuf_size);
    } else if (strcmp(key, "capture_timeout_ms") == 0) {
        if (!parse_nonneg_int(value_str, &entry->capture_timeout_ms)) {
            snprintf(errbuf, errbuf_size,
                     "line %d: 'capture_timeout_ms' must be a non-negative integer", line_no);
            return CONFIG_ERR_PARSE;
        }
        entry->capture_timeout_set = true;
    } else if (strcmp(key, "capture_limit") == 0) {
        char *v = parse_quoted_string(&cursor);
        size_t bytes;
        if (!v || !no_trailing_garbage(cursor) || !parse_size_bytes(v, &bytes)) {
            snprintf(errbuf, errbuf_size,
                     "line %d: 'capture_limit' must be a quoted size like \"8MiB\" or "
                     "\"8388608\"",
                     line_no);
            free(v);
            return CONFIG_ERR_PARSE;
        }
        free(v);
        entry->capture_limit_bytes = bytes;
        entry->capture_limit_set = true;
    } else if (strcmp(key, "rewrite_from") == 0) {
        return parse_str_array_field(value_str, &entry->rewrite_from,
                                     &entry->rewrite_from_count, key, line_no, errbuf,
                                     errbuf_size);
    } else if (strcmp(key, "rewrite_to") == 0) {
        return parse_str_array_field(value_str, &entry->rewrite_to,
                                     &entry->rewrite_to_count, key, line_no, errbuf,
                                     errbuf_size);
    } else if (strcmp(key, "strip_matched_args") == 0) {
        return parse_bool_field(value_str, &entry->strip_matched_args, key, line_no, errbuf,
                                errbuf_size);
    } else {
        warn("config: line %d: unknown key '%s' for shim '%s', ignoring", line_no, key,
             entry->name);
    }
    return CONFIG_OK;
}

/* Parses one already-split `key`/`value_str` pair into a single route of a
 * POLICY_ROUTE_MAP shim -- the fields a [[shims.x.routes]] (main config)
 * or bare [[routes]] (split file) block can have. Shared shape with
 * parse_shim_entry_field above, just for a much smaller field set. */
static ConfigStatus parse_route_field(RouteEntry *route, const char *key, char *value_str,
                                       int line_no, char *errbuf, size_t errbuf_size) {
    if (strcmp(key, "match") == 0) {
        return parse_str_field(value_str, &route->match, key, line_no, errbuf, errbuf_size);
    }
    if (strcmp(key, "command") == 0) {
        return parse_str_field(value_str, &route->command, key, line_no, errbuf, errbuf_size);
    }
    if (strcmp(key, "args") == 0) {
        return parse_str_array_field(value_str, &route->args, &route->arg_count, key, line_no,
                                     errbuf, errbuf_size);
    }
    warn("config: line %d: unknown key '%s' for a route, ignoring", line_no, key);
    return CONFIG_OK;
}

/* Per-entry validation shared by config_load (once for every shim, after
 * the whole file parses) and config_load_split (immediately, since a
 * split file only ever describes the one shim it's named after). Also
 * called directly by add.c's finish_add before writing a new/updated
 * entry to disk -- see the declaration in config.h. */
ConfigStatus validate_shim_entry(const ShimEntry *entry, char *errbuf, size_t errbuf_size) {
    if (!entry->fallback && policy_uses_fallback(entry->policy)) {
        snprintf(errbuf, errbuf_size, "shim '%s' is missing a required 'fallback'", entry->name);
        return CONFIG_ERR_VALIDATION;
    }
    if (entry->rewrite_from_count != entry->rewrite_to_count) {
        snprintf(errbuf, errbuf_size,
                 "shim '%s' has %zu 'rewrite_from' entries but %zu 'rewrite_to' entries -- "
                 "they must match up one-to-one",
                 entry->name, entry->rewrite_from_count, entry->rewrite_to_count);
        return CONFIG_ERR_VALIDATION;
    }
    if (entry->policy == POLICY_HEURISTIC && entry->error_pattern_count == 0) {
        snprintf(errbuf, errbuf_size,
                 "shim '%s' uses policy \"heuristic\" but has no error_patterns", entry->name);
        return CONFIG_ERR_VALIDATION;
    }
    if (entry->policy == POLICY_EXIT_CODE_MATCH && entry->exit_code_count == 0) {
        snprintf(errbuf, errbuf_size,
                 "shim '%s' uses policy \"exit-code-match\" but has no exit_codes", entry->name);
        return CONFIG_ERR_VALIDATION;
    }
    if (entry->policy == POLICY_ROUTE_ARGS && entry->route_arg_count == 0) {
        snprintf(errbuf, errbuf_size,
                 "shim '%s' uses policy \"route-args\" but has no route_args", entry->name);
        return CONFIG_ERR_VALIDATION;
    }
    if (entry->policy == POLICY_SPLIT_ARGS &&
        (entry->source_route_arg_count == 0 || entry->fallback_route_arg_count == 0)) {
        snprintf(errbuf, errbuf_size,
                 "shim '%s' uses policy \"split-args\" but needs at least one "
                 "source_route_args and one fallback_route_args entry",
                 entry->name);
        return CONFIG_ERR_VALIDATION;
    }
    if (entry->policy == POLICY_REWRITE && entry->rewrite_from_count == 0) {
        snprintf(errbuf, errbuf_size,
                 "shim '%s' uses policy \"rewrite\" but has no rewrite rules", entry->name);
        return CONFIG_ERR_VALIDATION;
    }
    if (entry->policy == POLICY_ROUTE_MAP && entry->route_count == 0) {
        snprintf(errbuf, errbuf_size,
                 "shim '%s' uses policy \"route-map\" but has no routes", entry->name);
        return CONFIG_ERR_VALIDATION;
    }
    if (entry->policy == POLICY_PASSTHROUGH && entry->diagnostic) {
        snprintf(errbuf, errbuf_size,
                 "shim '%s' uses policy \"passthrough\", which never captures or hides "
                 "anything -- \"diagnostic\" has nothing to report",
                 entry->name);
        return CONFIG_ERR_VALIDATION;
    }
    if (entry->policy == POLICY_PASSTHROUGH && entry->capture_timeout_set) {
        snprintf(errbuf, errbuf_size,
                 "shim '%s' uses policy \"passthrough\", which never captures output -- "
                 "a capture timeout has nothing to apply to",
                 entry->name);
        return CONFIG_ERR_VALIDATION;
    }
    if (entry->policy == POLICY_PASSTHROUGH && entry->capture_limit_set) {
        snprintf(errbuf, errbuf_size,
                 "shim '%s' uses policy \"passthrough\", which never captures output -- "
                 "a capture limit has nothing to apply to",
                 entry->name);
        return CONFIG_ERR_VALIDATION;
    }
    /* parse_route_field() lets a hand-edited [[...routes]] block omit either
     * key, so completeness has to be enforced here -- serialization,
     * dispatch, doctor, and the duplicate check below all assume both are
     * real, non-empty strings. */
    for (size_t i = 0; i < entry->route_count; i++) {
        const RouteEntry *r = &entry->routes[i];
        if (!r->match || r->match[0] == '\0' || !r->command || r->command[0] == '\0') {
            snprintf(errbuf, errbuf_size,
                     "shim '%s': route %zu requires a non-empty 'match' and 'command'",
                     entry->name, i + 1);
            return CONFIG_ERR_VALIDATION;
        }
    }
    /* Two routes with the same command and the exact same fixed args would
     * be genuinely unreachable duplication: whichever comes first in the
     * file always wins, so the second could never fire under any input,
     * unlike two routes that merely share a command with *different* args
     * (the whole point of this policy over route-args -- see config.h). */
    for (size_t i = 0; i < entry->route_count; i++) {
        for (size_t j = i + 1; j < entry->route_count; j++) {
            const RouteEntry *a = &entry->routes[i];
            const RouteEntry *b = &entry->routes[j];
            if (strcmp(a->command, b->command) == 0 &&
                str_array_eq(a->args, a->arg_count, b->args, b->arg_count)) {
                snprintf(errbuf, errbuf_size,
                         "shim '%s': route %zu and route %zu are identical (same command and "
                         "args) -- the second could never fire",
                         entry->name, i + 1, j + 1);
                return CONFIG_ERR_VALIDATION;
            }
        }
    }
    return CONFIG_OK;
}

ConfigStatus config_load(const char *path, Config *cfg, char *errbuf, size_t errbuf_size) {
    config_init(cfg);

    FILE *f = plat_fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) {
            return CONFIG_OK; /* no config yet is not an error */
        }
        snprintf(errbuf, errbuf_size, "cannot open %s: %s", path, plat_strerror(errno));
        return CONFIG_ERR_IO;
    }

    char *contents;
    ConfigStatus read_st = read_file_into_buffer(f, path, &contents, errbuf, errbuf_size);
    if (read_st != CONFIG_OK) {
        return read_st;
    }

    ssize_t current_index = -1; /* -1 = top-level, no [shims.x] section yet */
    ssize_t current_route_index = -1; /* -1 = not inside a [[shims.x.routes]] block */
    int line_no = 0;
    ConfigStatus status = CONFIG_OK;

    char *cursor = contents;
    char *trimmed;
    while (status == CONFIG_OK && (trimmed = next_content_line(&cursor, &line_no)) != NULL) {
        /* [[shims.<name>.routes]] -- an array-of-tables entry, one per
         * route of a POLICY_ROUTE_MAP shim. Checked before the plain
         * single-bracket branch below: that branch strips only one
         * trailing ']', which would otherwise leave this as
         * "[shims.<name>.routes" (leading '[' still attached) and
         * silently misfile it as an "unknown section", not fail loudly
         * (see review.md's history of exactly this class of silent-
         * misparse bug for other constructs). */
        if (trimmed[0] == '[' && trimmed[1] == '[') {
            size_t len = strlen(trimmed);
            if (len < 4 || trimmed[len - 1] != ']' || trimmed[len - 2] != ']') {
                snprintf(errbuf, errbuf_size, "line %d: malformed array-of-tables header",
                         line_no);
                status = CONFIG_ERR_PARSE;
                break;
            }
            trimmed[len - 2] = '\0';
            char *inner = trimmed + 2;
            const char *prefix = "shims.";
            const char *suffix = ".routes";
            size_t inner_len = strlen(inner);
            size_t prefix_len = strlen(prefix);
            size_t suffix_len = strlen(suffix);
            bool valid_shape = strncmp(inner, prefix, prefix_len) == 0 &&
                                inner_len > prefix_len + suffix_len &&
                                strcmp(inner + inner_len - suffix_len, suffix) == 0;
            if (!valid_shape) {
                snprintf(errbuf, errbuf_size,
                         "line %d: unrecognized array-of-tables [[%s]] -- expected "
                         "[[shims.<name>.routes]]",
                         line_no, inner);
                status = CONFIG_ERR_PARSE;
                break;
            }
            size_t name_len = inner_len - prefix_len - suffix_len;
            char *route_shim_name = xstrndup(inner + prefix_len, name_len);
            if (current_index < 0 ||
                strcmp(cfg->shims[current_index].name, route_shim_name) != 0) {
                snprintf(errbuf, errbuf_size,
                         "line %d: [[shims.%s.routes]] must come after its own [shims.%s] "
                         "section",
                         line_no, route_shim_name, route_shim_name);
                free(route_shim_name);
                status = CONFIG_ERR_PARSE;
                break;
            }
            free(route_shim_name);

            ShimEntry *shim = &cfg->shims[current_index];
            shim->routes = xrealloc(shim->routes, (shim->route_count + 1) * sizeof(RouteEntry));
            memset(&shim->routes[shim->route_count], 0, sizeof(RouteEntry));
            current_route_index = (ssize_t)shim->route_count;
            shim->route_count++;

            continue;
        }

        if (*trimmed == '[') {
            current_route_index = -1; /* leaving any route block */
            size_t len = strlen(trimmed);
            if (trimmed[len - 1] != ']') {
                snprintf(errbuf, errbuf_size, "line %d: malformed section header", line_no);
                status = CONFIG_ERR_PARSE;
                break;
            }
            trimmed[len - 1] = '\0';
            char *header = trimmed + 1;
            const char *prefix = "shims.";
            if (strncmp(header, prefix, strlen(prefix)) == 0 && header[strlen(prefix)] != '\0') {
                const char *shim_name = header + strlen(prefix);
                /* A name reaching config_upsert unvalidated ends up
                 * trusted everywhere downstream that reads it back out of
                 * `cfg` -- including split_config_filename() (via
                 * uninstall --full's sweep, doctor, list, ...), where a
                 * '/'/'..'-containing name could reach outside shimback's
                 * own directories entirely. A hand-edited config.toml
                 * with a name like this is exactly as malformed as one
                 * with broken section-header syntax, so it's rejected the
                 * same way (see review.md). */
                if (!is_valid_shim_name(shim_name)) {
                    snprintf(errbuf, errbuf_size,
                             "line %d: invalid shim name '%s' in section header -- names may "
                             "only contain letters, digits, '.', '_', '+', and '-'",
                             line_no, shim_name);
                    status = CONFIG_ERR_PARSE;
                    break;
                }
                current_index = (ssize_t)config_upsert(cfg, shim_name);
            } else {
                warn("config: line %d: unknown section [%s], ignoring", line_no, header);
                current_index = -1;
            }
            continue;
        }

        char *key;
        char *value_str;
        if (!split_key_value(trimmed, &key, &value_str)) {
            snprintf(errbuf, errbuf_size, "line %d: expected 'key = value'", line_no);
            status = CONFIG_ERR_PARSE;
            break;
        }

        if (current_route_index >= 0) {
            RouteEntry *route = &cfg->shims[current_index].routes[current_route_index];
            status = parse_route_field(route, key, value_str, line_no, errbuf, errbuf_size);
            if (status != CONFIG_OK) {
                break;
            }
            continue;
        }

        if (current_index < 0) {
            if (strcmp(key, "version") == 0) {
                if (!parse_nonneg_int(value_str, &cfg->version)) {
                    snprintf(errbuf, errbuf_size, "line %d: 'version' must be a non-negative "
                                                   "integer",
                             line_no);
                    status = CONFIG_ERR_PARSE;
                }
            } else if (strcmp(key, "verbose") == 0) {
                status = parse_bool_field(value_str, &cfg->verbose, key, line_no, errbuf,
                                          errbuf_size);
            } else if (strcmp(key, "capture_timeout_ms") == 0) {
                if (!parse_nonneg_int(value_str, &cfg->capture_timeout_ms)) {
                    snprintf(errbuf, errbuf_size,
                             "line %d: 'capture_timeout_ms' must be a non-negative integer",
                             line_no);
                    status = CONFIG_ERR_PARSE;
                }
            } else if (strcmp(key, "capture_limit") == 0) {
                const char *cursor = value_str;
                char *v = parse_quoted_string(&cursor);
                size_t bytes;
                if (!v || !no_trailing_garbage(cursor) || !parse_size_bytes(v, &bytes)) {
                    snprintf(errbuf, errbuf_size,
                             "line %d: 'capture_limit' must be a quoted size like \"8MiB\" or "
                             "\"8388608\"",
                             line_no);
                    status = CONFIG_ERR_PARSE;
                } else {
                    cfg->capture_limit_bytes = bytes;
                }
                free(v);
            } else if (strcmp(key, "backup_dir") == 0) {
                const char *cursor = value_str;
                char *v = parse_quoted_string(&cursor);
                if (!v || !no_trailing_garbage(cursor)) {
                    snprintf(errbuf, errbuf_size, "line %d: 'backup_dir' must be a quoted path",
                             line_no);
                    status = CONFIG_ERR_PARSE;
                    free(v);
                } else {
                    free(cfg->backup_dir);
                    cfg->backup_dir = v;
                }
            } else if (strcmp(key, "backup_name") == 0) {
                const char *cursor = value_str;
                char *v = parse_quoted_string(&cursor);
                if (!v || !no_trailing_garbage(cursor)) {
                    snprintf(errbuf, errbuf_size,
                             "line %d: 'backup_name' must be a quoted pattern", line_no);
                    status = CONFIG_ERR_PARSE;
                    free(v);
                } else {
                    free(cfg->backup_name_pattern);
                    cfg->backup_name_pattern = v;
                }
            } else if (strcmp(key, "backup_override") == 0) {
                status = parse_bool_field(value_str, &cfg->backup_override, key, line_no, errbuf,
                                          errbuf_size);
            } else if (strcmp(key, "mise_integration") == 0) {
                status = parse_bool_field(value_str, &cfg->mise_integration, key, line_no,
                                          errbuf, errbuf_size);
                cfg->mise_integration_set = status == CONFIG_OK;
            } else {
                warn("config: line %d: unknown top-level key '%s', ignoring", line_no, key);
            }
            continue;
        }

        ShimEntry *entry = &cfg->shims[current_index];
        status = parse_shim_entry_field(entry, key, value_str, line_no, errbuf, errbuf_size);
        if (status != CONFIG_OK) {
            break;
        }
    }

    free(contents);
    if (status != CONFIG_OK) {
        return status;
    }

    for (size_t i = 0; i < cfg->count; i++) {
        ConfigStatus vst = validate_shim_entry(&cfg->shims[i], errbuf, errbuf_size);
        if (vst != CONFIG_OK) {
            return vst;
        }
    }

    return CONFIG_OK;
}

ConfigStatus config_load_split(const char *path, ShimEntry *entry, char *errbuf,
                                size_t errbuf_size) {
    FILE *f = plat_fopen(path, "rb");
    if (!f) {
        snprintf(errbuf, errbuf_size, "cannot open %s: %s", path, plat_strerror(errno));
        return CONFIG_ERR_IO;
    }

    char *contents;
    ConfigStatus read_st = read_file_into_buffer(f, path, &contents, errbuf, errbuf_size);
    if (read_st != CONFIG_OK) {
        return read_st;
    }

    int line_no = 0;
    ConfigStatus status = CONFIG_OK;
    ssize_t current_route_index = -1; /* -1 = not inside a [[routes]] block */

    char *cursor = contents;
    char *trimmed;
    while (status == CONFIG_OK && (trimmed = next_content_line(&cursor, &line_no)) != NULL) {
        /* A split file has no [shims.<name>] section to qualify a route
         * block with (see config_save_split), so its own routes use bare
         * [[routes]] instead of [[shims.<name>.routes]]. */
        if (strcmp(trimmed, "[[routes]]") == 0) {
            entry->routes =
                xrealloc(entry->routes, (entry->route_count + 1) * sizeof(RouteEntry));
            memset(&entry->routes[entry->route_count], 0, sizeof(RouteEntry));
            current_route_index = (ssize_t)entry->route_count;
            entry->route_count++;
            continue;
        }

        if (*trimmed == '[') {
            snprintf(errbuf, errbuf_size,
                     "line %d: split config files hold one shim's settings as bare key = value "
                     "lines and, for policy \"route-map\", [[routes]] blocks -- they can't "
                     "contain a [shims.x] section header or any other bracketed header",
                     line_no);
            status = CONFIG_ERR_PARSE;
            break;
        }

        char *key;
        char *value_str;
        if (!split_key_value(trimmed, &key, &value_str)) {
            snprintf(errbuf, errbuf_size, "line %d: expected 'key = value'", line_no);
            status = CONFIG_ERR_PARSE;
            break;
        }

        if (current_route_index >= 0) {
            RouteEntry *route = &entry->routes[current_route_index];
            status = parse_route_field(route, key, value_str, line_no, errbuf, errbuf_size);
            if (status != CONFIG_OK) {
                break;
            }
            continue;
        }

        status = parse_shim_entry_field(entry, key, value_str, line_no, errbuf, errbuf_size);
        if (status != CONFIG_OK) {
            break;
        }
    }

    free(contents);
    if (status != CONFIG_OK) {
        return status;
    }

    return validate_shim_entry(entry, errbuf, errbuf_size);
}

/* ---- Serialization ---- */

static void append_escaped_string(DynBuf *out, const char *s) {
    dynbuf_append_char(out, '"');
    for (const char *p = s; *p != '\0'; p++) {
        switch (*p) {
            case '"': dynbuf_append_str(out, "\\\""); break;
            case '\\': dynbuf_append_str(out, "\\\\"); break;
            case '\n': dynbuf_append_str(out, "\\n"); break;
            case '\t': dynbuf_append_str(out, "\\t"); break;
            default: dynbuf_append_char(out, *p); break;
        }
    }
    dynbuf_append_char(out, '"');
}

/* Renders every field of `entry` except its name -- no [shims.<name>]
 * header, no bare "name = ..." line either, since the name is implied by
 * context (a section header render_config writes itself, or the filename
 * for a split file -- see config_save_split). Shared by both.
 *
 * `route_header_prefix` controls how any routes (POLICY_ROUTE_MAP) are
 * rendered: the main config file needs each route as its own
 * `[[shims.<name>.routes]]` block (pass the shim's name here), while a
 * split file -- which has no enclosing [shims.<name>] section at all --
 * uses bare `[[routes]]` (pass NULL). Routes are always rendered last:
 * once a `[[...]]` array-of-tables block is open, any subsequent bare
 * `key = value` line belongs to *that* table, not back to the shim itself
 * -- so nothing from this function can follow them. */
/* `key = "value"` -- nothing when `value` is NULL. */
static void append_str_line(DynBuf *out, const char *key, const char *value) {
    if (!value) {
        return;
    }
    dynbuf_append_str(out, key);
    dynbuf_append_str(out, " = ");
    append_escaped_string(out, value);
    dynbuf_append_char(out, '\n');
}

/* `key = ["a", "b"]` -- nothing when the array is empty. */
static void append_str_array_line(DynBuf *out, const char *key, char *const *items,
                                  size_t count) {
    if (count == 0) {
        return;
    }
    dynbuf_append_str(out, key);
    dynbuf_append_str(out, " = [");
    for (size_t j = 0; j < count; j++) {
        if (j > 0) {
            dynbuf_append_str(out, ", ");
        }
        append_escaped_string(out, items[j]);
    }
    dynbuf_append_str(out, "]\n");
}

static void render_shim_entry_body(const ShimEntry *entry, DynBuf *out,
                                    const char *route_header_prefix) {
    char line[64];
    append_str_line(out, "source", entry->source);
    append_str_array_line(out, "source_args", entry->source_args, entry->source_arg_count);

    append_str_line(out, "fallback", entry->fallback);
    append_str_array_line(out, "fallback_args", entry->fallback_args, entry->fallback_arg_count);

    append_str_line(out, "policy", policy_to_string(entry->policy));

    append_str_array_line(out, "error_patterns", entry->error_patterns,
                          entry->error_pattern_count);

    if (entry->exit_code_count > 0) {
        dynbuf_append_str(out, "exit_codes = [");
        for (size_t j = 0; j < entry->exit_code_count; j++) {
            if (j > 0) {
                dynbuf_append_str(out, ", ");
            }
            char numbuf[16];
            snprintf(numbuf, sizeof(numbuf), "%d", entry->exit_codes[j]);
            dynbuf_append_str(out, numbuf);
        }
        dynbuf_append_str(out, "]\n");
    }

    append_str_array_line(out, "route_args", entry->route_args, entry->route_arg_count);
    append_str_array_line(out, "source_route_args", entry->source_route_args,
                          entry->source_route_arg_count);
    append_str_array_line(out, "fallback_route_args", entry->fallback_route_args,
                          entry->fallback_route_arg_count);

    if (entry->strip_matched_args) {
        dynbuf_append_str(out, "strip_matched_args = true\n");
    }

    append_str_array_line(out, "rewrite_from", entry->rewrite_from, entry->rewrite_from_count);
    append_str_array_line(out, "rewrite_to", entry->rewrite_to, entry->rewrite_to_count);

    if (entry->diagnostic) {
        dynbuf_append_str(out, "diagnostic = true\n");
    }
    if (entry->force) {
        dynbuf_append_str(out, "force = true\n");
    }
    if (entry->capture_timeout_set) {
        snprintf(line, sizeof(line), "capture_timeout_ms = %d\n", entry->capture_timeout_ms);
        dynbuf_append_str(out, line);
    }
    if (entry->capture_limit_set) {
        snprintf(line, sizeof(line), "capture_limit = \"%zu\"\n", entry->capture_limit_bytes);
        dynbuf_append_str(out, line);
    }

    for (size_t i = 0; i < entry->route_count; i++) {
        const RouteEntry *route = &entry->routes[i];
        dynbuf_append_char(out, '\n');
        dynbuf_append_str(out, "[[");
        if (route_header_prefix) {
            dynbuf_append_str(out, "shims.");
            dynbuf_append_str(out, route_header_prefix);
            dynbuf_append_char(out, '.');
        }
        dynbuf_append_str(out, "routes]]\n");

        append_str_line(out, "match", route->match);
        append_str_line(out, "command", route->command);
        append_str_array_line(out, "args", route->args, route->arg_count);
    }
}

static void render_config(const Config *cfg, DynBuf *out) {
    char line[64];
    snprintf(line, sizeof(line), "version = %d\n", cfg->version);
    dynbuf_append_str(out, line);
    if (cfg->verbose) {
        dynbuf_append_str(out, "verbose = true\n");
    }
    if (cfg->capture_timeout_ms != SHIMBACK_DEFAULT_CAPTURE_TIMEOUT_MS) {
        snprintf(line, sizeof(line), "capture_timeout_ms = %d\n", cfg->capture_timeout_ms);
        dynbuf_append_str(out, line);
    }
    if (cfg->capture_limit_bytes != SHIMBACK_DEFAULT_CAPTURE_LIMIT_BYTES) {
        snprintf(line, sizeof(line), "capture_limit = \"%zu\"\n", cfg->capture_limit_bytes);
        dynbuf_append_str(out, line);
    }
    append_str_line(out, "backup_dir", cfg->backup_dir);
    append_str_line(out, "backup_name", cfg->backup_name_pattern);
    if (cfg->backup_override) {
        dynbuf_append_str(out, "backup_override = true\n");
    }
    if (cfg->mise_integration_set) {
        dynbuf_append_str(out, cfg->mise_integration ? "mise_integration = true\n"
                                                       : "mise_integration = false\n");
    }

    for (size_t i = 0; i < cfg->count; i++) {
        const ShimEntry *entry = &cfg->shims[i];
        dynbuf_append_char(out, '\n');
        dynbuf_append_str(out, "[shims.");
        dynbuf_append_str(out, entry->name);
        dynbuf_append_str(out, "]\n");
        render_shim_entry_body(entry, out, entry->name);
    }
}

/* Writes rendered config text to `path` (creating its directory first) and
 * frees `out`. 0600: a config file -- config.toml or a split file --
 * controls which executables a shim actually runs, so it's owner-only
 * rather than the world-readable 0644 generated shell files get, enforced
 * on every save (see write_file_atomic, paths.c), not just at first
 * creation, so it's self-healing even if something else ever leaves the
 * file at a weaker mode. */
static ConfigStatus write_config_text(const char *path, DynBuf *out, char *errbuf,
                                      size_t errbuf_size) {
    char *dir = dir_of(path);
    bool dir_ok = mkdir_p(dir);
    if (!dir_ok) {
        snprintf(errbuf, errbuf_size, "cannot create directory %s: %s", dir, plat_strerror(errno));
    }
    free(dir);
    if (!dir_ok) {
        dynbuf_free(out);
        return CONFIG_ERR_IO;
    }

    bool ok = write_file_atomic(path, out->data, out->len, 0600);
    dynbuf_free(out);
    if (!ok) {
        snprintf(errbuf, errbuf_size, "cannot write %s: %s", path, plat_strerror(errno));
        return CONFIG_ERR_IO;
    }
    return CONFIG_OK;
}

ConfigStatus config_save(const Config *cfg, const char *path, char *errbuf, size_t errbuf_size) {
    DynBuf out;
    dynbuf_init(&out);
    render_config(cfg, &out);
    return write_config_text(path, &out, errbuf, errbuf_size);
}

ConfigStatus config_save_split(const ShimEntry *entry, const char *path, char *errbuf,
                                size_t errbuf_size) {
    DynBuf out;
    dynbuf_init(&out);
    render_shim_entry_body(entry, &out, NULL);
    return write_config_text(path, &out, errbuf, errbuf_size);
}

size_t remove_split_configs(const char *name) {
    /* Same reasoning as resolve_split_config_path(): callers sweeping
     * every name they can find (uninstall --full over list_shim_symlink_
     * names(), in particular -- see review.md) can hand this an unsafe
     * name that was never validated by add.c/remove.c/config_load(). An
     * invalid name can never have a legitimate split file to remove, so
     * skip it (with a warning, since it's still worth knowing about)
     * instead of letting split_config_filename()'s defensive die() abort
     * the whole sweep partway through. */
    if (!is_valid_shim_name(name)) {
        warn("skipping split-config cleanup for invalid shim name '%s'", name);
        return 0;
    }
    char *paths[3];
    split_config_all_paths(name, paths);
    size_t removed = 0;
    for (int i = 0; i < 3; i++) {
        if (unlink(paths[i]) == 0) {
            removed++;
        } else if (errno != ENOENT) {
            /* Not existing is the common case, not an error -- only most
             * locations have anything in them at all. A real failure
             * (permissions, read-only filesystem, ...) is worth surfacing
             * even though callers here treat this as best-effort: it's
             * the only place that finds out at all. */
            warn("failed to remove split config %s: %s", paths[i], plat_strerror(errno));
        }
        free(paths[i]);
    }
    return removed;
}

ShimSource resolve_shim_entry(Config *cfg, const char *name, ShimEntry **entry,
                               char **split_path_out) {
    char *split_path = resolve_split_config_path(name);
    if (split_path) {
        ShimEntry *split_entry = xmalloc(sizeof(ShimEntry));
        memset(split_entry, 0, sizeof(*split_entry));
        split_entry->name = xstrdup(name);
        split_entry->policy = POLICY_EXIT_CODE;

        char errbuf[256];
        ConfigStatus st = config_load_split(split_path, split_entry, errbuf, sizeof(errbuf));
        if (st != CONFIG_OK) {
            die("failed to load split config for '%s': %s", name, errbuf);
        }

        *entry = split_entry;
        if (split_path_out) {
            *split_path_out = split_path;
        } else {
            free(split_path);
        }
        return SHIM_SOURCE_SPLIT;
    }

    if (split_path_out) {
        *split_path_out = NULL;
    }

    ShimEntry *found = config_find(cfg, name);
    if (found) {
        *entry = found;
        return SHIM_SOURCE_CONFIG;
    }

    *entry = NULL;
    return SHIM_SOURCE_ORPHAN;
}

/* Appends a copy of `name` to `names` unless it's already there. */
static void push_unique_name(char **names, size_t *count, const char *name) {
    for (size_t j = 0; j < *count; j++) {
        if (strcmp(names[j], name) == 0) {
            return;
        }
    }
    names[(*count)++] = xstrdup(name);
}

char **collect_all_shim_names(const Config *cfg, size_t *out_count) {
    size_t symlink_count = 0;
    char **symlink_names = list_shim_symlink_names(&symlink_count);
    size_t split_count = 0;
    char **split_names = list_split_config_names(&split_count);

    size_t cap = cfg->count + symlink_count + split_count;
    char **names = cap > 0 ? xmalloc(cap * sizeof(char *)) : NULL;
    size_t count = 0;

    for (size_t i = 0; i < cfg->count; i++) {
        names[count++] = xstrdup(cfg->shims[i].name);
    }
    for (size_t i = 0; i < symlink_count; i++) {
        push_unique_name(names, &count, symlink_names[i]);
        free(symlink_names[i]);
    }
    free(symlink_names);

    for (size_t i = 0; i < split_count; i++) {
        push_unique_name(names, &count, split_names[i]);
        free(split_names[i]);
    }
    free(split_names);

    *out_count = count;
    return names;
}
