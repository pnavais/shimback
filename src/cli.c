#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands/commands.h"
#include "suggest.h"
#include "util.h"
#include "version.h"

static const char *const KNOWN_COMMANDS[] = {
    "add", "remove", "rm", "init", "list", "ls", "doctor", "install", "uninstall", "edit",
};

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

/* One entry per real command (aliases -- rm, ls -- share their primary
 * command's entry; see COMMAND_ALIASES below). `usage` is that command's
 * own "shimback <cmd> ..." line(s) under the global USAGE: header, with
 * no trailing blank line; `description` is its COMMANDS: paragraph,
 * including the trailing blank line that separates it from the next
 * command's -- so print_usage() below reproduces the exact same combined
 * output by simply concatenating every entry's fields in order, and
 * print_command_help() reproduces exactly one command's slice of it. */
typedef struct {
    const char *name;
    const char *usage;
    const char *description;
} CommandHelp;

static const CommandHelp COMMAND_HELP[] = {
    {
        "add",
        "  \001shimback add\001 \002<name>\002 [\001-s\001 \002<source>\002] "
        "[\001--source-arg\001 \002<arg>\002]...\n"
        "                      \001-f\001 \002<fallback>\002 [\001--fallback-arg\001 "
        "\002<arg>\002]...\n"
        "                      [\001--policy\001 "
        "\002exit-code\002|\002heuristic\002|\002exit-code-match\002|\002route-args\002|"
        "\002rewrite\002|\002split-args\002]\n"
        "                      [\001--error-pattern\001 \002<p>\002]... [\001--exit-code\001 "
        "\002<code>\002]...\n"
        "                      [\001--route-arg\001 \002<arg>\002]... "
        "[\001--strip-matched-args\001]\n"
        "                      [\001--split-source-arg\001 \002<arg>\002]... "
        "[\001--split-fallback-arg\001 \002<arg>\002]...\n"
        "                      [\001--rewrite\001 \002<from>\002=\002<to>\002]... "
        "[\001--diagnostic\001] [\001--force\001] [\001-v\001|\001--verbose\001]\n"
        "                      [\001--capture-timeout\001 \002<ms>\002] "
        "[\001--capture-limit\001 \002<size>\002] [\001--split-config\001]\n",

        "  \001add\001       Create or update a shim named <name>, running \002<source>\002 "
        "first and\n"
        "            transparently retrying \002<fallback>\002 depending on the policy --\n"
        "            see README.md for all six.\n"
        "\n"
        "              \001--policy rewrite\001 is different from the rest: it never falls\n"
        "              back, just rewrites matched \001--rewrite\001 arguments before running\n"
        "              source every time, and \001--fallback\001 is optional for it.\n"
        "\n"
        "              \001--source-arg\001/\001--fallback-arg\001 attach fixed, baked-in arguments\n"
        "              to whichever runs, independent of policy, so a shim can also\n"
        "              work as a regular alias on top of whatever its policy does.\n"
        "\n"
        "              Run it with required information missing at an interactive\n"
        "              terminal, and an interactive wizard fills it in instead of\n"
        "              failing.\n"
        "\n"
        "              \001--force\001 allows a \002<source>\002/\002<fallback>\002 path (not a bare\n"
        "              name) that doesn't exist yet; \001doctor\001 skips its existence check\n"
        "              for whichever of them still doesn't, but dispatch always checks\n"
        "              for real when the shim actually runs.\n"
        "\n"
        "              The shim's confirmation line is colored; the shell-startup-file\n"
        "              PATH-update notices right after it are silent unless\n"
        "              \001-v\001/\001--verbose\001 is given, or \001verbose = true\001 is set as a\n"
        "              top-level config default (config defaults to \001false\001).\n"
        "\n"
        "              A trial run that takes too long or produces too much output\n"
        "              gives up on being hideable and streams live instead (nothing\n"
        "              lost, just no more fallback for that run) -- tune this with\n"
        "              \001--capture-timeout\001/\001--capture-limit\001 (default 2000ms/8MiB; also\n"
        "              settable globally in the config). \001--capture-limit\001 takes a\n"
        "              plain byte count or a size with a unit suffix (B, K/KB, KiB,\n"
        "              M/MB, MiB, G/GB, GiB).\n"
        "\n"
        "              \001--split-config\001 saves this shim's settings in their own\n"
        "              \002<name>\002-config.toml file (in the config directory) instead of\n"
        "              inside config.toml -- move that file to the shimback binary's\n"
        "              own directory, or the shim symlink's own directory, to override\n"
        "              it from there instead. Re-running \001add\001 without\n"
        "              \001--split-config\001 folds it back.\n"
        "\n"
        "              \004e.g. shimback add sed -f /usr/bin/sed\004\n"
        "\n",
    },
    {
        "remove",
        "  \001shimback remove\001 [\001-y\001] \002<name>\002 (alias: \001rm\001)\n",

        "  \001remove\001    Remove a shim's symlink and its config entry. Leaves the PATH\n"
        "            injection in your shell startup file alone. Also available as\n"
        "            \001rm\001. \001-y\001/\001--yes\001 auto-removes an unambiguous \"did you\n"
        "            mean\" match instead of just showing it.\n"
        "              \004e.g. shimback remove -y shed\004\n"
        "\n",
    },
    {
        "init",
        "  \001shimback init\001\n",

        "  \001init\001      Detect every installed shell (zsh, bash, fish) and add the shim\n"
        "            directory to PATH in each one, not just your current shell.\n"
        "              \004e.g. shimback init\004\n"
        "\n",
    },
    {
        "list",
        "  \001shimback list\001 [\001--full\001] (alias: \001ls\001)\n",

        "  \001list\001      List every shim (config.toml- or split-config-backed, plus any\n"
        "            orphan -- see \001doctor\001) as a table: name, source, fallback, policy,\n"
        "            and diagnostic flag. Also available as \001ls\001. \001--full\001 also prints\n"
        "            each shim's policy-specific configuration.\n"
        "              \004e.g. shimback list --full\004\n"
        "\n",
    },
    {
        "doctor",
        "  \001shimback doctor\001 [\001fix\001 [\001-y\001|\001--yes\001]]\n",

        "  \001doctor\001    Check the whole setup end to end -- dead symlinks, missing or\n"
        "            non-executable source/fallback binaries, a policy with nothing to\n"
        "            select on, and an orphaned shim symlink with no config.toml entry\n"
        "            or split config file anywhere for it -- and exit non-zero if\n"
        "            anything's wrong.\n"
        "\n"
        "              \001doctor fix\001 recreates any missing or dangling shim symlink it\n"
        "              finds, and prompts to remove each orphan it finds\n"
        "              (\001-y\001/\001--yes\001 removes every orphan without asking).\n"
        "\n"
        "              \004e.g. shimback doctor fix -y\004\n"
        "\n",
    },
    {
        "install",
        "  \001shimback install\001 [\001--prefix\001 \002<dir>\002] "
        "[\001--shell\001 \002<shell>\002[,\002<shell>\002]... | \001--all\001]\n",

        "  \001install\001   Copy the running shimback binary to a stable, PATH-ed location\n"
        "            (default: ~/.local/bin) so shim symlinks (which point at wherever\n"
        "            the binary was running from at `add` time) survive a rebuild. Also\n"
        "            puts the shim directory on PATH (usually add/init's job) and\n"
        "            installs this man page (bundled, or downloaded if missing).\n"
        "\n"
        "              By default only sets up PATH for the current shell; \001--shell\001\n"
        "              takes a comma-separated list and \001--all\001 means every installed\n"
        "              shell.\n"
        "\n"
        "              \004e.g. shimback install --prefix ~/.local --shell zsh,bash\004\n"
        "\n",
    },
    {
        "uninstall",
        "  \001shimback uninstall\001 [\001--prefix\001 \002<dir>\002] [\001--full\001]\n",

        "  \001uninstall\001 Remove every shim symlink shimback created, the installed binary,\n"
        "            and the man page. \001--full\001 also clears the config file and the\n"
        "            PATH blocks in shell startup files (left alone by default).\n"
        "              \004e.g. shimback uninstall --full\004\n"
        "\n",
    },
    {
        "edit",
        "  \001shimback edit\001 [\002<name>\002]\n",

        "  \001edit\001      Open config.toml in \002$EDITOR\002, or, if unset, the first of \002nvim\002,\n"
        "            \002vim\002, \002vi\002, \002nano\002, \002pico\002 found on \002PATH\002 -- fails if none of\n"
        "            those are found either. With a shim \002<name>\002, opens whichever file\n"
        "            defines that shim instead: its split config file if it has one,\n"
        "            else config.toml. Warns (without failing) if the file no longer\n"
        "            parses once the editor exits successfully.\n"
        "              \004e.g. shimback edit\004\n"
        "              \004e.g. shimback edit sed\004\n"
        "\n",
    },
};

