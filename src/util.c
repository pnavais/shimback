#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"

/* NO_COLOR (https://no-color.org/) always wins; otherwise CLICOLOR_FORCE
 * (set and not "0") turns color on even when the stream isn't a terminal --
 * e.g. the installers, which capture `install`'s output to indent it. */
static bool color_forced(void) {
    const char *force = plat_getenv("CLICOLOR_FORCE");
    return force && force[0] != '\0' && strcmp(force, "0") != 0;
}

bool stdout_is_color(void) {
    return !plat_getenv("NO_COLOR") && (color_forced() || plat_isatty_stdout());
}

bool stderr_is_color(void) {
    return !plat_getenv("NO_COLOR") && (color_forced() || plat_isatty_stderr());
}

/* "shimback: <msg>\n" on `f`, the prefix colored when `colorize`, and the
 * message body also wrapped in `body_color` when that's non-NULL. */
static void vemit(FILE *f, bool colorize, const char *body_color, const char *fmt, va_list ap) {
    bool color_body = colorize && body_color;
    fprintf(f, "%sshimback:%s %s", colorize ? ANSI_PREFIX : "", colorize ? ANSI_RESET : "",
            color_body ? body_color : "");
    vfprintf(f, fmt, ap);
    fprintf(f, "%s\n", color_body ? ANSI_RESET : "");
}

void info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vemit(stdout, stdout_is_color(), NULL, fmt, ap);
    va_end(ap);
}

void recommend(const char *fmt, ...) {
    bool colorize = stdout_is_color();
    va_list ap;
    printf("%s", colorize ? ANSI_YELLOW : "");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("%s\n", colorize ? ANSI_RESET : "");
}

_Noreturn void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vemit(stderr, stderr_is_color(), NULL, fmt, ap);
    va_end(ap);
    exit(1);
}

void warn_colored(const char *color, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vemit(stderr, stderr_is_color(), color, fmt, ap);
    va_end(ap);
}

void warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vemit(stderr, stderr_is_color(), NULL, fmt, ap);
    va_end(ap);
}

void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p && size > 0) {
        die("out of memory");
    }
    return p;
}

void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (!p && size > 0) {
        die("out of memory");
    }
    return p;
}

char *xstrdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = xmalloc(len);
    memcpy(copy, s, len);
    return copy;
}

