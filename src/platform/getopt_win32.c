/* getopt_long() for the Windows build -- see win32-compat/getopt.h for why
 * this exists and exactly what subset of behavior it targets. */
#include "win32-compat/getopt.h"

#include <stdio.h>
#include <string.h>

char *optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = 0;

/* Position within a short-option cluster (e.g. "-abc") being consumed
 * across repeated calls; NULL/empty between clusters. */
static const char *short_scan_pos = NULL;

static const char *prog_name(char *const argv[]) {
    return (argv[0] && argv[0][0]) ? argv[0] : "shimback";
}

static int is_option(const char *s) {
    return s[0] == '-' && s[1] != '\0';
}

/* Rotates argv[optind_ .. opt_i+block_len) left: the `block_len`-element
 * block starting at opt_i (an option token, and -- when block_len is 2 --
 * its separate-slot argument right after it) moves to optind_, and
 * whatever was at [optind_, opt_i) (the positional arguments skipped over
 * to find it) shifts right by block_len slots, preserving their relative
 * order. Applied once per call, this is the standard incremental step GNU
 * getopt_long uses to permute options and positionals apart across a full
 * sequence of calls, so that by the time option parsing is done every
 * positional has been pushed together, starting at argv[optind].
 *
 * block_len must be 2, not 1, whenever the option being rotated into place
 * needs a separate argument slot (see option_block_len below) -- rotating
 * just the flag and leaving its argument behind would let a *positional*
 * that happened to be adjacent get mistaken for that argument on the next
 * call, corrupting option values silently rather than erroring (found via
 * an actual failing `shimback add <name> -s <source> -f <fallback>`
 * invocation, not by inspection -- the original block_len-1-always version
 * of this function shipped and passed compilation, but every add/remove/
 * etc. invocation with a positional before any option was silently broken). */
static void rotate_block_to_front(char *const argv_c[], int optind_, int opt_i, int block_len) {
    char **argv = (char **)argv_c; /* reordering pointers, not their
                                     * pointed-to content -- see the real
                                     * getopt()'s identical convention. */
    char *tmp[2];
    for (int i = 0; i < block_len; i++) {
        tmp[i] = argv[opt_i + i];
    }
    for (int i = opt_i - 1; i >= optind_; i--) {
        argv[i + block_len] = argv[i];
    }
    for (int i = 0; i < block_len; i++) {
        argv[optind_ + i] = tmp[i];
    }
}

static const struct option *find_long(const struct option *longopts, const char *name,
                                       size_t name_len) {
    for (const struct option *o = longopts; o->name; o++) {
        if (strlen(o->name) == name_len && strncmp(o->name, name, name_len) == 0) {
            return o;
        }
    }
    return NULL;
}

/* How many argv slots the option token at argv[opt_i] occupies together
 * with its own argument, if it needs one that isn't already attached to
 * it (a long "--name=value", or a short "-xVALUE") -- 2 if it needs a
 * separate following slot, 1 otherwise. Doesn't handle a required-argument
 * short option buried *after* a no-argument one in the same cluster (e.g.
 * "-vs VALUE" with v no-arg, s required-arg): that specific combination
 * isn't used anywhere in this codebase's actual option strings/usage, and
 * handling it would need this same block-rotation awareness threaded into
 * handle_short's own mid-cluster continuation path too. */
static int option_block_len(const char *token, const char *optstring,
                             const struct option *longopts) {
    if (token[1] == '-') {
        if (token[2] == '\0') {
            return 1; /* bare "--" terminator */
        }
        const char *arg = token + 2;
        const char *eq = strchr(arg, '=');
        if (eq) {
            return 1; /* argument already attached */
        }
        const struct option *matched = longopts ? find_long(longopts, arg, strlen(arg)) : NULL;
        return (matched && matched->has_arg == required_argument) ? 2 : 1;
    }
    char c = token[1];
    const char *spec = c == ':' ? NULL : strchr(optstring, c);
    if (!spec || spec[1] != ':') {
        return 1;
    }
    return token[2] == '\0' ? 2 : 1; /* needs an arg, and none is attached */
}

