#ifndef SHIMBACK_CLI_H
#define SHIMBACK_CLI_H

#include <stddef.h>

/* Runs the shimback CLI itself (add/remove/init/list/--help/--version).
 * `argv[0]` is expected to be "shimback" (or a path ending in it). */
int cli_run(int argc, char **argv);

/* Iterate i = 0, 1, ... until NULL for every non-hidden subcommand's own
 * name (not an alias) -- for anything (e.g. `completions`) that needs the
 * live command list without duplicating cli.c's own COMMANDS[] table.
 * cli_command_alias(i) is NULL if that command has none. Indices line up:
 * cli_command_alias(i) is always the alias (or lack of one) for the same
 * command cli_command_name(i) named. */
const char *cli_command_name(size_t i);
const char *cli_command_alias(size_t i);

#endif /* SHIMBACK_CLI_H */
