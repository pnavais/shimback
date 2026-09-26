#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ctype.h>   /* tolower */
#include <direct.h>  /* _wmkdir, _wgetcwd */
#include <errno.h>
#include <fcntl.h>   /* _O_* */
#include <io.h>      /* _isatty, _wsopen_s, _mktemp_s, _close */
#include <share.h>   /* _SH_DENYNO */
#include <shlobj.h>  /* SHGetKnownFolderPath, FOLDERID_Profile/FOLDERID_Documents */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>  /* _S_IREAD, _S_IWRITE */
#include <tlhelp32.h>  /* CreateToolhelp32Snapshot, PROCESSENTRY32W */

#include "platform.h"
#include "../util.h"

/* --- UTF-8 <-> UTF-16 conversion -----------------------------------------
 * Every string this codebase passes across the platform boundary is UTF-8
 * (the same convention it already uses on POSIX, where paths are just
 * locale-independent byte strings) -- the Win32 API's "W" (wide) functions
 * are used throughout this file rather than the "A" (ANSI/codepage) ones,
 * specifically so usernames/paths containing non-ASCII characters work
 * correctly, and these two helpers are the only place that conversion
 * happens. Dies on allocation failure (matching xmalloc's convention
 * elsewhere); returns NULL for a genuine encoding failure (invalid UTF-8,
 * or a wide string that isn't valid UTF-16), which is a real, recoverable
 * condition callers are expected to handle like any other plat_* failure. */
static wchar_t *utf8_to_wide(const char *s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (len <= 0) {
        return NULL;
    }
    wchar_t *w = xmalloc((size_t)len * sizeof(wchar_t));
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, len) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

static char *wide_to_utf8(const wchar_t *w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (len <= 0) {
        return NULL;
    }
    char *s = xmalloc((size_t)len);
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, len, NULL, NULL) <= 0) {
        free(s);
        return NULL;
    }
    return s;
}

/* --- Process spawn (Phase 2) ----------------------------------------------
 * CreateProcessW + GetExitCodeProcess replace fork/execv/waitpid. Windows
 * has no fork()-then-exec() split -- CreateProcessW does both atomically --
 * so the POSIX "fork failed" (die-worthy, -1) vs. "execv failed in the
 * child" (looks like a normal exit-127, not fatal) distinction collapses
 * differently here: a CreateProcessW failure (the exe genuinely couldn't be
 * started -- not found, not executable, etc.) is the common, expected-to-
 * happen case and maps to the exit-127-with-message path, exactly mirroring
 * what run_inherited's execv-failure branch already looked like to callers
 * on POSIX. -1 is reserved for something failing *after* the process
 * already started (WaitForSingleObject/GetExitCodeProcess itself failing),
 * which is the genuine Windows analogue of POSIX's "waitpid failed". */

/* CreateProcessW takes one command-line *string*, not an argv array -- the
 * child re-parses it into its own argv using the C runtime's documented
 * quoting rules, so the parent must escape exactly the way the child will
 * un-escape, or arguments containing spaces/quotes/backslashes get split
 * wrong. This is the standard, documented algorithm (see Microsoft's "C++
 * Command-Line Arguments" docs): a run of N backslashes followed by a quote
 * becomes 2N+1 backslashes plus an escaped quote; a run of N backslashes at
 * the very end (right before the closing wrapping quote) becomes 2N: */
static void append_quoted_arg(DynBuf *buf, const char *arg) {
    if (buf->len > 0) {
        dynbuf_append_char(buf, ' ');
    }
    if (arg[0] != '\0' && strpbrk(arg, " \t\n\v\"") == NULL) {
        dynbuf_append_str(buf, arg);
        return;
    }
    dynbuf_append_char(buf, '"');
    for (const char *p = arg;; p++) {
        size_t backslashes = 0;
        while (*p == '\\') {
            backslashes++;
            p++;
        }
        if (*p == '\0') {
            for (size_t i = 0; i < backslashes * 2; i++) {
                dynbuf_append_char(buf, '\\');
            }
            break;
        } else if (*p == '"') {
            for (size_t i = 0; i < backslashes * 2 + 1; i++) {
                dynbuf_append_char(buf, '\\');
            }
            dynbuf_append_char(buf, '"');
        } else {
            for (size_t i = 0; i < backslashes; i++) {
                dynbuf_append_char(buf, '\\');
            }
            dynbuf_append_char(buf, *p);
        }
    }
    dynbuf_append_char(buf, '"');
}

/* True for a target CreateProcessW can't run directly: .bat/.cmd aren't
 * real PE executables, so passing one as lpApplicationName makes Windows
 * silently re-exec through cmd.exe on its own -- see build_command_line's
 * own comment for why that matters here. */
static bool is_batch_file(const char *exe) {
    size_t len = strlen(exe);
    return (len >= 4 && _stricmp(exe + len - 4, ".bat") == 0) ||
           (len >= 4 && _stricmp(exe + len - 4, ".cmd") == 0);
}