#define COMMAND_HELP_COUNT (sizeof(COMMAND_HELP) / sizeof(COMMAND_HELP[0]))

/* Maps an alias to the COMMAND_HELP entry that documents it -- both `rm`
 * and `remove` (same for `ls`/`list`) show identical help, matching
 * cli_run()'s own dispatch below. */
static const CommandHelp *find_command_help(const char *name) {
    if (strcmp(name, "rm") == 0) {
        name = "remove";
    } else if (strcmp(name, "ls") == 0) {
        name = "list";
    }
    for (size_t i = 0; i < COMMAND_HELP_COUNT; i++) {
        if (strcmp(COMMAND_HELP[i].name, name) == 0) {
            return &COMMAND_HELP[i];
        }
    }
    return NULL;
}

static void print_usage(void) {
    printf("shimback %s -- run a primary command, transparently fall back to another on failure\n"
           "\n",
           SHIMBACK_VERSION);
    bool colorize = stdout_is_color();
    print_markup(colorize, "\003USAGE:\003\n");
    for (size_t i = 0; i < COMMAND_HELP_COUNT; i++) {
        print_markup(colorize, COMMAND_HELP[i].usage);
    }
    print_markup(colorize, "  \001shimback --help\001 | \001--version\001\n\n\003COMMANDS:\003\n");
    for (size_t i = 0; i < COMMAND_HELP_COUNT; i++) {
        print_markup(colorize, COMMAND_HELP[i].description);
    }
    print_markup(colorize, "\004Copyright (c) 2026 pnavais. MIT OR Apache-2.0.\004\n");
}

