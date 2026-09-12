#ifndef SHIMBACK_DISPATCH_H
#define SHIMBACK_DISPATCH_H

/* Runs the shim named `shim_name` (the basename shimback was invoked as),
 * forwarding argv[1..argc-1] to whichever of source/fallback ends up
 * running. Returns the process exit code to use (never exits directly, so
 * main() stays the single place that calls exit()). */
int dispatch_run(const char *shim_name, int argc, char **argv);

#endif /* SHIMBACK_DISPATCH_H */