char *xstrndup(const char *s, size_t n) {
    size_t len = strlen(s);
    if (len < n) {
        n = len;
    }
    char *copy = xmalloc(n + 1);
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

void dynbuf_init(DynBuf *buf) {
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static void dynbuf_reserve(DynBuf *buf, size_t extra) {
    size_t needed = buf->len + extra;
    if (needed <= buf->cap) {
        return;
    }
    size_t new_cap = buf->cap == 0 ? 256 : buf->cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    buf->data = xrealloc(buf->data, new_cap);
    buf->cap = new_cap;
}

void dynbuf_append(DynBuf *buf, const char *data, size_t len) {
    if (len == 0) {
        return;
    }
    dynbuf_reserve(buf, len);
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
}

void dynbuf_append_str(DynBuf *buf, const char *str) {
    dynbuf_append(buf, str, strlen(str));
}

void dynbuf_append_char(DynBuf *buf, char c) {
    dynbuf_append(buf, &c, 1);
}

const char *dynbuf_cstr(DynBuf *buf) {
    dynbuf_reserve(buf, 1);
    buf->data[buf->len] = '\0';
    return buf->data;
}

void dynbuf_free(DynBuf *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void strvec_init(StrVec *v) {
    v->items = NULL;
    v->count = 0;
    v->cap = 0;
}

void strvec_push(StrVec *v, char *owned_str) {
    if (v->count == v->cap) {
        size_t new_cap = v->cap == 0 ? 4 : v->cap * 2;
        v->items = xrealloc(v->items, new_cap * sizeof(char *));
        v->cap = new_cap;
    }
    v->items[v->count++] = owned_str;
}

bool read_yes_no(bool *eof) {
    fflush(stdout);
    char line[64];
    bool got = fgets(line, sizeof(line), stdin) != NULL;
    if (eof) {
        *eof = !got;
    }
    if (!got) {
        printf("\n");
        return false;
    }
    return line[0] == 'y' || line[0] == 'Y';
}

FileReadStatus read_open_file(FILE *f, char **out, size_t *out_len, long *expected_len) {
    long size = -1;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        int saved_errno = errno;
        fclose(f);
        errno = saved_errno;
        return FILE_READ_SEEK_FAILED;
    }
    char *buf = xmalloc((size_t)size + 1);
    size_t n = fread(buf, 1, (size_t)size, f);
    bool read_error = ferror(f) || n != (size_t)size;
    fclose(f);
    *out_len = n;
    *expected_len = size;
    if (read_error) {
        free(buf);
        return FILE_READ_SHORT;
    }
    buf[n] = '\0';
    *out = buf;
    return FILE_READ_OK;
}

void str_array_free(char **a, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(a[i]);
    }
    free(a);
}

void strvec_free(StrVec *v) {
    str_array_free(v->items, v->count);
    v->items = NULL;
    v->count = 0;
    v->cap = 0;
}

bool is_literal_pattern(const char *pattern) {
    return strpbrk(pattern, "*?[") == NULL;
}

/* If `p` points at an unescaped '[' that opens a *terminated* bracket
 * expression (a matching ']' exists before the pattern's end), returns a
 * pointer to that closing ']'. Otherwise NULL, in which case the caller
 * treats the '[' as an ordinary literal character. A ']' that is the
 * first content character (right after '[', or right after '[!'/'[^') is
 * itself a literal member, not the closing bracket -- standard glob(7)
 * convention, which is what lets "[]abc]" match ']', 'a', 'b', or 'c'. */
static const char *glob_class_end(const char *p) {
    const char *scan = p + 1;
    if (*scan == '!' || *scan == '^') {
        scan++;
    }
    if (*scan == ']') {
        scan++;
    }
    while (*scan != '\0' && *scan != ']') {
        scan++;
    }
    return (*scan == ']') ? scan : NULL;
}

/* Tests `c` against the bracket expression delimited by `p` ('[') and
 * `class_end` (its ']', from glob_class_end) -- "[!...]"/"[^...]" negate;
 * "a-z"-style ranges are supported; anything else inside is a literal
 * member. */
static bool glob_class_matches(const char *p, const char *class_end, char c) {
    const char *scan = p + 1;
    bool negate = false;
    if (*scan == '!' || *scan == '^') {
        negate = true;
        scan++;
    }
    bool found = false;
    while (scan < class_end) {
        if (scan + 2 < class_end && scan[1] == '-') {
            unsigned char lo = (unsigned char)scan[0];
            unsigned char hi = (unsigned char)scan[2];
            if ((unsigned char)c >= lo && (unsigned char)c <= hi) {
                found = true;
            }
            scan += 3;
        } else {
            if (*scan == c) {
                found = true;
            }
            scan++;
        }
    }
    return negate ? !found : found;
}

bool route_pattern_match(const char *pattern, const char *token) {
    const char *p = pattern;
    const char *t = token;
    const char *star_p = NULL;
    const char *star_t = NULL;

    while (*t != '\0') {
        if (*p == '?') {
            p++;
            t++;
            continue;
        }
        if (*p == '[') {
            const char *class_end = glob_class_end(p);
            if (class_end != NULL) {
                if (glob_class_matches(p, class_end, *t)) {
                    p = class_end + 1;
                    t++;
                    continue;
                }
            } else if (*p == *t) {
                p++;
                t++;
                continue;
            }
        } else if (*p == '*') {
            star_p = p++;
            star_t = t;
            continue;
        } else if (*p == *t) {
            p++;
            t++;
            continue;
        }

        if (star_p == NULL) {
            return false;
        }
        p = star_p + 1;
        t = ++star_t;
    }
    while (*p == '*') {
        p++;
    }
    return *p == '\0';
}

void glob_literal_prefix_suffix(const char *pattern, size_t *prefix_len, size_t *suffix_start) {
    size_t len = strlen(pattern);
    *prefix_len = len;
    *suffix_start = len;
    bool seen_meta = false;
    size_t i = 0;
    while (i < len) {
        if (pattern[i] == '*' || pattern[i] == '?') {
            if (!seen_meta) {
                *prefix_len = i;
                seen_meta = true;
            }
            *suffix_start = i + 1;
            i++;
        } else if (pattern[i] == '[') {
            const char *class_end = glob_class_end(pattern + i);
            if (class_end != NULL) {
                if (!seen_meta) {
                    *prefix_len = i;
                    seen_meta = true;
                }
                size_t end_idx = (size_t)(class_end - pattern);
                *suffix_start = end_idx + 1;
                i = end_idx + 1;
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
}

/* Like route_pattern_match, but succeeds the moment `pattern` is fully
 * consumed, regardless of how much of `text` is left over -- i.e. "does
 * `pattern` match some PREFIX of `text`," not the whole of it. This is
 * the building block pattern_found_in uses to ask "does `pattern` occur
 * somewhere in `text`," by trying every starting offset -- the same
 * O(n)-outer-loop, early-mismatch-bailout shape a naive substring search
 * already has, just with the inner character comparison replaced by a
 * full glob match. */
static bool pattern_match_prefix(const char *pattern, const char *text) {
    const char *p = pattern;
    const char *t = text;
    const char *star_p = NULL;
    const char *star_t = NULL;

    while (*t != '\0') {
        if (*p == '\0') {
            return true;
        }
        if (*p == '?') {
            p++;
            t++;
            continue;
        }
        if (*p == '[') {
            const char *class_end = glob_class_end(p);
            if (class_end != NULL) {
                if (glob_class_matches(p, class_end, *t)) {
                    p = class_end + 1;
                    t++;
                    continue;
                }
            } else if (*p == *t) {
                p++;
                t++;
                continue;
            }
        } else if (*p == '*') {
            star_p = p++;
            star_t = t;
            continue;
        } else if (*p == *t) {
            p++;
            t++;
            continue;
        }

        if (star_p == NULL) {
            return false;
        }
        p = star_p + 1;
        t = ++star_t;
    }
    while (*p == '*') {
        p++;
    }
    return *p == '\0';
}

bool pattern_found_in(const char *pattern, const char *haystack) {
    for (const char *h = haystack;; h++) {
        if (pattern_match_prefix(pattern, h)) {
            return true;
        }
        if (*h == '\0') {
            return false;
        }
    }
}

/* A lowercased copy of `s`, byte-wise (same tolower() idiom as the rest of
 * this file uses elsewhere) -- lets heuristic's error-pattern matching
 * stay case-insensitive by folding both sides once before a single
 * case-sensitive pattern_found_in call, instead of needing a second,
 * case-folding variant of the glob matcher itself. Caller frees. */
char *str_lower_dup(const char *s) {
    size_t len = strlen(s);
    char *out = xmalloc(len + 1);
    for (size_t i = 0; i < len; i++) {
        out[i] = (char)tolower((unsigned char)s[i]);
    }
    out[len] = '\0';
    return out;
}

bool str_array_eq(char *const *a, size_t a_count, char *const *b, size_t b_count) {
    if (a_count != b_count) {
        return false;
    }
    for (size_t i = 0; i < a_count; i++) {
        if (strcmp(a[i], b[i]) != 0) {
            return false;
        }
    }
    return true;
}

bool parse_nonneg_int(const char *s, int *out) {
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

bool parse_size_bytes(const char *s, size_t *out) {
    if (s[0] == '\0' || !isdigit((unsigned char)s[0])) {
        return false;
    }
    char *end;
    errno = 0;
    unsigned long long value = strtoull(s, &end, 10);
    if (errno == ERANGE) {
        /* strtoull() clamps to ULLONG_MAX on overflow instead of failing --
         * on a 64-bit system that's frequently == SIZE_MAX, which would
         * otherwise slip straight past the multiplier-overflow check below
         * (ULLONG_MAX > SIZE_MAX / 1 is false) and get accepted as a huge,
         * unintended capture_limit. */
        return false;
    }
    const char *suffix = end;

    char buf[8];
    size_t suffix_len = strlen(suffix);
    if (suffix_len >= sizeof(buf)) {
        return false;
    }
    for (size_t i = 0; i < suffix_len; i++) {
        buf[i] = (char)tolower((unsigned char)suffix[i]);
    }
    buf[suffix_len] = '\0';

    unsigned long long multiplier;
    if (buf[0] == '\0' || strcmp(buf, "b") == 0) {
        multiplier = 1;
    } else if (strcmp(buf, "k") == 0 || strcmp(buf, "kb") == 0) {
        multiplier = 1000ULL;
    } else if (strcmp(buf, "ki") == 0 || strcmp(buf, "kib") == 0) {
        multiplier = 1024ULL;
    } else if (strcmp(buf, "m") == 0 || strcmp(buf, "mb") == 0) {
        multiplier = 1000ULL * 1000;
    } else if (strcmp(buf, "mi") == 0 || strcmp(buf, "mib") == 0) {
        multiplier = 1024ULL * 1024;
    } else if (strcmp(buf, "g") == 0 || strcmp(buf, "gb") == 0) {
        multiplier = 1000ULL * 1000 * 1000;
    } else if (strcmp(buf, "gi") == 0 || strcmp(buf, "gib") == 0) {
        multiplier = 1024ULL * 1024 * 1024;
    } else {
        return false;
    }

    if (value != 0 && value > (unsigned long long)SIZE_MAX / multiplier) {
        return false; /* would overflow size_t */
    }
    *out = (size_t)(value * multiplier);
    return true;
}
