#ifndef SHIMBACK_CONFIG_H
#define SHIMBACK_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    POLICY_EXIT_CODE,
    POLICY_HEURISTIC,
} Policy;

typedef struct {
    char *name;               /* owned; never NULL */
    char *source;              /* owned; NULL means "auto" (resolve from PATH at dispatch time) */
    char *fallback;             /* owned; required */
    Policy policy;
    char **error_patterns;      /* owned array of owned strings; NULL if none */
    size_t error_pattern_count;
    bool diagnostic;
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
