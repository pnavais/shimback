/* Note: this is a short-lived CLI command handler. Heap allocations here are
 * intentionally not freed before process exit -- the OS reclaims them, and
 * this is a standard, deliberate simplification for one-shot CLI tools. */
#include "commands.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../installation.h"
#include "../paths.h"
#include "../sha256.h"
#include "../util.h"

#define DEFAULT_RELEASE_URL "https://github.com/pnavais/shimback/releases/latest/download"

static const char *USAGE = "usage: shimback update [--check]\n";

/* Runs `argv` (argv[0] an absolute path) with inherited stdio and returns its
 * exit status, or -1 if it couldn't be run at all. */
static int run_child(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (xwaitpid(pid, &status) < 0 || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

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
    int fds[2];
    if (pipe(fds) != 0) {
        return NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
        }
        execl(binary, binary, "--version", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    char buf[128];
    ssize_t n = 0;
    size_t len = 0;
    while (len < sizeof(buf) - 1 && (n = read(fds[0], buf + len, sizeof(buf) - 1 - len)) > 0) {
        len += (size_t)n;
    }
    close(fds[0]);
    int status = 0;
    xwaitpid(pid, &status);
    buf[len] = '\0';
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    if (len == 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return NULL;
    }
    const char *prefix = "shimback ";
    return xstrdup(strncmp(buf, prefix, strlen(prefix)) == 0 ? buf + strlen(prefix) : buf);
}

/* Best-effort recursive delete of the scratch directory; never follows
 * symlinks (lstat), so nothing outside it can be touched. */
static void rmtree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                    continue;
                }
                char *child = path_join(path, e->d_name);
                rmtree(child);
                free(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* The release asset's platform name, mirroring install.sh's detect_os/
 * detect_arch (so the two never disagree about which tarball is "this
 * machine's"). Returns false for a platform shimback ships no binary for. */
static bool platform_asset(char *out, size_t out_size) {
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

int cmd_update(int argc, char **argv) {
    bool check_only = false;

    /* Progress goes to stdout and problems to stderr; line-buffering keeps
     * them in the order they happened even when both are piped together. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    static struct option long_opts[] = {
        {"check", no_argument, 0, 'c'},
        {0, 0, 0, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "c", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'c': check_only = true; break;
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
    snprintf(asset, sizeof(asset), "%s.tar.gz", platform);

    char *curl = path_search("curl", NULL, NULL);
    char *tar = path_search("tar", NULL, NULL);
    if (!curl) {
        die("update: 'curl' is required but not found on PATH");
    }
    if (!tar) {
        die("update: 'tar' is required but not found on PATH");
    }

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

    const char *tmp_root = getenv("TMPDIR");
    char tmpl[4096];
    snprintf(tmpl, sizeof(tmpl), "%s/shimback-update.XXXXXX",
             tmp_root && tmp_root[0] ? tmp_root : "/tmp");
    char *tmpdir = mkdtemp(tmpl);
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

    char *extract[] = {tar, "-xzf", asset_path, "-C", tmpdir, NULL};
    if (run_child(extract) != 0) {
        warn("update: failed to extract %s", asset);
        goto done;
    }
    char *pkg_dir = path_join(tmpdir, platform);
    char *new_binary = path_join(pkg_dir, "shimback");
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

    char *man_src = path_join(pkg_dir, "shimback.1");
    struct stat man_st;
    if (stat(man_src, &man_st) == 0 && S_ISREG(man_st.st_mode)) {
        char *man_dir = path_join(inst->prefix, "share/man/man1");
        char *man_dest = path_join(man_dir, "shimback.1");
        if (mkdir_p(man_dir) && copy_file(man_src, man_dest)) {
            printf("shimback: man page refreshed at %s\n", man_dest);
        } else {
            warn("update: failed to refresh the man page at %s", man_dest);
        }
    }

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
    print_status(ANSI_GREEN, summary);
    rc = 0;

done:
    rmtree(tmpdir);
    free_installations(found, found_count);
    return rc;
}
