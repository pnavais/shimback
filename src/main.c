#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "dispatch.h"

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int main(int argc, char **argv) {
    if (argc < 1 || !argv[0]) {
        fprintf(stderr, "shimback: invalid invocation\n");
        return 1;
    }

    const char *name = basename_of(argv[0]);
    if (strcmp(name, "shimback") == 0) {
        return cli_run(argc, argv);
    }
    return dispatch_run(name, argc, argv);
}
