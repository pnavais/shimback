#ifndef SHIMBACK_PLATFORM_H
#define SHIMBACK_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

#include "../util.h" /* DynBuf */

/* The $PATH list separator: POSIX has always used ':', which Windows can't
 * (it collides with a drive letter's own ':', e.g. "C:\Windows") -- ';' is
 * the Windows convention instead. A compile-time constant, not a function,
 * since every call site uses it as a literal strtok_r delimiter argument. */
#ifdef _WIN32
#define PLAT_PATH_LIST_SEP ";"
#else
#define PLAT_PATH_LIST_SEP ":"
#endif

/* Process-spawn seam: every "run another program and wait for it" pattern
 * in the codebase funnels through one of the three functions below. Each
 * hides the platform's actual spawn/wait primitives (fork+execv+waitpid on
 * POSIX; CreateProcess+GetExitCodeProcess on Windows, once platform_win32.c
 * exists) and its exit-status decoding convention (POSIX folds a
 * fatal-signal death into 128+signum, matching shell convention; Windows
 * has no such folding -- there is no signal-based process death) behind a
 * single already-decoded exit code.
 *
 * None of these three ever call die() themselves on a failure to spawn or
 * wait -- that's a policy decision for each call site (dispatch.c always
 * wants to die immediately on it and wraps these in a small local helper
 * that does so; update.c/install.c already handle it gracefully today and
 * call these directly).
 *
 * All three take a fully-resolved `exe` (an absolute path -- callers are
 * expected to have already searched PATH/resolved a symlink themselves,
 * e.g. via path_search()/canonicalize()) and a NULL-terminated `argv`
 * (argv[0] conventionally the name the caller was itself invoked as, not
 * necessarily equal to `exe` -- see dispatch.c's build_argv). */

/* Runs `exe` with stdio inherited from this process (nothing captured or
 * redirected) and blocks until it exits. Returns the exit code (0-255
 * normally; see above for the signal-death convention), or -1 if the
 * process couldn't even be spawned or waited on for a reason unrelated to
 * `exe` itself -- as distinct from `exe` not existing or not being
 * runnable, which is reported as a normal-looking exit code around 127,
 * matching the shell convention for "command not found". */
int plat_run_inherited(const char *exe, char *const argv[]);

/* Runs `exe` with stdout/stderr captured into `out`/`err` instead of
 * inherited -- this is what makes a failed trial run invisible (see
 * dispatch.c's dispatch_run). Bounded by `timeout_ms`/`limit_bytes`:
 * whichever is hit first, capturing gives up and *committed_live is set to
 * true -- everything captured so far is flushed to the real stdout/stderr
 * and the rest relayed live from that point on (nothing is ever
 * discarded). Once *committed_live is true, `out`/`err` no longer hold the
 * process's full output (the tail was relayed, not buffered) -- callers
 * must not treat them as complete in that case. Returns the exit code
 * using the same convention as plat_run_inherited; -1 on a spawn/wait
 * failure unrelated to `exe` itself (same distinction as above). */
int plat_run_captured(const char *exe, char *const argv[], DynBuf *out, DynBuf *err,
                       int timeout_ms, size_t limit_bytes, bool *committed_live);

/* Runs `exe` with stdout captured into `buf` (NUL-terminated, at most
 * `bufcap`-1 bytes) and stderr discarded -- for reading a short value out
 * of a trusted helper binary's output (e.g. `<binary> --version`), not
 * general output capture (no timeout/live-relay handling -- see
 * plat_run_captured for that). Returns the number of bytes captured on a
 * clean, exit-0 run; -1 on a spawn/wait failure, a nonzero exit, or empty
 * output. */
long plat_capture_stdout(const char *exe, char *const argv[], char *buf, size_t bufcap);

/* Filesystem/environment seam. */

/* The current user's home directory, resolved however the platform does
 * that (POSIX: $HOME, falling back to the passwd entry; Windows:
 * %USERPROFILE%, falling back to SHGetKnownFolderPath). Caller frees.
 * Dies (via die()) if it truly can't be determined -- every caller already
 * treats "no home directory" as unrecoverable, not something to handle
 * per-call. */
char *plat_home_dir(void);

/* This running executable's own canonicalized path. Caller frees. Dies if
 * it can't be determined -- same reasoning as plat_home_dir. */
char *plat_self_exe_path(void);

/* Lists every entry name directly inside `dir` except "." and ".."
 * (NUL-terminated array of names, not full paths -- join with `dir`
 * yourself). NULL if `dir` doesn't exist or can't be opened, which every
 * current caller already treats as "nothing found", not a failure worth
 * distinguishing. Caller frees with plat_free_dir_entries. */
char **plat_list_dir(const char *dir);
void plat_free_dir_entries(char **entries);

/* Creates a new, uniquely-named, exclusively-owned regular file (mode
 * 0600) derived from `template_path`'s trailing "XXXXXX" (POSIX:
 * mkstemp() -- the same in/out template convention is honored by the
 * eventual Windows implementation too, even though the underlying
 * primitive there isn't mkstemp itself). Atomic: never opens a
 * pre-existing file at the generated name, symlink or otherwise.
 * `template_path` is overwritten in place with the actual generated path.
 * Returns an open, writable fd (caller closes it, typically via fdopen()
 * into a FILE*), or -1 on failure. */
