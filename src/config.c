#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "paths.h"
#include "util.h"

void config_init(Config *cfg) {
    cfg->version = 1;
    cfg->shims = NULL;
    cfg->count = 0;
    cfg->cap = 0;
}

const char *policy_to_string(Policy p) {
    switch (p) {
        case POLICY_HEURISTIC: return "heuristic";
        case POLICY_EXIT_CODE_MATCH: return "exit-code-match";
        case POLICY_ROUTE_ARGS: return "route-args";
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
    return false;
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
    for (size_t i = 0; i < entry->error_pattern_count; i++) {
        free(entry->error_patterns[i]);
    }
    free(entry->error_patterns);
    free(entry->exit_codes);
    for (size_t i = 0; i < entry->route_arg_count; i++) {
        free(entry->route_args[i]);
    }
    free(entry->route_args);
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
    fclose(f);
    contents[n] = '\0';

    ssize_t current_index = -1; /* -1 = top-level, no [shims.x] section yet */
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

        if (*trimmed == '[') {
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
                current_index = (ssize_t)config_upsert(cfg, header + strlen(prefix));
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

        if (current_index < 0) {
            if (strcmp(key, "version") == 0) {
                cfg->version = (int)strtol(value_str, NULL, 10);
            } else {
                warn("config: line %d: unknown top-level key '%s', ignoring", line_no, key);
            }
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        ShimEntry *entry = &cfg->shims[current_index];
        const char *cursor = value_str;

        if (strcmp(key, "source") == 0) {
            char *v = parse_quoted_string(&cursor);
            if (!v) {
                snprintf(errbuf, errbuf_size, "line %d: expected a string for 'source'", line_no);
                status = CONFIG_ERR_PARSE;
                break;
            }
            free(entry->source);
            entry->source = v;
        } else if (strcmp(key, "fallback") == 0) {
            char *v = parse_quoted_string(&cursor);
            if (!v) {
                snprintf(errbuf, errbuf_size, "line %d: expected a string for 'fallback'", line_no);
                status = CONFIG_ERR_PARSE;
                break;
            }
            free(entry->fallback);
            entry->fallback = v;
        } else if (strcmp(key, "policy") == 0) {
            char *v = parse_quoted_string(&cursor);
            if (!v || !policy_from_string(v, &entry->policy)) {
                snprintf(errbuf, errbuf_size,
                         "line %d: 'policy' must be \"exit-code\", \"heuristic\", "
                         "\"exit-code-match\", or \"route-args\"",
                         line_no);
                free(v);
                status = CONFIG_ERR_PARSE;
                break;
            }
            free(v);
        } else if (strcmp(key, "exit_codes") == 0) {
            int *items = NULL;
            size_t count = 0;
            if (!parse_int_array(&cursor, &items, &count)) {
                snprintf(errbuf, errbuf_size, "line %d: malformed 'exit_codes' array (values "
                                               "must be integers 0-255)",
                         line_no);
                status = CONFIG_ERR_PARSE;
                break;
            }
            free(entry->exit_codes);
            entry->exit_codes = items;
            entry->exit_code_count = count;
        } else if (strcmp(key, "error_patterns") == 0) {
            StrVec vec;
            strvec_init(&vec);
            if (!parse_string_array(&cursor, &vec)) {
                strvec_free(&vec);
                snprintf(errbuf, errbuf_size, "line %d: malformed 'error_patterns' array", line_no);
                status = CONFIG_ERR_PARSE;
                break;
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
            if (!parse_string_array(&cursor, &vec)) {
                strvec_free(&vec);
                snprintf(errbuf, errbuf_size, "line %d: malformed 'route_args' array", line_no);
                status = CONFIG_ERR_PARSE;
                break;
            }
            for (size_t i = 0; i < entry->route_arg_count; i++) {
                free(entry->route_args[i]);
            }
            free(entry->route_args);
            entry->route_args = vec.items;
            entry->route_arg_count = vec.count;
        } else if (strcmp(key, "diagnostic") == 0) {
            if (strcmp(value_str, "true") == 0) {
                entry->diagnostic = true;
            } else if (strcmp(value_str, "false") == 0) {
                entry->diagnostic = false;
            } else {
                snprintf(errbuf, errbuf_size, "line %d: 'diagnostic' must be true or false", line_no);
                status = CONFIG_ERR_PARSE;
                break;
            }
        } else if (strcmp(key, "strip_matched_args") == 0) {
            if (strcmp(value_str, "true") == 0) {
                entry->strip_matched_args = true;
            } else if (strcmp(value_str, "false") == 0) {
                entry->strip_matched_args = false;
            } else {
                snprintf(errbuf, errbuf_size,
                         "line %d: 'strip_matched_args' must be true or false", line_no);
                status = CONFIG_ERR_PARSE;
                break;
            }
        } else {
            warn("config: line %d: unknown key '%s' in [shims.%s], ignoring", line_no, key,
                 entry->name);
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(contents);
    if (status != CONFIG_OK) {
        return status;
    }

    for (size_t i = 0; i < cfg->count; i++) {
        ShimEntry *entry = &cfg->shims[i];
        if (!entry->fallback) {
            snprintf(errbuf, errbuf_size, "shim '%s' is missing a required 'fallback'", entry->name);
            return CONFIG_ERR_VALIDATION;
        }
        if (entry->policy == POLICY_HEURISTIC && entry->error_pattern_count == 0) {
            snprintf(errbuf, errbuf_size,
                     "shim '%s' uses policy \"heuristic\" but has no error_patterns", entry->name);
            return CONFIG_ERR_VALIDATION;
        }
        if (entry->policy == POLICY_EXIT_CODE_MATCH && entry->exit_code_count == 0) {
            snprintf(errbuf, errbuf_size,
                     "shim '%s' uses policy \"exit-code-match\" but has no exit_codes",
                     entry->name);
            return CONFIG_ERR_VALIDATION;
        }
        if (entry->policy == POLICY_ROUTE_ARGS && entry->route_arg_count == 0) {
            snprintf(errbuf, errbuf_size,
                     "shim '%s' uses policy \"route-args\" but has no route_args", entry->name);
            return CONFIG_ERR_VALIDATION;
        }
    }

    return CONFIG_OK;
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

static void render_config(const Config *cfg, DynBuf *out) {
    char line[64];
    snprintf(line, sizeof(line), "version = %d\n", cfg->version);
    dynbuf_append_str(out, line);

    for (size_t i = 0; i < cfg->count; i++) {
        const ShimEntry *entry = &cfg->shims[i];
        dynbuf_append_char(out, '\n');
        dynbuf_append_str(out, "[shims.");
        dynbuf_append_str(out, entry->name);
        dynbuf_append_str(out, "]\n");

        if (entry->source) {
            dynbuf_append_str(out, "source = ");
            append_escaped_string(out, entry->source);
            dynbuf_append_char(out, '\n');
        }

        dynbuf_append_str(out, "fallback = ");
        append_escaped_string(out, entry->fallback);
        dynbuf_append_char(out, '\n');

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

        if (entry->strip_matched_args) {
            dynbuf_append_str(out, "strip_matched_args = true\n");
        }

        if (entry->diagnostic) {
            dynbuf_append_str(out, "diagnostic = true\n");
        }
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

    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        snprintf(errbuf, errbuf_size, "cannot create %s: %s", tmp_path, strerror(errno));
        dynbuf_free(&out);
        return CONFIG_ERR_IO;
    }
    size_t written = fwrite(out.data, 1, out.len, f);
    bool ok = written == out.len;
    if (ok) {
        ok = fflush(f) == 0;
    }
    fclose(f);
    dynbuf_free(&out);

    if (!ok) {
        snprintf(errbuf, errbuf_size, "cannot write %s: %s", tmp_path, strerror(errno));
        unlink(tmp_path);
        return CONFIG_ERR_IO;
    }

    if (rename(tmp_path, path) != 0) {
        snprintf(errbuf, errbuf_size, "cannot replace %s: %s", path, strerror(errno));
        unlink(tmp_path);
        return CONFIG_ERR_IO;
    }

    return CONFIG_OK;
}
