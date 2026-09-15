#include "commands.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../config.h"
#include "../paths.h"
#include "../util.h"

static const char *USAGE = "usage: shimback list [--full]\n";

/* Prints `text`, optionally wrapped in `color`, then pads with spaces up to
 * `width` -- padding is based on the plain text length, since padding to
 * the length of a color-escaped string would misalign columns. */
static void print_cell(const char *text, size_t width, const char *color, bool colorize) {
    if (colorize && color) {
        printf("%s%s%s", color, text, ANSI_RESET);
    } else {
        printf("%s", text);
    }
    for (size_t len = strlen(text); len < width; len++) {
        putchar(' ');
    }
}

static void print_detail_line(bool colorize, const char *label, const char *value) {
    const char *dim = colorize ? ANSI_DIM : "";
    const char *reset = colorize ? ANSI_RESET : "";
    printf("        %s%s:%s %s\n", dim, label, reset, value);
}

static void print_joined(bool colorize, const char *label, char *const *items, size_t count) {
    if (count == 0) {
        return;
    }
    DynBuf buf;
    dynbuf_init(&buf);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            dynbuf_append_str(&buf, ", ");
        }
        dynbuf_append_str(&buf, items[i]);
    }
    print_detail_line(colorize, label, dynbuf_cstr(&buf));
    dynbuf_free(&buf);
}

/* Everything the compact table leaves out: source_args/fallback_args
 * (independent of policy -- see add.c), and the policy-specific
 * configuration that actually drives a shim's behavior (which arguments
 * route it, which exit codes trigger a fallback, what gets rewritten into
 * what, ...). Prints nothing for a policy with no such configuration
 * (POLICY_EXIT_CODE), or for a field that's simply empty. */
static void print_full_details(const ShimEntry *e, bool colorize) {
    print_joined(colorize, "source args", e->source_args, e->source_arg_count);
    print_joined(colorize, "fallback args", e->fallback_args, e->fallback_arg_count);

    if (e->policy == POLICY_HEURISTIC) {
        print_joined(colorize, "error patterns", e->error_patterns, e->error_pattern_count);
    }

    if (e->policy == POLICY_EXIT_CODE_MATCH && e->exit_code_count > 0) {
        DynBuf buf;
        dynbuf_init(&buf);
        for (size_t i = 0; i < e->exit_code_count; i++) {
            char num[16];
            snprintf(num, sizeof(num), "%s%d", i > 0 ? ", " : "", e->exit_codes[i]);
            dynbuf_append_str(&buf, num);
        }
        print_detail_line(colorize, "exit codes", dynbuf_cstr(&buf));
        dynbuf_free(&buf);
    }

    if (e->policy == POLICY_ROUTE_ARGS) {
        print_joined(colorize, "route args", e->route_args, e->route_arg_count);
        print_detail_line(colorize, "strip matched args", e->strip_matched_args ? "true" : "false");
    }

    if (e->policy == POLICY_SPLIT_ARGS) {
        print_joined(colorize, "source route args", e->source_route_args,
                     e->source_route_arg_count);
        print_joined(colorize, "fallback route args", e->fallback_route_args,
                     e->fallback_route_arg_count);
        print_detail_line(colorize, "strip matched args", e->strip_matched_args ? "true" : "false");
    }

    if (e->policy == POLICY_REWRITE && e->rewrite_from_count > 0) {
        DynBuf buf;
        dynbuf_init(&buf);
        for (size_t i = 0; i < e->rewrite_from_count; i++) {
            if (i > 0) {
                dynbuf_append_str(&buf, ", ");
            }
            dynbuf_append_str(&buf, e->rewrite_from[i]);
            dynbuf_append_str(&buf, " -> ");
            dynbuf_append_str(&buf, e->rewrite_to[i]);
        }
        print_detail_line(colorize, "rewrite rules", dynbuf_cstr(&buf));
        dynbuf_free(&buf);
    }
}

/* strlen(ORPHAN_LABEL) has to fit inside source_w + 2 + fallback_w + 2 +
 * policy_w for print_orphan_row's spanning cell below to make sense --
 * given how short the fixed column headers are, this is always true in
 * practice (a shim name long enough to make name_w dominate doesn't
 * affect this), but there's no enforced invariant tying the two, so this
 * comment is the only thing keeping that in view. */
#define ORPHAN_LABEL "(orphaned symlink -- no config.toml entry or split config file found)"

static void print_orphan_row(const char *name, size_t name_w, bool colorize) {
    print_cell(name, name_w, ANSI_BOLD ANSI_CYAN, colorize);
    printf("  ");
    print_cell(ORPHAN_LABEL, 0, ANSI_RED, colorize);
    printf("\n");
}

