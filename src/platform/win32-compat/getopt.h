#ifndef SHIMBACK_WIN32_COMPAT_GETOPT_H
#define SHIMBACK_WIN32_COMPAT_GETOPT_H

/* Minimal getopt_long() for the Windows build -- the Microsoft UCRT headers
 * clang-cl compiles against have no getopt of any kind, unlike glibc/
 * Darwin's libc on the platforms this project already supports. Vendored
 * rather than assumed, since this is the one CLI-parsing primitive with no
 * Windows SDK equivalent at all (see windows-port.md's toolchain
 * discussion). Only this directory is added to the include path, and only
 * for WIN32 builds (see CMakeLists.txt), so a plain `#include <getopt.h>`
 * in the existing source files resolves here automatically with no source
 * changes needed, and this header can never shadow a real system one on
 * macOS/Linux.
 *
 * Implements exactly the subset this codebase actually uses (see every
 * `getopt_long` call site under src/commands/): `required_argument` and
 * `no_argument` (no `optional_argument`), every long option's `flag` field
 * always NULL (val is always returned directly, never written through a
 * flag pointer), and GNU-style permutation (options and positional
 * arguments may be interleaved on the command line; argv is reordered in
 * place so a positional argument always ends up at `argv[optind]` once
 * option parsing is done -- src/commands/add.c:779 relies on exactly this).
 * Long-option matching is exact-name only, not glibc's unambiguous-prefix
 * abbreviation -- a deliberate, minor simplification, not a bug: nothing in
 * this codebase's own usage relies on abbreviating a long option. */

#ifdef __cplusplus
extern "C" {
#endif

struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

#define no_argument 0
#define required_argument 1
#define optional_argument 2

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt_long(int argc, char *const argv[], const char *optstring, const struct option *longopts,
                 int *longindex);

#ifdef __cplusplus
}
#endif

#endif /* SHIMBACK_WIN32_COMPAT_GETOPT_H */
