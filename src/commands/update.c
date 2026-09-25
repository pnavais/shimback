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
#ifndef _WIN32
#include <sys/utsname.h> /* no Windows equivalent -- see platform_asset() */
#endif
#include <unistd.h>

#include "../installation.h"
#include "../paths.h"
#include "../platform/platform.h"
#include "../sha256.h"
#include "../util.h"

#define DEFAULT_RELEASE_URL "https://github.com/pnavais/shimback/releases/latest/download"

static const char *USAGE = "usage: shimback update [--check] [-y|--yes]\n";

/* Runs `argv` (argv[0] an absolute path) with inherited stdio and returns its
 * exit status, or -1 if it couldn't be run at all. */
static int run_child(char *const argv[]) {
    return plat_run_inherited(argv[0], argv);
}

#ifdef _WIN32
/* Escapes `s` for use inside a PowerShell single-quoted string (doubling
 * each embedded ') into `out`, silently truncating if it doesn't fit --
 * both call sites below pass our own bounded, %TEMP%-derived paths, never
 * long enough to hit that in practice. Nothing else is special inside a
 * PowerShell single-quoted string, the same property this project's
 * POSIX-shell/PowerShell shim-block quoting already relies on (see
 * shell.c's append_sh_squoted/append_ps_squoted). */
