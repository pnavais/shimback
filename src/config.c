#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "paths.h"
#include "util.h"

/* Strictly parses a non-negative integer field: rejects empty input,
 * trailing garbage, an out-of-range value, and a negative one. Unlike a
 * bare strtol() call, "nope" (or "-5", or "99999999999999999999") is
 * never silently accepted as 0 (or some wrapped/truncated value) -- for a
 * field like capture_timeout_ms, which directly drives dispatch behavior
 * (0 means "cut over to live output immediately"), silently misparsing a
 * typo would change behavior without ever surfacing as an error. */
static bool parse_nonneg_int(const char *s, int *out) {
    if (s[0] == '\0') {
        return false;
    }
    errno = 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || errno == ERANGE || v < 0 || v > INT_MAX) {
        return false;
    }
    *out = (int)v;
    return true;
}

void config_init(Config *cfg) {
    cfg->version = 1;
    cfg->shims = NULL;
    cfg->count = 0;
    cfg->cap = 0;
    cfg->verbose = false;
    cfg->capture_timeout_ms = SHIMBACK_DEFAULT_CAPTURE_TIMEOUT_MS;
    cfg->capture_limit_bytes = SHIMBACK_DEFAULT_CAPTURE_LIMIT_BYTES;
}

const char *policy_to_string(Policy p) {
    switch (p) {
        case POLICY_HEURISTIC: return "heuristic";
        case POLICY_EXIT_CODE_MATCH: return "exit-code-match";
        case POLICY_ROUTE_ARGS: return "route-args";
        case POLICY_REWRITE: return "rewrite";
        case POLICY_SPLIT_ARGS: return "split-args";
        case POLICY_ROUTE_MAP: return "route-map";
        case POLICY_PASSTHROUGH: return "passthrough";
        case POLICY_EXIT_CODE:
        default: return "exit-code";
    }
}

bool policy_from_string(const char *s, Policy *out) {
    if (strcmp(s, "exit-code") == 0) {
        *out = POLICY_EXIT_CODE;
        return true;
    }
    if (strcmp(s, "heuristic") == 0) {
        *out = POLICY_HEURISTIC;
        return true;
    }
    if (strcmp(s, "exit-code-match") == 0) {
        *out = POLICY_EXIT_CODE_MATCH;
        return true;
    }
    if (strcmp(s, "route-args") == 0) {
        *out = POLICY_ROUTE_ARGS;
        return true;
    }
    if (strcmp(s, "rewrite") == 0) {
        *out = POLICY_REWRITE;
        return true;
    }
    if (strcmp(s, "split-args") == 0) {
        *out = POLICY_SPLIT_ARGS;
        return true;
    }
    if (strcmp(s, "route-map") == 0) {
        *out = POLICY_ROUTE_MAP;
        return true;
    }
    if (strcmp(s, "passthrough") == 0) {
        *out = POLICY_PASSTHROUGH;
        return true;
    }
    return false;
}