static int handle_long(int argc, char *const argv[], const struct option *longopts,
                        int *longindex) {
    const char *arg = argv[optind] + 2; /* past "--" */
    const char *eq = strchr(arg, '=');
    size_t name_len = eq ? (size_t)(eq - arg) : strlen(arg);

    const struct option *matched = longopts ? find_long(longopts, arg, name_len) : NULL;
    if (!matched) {
        if (opterr) {
            fprintf(stderr, "%s: unrecognized option '--%.*s'\n", prog_name(argv), (int)name_len,
                    arg);
        }
        optind++;
        return '?';
    }
    if (longindex) {
        *longindex = (int)(matched - longopts);
    }

    if (matched->has_arg == required_argument) {
        if (eq) {
            optarg = (char *)(eq + 1);
            optind++;
        } else if (optind + 1 < argc) {
            optarg = argv[optind + 1];
            optind += 2;
        } else {
            if (opterr) {
                fprintf(stderr, "%s: option '--%s' requires an argument\n", prog_name(argv),
                        matched->name);
            }
            optind++;
            return '?';
        }
    } else {
        optarg = NULL;
        optind++;
        if (eq) {
            if (opterr) {
                fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n", prog_name(argv),
                        matched->name);
            }
            return '?';
        }
    }

    if (matched->flag) {
        *matched->flag = matched->val;
        return 0;
    }
    return matched->val;
}

static int handle_short(int argc, char *const argv[], const char *optstring) {
    if (!short_scan_pos || *short_scan_pos == '\0') {
        short_scan_pos = argv[optind] + 1; /* past the leading '-' */
    }

    char c = *short_scan_pos;
    const char *spec = c == ':' ? NULL : strchr(optstring, c);
    if (!spec) {
        optopt = c;
        if (opterr) {
            fprintf(stderr, "%s: invalid option -- '%c'\n", prog_name(argv), c);
        }
        short_scan_pos++;
        if (*short_scan_pos == '\0') {
            short_scan_pos = NULL;
            optind++;
        }
        return '?';
    }

    short_scan_pos++;
    if (spec[1] == ':') {
        if (*short_scan_pos != '\0') {
            optarg = (char *)short_scan_pos;
            short_scan_pos = NULL;
            optind++;
        } else if (optind + 1 < argc) {
            optarg = argv[optind + 1];
            short_scan_pos = NULL;
            optind += 2;
        } else {
            optopt = c;
            if (opterr) {
                fprintf(stderr, "%s: option requires an argument -- '%c'\n", prog_name(argv), c);
            }
            short_scan_pos = NULL;
            optind++;
            return '?';
        }
    } else {
        optarg = NULL;
        if (*short_scan_pos == '\0') {
            short_scan_pos = NULL;
            optind++;
        }
    }
    return c;
}

int getopt_long(int argc, char *const argv[], const char *optstring, const struct option *longopts,
                 int *longindex) {
    optarg = NULL;
    if (optind == 0) {
        optind = 1;
    }

    /* Resuming a short-option cluster (e.g. "-abc") from a previous call --
     * the option that started it was already moved to the front. */
    if (short_scan_pos && *short_scan_pos != '\0') {
        return handle_short(argc, argv, optstring);
    }
    short_scan_pos = NULL;

    if (optind >= argc) {
        return -1;
    }

    int opt_i = optind;
    while (opt_i < argc && !is_option(argv[opt_i])) {
        opt_i++;
    }
    if (opt_i >= argc) {
        return -1; /* nothing left but positionals; optind is already correct */
    }
    if (strcmp(argv[opt_i], "--") == 0) {
        if (opt_i != optind) {
            rotate_block_to_front(argv, optind, opt_i, 1);
        }
        optind++;
        return -1;
    }
    int block_len = option_block_len(argv[opt_i], optstring, longopts);
    if (opt_i != optind) {
        rotate_block_to_front(argv, optind, opt_i, block_len);
    }

    if (argv[optind][1] == '-') {
        return handle_long(argc, argv, longopts, longindex);
    }
    return handle_short(argc, argv, optstring);
}