static void ps_squote_into(char *out, size_t out_size, const char *s) {
    size_t o = 0;
    for (const char *p = s; *p != '\0' && o + 1 < out_size; p++) {
        if (*p == '\'') {
            if (o + 2 >= out_size) {
                break;
            }
            out[o++] = '\'';
            out[o++] = '\'';
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}
#endif

/* Fetches `url` to `dest` with curl. For the default GitHub location
 * (`https_only`), curl is restricted to HTTPS for both the request and any
 * redirect it's sent through, so nothing along the way can downgrade the
 * download to plain HTTP or another protocol. A caller-supplied
 * SHIMBACK_RELEASE_URL is left unrestricted (file://, http:// mirrors are the
 * point of it). Returns curl's exit status, or -1 if it couldn't be run. */
static int download(char *curl, const char *url, const char *dest, bool https_only) {
    char *argv[12];
    size_t n = 0;
    argv[n++] = curl;
    argv[n++] = "-fsSL";
    if (https_only) {
        argv[n++] = "--proto";
        argv[n++] = "=https";
        argv[n++] = "--proto-redir";
        argv[n++] = "=https";
    }
    argv[n++] = (char *)url;
    argv[n++] = "-o";
    argv[n++] = (char *)dest;
    argv[n] = NULL;
    return run_child(argv);
}

/* `<binary> --version`'s output, minus the "shimback " prefix and trailing
 * newline (e.g. "0.1.0"); NULL if it couldn't be read. Only ever run on a
 * binary that's already passed its checksum, or on the installed one. */
static char *binary_version(const char *binary) {
    char buf[128];
    char *argv[] = {(char *)binary, (char *)"--version", NULL};
    long len = plat_capture_stdout(binary, argv, buf, sizeof(buf));
    if (len < 0) {
        return NULL;
    }
    size_t ulen = (size_t)len;
    while (ulen > 0 && (buf[ulen - 1] == '\n' || buf[ulen - 1] == '\r')) {
        buf[--ulen] = '\0';
    }
    if (ulen == 0) {
        return NULL;
    }
    const char *prefix = "shimback ";
    return xstrdup(strncmp(buf, prefix, strlen(prefix)) == 0 ? buf + strlen(prefix) : buf);
}

/* Best-effort recursive delete of the scratch directory; never follows a
 * symlink into recursing on whatever it points at, so nothing outside it
 * can be touched -- unlinks the link itself instead. On Windows,
 * plat_path_is_symlink() can't detect this yet (see its own comment in
 * platform.h), so this specific defense is a known gap there for now; the
 * scratch directory is exclusively created and populated by this same
 * update flow, not attacker-influenced, which is why that gap is
 * acceptable to leave open rather than block Phase 1 on closing it. */
static void rmtree(const char *path) {
    if (plat_path_is_symlink(path)) {
        unlink(path);
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        char **entries = plat_list_dir(path);
        for (char **e = entries; e && *e; e++) {
            char *child = path_join(path, *e);
            rmtree(child);
            free(child);
        }
        plat_free_dir_entries(entries);
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* The release asset's platform name, mirroring install.sh's detect_os/
 * detect_arch (so the two never disagree about which tarball is "this
 * machine's"). Returns false for a platform shimback ships no binary for. */
static bool platform_asset(char *out, size_t out_size) {
#ifdef _WIN32
    /* uname()/struct utsname have no Windows equivalent at all -- but
     * Windows doesn't need one here: unlike macOS/Linux, this project
     * only ever builds/ships Windows x86_64 (ARM64 deferred, see
     * windows-port.md's "Resolved decisions"), so there's nothing to
     * detect. */
    snprintf(out, out_size, "shimback-windows-x86_64");
    return true;
#else
    struct utsname u;
    if (uname(&u) != 0) {
        return false;
    }
    const char *os = NULL;
    if (strcmp(u.sysname, "Darwin") == 0) {
        os = "macos";
    } else if (strcmp(u.sysname, "Linux") == 0) {
        os = "linux";
    }
    const char *arch = NULL;
    if (strcmp(u.machine, "x86_64") == 0 || strcmp(u.machine, "amd64") == 0) {
        arch = "x86_64";
    } else if (strcmp(u.machine, "arm64") == 0 || strcmp(u.machine, "aarch64") == 0) {
        arch = "arm64";
    }
    if (!os || !arch) {
        return false;
    }
    snprintf(out, out_size, "shimback-%s-%s", os, arch);
    return true;
#endif
}

/* Finds `asset`'s digest in a SHA256SUMS file ("<hex>  <name>" per line, the
 * name optionally prefixed with '*' for binary mode). Returns a heap string
 * or NULL. */
static char *expected_digest(const char *sums_path, const char *asset) {
    FILE *f = fopen(sums_path, "r");
    if (!f) {
        return NULL;
    }
    char line[512];
    char *found = NULL;
    while (!found && fgets(line, sizeof(line), f)) {
        char hex[128];
        char name[256];
        if (sscanf(line, "%127s %255s", hex, name) != 2) {
            continue;
        }
        const char *n = name[0] == '*' ? name + 1 : name;
        if (strcmp(n, asset) == 0) {
            found = xstrdup(hex);
        }
    }
    fclose(f);
    return found;
}

static void print_status(const char *color, const char *text) {
    bool c = stdout_is_color();
    printf("shimback: %s%s%s\n", c ? color : "", text, c ? ANSI_RESET : "");
}

/* Interactively confirms refreshing shims left pointing at the previous
 * binary after this update -- `auto_yes` (`shimback update -y`/`--yes`)
 * skips the prompt outright, for unattended/scripted invocations. On EOF
 * (non-interactive stdin), treats it as "no" rather than hanging, the
 * same convention doctor.c's confirm_remove_orphan already uses. */
static bool confirm_refresh_shims(bool auto_yes) {
    if (auto_yes) {
        return true;
    }
    printf("Refresh them now? [y/N] ");
    fflush(stdout);
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) {
        printf("\n");
        return false;
    }
    return line[0] == 'y' || line[0] == 'Y';
}

int cmd_update(int argc, char **argv) {
    bool check_only = false;
    bool auto_yes = false;

    /* Progress goes to stdout and problems to stderr; line-buffering keeps
     * them in the order they happened even when both are piped together.
     * Windows: MSVC's UCRT setvbuf() with _IOLBF crashes outright (a
     * stack-buffer-overrun /GS failure, confirmed directly -- never
     * caught before now since `update` always died earlier at
     * platform_asset() on Windows, before ever reaching this line).
     * _IONBF (fully unbuffered) gets the same ordering guarantee --
     * everything flushed immediately, not just line-by-line -- without
     * whatever _IOLBF-specific UCRT bug this is. */
#ifdef _WIN32
    setvbuf(stdout, NULL, _IONBF, 0);
#else
    setvbuf(stdout, NULL, _IOLBF, 0);
#endif

    static struct option long_opts[] = {
        {"check", no_argument, 0, 'c'},
        {"yes", no_argument, 0, 'y'},
        {0, 0, 0, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "cy", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'c': check_only = true; break;
            case 'y': auto_yes = true; break;
            default: fprintf(stderr, "%s", USAGE); return 1;
        }
    }
    if (optind < argc) {
        die("update: unexpected extra argument '%s'", argv[optind]);
    }

    Installation *found = NULL;
    size_t found_count = find_installations(&found);
    if (found_count == 0) {
        die("update: shimback isn't installed -- run `shimback install` first (`update` "
            "replaces the installed copy with the latest release)");
    }
    if (found_count > 1) {
        die("update: found %zu installations (%s and %s ...) -- only one is supported; run "
            "`shimback uninstall` to remove them, then `shimback install` once",
            found_count, found[0].binary, found[1].binary);
    }
    const Installation *inst = &found[0];

    char platform[64];
    if (!platform_asset(platform, sizeof(platform))) {
        die("update: no prebuilt shimback binary for this OS/architecture -- build from source "
            "instead");
    }
    char asset[96];
#ifdef _WIN32
    /* .zip, not .tar.gz: Windows users generally don't have tar/gzip on
     * PATH by default (modern Windows 10+ does ship tar.exe, but it's
     * GNU tar there -- no zip support -- not the zip-capable bsdtar this
     * project would otherwise need; see the PowerShell-based extraction
     * below), so .zip avoids relying on either. */
    snprintf(asset, sizeof(asset), "%s.zip", platform);
#else
    snprintf(asset, sizeof(asset), "%s.tar.gz", platform);
#endif

    char *curl = path_search("curl", NULL, NULL);
    if (!curl) {
        die("update: 'curl' is required but not found on PATH");
    }
#ifndef _WIN32
    char *tar = path_search("tar", NULL, NULL);
    if (!tar) {
        die("update: 'tar' is required but not found on PATH");
    }
#endif

    const char *base_url = getenv("SHIMBACK_RELEASE_URL");
    bool custom_release_url = base_url && base_url[0] != '\0';
    if (!custom_release_url) {
        base_url = DEFAULT_RELEASE_URL;
    } else {
        /* The archive's SHA256SUMS is fetched from this same place, so it can
         * only catch corruption or a mismatched pair -- it can't vouch for
         * the location itself (there's no release signature). The default
         * GitHub URL rests on HTTPS to github.com, exactly as install.sh's
         * does; an override is the caller's explicit choice to trust
         * somewhere else, and worth saying out loud. */
        warn_colored(ANSI_YELLOW,
                     "using SHIMBACK_RELEASE_URL=%s -- its checksums come from the same place "
                     "as the download, so they only guard against corruption, not a malicious "
                     "mirror; only use a location you trust",
                     base_url);
    }
    size_t base_len = strlen(base_url);
    while (base_len > 0 && base_url[base_len - 1] == '/') {
        base_len--;
    }
    char *asset_url = xmalloc(base_len + strlen(asset) + 2);
    snprintf(asset_url, base_len + strlen(asset) + 2, "%.*s/%s", (int)base_len, base_url, asset);
    char *sums_url = xmalloc(base_len + 16);
    snprintf(sums_url, base_len + 16, "%.*s/SHA256SUMS", (int)base_len, base_url);

#ifdef _WIN32
    /* No $TMPDIR/"/tmp" convention on Windows -- %TEMP%/%TMP% are the
     * usual per-user scratch dirs; falling back to "." if neither is set
     * is unusual but safe (matches this function's own working directory
     * assumptions no worse than /tmp would). */
    const char *tmp_root = getenv("TEMP");
    if (!tmp_root || !tmp_root[0]) {
        tmp_root = getenv("TMP");
    }
    if (!tmp_root || !tmp_root[0]) {
        tmp_root = ".";
    }
#else
    const char *tmp_root = getenv("TMPDIR");
    if (!tmp_root || !tmp_root[0]) {
        tmp_root = "/tmp";
    }
#endif
    char tmpl[4096];
    snprintf(tmpl, sizeof(tmpl), "%s/shimback-update.XXXXXX", tmp_root);
    char *tmpdir = plat_mkdtemp(tmpl);
    if (!tmpdir) {
        die("update: failed to create a temporary directory: %s", strerror(errno));
    }

    char *installed_version = binary_version(inst->binary);
    printf("shimback: installed %s (version %s)\n", inst->binary,
           installed_version ? installed_version : "unknown");

    char *asset_path = path_join(tmpdir, asset);
    char *sums_path = path_join(tmpdir, "SHA256SUMS");
    int rc = 1;

    printf("Downloading %s ...\n", asset);
    if (download(curl, asset_url, asset_path, !custom_release_url) != 0) {
        warn("update: failed to download %s", asset_url);
        goto done;
    }
    if (download(curl, sums_url, sums_path, !custom_release_url) != 0) {
        warn("update: failed to download %s -- refusing to update from an unverified "
             "download",
             sums_url);
        goto done;
    }

    char *expected = expected_digest(sums_path, asset);
    if (!expected) {
        warn("update: no checksum entry for %s in SHA256SUMS -- refusing to update from an "
             "unverified download",
             asset);
        goto done;
    }
    char actual[65];
    if (!sha256_file(asset_path, actual) || strcmp(actual, expected) != 0) {
        warn("update: checksum mismatch for %s (expected %s, got %s) -- refusing to install a "
             "possibly corrupted or tampered download",
             asset, expected, actual);
        goto done;
    }
    printf("Checksum verified.\n");

#ifdef _WIN32
    /* PowerShell's Expand-Archive, not tar: Windows 10+ does ship a
     * tar.exe, but it's GNU tar there (no zip support), and even the
     * zip-capable bsdtar at %SystemRoot%\System32\tar.exe can lose a
     * PATH race to Git for Windows' own GNU tar (confirmed directly --
     * `where tar` on a real dev machine listed Git's ahead of System32's)
     * -- so this doesn't try to pick "the right tar" off PATH at all.
     * Expand-Archive is always present (PowerShell 5.1+ ships with every
     * Windows 10/11 install) and handles .zip natively regardless. */
    char *powershell = path_search("powershell", NULL, NULL);
    if (!powershell) {
        warn("update: 'powershell' is required to extract the downloaded archive but wasn't "
             "found on PATH");
        goto done;
    }
    char esc_asset[4160];
    char esc_tmpdir[4160];
    ps_squote_into(esc_asset, sizeof(esc_asset), asset_path);
    ps_squote_into(esc_tmpdir, sizeof(esc_tmpdir), tmpdir);
    char ps_cmd[8448];
    snprintf(ps_cmd, sizeof(ps_cmd),
             "Expand-Archive -LiteralPath '%s' -DestinationPath '%s' -Force", esc_asset,
             esc_tmpdir);
    char *extract[] = {powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", ps_cmd,
                        NULL};
    if (run_child(extract) != 0) {
        warn("update: failed to extract %s", asset);
        goto done;
    }
#else
    char *extract[] = {tar, "-xzf", asset_path, "-C", tmpdir, NULL};
    if (run_child(extract) != 0) {
        warn("update: failed to extract %s", asset);
        goto done;
    }
#endif
    char *pkg_dir = path_join(tmpdir, platform);
    char *new_binary_filename = shimback_exe_name();
    char *new_binary = path_join(pkg_dir, new_binary_filename);
    free(new_binary_filename);
    if (!is_executable_file(new_binary) || !looks_like_shimback_binary(new_binary)) {
        warn("update: the downloaded archive didn't contain a shimback binary -- refusing to "
             "install it");
        goto done;
    }

    /* Compare contents, not version strings: a release can be re-tagged
     * under the same version, and only the bytes say whether the installed
     * copy actually differs. */
    char new_hash[65];
    char cur_hash[65];
    bool differs = !sha256_file(new_binary, new_hash) || !sha256_file(inst->binary, cur_hash) ||
                   strcmp(new_hash, cur_hash) != 0;
    if (!differs) {
        print_status(ANSI_GREEN, "already up to date");
        rc = 0;
        goto done;
    }
    if (check_only) {
        print_status(ANSI_YELLOW, "an update is available -- run `shimback update` to install it");
        rc = 0;
        goto done;
    }

    if (!copy_executable(new_binary, inst->binary)) {
        warn("update: failed to replace %s: %s", inst->binary, strerror(errno));
        goto done;
    }

    /* The man page is best-effort, as in `install`: the binary is what was
     * asked for and it's already in place, so a failed refresh doesn't fail
     * the update -- but the final line must not claim more than happened. */
    bool man_failed = false;
    char *man_src = path_join(pkg_dir, "shimback.1");
    struct stat man_st;
    if (stat(man_src, &man_st) == 0 && S_ISREG(man_st.st_mode)) {
        char *man_dir = path_join(inst->prefix, "share/man/man1");
        char *man_dest = path_join(man_dir, "shimback.1");
        if (mkdir_p(man_dir) && copy_file(man_src, man_dest)) {
            printf("shimback: man page refreshed at %s\n", man_dest);
        } else {
            warn("update: failed to refresh the man page at %s", man_dest);
            man_failed = true;
        }
    }

    /* Existing shims can go stale from this same replace: a Windows hard
     * link that shared data with the *old* inst->binary keeps pointing at
     * that old file once copy_executable()'s own atomic rename swaps a
     * new one into inst->binary's place, and a copy-fallback shim
     * (create_shim_link(), used when a hard link couldn't be made at all
     * -- different drives) never shared data with it in the first place
     * (see windows-port.md's Phase 3/6 notes). Comparing each shim's own
     * content hash against the binary just installed catches both cases
     * uniformly, and is naturally a no-op on POSIX, where shims are
     * symlinks that always resolve to whatever's at inst->binary *now* --
     * their content can never actually differ from it. */
    char *shim_dir = shim_bin_dir();
    size_t shim_count = 0;
    char **shim_names = list_shim_symlink_names(&shim_count);
    StrVec stale_names;
    strvec_init(&stale_names);
    for (size_t i = 0; i < shim_count; i++) {
        char *filename = shim_file_name(shim_names[i]);
        char *shim_path = path_join(shim_dir, filename);
        free(filename);
        char shim_hash[65];
        if (sha256_file(shim_path, shim_hash) && strcmp(shim_hash, new_hash) != 0) {
            strvec_push(&stale_names, xstrdup(shim_names[i]));
        }
        free(shim_path);
    }
    /* Not plat_free_dir_entries(): that expects a NULL-terminated array
     * (plat_list_dir()'s own convention). list_shim_symlink_names()'s
     * array has no such sentinel -- it's sized by *out_count alone (its
     * capacity can legitimately exceed that count, from the doubling
     * growth strategy building it), so scanning it for a NULL terminator
     * reads past the real entries into uninitialized memory and frees
     * garbage pointers. Confirmed the hard way: this was silent heap
     * corruption, not caught until a later, unrelated allocation. */
    for (size_t i = 0; i < shim_count; i++) {
        free(shim_names[i]);
    }
    free(shim_names);

    if (stale_names.count > 0) {
        printf("shimback: %zu shim(s) still point at the previous binary:", stale_names.count);
        for (size_t i = 0; i < stale_names.count; i++) {
            printf(" %s", stale_names.items[i]);
        }
        printf("\n");
        if (confirm_refresh_shims(auto_yes)) {
            int shim_lock_fd = shim_dir_lock_acquire(shim_dir);
            for (size_t i = 0; i < stale_names.count; i++) {
                char *filename = shim_file_name(stale_names.items[i]);
                char *shim_path = path_join(shim_dir, filename);
                free(filename);
                if (refresh_shim_link(inst->binary, shim_path, "update")) {
                    printf("shimback: refreshed shim '%s'\n", stale_names.items[i]);
                } else {
                    warn("update: failed to refresh shim '%s' at %s: %s", stale_names.items[i],
                         shim_path, strerror(errno));
                }
                free(shim_path);
            }
            shim_dir_lock_release(shim_lock_fd);
        } else {
            printf("shimback: leaving them as-is -- run `shimback update -y` (or `shimback "
                   "update` again) to refresh them later.\n");
        }
    }
    strvec_free(&stale_names);
    free(shim_dir);

    char *new_version = binary_version(inst->binary);
    char summary[512];
    if (installed_version && new_version && strcmp(installed_version, new_version) == 0) {
        /* A re-tagged release: same version string, different bytes. */
        snprintf(summary, sizeof(summary), "updated %s (still version %s, but the binary's "
                 "contents changed)", inst->binary, new_version);
    } else {
        snprintf(summary, sizeof(summary), "updated %s (%s -> %s)", inst->binary,
                 installed_version ? installed_version : "unknown",
                 new_version ? new_version : "unknown");
    }
    if (man_failed) {
        size_t len = strlen(summary);
        snprintf(summary + len, sizeof(summary) - len,
                 " -- but the man page could not be refreshed");
        print_status(ANSI_YELLOW, summary);
    } else {
        print_status(ANSI_GREEN, summary);
    }
    rc = 0;

done:
    rmtree(tmpdir);
    free_installations(found, found_count);
    return rc;
}
