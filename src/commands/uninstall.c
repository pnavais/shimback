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
#include <sys/wait.h>
#include <unistd.h>

#include "../config.h"
#include "../paths.h"
#include "../shell.h"
#include "../util.h"

/* The single tag every command shares (add/init/install all merge their
 * directory into the same block -- see shell.c). "shimback-bin" was a
 * separate tag install used before that merge existed; removing it too is
 * a harmless no-op once install has migrated someone off it, and a real
 * cleanup for anyone who upgrades straight to `uninstall --full` without
 * ever re-running install first. */
#define SHIM_DIR_TAG "shimback"
#define LEGACY_INSTALL_TAG "shimback-bin"

static const char *USAGE = "usage: shimback uninstall [--prefix <dir>] [--full]\n";

/* Runs `path --version` in a fork and checks its stdout starts with
 * "shimback " -- a lightweight, no-persistent-state way to confirm a file
 * is actually a shimback binary (any version/build of it, not just this
 * exact one) before either deleting it (bin_dest, below -- built from
 * user-controlled --prefix, which could point anywhere) or treating a
 * shim's symlink target as legitimately ours (remove_shim_symlinks,
 * below -- a shim can be created by a different shimback binary/build
 * than whichever one happens to be running `uninstall`, e.g. after an
 * upgrade; that's still ours, unlike a symlink to something else entirely
 * hand-placed in this directory). Fails closed: anything unexpected
 * (can't fork/exec, a non-zero exit, unrecognized output) is treated as
 * "not verified", never as "verified" by default. */