const char *policy_color(Policy p) {
    switch (p) {
        case POLICY_HEURISTIC: return ANSI_YELLOW;
        case POLICY_EXIT_CODE_MATCH: return ANSI_MAGENTA;
        case POLICY_ROUTE_ARGS: return ANSI_CYAN;
        case POLICY_REWRITE: return ANSI_GREEN;
        case POLICY_SPLIT_ARGS: return ANSI_RED;
        case POLICY_ROUTE_MAP: return ANSI_BLUE;
        case POLICY_PASSTHROUGH: return ANSI_BOLD ANSI_MAGENTA;
        case POLICY_EXIT_CODE:
        default: return NULL;
    }
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
    entry->diagnostic = false;
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

void shim_entry_free(ShimEntry *entry) {
    free(entry->name);
    free(entry->source);
    free(entry->fallback);
    for (size_t i = 0; i < entry->source_arg_count; i++) {
        free(entry->source_args[i]);
    }
    free(entry->source_args);
    for (size_t i = 0; i < entry->fallback_arg_count; i++) {
        free(entry->fallback_args[i]);
    }
    free(entry->fallback_args);
    for (size_t i = 0; i < entry->error_pattern_count; i++) {
        free(entry->error_patterns[i]);
    }
    free(entry->error_patterns);
    free(entry->exit_codes);
    for (size_t i = 0; i < entry->route_arg_count; i++) {
        free(entry->route_args[i]);
    }
    free(entry->route_args);
    for (size_t i = 0; i < entry->source_route_arg_count; i++) {
        free(entry->source_route_args[i]);
    }
    free(entry->source_route_args);
    for (size_t i = 0; i < entry->fallback_route_arg_count; i++) {
        free(entry->fallback_route_args[i]);
    }
    free(entry->fallback_route_args);
    for (size_t i = 0; i < entry->rewrite_from_count; i++) {
        free(entry->rewrite_from[i]);
    }
    free(entry->rewrite_from);
    for (size_t i = 0; i < entry->rewrite_to_count; i++) {
        free(entry->rewrite_to[i]);
    }
    free(entry->rewrite_to);
    for (size_t i = 0; i < entry->route_count; i++) {
        RouteEntry *route = &entry->routes[i];
        free(route->match);
        free(route->command);
        for (size_t j = 0; j < route->arg_count; j++) {
            free(route->args[j]);
        }
        free(route->args);
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
}

/* ---- Parsing ---- */

static void trim(char **start, char *end_hint) {
    /* end_hint points just past the last meaningful char (a NUL or similar);
     * trims leading/trailing ASCII whitespace in place by adjusting *start
     * and writing a new NUL. */
    char *s = *start;
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    char *e = s + strlen(s);
    (void)end_hint;
    while (e > s && isspace((unsigned char)e[-1])) {
        e--;
    }
    *e = '\0';
    *start = s;
}

/* Truncates `line` at the first '#' that occurs outside a quoted string. */
static void strip_trailing_comment(char *line) {
    bool in_quotes = false;
    for (char *p = line; *p != '\0'; p++) {
        if (*p == '"' && (p == line || p[-1] != '\\')) {
            in_quotes = !in_quotes;
        } else if (*p == '#' && !in_quotes) {
            *p = '\0';
            return;
        }
    }
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
 * handled (empty-but-valid vs. an error) -- both, so, do their own fopen()
 * and ENOENT-handling before calling this. */
static ConfigStatus read_file_into_buffer(FILE *f, const char *path, char **out, char *errbuf,
                                           size_t errbuf_size) {
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(errbuf, errbuf_size, "cannot read %s: %s", path, strerror(errno));
        return CONFIG_ERR_IO;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(errbuf, errbuf_size, "cannot read %s: %s", path, strerror(errno));
        return CONFIG_ERR_IO;
    }
    char *contents = xmalloc((size_t)size + 1);
    size_t n = fread(contents, 1, (size_t)size, f);
    bool read_error = ferror(f) || n != (size_t)size;
    fclose(f);
    if (read_error) {
        /* A short read here means either a genuine I/O error or the file
         * changing size underneath us between the ftell() above and this
         * fread() -- either way, treating whatever partial bytes came
         * through as "the whole file" would let a subsequent save quietly
         * rewrite the file down to just that truncated prefix, discarding
         * every entry after it. Fail instead of guessing. */
        free(contents);
        snprintf(errbuf, errbuf_size, "cannot read %s: incomplete read (got %zu of %ld bytes)",
                 path, n, size);
        return CONFIG_ERR_IO;
    }
    contents[n] = '\0';
    *out = contents;
    return CONFIG_OK;
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
static ConfigStatus parse_shim_entry_field(ShimEntry *entry, const char *key, char *value_str,
                                            int line_no, char *errbuf, size_t errbuf_size) {
    const char *cursor = value_str;

    if (strcmp(key, "source") == 0) {
        char *v = parse_quoted_string(&cursor);
        if (!v || !no_trailing_garbage(cursor)) {
            free(v);
            snprintf(errbuf, errbuf_size, "line %d: expected a string for 'source'", line_no);
            return CONFIG_ERR_PARSE;
        }
        free(entry->source);
        entry->source = v;
    } else if (strcmp(key, "fallback") == 0) {
        char *v = parse_quoted_string(&cursor);
        if (!v || !no_trailing_garbage(cursor)) {
            free(v);
            snprintf(errbuf, errbuf_size, "line %d: expected a string for 'fallback'", line_no);
            return CONFIG_ERR_PARSE;
        }
        free(entry->fallback);
        entry->fallback = v;
    } else if (strcmp(key, "source_args") == 0) {
        StrVec vec;
        strvec_init(&vec);
        if (!parse_string_array(&cursor, &vec) || !no_trailing_garbage(cursor)) {
            strvec_free(&vec);
            snprintf(errbuf, errbuf_size, "line %d: malformed 'source_args' array", line_no);
            return CONFIG_ERR_PARSE;
        }
        for (size_t i = 0; i < entry->source_arg_count; i++) {
            free(entry->source_args[i]);
        }
        free(entry->source_args);
        entry->source_args = vec.items;
        entry->source_arg_count = vec.count;
    } else if (strcmp(key, "fallback_args") == 0) {
        StrVec vec;
        strvec_init(&vec);
        if (!parse_string_array(&cursor, &vec) || !no_trailing_garbage(cursor)) {
            strvec_free(&vec);
            snprintf(errbuf, errbuf_size, "line %d: malformed 'fallback_args' array", line_no);
            return CONFIG_ERR_PARSE;
        }
        for (size_t i = 0; i < entry->fallback_arg_count; i++) {
            free(entry->fallback_args[i]);
        }
        free(entry->fallback_args);
        entry->fallback_args = vec.items;
        entry->fallback_arg_count = vec.count;
    } else if (strcmp(key, "policy") == 0) {
        char *v = parse_quoted_string(&cursor);
        if (!v || !no_trailing_garbage(cursor) || !policy_from_string(v, &entry->policy)) {
            snprintf(errbuf, errbuf_size,
                     "line %d: 'policy' must be \"exit-code\", \"heuristic\", "
                     "\"exit-code-match\", \"route-args\", or \"rewrite\"",
                     line_no);
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
        StrVec vec;
        strvec_init(&vec);
        if (!parse_string_array(&cursor, &vec) || !no_trailing_garbage(cursor)) {
            strvec_free(&vec);
            snprintf(errbuf, errbuf_size, "line %d: malformed 'error_patterns' array", line_no);
            return CONFIG_ERR_PARSE;
        }
        for (size_t i = 0; i < entry->error_pattern_count; i++) {
            free(entry->error_patterns[i]);
        }
        free(entry->error_patterns);
        entry->error_patterns = vec.items;
        entry->error_pattern_count = vec.count;
    } else if (strcmp(key, "route_args") == 0) {
        StrVec vec;
        strvec_init(&vec);
        if (!parse_string_array(&cursor, &vec) || !no_trailing_garbage(cursor)) {
            strvec_free(&vec);
            snprintf(errbuf, errbuf_size, "line %d: malformed 'route_args' array", line_no);
            return CONFIG_ERR_PARSE;
        }
        for (size_t i = 0; i < entry->route_arg_count; i++) {
            free(entry->route_args[i]);
        }
        free(entry->route_args);
        entry->route_args = vec.items;
        entry->route_arg_count = vec.count;
    } else if (strcmp(key, "source_route_args") == 0) {
        StrVec vec;
        strvec_init(&vec);
        if (!parse_string_array(&cursor, &vec) || !no_trailing_garbage(cursor)) {
            strvec_free(&vec);
            snprintf(errbuf, errbuf_size, "line %d: malformed 'source_route_args' array",
                     line_no);
            return CONFIG_ERR_PARSE;
        }
        for (size_t i = 0; i < entry->source_route_arg_count; i++) {
            free(entry->source_route_args[i]);
        }
        free(entry->source_route_args);
        entry->source_route_args = vec.items;
        entry->source_route_arg_count = vec.count;
    } else if (strcmp(key, "fallback_route_args") == 0) {
        StrVec vec;
        strvec_init(&vec);
        if (!parse_string_array(&cursor, &vec) || !no_trailing_garbage(cursor)) {
            strvec_free(&vec);
            snprintf(errbuf, errbuf_size, "line %d: malformed 'fallback_route_args' array",
                     line_no);
            return CONFIG_ERR_PARSE;
        }
        for (size_t i = 0; i < entry->fallback_route_arg_count; i++) {
            free(entry->fallback_route_args[i]);
        }
        free(entry->fallback_route_args);
        entry->fallback_route_args = vec.items;
        entry->fallback_route_arg_count = vec.count;
    } else if (strcmp(key, "diagnostic") == 0) {
        if (strcmp(value_str, "true") == 0) {
            entry->diagnostic = true;
        } else if (strcmp(value_str, "false") == 0) {
            entry->diagnostic = false;
        } else {
            snprintf(errbuf, errbuf_size, "line %d: 'diagnostic' must be true or false", line_no);
            return CONFIG_ERR_PARSE;
        }
    } else if (strcmp(key, "force") == 0) {
        if (strcmp(value_str, "true") == 0) {
            entry->force = true;
        } else if (strcmp(value_str, "false") == 0) {
            entry->force = false;
        } else {
            snprintf(errbuf, errbuf_size, "line %d: 'force' must be true or false", line_no);
            return CONFIG_ERR_PARSE;
        }
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
        StrVec vec;
        strvec_init(&vec);
        if (!parse_string_array(&cursor, &vec) || !no_trailing_garbage(cursor)) {
            strvec_free(&vec);
            snprintf(errbuf, errbuf_size, "line %d: malformed 'rewrite_from' array", line_no);
            return CONFIG_ERR_PARSE;
        }
        for (size_t i = 0; i < entry->rewrite_from_count; i++) {
            free(entry->rewrite_from[i]);
        }
        free(entry->rewrite_from);
        entry->rewrite_from = vec.items;
        entry->rewrite_from_count = vec.count;
    } else if (strcmp(key, "rewrite_to") == 0) {
        StrVec vec;
        strvec_init(&vec);
        if (!parse_string_array(&cursor, &vec) || !no_trailing_garbage(cursor)) {
            strvec_free(&vec);
            snprintf(errbuf, errbuf_size, "line %d: malformed 'rewrite_to' array", line_no);
            return CONFIG_ERR_PARSE;
        }
        for (size_t i = 0; i < entry->rewrite_to_count; i++) {
            free(entry->rewrite_to[i]);
        }
        free(entry->rewrite_to);
        entry->rewrite_to = vec.items;
        entry->rewrite_to_count = vec.count;
    } else if (strcmp(key, "strip_matched_args") == 0) {
        if (strcmp(value_str, "true") == 0) {
            entry->strip_matched_args = true;
        } else if (strcmp(value_str, "false") == 0) {
            entry->strip_matched_args = false;
        } else {
            snprintf(errbuf, errbuf_size,
                     "line %d: 'strip_matched_args' must be true or false", line_no);
            return CONFIG_ERR_PARSE;
        }
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
    const char *cursor = value_str;

    if (strcmp(key, "match") == 0) {
        char *v = parse_quoted_string(&cursor);
        if (!v || !no_trailing_garbage(cursor)) {
            free(v);
            snprintf(errbuf, errbuf_size, "line %d: expected a string for 'match'", line_no);
            return CONFIG_ERR_PARSE;
        }
        free(route->match);
        route->match = v;
    } else if (strcmp(key, "command") == 0) {
        char *v = parse_quoted_string(&cursor);
        if (!v || !no_trailing_garbage(cursor)) {
            free(v);
            snprintf(errbuf, errbuf_size, "line %d: expected a string for 'command'", line_no);
            return CONFIG_ERR_PARSE;
        }
        free(route->command);
        route->command = v;
    } else if (strcmp(key, "args") == 0) {
        StrVec vec;
        strvec_init(&vec);
        if (!parse_string_array(&cursor, &vec) || !no_trailing_garbage(cursor)) {
            strvec_free(&vec);
            snprintf(errbuf, errbuf_size, "line %d: malformed 'args' array", line_no);
            return CONFIG_ERR_PARSE;
        }
        for (size_t i = 0; i < route->arg_count; i++) {
            free(route->args[i]);
        }
        free(route->args);
        route->args = vec.items;
        route->arg_count = vec.count;
    } else {
        warn("config: line %d: unknown key '%s' for a route, ignoring", line_no, key);
    }
    return CONFIG_OK;
}

/* Per-entry validation shared by config_load (once for every shim, after
 * the whole file parses) and config_load_split (immediately, since a
 * split file only ever describes the one shim it's named after). Also
 * called directly by add.c's finish_add before writing a new/updated
 * entry to disk -- see the declaration in config.h. */
ConfigStatus validate_shim_entry(const ShimEntry *entry, char *errbuf, size_t errbuf_size) {
    if (!entry->fallback && entry->policy != POLICY_REWRITE && entry->policy != POLICY_ROUTE_MAP) {
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
            if (strcmp(a->command, b->command) != 0 || a->arg_count != b->arg_count) {
                continue;
            }
            bool args_equal = true;
            for (size_t k = 0; k < a->arg_count; k++) {
                if (strcmp(a->args[k], b->args[k]) != 0) {
                    args_equal = false;
                    break;
                }
            }
            if (args_equal) {
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

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) {
            return CONFIG_OK; /* no config yet is not an error */
        }
        snprintf(errbuf, errbuf_size, "cannot open %s: %s", path, strerror(errno));
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

    char *saveptr = NULL;
    char *line = strtok_r(contents, "\n", &saveptr);
    while (line != NULL && status == CONFIG_OK) {
        line_no++;
        strip_trailing_comment(line);
        char *trimmed = line;
        trim(&trimmed, line + strlen(line));

        if (*trimmed == '\0') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

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

            line = strtok_r(NULL, "\n", &saveptr);
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
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        char *eq = strchr(trimmed, '=');
        if (!eq) {
            snprintf(errbuf, errbuf_size, "line %d: expected 'key = value'", line_no);
            status = CONFIG_ERR_PARSE;
            break;
        }
        *eq = '\0';
        char *key = trimmed;
        char *value_str = eq + 1;
        trim(&key, key + strlen(key));
        {
            char *vs = value_str;
            trim(&vs, vs + strlen(vs));
            value_str = vs;
        }

        if (current_route_index >= 0) {
            RouteEntry *route = &cfg->shims[current_index].routes[current_route_index];
            status = parse_route_field(route, key, value_str, line_no, errbuf, errbuf_size);
            if (status != CONFIG_OK) {
                break;
            }
            line = strtok_r(NULL, "\n", &saveptr);
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
                if (strcmp(value_str, "true") == 0) {
                    cfg->verbose = true;
                } else if (strcmp(value_str, "false") == 0) {
                    cfg->verbose = false;
                } else {
                    snprintf(errbuf, errbuf_size, "line %d: 'verbose' must be true or false",
                             line_no);
                    status = CONFIG_ERR_PARSE;
                }
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
            } else {
                warn("config: line %d: unknown top-level key '%s', ignoring", line_no, key);
            }
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        ShimEntry *entry = &cfg->shims[current_index];
        status = parse_shim_entry_field(entry, key, value_str, line_no, errbuf, errbuf_size);
        if (status != CONFIG_OK) {
            break;
        }

        line = strtok_r(NULL, "\n", &saveptr);
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
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(errbuf, errbuf_size, "cannot open %s: %s", path, strerror(errno));
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

    char *saveptr = NULL;
    char *line = strtok_r(contents, "\n", &saveptr);
    while (line != NULL && status == CONFIG_OK) {
        line_no++;
        strip_trailing_comment(line);
        char *trimmed = line;
        trim(&trimmed, line + strlen(line));

        if (*trimmed == '\0') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        /* A split file has no [shims.<name>] section to qualify a route
         * block with (see config_save_split), so its own routes use bare
         * [[routes]] instead of [[shims.<name>.routes]]. */
        if (strcmp(trimmed, "[[routes]]") == 0) {
            entry->routes =
                xrealloc(entry->routes, (entry->route_count + 1) * sizeof(RouteEntry));
            memset(&entry->routes[entry->route_count], 0, sizeof(RouteEntry));
            current_route_index = (ssize_t)entry->route_count;
            entry->route_count++;
            line = strtok_r(NULL, "\n", &saveptr);
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

        char *eq = strchr(trimmed, '=');
        if (!eq) {
            snprintf(errbuf, errbuf_size, "line %d: expected 'key = value'", line_no);
            status = CONFIG_ERR_PARSE;
            break;
        }
        *eq = '\0';
        char *key = trimmed;
        char *value_str = eq + 1;
        trim(&key, key + strlen(key));
        {
            char *vs = value_str;
            trim(&vs, vs + strlen(vs));
            value_str = vs;
        }

        if (current_route_index >= 0) {
            RouteEntry *route = &entry->routes[current_route_index];
            status = parse_route_field(route, key, value_str, line_no, errbuf, errbuf_size);
            if (status != CONFIG_OK) {
                break;
            }
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        status = parse_shim_entry_field(entry, key, value_str, line_no, errbuf, errbuf_size);
        if (status != CONFIG_OK) {
            break;
        }

        line = strtok_r(NULL, "\n", &saveptr);
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
static void render_shim_entry_body(const ShimEntry *entry, DynBuf *out,
                                    const char *route_header_prefix) {
    char line[64];
    {
        if (entry->source) {
            dynbuf_append_str(out, "source = ");
            append_escaped_string(out, entry->source);
            dynbuf_append_char(out, '\n');
        }
        if (entry->source_arg_count > 0) {
            dynbuf_append_str(out, "source_args = [");
            for (size_t j = 0; j < entry->source_arg_count; j++) {
                if (j > 0) {
                    dynbuf_append_str(out, ", ");
                }
                append_escaped_string(out, entry->source_args[j]);
            }
            dynbuf_append_str(out, "]\n");
        }

        if (entry->fallback) {
            dynbuf_append_str(out, "fallback = ");
            append_escaped_string(out, entry->fallback);
            dynbuf_append_char(out, '\n');
        }
        if (entry->fallback_arg_count > 0) {
            dynbuf_append_str(out, "fallback_args = [");
            for (size_t j = 0; j < entry->fallback_arg_count; j++) {
                if (j > 0) {
                    dynbuf_append_str(out, ", ");
                }
                append_escaped_string(out, entry->fallback_args[j]);
            }
            dynbuf_append_str(out, "]\n");
        }

        dynbuf_append_str(out, "policy = ");
        append_escaped_string(out, policy_to_string(entry->policy));
        dynbuf_append_char(out, '\n');

        if (entry->error_pattern_count > 0) {
            dynbuf_append_str(out, "error_patterns = [");
            for (size_t j = 0; j < entry->error_pattern_count; j++) {
                if (j > 0) {
                    dynbuf_append_str(out, ", ");
                }
                append_escaped_string(out, entry->error_patterns[j]);
            }
            dynbuf_append_str(out, "]\n");
        }

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

        if (entry->route_arg_count > 0) {
            dynbuf_append_str(out, "route_args = [");
            for (size_t j = 0; j < entry->route_arg_count; j++) {
                if (j > 0) {
                    dynbuf_append_str(out, ", ");
                }
                append_escaped_string(out, entry->route_args[j]);
            }
            dynbuf_append_str(out, "]\n");
        }

        if (entry->source_route_arg_count > 0) {
            dynbuf_append_str(out, "source_route_args = [");
            for (size_t j = 0; j < entry->source_route_arg_count; j++) {
                if (j > 0) {
                    dynbuf_append_str(out, ", ");
                }
                append_escaped_string(out, entry->source_route_args[j]);
            }
            dynbuf_append_str(out, "]\n");
        }

        if (entry->fallback_route_arg_count > 0) {
            dynbuf_append_str(out, "fallback_route_args = [");
            for (size_t j = 0; j < entry->fallback_route_arg_count; j++) {
                if (j > 0) {
                    dynbuf_append_str(out, ", ");
                }
                append_escaped_string(out, entry->fallback_route_args[j]);
            }
            dynbuf_append_str(out, "]\n");
        }

        if (entry->strip_matched_args) {
            dynbuf_append_str(out, "strip_matched_args = true\n");
        }

        if (entry->rewrite_from_count > 0) {
            dynbuf_append_str(out, "rewrite_from = [");
            for (size_t j = 0; j < entry->rewrite_from_count; j++) {
                if (j > 0) {
                    dynbuf_append_str(out, ", ");
                }
                append_escaped_string(out, entry->rewrite_from[j]);
            }
            dynbuf_append_str(out, "]\n");
        }
        if (entry->rewrite_to_count > 0) {
            dynbuf_append_str(out, "rewrite_to = [");
            for (size_t j = 0; j < entry->rewrite_to_count; j++) {
                if (j > 0) {
                    dynbuf_append_str(out, ", ");
                }
                append_escaped_string(out, entry->rewrite_to[j]);
            }
            dynbuf_append_str(out, "]\n");
        }

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

        dynbuf_append_str(out, "match = ");
        append_escaped_string(out, route->match);
        dynbuf_append_char(out, '\n');

        dynbuf_append_str(out, "command = ");
        append_escaped_string(out, route->command);
        dynbuf_append_char(out, '\n');

        if (route->arg_count > 0) {
            dynbuf_append_str(out, "args = [");
            for (size_t j = 0; j < route->arg_count; j++) {
                if (j > 0) {
                    dynbuf_append_str(out, ", ");
                }
                append_escaped_string(out, route->args[j]);
            }
            dynbuf_append_str(out, "]\n");
        }
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

    for (size_t i = 0; i < cfg->count; i++) {
        const ShimEntry *entry = &cfg->shims[i];
        dynbuf_append_char(out, '\n');
        dynbuf_append_str(out, "[shims.");
        dynbuf_append_str(out, entry->name);
        dynbuf_append_str(out, "]\n");
        render_shim_entry_body(entry, out, entry->name);
    }
}

ConfigStatus config_save(const Config *cfg, const char *path, char *errbuf, size_t errbuf_size) {
    char *dir = xstrdup(path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (!mkdir_p(dir)) {
            snprintf(errbuf, errbuf_size, "cannot create directory %s: %s", dir, strerror(errno));
            free(dir);
            return CONFIG_ERR_IO;
        }
    }
    free(dir);

    DynBuf out;
    dynbuf_init(&out);
    render_config(cfg, &out);

    /* 0600: config.toml controls which executables a shim actually runs,
     * so it's owner-only rather than the world-readable 0644 generated
     * shell files get -- enforced on every save (see write_file_atomic,
     * paths.c), not just at first creation, so it's self-healing even if
     * something else ever leaves the file at a weaker mode. */
    bool ok = write_file_atomic(path, out.data, out.len, 0600);
    dynbuf_free(&out);

    if (!ok) {
        snprintf(errbuf, errbuf_size, "cannot write %s: %s", path, strerror(errno));
        return CONFIG_ERR_IO;
    }

    return CONFIG_OK;
}

ConfigStatus config_save_split(const ShimEntry *entry, const char *path, char *errbuf,
                                size_t errbuf_size) {
    char *dir = xstrdup(path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (!mkdir_p(dir)) {
            snprintf(errbuf, errbuf_size, "cannot create directory %s: %s", dir, strerror(errno));
            free(dir);
            return CONFIG_ERR_IO;
        }
    }
    free(dir);

    DynBuf out;
    dynbuf_init(&out);
    render_shim_entry_body(entry, &out, NULL);

    /* 0600: same reasoning as config_save -- a split file is just as much
     * "which executable does this shim actually run" as an entry inside
     * config.toml itself. */
    bool ok = write_file_atomic(path, out.data, out.len, 0600);
    dynbuf_free(&out);

    if (!ok) {
        snprintf(errbuf, errbuf_size, "cannot write %s: %s", path, strerror(errno));
        return CONFIG_ERR_IO;
    }

    return CONFIG_OK;
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
            warn("failed to remove split config %s: %s", paths[i], strerror(errno));
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
        bool dup = false;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(names[j], symlink_names[i]) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            names[count++] = xstrdup(symlink_names[i]);
        }
        free(symlink_names[i]);
    }
    free(symlink_names);

    for (size_t i = 0; i < split_count; i++) {
        bool dup = false;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(names[j], split_names[i]) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            names[count++] = xstrdup(split_names[i]);
        }
        free(split_names[i]);
    }
    free(split_names);

    *out_count = count;
    return names;
}