int cmd_list(int argc, char **argv) {
    bool full = false;

    static struct option long_opts[] = {
        {"full", no_argument, 0, 'f'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "f", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'f': full = true; break;
            default:
                fprintf(stderr, "%s", USAGE);
                return 1;
        }
    }
    if (optind < argc) {
        die("list: unexpected argument '%s'", argv[optind]);
    }

    char *cfg_path = config_file_path();
    Config cfg;
    char errbuf[256];
    ConfigStatus cst = config_load(cfg_path, &cfg, errbuf, sizeof(errbuf));
    if (cst != CONFIG_OK) {
        die("list: %s", errbuf);
    }

    size_t name_count = 0;
    char **names = collect_all_shim_names(&cfg, &name_count);

    if (name_count == 0) {
        printf("No shims configured.\n");
        return 0;
    }

    char *shim_dir = shim_bin_dir();

    /* Resolved once up front (not per pass) since a split entry is a
     * freshly heap-loaded ShimEntry -- loading it twice (once to measure
     * column widths, again to print) would be wasted work for no reason. */
    ShimEntry **entries = xmalloc(name_count * sizeof(ShimEntry *));
    ShimSource *sources = xmalloc(name_count * sizeof(ShimSource));
    char **split_paths = xmalloc(name_count * sizeof(char *));
    for (size_t i = 0; i < name_count; i++) {
        sources[i] = resolve_shim_entry(&cfg, names[i], &entries[i], &split_paths[i]);
    }

    bool colorize = stdout_is_color();

    size_t name_w = strlen("NAME");
    size_t source_w = strlen("SOURCE");
    size_t fallback_w = strlen("FALLBACK");
    size_t policy_w = strlen("POLICY");

    for (size_t i = 0; i < name_count; i++) {
        size_t nl = strlen(names[i]);
        if (nl > name_w) name_w = nl;
        if (sources[i] == SHIM_SOURCE_ORPHAN) {
            continue;
        }
        ShimEntry *e = entries[i];
        const char *source_display = e->source ? e->source : "auto";
        const char *fallback_display = e->fallback ? e->fallback : "none";
        size_t sl = strlen(source_display);
        size_t fl = strlen(fallback_display);
        size_t pl = strlen(policy_to_string(e->policy));
        if (sl > source_w) source_w = sl;
        if (fl > fallback_w) fallback_w = fl;
        if (pl > policy_w) policy_w = pl;
    }

    const char *header_color = ANSI_BOLD ANSI_YELLOW;
    print_cell("NAME", name_w, header_color, colorize);
    printf("  ");
    print_cell("SOURCE", source_w, header_color, colorize);
    printf("  ");
    print_cell("FALLBACK", fallback_w, header_color, colorize);
    printf("  ");
    print_cell("POLICY", policy_w, header_color, colorize);
    printf("  ");
    print_cell("DIAGNOSTIC", 0, header_color, colorize);
    printf("\n");

    for (size_t i = 0; i < name_count; i++) {
        char *symlink_path = path_join(shim_dir, names[i]);

        if (sources[i] == SHIM_SOURCE_ORPHAN) {
            print_orphan_row(names[i], name_w, colorize);
            if (full) {
                print_detail_line(colorize, "symlink", symlink_path);
            }
            free(symlink_path);
            continue;
        }
        ShimEntry *e = entries[i];
        const char *source_display = e->source ? e->source : "auto";
        const char *fallback_display = e->fallback ? e->fallback : "none";
        const char *policy_str = policy_to_string(e->policy);

        print_cell(names[i], name_w, ANSI_BOLD ANSI_CYAN, colorize);
        printf("  ");
        print_cell(source_display, source_w, e->source ? ANSI_GREEN : ANSI_DIM, colorize);
        printf("  ");
        print_cell(fallback_display, fallback_w, e->fallback ? ANSI_BLUE : ANSI_DIM, colorize);
        printf("  ");
        print_cell(policy_str, policy_w, policy_color(e->policy), colorize);
        printf("  ");
        print_cell(e->diagnostic ? "true" : "false", 0, e->diagnostic ? ANSI_GREEN : ANSI_DIM,
                   colorize);
        printf("\n");

        if (full) {
            print_detail_line(colorize, "symlink", symlink_path);
            if (sources[i] == SHIM_SOURCE_SPLIT) {
                print_detail_line(colorize, "config", split_paths[i]);
            }
            print_full_details(e, colorize);
        }
        free(symlink_path);
    }

    free(shim_dir);
    for (size_t i = 0; i < name_count; i++) {
        if (sources[i] == SHIM_SOURCE_SPLIT) {
            shim_entry_free(entries[i]);
            free(entries[i]);
        }
        free(split_paths[i]);
        free(names[i]);
    }
    free(entries);
    free(sources);
    free(split_paths);
    free(names);

    return 0;
}
