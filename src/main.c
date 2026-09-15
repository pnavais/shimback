#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "dispatch.h"
#include "version.h"

/* Global (external linkage), not static, and referenced below from a
 * reachable code path -- both deliberate, so no optimization level can
 * treat this as dead and strip it from the compiled binary. See
 * SHIMBACK_BINARY_MARKER's own comment in version.h.in. */
const char shimback_binary_marker[] = SHIMBACK_BINARY_MARKER;

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int main(int argc, char **argv) {
    if (argc < 1 || !argv[0]) {
        fprintf(stderr, "shimback: invalid invocation\n");
        return 1;
    }

    /* Referenced, not printed -- keeps the marker linked into the binary
     * without changing --version's actual output. */
    (void)shimback_binary_marker;

    const char *name = basename_of(argv[0]);
    if (strcmp(name, "shimback") == 0) {
        return cli_run(argc, argv);
    }
    return dispatch_run(name, argc, argv);
}
