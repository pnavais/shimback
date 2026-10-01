/* Note: this is a short-lived CLI command handler. Heap allocations here are
 * intentionally not freed before process exit -- the OS reclaims them, and
 * this is a standard, deliberate simplification for one-shot CLI tools. */
#include "commands.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../config.h"
#include "../installation.h"
#include "../paths.h"
#include "../platform/platform.h"
#include "../shell.h"
#include "../util.h"
#include "version.h"

#define MAN_PAGE_NAME "shimback.1"

static const char *USAGE =
    "usage: shimback install [--prefix <dir>] [--force] [--shell <shell>[,<shell>]... | --all]\n";

/* download_via_curl/install_man_page (and MAN_PAGE_NAME above) are only
 * ever used from their one call site below, itself compiled out on
 * Windows -- no man page ships there at all (see that call site's own
 * comment). Guarding the definitions too, not just the call, avoids
 * compiling genuinely dead code and the unused-function warning that
 * would otherwise come with it. */
#ifndef _WIN32

/* Downloads `url` to `dest` via `curl` (its resolved absolute path), atomically
 * via a temp-file-plus-rename in `dest`'s own directory. Returns false on any
 * failure (curl missing/erroring, network down, non-2xx response with -f). */
static bool download_via_curl(const char *curl, const char *url, const char *dest) {
    char *dest_dir = dir_of(dest);
    char tmpl[4160];
    snprintf(tmpl, sizeof(tmpl), "%s/.shimback-install-tmp.XXXXXX", dest_dir);
    free(dest_dir);

    /* mkdtemp() creates this with mode 0700, owned by us -- curl's own `-o`
     * just opens whatever path it's given (there's no way to hand it an
     * already-open fd instead), so without this, an attacker with write
     * access to dest's directory (plausible if --prefix points somewhere
     * shared) could unlink and replace a bare temp *file* with a symlink
     * in the gap between us creating it and curl reopening it, making curl
     * follow the symlink and overwrite whatever it points at. Putting the
     * temp file inside a directory only we can write into closes that
     * window outright rather than just narrowing it: nothing else can
     * touch the name curl is about to open, symlink or otherwise. */
    char *tmpdir = plat_mkdtemp(tmpl);
    if (!tmpdir) {
        return false;
    }

    char tmp_file[4224];
    snprintf(tmp_file, sizeof(tmp_file), "%s/shimback", tmpdir);

    char *argv[] = {(char *)curl, (char *)"-fsSL", (char *)url, (char *)"-o", tmp_file, NULL};
    bool ok = plat_run_inherited(curl, argv) == 0;
    if (ok && chmod(tmp_file, 0644) != 0) {
        /* Don't rename a file into place with whatever mode it happened
         * to get otherwise (mkdtemp()'s own directory is 0700, but that
         * says nothing about what curl -o created the file itself with)
         * -- treat a failed chmod() as a failed download instead. */
        warn("install: failed to set permissions on downloaded man page: %s", strerror(errno));
        ok = false;
    }
    if (ok && rename(tmp_file, dest) == 0) {
        if (rmdir(tmpdir) != 0) {
            warn("install: failed to remove temporary directory %s: %s", tmpdir,
                 strerror(errno));
        }
        return true;
    }
    unlink(tmp_file);
    if (rmdir(tmpdir) != 0) {
        warn("install: failed to remove temporary directory %s: %s", tmpdir, strerror(errno));
    }
    return false;
}

/* Installs the man page to <prefix>/share/man/man1/shimback.1: prefers a
 * copy bundled next to the running binary (how the release tarball ships
 * it), falling back to downloading it from the GitHub release matching the
 * running version if no local copy is found. Never fatal -- a missing man
 * page shouldn't fail `install`'s primary job of getting the binary in
 * place. */
