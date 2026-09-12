#include "suggest.h"

#include <stdio.h>
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

/* Finds the candidate closest to `query` by edit distance, and how many
 * candidates tie for that same distance (always >= 1 when a candidate is
 * returned at all). Shared by fuzzy_suggest (which doesn't care about
 * ties -- a hint is just shown, never acted on) and fuzzy_suggest_unique
 * (which does, since it's used to act automatically). */
static const char *closest_match(const char *query, const char *const *candidates, size_t count,
                                  size_t *out_dist, size_t *out_ties) {
    if (count == 0 || query[0] == '\0') {
        return NULL;
    }

    const char *best = NULL;
    size_t best_dist = (size_t)-1;
    size_t ties = 0;
    for (size_t i = 0; i < count; i++) {
        size_t dist = edit_distance(query, candidates[i]);
        if (dist < best_dist) {
            best_dist = dist;
            best = candidates[i];
            ties = 1;
        } else if (dist == best_dist) {
            ties++;
        }
    }
    if (!best) {
        return NULL;
    }
    *out_dist = best_dist;
    *out_ties = ties;
    return best;
}

/* Scaled to the candidate's own length so short names still tolerate a
 * typo (one edit in three characters) without unrelated longer names
 * getting suggested for a query that barely resembles them. */
static size_t threshold_for(const char *candidate) {
    size_t threshold = strlen(candidate) / 3;
    return threshold < 1 ? 1 : threshold;
}

char *fuzzy_suggest(const char *query, const char *const *candidates, size_t count) {
    size_t dist, ties;
    const char *best = closest_match(query, candidates, count, &dist, &ties);
    if (!best || dist > threshold_for(best)) {
        return NULL;
    }
    return xstrdup(best);
}

char *fuzzy_suggest_unique(const char *query, const char *const *candidates, size_t count) {
    size_t dist, ties;
    const char *best = closest_match(query, candidates, count, &dist, &ties);
    if (!best || ties > 1 || dist > threshold_for(best)) {
        return NULL;
    }
    return xstrdup(best);
}

void print_suggestion_hint(const char *suggestion) {
    bool colorize = stderr_is_color();
    fprintf(stderr, "%sshimback: did you mean '%s'?%s\n", colorize ? ANSI_YELLOW : "",
            suggestion, colorize ? ANSI_RESET : "");
}
