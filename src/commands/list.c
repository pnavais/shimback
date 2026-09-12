#include "commands.h"

#include <stdio.h>
#include <string.h>

#include "../config.h"
#include "../paths.h"
#include "../util.h"

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
        case POLICY_EXIT_CODE:
        default: return NULL;
    }
}

int cmd_list(int argc, char **argv) {
    if (argc > 1) {
        die("list: unexpected argument '%s'", argv[1]);
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
        size_t nl = strlen(e->name);
        size_t sl = strlen(source_display);
        size_t fl = strlen(e->fallback);
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
        const char *policy_str = policy_to_string(e->policy);

        print_cell(e->name, name_w, ANSI_BOLD ANSI_CYAN, colorize);
        printf("  ");
        print_cell(source_display, source_w, e->source ? ANSI_GREEN : ANSI_DIM, colorize);
        printf("  ");
        print_cell(e->fallback, fallback_w, ANSI_BLUE, colorize);
        printf("  ");
        print_cell(policy_str, policy_w, policy_color(e->policy), colorize);
        printf("  ");
        print_cell(e->diagnostic ? "true" : "false", 0, e->diagnostic ? ANSI_GREEN : ANSI_DIM,
                   colorize);
        printf("\n");
    }

    return 0;
}