/* CreateProcessW may write through this buffer (it's documented as
 * mutable), so the caller must not treat the result as read-only -- it
 * never is here anyway, since it's freed right after the call.
 *
 * `exe` (the same resolved path passed as lpApplicationName) is needed
 * here, not just `argv`, because of a genuine Windows quirk found the hard
 * way: for a .bat/.cmd target, CreateProcessW's own implicit re-exec
 * through cmd.exe (since a batch file isn't a real PE executable) uses the
 * command line's *first token* to decide what to run, not the
 * lpApplicationName path actually passed to it -- confirmed directly with
 * a minimal CreateProcessW repro (mismatched app/cmdline first token: the
 * mismatched name failed with cmd.exe's "not recognized" error; making
 * them match fixed it). Every other call site here builds argv[0] as the
 * shim's own invoked name (see dispatch.c's build_argv), which is exactly
 * what breaks this -- any shim wrapping a .bat/.cmd source or fallback
 * (very common in the Node ecosystem: npm.cmd, yarn.cmd, tsc.cmd, ...)
 * would otherwise always fail, trying to run its own name as a command.
 * Fixed by substituting `exe` itself for argv[0] in that one case --
 * lossless, since a batch script has no way to observe a spoofed argv[0]
 * (via %0) differently from its own real invocation path anyway. */
static wchar_t *build_command_line(const char *exe, char *const argv[]) {
    DynBuf buf;
    dynbuf_init(&buf);
    bool batch = is_batch_file(exe);
    for (int i = 0; argv[i]; i++) {
        append_quoted_arg(&buf, (i == 0 && batch) ? exe : argv[i]);
    }
    wchar_t *w = utf8_to_wide(dynbuf_cstr(&buf));
    dynbuf_free(&buf);
    return w;
}

static char *win32_error_message(DWORD err) {
    wchar_t *msg = NULL;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                  FORMAT_MESSAGE_IGNORE_INSERTS,
                              NULL, err, 0, (LPWSTR)&msg, 0, NULL);
    char *result = NULL;
    if (n > 0 && msg) {
        while (n > 0 && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n')) {
            msg[--n] = L'\0';
        }
        result = wide_to_utf8(msg);
    }
    if (msg) {
        LocalFree(msg);
    }
    if (!result) {
        result = xmalloc(32);
        snprintf(result, 32, "error %lu", (unsigned long)err);
    }
    return result;
}

/* Maps a GetLastError() code to the closest errno value, for the several
 * plat_* functions (plat_link_create, plat_rename_replace) whose callers
 * report failure via the existing `strerror(errno)` call sites this
 * codebase already has throughout -- those raw Win32 APIs (CreateHardLinkW,
 * MoveFileExW) only ever set GetLastError(), never the CRT errno, so
 * without this translation `strerror(errno)` would report stale, unrelated
 * garbage left over from some earlier call instead of the real reason.
 * Not exhaustive -- covers the cases these two callers can actually hit;
 * anything else falls back to a generic, still-truthful EIO. */
static void set_errno_from_win32(DWORD err) {
    switch (err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            errno = ENOENT;
            break;
        case ERROR_ACCESS_DENIED:
            errno = EACCES;
            break;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            errno = EEXIST;
            break;
        case ERROR_NOT_SAME_DEVICE:
            /* The specific, documented reason CreateHardLinkW refuses a
             * cross-volume link -- EXDEV is exactly what a POSIX link(2)
             * reports for the identical condition. */
            errno = EXDEV;
            break;
        case ERROR_INVALID_PARAMETER:
            errno = EINVAL;
            break;
        default:
            errno = EIO;
            break;
    }
}

