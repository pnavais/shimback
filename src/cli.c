#include "cli.h"

#include <stdio.h>
#include <string.h>

#include "commands/commands.h"
#include "version.h"

static void print_usage(void) {
    printf(
        "shimback %s -- run a primary command, transparently fall back to another on failure\n"
        "\n"
        "USAGE:\n"
        "  shimback add <name> [-s <source>] -f <fallback> [--policy exit-code|heuristic]\n"
        "                      [--error-pattern <p>]... [--diagnostic]\n"
        "  shimback remove <name>\n"
        "  shimback init\n"
        "  shimback list\n"
        "  shimback doctor\n"
        "  shimback install [--prefix <dir>]\n"
        "  shimback --help | --version\n",
        SHIMBACK_VERSION);
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