static void install_man_page(const char *prefix, const char *self_exe) {
    char *man_dir = path_join(prefix, "share/man/man1");
    if (!mkdir_p(man_dir)) {
        warn("install: failed to create %s; skipping man page", man_dir);
        free(man_dir);
        return;
    }

    char *man_dest = path_join(man_dir, MAN_PAGE_NAME);
    char *self_dir = dir_of(self_exe);
    char *local_man = path_join(self_dir, MAN_PAGE_NAME);

    bool colorize = stdout_is_color();
    struct stat st;
    if (stat(local_man, &st) == 0 && S_ISREG(st.st_mode)) {
        if (copy_file(local_man, man_dest)) {
            info("man page installed to " COLOR_PATH_FMT, COLOR_PATH_ARGS(man_dest, colorize));
        } else {
            warn("install: failed to copy man page from %s to %s: %s", local_man, man_dest,
                 strerror(errno));
        }
    } else {
        char *curl = path_search("curl", NULL, NULL);
        if (!curl) {
            warn("install: no bundled man page found next to the binary, and 'curl' is not on "
                 "PATH -- skipping man page");
        } else {
            char url[256];
            snprintf(url, sizeof(url),
                     "https://github.com/pnavais/shimback/releases/download/v%s/%s",
                     SHIMBACK_VERSION, MAN_PAGE_NAME);
            if (download_via_curl(curl, url, man_dest)) {
                info("man page downloaded and installed to " COLOR_PATH_FMT,
                     COLOR_PATH_ARGS(man_dest, colorize));
            } else {
                warn("install: failed to download man page from %s -- skipping", url);
            }
        }
    }

    free(local_man);
    free(self_dir);
    free(man_dest);
    free(man_dir);
}

#endif /* _WIN32 */

/* Merges both the shim dir and this binary's own dir into the single
 * "shimback"-tagged PATH block for `kind` (shell_ensure_path unions its
 * directory into whatever's already there, so calling it twice combines
 * both without either clobbering the other). Also removes the old separate
 * "shimback-bin" block, a one-time migration for anyone who ran an earlier
 * version of `install` that kept the two directories in separate blocks.
 *
 * Only the second call is verbose: both calls touch the exact same
 * profile/AutoRun/registry target per shell, so printing a "PATH updated"
 * notice from each one would show two identical-looking lines per shell
 * for what's really a single, combined update -- confirmed confusing for
 * real (four "PATH updated" lines, two shells, from one `install` run).
 * By the time the second call runs, the block already contains both
 * directories, so its notice already reflects the final, complete state. */
static void ensure_shell_path(ShellKind kind, const char *shim_dir, const char *bin_dir,
                               MiseIntegrationMode mise_mode) {
    shell_ensure_path(kind, shim_dir, false, mise_mode);
    shell_ensure_path(kind, bin_dir, true, mise_mode);
    shell_remove_path_tagged(kind, "shimback-bin");
}

