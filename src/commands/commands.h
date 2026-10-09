#ifndef SHIMBACK_COMMANDS_H
#define SHIMBACK_COMMANDS_H

/* Each handler receives its own argv, shifted so argv[0] is the subcommand
 * name itself (e.g. "add") -- the same convention getopt_long expects, and
 * the same one git/cargo-style subcommands use. */
int cmd_add(int argc, char **argv);
int cmd_remove(int argc, char **argv);
int cmd_init(int argc, char **argv);
int cmd_list(int argc, char **argv);
int cmd_doctor(int argc, char **argv);
int cmd_install(int argc, char **argv);
int cmd_uninstall(int argc, char **argv);
int cmd_edit(int argc, char **argv);
int cmd_info(int argc, char **argv);
int cmd_update(int argc, char **argv);
int cmd_export(int argc, char **argv);
int cmd_completions(int argc, char **argv);

/* Hidden plumbing command, not a real user-facing subcommand (see cli.c's
 * COMMANDS[] `hidden` field) -- prints every configured shim name, one per
 * line, for completion scripts to shell out to for remove/edit/info's
 * dynamic candidates. Ignores argv entirely and never fails: a config
 * load error means print nothing and exit 0, since this runs on every
 * Tab press and a noisy failure is worse than an empty completion list. */
int cmd_complete_names(int argc, char **argv);

#endif /* SHIMBACK_COMMANDS_H */