static bool looks_like_shimback_binary(const char *path) {
    if (!is_executable_file(path)) {
        return false;
    }
    int out_pipe[2];
    if (pipe(out_pipe) != 0) {
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return false;
    }
    if (pid == 0) {
        close(out_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(out_pipe[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execl(path, path, "--version", (char *)NULL);
        _exit(127);
    }
    close(out_pipe[1]);
    char buf[64];
    ssize_t n = read(out_pipe[0], buf, sizeof(buf) - 1);
    close(out_pipe[0]);
    int status;
    bool waited_ok = xwaitpid(pid, &status) >= 0;
    if (!waited_ok || n <= 0) {
        return false;
    }
    buf[n] = '\0';
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && strncmp(buf, "shimback ", 9) == 0;
}

/* Same idea as looks_like_shimback_binary, but for the man page: no need
 * to execute anything for this one -- shimback's own man page always
 * starts with a recognizable ".TH SHIMBACK" troff header (see
 * man/shimback.1), so a plain read is enough. */
static bool looks_like_shimback_man_page(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    char buf[32] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    (void)n;
    return strncmp(buf, ".TH SHIMBACK", 12) == 0;
}

/* Removes every symlink directly inside `shim_dir` that's actually ours --
 * dangling (its target no longer exists at all; by convention this
 * directory is exclusively shimback's, so a dangling entry here is always
 * a stale shim, not something else) or resolving to a real shimback
 * binary (see looks_like_shimback_binary -- deliberately not narrowed to
 * *this* running binary specifically, since a shim can predate an
 * upgrade/reinstall) -- and leaves anything else alone (a live symlink
 * resolving to something other than shimback, e.g. hand-placed by the
 * user or another tool in this directory despite the convention). Then
 * removes the directory itself, if left empty. Returns the number of
 * symlinks actually removed. If `removed_names` is non-NULL, each removed
 * symlink's own name (i.e. the shim name) is pushed onto it -- used by
 * `--full` to know which shims to also sweep split-config files for,
 * including ones that only ever existed via a split file with no
 * config.toml entry at all. */
static int remove_shim_symlinks(const char *shim_dir, StrVec *removed_names) {
    DIR *d = opendir(shim_dir);
    if (!d) {
        return 0;
    }
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char *entry_path = path_join(shim_dir, ent->d_name);
        struct stat lst;
        if (lstat(entry_path, &lst) == 0 && S_ISLNK(lst.st_mode)) {
            char *resolved = canonicalize(entry_path);
            bool dangling = resolved == NULL;
            bool ours = resolved && looks_like_shimback_binary(resolved);
            free(resolved);
            if (dangling || ours) {
                if (unlink(entry_path) == 0) {
                    count++;
                    if (removed_names) {
                        strvec_push(removed_names, xstrdup(ent->d_name));
                    }
                } else {
                    warn("uninstall: failed to remove %s: %s", entry_path, strerror(errno));
                }
            } else {
                warn("uninstall: leaving %s alone -- it doesn't resolve to the shimback binary",
                     entry_path);
            }
        }
        free(entry_path);
    }
    closedir(d);
    rmdir(shim_dir); /* best-effort: harmless failure if non-empty (e.g. a foreign symlink
                       * deliberately left alone above) or already gone */
    return count;
}

typedef bool (*FileVerifier)(const char *path);

/* Removes `path` if present -- and, when `verify` is given, only if it
 * actually looks like something shimback itself would have put there.
 * `--prefix` is user-controlled and can point anywhere, so `path` being
 * exactly where shimback expects its own binary/man page to live is not
 * by itself proof that's what's actually there (a typo'd --prefix, or one
 * shared with another project that happens to use the same file name,
 * would otherwise make this delete an unrelated file). `verify` is NULL
 * for config.toml, whose path is never --prefix-derived. */
static void remove_file_if_present(const char *path, const char *label, FileVerifier verify) {
    if (access(path, F_OK) != 0) {
        return;
    }
    if (verify && !verify(path)) {
        warn("uninstall: %s doesn't look like a shimback %s -- leaving it alone (check "
             "--prefix)",
             path, label);
        return;
    }
    if (unlink(path) == 0) {
        printf("shimback: removed %s (%s)\n", path, label);
    } else {
        warn("uninstall: failed to remove %s: %s", path, strerror(errno));
    }
}

static void remove_config(void) {
    char *cfg_path = config_file_path();
    remove_file_if_present(cfg_path, "config", NULL);
    char *cfg_dir = dir_of(cfg_path);
    rmdir(cfg_dir); /* best-effort */
    free(cfg_dir);
    free(cfg_path);
}

static void remove_path_blocks(void) {
    /* Deliberately unconditional -- NOT gated on shell_is_installed(). A
     * shell's rc file can have a stale shimback block in it regardless of
     * whether that shell's binary is currently findable on $PATH (it may
     * have been uninstalled since, or just not be on this particular
     * PATH), and shell_remove_path_tagged already treats "no such block"
     * as a harmless no-op -- so skipping a kind here only risks leaving a
     * real block behind, never saves useful work. */
    ShellKind kinds[] = {SHELL_ZSH, SHELL_BASH, SHELL_FISH};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        shell_remove_path_tagged(kinds[i], SHIM_DIR_TAG);
        shell_remove_path_tagged(kinds[i], LEGACY_INSTALL_TAG);
    }
}

int cmd_uninstall(int argc, char **argv) {
    const char *prefix_arg = NULL;
    bool full = false;

    static struct option long_opts[] = {
        {"prefix", required_argument, 0, 'p'},
        {"full", no_argument, 0, 'f'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:f", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p': prefix_arg = optarg; break;
            case 'f': full = true; break;
            default:
                fprintf(stderr, "%s", USAGE);
                return 1;
        }
    }
    if (optind < argc) {
        die("uninstall: unexpected extra argument '%s'", argv[optind]);
    }

    char *prefix;
    if (prefix_arg) {
        prefix = xstrdup(prefix_arg);
    } else {
        char *home = home_dir();
        prefix = path_join(home, ".local");
        free(home);
    }

    char *shim_dir = shim_bin_dir();
    StrVec removed_shim_names;
    strvec_init(&removed_shim_names);
    int removed = remove_shim_symlinks(shim_dir, &removed_shim_names);
    if (removed > 0) {
        printf("shimback: removed %d shim symlink(s) from %s\n", removed, shim_dir);
    }
    free(shim_dir);

    if (full) {
        /* Split-config files (see paths.h's split_config_all_paths) are
         * just another storage form of the same shim data config.toml
         * holds -- --full clearing "the config" needs to sweep them too,
         * for every shim name we know of: both ones with a config.toml
         * entry, and ones that only ever existed via a split file (whose
         * symlink -- and so name -- we still just saw above, even though
         * they'd have no entry in cfg at all). Read before remove_config()
         * deletes config.toml out from under it.
         *
         * This whole block must run before the installed-binary removal
         * below: split_config_all_paths locates one of its three
         * candidate paths via self_exe_path(), which canonicalizes the
         * currently running executable's own path -- and when this *is*
         * the installed copy (the common case, e.g. `~/.local/bin/shimback
         * uninstall --full`), that resolution starts failing the moment
         * that file is unlinked, since realpath() needs the directory
         * entry to still exist. Nothing below this point may depend on
         * self_exe_path() succeeding. */
        char *cfg_path_for_sweep = config_file_path();
        Config cfg_for_sweep;
        char sweep_errbuf[256];
        ConfigStatus sweep_cst =
            config_load(cfg_path_for_sweep, &cfg_for_sweep, sweep_errbuf, sizeof(sweep_errbuf));
        if (sweep_cst == CONFIG_OK) {
            for (size_t i = 0; i < removed_shim_names.count; i++) {
                remove_split_configs(removed_shim_names.items[i]);
            }
            for (size_t i = 0; i < cfg_for_sweep.count; i++) {
                remove_split_configs(cfg_for_sweep.shims[i].name);
            }
        } else {
            warn("uninstall: could not parse existing config to sweep split-config files: %s",
                 sweep_errbuf);
        }
        free(cfg_path_for_sweep);

        remove_config();
        remove_path_blocks();
        printf("shimback: --full also cleared the config file, any split <name>-config.toml "
               "files, and PATH blocks in shell startup files\n");
    }

    char *bin_dest = path_join(path_join(prefix, "bin"), "shimback");
    remove_file_if_present(bin_dest, "installed binary", looks_like_shimback_binary);
    free(bin_dest);

    char *man_dest = path_join(path_join(prefix, "share/man/man1"), "shimback.1");
    remove_file_if_present(man_dest, "man page", looks_like_shimback_man_page);
    free(man_dest);

    free(prefix);
    return 0;
}