int plat_run_inherited(const char *exe, char *const argv[]) {
    wchar_t *wexe = utf8_to_wide(exe);
    wchar_t *cmdline = build_command_line(exe, argv);
    if (!wexe || !cmdline) {
        free(wexe);
        free(cmdline);
        return -1;
    }

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    /* bInheritHandles=FALSE: nothing here needs to hand the child any
     * specific handle (unlike the captured variants below) -- a console
     * child still shares the parent's console for stdin/stdout/stderr
     * regardless, via Windows' normal console-inheritance, not handle
     * inheritance, so this is both correct and avoids leaking any of the
     * parent's other open handles into the child unnecessarily. */
    BOOL ok = CreateProcessW(wexe, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    free(cmdline);
    free(wexe);
    if (!ok) {
        char *msg = win32_error_message(GetLastError());
        fprintf(stderr, "shimback: exec %s: %s\n", exe, msg);
        free(msg);
        return 127;
    }
    CloseHandle(pi.hThread);

    if (WaitForSingleObject(pi.hProcess, INFINITE) != WAIT_OBJECT_0) {
        CloseHandle(pi.hProcess);
        return -1;
    }
    DWORD exit_code = 0;
    BOOL got = GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    return got ? (int)exit_code : -1;
}

/* Anonymous pipes (CreatePipe) can't be waited on via
 * WaitForMultipleObjects the way a POSIX fd can via poll() -- they don't
 * support overlapped I/O at all. The alternative that does (named pipes
 * with FILE_FLAG_OVERLAPPED) is substantially more machinery for what this
 * needs: PeekNamedPipe-based polling is simpler, still fully correct (no
 * data is ever missed -- PeekNamedPipe never consumes bytes, it only
 * reports how many are available), and the poll interval is irrelevant at
 * the scale this runs at (a CLI tool's trial-run capture, not a
 * high-throughput server). */
#define SHIMBACK_PIPE_POLL_MS 15

int plat_run_captured(const char *exe, char *const argv[], DynBuf *out, DynBuf *err,
                       int timeout_ms, size_t limit_bytes, bool *committed_live) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE out_read, out_write;
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) {
        return -1;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    HANDLE err_read, err_write;
    if (!CreatePipe(&err_read, &err_write, &sa, 0)) {
        CloseHandle(out_read);
        CloseHandle(out_write);
        return -1;
    }
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    wchar_t *wexe = utf8_to_wide(exe);
    wchar_t *cmdline = build_command_line(exe, argv);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_write;
    si.hStdError = err_write;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = (wexe && cmdline)
                  ? CreateProcessW(wexe, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)
                  : FALSE;
    DWORD create_err = ok ? 0 : GetLastError();
    free(cmdline);
    free(wexe);

    /* The parent's copies of the write ends must close regardless of
     * success -- on success, so EOF is ever detectable once the child's
     * own copies close; on failure, just to release them. */
    CloseHandle(out_write);
    CloseHandle(err_write);

    if (!ok) {
        CloseHandle(out_read);
        CloseHandle(err_read);
        char *msg = win32_error_message(create_err);
        fprintf(stderr, "shimback: exec %s: %s\n", exe, msg);
        free(msg);
        return 127;
    }
    CloseHandle(pi.hThread);

    ULONGLONG start = GetTickCount64();
    bool out_done = false;
    bool err_done = false;
    bool live = false;
    char chunk[4096];
    while (!out_done || !err_done) {
        if (!live) {
            ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= (ULONGLONG)timeout_ms || out->len + err->len >= limit_bytes) {
                fwrite(out->data, 1, out->len, stdout);
                fwrite(err->data, 1, err->len, stderr);
                fflush(stdout);
                fflush(stderr);
                live = true;
                *committed_live = true;
            }
        }

        bool progressed = false;
        HANDLE streams[2] = {out_read, err_read};
        bool *dones[2] = {&out_done, &err_done};
        DynBuf *bufs[2] = {out, err};
        FILE *live_targets[2] = {stdout, stderr};
        for (int i = 0; i < 2; i++) {
            if (*dones[i]) {
                continue;
            }
            DWORD avail = 0;
            if (!PeekNamedPipe(streams[i], NULL, 0, NULL, &avail, NULL)) {
                *dones[i] = true; /* write end closed -- EOF */
                continue;
            }
            if (avail == 0) {
                continue;
            }
            DWORD n = 0;
            if (!ReadFile(streams[i], chunk, sizeof(chunk), &n, NULL) || n == 0) {
                *dones[i] = true;
                continue;
            }
            progressed = true;
            if (live) {
                fwrite(chunk, 1, n, live_targets[i]);
                fflush(live_targets[i]);
            } else {
                dynbuf_append(bufs[i], chunk, n);
            }
        }
        if (!progressed && (!out_done || !err_done)) {
            Sleep(SHIMBACK_PIPE_POLL_MS);
        }
    }

    CloseHandle(out_read);
    CloseHandle(err_read);

    if (WaitForSingleObject(pi.hProcess, INFINITE) != WAIT_OBJECT_0) {
        CloseHandle(pi.hProcess);
        return -1;
    }
    DWORD exit_code = 0;
    BOOL got = GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    return got ? (int)exit_code : -1;
}

long plat_capture_stdout(const char *exe, char *const argv[], char *buf, size_t bufcap) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE read_h, write_h;
    if (!CreatePipe(&read_h, &write_h, &sa, 0)) {
        return -1;
    }
    SetHandleInformation(read_h, HANDLE_FLAG_INHERIT, 0);

    /* Best-effort, matching the POSIX version's "if (devnull >= 0)"
     * spirit -- if this fails, the child's stderr falls back to the
     * parent's own, which is not ideal but not a reason to abort. */
    HANDLE devnull = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    wchar_t *wexe = utf8_to_wide(exe);
    wchar_t *cmdline = build_command_line(exe, argv);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_h;
    si.hStdError = (devnull != INVALID_HANDLE_VALUE) ? devnull : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = (wexe && cmdline)
                  ? CreateProcessW(wexe, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)
                  : FALSE;
    free(cmdline);
    free(wexe);
    CloseHandle(write_h);
    if (devnull != INVALID_HANDLE_VALUE) {
        CloseHandle(devnull);
    }

    if (!ok) {
        CloseHandle(read_h);
        return -1;
    }
    CloseHandle(pi.hThread);

    size_t len = 0;
    while (len < bufcap - 1) {
        DWORD n = 0;
        if (!ReadFile(read_h, buf + len, (DWORD)(bufcap - 1 - len), &n, NULL) || n == 0) {
            break;
        }
        len += n;
    }
    CloseHandle(read_h);
    buf[len] = '\0';

    DWORD wait_rc = WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    bool got_exit = wait_rc == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);

    if (len == 0 || !got_exit || exit_code != 0) {
        return -1;
    }
    return (long)len;
}

