#ifndef SHIMBACK_CONFIG_H
#define SHIMBACK_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/* Global defaults for the trial-run capture cutover (see ShimEntry's
 * capture_timeout_ms/capture_limit_bytes and dispatch.c's run_captured):
 * how long, and how much output, a source's invisible trial run is allowed
 * to accumulate before shimback gives up on ever hiding/falling back on it
 * and switches to relaying it live instead. 2s comfortably covers the
 * fast local failures (bad flag, missing file, ...) this mechanism exists
 * for; 8MiB is far more than any realistic error message while still
 * bounding worst-case memory for a source that turns out to be
 * long-running or unexpectedly chatty. */
#define SHIMBACK_DEFAULT_CAPTURE_TIMEOUT_MS 2000
#define SHIMBACK_DEFAULT_CAPTURE_LIMIT_BYTES (8u * 1024 * 1024)

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
 * POLICY_ROUTE_MAP: like POLICY_ROUTE_ARGS, but generalized from one
 * fallback to an ordered list of `routes` (see RouteEntry): the first
 * route whose `match` exactly equals one of the invocation's arguments
 * runs, with its own fixed `args` prepended to whatever's forwarded. No
 * route matching falls through to source, exactly like route-args' own
 * source/fallback default. Unlike route-args, two different routes may
 * point at the very same command with different `args` (see RouteEntry).
 *
 * Independent of all seven: source_args/fallback_args (see ShimEntry) let
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
    POLICY_ROUTE_MAP,
} Policy;

/* One entry in a POLICY_ROUTE_MAP shim's `routes` list. Unlike
 * route-args' single fallback, `command` isn't required to be unique
 * across routes -- the same command can appear in two routes with
 * different `args`, which is the whole point of this policy over
 * route-args (see config_load's validate_shim_entry for the one thing
 * that *is* rejected: two routes with an identical (command, args)
 * pair, which would just be dead, unreachable duplication). */
typedef struct {
    char *match;      /* owned; exact argv token that triggers this route */
    char *command;    /* owned; resolved, absolute path -- same treatment
                        * as source/fallback (see add.c) */
    char **args;      /* owned array of owned strings; fixed args always
                        * prepended before whatever's forwarded, when this
                        * route fires. NULL/0 if none configured. */
    size_t arg_count;
} RouteEntry;

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
    RouteEntry *routes;          /* owned array of owned RouteEntry; only meaningful for
                                   * POLICY_ROUTE_MAP. NULL/0 is invalid for that policy
                                   * (validate_shim_entry requires at least one route). */
    size_t route_count;
    bool strip_matched_args;     /* meaningful for POLICY_ROUTE_ARGS, POLICY_SPLIT_ARGS, and
                                   * POLICY_ROUTE_MAP: remove whichever route arg(s) actually
                                   * matched before forwarding to whichever of source/fallback/
                                   * the matched route won */
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
    bool capture_timeout_set;    /* true if this shim overrides Config.capture_timeout_ms */
    int capture_timeout_ms;      /* only meaningful when capture_timeout_set */
    bool capture_limit_set;      /* true if this shim overrides Config.capture_limit_bytes */
    size_t capture_limit_bytes;  /* only meaningful when capture_limit_set */
} ShimEntry;

