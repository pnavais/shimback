#include "cli.h"

#include <stdio.h>
#include <string.h>

#include "commands/commands.h"
#include "util.h"
#include "version.h"

/* Renders `markup`: a span opened and closed with octal '\001' is a literal
 * (command/flag) and is colored bold green, '\002' is a placeholder value
 * and is colored cyan, '\003' is a section header and is colored bold
 * yellow, '\004' is muted example/aside text and is colored dim --
 * matching clap-rs's default styled-help palette for the first three.
 * Delimiters are stripped either way; colors are only ever emitted when
 * `colorize`. Octal escapes, not hex: `\x` greedily consumes trailing hex
 * digits, which would silently swallow a leading 'e' in content like
 * "exit-code". */
static void print_markup(bool colorize, const char *markup) {
    const char *lit = ANSI_BOLD ANSI_GREEN;
    const char *ph = ANSI_CYAN;
    const char *hdr = ANSI_BOLD ANSI_YELLOW;
    const char *dim = ANSI_DIM;
    for (const char *p = markup; *p != '\0'; p++) {
        char delim = *p;
        if (delim == '\001' || delim == '\002' || delim == '\003' || delim == '\004') {
            const char *color =
                delim == '\001' ? lit : delim == '\002' ? ph : delim == '\003' ? hdr : dim;
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
        "  \001shimback list\001 (alias: \001ls\001)\n"
        "  \001shimback doctor\001\n"
        "  \001shimback install\001 [\001--prefix\001 \002<dir>\002]\n"
        "  \001shimback --help\001 | \001--version\001\n"
        "\n"
        "\003COMMANDS:\003\n"
        "  \001add\001       Create or update a shim named <name>, running \002<source>\002 "
        "first and\n"
        "            transparently retrying \002<fallback>\002 depending on the policy (see\n"
        "            README.md for all four).\n"
        "              \004e.g. shimback add sed -f /usr/bin/sed\004\n"
        "\n"
        "  \001remove\001    Remove a shim's symlink and its config entry. Leaves the PATH\n"
        "            injection in your shell startup file alone.\n"
        "              \004e.g. shimback remove sed\004\n"
        "\n"
        "  \001init\001      Detect every installed shell (zsh, bash, fish) and add the shim\n"
        "            directory to PATH in each one, not just your current shell.\n"
        "              \004e.g. shimback init\004\n"
        "\n"
        "  \001list\001      List every configured shim as a table: name, source, fallback,\n"
        "            policy, and diagnostic flag. Also available as \001ls\001.\n"
        "              \004e.g. shimback list\004\n"
        "\n"
        "  \001doctor\001    Check the whole setup end to end -- dead symlinks, missing or\n"
        "            non-executable source/fallback binaries, a policy with nothing to\n"
        "            select on -- and exit non-zero if anything's wrong.\n"
        "              \004e.g. shimback doctor\004\n"
        "\n"
        "  \001install\001   Copy the running shimback binary to a stable, PATH-ed location\n"
        "            (default: ~/.local/bin) so shim symlinks (which point at wherever\n"
        "            the binary was running from at `add` time) survive a rebuild.\n"
        "              \004e.g. shimback install --prefix ~/.local\004\n");
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
    if (strcmp(argv[1], "list") == 0 || strcmp(argv[1], "ls") == 0) {
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
