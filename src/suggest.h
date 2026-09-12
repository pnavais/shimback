#ifndef SHIMBACK_SUGGEST_H
#define SHIMBACK_SUGGEST_H

#include <stddef.h>

/* Suggests the closest match for `query` among `candidates`, by edit
 * (Levenshtein) distance -- the minimum number of single-character
 * insertions, deletions, or substitutions needed to turn one string into
 * the other. Catches the typo shapes a subsequence-only fuzzy match (e.g.
 * `fzf --filter`) can't: an inserted or substituted character, not just a
 * dropped one, since it doesn't require `query` to literally be containable
 * in order within the candidate (or vice versa). Returns a newly allocated
 * string (the closest candidate) only if it's close enough to be worth
 * suggesting -- scaled to the candidate's own length, so unrelated names
 * never get suggested -- or NULL otherwise. Callers should treat NULL as
 * "no hint to show", never as an error. No external dependency. */
char *fuzzy_suggest(const char *query, const char *const *candidates, size_t count);

#endif /* SHIMBACK_SUGGEST_H */