/* --- Filesystem / environment ---------------------------------------- */

char *plat_home_dir(void) {
    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n > 0 && n < sizeof(buf) / sizeof(buf[0])) {
        char *result = wide_to_utf8(buf);
        if (result) {
            return result;
        }
    }
    /* USERPROFILE unset or unreadable as UTF-8 -- fall back to the shell's
     * own notion of the profile folder. */
    PWSTR known_path = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &known_path) == S_OK) {
        char *result = wide_to_utf8(known_path);
        CoTaskMemFree(known_path);
        if (result) {
            return result;
        }
    }
    die("could not determine home directory (%%USERPROFILE%% is unset and the shell's own "
        "profile-folder lookup failed)");
    return NULL; /* unreachable */
}

char *plat_self_exe_path(void) {
    wchar_t stack_buf[MAX_PATH];
    wchar_t *buf = stack_buf;
    size_t cap = MAX_PATH;
    wchar_t *heap_buf = NULL;
    for (;;) {
        DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)cap);
        if (n == 0) {
            die("failed to resolve the shimback executable's own path");
        }
        if (n < cap) {
            break; /* fit, not truncated */
        }
        cap *= 2;
        heap_buf = xrealloc(heap_buf, cap * sizeof(wchar_t));
        buf = heap_buf;
    }
    char *result = wide_to_utf8(buf);
    free(heap_buf);
    if (!result) {
        die("failed to convert the shimback executable's own path to UTF-8");
    }
    return result;
}

char **plat_list_dir(const char *dir) {
    char pattern_utf8[4160];
    snprintf(pattern_utf8, sizeof(pattern_utf8), "%s\\*", dir);
    wchar_t *pattern = utf8_to_wide(pattern_utf8);
    if (!pattern) {
        return NULL;
    }

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }

    char **names = NULL;
    size_t count = 0;
    size_t cap = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        char *name = wide_to_utf8(fd.cFileName);
        if (!name) {
            continue; /* skip an entry whose name isn't valid UTF-16/UTF-8
                       * round-trippable, rather than fail the whole listing */
        }
        if (count + 1 >= cap) {
            cap = cap == 0 ? 8 : cap * 2;
            names = xrealloc(names, cap * sizeof(char *));
        }
        names[count++] = name;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (!names) {
        names = xmalloc(sizeof(char *));
    }
    names[count] = NULL;
    return names;
}

void plat_free_dir_entries(char **entries) {
    if (!entries) {
        return;
    }
    for (char **p = entries; *p; p++) {
        free(*p);
    }
    free(entries);
}

int plat_mkstemp(char *template_path) {
    /* _mktemp_s only rewrites the trailing "XXXXXX" -- it doesn't itself
     * guarantee the result doesn't already exist (unlike POSIX mkstemp()),
     * so the actual atomicity/exclusivity guarantee comes from opening
     * with _O_CREAT|_O_EXCL below and retrying on a collision. */
    char original[4160];
    strncpy_s(original, sizeof(original), template_path, _TRUNCATE);

    for (int attempt = 0; attempt < 10; attempt++) {
        char candidate[4160];
        strncpy_s(candidate, sizeof(candidate), original, _TRUNCATE);
        if (_mktemp_s(candidate, strlen(candidate) + 1) != 0) {
            return -1;
        }
        wchar_t *wpath = utf8_to_wide(candidate);
        if (!wpath) {
            return -1;
        }
        int fd = -1;
        errno_t e = _wsopen_s(&fd, wpath, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _SH_DENYNO,
                              _S_IREAD | _S_IWRITE);
        free(wpath);
        if (e == 0) {
            strncpy_s(template_path, 4160, candidate, _TRUNCATE);
            return fd;
        }
        if (e != EEXIST) {
            return -1;
        }
        /* collision -- loop and try _mktemp_s again from the original template */
    }
    return -1;
}

