#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

bool stdout_is_color(void) {
    return !getenv("NO_COLOR") && isatty(STDOUT_FILENO);
}

bool stderr_is_color(void) {
    return !getenv("NO_COLOR") && isatty(STDERR_FILENO);
}

void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "shimback: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

void warn(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "shimback: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
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

void strvec_free(StrVec *v) {
    for (size_t i = 0; i < v->count; i++) {
        free(v->items[i]);
    }
    free(v->items);
    v->items = NULL;
    v->count = 0;
    v->cap = 0;
}

const char *str_casestr(const char *haystack, const char *needle) {
    if (*needle == '\0') {
        return haystack;
    }
    for (const char *h = haystack; *h != '\0'; h++) {
        const char *hp = h;
        const char *np = needle;
        while (*hp != '\0' && *np != '\0' &&
               tolower((unsigned char)*hp) == tolower((unsigned char)*np)) {
            hp++;
            np++;
        }
        if (*np == '\0') {
            return h;
        }
    }
    return NULL;
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

pid_t xwaitpid(pid_t pid, int *status) {
    pid_t rc;
    do {
        rc = waitpid(pid, status, 0);
    } while (rc < 0 && errno == EINTR);
    return rc;
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
         * unintended capture_limit (see review.md). */
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
