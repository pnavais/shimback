#ifndef SHIMBACK_UTIL_H
#define SHIMBACK_UTIL_H

#include <stdbool.h>
#include <stddef.h>

#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_DIM "\033[2m"
#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_CYAN "\033[36m"
#define ANSI_MAGENTA "\033[35m"

/* Whether to color stdout output: a real terminal, unless NO_COLOR
 * (https://no-color.org/) is set. */
bool stdout_is_color(void);

/* Prints "shimback: <msg>" to stderr and exits with status 1. Never returns.
 * Reserved for unrecoverable CLI/validation errors -- never call this from
 * dispatch's success/fallback paths, which have their own precise exit codes. */
void die(const char *fmt, ...);

/* Prints "shimback: <msg>" to stderr. Does not exit. */
void warn(const char *fmt, ...);

/* Allocation wrappers that die() on OOM, so call sites never need to check. */
void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);

/* A growable byte buffer, reused by config serialization, shell rc-file
 * rewriting, and dispatch's captured child output. Not assumed to be a
 * NUL-terminated C string unless the caller explicitly appends one; dynbuf_cstr
 * ensures a trailing NUL without counting it in len. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} DynBuf;

void dynbuf_init(DynBuf *buf);
void dynbuf_append(DynBuf *buf, const char *data, size_t len);
void dynbuf_append_str(DynBuf *buf, const char *str);
void dynbuf_append_char(DynBuf *buf, char c);
/* Ensures the buffer is NUL-terminated without including the NUL in len. */
const char *dynbuf_cstr(DynBuf *buf);
void dynbuf_free(DynBuf *buf);

/* A growable array of owned strings, e.g. for repeated --error-pattern flags. */
typedef struct {
    char **items;
    size_t count;
    size_t cap;
} StrVec;

void strvec_init(StrVec *v);
void strvec_push(StrVec *v, char *owned_str);
void strvec_free(StrVec *v);

/* Case-insensitive substring search, hand-rolled to avoid relying on
 * strcasestr's availability/feature-test-macro quirks across platforms. */
const char *str_casestr(const char *haystack, const char *needle);

#endif /* SHIMBACK_UTIL_H */