typedef struct {
    int version;
    ShimEntry *shims;   /* owned dynamic array */
    size_t count;
    size_t cap;
    bool verbose;       /* global default (top-level `verbose = true/false` in config.toml,
                          * default false) for whether `add` prints the shell-startup-file
                          * PATH-update notices; `add --verbose` overrides this to true for
                          * that one invocation regardless of the config default. */
    int capture_timeout_ms;      /* global default for the trial-run capture cutover (see
                                   * SHIMBACK_DEFAULT_CAPTURE_TIMEOUT_MS above); always
                                   * populated -- config_init seeds the hardcoded default,
                                   * a top-level `capture_timeout_ms = <n>` in config.toml
                                   * overrides it. A shim's own capture_timeout_set/
                                   * capture_timeout_ms, if present, wins over this. */
    size_t capture_limit_bytes;  /* global default for the same cutover's size half; always
                                   * populated the same way (see
                                   * SHIMBACK_DEFAULT_CAPTURE_LIMIT_BYTES). */
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

/* The same per-entry checks config_load/config_load_split already run on
 * every entry they parse (at least one route_args/rewrite rule/route/etc.
 * where a policy requires it, no two identical POLICY_ROUTE_MAP routes,
 * ...) -- exposed so a command that builds/mutates a ShimEntry in memory
 * (add.c's finish_add) can run the same check before writing it to disk,
 * instead of only finding out the config is invalid the next time
 * something reloads it (see review.md-style reasoning: a gap here once
 * already let `add` write a config that every other command then refused
 * to load). */
ConfigStatus validate_shim_entry(const ShimEntry *entry, char *errbuf, size_t errbuf_size);

/* Parses a "split config" file: the same per-shim keys a [shims.<name>]
 * table in config.toml would have, but as bare `key = value` lines with no
 * section header at all (the filename itself, "<name>-config.toml", is
 * what supplies the name -- see split_config_all_paths in paths.h).
 * `entry` must already have its name and policy set (as config_upsert
 * would); a section header ('[' as the first non-blank character of a
 * line) is a parse error, unlike in config.toml. Same validation as
 * config_load applies once parsing finishes (a missing required fallback,
 * a policy missing the fields it needs, ...). */
ConfigStatus config_load_split(const char *path, ShimEntry *entry, char *errbuf, size_t errbuf_size);

/* Writes just `entry`'s fields (no name, no [shims.<name>] header) to
 * `path` as bare key = value lines, atomically, creating parent
 * directories as needed -- the counterpart to config_load_split. */
ConfigStatus config_save_split(const ShimEntry *entry, const char *path, char *errbuf,
                                size_t errbuf_size);

/* Deletes any "<name>-config.toml" split file found in any of the three
 * potential locations (see split_config_all_paths in paths.h).
 * Best-effort: a location where nothing exists is silently skipped, not
 * treated as an error. Returns the number of files actually removed. */
size_t remove_split_configs(const char *name);

/* Where a shim's effective configuration actually comes from -- `list`
 * and `doctor` must consider every real shim symlink, not just ones with
 * a config.toml entry (see collect_all_shim_names), so a shim can be
 * config.toml-backed, split-file-backed, or (a real symlink, but no
 * configuration anywhere for it -- typically the config.toml entry or
 * split file having been deleted by hand) an orphan. */
typedef enum {
    SHIM_SOURCE_CONFIG,
    SHIM_SOURCE_SPLIT,
    SHIM_SOURCE_ORPHAN,
} ShimSource;

/* Resolves `name`'s effective ShimEntry: a split file (see
 * resolve_split_config_path, paths.h) always wins if one exists, then a
 * config.toml entry in `cfg`, then SHIM_SOURCE_ORPHAN if neither does.
 * For SHIM_SOURCE_SPLIT, *entry is a freshly heap-allocated ShimEntry the
 * caller must shim_entry_free() and free() once done; for
 * SHIM_SOURCE_CONFIG, *entry aliases the entry already living inside
 * `cfg` (config_find's own pointer -- do not free it separately, and it's
 * invalidated the same way that is); for SHIM_SOURCE_ORPHAN, *entry is
 * NULL. If `split_path_out` is non-NULL, it receives the split file's own
 * path (caller-owned) for SHIM_SOURCE_SPLIT, or NULL otherwise -- callers
 * that might need to write a fix back (doctor fix) need to know which
 * split file that is. Dies on a malformed split file, the same way
 * config_load dying on a malformed config.toml already would. */
ShimSource resolve_shim_entry(Config *cfg, const char *name, ShimEntry **entry,
                               char **split_path_out);

/* Builds the full set of shim names `list`/`doctor` must consider: every
 * name already in `cfg`, in its own order, followed by every
 * shimback-managed symlink name (see list_shim_symlink_names, paths.h)
 * that isn't already one of those -- a split-only shim, or an orphan (see
 * ShimSource). Newly allocated array of newly allocated strings;
 * *out_count receives its length (0/NULL if there's nothing at all). */
char **collect_all_shim_names(const Config *cfg, size_t *out_count);

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

/* ANSI color escape (see util.h) associated with `p` for colored output in
 * `list` and `add`'s confirmation line -- NULL for POLICY_EXIT_CODE, which
 * is left uncolored as the baseline default policy. */
const char *policy_color(Policy p);

#endif /* SHIMBACK_CONFIG_H */
