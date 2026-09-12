#include "commands.h"

#include <getopt.h>
#include <stdio.h>
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

static const char *policy_color(Policy p) {
    switch (p) {
        case POLICY_HEURISTIC: return ANSI_YELLOW;
        case POLICY_EXIT_CODE_MATCH: return ANSI_MAGENTA;
        case POLICY_ROUTE_ARGS: return ANSI_CYAN;
        case POLICY_REWRITE: return ANSI_GREEN;
        case POLICY_EXIT_CODE:
        default: return NULL;
    }
}

static void print_detail_line(bool colorize, const char *label, const char *value) {
    const char *dim = colorize ? ANSI_DIM : "";
    const char *reset = colorize ? ANSI_RESET : "";
    printf("        %s%s:%s %s\n", dim, label, reset, value);
}

/* Everything the compact table leaves out: the policy-specific
 * configuration that actually drives a shim's behavior (which arguments
 * route it, which exit codes trigger a fallback, what gets rewritten into
 * what, ...). Prints nothing for a policy with no such configuration
 * (POLICY_EXIT_CODE), or for a field that's simply empty. */
static void print_full_details(const ShimEntry *e, bool colorize) {
    if (e->policy == POLICY_HEURISTIC && e->error_pattern_count > 0) {
        DynBuf buf;
        dynbuf_init(&buf);
        for (size_t i = 0; i < e->error_pattern_count; i++) {
            if (i > 0) {
                dynbuf_append_str(&buf, ", ");
            }
            dynbuf_append_str(&buf, e->error_patterns[i]);
        }
        print_detail_line(colorize, "error patterns", dynbuf_cstr(&buf));
        dynbuf_free(&buf);
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
        if (e->route_arg_count > 0) {
            DynBuf buf;
            dynbuf_init(&buf);
            for (size_t i = 0; i < e->route_arg_count; i++) {
                if (i > 0) {
                    dynbuf_append_str(&buf, ", ");
                }
                dynbuf_append_str(&buf, e->route_args[i]);
            }
            print_detail_line(colorize, "route args", dynbuf_cstr(&buf));
            dynbuf_free(&buf);
        }
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

    if (cfg.count == 0) {
        printf("No shims configured.\n");
        return 0;
    }

    bool colorize = stdout_is_color();

    size_t name_w = strlen("NAME");
    size_t source_w = strlen("SOURCE");
    size_t fallback_w = strlen("FALLBACK");
    size_t policy_w = strlen("POLICY");

    for (size_t i = 0; i < cfg.count; i++) {
        ShimEntry *e = &cfg.shims[i];
        const char *source_display = e->source ? e->source : "auto";
        const char *fallback_display = e->fallback ? e->fallback : "none";
        size_t nl = strlen(e->name);
        size_t sl = strlen(source_display);
        size_t fl = strlen(fallback_display);
        size_t pl = strlen(policy_to_string(e->policy));
        if (nl > name_w) name_w = nl;
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

    for (size_t i = 0; i < cfg.count; i++) {
        ShimEntry *e = &cfg.shims[i];
        const char *source_display = e->source ? e->source : "auto";
        const char *fallback_display = e->fallback ? e->fallback : "none";
        const char *policy_str = policy_to_string(e->policy);

        print_cell(e->name, name_w, ANSI_BOLD ANSI_CYAN, colorize);
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
            print_full_details(e, colorize);
        }
    }

    return 0;
}
