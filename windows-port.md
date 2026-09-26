# Windows port implementation plan

## Status (updated 2026-09-25, later same day) — resume here

| Phase | What | Status |
|---|---|---|
| 0 | Platform seam refactor (POSIX side) | ✅ done |
| 1 | Toolchain/CMake wiring (clang-cl + lld-link) | ✅ done |
| 2 | Process model (`CreateProcessW`) | ✅ done — verified end-to-end with real captured/inherited/timeout scenarios |
| 3 | Shim links (hard links) | ✅ done — real hard-link shims verified end-to-end (add/list/doctor/doctor fix/remove/uninstall); `update`-staleness fix **now implemented**, see Phase 7 |
| 4 | `paths.c` (home/config/data, scanning) | ✅ done — defaults to `.config`/`.local/share` under `%USERPROFILE%` (same as macOS/Linux), falling back to `%APPDATA%`/`%LOCALAPPDATA%` only when that's the only one that already exists (migration); cosmetic path-separator cleanup still outstanding (low priority) |
| 5 | Shell/PATH integration | ✅ done — PowerShell profile injection + cmd.exe AutoRun + `HKCU\Environment\Path` fallback, all verified end-to-end for real (including registry state, not just files) |
| 6 | TUI raw mode | ✅ done — live keystroke navigation manually verified by the user in a real terminal (raw mode, arrow keys, typed input all worked); that same test caught a real bug, an unclear `EXDEV` error message, since fixed and then upgraded further: `add`/`doctor fix` now transparently fall back to copying shimback's binary instead of dying when a hard link can't cross drives (see Phase 6's two addenda) |
| 7 | Packaging (`install.ps1`, CI, release artifact) | ✅ done — `build-windows` is green on a real GitHub Actions run (`workflow_dispatch`, not a tag, so the actual `v0.1.0` release was untouched), producing a genuine, independently-built, working `shimback.exe`. Took 5 CI iterations to get there; see Phase 7's CI addendum for every real bug that surfaced only by actually running it |
| 8 | Pester test suite | ⬜ not started as a formal suite (this session's verification was manual) |

**Done (was the outstanding action item)**: `v0.1.0` has been re-cut with
Windows support included, per the user's explicit decision -- the old
`v0.1.0` release + tag were deleted (nobody had downloaded it; the repo
wasn't published/promoted yet, so this was judged safe), and a fresh
`v0.1.0` tag pushed at the post-Windows-port `main`, producing a real
GitHub Release with all platform assets, including
`shimback-windows-x86_64.zip`. A `0.1.0-unix` tag still marks the last
pre-Windows commit for the record.

Build command and everything learned getting Phase 1 working: see Phase 1
below. Phase 2's design decisions (Windows' fork/exec-failure collapsing,
the command-line quoting algorithm, `PeekNamedPipe`-based dual-stream
polling instead of overlapped I/O) and the real `getopt_long` permutation
bug found while verifying it: see Phase 2. Phase 3's new shared primitives
(`plat_link_create`, `is_shim_dir_entry`, `shim_file_name`/
`shim_name_from_file`), the `errno`-mapping bug and the real POSIX
regression it caught along the way (missing-vs-dangling conflation in
`doctor fix`), and the several `.exe`-suffix omissions found via a full
`path_join(shim_dir, ...)` sweep: see Phase 3. Phase 4's config/data
default-location decision (`%APPDATA%`/`%LOCALAPPDATA%` with fallback to
the old `.config`/`.local/share` layout) and the `XDG_*_HOME`
Windows-absolute-path validation bug found while implementing it: see
Phase 4 (also since revised -- see below). Phase 5's new subsystem
(PowerShell/cmd.exe shell integration), the cmd.exe AutoRun
single-line/no-`rem` discovery (a real behavior found only by testing,
not documented anywhere obvious), and the
`dynbuf`-NULL-vs-`plat_win_autorun_set` bug it led to finding: see Phase
5. Phase 6's `ReadConsoleInputW`-based design (and why it was chosen over
`ENABLE_VIRTUAL_TERMINAL_INPUT`), the user's own real end-to-end smoke
test of the interactive wizard, the unclear `EXDEV` error message it
caught (fixed with a real explanation), and the follow-on decision to
recover from `EXDEV` outright -- `add`/`doctor fix` now fall back to
copying shimback's own binary under the shim's name (mirroring how
`mise`'s own shims turned out to work, confirmed by inspection) instead
of just failing with a clearer message -- with elevation-based
alternatives (`sudo`, a "transparent" UAC prompt) considered and
explicitly rejected: see Phase 6's two addenda.
**Since Phase 5 was written**: Phase 4's config/data default was flipped
back the other way -- `.config`/`.local/share` under `%USERPROFILE%` is
now the *default* (matching macOS/Linux), with `%APPDATA%`/
`%LOCALAPPDATA%` only as a migration fallback for an install already
using the old default -- see Phase 4's updated note for why. Phase 7 is
now started: `platform_asset()`/`.zip` extraction/checksum verification
all wired up and `update` verified end-to-end against a real local fake
release, the stale-shim auto-refresh feature (`-y`/`--yes`, mirroring
`doctor fix -y`) that closes Phase 6's copy-fallback disclaimer, a real
cascading `.exe`-suffix bug found across `install.c`/`installation.c`/
`uninstall.c`/`update.c` (fixed once via a new shared
`shimback_exe_name()`), and two separate real crashes found and fixed
along the way (a pre-existing, never-before-exercised MSVC `_IOLBF`
crash in `update.c`; a genuine free()-convention bug of this session's
own making) -- see Phase 7. Since then, also same session: a
"restart your shell" reminder folded into `create_shim_link()`'s own
copy-fallback warning (the user caught that `doctor fix`/`update` didn't
have one, unlike `add`); `install.ps1` written and verified the same way
`update` was (a real local fake release, this time served over an actual
local HTTP server since `Invoke-WebRequest` doesn't support `file://`);
the `.github/workflows/release.yml` `build-windows` job written and then
**actually proven green on real GitHub Actions runs** (5 iterations, 3
real bugs found only by running it for real -- xwin's own cross-drive
move, the same pwsh-argument-mangling bug local setup hit earlier, and
xwin's splat layout having changed versions -- see Phase 7's CI
addendum); and the README updated throughout. Also, per the user's
explicit call: the whole port was committed and pushed to `main`, with a
`0.1.0-unix` tag marking the last pre-Windows commit, since the plan is
to re-cut `v0.1.0` itself (not ship Windows support as a point release)
once everything's confirmed -- the old `v0.1.0` release/tag deletion and
the real tag-triggered re-release are the one step still not done. Next
session: **Phase 8** (the Pester suite) is the only phase left
unstarted; the re-release itself is a quick, now low-risk finish-up
whenever wanted.

---

Status quo: `README.md` (Platform support) says Windows isn't supported;
the implementation relies on POSIX symlinks, `fork`/`exec`, and Unix-style
shell startup files throughout. This plan replaces that blocker list with a
concrete path to a native Windows build via **clang-cl + lld-link**,
targeting the MSVC ABI without requiring a full Visual Studio install, and
built around one decision: **shim links are hard links, not symlinks**,
since every `symlink()` call in the codebase links to a single file (the
shimback binary itself) and never a directory — see
`src/commands/add.c:566`, `add.c:594`, `src/commands/doctor.c:127`. Hard
links to files need no elevation and no Developer Mode toggle on Windows,
unlike symlinks.

**Toolchain proven working as of 2026-09-22**: a minimal hello-world was
compiled and linked for both `x86_64-pc-windows-msvc` and
`aarch64-pc-windows-msvc` using clang-cl 23.1.1 (installed via
`winget install LLVM.LLVM`) and lld-link against MSVC/Windows SDK sysroots
fetched with `xwin` (split per architecture at
`D:\Projects\msvc-sysroot\{x86_64,aarch64}`, MSVC 14.44.17.14 / Windows Kit
10.0.26100). The x86_64 binary ran and printed correctly; the aarch64
binary can't run on this x86_64 machine, but `llvm-readobj --file-headers`
confirmed a genuine `IMAGE_FILE_MACHINE_ARM64 (0xAA64)` PE header, so the
cross-compile is real, not a silent x64 fallback. Full working invocation
and the two gotchas that cost time getting there are in Phase 1 below.

## Guiding principle

Don't scatter `#ifdef _WIN32` through business logic. The command layer
(`src/commands/*.c`) and `src/dispatch.c`'s policy logic should stay
platform-agnostic; all POSIX calls get pulled behind a small platform
abstraction so the Windows backend is additive, not a rewrite. Concretely:
introduce `src/platform/` with a common header (`platform.h`) declaring the
functions below, plus `platform_posix.c` (wraps the existing calls, used on
macOS/Linux, near-zero behavior change) and `platform_win32.c` (new).

## Inventory of what needs a platform seam

Grepped across `src/*.c` and `src/commands/*.c`:

| Concern | POSIX calls in use | Files | Windows replacement |
|---|---|---|---|
| Shim link creation/removal — **next up (Phase 3)** | `symlink()`, `lstat()`+`S_ISLNK`, `unlink()` | `add.c`, `doctor.c`, `remove.c`, `uninstall.c`, `info.c`, `paths.c` | `CreateHardLinkW`, `GetFileInformationByHandle` (link count / same-target check), `DeleteFileW` |
| Process spawn + exit status — **✅ behind the seam (`plat_run_inherited`/`plat_run_captured`/`plat_capture_stdout`), POSIX side done and tested** | `fork()`, `execv()`/`execvp()`, `waitpid()`, `WIFEXITED`/`WEXITSTATUS`/`WIFSIGNALED`/`WTERMSIG` | `dispatch.c`, `commands/update.c`, `commands/install.c` | `CreateProcessW` + `GetExitCodeProcess`; no signal-based exit, so the 128+signal convention collapses to plain exit codes |
| Trial-run output capture (invisible fallback) — **✅ behind the seam**, same functions as above | `pipe()`, `dup2()`, `poll()`, `read()`, `close()` | `dispatch.c` | anonymous pipes via `CreatePipe`, `SetHandleInformation` for inheritance, `ReadFile`/`PeekNamedPipe` in place of `poll()` |
| $EDITOR word-splitting + candidate fallback chain — **deferred, see Phase 0 notes** | `wordexp()` (`WRDE_NOCMD`), a `execvp`/`execlp`-in-a-loop fallback idiom | `commands/edit.c` | no `wordexp` equivalent at all; needs a PATH-search-first redesign, not a mechanical port — real design work, not in scope for Phase 0's "near-zero behavior change" pass |
| Home / config / data dirs | `getenv("HOME")`, `getpwuid(getuid())`, XDG (`XDG_CONFIG_HOME`, `XDG_DATA_HOME`) | `paths.c:51-92` | `%USERPROFILE%`, fall back to `SHGetKnownFolderPath(FOLDERID_Profile)`; config → `%APPDATA%\shimback`, data → `%LOCALAPPDATA%\shimback` (or keep XDG vars if explicitly set, for Git-Bash/MSYS2 users) |
| Directory scanning | `opendir`/`readdir`/`closedir`, `scandir` | `paths.c` (split-config scan), `doctor.c` | `FindFirstFileW`/`FindNextFileW`/`FindClose` |
| File metadata / perms | `stat`, `lstat`, `chmod`, `fchmod`, `umask`, `access()` + `F_OK`/`X_OK`/`W_OK`/`R_OK` | `paths.c`, `install.c`, `doctor.c`, `add.c` | `GetFileAttributesExW`, `_waccess`; POSIX mode bits don't map cleanly — treat "executable" as "has `.exe`/`.cmd`/is in `PATHEXT`" rather than a permission bit |
| Path building / resolution | `realpath()`, `readlink()`, `PATH_MAX` | `paths.c` | `GetFullPathNameW`, `_MAX_PATH` (and prefer wide-char + long-path (`\\?\`) handling over `MAX_PATH`-limited APIs) |
| Terminal / raw mode (add wizard) | `termios`, `tcgetattr`/`tcsetattr`, `isatty` | `tui.c` | `GetConsoleMode`/`SetConsoleMode` (clear `ENABLE_LINE_INPUT`/`ENABLE_ECHO_INPUT`), `_isatty`; also enable `ENABLE_VIRTUAL_TERMINAL_PROCESSING` for ANSI colors if the TUI emits escape codes |
| Env var access | `getenv`, implicit `environ` passthrough to child | `cli.c`, `config.c`, `dispatch.c`, `paths.c` | `getenv`/`_wgetenv` work as-is via MSVCRT; child env passed explicitly to `CreateProcessW` rather than inherited implicitly |
| Misc | `getopt` (CLI arg parsing), `getpid` | `cli.c` | no `getopt` in the Microsoft UCRT headers (a gap clang-cl inherits since it compiles against the same SDK) — vendor a small `getopt` shim (e.g. the public-domain `ya_getopt`) rather than reimplementing CLI parsing |

`src/sha256.c` and `src/util.c`'s string helpers are portable C and need no
changes. `src/config.c` (1530 lines, the largest file) is mostly
INI/config-format parsing over already-abstracted path/IO helpers — expect
it to need path-separator awareness (`/` vs `\`) but not a platform seam of
its own.

## Phased plan

### Phase 0 — groundwork (no behavior change on macOS/Linux)

**Progress as of 2026-09-22: process-spawn subsystem done and verified.**
`src/platform/platform.h` + `platform_posix.c` now exist, exposing
`plat_run_inherited`, `plat_run_captured`, and `plat_capture_stdout`.
Refactored onto them: `dispatch.c` (both `run_inherited`/`run_captured`
plus the duplicated `decode_exit_code`/`child_succeeded`, which no longer
exist as separate functions — every caller now gets an already-decoded
exit code straight from the platform layer), `commands/update.c`'s
`run_child`/`binary_version`, and `commands/install.c`'s inline
download-via-curl fork block. `CMakeLists.txt` updated to build
`src/platform/platform_posix.c`. Verified via a from-scratch WSL clone
(`gcc`/`cmake` via `mise`, to dodge the CRLF-from-Windows-checkout issue
that breaks the test scripts' shebangs under `/mnt/d`) with
`-Wall -Wextra -Werror`: clean build, 15/15 existing tests pass, identical
timing to the pre-refactor baseline (including `test_dispatch`,
`test_update`, `test_install`, which most directly exercise this code).

One deliberate, disclosed behavior seam: `plat_run_inherited`/
`plat_run_captured` never call `die()` themselves on a spawn/wait failure
(they return `-1`) — that's now a policy decision at each call site rather
than baked into the primitive. `dispatch.c`'s six call sites, which always
died on this before, get a tiny local `run_inherited_or_die` wrapper that
preserves that; `update.c`/`install.c` already handled it gracefully
before this refactor and needed no wrapper change at all.

**Progress, continued: `paths.c`'s mechanical portion done and verified.**
Added `plat_home_dir`, `plat_self_exe_path` (folds the file's pre-existing
`#if defined(__APPLE__)`/`__linux__` branch into the same seam, rather than
leaving it as a second, inconsistent platform-branching mechanism
alongside the new one), and `plat_list_dir`/`plat_free_dir_entries` (a
generic directory-enumeration primitive). `home_dir()` and `self_exe_path()`
are now one-line delegations; `list_shim_symlink_names()` and
`scan_split_config_dir()` use `plat_list_dir()` for enumeration. Verified
the same way as the process-spawn subsystem — clean `-Wall -Wextra -Werror`
build, 15/15 tests pass, identical timing. (`test_config_parser`, a second
CMake target that links `paths.c` directly rather than through the main
`shimback` target, needed `platform_posix.c` added to its own source list
too — an easy miss worth remembering for later platform-seam additions.)

**Deliberately left inline, not abstracted**: the `lstat`+`S_ISLNK` check
in `list_shim_symlink_names()` that recognizes a shim symlink among a
directory's entries. This is genuinely different in shape on Windows (a
hard link has no distinct "is this a link" file-type bit to check the same
way — the Phase 3-designed replacement is a file-index comparison via
`GetFileInformationByHandle` against `self_exe_path()`), so abstracting it
today would mean guessing at Phase 3's design before that phase actually
decides it. Left as direct POSIX code with a comment pointing at Phase 3
rather than a premature/wrong seam.

**Progress, continued: the rest of `paths.c` done and verified.** Added
`plat_mkstemp`, `plat_chmod`/`plat_fchmod`, `plat_rename_replace`,
`plat_mkdir`, `plat_getcwd`, `plat_lockfile_open`/`plat_lockfile_close`.
Refactored onto them: `copy_file_mode` (backs `copy_executable`/
`copy_file`), `mkdir_p`, `shim_dir_lock_acquire`/`release`,
`write_file_atomic`, `force_resolve_binary_arg`. One correctness point
worth remembering when Windows implementations get written: plain
`rename()`/`MoveFileW` on Windows do **not** replace an existing
destination the way POSIX `rename()` does — every call site here uses
rename for "atomically install a new version of a file that may already be
there", so `plat_rename_replace` exists specifically so the eventual
Windows backend can use `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`
instead of a bare rename that would silently start failing whenever the
destination already exists. `chmod`/`fchmod` are expected to become
best-effort no-ops on Windows (no permission-bit model for a single-user
install); `flock` becomes `LockFileEx` (different API shape, hence
`plat_lockfile_open`/`close` bundle the open+lock as one step rather than
mirroring POSIX's two separate calls). Verified the same way: clean build,
15/15 tests, identical timing. `paths.c` no longer includes `<fcntl.h>` or
`<sys/file.h>` at all — everything that needed them moved behind the seam.

**Progress, continued: `tui.c` done and verified.** Added
`plat_isatty_stdin`/`plat_isatty_stdout`, `plat_tty_raw_enter`/
`plat_tty_raw_exit` (moved the entire raw-mode save/restore state machine,
including the `atexit` safety net, behind the seam — not just the raw
`tcgetattr`/`tcsetattr` calls), `plat_stdin_byte_ready`,
`plat_read_stdin_byte`. `tui_read_key()`'s escape-sequence state machine
(`ESC [ A/B/C/D` → arrow keys, etc.) stays in `tui.c` unchanged and needs
no Windows-specific redesign at all — modern Windows consoles emit the
same VT sequences once virtual-terminal input is enabled, so the parser is
already portable; only the byte-level primitives underneath it needed
seaming. Verified via the full suite including `test_add_wizard` — the one
test that actually drives this code interactively through a real
pseudo-terminal — passing at the same ~76s.

**Remaining for Phase 0** (not yet started):
- The symlink call sites themselves: `add.c:566`/`add.c:594` (`symlink()`
  creation), `doctor.c:127` (repair), plus the `lstat`/`S_ISLNK` detection
  in `doctor.c`/`info.c`/`remove.c`/`uninstall.c` — this is where Phase 3's
  hard-link design actually needs to get decided in code, not just planned.

**Discovered wrinkle, scoped out of Phase 0 deliberately**: `edit.c` uses
`wordexp()` (no Windows equivalent at all — parses `$EDITOR` with
`WRDE_NOCMD` to refuse command substitution while still respecting
quoting) and a "try `$EDITOR` via `execvp`, on failure try each of
nvim/vim/vi/nano/pico via `execlp` in turn" loop that relies on POSIX's
exec-fails-in-child idiom to move to the next candidate. Porting this
properly means redesigning it around a PATH-search-first approach (resolve
which editor to use via an explicit search, *then* spawn once) since
Windows has no equivalent "just keep trying execve in a loop" idiom — that
is real design work, not a mechanical rename, so it doesn't fit this
phase's "near-zero behavior change" bar. Left untouched for now; revisit
alongside Phase 6 (TUI) or as its own line item, not bundled into the rest
of Phase 0's mechanical pass.

When that phase happens: Windows' fallback-editor chain should lead with
[Microsoft `edit`](https://github.com/microsoft/edit) (MIT-licensed,
single-binary, terminal-based — the natural Windows analogue to
nvim/vim/vi/nano/pico here) rather than assuming one of the POSIX fallback
editors happens to be on `PATH`. Worth offering to install it via
`winget install Microsoft.Edit` when it's missing, the same spirit as
Phase 7's `install.ps1` — a good, low-friction "it just works" moment for
a Windows user who has no editor configured at all.

### Phase 1 — CMake + toolchain

**✅ DONE as of 2026-09-22.** `cmake/windows-clang-cl.cmake` + `CMakeLists.txt`
+ `src/platform/platform_win32.c` + `src/platform/win32-compat/` together
produce a real, running `shimback.exe`, built and verified on this machine
end-to-end — not just compiled: `--version`, `--help`, `list`, `doctor`,
and `info` all execute correctly and produce sensible output; commands that
depend on later phases (`add` past validation, `edit`, `doctor fix`) fail
with a clear "not yet implemented" message instead of crashing or behaving
wrong. `test_config_parser.exe` also builds and passes natively. The full
POSIX suite (15/15 tests) was re-verified after every change in this phase
and remains green — nothing here regressed macOS/Linux.

**How to build it**, for reference:
```
cmake -S . -B build-windows -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/windows-clang-cl.cmake \
  -DSHIMBACK_LLVM_BIN="C:\Program Files\LLVM\bin" \
  -DSHIMBACK_MSVC_SYSROOT="D:\Projects\msvc-sysroot\x86_64" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows
```
Run from PowerShell or `cmd`, never Git Bash (see the MSYS gotcha below,
which applies to `cmake`/`ninja` invocations too, not just raw `clang-cl`).
`SHIMBACK_WIN_TARGET` (default `x86_64-pc-windows-msvc`) selects the arch.

**What's behind `src/platform/win32-compat/` and why**: three files, added
to the Windows-only include path (never visible to macOS/Linux builds):
- `getopt.h` — a full, real `getopt_long` implementation (`getopt_win32.c`),
  not a partial one: GNU-style permutation, `--name=value`, short-option
  clustering, `--` termination. `add.c` genuinely depends on permutation
  (`argv[optind]` for the positional `<name>`, which can appear before or
  after options), so a stripped-down version would have been a silent
  correctness gap, not just a simplification.
- `unistd.h` — maps the handful of POSIX names this codebase actually uses
  (`access`/`unlink`/`rmdir`/`getcwd`/`getpid`/`close`/`read`/`write`) to
  their UCRT underscore-prefixed equivalents, plus `F_OK`/`W_OK`/`R_OK`/
  `X_OK` (`X_OK` degrades to existence-only — Windows has no execute-bit to
  check; see `is_executable_file`'s known gap below).
- `shimback_prelude.h` — **force-included** (`/FI`, not a normal header) into
  *every* translation unit, for things that can't be solved by adding a
  same-named header earlier on the include path because a real one already
  exists (`<sys/stat.h>` genuinely exists in the UCRT sysroot, so a
  same-named override would risk shadowing real content, not just filling a
  gap): `S_ISREG`/`S_ISDIR` (UCRT defines `S_IFREG`/`S_IFDIR` but not the
  POSIX convenience macros built on them), `mode_t`, `ssize_t` (aliased to
  `SSIZE_T`), and `strtok_r` (aliased to Microsoft's `strtok_s`, which
  takes the identical three arguments).

**New platform.h functions added this phase** (beyond what Phase 0 already
had), all implemented for both POSIX and Windows:
- `plat_realpath` — `canonicalize()`'s new backing. **Not just a Phase-3
  concern**: this is used throughout ordinary dispatch/PATH-search logic
  (`path_search`, `resolve_binary_arg`), not only symlink-specific code, so
  it had to actually work, not stub out. Windows implementation:
  `CreateFileW` + `GetFinalPathNameByHandleW` (existence check + reparse-
  point resolution, stripping the `\\?\` prefix the latter adds).
- `plat_mkdtemp` — `install.c`/`update.c`'s scratch-directory creation
  (Windows: `_mktemp_s` + `_wmkdir`, retried on collision, mirroring
  `plat_mkstemp`'s approach).
- `plat_path_is_symlink` — added when the compiler, not just planning,
  revealed that `lstat`/`S_ISLNK` call sites are more widespread than
  Phase 0's inventory caught (`paths.c`, `add.c`, `doctor.c`, `remove.c`,
  `uninstall.c`, `info.c`) and that `paths.c` — foundational, can't be
  excluded from the Windows build the way `edit.c` can — needed *something*
  that compiles. Always returns `false` on Windows (no hard-link detection
  yet), which is honest, not a placeholder: every call site already treats
  "not a link" as a normal outcome (nothing found / leave alone), so
  `list`/`doctor`/`uninstall` run cleanly today reporting zero shims rather
  than crashing.

**Real, non-cosmetic bugs found only by actually *running* the binary, not
just compiling it** — the reason this phase didn't stop at "it builds":
- `main.c`'s `basename_of()` only split on `/`, and the "is this invoked as
  `shimback` itself vs. as a shim" check compared against the bare string
  `"shimback"` with no `.exe` handling. On Windows, `argv[0]` is a full
  backslash path, so *every* invocation — including `--version`/`--help` —
  fell through to shim-dispatch and failed with "no shim configured for
  'D:\...\shimback.exe'". Fixed: `basename_of` now checks both separators,
  and a new `strip_exe_suffix` (Windows-only) strips a trailing `.exe`
  before the comparison. This is core, universal dispatch logic, not a
  Phase-3-adjacent detail — it would have silently broken *every* command.
- `path_search`/`dir_on_path`/`first_on_path` split `$PATH` on `:` via
  `strtok_r`. Windows uses `;` (`:` collides with a drive letter's own
  colon, e.g. `C:\Windows`) — silently wrong, not a compile error, so it
  only surfaced when `shimback add ... -f cmd` failed to find `cmd` even
  though it's on `PATH`. Fixed with a new `PLAT_PATH_LIST_SEP` macro in
  `platform.h` used at all four call sites (`paths.c`, `info.c`, `doctor.c`).

**Known, deliberately-left-open gaps** (none block Phase 2/3, all noted so
they're not "discovered" again later):
- `is_executable_file` doesn't yet do PATHEXT-aware resolution — a bare
  `cmd` won't resolve to `cmd.exe` the way `CreateProcess`/the real shell
  would. Matches the already-documented "executable is extension-based on
  Windows, not permission-bit-based" gap in Phase 0's inventory table, now
  concretely observed (`shimback add foo -f cmd` fails to resolve `cmd`).
- Displayed paths mix separators (e.g. `C:\Users\Pablo/.config/shimback/
  config.toml`) since `path_join()` hardcodes `/`. Windows APIs accept
  mixed separators fine (confirmed — nothing actually breaks), so this is
  cosmetic, matching Phase 4's already-noted path-separator polish item.
- `-Wall -Wextra -Werror` (`SHIMBACK_DEV_WARNINGS`, Debug builds only) is
  dramatically stricter under clang-cl than gcc/clang-native mode — flags
  fully idiomatic, valid C used throughout this codebase (declaration-
  after-statement, implicit `void*` conversion, struct padding) as errors.
  All verification this phase used Release builds (`SHIMBACK_DEV_WARNINGS`
  off) to see real signal through this noise. Curating a Windows-
  appropriate warning set (or disabling specific categories there) is a
  follow-up, not done yet — Debug builds on Windows won't compile as-is.
- `strerror`/`getenv`/`fopen`/etc. trigger `-Wdeprecated-declarations`
  (Microsoft's "use the `_s` suffix" nagging) — warnings only, not errors,
  even in Debug, so non-blocking, but worth silencing eventually
  (`_CRT_SECURE_NO_WARNINGS` or migrating call sites) rather than living
  with the noise long-term.

**Excluded from the Windows build entirely, each with a clear "not yet
implemented" message rather than a silent gap**:
- `edit.c` (`wordexp()` — see Phase 0's original note).
- `finish_add` in `add.c` bails out with `die()` before touching the
  filesystem, once validation/config-building (portable, and it runs)
  completes — creating a shim is Phase 3 work end to end.
- `doctor.c`'s `fix_symlink_if_needed`/`check_symlink` and `info.c`'s
  `print_symlink` are symlink-diagnostic-specific by nature (the
  "dangling" concept doesn't even apply to hard links — see Phase 3's
  write-up above), so each prints an honest placeholder on Windows rather
  than attempting a wrong or partial diagnosis.

**Toolchain/CMake lessons worth keeping** (mistakes made and fixed while
getting here, so Phase 2+ doesn't repeat them):
- A toolchain file (`-DCMAKE_TOOLCHAIN_FILE=...`) is *required*, not
  optional, for sysroot wiring — CMake's own internal "does this compiler
  work" check runs *during* `project()`, before anything in the main
  `CMakeLists.txt` body executes, so sysroot flags set there (even via
  `add_link_options` at the top of the file) are too late for that one
  check, even though they'd be fine for every target built afterward.
- `CMAKE_C_FLAGS_INIT`/`CMAKE_EXE_LINKER_FLAGS_INIT` (the toolchain-file
  mechanism for seeding those checks) only take effect on a *fresh*
  configure — changing them and re-running `cmake --build` (which
  auto-reconfigures an existing build dir) silently keeps the old cached
  flags. Always wipe the build directory after editing the toolchain file.
- Custom cache variables (`SHIMBACK_MSVC_SYSROOT` etc.) aren't
  automatically visible inside CMake's internal `try_compile` sub-project
  used for that same compiler check — needs
  `list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ...)` in the toolchain
  file to forward them explicitly.
- Use `-imsvc`, not `/I`, for the sysroot include paths. `/I` puts
  Microsoft's own headers on equal footing with this project's code for
  warning purposes, and they trip a long list of clang's pedantic warnings
  that have nothing to do with this project (`-Wreserved-macro-identifier`,
  `-Wlanguage-extension-token` for `__int64`, `-Wc++-keyword` for `wchar_t`,
  `-Wshadow-header` for VC's and the SDK's both shipping a `sal.h`, and
  more) — `-imsvc` marks them as system headers, which clang exempts from
  these by default, fixing all of them at once instead of chasing each
  warning category individually.
- This `xwin`-splatted sysroot only has the *release* CRT import libs
  (`libcmt.lib`, `msvcrt.lib`), not the debug ones (`libcmtd.lib` etc. —
  `xwin` doesn't fetch those by default). `CMAKE_MSVC_RUNTIME_LIBRARY` must
  be pinned to `"MultiThreaded"` unconditionally (not
  `"MultiThreaded$<$<CONFIG:Debug>:Debug>"`, CMake's usual per-config
  default) — set in the toolchain file, again because the internal
  compiler check needs it too — or a `CMAKE_BUILD_TYPE=Debug` configure
  fails trying to link against a debug CRT lib that doesn't exist here.

- **Settled: clang-cl + lld-link**, targeting the MSVC ABI, is the primary
  (and, for v1, only) Windows toolchain — reversing the earlier MinGW-w64
  choice. Rationale (see the discussion that led here): avoids the MinGW
  SmartScreen/AV false-positive reputation issue, gets native PDB/WinDbg
  debugging and MSVC-ABI compatibility for free, and matches what GitHub
  Actions' `windows-latest` runner already has installed (Visual Studio
  Build Tools with the Clang component), so CI needs no separate toolchain-
  install step. First-class CMake support either via
  `-DCMAKE_C_COMPILER=clang-cl` with the Ninja generator, or `-T ClangCL`
  with the Visual Studio generator — prefer Ninja to keep the CMake
  invocation shape close to the existing Linux/macOS jobs.
- **No full Visual Studio install required.** clang-cl still needs the
  Microsoft Windows SDK + MSVC-compatible CRT/import libraries (that's the
  point of targeting the MSVC ABI), but these were obtained standalone via
  [`xwin`](https://github.com/Jake-Shadle/xwin) (`xwin splat`), which
  downloads and lays out just the SDK/CRT headers and import libs without
  installing Visual Studio itself. Confirmed working layout (one splat per
  architecture, matching a real VS tree):
  ```
  <sysroot>/<arch>/VC/Tools/MSVC/<ver>/include
  <sysroot>/<arch>/VC/Tools/MSVC/<ver>/lib/<arch>
  <sysroot>/<arch>/Windows Kits/10/Include/<winkit-ver>/{ucrt,um,shared,winrt,cppwinrt}
  <sysroot>/<arch>/Windows Kits/10/Lib/<winkit-ver>/{ucrt,um}/<arch>
  ```
  `<arch>` is `x86_64` or `aarch64`; `xwin` also supports `x86`/`arm` if
  ever needed. CMake should locate this via a `SHIMBACK_MSVC_SYSROOT`
  cache variable (or an env var of the same name) rather than a hardcoded
  path, since CI and each developer's machine will have it somewhere
  different — wire it into `CMAKE_C_FLAGS`/`CMAKE_EXE_LINKER_FLAGS` as
  `/I`/`-imsvc` and `/LIBPATH:` entries built from that root plus the
  detected MSVC/Windows-Kit version subdirectory (`file(GLOB ...)` over the
  `VC/Tools/MSVC/*` and `Windows Kits/10/Include/*` dirs, since those
  version numbers will drift as `xwin` is re-run over time).
- **Confirmed working invocation** (compile and link as two explicit
  steps — see the next bullet for why):
  ```
  clang-cl.exe --target=x86_64-pc-windows-msvc ^
    "/I<sysroot>\VC\Tools\MSVC\<ver>\include" ^
    "/I<sysroot>\Windows Kits\10\Include\<wk>\ucrt" ^
    "/I<sysroot>\Windows Kits\10\Include\<wk>\um" ^
    "/I<sysroot>\Windows Kits\10\Include\<wk>\shared" ^
    /c hello.c /Fohello.obj

  lld-link.exe hello.obj ^
    "/LIBPATH:<sysroot>\VC\Tools\MSVC\<ver>\lib\x86_64" ^
    "/LIBPATH:<sysroot>\Windows Kits\10\Lib\<wk>\ucrt\x86_64" ^
    "/LIBPATH:<sysroot>\Windows Kits\10\Lib\<wk>\um\x86_64" ^
    /OUT:hello.exe /SUBSYSTEM:CONSOLE
  ```
  For `aarch64-pc-windows-msvc`, swap `--target=` and every `\x86_64\`
  library path segment for `\aarch64\`, and add `/MACHINE:ARM64` to the
  `lld-link` invocation.
- **Two gotchas that cost real time getting here, worth documenting so
  Phase 2+ doesn't rediscover them**:
  1. **Drive clang-cl/lld-link from `cmd`/PowerShell, never Git Bash/MSYS.**
     MSYS auto-translates any bare `/Xxx` argument into a fake Windows path
     before exec'ing a non-MSYS binary (it can't distinguish an `/Fo...`
     compiler flag from a POSIX path) — this silently corrupted `/Fo`,
     `/LIBPATH:`, etc. with no obviously-related error message. Any
     CMake/build-script invocation of this toolchain must run under a
     native Windows shell, not Git Bash — relevant since local dev on this
     project happens from Git Bash by default.
  2. **Compile and link as two separate steps** (`clang-cl /c` → `lld-link`
     directly) rather than clang-cl's automatic `/link`-forwarding mode
     combined with `-fuse-ld` — that combination produced confusing
     argument-parsing errors (`/LIBPATH:...` misparsed as an input file).
     The explicit two-step form is also just more transparent for a CMake
     custom toolchain file to reason about.
- **Cosmetic warnings, not errors**: compiling against the SDK headers
  produces ~7 `-Wignored-attributes` warnings from `corecrt_wstdio.h`/
  `stdio.h` macro expansions (`__DEFINE_CPP_OVERLOAD_STANDARD_FUNC_*`) — a
  known clang-vs-Windows-SDK-headers ordering nitpick, harmless. Add
  `-Wno-ignored-attributes` to the Windows compile flags to keep CI output
  clean rather than let it accumulate as noise.
- **Single self-contained `.exe`, no shipped DLLs**: use `/MT` (static
  CRT) instead of the default `/MD` (dynamic CRT) — the clang-cl/MSVC
  equivalent of MinGW's `-static`, bakes the UCRT/vcruntime statically into
  `shimback.exe` so there's nothing extra to ship alongside it, matching
  the plain-exe experience the macOS/Linux builds already have.
- **`getopt` vendoring is needed** (this reverses the "no vendoring needed"
  conclusion from when MinGW-w64 was the plan) — `getopt.h` isn't part of
  the Microsoft UCRT headers regardless of which compiler targets them, so
  vendor a small public-domain shim (e.g. `ya_getopt`) for `cli.c`'s CLI
  parsing on Windows.

### Phase 2 — dispatch.c (process model)

**✅ DONE as of 2026-09-23.** `plat_run_inherited`, `plat_run_captured`,
and `plat_capture_stdout` are fully implemented in `platform_win32.c` via
`CreateProcessW`/`GetExitCodeProcess`, with zero changes needed to
`dispatch.c` itself (exactly as Phase 0's seam was designed to allow).
Verified end-to-end with real, hand-crafted `config.toml` shims (`cmd.exe`
as source/fallback, no Phase-3 hard-link creation needed since dispatch is
driven purely by `argv[0]`'s basename — copying `shimback.exe` to a shim
name is enough to exercise it): confirmed all three of (1) an invisible
failing trial run correctly triggering a live fallback, (2) an invisible
succeeding trial run correctly replaying its captured output, and (3) a
trial run that outlasts `capture_timeout_ms` correctly committing to live
relay mid-stream and never falling back afterward (~3.1s real elapsed time
for a ~3s child, output split correctly across the timeout boundary).

**Design decisions made getting there**:
- **Windows collapses POSIX's fork-vs-exec failure distinction differently
  than planned**: originally this doc expected to mirror POSIX's "fork
  failed is fatal, execv failed in the child looks like exit 127" split.
  Windows has no fork/exec split at all (`CreateProcessW` does both
  atomically), so the mapping instead is: `CreateProcessW` itself failing
  (exe not found/not runnable — the common case) → the exit-127-with-
  message path, exactly matching what callers already saw from POSIX's
  execv-failure branch; `-1` is reserved for something failing *after* the
  process already started (`WaitForSingleObject`/`GetExitCodeProcess`
  itself failing) — the genuine Windows analogue of POSIX's "waitpid
  failed". This preserves the same two-tier caller-visible contract
  `plat_run_inherited`'s doc comment already promised, just remapped to
  where Windows actually has two distinct failure points.
- **`CreateProcessW` takes one command-line *string*, not an argv array** —
  the child re-parses it into its own argv using the C runtime's documented
  quoting rules, so the parent must escape exactly the way the child will
  un-escape (a run of N backslashes before a quote becomes 2N+1 plus an
  escaped quote; N backslashes at the very end becomes 2N). Implemented as
  `build_command_line`/`append_quoted_arg` in `platform_win32.c`, verified
  with arguments containing spaces and shell metacharacters (`cmd.exe /c
  "echo X && exit 1"`-shaped commands) round-tripping correctly.
- **Anonymous pipes (`CreatePipe`) can't be waited on via
  `WaitForMultipleObjects`** the way a POSIX fd can via `poll()` — no
  overlapped I/O support at all. The documented alternative (named pipes
  with `FILE_FLAG_OVERLAPPED`) is substantially more machinery than this
  needs; used `PeekNamedPipe`-based polling instead (a 15ms sleep between
  checks when neither stream has data ready) — still fully correct
  (`PeekNamedPipe` never consumes bytes, so nothing is ever missed) and the
  poll interval is irrelevant at CLI-tool scale. Confirmed via the timeout
  test above that this doesn't introduce any noticeable latency or
  data loss across the live-relay transition.
- Exit-code decoding: `GetExitCodeProcess`'s value is passed straight
  through with no folding — Windows has no `SIGKILL`-style signal-death
  convention to fold into a `128+n` range the way POSIX does.

**Bug found and fixed while verifying this, not in `dispatch.c` at all**:
Phase 1's vendored `getopt_long` (`getopt_win32.c`) had a real permutation
bug — `rotate_to_front` moved only the *option flag* to the front when
skipping past a positional argument to find it, not the flag *and* its
required argument together. `shimback add <name> -s <source> -f <fallback>`
(the documented, primary usage form — name before any options) silently
fed the wrong values to `-s`/`-f` as a result: confirmed by testing with
distinguishable fake values (`fakesource12345.exe` ended up being checked
as the *fallback*, not the source). This shipped and passed compilation in
Phase 1 — only surfaced now that `add`'s full validation path was actually
exercised end-to-end. Fixed with `rotate_block_to_front` (a 1-or-2-slot
block rotation, sized via a new `option_block_len` helper) and reverified
with distinct source/fallback values resolving correctly. **Lesson**:
compiling cleanly proved nothing about `getopt_long`'s correctness — only
running it with real, distinguishable arguments did.

**Addendum (new `passthrough` policy, added post-1.0-port for both
platforms)**: a new `POLICY_PASSTHROUGH` skips `plat_run_captured()`
entirely and calls `plat_run_inherited()` directly for source, then again
for fallback on any non-zero exit -- exactly the "up-front, live/inherited
stdio" shape `dispatch_run()` already used for `route-args`/`rewrite`/
`split-args`/`route-map`, just gated on the exit code afterward instead of
on the arguments beforehand. No new platform seam needed -- both
`run_inherited_or_die()` call sites this policy uses already existed.
`--diagnostic`/`--capture-timeout`/`--capture-limit` are rejected alongside
this policy in `validate_shim_entry()`, since nothing is ever captured or
hidden for any of them to apply to. Verified end-to-end on both platforms:
Windows (a from-scratch sandbox, see this phase's own mkdir_p addendum
under Phase 4 for why that needed its own fix first) and Linux (WSL,
compiled directly with `gcc` against the existing source list since no
`cmake` was available there) -- success, fallback, and both rejected-flag
cases all behaved identically on both, plus the full existing `tests/*.sh`
suite (extended with a new `passthrough` section in `test_dispatch.sh`)
stayed green on Linux.

### Phase 3 — shim links (add/remove/doctor/info/uninstall)

**✅ DONE as of 2026-09-23**, except the `update`-staleness fix below
(deliberately deferred — see why at the end of this section). Verified
end-to-end on real hardware, not just compiled: `add` creates a genuine
`CreateHardLinkW` hard link (confirmed via `fsutil hardlink list` showing
two names sharing one file, and `(Get-Item).LinkType` reporting
`HardLink`); the created shim actually dispatches correctly when invoked
directly; `list`/`doctor`/`info` correctly recognize it; `doctor fix`
correctly recreates a manually-deleted ("missing") hard link; `remove` and
`uninstall --full` correctly delete it. Full cycle (`add` → `list` →
`doctor` → `doctor fix` → `remove`/`uninstall`) run for real, repeatedly,
against `cmd.exe`/`where.exe` as source/fallback.

**New shared primitives, used everywhere the old per-file lstat+S_ISLNK
dance used to be duplicated**:
- `plat_link_create(target, link_path)` — `symlink()` on POSIX,
  `CreateHardLinkW` on Windows.
- `is_shim_dir_entry(entry_path)` (paths.c/paths.h, not platform.h — this
  is business logic with a platform-specific *shape*, not a thin OS-
  primitive wrapper) — replaces the `lstat`+`S_ISLNK`+`canonicalize`+
  `looks_like_shimback_binary` dance previously duplicated across
  `paths.c`, `add.c` (×2), `doctor.c` (×2), `remove.c`, `uninstall.c`.
  POSIX: unchanged behavior (symlink check, then resolve, then marker
  scan). Windows: `looks_like_shimback_binary(entry_path)` directly, no
  resolution step at all — a hard link's content already *is* the shared
  file's content, so there's nothing to follow first. This is a genuine
  asymmetry, not a shortcut: Windows structurally cannot distinguish "a
  hard link to shimback" from "a plain file containing the same bytes" by
  any syntactic check, and doesn't need to — both are equally valid,
  functioning shims.
- `shim_file_name(name)` / `shim_name_from_file(filename)` (paths.c/
  paths.h) — the `.exe` suffix shims need on Windows (`cmd.exe`/PowerShell
  only resolve a bare command name against a PATHEXT-listed extension),
  appended/stripped symmetrically.

**`add.c`'s `finish_add` rewritten from a Windows-excluded `#else` block
(Phase 0/1) into genuinely portable code** — removed the early `die()`
bailout entirely; only two small POSIX-only islands remain (`#ifndef
_WIN32`): the writable-by-group-or-other directory check (no cheap ACL-
equivalent check exists on Windows; default per-user directory ACLs
already aren't other-writable there, unlike a misconfigured POSIX
directory, so this is a disclosed, low-priority gap, not silently assumed
safe) and the exact wording of one error message (POSIX still
distinguishes "not a symlink" from "not shimback-managed"; Windows unifies
both into one "not a shimback-managed shim" message, since a hard link has
no separate "wrong type" case to distinguish — a minor, deliberate UX
simplification, not a functional change).

**Bugs found only by testing real hard-link creation, not by compiling**:
- **`plat_link_create`/`plat_rename_replace` didn't set `errno`** —
  `CreateHardLinkW`/`MoveFileExW` only ever set `GetLastError()`, never
  the CRT `errno`, but every calling site's error message uses the
  existing `strerror(errno)` pattern throughout this codebase. Without a
  translation, failures reported stale, unrelated garbage (`add` first
  failed with a nonsensical "File exists" for a shim name that had never
  existed). Fixed with a small `set_errno_from_win32()` mapping in
  `platform_win32.c` (`ERROR_NOT_SAME_DEVICE`→`EXDEV` in particular —
  the exact, correct signal for hard links refusing to cross volumes).
  Once fixed, this **immediately surfaced a real, expected constraint**:
  this dev machine's build lives on `D:\`, the default shim directory on
  `C:\Users\...` — cross-volume, so `add` correctly failed with `EXDEV`.
  Not a bug; exactly the cross-volume constraint already flagged below —
  but "a clear message" turned out to be wrong: **the message itself was
  not actually clear** (see the Phase 6 addendum below, where a real user
  hit this through the interactive wizard and the raw `strerror(EXDEV)`
  text — the MSVC CRT's own string for it, literally "Improper link" —
  turned out to be meaningless without already knowing what `EXDEV` is).
  Fixed properly once someone actually hit it blind; see that note.
- **A real POSIX regression**, caught by the WSL test suite (`test_doctor`
  failed) after rewriting `fix_symlink_if_needed` for portability: the
  rewrite used `access(link_path, F_OK)` to detect "missing", but
  `access()` *follows* symlinks — for a *dangling* symlink (entry exists,
  target doesn't), that reports "doesn't exist" too, wrongly conflating
  "missing" with "dangling" and skipping the `unlink()` step before
  recreating, causing a "File exists" failure recreating a shim that was
  supposed to be dangling, not missing. Fixed by restoring the original
  `lstat()`-based check (which does not follow symlinks) for the POSIX
  branch specifically, using `access()` only on the Windows branch, where
  there's no separate target-resolution step to conflate it with. **Every
  POSIX-visible file touched this phase was re-verified against the full
  WSL suite** (15/15, including this catch) before considering it done.
- **`.exe` suffix omitted at more call sites than expected**: found and
  fixed in `remove.c`, `list.c`, `doctor.c` (×2 more, beyond the two
  already covered above), and `info.c` (×3 separate `path_join(shim_dir,
  name)` occurrences — the symlink-diagnostic display, the ASCII flow
  diagram, and the orphan-detection path). A full `grep` sweep for every
  `path_join(shim_dir, ...)` call site was needed to find them all — easy
  to miss one at a time, since each compiles fine and only breaks at
  runtime (looks for `name` instead of `name.exe`, silently finds
  nothing). `info.c`'s flow diagram also hardcoded "(symlink)" wording,
  fixed to say "(hard link)" on Windows.

**Deliberately deferred, not forgotten**: the `update`-staleness fix
described below. `platform_asset()` (Phase 1) already always returns
`false` on Windows — no Windows release artifact exists until Phase 7
ships one — so `update` can never actually reach the code path that would
trigger this staleness today; there is no live bug to fix yet, only a
known one to fix *before* Phase 7 makes Windows self-update possible.
Tracked here so it isn't rediscovered cold at that point.

**Load-bearing design problem, found while tracing this for the Phase 0
pass (2026-09-22) — read before writing any of the code below.** A hard
link references the underlying file *data* directly; a symlink references
a *path*, re-resolved on every access. `shimback update` (`update.c:315`,
via `copy_executable` → `copy_file_mode` in `paths.c`) replaces the
installed binary by writing the new one to a temp file and atomically
renaming it over the old path (`plat_rename_replace` / `MoveFileExW` with
`MOVEFILE_REPLACE_EXISTING`, once that's wired up) — this creates a *new*
file object at that path rather than mutating the old one in place. On
POSIX, every shim symlink transparently starts resolving to the new binary
the instant that rename completes, for free, because symlinks are
path-based. On Windows, **every existing hard-linked shim would keep
pointing at the old binary's bytes indefinitely** — silently stale after
every single `update`, still fully functional (it's a complete, valid copy
of the old binary), just permanently frozen at the pre-update version with
no error or warning. This is a real correctness gap the hard-link decision
introduces, not a cosmetic porting detail, and today's `update.c` has no
code path that would even notice, let alone fix it.

The fix has to be explicit: Windows' `update` needs to enumerate every
existing shim (the same walk `list`/`doctor` already do) and **recreate
each hard link** (`DeleteFileW` + `CreateHardLinkW` against the newly
-installed binary) as part of the update, not just replace the binary
file itself. Budget real design/test time for this in Phase 3 — it's the
single most consequential difference the symlink→hard-link swap
introduces, and it touches `update.c` (Phase 7 territory) as much as it
touches `add.c`/`doctor.c` here, so treat this bullet list and Phase 7's
update-related work as one design problem, not two independent phases.
A Pester spec for exactly this scenario (`add` a shim, simulate `update`,
assert the shim's hard link now resolves to the new binary's content) is
worth writing in Phase 3 rather than deferring it to Phase 8's general
pass, precisely because it's easy for this regression to hide silently
otherwise — the shim keeps working, it's just wrong.

- Swap `symlink()` → `CreateHardLinkW` in `add.c:566`, `add.c:594`, and
  `doctor.c:127`'s repair path.
- **Related finding**: `doctor.c`'s `fix_symlink_if_needed` (`doctor.c:90`)
  repairs two cases — "missing" (the symlink entry itself is gone) and
  "dangling" (the symlink exists but its target no longer does, e.g.
  shimback was moved/deleted without running `uninstall`). "Dangling" has
  **no hard-link equivalent at all** — a hard link doesn't reference a
  target that can disappear out from under it; as long as the shim's own
  hard link exists, the file data it shares stays alive regardless of what
  happens to `self_exe`'s own path (see the update-staleness problem
  above — same underlying property, opposite symptom: there, staying alive
  after `self_exe` changes is the *bug*; here, it's *why "dangling" can't
  happen*). Windows' `doctor` only ever has "missing" to detect and repair
  for this specific check; don't port the dangling branch's logic, replace
  it with nothing rather than a no-op stand-in.
- Swap the `lstat()`/`S_ISLNK` detection used to recognize an existing shim
  (`doctor.c:check_symlink`, `info.c:print_symlink`) for a hard-link-aware
  check: compare `nFileIndexHigh`/`nFileIndexLow` (via
  `GetFileInformationByHandle`) against the shimback binary's own file
  index to confirm a shim dir entry is a hard link to `self_exe`, since
  Windows has no `readlink()`-equivalent to recover "what does this point
  at" from a hard link (unlike a symlink, a hard link has no separate
  target to read — this changes what `doctor`/`info` can *report*, not just
  how they detect it; update their output strings accordingly).
- `remove.c`/`uninstall.c`'s `unlink()` calls map straight to
  `DeleteFileW` — no behavior change.
- Decide the shim binary's Windows name: shims must be `<name>.exe` (or
  registered in `PATHEXT`) for `cmd.exe`/PowerShell to resolve them without
  an extension the user has to type. `add.c` will need to append `.exe`
  when creating the hard link, and every place that derives a shim name
  from a link's filename (`doctor.c`, `list.c`, `paths.c`'s split-config
  scan) needs to strip it back off symmetrically.
- Cross-volume install note: hard links require the shim directory and the
  shimback binary to be on the same NTFS volume. Since both already live
  under the same install prefix, this should hold by construction — add a
  `doctor` check that flags it explicitly (clearer failure than a raw
  `CreateHardLinkW` error) rather than assuming it silently.

### Phase 4 — paths.c (home/config/data resolution, scanning)

**Done.** Mostly already covered by Phases 0–1 — `home_dir()` (via
`plat_home_dir`: `%USERPROFILE%`, falling back to `SHGetKnownFolderPath`),
directory scanning (`plat_list_dir` → `FindFirstFileW`), and `realpath()`
(`plat_realpath` → `CreateFileW` + `GetFinalPathNameByHandleW`, which
actually resolves reparse points, a bit stronger than the
`GetFullPathNameW`-only approach originally sketched here) were all live
and verified working on Windows already. The one remaining piece — the
config/data default-location decision — is now resolved and implemented:

- **Decision (revised)**: default to the same `%USERPROFILE%\.config\shimback`
  (config) / `\.local\share\shimback` (data) layout macOS/Linux already
  use -- deliberately, not the Windows-conventional
  `%APPDATA%\shimback`/`%LOCALAPPDATA%\shimback`, so a config shared
  across WSL/Git-Bash/MSYS2 and native Windows (or a dotfiles repo already
  laid out that way) keeps working without a separate Windows-only
  location. (First implemented the other way around --
  `%APPDATA%`/`%LOCALAPPDATA%` preferred, `.config`/`.local/share` as the
  fallback -- then flipped once actually in use: not worth keeping two
  real defaults' worth of complexity for a preference that turned out to
  go the other way in practice.) If the home-based `shimback`
  subdirectory doesn't exist yet *and* `%APPDATA%\shimback`/
  `%LOCALAPPDATA%\shimback` already does (an install made back when that
  was the default), uses that instead -- a one-way migration fallback, not
  a permanent dual-lookup: a fresh install gets the home-based default
  outright (neither exists); an install that already has the
  `%APPDATA%`-based layout keeps working exactly as before, un-stranded,
  until the user (or a future `doctor`/`migrate` step, not built) moves
  it. Once both exist, the home-based location always wins.
  `XDG_CONFIG_HOME`/`XDG_DATA_HOME`, if explicitly set, still
  short-circuit this entirely on both platforms, unchanged.
- Implemented in `src/paths.c`: `base_dir()` gained a Windows-only branch
  (`win_base_dir()`) that checks `path_is_dir()` on the candidate
  `<base>/shimback` directories before picking one; `config_file_path()`/
  `shim_bin_dir()` pass `"APPDATA"`/`"LOCALAPPDATA"` through to it (the
  migration-fallback env var) alongside the `.config`/`.local/share`
  leaves, which are now the preferred ones, not the fallback.
- **Bug found while implementing this**: `xdg_env()`'s validation for
  `XDG_CONFIG_HOME`/`XDG_DATA_HOME` rejected any value not starting with
  `/`, silently treating a Windows-style value (`C:\Users\...`) as unset
  regardless of what the user configured. Not previously caught because
  nothing had exercised an `XDG_*_HOME` override on Windows before now.
  Fixed with a new `path_looks_absolute()` helper that also accepts
  drive-letter (`C:\`/`C:/`) and UNC (`\\server\share`) forms on Windows.
- **Verified for real**, not just compiled, twice -- once for each
  ordering. Current (home-based default): (1) fresh install (neither
  exists) → home-based `.config`/`.local/share`; (2) only
  `%APPDATA%`/`%LOCALAPPDATA%`'s `shimback` dirs exist (the migration
  case) → those; (3) both exist → home-based wins; (4) `XDG_CONFIG_HOME`/
  `XDG_DATA_HOME` set to Windows-style paths → those win outright,
  unconditionally. All four matched expectations. Re-verified the WSL
  POSIX suite afterward (15/15 still passing) both times, since
  `base_dir()`/`xdg_env()` are shared, not Windows-exclusive, code.
- All path joining still hardcodes `/` (`path_join()`) — confirmed cosmetic
  only (Windows APIs tolerate the mixed separators this produces, verified
  in Phase 1 testing), but worth cleaning up for a polished 1.0, not just
  left forever. Low priority relative to Phase 5, the only thing left
  outstanding from this phase.

**Addendum (found while testing the `passthrough` policy, see Phase 2's own
addendum below)**: `mkdir_p()`'s per-component walk always tried to
`mkdir`/`stat` the bare Windows drive-letter root (`"C:"`, `"D:"`, no
trailing separator) as if it were an ordinary path component, since its
loop only ever skipped a POSIX leading `/`. `_wmkdir`/`stat` on that bare
form turned out to behave inconsistently across Windows/CRT versions --
`_wmkdir("C:")` returned `EACCES` on one machine, immediately aborting the
whole call; on another it returned `EEXIST` as hoped, but the very next
`stat("C:")` (no trailing separator) still reported "not a directory",
aborting it anyway with `ENOTDIR`. Every prior real Windows test this
project ran happened to install into a location whose full parent chain
already existed up to and including the drive root reported success for
unrelated reasons, or never exercised a from-scratch `mkdir_p` on this
exact machine/CRT combination -- this was only caught because testing
`passthrough` needed a fully isolated, from-scratch `$XDG_CONFIG_HOME`
sandbox (real `init`/`add` couldn't be used directly either, since
`shell.c`'s PowerShell-profile lookup uses the real Windows "Documents"
special folder regardless of `$HOME`/XDG overrides -- a separate, accepted
limitation for local dev testing, not a bug). Fixed by skipping straight
past `"C:/"` (or bare `"C:"`) to the first real path segment, the same way
the existing code already skips a POSIX leading `/` — a drive root always
exists and is never usefully `mkdir`'d.

### Phase 5 — shell integration (shell.c) — the other genuinely new subsystem

**Done.** Added `SHELL_POWERSHELL`/`SHELL_CMD` to `ShellKind` alongside
zsh/bash/fish, and a `shell_all_kinds()` helper (POSIX: zsh/bash/fish;
Windows: PowerShell/cmd) that `install --all`, `init`, `uninstall`, and
`find_installations` now all call instead of each hardcoding its own
POSIX-only array — one shared platform split instead of four duplicated
ones.

- **Detection** (`detect_current_shell()`): Windows has no `$SHELL`
  equivalent, so this walks the immediate parent process via
  `CreateToolhelp32Snapshot` (new `plat_parent_process_name()`) —
  `powershell.exe`/`pwsh.exe` → `SHELL_POWERSHELL`, `cmd.exe` → `SHELL_CMD`.
  Verified for real by invoking `shimback add`/`install` as a direct child
  of both a real `cmd.exe /d /c` and the PowerShell tool's own `pwsh.exe`
  process and confirming the right branch fired each time.
- **PowerShell**: reuses the *exact same* idempotent marker-block machinery
  as zsh/bash (`ensure_dir_in_block`/`find_block`/`remove_block`,
  generalized with a new `BodyStyle` enum instead of a `zsh_style` bool),
  writing `$env:Path = '<dir1>' + ';' + '<dir2>' + ';' + $env:Path` into
  `$PROFILE.CurrentUserAllHosts` — computed directly via a new
  `plat_documents_dir()` (`SHGetKnownFolderPath(FOLDERID_Documents)`)
  rather than by shelling out to a spawned `pwsh`/`powershell` process,
  specifically to avoid a subprocess spawn on every `shimback add`. Both
  editions' profiles (`Documents\WindowsPowerShell\profile.ps1` and
  `Documents\PowerShell\profile.ps1`) are touched whenever their binary is
  found on PATH, regardless of which one was actually detected as the
  *current* shell — since which edition ends up running a later session
  can't be known in advance, this is the only way `shimback add` under one
  edition still works correctly if the user launches the other one next.
- **cmd.exe**: `HKCU\Software\Microsoft\Command Processor\AutoRun`.
  **Two real, load-bearing behaviors found only by testing, not from any
  documentation consulted while planning this**:
  1. AutoRun executes *only its first line* — an embedded newline is
     **not** a further command the way a batch file's lines are. A
     multi-line, `\n`-separated value (the original plan, mirroring the
     file-based `#`-marker scheme) silently ran just its first line and
     dropped everything else, with no error.
  2. `rem` (cmd.exe's comment syntax) consumes the rest of the *physical
     line* regardless of any `&` that follows it — `rem foo & echo bar`
     never runs `echo bar`. So `rem`-based markers, even reformatted onto
     one `&`-joined line, don't work either: a `rem` start marker would
     silently swallow every command chained after it, including the real
     payload.

  Both confirmed directly: setting known AutoRun values via the registry
  and observing `cmd.exe /c`'s actual behavior (a plain `echo`-based probe
  first, then a multi-line one, then an `&`-joined one, then `rem`
  specifically) rather than trusting assumptions about how AutoRun works.
  **Fix**: the whole managed segment is one `&`-joined line, and two
  harmless `set "shimback_block_<tag>=begin/end"` assignments serve as the
  markers instead of `rem` — found via a plain (non-line-anchored)
  substring search (`find_autorun_segment`), not the line-anchored
  `find_marker_line` the file-based blocks use, since AutoRun genuinely
  has no lines. The env-var side effect of the two marker `set`s is a
  deliberate, accepted trade for a marker that actually survives being
  `&`-joined. Payload: `set "PATH=<dir1>;<dir2>;%PATH%"`, dying if any
  directory contains a `"` (cmd.exe's quoting has no in-band escape for
  one, unlike POSIX/PowerShell's single-quote doubling — the same
  injection class `append_sh_squoted`/`append_ps_squoted` close, just with
  no safe way to neutralize it here, so it's refused outright).
- **`HKCU\Environment\Path` fallback layer** (new `plat_win_userenv_path_add`/
  `_remove`): both `ensure_powershell`/`ensure_cmd` also persist the shim
  dir here — case-insensitively deduped, `WM_SETTINGCHANGE`-broadcast —
  for GUI-launched tools and other non-interactive contexts that never load
  a profile/AutoRun. Confirmed this is real, global, per-user state (not
  sandboxable via env-var overrides the way `%USERPROFILE%`/`%APPDATA%`
  are for file-based testing) the hard way: an early manual test left a
  scratch test directory sitting in the *actual* logged-in user's real
  `HKCU\Environment\Path` until it was caught and cleaned up by hand. Every
  later test of this specific code path re-verified the real registry key
  was back to its original value immediately afterward, not just that the
  command reported success.
- **Bug found via this same real-registry testing**: `remove_autorun_block`
  passed `out.data` straight to `plat_win_autorun_set` instead of
  `dynbuf_cstr(&out)` — for the common case (the AutoRun value contained
  *only* shimback's own block), the resulting `DynBuf` had nothing ever
  appended to it, leaving `.data` `NULL`. `plat_win_autorun_set(NULL)`
  failed silently inside `utf8_to_wide(NULL)`, so `uninstall --full`
  printed its normal success output while the AutoRun registry value
  stayed completely untouched. Fixed at all three `plat_win_autorun_set`
  call sites; re-verified with a real `install` → `uninstall --full`
  cycle showing the AutoRun value actually going back to empty.
- Verified end-to-end, for real, not mocked: `add`/`install`/`uninstall
  --full` run as direct children of both `cmd.exe` and PowerShell,
  inspecting the real `$PROFILE` file content, the real AutoRun registry
  string (`reg.exe query`, not just PowerShell's provider, to rule out any
  caching), and the real `HKCU\Environment\Path` value before and after —
  including idempotency (re-adding the same shim dir is a silent no-op)
  and the full install→uninstall symmetry leaving zero trace in either the
  registry or the filesystem. Re-ran the WSL POSIX suite (15/15) after
  every `shell.c` change, since the marker-block machinery is shared code,
  not Windows-exclusive.
- **Known, stated limitation** (for the README, not solved here): a shell
  already open before install, or one started with `-NoProfile`/`/D`,
  won't get the prepend until restarted — the same login-vs-non-login
  caveat bash already has today, not a new class of problem from this
  port.
- **Not done / deferred**: the Machine-vs-User PATH first-in-PATH ordering
  guarantee is delivered by the profile/AutoRun session-prepend (same as
  originally planned), not the registry fallback layer — unchanged from
  the original design. A formal Pester spec for any of this is still owed
  (see Phase 8's standing note).

### Phase 6 — tui.c (interactive add wizard)

**Implemented and build-verified; one real gap, see below.** `tui.c` itself
needed zero changes — its escape-sequence key-parsing state machine was
already portable, exactly as planned; only the four `plat_tty_*`/
`plat_*_stdin_byte*` primitives needed a real Windows implementation.

- **Design change from the original plan**: raw mode is `GetConsoleMode`/
  `SetConsoleMode` as planned (clearing `ENABLE_LINE_INPUT |
  ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT` — the last one is what makes
  Ctrl-C arrive as a plain 0x03 byte instead of the console's default
  SIGINT-like handling, the Windows analogue of POSIX raw mode clearing
  `ISIG`), with the same `g_saved_console_mode`/`atexit`-restore shape as
  POSIX's `g_saved_termios`. But *input* is read via `ReadConsoleInputW`'s
  raw `INPUT_RECORD` stream and translated by hand into the byte stream
  `tui_read_key()` expects (arrow keys synthesized as the same 3-byte
  `ESC [ <letter>` sequence POSIX gets from a real terminal; everything
  else taken from the key event's own `UnicodeChar`, UTF-8-encoded),
  **not** `ENABLE_VIRTUAL_TERMINAL_INPUT`'s `ReadFile`-level ANSI
  translation as the plan originally called for — that would still need a
  way to tell a genuine VT byte from a window-resize/focus-change event,
  and its exact behavior isn't consistent between conhost.exe and Windows
  Terminal. Reading `INPUT_RECORD`s directly sidesteps both: anything
  that isn't a key-down `KEY_EVENT` is simply discarded before it ever
  reaches `tui_read_key()`. `plat_stdin_byte_ready()`'s timeout is built
  on `WaitForSingleObject` on the console handle, which is documented to
  signal for *any* unread input record (not just real keystrokes) — a
  pending resize/key-up event is drained and discarded internally, with
  the remaining time budget carried over, rather than reported as "ready"
  with nothing actually readable.
- **`ENABLE_VIRTUAL_TERMINAL_PROCESSING`** (new `plat_enable_vt_output()`,
  called once from `main()` before any output): needed more broadly than
  just the wizard turned out to be — this codebase already emits ANSI
  color codes throughout (`util.c`'s `ANSI_*` constants, used by `doctor`/
  `info`/`install`/etc., not just `tui.c`'s clear-screen), so this is
  enabled unconditionally at startup rather than only around raw-mode
  entry. A no-op (and safe to call) when stdout/stderr aren't real
  consoles, consistent with `plat_isatty_stdout`/`stderr` already gating
  whether any ANSI is emitted in the first place.
- Removed the now-fully-superseded `not_yet_implemented()` stub and its
  three call sites.
- **Verified**: clean Windows build (both `shimback` and
  `test_config_parser` targets); `shimback add <name>` with missing
  required flags falls back to the same clean `die()`-with-usage path it
  always has rather than hanging, when no real console is available to
  enter raw mode on (confirmed this is genuinely what happens in a
  redirected/piped stdio context — the same context every tool call in
  this session runs under, confirmed directly via `GetConsoleMode`
  failing on stdin/stdout/stderr alike here). Re-ran the WSL POSIX suite
  (15/15, including `test_add_wizard`, which drives the real interactive
  wizard over a pty) to confirm the shared `tui.c`/`main.c`/`platform.h`
  changes didn't regress the POSIX raw-mode path.
- **Known gap, not resolved this session**: actual live keystroke
  navigation (arrow-key movement between prompts, live character echo,
  Ctrl-C mid-wizard, ...) was never exercised for real. Every tool call
  available this session runs with stdio redirected to pipes for
  programmatic output capture, not a real attached console — confirmed
  directly (`GetConsoleMode` fails on stdin/stdout/stderr in that
  context) rather than assumed — so there is no way from here to drive
  `ReadConsoleInputW`'s actual `INPUT_RECORD` stream with real
  keystrokes; a redirected/piped stdin correctly (and by design) makes
  `tui_supported()` report false and the wizard never engages at all,
  which is exactly what was confirmed above, not a bypass of the real
  path. **Needs a manual smoke test**: run `shimback add` with a required
  value omitted in a real Windows Terminal/conhost.exe session, and
  confirm prompts render, arrow keys move between choices, typed
  characters echo and Backspace erases them correctly, and Ctrl-C aborts
  cleanly leaving the terminal in a normal (non-raw) state afterward.

**Addendum — the manual smoke test happened, and found a real bug**: the
user ran the wizard for real (arrow keys, typed values, the works) and it
worked all the way through prompt navigation — raw mode, key translation,
and rendering all functioned correctly, closing out the one gap above.
But the final save step failed with `shimback: add: failed to create
symlink C:\Users\Pablo\AppData\Local/shimback/bin/java.exe: Improper
link`. This is the exact `EXDEV` cross-drive constraint from Phase 3
(the dev build ran from `D:\`, the shim directory defaulted to `C:\...`)
— correctly detected, but Phase 3's error message was, in hindsight, not
actually clear at all: `"Improper link"` is the MSVC CRT's own string for
`EXDEV`, meaningless without already knowing what `EXDEV` means or that
shims are hard links. A user who has never read this plan document has no
way to act on it. **Fixed**: a new shared `format_link_create_error()`
(`paths.c`/`paths.h`, used by both `add` and `doctor fix`'s
`plat_link_create` failure sites) detects `EXDEV` specifically on Windows
and explains what's actually happening — the shim directory and
shimback's own running binary are on different drives, hard links can't
cross drives, and re-running `install` alone won't fix it (the *running*
binary is what gets linked, so shims must be added via whichever copy
lives on the shim directory's own drive) — with two concrete fixes
offered (`install --prefix` onto the same drive, or `XDG_DATA_HOME`
pointed at shimback's own drive instead). Any other errno still falls
back to the plain `strerror()`-based message, unchanged. Verified by
reproducing the user's exact failure shape (a `D:`-drive build, a
`C:`-drive shim directory via `XDG_DATA_HOME`) and confirming both the
new message text and that the existing rollback logic still leaves no
half-written config behind. Re-ran the WSL POSIX suite (15/15) since
`paths.c`/`add.c`/`doctor.c` are shared code. A proactive `doctor` check
for this condition (rather than only a reactive error at `add`/`doctor
fix` time) remains a nice-to-have, still not built.

**Second addendum — `EXDEV` is now recovered from, not just explained**.
Comparing notes against how `mise` (already installed on the test
machine) does its own shimming turned up that it doesn't link at all: it
copies one small, purpose-built dispatcher binary under every managed
tool's name (confirmed directly -- `mise.exe` itself is ~106MB, but every
shim in `~/.local/share/mise/shims/` is an identical 240,128-byte file,
same MD5 across every tool name; classic BusyBox-style "one binary, many
names, dispatch on argv[0]/invoked-name", the same trick shimback's own
`dispatch_run(shim_name, ...)` already uses). A copy has no cross-volume
restriction at all, unlike a hard link. Considered and explicitly
rejected: detecting/using `sudo` (Windows 11's native `sudo` is opt-in,
disabled by default, and not available before very recent builds -- helps
too small a slice of users to build a fallback chain around) and
launching a UAC-elevated helper "transparently" (not actually achievable
without either disabling UAC system-wide -- out of scope for a CLI tool
to touch -- or pre-provisioning a Scheduled Task, which itself needs one
visible elevated consent to set up and then makes the tool *silently
self-elevate* on a later run, exactly the kind of behavior that gets
flagged by antivirus/EDR and undermines trust in a tool whose founding
design goal, back at the very start of this port, was specifically to
*avoid* ever needing elevation).

**Implemented**: a new shared `create_shim_link()` (`paths.c`/`paths.h`,
used by `add`'s two link-creation sites and `doctor fix`'s) tries the
normal hard link first and, only on `EXDEV`, falls back to a plain
`copy_executable()` of shimback's own binary under the shim's name --
recognized correctly everywhere else in the codebase with **zero further
changes needed**, since Windows shim detection (`is_shim_dir_entry`/
`looks_like_shimback_binary`) already works by scanning file *content*
for the embedded marker, not by link identity, so a byte-identical copy
passes exactly the same check a hard link's shared data would. Prints a
`warn_colored(ANSI_YELLOW, ...)` notice when the fallback fires, naming
the real trade-off: a copy doesn't share disk space with shimback's
binary and won't automatically reflect a later `shimback update` (the
copy equivalent of the hard-link staleness gap noted above -- both need
the same eventual `update` fix to re-propagate to every shim, so neither
approach is worse off here than the other already was). If the copy
*also* fails, `format_link_create_error()`'s message now says so
explicitly and still offers the same two remedies as before.
**Verified**: reproduced the user's exact failure shape again and
confirmed it now succeeds with the copy in place instead of dying;
confirmed the resulting shim actually works end-to-end (a real
`route-map` java shim routing correctly between two JDKs by an
argument-based flag) and is correctly recognized by `list`/`doctor`/
`remove`; confirmed the *normal*, same-drive path still produces a real
hard link, not a copy (`fsutil hardlink list` showing two names sharing
one file), so the fallback only ever engages when it's actually needed.
Re-ran the WSL POSIX suite (15/15) once more. **Known minor gap, not
fixed**: `doctor`'s per-shim display always says "(hard link to the
shimback binary)" regardless of which strategy actually produced it --
harmless (both are valid, functioning shims either way) but not strictly
accurate for a copy; telling them apart would need a real file-identity
comparison this codebase doesn't do yet (see Phase 3's own note on this
same limitation for "which shimback build" detection).

**Third addendum — a real gap in the copy-fallback's own messaging,
caught by the user's own follow-up question**: the copy always lands at
the same path a hard link would have (`shim_bin_dir()`, already covered
by the PATH block `add`/`install`/`init` write), so there's no *location*
problem -- but `add` is the only one of `create_shim_link()`'s three
callers that separately prints a "restart your shell" reminder (and
`add`'s own is only shown when `--verbose`, so even there it's not
guaranteed); `doctor fix` and `update -y` print nothing, which matters if
the *current* shell session predates the original `add`/`install`.
Fixed by folding a one-line reminder into `create_shim_link()`'s own
`warn_colored()` notice instead, so all three callers get it
unconditionally. Verified the new message text end-to-end; re-ran the
WSL POSIX suite (15/15).

### Phase 7 — install.sh equivalent + packaging

**Started — `update` now fully works on Windows, verified end-to-end
against a real, locally-hosted fake release (not mocked).**

- **`platform_asset()`**: returns `shimback-windows-x86_64` unconditionally
  (no `uname()` equivalent needed -- unlike macOS/Linux, this project only
  ever builds/ships Windows x86_64, ARM64 already deferred). This is what
  actually *unblocks* `update` on Windows at all -- it previously always
  returned `false` there ("no Windows release artifact exists yet"),
  which is exactly why the hard-link/copy staleness problem flagged in
  Phase 3 was unreachable until now.
- **`.zip`, not `.tar.gz`**: Windows 10+ does ship a `tar.exe`, but it's
  GNU tar (no zip support), not the zip-capable bsdtar also present at
  `%SystemRoot%\System32\tar.exe` -- and that one can lose a PATH race to
  Git for Windows' own GNU tar (confirmed directly: `where tar` on the
  dev machine listed Git's ahead of System32's). Extraction uses
  PowerShell's `Expand-Archive` instead (`-NoLogo -NoProfile
  -NonInteractive -Command "Expand-Archive -LiteralPath '...'
  -DestinationPath '...' -Force"`, single-quote-escaped via a new
  `ps_squote_into()`), always present and zip-capable regardless of what
  else is on PATH.
- **Real, cascading bug found and fixed**: `install.c` created the
  installed binary as a file literally named `shimback` (no `.exe`) on
  Windows -- syntactically valid, but neither `cmd.exe` nor PowerShell's
  own command resolution will run an extension-less file (confirmed
  directly: `Start-Process`/raw `CreateProcess` *can* launch it by exact
  path, but typing `shimback` at an actual prompt in either shell reports
  "not recognized" -- the one thing `install` exists to make possible).
  This had been silently wrong since Phase 5, missed because every
  earlier test used the binary only as a hard-link *source* or deleted it
  by path, never actually ran it by name after installing. An exhaustive
  sweep (`grep -rn '"shimback"'`) found the same bug baked into
  `installation.c` (`find_installations`'s own lookup -- meaning it could
  never have found a real Windows install at all), `uninstall.c`, and
  `update.c`'s extracted-binary lookup, all independently using the bare
  name. Fixed once, consistently, with a new shared `shimback_exe_name()`
  (`paths.c`/`paths.h`, `shim_file_name("shimback")` under a clearer
  name) used at all four sites.
- **Stale-shim auto-refresh** (closing the disclaimer `create_shim_link()`
  added in Phase 6's addendum): after replacing the binary, `update`
  hashes every configured shim and compares it against the newly
  installed binary's own hash -- catching both a hard link that kept
  pointing at the *old* file after `copy_executable()`'s atomic
  rename-replace, and a copy-fallback shim, which never shared data with
  it at all, with one uniform check (naturally a no-op on POSIX, where a
  symlink shim always resolves to whatever's at the target path *now*).
  Any stale shims found are listed and, after a `[y/N]` prompt (or
  unconditionally with a new `-y`/`--yes` flag -- `shimback update -y`,
  for unattended/scripted use, following the exact same convention
  `doctor fix -y` already established), refreshed via a new shared
  `refresh_shim_link()` (`paths.c`/`paths.h`): builds the replacement at a
  temp name via `create_shim_link()` (hard link, falling back to a copy
  on `EXDEV`, same as `add`) and atomically swaps it into place, held
  under the same `shim_dir_lock_acquire`/`release` pair `doctor fix`
  already uses so a concurrent `add`/`remove` can't race it.
- **Two real, separate crashes found and fixed while testing this for
  real** (both confirmed via `fprintf`/`fflush` tracing bisection, not
  guessed at):
  1. `setvbuf(stdout, NULL, _IOLBF, 0)` -- pre-existing code, unchanged
     since long before this port, never once exercised on Windows because
     `platform_asset()` always died first. MSVC's UCRT `_IOLBF` crashes
     outright on entry (a `/GS` stack-buffer-overrun failure). Fixed by
     using `_IONBF` (fully unbuffered) on Windows specifically, which
     gets the same "stdout/stderr interleave correctly when piped
     together" guarantee this call exists for, without whatever
     `_IOLBF`-specific UCRT bug this is.
  2. A genuine bug in the new stale-shim-refresh code: freed
     `list_shim_symlink_names()`'s result with `plat_free_dir_entries()`,
     which expects a NULL-terminated array (`plat_list_dir()`'s own
     convention) -- but `list_shim_symlink_names()`'s array has no such
     sentinel, sized by its own `*out_count` out-parameter alone (its
     backing capacity can legitimately exceed that count, from the
     doubling growth strategy building it). Scanning it for a NULL
     terminator read past the real entries into uninitialized memory and
     freed garbage pointers -- silent heap corruption, not detected until
     a later, unrelated allocation (`STATUS_HEAP_CORRUPTION`, not even at
     the actual bad `free()` call), which is exactly what made this one
     slow to isolate. Fixed with a plain bounded loop instead.
- **Verified for real**: built a throwaway "release" (bumped `VERSION` to
  0.1.1, rebuilt, packaged the result as a real `.zip` + `SHA256SUMS` via
  `Compress-Archive`/`Get-FileHash`, served from a local directory via
  `SHIMBACK_RELEASE_URL=file:///...` -- `download()` already left a
  custom release URL protocol-unrestricted for exactly this kind of use)
  and ran the *actual* `update` command against it end-to-end: `--check`
  correctly detected the update; a real `update -y` downloaded, verified
  the checksum, extracted the `.zip`, swapped the binary, detected the
  one existing shim as stale, refreshed it, and left it genuinely
  hard-linked to the new binary again (`fsutil hardlink list` confirmed).
  `list`/`doctor` both correctly recognized everything afterward. Re-ran
  the WSL POSIX suite (15/15, including `test_update`) since
  `paths.c`/`installation.c`/`install.c`/`uninstall.c`/`update.c` are all
  shared code.
**`install.ps1`, CI, and README are now also done** (all three were the
"still to do" from the previous update):

- **`install.ps1`**: mirrors `install.sh` in PowerShell idioms rather than
  a line-by-line port -- `Invoke-WebRequest` instead of shelling out to
  `curl` (no extra dependency for a script that runs *before* shimback
  itself is even on disk), `Get-FileHash -Algorithm SHA256` instead of
  `sha256sum`/`shasum`, `Expand-Archive` instead of `tar`, all built into
  PowerShell already. `$env:SHIMBACK_VERSION` pins a release, matching
  `install.sh`'s own env var. Documented two invocation forms: a plain
  `irm ... | iex` one-liner for the common case, and a
  `& ([scriptblock]::Create((irm ...)))  --prefix ...` form specifically
  because `iex` alone can't forward arguments through a pipe the way
  `sh -s --` can -- a real difference from the POSIX one-liner, not an
  oversight. **Verified for real**: built a throwaway release (reused the
  same fake-`.zip`-`SHA256SUMS` technique from testing `update`), served
  it from a real local HTTP server (`Invoke-WebRequest` doesn't support
  `file://` the way `curl` does, so `SHIMBACK_RELEASE_URL`'s file:// trick
  from `update` testing doesn't carry over here), and ran the actual
  script against it with a temporary URL substitution -- confirmed the
  full flow (download, checksum, extract, install, forwarded `--prefix`)
  and the "already installed" no-op path both work, then confirmed the
  installed binary actually runs. Found and fixed one more small,
  related bug while at it: `install.c`'s "already installed" message
  still said `<dir>/shimback` (no `.exe`), inherited from before the
  cascading `.exe`-suffix fix above -- now uses `shimback_exe_name()`
  like everywhere else.
- **CI** (`.github/workflows/release.yml`): a new `build-windows` job,
  kept separate from the existing macOS/Linux matrix rather than folded
  into it as another leg -- every step here needs its own
  PowerShell/toolchain/packaging logic, not just a different shell for
  the same commands, so per-step `if: runner.os == 'Windows'`
  conditionals on the shared job would have been messier. Installs Ninja
  via Chocolatey and `xwin` via `cargo install` (both already available on
  GitHub's Windows runner image, matching this project's own local
  toolchain exactly -- clang-cl/lld-link ship with the runner's own LLVM
  install), splats the sysroot, builds, packages as `.zip`
  (`shimback-windows-x86_64.zip`), and smoke-tests the packaged archive
  (binary present and runs, `install` places a `.exe` at the expected
  path) the same way the existing macOS/Linux job does for its own
  archives. **Deliberately does not run `ctest`**: the existing suite
  (`tests/*.sh`) is POSIX-shell/symlink-based and would fail on
  assumptions this port doesn't share, not on anything actually broken --
  a Windows-native Pester suite is Phase 8, still not written, so the
  smoke test is this job's real correctness gate for now.

  **Now verified against real GitHub Actions runs -- green, after 5
  iterations, each one a real bug caught only by actually running it,**
  triggered via `workflow_dispatch` each time (never a tag push, so the
  real `v0.1.0` release stayed untouched throughout):
  1. `xwin`'s own splat step failed with "The system cannot move the file
     to a different disk drive" -- it moves files from its download cache
     into the splat output as a fast path, and the checkout/work
     directory on GitHub's windows runners lives on `D:\`, not `C:\` --
     the exact `EXDEV`-class problem this whole port spent so much time
     on elsewhere, this time hitting `xwin` itself. Fixed by splatting to
     `D:\msvc-sysroot` instead of `C:\`.
  2. Configure failed with "Could not find toolchain file:
     cmake/windows-clang-cl" (the trailing `.cmake` silently gone) -- the
     exact same pwsh command-line-argument-mangling bug hit during local
     toolchain setup earlier this session (see the very top of this
     document), now confirmed on the runner too, since `run:` steps there
     default to `pwsh`. Fixed by quoting
     `"-DCMAKE_TOOLCHAIN_FILE=cmake/windows-clang-cl.cmake"`.
  3. Configure then failed the toolchain file's own "expected exactly one
     MSVC version / one Windows Kit version" check, with 0 and 0 found. A
     temporary debug step (listing the actual splatted directory tree)
     showed why: the CI runner's `xwin` (installed fresh via
     `cargo install xwin --locked`, so whatever's currently published) is
     version 0.10.0, and its default splat layout has changed to a flat
     `<root>/crt/{include,lib/<arch>}` +
     `<root>/sdk/{include,lib}/{ucrt,um,shared}[/<arch>]` structure with
     no version directory at all -- not the
     `VC/Tools/MSVC/<ver>`/`Windows Kits/10/<ver>` layout this toolchain
     file was written and tested against (and this project's own local
     dev sysroot still uses). Fixed properly, not by picking one:
     `cmake/windows-clang-cl.cmake` now detects which layout is actually
     present (`if(EXISTS .../crt/include)`) and builds the right
     include/lib dir list either way -- confirmed this doesn't regress
     the local build with a full local reconfigure + rebuild + smoke test
     against the existing (old-layout) local sysroot before pushing.
  4. Green: `build-windows` passed every step (checkout through artifact
     upload) end to end.
  5. Downloaded the actual artifact `build-windows` produced on GitHub's
     own infrastructure (not built locally at all), extracted it, and ran
     it -- a genuine, independently-produced `shimback.exe` reporting
     `shimback 0.1.0` correctly.

  The runner label (`windows-2025`) and the LLVM path
  (`C:\Program Files\LLVM\bin`) both turned out to be correct as guessed
  -- worth noting since they were flagged as the most likely things to
  need adjusting, and in the end neither did.
- **README**: install one-liners (both forms), the Windows download row,
  a rewritten Platform support section (hard links vs. symlinks, the
  copy fallback, PowerShell/cmd.exe PATH integration, no man page, ARM64
  still deferred), a Building-from-source note pointing at the clang-cl/
  `xwin` toolchain instead of "any C11 compiler," and an `update` section
  rewrite covering `-y`/`--yes` and the real POSIX-vs-Windows staleness
  difference (a symlink never goes stale; a Windows shim can, and
  `update` now detects and offers to fix it).

- **Future consideration, not v1 scope**: winget distribution. A
  statically-linked (`/MT`) clang-cl/MSVC-ABI build is fully shippable via
  winget — arguably an easier sell than a MinGW build would have been,
  since it's binary-compatible with what a "native Windows build" is
  normally expected to look like. Its `portable` manifest type is also a
  good fit for shimback specifically: it drops the exe into
  `%LOCALAPPDATA%\Microsoft\WinGet\Links` (on PATH by default), which
  solves *shimback's own* discoverability as a nice complement to — not a
  replacement for — the shim-management PATH logic in Phase 5, which
  handles the commands shimback itself manages. Revisit once the `.zip`
  release artifact from this phase has shipped and stabilized.

### Phase 8 — test suite
- **Settled: a native Pester suite, built from day one** rather than
  leaning on the existing `tests/*.sh` under Git-Bash as a stopgap. Concretely:
  a `tests/windows/` directory of `.Tests.ps1` files, developed alongside
  each phase rather than bolted on at the end — Phase 2's dispatch/process-
  model work gets its Pester coverage as part of Phase 2, Phase 3's
  hard-link logic as part of Phase 3, and so on, mirroring how
  `tests/*.sh` already sits next to the POSIX implementation.
- Coverage should mirror the existing bash suite's intent one file at a
  time (`test_add_remove.sh` → an `Add-Remove.Tests.ps1` equivalent,
  `test_doctor.sh` → `Doctor.Tests.ps1`, etc.) so behavior parity between
  platforms is checkable test-by-test, plus dedicated Windows-only specs
  for the genuinely new surface: hard-link creation/detection
  (`GetFileInformationByHandle` file-index comparison from Phase 3),
  `HKCU\Environment` registry persistence and `WM_SETTINGCHANGE`
  broadcasting, and PowerShell profile block idempotency/migration
  (Phase 5).
- Wire a `windows-latest` Pester job into CI (`.github/workflows/`) in the
  same phase the first Windows-specific tests land, rather than waiting for
  Phase 7 — gives every subsequent phase a red/green signal instead of
  accumulating untested Windows code until packaging time.

## Suggested sequencing / milestones

1. **✅ Phase 0** (platform seam refactor) — done. Process-spawn subsystem,
   `paths.c` in full, `tui.c` all behind the seam; POSIX side verified
   (15/15 tests, identical timing to baseline).
2. **✅ Phase 1** (clang-cl/lld-link toolchain wiring) — done. A real
   `shimback.exe` builds and runs correctly for `--version`/`--help`/
   `list`/`doctor`/`info`; commands needing later phases fail with a clear
   message instead of crashing. Not yet done: the `windows-latest` Pester
   CI job itself (this phase proved the build locally; wiring it into
   `.github/workflows/` is still open) and curating a Windows-appropriate
   `-Wall -Wextra` set so Debug builds work there too.
3. **✅ Phase 2** (process model) — done. `CreateProcessW`/pipe-based
   capture verified end-to-end with real shims (invisible-trial-then-
   fallback, invisible-trial-then-replay, and timeout-triggered live-relay
   all confirmed working), zero changes needed to `dispatch.c` itself. Also
   found and fixed a real `getopt_long` permutation bug from Phase 1 that
   only surfaced once `add`'s argument parsing was actually exercised.
4. **✅ Phase 3** (shim links) — done. Real `CreateHardLinkW` shims,
   verified end-to-end (`add`/`list`/`doctor`/`doctor fix`/`remove`/
   `uninstall`, all against real hard links, not mocked). Found and fixed
   an `errno`-mapping gap, a real POSIX regression in the missing-vs-
   dangling detection logic (caught by the WSL suite, not by inspection),
   and several more `.exe`-suffix omissions beyond the two `add.c` already
   had. `update`-staleness fix deliberately deferred (not reachable until
   Phase 7 ships a Windows release artifact — see Phase 3's own note).
   Phase 4's remaining scope (`is_executable_file` PATHEXT resolution,
   `path_join` separator cosmetics) stayed untouched — small, and blocks
   nothing that's happened so far.
5. **✅ Phase 4** (paths.c) — done. Config/data default-location decision
   resolved and later revised: `.config`/`.local/share` under
   `%USERPROFILE%` (matching macOS/Linux) is the default, falling back to
   `%APPDATA%`/`%LOCALAPPDATA%` only for an already-existing install made
   under that (earlier, now-superseded) default; verified with a real
   add/list/doctor/remove lifecycle across four scenarios plus a WSL
   POSIX-regression re-run, both before and after the reordering. Found
   and fixed a real bug along the way: `XDG_CONFIG_HOME`/`XDG_DATA_HOME`
   silently rejected Windows-style path values. `path_join`'s separator
   cosmetics remain the one deliberately-deferred item, unchanged from
   Phase 3's note.
6. **✅ Phase 5** (shell/PATH integration) — done. PowerShell profile
   injection, cmd.exe AutoRun, and the `HKCU\Environment\Path` fallback
   layer all verified end-to-end against real registry/file state, not
   mocked. Found and fixed two real, undocumented cmd.exe AutoRun
   behaviors (only-the-first-line execution; `rem` swallowing everything
   after it regardless of `&`) that broke the originally-planned
   multi-line/`rem`-marker design, plus a `dynbuf`-NULL bug in
   `plat_win_autorun_set`'s removal path that made `uninstall --full`
   silently fail to clear AutoRun while reporting success. Its own Pester
   specs for registry persistence and profile-block idempotency are still
   owed (see Phase 8's standing note).
7. **✅ Phase 6** (TUI) — done. `tui.c` needed no changes at all, only the
   four `plat_tty_*`/`plat_stdin_byte_ready`/`plat_read_stdin_byte`
   primitives. Built on `ReadConsoleInputW`'s raw `INPUT_RECORD` stream
   (translated by hand into the same byte stream a POSIX terminal
   produces) rather than `ENABLE_VIRTUAL_TERMINAL_INPUT`, a deliberate
   deviation from the original plan — see Phase 6 for why. Also added
   `plat_enable_vt_output()` for ANSI color rendering more broadly (not
   just the wizard), called once from `main()`. Live keystroke navigation
   — the one thing no tool call in this session could verify, no real
   attached console being available — was confirmed working by the user
   running the real wizard end-to-end; that same run caught a real,
   separate bug (an unhelpful `EXDEV` error message at the final save
   step, now fixed and shared with `doctor fix` — see Phase 6's
   addendum).
8. Phase 7 (packaging: `install.ps1`, `.zip` release artifact, CI matrix,
   smoke test, README update) — flip the README's platform-support line
   once the release pipeline produces and smoke-tests a Windows artifact.
   ARM64 stays out of this milestone (see below) — x86_64 only.

**Note on Phase 8's "build Pester alongside each phase" plan**: not
followed in practice for Phases 2–3 — verification instead used direct
manual testing (PowerShell scripts exercising real commands, `fsutil
hardlink list`, hand-crafted `config.toml` fixtures) plus the existing WSL
POSIX suite for regression coverage, not `.Tests.ps1` files landing
alongside the code. Real coverage happened, just not in the form
originally planned; a `tests/windows/` Pester suite capturing these same
scenarios as actual, re-runnable tests is still owed, not yet written.

## Resolved decisions

- **Toolchain: clang-cl + lld-link**, targeting the MSVC ABI, statically
  linked (`/MT`, pinned unconditionally — this sysroot has no debug CRT
  libs), sysroot via `xwin`, `getopt`/`unistd.h`/a few `<sys/stat.h>`
  macros vendored under `src/platform/win32-compat/`. Reverses the earlier
  MinGW-w64 choice. **Fully wired and verified** — see Phase 1 above for
  the working CMake toolchain file, the exact build command, and every
  lesson learned getting there.
- **`cmd.exe`: no startup *file*, but a startup-hook registry entry is
  needed for the ordering guarantee** — `HKCU\Software\Microsoft\Command
  Processor\AutoRun` (Phase 5), merged idempotently alongside the
  `HKCU\Environment\Path` write. Fully functional either way; the AutoRun
  entry is what makes shimback win the ordering race on every new cmd.exe,
  not just be reachable.
- **ARM64: cross-compilation confirmed working** (hello-world built and
  verified via PE header inspection), but shipping it as a *release*
  artifact is still **deferred** to a follow-up after x86_64 ships and is
  validated in CI and real usage — this is a scope/testing-effort decision
  now, not a toolchain-capability one. x86_64 covers the large majority of
  Windows machines and runs fine under emulation on ARM64 in the meantime.
- **Tests: native Pester suite, built incrementally alongside each phase**
  (not bolted on at the end, not a Git-Bash reuse of the bash suite) — see
  Phase 8.
