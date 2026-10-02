#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "dispatch.h"
#include "paths.h"
#include "platform/platform.h"
#include "version.h"

/* Global (external linkage), not static, and referenced below from a
 * reachable code path -- both deliberate, so no optimization level can
 * treat this as dead and strip it from the compiled binary. See
 * SHIMBACK_BINARY_MARKER's own comment in version.h.in. */
const char shimback_binary_marker[] = SHIMBACK_BINARY_MARKER;

#ifdef _WIN32
/* Every Windows executable this project produces -- shimback.exe itself
 * and every shim (a hard link named "<name>.exe", see windows-port.md
 * Phase 3) -- carries a .exe suffix that argv[0]'s basename includes but
 * shim names themselves (as stored in config.toml, and as compared
 * against "shimback" below) never do. Stripped once, here, at the single
 * point every dispatch decision already funnels through, rather than at
 * every place downstream that would otherwise need to know about it. */
static const char *strip_exe_suffix(const char *name) {
    static char buf[4096];
    size_t len = strlen(name);
    if (len >= 4 && len < sizeof(buf) && _stricmp(name + len - 4, ".exe") == 0) {
        memcpy(buf, name, len - 4);
        buf[len - 4] = '\0';
        return buf;
    }
    snprintf(buf, sizeof(buf), "%s", name);
    return buf;
}
#endif

int main(int argc, char **argv) {
    if (argc < 1 || !argv[0]) {
        fprintf(stderr, "shimback: invalid invocation\n");
        return 1;
    }

    /* Referenced, not printed -- keeps the marker linked into the binary
     * without changing --version's actual output. */
    (void)shimback_binary_marker;

    plat_enable_vt_output();

    const char *name = path_basename(argv[0]);
#ifdef _WIN32
    name = strip_exe_suffix(name);
#endif
    if (strcmp(name, "shimback") == 0) {
        return cli_run(argc, argv);
    }
    return dispatch_run(name, argc, argv);
}