/* `shimback <cmd> --help`/`-h` (clap-rs-style per-subcommand help,
 * checked anywhere in that command's own arguments -- see
 * argv_has_help_flag below): just that command's slice of print_usage()'s
 * combined output, so the two can never drift apart on wording. */
static void print_command_help(const CommandHelp *help) {
    printf("shimback %s -- run a primary command, transparently fall back to another on failure\n"
           "\n",
           SHIMBACK_VERSION);
    bool colorize = stdout_is_color();
    print_markup(colorize, "\003USAGE:\003\n");
    print_markup(colorize, help->usage);
    print_markup(colorize, "\n\003DESCRIPTION:\003\n");
    print_markup(colorize, help->description);
    print_markup(colorize,
                  "\004Run `shimback --help` to see every command.\004\n\n"
                  "\004Copyright (c) 2026 pnavais. MIT OR Apache-2.0.\004\n");
}

/* True if `-h`/`--help` appears anywhere in argv[0..argc-1] -- matching
 * clap-rs, where --help short-circuits regardless of position rather than
 * only being recognized as the very first/only argument. */
static bool argv_has_help_flag(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            return true;
        }
    }
    return false;
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

    /* `shimback <cmd> --help`/`-h` (clap-rs-style per-subcommand help),
     * checked before dispatch so it short-circuits regardless of where in
     * that command's own arguments the flag appears, and regardless of
     * whether the rest of the command line would otherwise be valid. */
    const CommandHelp *help = find_command_help(argv[1]);
    if (help && argv_has_help_flag(argc - 2, argv + 2)) {
        print_command_help(help);
        return 0;
    }

    if (strcmp(argv[1], "add") == 0) {
        return cmd_add(argc - 1, argv + 1);
    }
    if (strcmp(argv[1], "remove") == 0 || strcmp(argv[1], "rm") == 0) {
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
    if (strcmp(argv[1], "uninstall") == 0) {
        return cmd_uninstall(argc - 1, argv + 1);
    }
    if (strcmp(argv[1], "edit") == 0) {
        return cmd_edit(argc - 1, argv + 1);
    }

    fprintf(stderr, "shimback: unknown command '%s'\n", argv[1]);
    char *suggestion = fuzzy_suggest(argv[1], KNOWN_COMMANDS,
                                      sizeof(KNOWN_COMMANDS) / sizeof(KNOWN_COMMANDS[0]));
    if (suggestion) {
        print_suggestion_hint(suggestion);
        free(suggestion);
    }
    print_usage();
    return 1;
}
