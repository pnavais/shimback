#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool stdout_is_color(void) {
    return !getenv("NO_COLOR") && isatty(STDOUT_FILENO);
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
