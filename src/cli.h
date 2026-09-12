#ifndef SHIMBACK_CLI_H
#define SHIMBACK_CLI_H

/* Runs the shimback CLI itself (add/remove/init/list/--help/--version).
 * `argv[0]` is expected to be "shimback" (or a path ending in it). */
int cli_run(int argc, char **argv);

#endif /* SHIMBACK_CLI_H */
