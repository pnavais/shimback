#ifndef SHIMBACK_CONFIG_H
#define SHIMBACK_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/* POLICY_EXIT_CODE: fall back on any non-zero exit (the default).
 * POLICY_HEURISTIC: fall back only when stderr matches an error_pattern.
 * POLICY_EXIT_CODE_MATCH: fall back only when the exit code is one of
 * exit_codes -- unlike POLICY_EXIT_CODE, other non-zero exits surface as-is.
 * POLICY_ROUTE_ARGS: not a fallback-on-failure policy at all -- picks source
 * or fallback up front, before running either, based on whether any of the
 * invocation's arguments match one of route_args (fallback if so, source if
 * not), then runs only that one with live/inherited stdio.
 * POLICY_REWRITE: not a fallback mechanism either -- always runs source,
 * after rewriting any argument matching one of rewrite_from into the
 * corresponding rewrite_to (an alias/macro mechanism, e.g. "--full" ->
 * "-ltrah"), with live/inherited stdio. fallback is never used, and so is
 * the one policy where it's optional.
 * POLICY_SPLIT_ARGS: also picks source or fallback up front rather than
 * falling back on failure, but two-sided: source_route_args and
 * fallback_route_args are separate sets, and a side only "wins" when every
 * one of its own args is present in the invocation (a full match). If both
 * sides fully match (one's args are a subset of the other's), the side
 * requiring more matched args -- the more discriminating one -- wins;
 * exactly tied counts favor source. If neither side fully matches, this
 * behaves like POLICY_EXIT_CODE instead: run source, and on any failure,
 * fall back. Runs with live/inherited stdio only when a side actually won
 * outright; the "neither matched" case goes through the normal captured
 * trial run like every fallback-on-failure policy.
 *
 * Independent of all six: source_args/fallback_args (see ShimEntry) let
 * any shim, under any policy, also work as a plain alias with flags baked
 * in -- e.g. source "ls" with source_args ["-ltrah"] always runs
 * "ls -ltrah <whatever else was typed>", the same way `alias cools='ls
 * -ltrah'` would. */
typedef enum {
    POLICY_EXIT_CODE,
    POLICY_HEURISTIC,
    POLICY_EXIT_CODE_MATCH,
    POLICY_ROUTE_ARGS,
    POLICY_REWRITE,
    POLICY_SPLIT_ARGS,
} Policy;

typedef struct {
    char *name;               /* owned; never NULL */
    char *source;              /* owned; NULL means "auto" (resolve from PATH at dispatch time) */
    char *fallback;             /* owned; required, except NULL is allowed for POLICY_REWRITE */
    Policy policy;
    char **source_args;         /* owned array of owned strings; fixed arguments, prepended
                                  * before whatever the shim was actually invoked with, every
                                  * time source runs -- e.g. source "ls" plus source_args
                                  * ["-l","-t","-r","-a","-h"] makes the shim also work as a
                                  * regular alias (like `alias cools='ls -ltrah'`), on top of
                                  * whatever its policy already does. Independent of policy;
                                  * NULL/0 if none configured. */
    size_t source_arg_count;
    char **fallback_args;       /* same, prepended before fallback's own invocation whenever
                                  * fallback runs (meaningless -- and always empty -- for
                                  * POLICY_REWRITE, which never runs fallback at all). */
    size_t fallback_arg_count;
    char **error_patterns;      /* owned array of owned strings; NULL if none */
    size_t error_pattern_count;
    int *exit_codes;            /* owned array; only meaningful for POLICY_EXIT_CODE_MATCH */
    size_t exit_code_count;
    char **route_args;           /* owned array of owned strings; only meaningful for
                                   * POLICY_ROUTE_ARGS */
    size_t route_arg_count;
    char **source_route_args;    /* owned array of owned strings; only meaningful for
                                   * POLICY_SPLIT_ARGS -- source's own discriminating args,
                                   * separate from fallback_route_args below. */
    size_t source_route_arg_count;
    char **fallback_route_args;  /* owned array of owned strings; only meaningful for
                                   * POLICY_SPLIT_ARGS -- fallback's own discriminating args. */
    size_t fallback_route_arg_count;
    bool strip_matched_args;     /* meaningful for POLICY_ROUTE_ARGS and POLICY_SPLIT_ARGS:
                                   * remove whichever route args actually matched before
                                   * forwarding to whichever of source/fallback won */
    char **rewrite_from;         /* owned array of owned strings; only meaningful for
                                   * POLICY_REWRITE. Parallel to rewrite_to: rewrite_from[i]
                                   * -> rewrite_to[i]. Tracked as two separate counts (not
                                   * one shared one) because config_load parses the two
                                   * TOML arrays independently and must be able to detect
                                   * a hand-edited config where they end up different
                                   * lengths, rather than silently indexing past the
                                   * shorter one. config_load rejects any mismatch, so
                                   * everywhere past that point the two counts are equal. */
    size_t rewrite_from_count;
    char **rewrite_to;           /* owned array of owned strings; each may contain spaces,
                                   * expanding to multiple forwarded arguments. */
    size_t rewrite_to_count;
    bool diagnostic;
    bool force;                  /* set by `add --force`: source/fallback were allowed to not
                                   * exist yet at add time. `doctor` skips its "does it exist
                                   * and is executable" check for whichever of them still
                                   * doesn't, rather than reporting it as broken; has no effect
                                   * on dispatch, which always checks for real at invocation
                                   * time regardless. */
} ShimEntry;

typedef struct {
    int version;
    ShimEntry *shims;   /* owned dynamic array */
    size_t count;
    size_t cap;
} Config;

typedef enum {
    CONFIG_OK = 0,
    CONFIG_ERR_IO,
    CONFIG_ERR_PARSE,
    CONFIG_ERR_VALIDATION,
} ConfigStatus;

void config_init(Config *cfg);

/* Loads `path` into `cfg` (which must already be config_init'd, or freshly
 * zeroed). A missing file is treated as a valid, empty (version=1) config,
 * not an error. On CONFIG_ERR_* a human-readable message is written into
 * errbuf. */
ConfigStatus config_load(const char *path, Config *cfg, char *errbuf, size_t errbuf_size);

/* Writes `cfg` to `path` atomically (temp file + rename), creating parent
 * directories as needed. */
ConfigStatus config_save(const Config *cfg, const char *path, char *errbuf, size_t errbuf_size);

/* Read-only lookup; NULL if not found. Only valid to call when no further
 * config_upsert calls are pending (the returned pointer can be invalidated
 * by a later upsert that grows the backing array). */
ShimEntry *config_find(Config *cfg, const char *name);

/* Finds or creates (with name set and defaults: policy=exit-code,
 * diagnostic=false, source=NULL) the entry for `name`, returning its index
 * (stable even if the array reallocates on a later call -- re-derive the
 * pointer via &cfg->shims[index] rather than caching it). */
size_t config_upsert(Config *cfg, const char *name);

/* Removes the entry for `name`, if present. Returns true if something was
 * removed. */
bool config_remove(Config *cfg, const char *name);

void shim_entry_free(ShimEntry *entry);
void config_free(Config *cfg);

const char *policy_to_string(Policy p);
bool policy_from_string(const char *s, Policy *out);

#endif /* SHIMBACK_CONFIG_H */