int cmd_install(int argc, char **argv) {
    const char *prefix_arg = NULL;
    const char *shell_arg = NULL;
    bool all_shells = false;
    bool force = false;

    static struct option long_opts[] = {
        {"prefix", required_argument, 0, 'p'},
        {"shell", required_argument, 0, 's'},
        {"all", no_argument, 0, 'a'},
        {"force", no_argument, 0, 'f'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:s:af", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p': prefix_arg = optarg; break;
            case 's': shell_arg = optarg; break;
            case 'a': all_shells = true; break;
            case 'f': force = true; break;
            default:
                fprintf(stderr, "%s", USAGE);
                return 1;
        }
    }
    if (optind < argc) {
        die("install: unexpected extra argument '%s'", argv[optind]);
    }
    if (shell_arg && all_shells) {
        die("install: --shell and --all are mutually exclusive");
    }

    /* Parse and validate --shell up front, before any filesystem writes --
     * a typo here shouldn't leave a half-finished install (binary copied,
     * then a die() on a bad shell name). */
    ShellKind selected_shells[8];
    size_t selected_count = 0;
    if (shell_arg) {
        char *copy = xstrdup(shell_arg);
        char *saveptr = NULL;
        char *tok = strtok_r(copy, ",", &saveptr);
        while (tok) {
            if (selected_count >= sizeof(selected_shells) / sizeof(selected_shells[0])) {
                die("install: too many --shell values");
            }
            if (!shell_kind_from_name(tok, &selected_shells[selected_count])) {
                die("install: unknown shell '%s' (expected zsh, bash, or fish)", tok);
            }
            selected_count++;
            tok = strtok_r(NULL, ",", &saveptr);
        }
        free(copy);
        if (selected_count == 0) {
            die("install: --shell requires at least one shell name");
        }
    }

    char *prefix;
    if (prefix_arg) {
        prefix = xstrdup(prefix_arg);
    } else {
        char *home = home_dir();
        prefix = path_join(home, ".local");
        free(home);
    }

    char *bin_dir = path_join(prefix, "bin");

    /* Only one installation at a time, decided before anything is written:
     * a second binary on another prefix would leave two copies and two
     * `bin` entries in the PATH block, with `uninstall`/`update` unable to
     * tell which is "the" one. An installation at this same prefix is fine
     * to *overwrite* -- but only on request (--force), since upgrading from
     * a release is `update`'s job and a plain re-run is almost always a
     * mistake. */
    Installation *found = NULL;
    size_t found_count = find_installations(&found);
    bool same_prefix_installed = false;
    size_t other_count = 0;
    for (size_t i = 0; i < found_count; i++) {
        if (same_dir_path(found[i].bin_dir, bin_dir)) {
            same_prefix_installed = true;
        } else {
            other_count++;
        }
    }
    if (other_count > 0) {
        for (size_t i = 0; i < found_count; i++) {
            if (!same_dir_path(found[i].bin_dir, bin_dir)) {
                warn_colored(ANSI_RED,
                             "shimback is already installed at %s -- only one installation is "
                             "allowed. Run `shimback uninstall` first to move it (--force only "
                             "overwrites an installation at the same --prefix).",
                             found[i].binary);
            }
        }
        return 1;
    }
    if (same_prefix_installed && !force) {
        char *existing_filename = shimback_exe_name();
        char *existing_dest = path_join(bin_dir, existing_filename);
        free(existing_filename);
        warn_colored(ANSI_YELLOW,
                     "shimback is already installed at %s -- nothing to do. "
                     "`shimback update` upgrades it to the latest release; `shimback install "
                     "--force` overwrites it with this binary.",
                     existing_dest);
        free(existing_dest);
        free_installations(found, found_count);
        return 0;
    }
    free_installations(found, found_count);

    if (!mkdir_p(bin_dir)) {
        die("install: failed to create %s", bin_dir);
    }

    char *self_exe = self_exe_path();
    char *dest_filename = shimback_exe_name();
    char *dest = path_join(bin_dir, dest_filename);
    free(dest_filename);

    /* Windows can't delete a running process's own image file, so
     * `uninstall` falls back to renaming it to "<dest>.old" instead (see
     * uninstall.c's orphan_running_binary) -- harmless, but it never
     * cleans itself up on its own, since by the time it's created the
     * process that would need to delete it is already the one that
     * couldn't. By the time a fresh `install` runs at the same --prefix,
     * that old process has necessarily exited (nothing else could still
     * be holding the file open under the same path), so this is always
     * safe to remove outright. Verified via looks_like_shimback_binary
     * first anyway, same ownership caution as the dest check just below --
     * a --prefix typo'd onto an unrelated directory shouldn't delete
     * someone else's "*.old" file on the strength of its name alone. A
     * no-op everywhere else (POSIX never creates this file in the first
     * place), so no #ifdef needed. */
    bool colorize = stdout_is_color();
    DynBuf dest_old_buf;
    dynbuf_init(&dest_old_buf);
    dynbuf_append_str(&dest_old_buf, dest);
    dynbuf_append_str(&dest_old_buf, ".old");
    const char *dest_old = dynbuf_cstr(&dest_old_buf);
    if (access(dest_old, F_OK) == 0 && looks_like_shimback_binary(dest_old)) {
        if (unlink(dest_old) == 0) {
            info("removed leftover " COLOR_PATH_FMT " from a previous uninstall",
                 COLOR_PATH_ARGS(dest_old, colorize));
        } else {
            warn("install: failed to remove leftover %s: %s", dest_old, strerror(errno));
        }
    }
    dynbuf_free(&dest_old_buf);

    /* Unlike uninstall, which always verifies ownership before deleting
     * anything, install used to overwrite whatever was already at `dest`
     * unconditionally -- a typo'd or shared --prefix could silently
     * replace an unrelated existing file that just happened to be named
     * "shimback" (see review.md). Refusing here when something else is
     * already there costs nothing for the normal cases: a brand-new
     * install has nothing at `dest` yet, and overwriting an existing
     * shimback binary (--force) still passes this check, since that
     * existing binary already embeds the marker. */
    if (access(dest, F_OK) == 0 && !looks_like_shimback_binary(dest)) {
        die("install: %s already exists and doesn't look like a shimback binary -- refusing to "
            "overwrite it (remove it manually first if this --prefix is correct)",
            dest);
    }

    /* By now this is either a fresh install or an explicit --force
     * overwrite of the installation at this very prefix (the developer's
     * "I just rebuilt it" path). Copying a binary over itself is pointless,
     * so when this *is* the installed copy there's nothing to place. */
    char *dest_canon = canonicalize(dest);
    bool running_installed_copy = dest_canon && strcmp(dest_canon, self_exe) == 0;
    free(dest_canon);
    if (running_installed_copy) {
        info(COLOR_PATH_FMT " is already the running copy -- leaving it in place",
             COLOR_PATH_ARGS(dest, colorize));
    } else {
        if (!copy_executable(self_exe, dest)) {
            die("install: failed to copy %s to %s: %s", self_exe, dest, strerror(errno));
        }
        info("installed to " COLOR_PATH_FMT, COLOR_PATH_ARGS(dest, colorize));
    }

#ifndef _WIN32
    /* No man page ships on Windows at all (see README's Platform support
     * section) -- without this guard, install_man_page()'s local-copy
     * check correctly finds nothing next to the binary, but then falls
     * through to a pointless curl download attempt: nowhere to view a man
     * page on Windows anyway (`--help`/`doctor` are the documented
     * fallback there), and the download can't ever succeed for a
     * not-yet-released version, so it only ever produced a confusing
     * "curl: (22) The requested URL returned error: 404" -- confirmed for
     * real. */
    install_man_page(prefix, self_exe);
#endif

    /* Ensures both PATH entries are set up even on a totally fresh install,
     * before any `add` has ever run: this binary's own location, and the
     * shim directory itself (normally add/init's job) -- redundant, and a
     * harmless no-op, if add/init already wrote it. Defaults to the current
     * shell only; --shell/--all broaden that, matching `init`'s "every
     * installed shell" semantics for --all. */
    char *shim_dir = shim_bin_dir();
    char *cfg_path_for_mise = config_file_path();
    Config cfg_for_mise;
    char mise_errbuf[256];
    /* A malformed config.toml shouldn't block `install` from wiring up
     * PATH at all -- it has nothing else to do with this unrelated
     * setting, so a load failure just falls back to auto-detection rather
     * than dying. */
    MiseIntegrationMode mise_mode = MISE_AUTO;
    if (config_load(cfg_path_for_mise, &cfg_for_mise, mise_errbuf, sizeof(mise_errbuf)) ==
        CONFIG_OK) {
        mise_mode = cfg_for_mise.mise_integration_set
                        ? (cfg_for_mise.mise_integration ? MISE_ON : MISE_OFF)
                        : MISE_AUTO;
    }
    free(cfg_path_for_mise);
    if (all_shells) {
        ShellKind kinds[3];
        size_t kind_count = shell_all_kinds(kinds);
        for (size_t i = 0; i < kind_count; i++) {
            if (shell_is_installed(kinds[i])) {
                ensure_shell_path(kinds[i], shim_dir, bin_dir, mise_mode);
            }
        }
    } else if (selected_count > 0) {
        for (size_t i = 0; i < selected_count; i++) {
            ensure_shell_path(selected_shells[i], shim_dir, bin_dir, mise_mode);
        }
    } else {
        ensure_shell_path(detect_current_shell(), shim_dir, bin_dir, mise_mode);
    }
    free(shim_dir);
    recommend("Restart your shell (or re-source its startup file) for the PATH change to take "
              "effect.");

    return 0;
}
