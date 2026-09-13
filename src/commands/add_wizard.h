#ifndef SHIMBACK_ADD_WIZARD_H
#define SHIMBACK_ADD_WIZARD_H

#include <stddef.h>

#include "../config.h"
#include "../util.h"

/* Whatever `add`'s own CLI-flag parsing already collected before it found
 * something required missing -- the wizard is seeded with this so it can
 * skip straight past any page whose data is already known (see
 * run_add_wizard), while still letting the user revisit and edit it via the
 * Left arrow. NULL/0/false means "not given". */
typedef struct {
    const char *name;
    const char *source_arg;
    StrVec *source_args;
    const char *fallback_arg;
    StrVec *fallback_args;
    Policy policy; /* whatever cmd_add already resolved (default POLICY_EXIT_CODE) */
    StrVec *patterns;
    int *exit_codes;
    size_t exit_code_count;
    StrVec *route_args;
    bool strip_matched_args;
    StrVec *split_source_args;
    StrVec *split_fallback_args;
    StrVec *rewrite_from;
    StrVec *rewrite_to;
    bool diagnostic;
} WizardSeed;

/* Mirrors the same set of fields cmd_add's shared "finish" tail expects.
 * Everything is owned by the caller once returned. */
typedef struct {
    char *name;
    char *source_arg;   /* raw string as typed/accepted, or NULL for "auto" */
    StrVec source_args;
    char *fallback_arg; /* raw string as typed/accepted, or NULL (POLICY_REWRITE only) */
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
    bool diagnostic;
} WizardResult;

/* Runs the interactive "add" wizard, pre-seeded from `seed`. Caller must
 * have already confirmed tui_supported(). Returns true and fills `*out`
 * (caller owns everything in it) if the user completed every page; returns
 * false, leaving `*out` untouched and nothing written to disk, if aborted
 * via Esc/Ctrl-C. */
bool run_add_wizard(const WizardSeed *seed, WizardResult *out);

/* The exact shim-name validity rule the CLI path enforces too (empty, a
 * '/' anywhere, or literally "shimback") -- exported so both the wizard's
 * name page and add.c's shared tail check the identical rule instead of
 * two copies drifting apart. */
bool is_valid_shim_name(const char *name);

#endif /* SHIMBACK_ADD_WIZARD_H */
