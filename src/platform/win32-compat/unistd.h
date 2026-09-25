#ifndef SHIMBACK_WIN32_COMPAT_UNISTD_H
#define SHIMBACK_WIN32_COMPAT_UNISTD_H

/* Windows/UCRT has no <unistd.h> at all -- this resolves a bare
 * `#include <unistd.h>` in the existing source files (unchanged) to the
 * handful of POSIX names those files actually use from it, each mapped to
 * its UCRT equivalent. Only this directory is added to the include path,
 * and only for WIN32 builds (see CMakeLists.txt and win32-compat/getopt.h's
 * identical note), so this can never shadow a real system <unistd.h> on
 * macOS/Linux.
 *
 * Not a general-purpose <unistd.h> shim -- extend this only when the
 * compiler actually points at a missing symbol from it, the same way this
 * file's own current contents were arrived at. */

#include <direct.h> /* _getcwd, _rmdir */
#include <io.h>     /* _access, _close, _read, _unlink, _write */
#include <process.h> /* _getpid */

#define F_OK 0
#define W_OK 2
#define R_OK 4
/* UCRT's _access() has no execute-permission bit to check at all (Windows
 * has no such permission for a regular file) -- X_OK degrades to "exists",
 * matching F_OK. This means is_executable_file() (paths.c) doesn't yet
 * distinguish an executable from any other existing regular file on
 * Windows -- a known, tracked gap (see windows-port.md's "File metadata /
 * perms" seam entry: Windows executability is extension-based, not
 * permission-bit-based, and needs its own real check, not this stand-in). */
#define X_OK 0

#define access _access
#define close _close
#define getcwd _getcwd
#define getpid _getpid
#define read _read
#define rmdir _rmdir
#define unlink _unlink
#define write _write

#endif /* SHIMBACK_WIN32_COMPAT_UNISTD_H */
