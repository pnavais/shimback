#ifndef SHIMBACK_COMMANDS_H
#define SHIMBACK_COMMANDS_H

/* Each handler receives its own argv, shifted so argv[0] is the subcommand
 * name itself (e.g. "add") -- the same convention getopt_long expects, and
 * the same one git/cargo-style subcommands use. */
int cmd_add(int argc, char **argv);
int cmd_remove(int argc, char **argv);
int cmd_init(int argc, char **argv);
int cmd_list(int argc, char **argv);

#endif /* SHIMBACK_COMMANDS_H */
