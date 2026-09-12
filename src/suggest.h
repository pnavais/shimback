#ifndef SHIMBACK_SUGGEST_H
#define SHIMBACK_SUGGEST_H

#include <stddef.h>

/* Suggests the closest match for `query` among `candidates`, using `fzf
 * --filter` (fzf's non-interactive fuzzy-filter mode, which just ranks and
 * prints matches instead of launching its usual interactive UI) if `fzf` is
 * found on $PATH. Returns a newly allocated string (the top-ranked match)
 * or NULL if fzf isn't available, or found no match at all -- callers
 * should treat NULL as "no hint to show", never as an error. */
char *fuzzy_suggest(const char *query, const char *const *candidates, size_t count);

#endif /* SHIMBACK_SUGGEST_H */