int plat_mkstemp(char *template_path);

/* Creates a new, uniquely-named, exclusively-owned directory (mode 0700)
 * derived from `template_path`'s trailing "XXXXXX" (POSIX: mkdtemp() --
 * same in/out template convention as plat_mkstemp). `template_path` is
 * overwritten in place with the actual generated path. Returns
 * `template_path` on success, or NULL on failure. */
char *plat_mkdtemp(char *template_path);

/* Sets `path`'s/`fd`'s permissions to `mode` (POSIX mode bits). On Windows
 * these are best-effort no-ops for anything this codebase actually asks
 * for (0600/0644/0755) and always report success there -- Windows has no
 * equivalent permission-bit model for a single-user install, and
 * "executable" is determined by the .exe extension, not a bit. Return
 * false only when the POSIX chmod()/fchmod() call itself fails. */
bool plat_chmod(const char *path, int mode);
bool plat_fchmod(int fd, int mode);

/* Renames `from` to `to`, atomically replacing `to` if it already exists
 * (POSIX: rename() already has this semantics; Windows: plain rename()/
 * MoveFileW do NOT replace an existing destination -- the eventual Windows
 * implementation needs MoveFileExW with MOVEFILE_REPLACE_EXISTING to match
 * this contract, so callers must go through this rather than a bare
 * rename() if `to` might already exist, which every current caller's use
 * case -- atomically installing a new version of a file that may already
 * be there -- always risks). Returns false on failure. */
bool plat_rename_replace(const char *from, const char *to);

/* Creates a single directory (not recursive) if it doesn't already exist.
 * mkdir()-like semantics: 0 on success, -1 on failure with errno set
 * (EEXIST in particular, which paths.c's mkdir_p() specifically tolerates
 * before separately re-checking the result is actually a directory, not
 * some other kind of entry blocking the name). Windows has no
 * permission-bit equivalent for the implied mode 0755. */
int plat_mkdir(const char *path);

/* The current working directory as an absolute path, or NULL if it can't
 * be determined. Caller frees. */
char *plat_getcwd(void);

/* Opens (creating if necessary, mode 0600) and takes an exclusive,
 * blocking advisory lock on the file at `path` in one step (POSIX:
 * open() + flock(); Windows: CreateFileW + LockFileEx over the whole
 * file, the nearest equivalent -- a different API shape, which is why
 * this is one combined operation rather than separate open/lock
 * primitives). Returns a handle >= 0 to later pass to
 * plat_lockfile_close(), or -1 on failure -- in which case, if
 * `out_errno` is non-NULL, `*out_errno` is set to ENOENT specifically
 * when `path`'s parent directory doesn't exist (distinct from any other
 * failure reason; some callers need to tell "there was nothing to lock"
 * apart from a real failure). */
int plat_lockfile_open(const char *path, int *out_errno);

/* Releases a lock acquired by plat_lockfile_open(). Safe to call with -1
 * (a no-op) so callers don't need to track whether acquisition actually
 * succeeded before cleaning up. */
void plat_lockfile_close(int handle);

/* Terminal seam for the interactive add wizard (tui.c). The escape-
 * sequence key-parsing state machine built on top of these (tui_read_key)
 * is itself portable as-is: modern Windows consoles emit the same VT
 * sequences (ESC [ A/B/C/D for arrows, etc.) once virtual-terminal input
 * is enabled, so only the raw-mode and byte-level primitives below need a
 * platform-specific implementation. */

bool plat_isatty_stdin(void);
bool plat_isatty_stdout(void);
bool plat_isatty_stderr(void);

/* Enters raw input mode on stdin (no line buffering, no echo, no signal
 * generation from control characters) if not already active, saving
 * whatever mode was active before on first entry so a later
 * plat_tty_raw_exit() -- or an abnormal process exit, guarded against via
 * an atexit() hook registered the first time this succeeds -- restores
 * it. Returns false if the terminal mode couldn't be read or changed
 * (e.g. stdin isn't actually a terminal). */
bool plat_tty_raw_enter(void);

/* Restores the mode saved by the first plat_tty_raw_enter() call, if raw
 * mode is currently active; a safe no-op otherwise. */
void plat_tty_raw_exit(void);

/* True if another byte is available to read from stdin within `ms`
 * milliseconds -- used only to tell a bare Esc apart from the start of an
 * "ESC [ <letter>" sequence. */
bool plat_stdin_byte_ready(int ms);

/* Blocking read of exactly one byte from stdin into `*out`. Returns false
 * on EOF/error. */
bool plat_read_stdin_byte(char *out);

