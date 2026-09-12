#include "suggest.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Iterative Levenshtein distance, using two rolling rows instead of a full
 * (strlen(a)+1) x (strlen(b)+1) table -- only the previous row is ever
 * needed to compute the current one. */
static size_t edit_distance(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);

    size_t *prev = xmalloc((lb + 1) * sizeof(size_t));
    size_t *curr = xmalloc((lb + 1) * sizeof(size_t));
    for (size_t j = 0; j <= lb; j++) {
        prev[j] = j;
    }

    for (size_t i = 1; i <= la; i++) {
        curr[0] = i;
        for (size_t j = 1; j <= lb; j++) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            size_t deletion = prev[j] + 1;
            size_t insertion = curr[j - 1] + 1;
            size_t substitution = prev[j - 1] + cost;
            size_t best = deletion < insertion ? deletion : insertion;
            curr[j] = best < substitution ? best : substitution;
        }
        size_t *tmp = prev;
        prev = curr;
        curr = tmp;
    }

    size_t result = prev[lb];
    free(prev);
    free(curr);
    return result;
}

char *fuzzy_suggest(const char *query, const char *const *candidates, size_t count) {
    if (count == 0 || query[0] == '\0') {
        return NULL;
    }

    const char *best = NULL;
    size_t best_dist = (size_t)-1;
    for (size_t i = 0; i < count; i++) {
        size_t dist = edit_distance(query, candidates[i]);
        if (dist < best_dist) {
            best_dist = dist;
            best = candidates[i];
        }
    }
    if (!best) {
        return NULL;
    }

    /* Scaled to the candidate's own length so short names still tolerate a
     * typo (one edit in three characters) without unrelated longer names
     * getting suggested for a query that barely resembles them. */
    size_t threshold = strlen(best) / 3;
    if (threshold < 1) {
        threshold = 1;
    }
    if (best_dist > threshold) {
        return NULL;
    }
    return xstrdup(best);
}