char *plat_mkdtemp(char *template_path) {
    char original[4160];
    strncpy_s(original, sizeof(original), template_path, _TRUNCATE);

    for (int attempt = 0; attempt < 10; attempt++) {
        char candidate[4160];
        strncpy_s(candidate, sizeof(candidate), original, _TRUNCATE);
        if (_mktemp_s(candidate, strlen(candidate) + 1) != 0) {
            return NULL;
        }
        wchar_t *wpath = utf8_to_wide(candidate);
        if (!wpath) {
            return NULL;
        }
        int rc = _wmkdir(wpath);
        errno_t e = errno;
        free(wpath);
        if (rc == 0) {
            strncpy_s(template_path, 4160, candidate, _TRUNCATE);
            return template_path;
        }
        if (e != EEXIST) {
            return NULL;
        }
        /* collision -- loop and try _mktemp_s again from the original template */
    }
    return NULL;
}

bool plat_chmod(const char *path, int mode) {
    /* Best-effort no-op, by design -- see platform.h. */
    (void)path;
    (void)mode;
    return true;
}

bool plat_fchmod(int fd, int mode) {
    (void)fd;
    (void)mode;
    return true;
}

bool plat_rename_replace(const char *from, const char *to) {
    wchar_t *wfrom = utf8_to_wide(from);
    wchar_t *wto = to ? utf8_to_wide(to) : NULL;
    if (!wfrom || !wto) {
        free(wfrom);
        free(wto);
        errno = EINVAL;
        return false;
    }
    bool ok = MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) {
        set_errno_from_win32(GetLastError());
    }
    free(wfrom);
    free(wto);
    return ok;
}

int plat_mkdir(const char *path) {
    wchar_t *w = utf8_to_wide(path);
    if (!w) {
        errno = EINVAL;
        return -1;
    }
    int rc = _wmkdir(w); /* a CRT call -- sets the usual errno (EEXIST, etc.)
                           * itself, unlike the raw CreateDirectoryW Win32 API,
                           * which only sets GetLastError(). */
    free(w);
    return rc;
}

char *plat_getcwd(void) {
    wchar_t *w = _wgetcwd(NULL, 0); /* _wgetcwd allocates its own buffer when
                                      * given NULL, a documented UCRT
                                      * behavior (unlike POSIX getcwd(NULL,0),
                                      * which is technically unspecified). */
    if (!w) {
        return NULL;
    }
    char *result = wide_to_utf8(w);
    free(w);
    return result;
}

int plat_lockfile_open(const char *path, int *out_errno) {
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) {
        if (out_errno) {
            *out_errno = EINVAL;
        }
        return -1;
    }
    int fd = -1;
    errno_t e =
        _wsopen_s(&fd, wpath, _O_CREAT | _O_RDWR | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE);
    free(wpath);
    if (e != 0) {
        if (out_errno) {
            *out_errno = e; /* _wsopen_s's errno_t already matches the
                              * standard errno values (ENOENT etc.) callers
                              * compare against. */
        }
        return -1;
    }

    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov)) {
        if (out_errno) {
            *out_errno = EIO;
        }
        _close(fd);
        return -1;
    }
    return fd;
}

void plat_lockfile_close(int handle) {
    if (handle >= 0) {
        /* Closing the handle releases the LockFileEx lock too -- same net
         * effect as POSIX's flock(LOCK_UN) + close(). */
        _close(handle);
    }
}

bool plat_isatty_stdin(void) {
    return _isatty(_fileno(stdin)) != 0;
}

bool plat_isatty_stdout(void) {
    return _isatty(_fileno(stdout)) != 0;
}

bool plat_isatty_stderr(void) {
    return _isatty(_fileno(stderr)) != 0;
}

/* --- Terminal raw mode (Phase 6) --------------------------------------
 * GetConsoleMode/SetConsoleMode replace termios; tui_read_key()'s escape-
 * sequence parsing itself is portable as-is (see platform.h) -- these four
 * just need to feed it the same byte stream a real POSIX terminal would.
 *
 * Deliberately built on ReadConsoleInputW's raw INPUT_RECORD stream and a
 * small internal byte queue, translating each key event into bytes here,
 * rather than on ENABLE_VIRTUAL_TERMINAL_INPUT's ReadFile-level ANSI
 * translation: that would also require distinguishing an actual VT byte
 * stream from window-resize/focus-change events some other way, and its
 * exact behavior isn't consistent between conhost.exe and Windows
 * Terminal. Reading INPUT_RECORDs directly and translating them by hand
 * sidesteps both: non-KEY_EVENT records (and key-up events) are simply
 * discarded before they ever reach the queue. */

static HANDLE g_console_in = NULL;
static DWORD g_saved_console_mode = 0;
static bool g_have_saved_console_mode = false;
static bool g_raw_mode_active = false;

static void restore_console_mode_at_exit(void) {
    if (g_raw_mode_active && g_have_saved_console_mode) {
        SetConsoleMode(g_console_in, g_saved_console_mode);
        g_raw_mode_active = false;
    }
}

