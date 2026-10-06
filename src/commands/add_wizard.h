#ifndef SHIMBACK_ADD_WIZARD_H
#define SHIMBACK_ADD_WIZARD_H

#include <stddef.h>

#include "../config.h"
#include "../util.h"

/* Everything one `add` needs: filled from the command line by cmd_add,
 * completed by the wizard when something required is missing, then
 * consumed by finish_add. NULL/0/false/empty means "not given". */
typedef struct {
    const char *name;
    const char *source_arg;   /* raw string as typed/accepted, or NULL for "auto" */
    StrVec source_args;
    const char *fallback_arg; /* raw string as typed/accepted, or NULL (POLICY_REWRITE only) */
    StrVec fallback_args;
    Policy policy;
    StrVec patterns;
    int *exit_codes;
    size_t exit_code_count;
    StrVec route_args;
    bool strip_matched_args;
    StrVec split_source_args;
    StrVec split_fallback_args;
    StrVec rewrite_from;
    StrVec rewrite_to;
    StrVec route_match;   /* parallel to route_command: route_match.items[i] is the
                           * trigger for route_command.items[i] -- see POLICY_ROUTE_MAP */
    StrVec route_command;
    bool diagnostic;
    bool force;
    bool verbose;
    bool capture_timeout_set;
    int capture_timeout_ms;
    bool capture_limit_set;
    size_t capture_limit_bytes;
    bool split_config;
} AddRequest;

/* Runs the interactive "add" wizard, pre-seeded from `seed` (it skips
 * straight past any page whose data is already known, while still letting
 * the user revisit and edit it via the Left arrow). Caller must have
 * already confirmed tui_supported(). Returns true and fills `*out` -- a
 * copy of `seed` with every field the wizard asks about replaced by the
 * user's answers, newly allocated -- if the user completed every page;
 * returns false, leaving `*out` untouched and nothing written to disk, if
 * aborted via Esc/Ctrl-C. */
bool run_add_wizard(const AddRequest *seed, AddRequest *out);

/* is_valid_shim_name lives in paths.h now (config.c's config_load needs
 * it too, to validate a [shims.<name>] section header the same way --
 * see its own comment there), not declared again here since every user
 * of this header already gets paths.h transitively. */

#endif /* SHIMBACK_ADD_WIZARD_H */
