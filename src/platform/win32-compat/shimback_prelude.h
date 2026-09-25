#ifndef SHIMBACK_WIN32_COMPAT_PRELUDE_H
#define SHIMBACK_WIN32_COMPAT_PRELUDE_H

/* Force-included (via /FI, see CMakeLists.txt) into every translation unit
 * on Windows -- unlike getopt.h/unistd.h, this doesn't stand in for a
 * missing header (<sys/stat.h> genuinely exists in the UCRT sysroot), so
 * it can't be resolved by adding a same-named file earlier on the include
 * path without risking shadowing real content from it. Force-include is
 * the standard alternative for "add a few missing macros to a header that
 * otherwise exists and is fine as-is".
 *
 * UCRT's own <sys/stat.h> defines S_IFREG/S_IFDIR (the raw mode bits) but
 * not S_ISREG()/S_ISDIR() (the POSIX convenience macros built on them) --
 * glibc and Darwin's libc provide both, so every existing S_ISREG/S_ISDIR
 * call site in this codebase (paths.c, several files under commands)
 * compiles unchanged once these exist. No S_ISLNK: Windows hard links have no
 * distinct file-type bit to check that way at all -- every S_ISLNK call
 * site is part of the Phase 3-deferred symlink/hard-link work (see
 * windows-port.md) and isn't expected to compile on Windows yet regardless. */
#include <sys/stat.h>
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

/* strtok_r has no Windows/UCRT name of its own, but Microsoft's own
 * strtok_s (not to be confused with C11 Annex K's differently-shaped
 * strtok_s) takes the exact same three arguments in the same order with
 * the same semantics -- a direct macro alias, not a behavior change. */
#include <string.h>
#ifndef strtok_r
#define strtok_r strtok_s
#endif

/* mode_t (normally from POSIX's <sys/types.h>, which doesn't exist on
 * Windows) shows up in a few portable function signatures (e.g.
 * paths.h's write_file_atomic) purely as an integer-sized permission-bits
 * parameter -- meaningless on Windows (see plat_chmod's own comment) but
 * still needed for those signatures to parse at all. */
#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef unsigned int mode_t;
#endif

/* ssize_t (POSIX, from <sys/types.h>) has no Windows/UCRT lowercase name --
 * BaseTsd.h's SSIZE_T (uppercase) is the same concept, so this just aliases
 * it rather than defining a fresh, possibly differently-sized type. */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#endif /* SHIMBACK_WIN32_COMPAT_PRELUDE_H */