bool plat_tty_raw_enter(void) {
    if (g_raw_mode_active) {
        return true;
    }
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == NULL) {
        return false;
    }
    DWORD mode;
    if (!GetConsoleMode(h, &mode)) {
        return false; /* not a real console (redirected/piped stdin) */
    }
    if (!g_have_saved_console_mode) {
        g_saved_console_mode = mode;
        g_have_saved_console_mode = true;
        atexit(restore_console_mode_at_exit);
    }
    DWORD raw_mode = mode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    if (!SetConsoleMode(h, raw_mode)) {
        return false;
    }
    g_console_in = h;
    g_raw_mode_active = true;
    return true;
}

void plat_tty_raw_exit(void) {
    if (!g_raw_mode_active || !g_have_saved_console_mode) {
        return;
    }
    SetConsoleMode(g_console_in, g_saved_console_mode);
    g_raw_mode_active = false;
}

#define TTY_QUEUE_CAP 8
static unsigned char g_tty_queue[TTY_QUEUE_CAP];
static size_t g_tty_queue_len = 0;
static size_t g_tty_queue_pos = 0;

static void tty_queue_reset(void) {
    g_tty_queue_len = 0;
    g_tty_queue_pos = 0;
}

static void tty_queue_push(unsigned char b) {
    if (g_tty_queue_len < TTY_QUEUE_CAP) {
        g_tty_queue[g_tty_queue_len++] = b;
    }
}

/* UTF-8-encodes one UTF-16 code unit into the queue, mirroring what a real
 * terminal would send over the wire for the same keystroke. A lone
 * surrogate half (a non-BMP character, e.g. an emoji) is dropped rather
 * than reassembled -- tui_read_key() only ever recognizes plain ASCII and
 * a handful of control bytes anyway, so there is nothing a caller could
 * do with it even if it were decoded correctly. */
static void tty_queue_push_utf8(wchar_t wc) {
    unsigned int cp = (unsigned int)wc;
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        return;
    }
    if (cp <= 0x7F) {
        tty_queue_push((unsigned char)cp);
    } else if (cp <= 0x7FF) {
        tty_queue_push((unsigned char)(0xC0 | (cp >> 6)));
        tty_queue_push((unsigned char)(0x80 | (cp & 0x3F)));
    } else {
        tty_queue_push((unsigned char)(0xE0 | (cp >> 12)));
        tty_queue_push((unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
        tty_queue_push((unsigned char)(0x80 | (cp & 0x3F)));
    }
}

/* Reads and translates INPUT_RECORDs until the queue has at least one byte
 * (blocking), or returns false if the console handle is gone or a read
 * genuinely fails. Arrow keys become the same 3-byte "ESC [ <letter>"
 * sequence tui_read_key() already parses on POSIX; anything else with a
 * printable character comes from the key event's own UnicodeChar (which
 * Windows already delivers as 0x03 for Ctrl-C, 0x0d for Enter, and 0x08
 * for Backspace with ENABLE_PROCESSED_INPUT off, matching what
 * tui_read_key() expects without any special-casing here). A key-down
 * event with neither (a bare modifier, a function key, ...) contributes
 * nothing and is skipped. */
static bool tty_fill_queue(void) {
    if (g_tty_queue_pos < g_tty_queue_len) {
        return true;
    }
    tty_queue_reset();
    if (!g_console_in) {
        return false;
    }
    for (;;) {
        INPUT_RECORD rec;
        DWORD n = 0;
        if (!ReadConsoleInputW(g_console_in, &rec, 1, &n) || n == 0) {
            return false;
        }
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) {
            continue;
        }
        const KEY_EVENT_RECORD *ke = &rec.Event.KeyEvent;
        switch (ke->wVirtualKeyCode) {
            case VK_UP: tty_queue_push(0x1b); tty_queue_push('['); tty_queue_push('A'); break;
            case VK_DOWN: tty_queue_push(0x1b); tty_queue_push('['); tty_queue_push('B'); break;
            case VK_RIGHT: tty_queue_push(0x1b); tty_queue_push('['); tty_queue_push('C'); break;
            case VK_LEFT: tty_queue_push(0x1b); tty_queue_push('['); tty_queue_push('D'); break;
            default:
                if (ke->uChar.UnicodeChar != 0) {
                    tty_queue_push_utf8(ke->uChar.UnicodeChar);
                }
                break;
        }
        if (g_tty_queue_len > 0) {
            return true;
        }
    }
}

bool plat_stdin_byte_ready(int ms) {
    if (g_tty_queue_pos < g_tty_queue_len) {
        return true;
    }
    if (!g_console_in) {
        return false;
    }
    DWORD start = GetTickCount();
    for (;;) {
        DWORD wr = WaitForSingleObject(g_console_in, ms < 0 ? INFINITE : (DWORD)ms);
        if (wr != WAIT_OBJECT_0) {
            return false; /* timeout, or the wait itself failed */
        }
        INPUT_RECORD rec;
        DWORD n = 0;
        if (!PeekConsoleInputW(g_console_in, &rec, 1, &n) || n == 0) {
            return false;
        }
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            return true;
        }
        /* The console handle is signaled for ANY unread input record, not
         * just genuine keystrokes (window resize, focus change, and
         * key-up events all count) -- drain this one so it stops keeping
         * the handle signaled, then keep waiting out whatever time budget
         * is left. */
        ReadConsoleInputW(g_console_in, &rec, 1, &n);
        if (ms >= 0) {
            DWORD elapsed = GetTickCount() - start;
            if (elapsed >= (DWORD)ms) {
                return false;
            }
            ms = (int)((DWORD)ms - elapsed);
        }
    }
}

