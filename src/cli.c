#include "cli.h"

#include <stdio.h>
#include <string.h>

#include "commands/commands.h"
#include "util.h"
#include "version.h"

/* Renders `markup`: a span opened and closed with octal '\001' is a literal
 * (command/flag) and is colored bold green, '\002' is a placeholder value
 * and is colored cyan, '\003' is a section header and is colored bold
 * yellow -- matching clap-rs's default styled-help palette. Delimiters are
 * stripped either way; colors are only ever emitted when `colorize`.
 * Octal escapes, not hex: `\x` greedily consumes trailing hex digits, which
 * would silently swallow a leading 'e' in content like "exit-code". */
static void print_markup(bool colorize, const char *markup) {
    const char *lit = ANSI_BOLD ANSI_GREEN;
    const char *ph = ANSI_CYAN;
    const char *hdr = ANSI_BOLD ANSI_YELLOW;
    for (const char *p = markup; *p != '\0'; p++) {
        char delim = *p;
        if (delim == '\001' || delim == '\002' || delim == '\003') {
            const char *color = delim == '\001' ? lit : delim == '\002' ? ph : hdr;
            p++;
            if (colorize) {
                printf("%s", color);
            }
            while (*p != '\0' && *p != delim) {
                putchar(*p);
                p++;
            }
            if (colorize) {
                printf("%s", ANSI_RESET);
            }
            if (*p == '\0') {
                break;
            }
        } else {
            putchar(delim);
        }
    }
}

static void print_usage(void) {
    printf("shimback %s -- run a primary command, transparently fall back to another on failure\n"
           "\n",
           SHIMBACK_VERSION);
    print_markup(
        stdout_is_color(),
        "\003USAGE:\003\n"
        "  \001shimback add\001 \002<name>\002 [\001-s\001 \002<source>\002] \001-f\001 "
        "\002<fallback>\002\n"
        "                      [\001--policy\001 "
        "\002exit-code\002|\002heuristic\002|\002exit-code-match\002|\002route-args\002]\n"
        "                      [\001--error-pattern\001 \002<p>\002]... [\001--exit-code\001 "
        "\002<code>\002]...\n"
        "                      [\001--route-arg\001 \002<arg>\002]... "
        "[\001--strip-matched-args\001] [\001--diagnostic\001]\n"
        "  \001shimback remove\001 \002<name>\002\n"
        "  \001shimback init\001\n"
        "  \001shimback list\001\n"
        "  \001shimback doctor\001\n"
        "  \001shimback install\001 [\001--prefix\001 \002<dir>\002]\n"
        "  \001shimback --help\001 | \001--version\001\n");
}

int cli_run(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage();
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0) {
        printf("shimback %s\n", SHIMBACK_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "add") == 0) {
        return cmd_add(argc - 1, argv + 1);
    }
    if (strcmp(argv[1], "remove") == 0) {
        return cmd_remove(argc - 1, argv + 1);
    }
    if (strcmp(argv[1], "init") == 0) {
        return cmd_init(argc - 1, argv + 1);
    }
    if (strcmp(argv[1], "list") == 0) {
        return cmd_list(argc - 1, argv + 1);
    }
    if (strcmp(argv[1], "doctor") == 0) {
        return cmd_doctor(argc - 1, argv + 1);
    }
    if (strcmp(argv[1], "install") == 0) {
        return cmd_install(argc - 1, argv + 1);
    }

    fprintf(stderr, "shimback: unknown command '%s'\n", argv[1]);
    print_usage();
    return 1;
}