/* Turns on ANSI/VT escape-sequence interpretation for stdout/stderr, if
 * they're real consoles (Windows: SetConsoleMode +
 * ENABLE_VIRTUAL_TERMINAL_PROCESSING -- conhost.exe doesn't interpret the
 * color codes and cursor-movement sequences this codebase already emits
 * (util.c's ANSI_* constants, tui.c's clear-screen) unless this is turned
 * on explicitly; POSIX terminals already interpret them natively, so this
 * is a no-op there). Safe to call unconditionally, including when
 * stdout/stderr are redirected to a file/pipe rather than a real console
 * -- called once, early in main(), before any output. */
void plat_enable_vt_output(void);

/* True if `path` exists and is specifically a symlink (POSIX: lstat() +
 * S_ISLNK(); Windows: always false, for now -- a Windows shim is a hard
 * link, which has no distinct "is this a link" file-type bit to check
 * this way at all, so there is nothing to positively detect yet. This is
 * deliberately not a "not yet implemented" die() the way the process-
 * spawn/tty stubs are: every call site already treats "not a link" as a
 * normal, valid outcome (nothing found here / leave this entry alone),
 * not an error, so returning false lets list/doctor/add/remove/uninstall
 * run cleanly today reporting zero shims rather than crashing -- the
 * right degraded behavior until Phase 3 (windows-port.md) adds a real
 * hard-link-aware check (file-index comparison against self_exe_path()). */
bool plat_path_is_symlink(const char *path);

/* Resolves `path` to a canonical, absolute form -- the existing symlink
 * chain and `.`/`..` components followed and collapsed (POSIX:
 * realpath(path, NULL); Windows: CreateFileW + GetFinalPathNameByHandleW,
 * which together give the closest equivalent: an existence check plus
 * reparse-point/symlink resolution to the real final path, not just
 * string-level `.`/`..` normalization). `path` must exist. Caller frees
 * the result; NULL on failure (including "doesn't exist"). */
char *plat_realpath(const char *path);

/* Creates a shim link at `link_path` sharing the same underlying file as
 * `target` (POSIX: symlink() -- link_path becomes a *path reference* to
 * target; Windows: CreateHardLinkW -- link_path becomes a second name for
 * the *same file data*, no path reference involved at all -- see
 * windows-port.md's "why hard links, not symlinks" discussion up top).
 * `target` should already be an absolute, canonical path. Returns false
 * on failure (including, on Windows, if `link_path` and `target` aren't
 * on the same volume -- hard links can't cross volumes; callers surface
 * this as a clear error rather than a generic one where practical). */
bool plat_link_create(const char *target, const char *link_path);

/* --- Shell/PATH integration (Phase 5) -------------------------------- */

/* The current user's "Documents" special folder (Windows:
 * SHGetKnownFolderPath(FOLDERID_Documents) -- the same folder PowerShell
 * itself resolves $PROFILE.CurrentUserAllHosts under, honoring
 * OneDrive-redirected/relocated Documents folders). Caller frees. POSIX
 * has no such concept and never calls this; returns NULL there. */
char *plat_documents_dir(void);

/* The basename (lowercased) of this process's immediate parent process
 * (Windows: a CreateToolhelp32Snapshot walk -- the standard technique for
 * "what shell am I running under" on Windows, since there's no $SHELL-
 * style env var equivalent; subject to the same kind of imprecision
 * $SHELL already has, plus a possible PID-reuse race in the snapshot,
 * both acceptable for this best-effort use). Caller frees. NULL if it
 * can't be determined. POSIX detects its shell via $SHELL instead and
 * never calls this; returns NULL there. */
char *plat_parent_process_name(void);

/* HKCU\Software\Microsoft\Command Processor\AutoRun -- cmd.exe's
 * per-session startup hook: a registry *string*, not a file, that cmd.exe
 * runs verbatim as its first commands on every new instance (interactive
 * or /C, /K) unless started with /D. get() returns "" if unset (caller
 * frees); set() creates the key if needed and returns false on failure.
 * No POSIX equivalent -- POSIX's shell startup files are handled entirely
 * in shell.c itself, with no platform primitive needed; these are never
 * called there. */
char *plat_win_autorun_get(void);
bool plat_win_autorun_set(const char *value);

/* HKCU\Environment\Path -- Windows' per-user persistent PATH, composed
 * into every new session's PATH (via userenv.dll's
 * CreateEnvironmentBlock, used at logon and whenever Explorer spawns a
 * child) after the Machine-level PATH. _add prepends `dir` if not already
 * present (case-insensitively deduped, matching NTFS path-comparison
 * semantics, and reinserted at the front if already present elsewhere in
 * the value); _remove drops every occurrence of `dir`. Both broadcast
 * WM_SETTINGCHANGE afterward so already-running processes that listen
 * for it (Explorer in particular) pick it up for whatever they spawn
 * next. This is a *fallback* layer, not sufficient by itself for
 * first-in-PATH ordering against the Machine PATH -- see shell.c's
 * ensure_powershell/ensure_cmd, which also write a session-prepend block/
 * AutoRun line that actually delivers that guarantee. No POSIX
 * equivalent (POSIX's PATH persistence is entirely the shell rc files
 * shell.c already writes); never called there. */
bool plat_win_userenv_path_add(const char *dir);
bool plat_win_userenv_path_remove(const char *dir);

#endif /* SHIMBACK_PLATFORM_H */