bool plat_read_stdin_byte(char *out) {
    if (!tty_fill_queue()) {
        return false;
    }
    *out = (char)g_tty_queue[g_tty_queue_pos++];
    return true;
}

/* --- ANSI/VT output (Phase 6) ------------------------------------------
 * Modern Windows consoles (10+) and Windows Terminal understand the same
 * ANSI escape sequences already used throughout this codebase for color
 * (util.c's ANSI_* constants) and the add wizard's cursor movement/clear-
 * screen -- but conhost.exe only actually interprets them once
 * ENABLE_VIRTUAL_TERMINAL_PROCESSING is turned on for the output handle;
 * without it, they print as literal, garbled escape bytes. Called once
 * from main(), before any output -- unconditionally safe to call even
 * when stdout/stderr aren't real consoles (redirected to a file/pipe):
 * GetConsoleMode simply fails there and this becomes a no-op, consistent
 * with plat_isatty_stdout/stderr already gating whether any ANSI is
 * emitted in the first place. */
void plat_enable_vt_output(void) {
    HANDLE handles[2] = {GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
    for (size_t i = 0; i < 2; i++) {
        HANDLE h = handles[i];
        if (h == INVALID_HANDLE_VALUE || h == NULL) {
            continue;
        }
        DWORD mode;
        if (GetConsoleMode(h, &mode)) {
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
}

bool plat_path_is_symlink(const char *path) {
    (void)path;
    return false; /* see platform.h's own comment on this function */
}

bool plat_link_create(const char *target, const char *link_path) {
    wchar_t *wtarget = utf8_to_wide(target);
    wchar_t *wlink = utf8_to_wide(link_path);
    if (!wtarget || !wlink) {
        free(wtarget);
        free(wlink);
        errno = EINVAL;
        return false;
    }
    bool ok = CreateHardLinkW(wlink, wtarget, NULL) != 0;
    if (!ok) {
        set_errno_from_win32(GetLastError());
    }
    free(wtarget);
    free(wlink);
    return ok;
}

char *plat_realpath(const char *path) {
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) {
        return NULL;
    }
    /* FILE_FLAG_BACKUP_SEMANTICS: the only documented way to get a HANDLE
     * to a directory with CreateFileW, which realpath() also needs to
     * support (it resolves directories, not just regular files). */
    HANDLE h = CreateFileW(wpath, FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }

    wchar_t stack_buf[MAX_PATH];
    wchar_t *buf = stack_buf;
    DWORD cap = MAX_PATH;
    wchar_t *heap_buf = NULL;
    DWORD n = GetFinalPathNameByHandleW(h, buf, cap, FILE_NAME_NORMALIZED);
    if (n >= cap) {
        cap = n + 1;
        heap_buf = xmalloc(cap * sizeof(wchar_t));
        buf = heap_buf;
        n = GetFinalPathNameByHandleW(h, buf, cap, FILE_NAME_NORMALIZED);
    }
    CloseHandle(h);
    if (n == 0) {
        free(heap_buf);
        return NULL;
    }

    /* GetFinalPathNameByHandleW prepends "\\?\" (or "\\?\UNC\" for a UNC
     * path) -- stripped here so the result looks like an ordinary path,
     * matching what realpath() returns and what the rest of this codebase
     * expects to display and string-compare. */
    wchar_t *result_start = buf;
    if (wcsncmp(buf, L"\\\\?\\UNC\\", 8) == 0) {
        result_start = buf + 6;
        result_start[0] = L'\\'; /* "\\?\UNC\server\share" -> "\\server\share" */
    } else if (wcsncmp(buf, L"\\\\?\\", 4) == 0) {
        result_start = buf + 4;
    }
    char *result = wide_to_utf8(result_start);
    free(heap_buf);
    return result;
}

/* --- Shell/PATH integration (Phase 5) -------------------------------- */

char *plat_documents_dir(void) {
    PWSTR known_path = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &known_path) != S_OK) {
        return NULL;
    }
    char *result = wide_to_utf8(known_path);
    CoTaskMemFree(known_path);
    return result;
}

char *plat_parent_process_name(void) {
    DWORD my_pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return NULL;
    }

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    DWORD parent_pid = 0;
    bool found_self = false;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ProcessID == my_pid) {
                parent_pid = entry.th32ParentProcessID;
                found_self = true;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }

    char *result = NULL;
    if (found_self) {
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snap, &entry)) {
            do {
                if (entry.th32ProcessID == parent_pid) {
                    result = wide_to_utf8(entry.szExeFile);
                    break;
                }
            } while (Process32NextW(snap, &entry));
        }
    }
    CloseHandle(snap);

    if (result) {
        for (char *p = result; *p != '\0'; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
    }
    return result;
}

/* Reads registry value `value_name` under already-open `key` as a string,
 * decoding it from UTF-16 -- shared by plat_win_autorun_get and the
 * HKCU\Environment\Path helpers below. Always returns a non-NULL,
 * NUL-terminated string ("" if the value is unset, unreadable, or not a
 * string type); the registry doesn't guarantee a stored REG_SZ/
 * REG_EXPAND_SZ is itself NUL-terminated, so that's enforced here. Caller
 * frees. */
static char *reg_get_string_utf8(HKEY key, const wchar_t *value_name) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, value_name, NULL, &type, NULL, &size) != ERROR_SUCCESS || size == 0 ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return xstrdup("");
    }
    wchar_t *buf = xmalloc(size + sizeof(wchar_t));
    if (RegQueryValueExW(key, value_name, NULL, &type, (BYTE *)buf, &size) != ERROR_SUCCESS) {
        free(buf);
        return xstrdup("");
    }
    buf[size / sizeof(wchar_t)] = L'\0';
    char *result = wide_to_utf8(buf);
    free(buf);
    return result ? result : xstrdup("");
}

#define AUTORUN_KEY_PATH L"Software\\Microsoft\\Command Processor"
#define AUTORUN_VALUE_NAME L"AutoRun"

char *plat_win_autorun_get(void) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTORUN_KEY_PATH, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return xstrdup("");
    }
    char *result = reg_get_string_utf8(key, AUTORUN_VALUE_NAME);
    RegCloseKey(key);
    return result;
}

bool plat_win_autorun_set(const char *value) {
    wchar_t *wvalue = utf8_to_wide(value);
    if (!wvalue) {
        return false;
    }
    HKEY key;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, AUTORUN_KEY_PATH, 0, NULL, 0, KEY_SET_VALUE, NULL,
                               &key, NULL);
    if (rc != ERROR_SUCCESS) {
        free(wvalue);
        return false;
    }
    rc = RegSetValueExW(key, AUTORUN_VALUE_NAME, 0, REG_SZ, (const BYTE *)wvalue,
                         (DWORD)((wcslen(wvalue) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    free(wvalue);
    return rc == ERROR_SUCCESS;
}

#define USERENV_KEY_PATH L"Environment"
#define USERENV_VALUE_NAME L"Path"

static void broadcast_environment_change(void) {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
                         SMTO_ABORTIFHUNG, 5000, &result);
}

/* Rebuilds a ';'-joined PATH string with `dir` removed wherever it appears
 * (case-insensitively -- NTFS path comparison is case-insensitive; empty
 * segments from a stray/doubled ';' are dropped too) and, if `add` is
 * true, reinserted once at the front. Caller frees the result. */
static char *rebuild_path_value(const char *path_value, const char *dir, bool add) {
    DynBuf out;
    dynbuf_init(&out);
    if (add) {
        dynbuf_append_str(&out, dir);
    }
    char *copy = xstrdup(path_value);
    char *saveptr = NULL;
    char *tok = strtok_r(copy, ";", &saveptr);
    while (tok) {
        if (tok[0] != '\0' && _stricmp(tok, dir) != 0) {
            if (out.len > 0) {
                dynbuf_append_char(&out, ';');
            }
            dynbuf_append_str(&out, tok);
        }
        tok = strtok_r(NULL, ";", &saveptr);
    }
    free(copy);
    char *result = xstrdup(dynbuf_cstr(&out));
    dynbuf_free(&out);
    return result;
}

static bool win_userenv_path_update(const char *dir, bool add) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, USERENV_KEY_PATH, 0, NULL, 0,
                         KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) {
        return false;
    }

    char *existing = reg_get_string_utf8(key, USERENV_VALUE_NAME);
    char *new_value = rebuild_path_value(existing, dir, add);
    free(existing);

    wchar_t *wnew = utf8_to_wide(new_value);
    free(new_value);
    if (!wnew) {
        RegCloseKey(key);
        return false;
    }

    /* HKCU\Environment\Path is conventionally REG_EXPAND_SZ (so
     * %SystemRoot%-style references other tools add still expand when
     * composed into a session's PATH) -- written as that type
     * unconditionally, regardless of whatever type it had before. */
    LONG rc = RegSetValueExW(key, USERENV_VALUE_NAME, 0, REG_EXPAND_SZ, (const BYTE *)wnew,
                              (DWORD)((wcslen(wnew) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    free(wnew);
    if (rc != ERROR_SUCCESS) {
        return false;
    }
    broadcast_environment_change();
    return true;
}

bool plat_win_userenv_path_add(const char *dir) {
    return win_userenv_path_update(dir, true);
}

bool plat_win_userenv_path_remove(const char *dir) {
    return win_userenv_path_update(dir, false);
}
