#include "commands.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../config.h"
#include "../paths.h"
#include "../platform/platform.h"
#include "../util.h"
#include "../zip.h"
#include "version.h" /* generated into <build>/generated -- see CMakeLists.txt's
                       * target_include_directories, same as cli.c/install.c's own
                       * unqualified "version.h" include. */

#define OPT_OVERRIDE 1000

static const char *USAGE = "usage: shimback export [-o|--output <path>] [-y|--yes] [--override]\n";

#define DEFAULT_BACKUP_NAME_PATTERN "shimback_backup_<hostname>_<timestamp>"

/* Reads the whole file at `path` into a newly allocated, NUL-terminated
 * buffer; *out_len receives its length (not counting the NUL). Returns
 * false on any I/O failure, leaving *out untouched. */
static bool read_whole_file(const char *path, char **out, size_t *out_len) {
    FILE *f = plat_fopen(path, "rb");
    if (!f) {
        return false;
    }
    long size;
    return read_open_file(f, out, out_len, &size) == FILE_READ_OK;
}

/* True if `path` ends in a path separator -- the -o directory-vs-file
 * disambiguation below. */
static bool ends_with_sep(const char *path) {
    size_t len = strlen(path);
    return len > 0 && is_path_sep(path[len - 1]);
}

/* Splits `path` into its directory and filename components purely
 * lexically (no existence check, no symlink resolution). `path` itself
 * is not required to exist. */
static void split_dir_name(const char *path, char **out_dir, char **out_name) {
    *out_dir = dir_of(path);
    *out_name = xstrdup(path_basename(path));
}

static bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Interactively confirms creating `dir` -- same [y/N] pattern as
 * doctor.c's confirm_remove_orphan: `-y`/`--yes` skips the prompt, EOF
 * (non-interactive stdin) declines rather than hanging. */
static bool confirm_create_dir(const char *dir, bool auto_yes) {
    if (auto_yes) {
        return true;
    }
    /* Not info(): an interactive prompt waits for input on the same line,
     * so it can't end with info()'s own unconditional trailing newline. */
    bool colorize = stdout_is_color();
    printf("%sshimback:%s export: directory %s does not exist. Create it? [y/N] ",
           colorize ? ANSI_PREFIX : "", colorize ? ANSI_RESET : "", dir);
    return read_yes_no(NULL);
}

/* Resolves `-o`'s value (or, if NULL, the default backup location) to a
 * concrete target directory + filename, per the rule documented in the
 * export command's own plan: an existing directory or a trailing path
 * separator means "directory, place the default name inside"; anything
 * else is treated as the exact target filename. Only ever prompts (via
 * confirm_create_dir) when a directory actually needs creating under a
 * user-supplied -o -- the default location is silently mkdir_p'd, same
 * as shim_bin_dir()/the config dir already are elsewhere in this
 * codebase. Exits the process (via die()) if a needed directory can't be
 * created, or if the user declines creating one. */
static void resolve_target(const char *output_arg, const char *effective_dir,
                            const char *default_name, bool auto_yes, char **out_dir,
                            char **out_name) {
    bool needs_prompt = false;
    char *dir, *name;

    if (!output_arg) {
        dir = xstrdup(effective_dir);
        name = xstrdup(default_name);
    } else if (path_is_dir(output_arg)) {
        dir = xstrdup(output_arg);
        name = xstrdup(default_name);
    } else if (ends_with_sep(output_arg)) {
        dir = xstrdup(output_arg);
        name = xstrdup(default_name);
        needs_prompt = true;
    } else {
        /* Also correctly handles an already-existing non-directory path
         * (a file, or a dangling symlink): its parent must exist, so
         * path_is_dir(dir) is already true and needs_prompt comes out
         * false here, same as an explicit existence check would give --
         * no separate branch needed for that case. */
        split_dir_name(output_arg, &dir, &name);
        needs_prompt = !path_is_dir(dir);
    }

    if (needs_prompt) {
        if (!confirm_create_dir(dir, auto_yes)) {
            die("export: aborted -- %s was not created", dir);
        }
    }
    if (!mkdir_p(dir)) {
        die("export: cannot create directory %s: %s", dir, plat_strerror(errno));
    }

    *out_dir = dir;
    *out_name = name;
}

/* Inserts "_<n>" into `name` just before its last '.' (its extension),
 * or appends it at the end if `name` has none. */
static char *name_with_suffix(const char *name, int n) {
    const char *dot = strrchr(name, '.');
    char buf[600];
    if (!dot) {
        snprintf(buf, sizeof(buf), "%s_%d", name, n);
    } else {
        snprintf(buf, sizeof(buf), "%.*s_%d%s", (int)(dot - name), name, n, dot);
    }
    return xstrdup(buf);
}

/* Finds the first available (non-existing) path for `dir`/`name`,
 * trying "<name>", then "<name>_1", "<name>_2", ... before its
 * extension -- never overwrites silently. Only called when override
 * isn't in effect. */
static char *resolve_collision(const char *dir, const char *name) {
    char *path = path_join(dir, name);
    if (!path_exists(path)) {
        return path;
    }
    free(path);
    for (int n = 1;; n++) {
        char *candidate_name = name_with_suffix(name, n);
        char *candidate_path = path_join(dir, candidate_name);
        free(candidate_name);
        if (!path_exists(candidate_path)) {
            return candidate_path;
        }
        free(candidate_path);
    }
}

static char *sanitize_for_filename(const char *raw) {
    char *out = xstrdup(raw);
    for (char *p = out; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) {
            *p = '_';
        }
    }
    return out;
}

static char *make_timestamp(void) {
    time_t now = time(NULL);
    struct tm tmv = *plat_localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmv);
    return xstrdup(buf);
}

/* Substitutes every "<hostname>"/"<timestamp>" occurrence in `pattern`
 * with `hostname`/`timestamp`; anything else in `pattern` passes through
 * literally, including a pattern with neither placeholder at all (a
 * fixed name is legal -- resolve_collision above still keeps repeated
 * exports safe). */
static char *render_name_pattern(const char *pattern, const char *hostname,
                                  const char *timestamp) {
    DynBuf buf;
    dynbuf_init(&buf);
    for (const char *p = pattern; *p != '\0';) {
        if (strncmp(p, "<hostname>", 10) == 0) {
            dynbuf_append_str(&buf, hostname);
            p += 10;
        } else if (strncmp(p, "<timestamp>", 11) == 0) {
            dynbuf_append_str(&buf, timestamp);
            p += 11;
        } else {
            dynbuf_append_char(&buf, *p);
            p++;
        }
    }
    char *result = xstrdup(dynbuf_cstr(&buf));
    dynbuf_free(&buf);
    return result;
}

/* hostname is at most 255 bytes (plat_hostname's own buffer size) and
 * already sanitized to [A-Za-z0-9._-], so no quote/escape can reach the
 * TOML string; timestamp is always 15 bytes ("%Y%m%d-%H%M%S"). 512 is
 * comfortably bounded for both plus the fixed keys/version. */
static char *build_manifest(const char *hostname, const char *timestamp, size_t shim_count) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "format_version = 1\n"
             "shimback_version = \"" SHIMBACK_VERSION "\"\n"
             "hostname = \"%s\"\n"
             "exported_at = \"%s\"\n"
             "shim_count = %zu\n",
             hostname, timestamp, shim_count);
    return xstrdup(buf);
}

int cmd_export(int argc, char **argv) {
    bool auto_yes = false;
    bool cli_override = false;
    const char *output_arg = NULL;

    static struct option long_opts[] = {
        {"output", required_argument, 0, 'o'},
        {"yes", no_argument, 0, 'y'},
        {"override", no_argument, 0, OPT_OVERRIDE},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "o:y", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'o': output_arg = optarg; break;
            case 'y': auto_yes = true; break;
            case OPT_OVERRIDE: cli_override = true; break;
            default:
                fprintf(stderr, "%s", USAGE);
                return 1;
        }
    }
    if (optind < argc) {
        die("export: unexpected argument '%s'", argv[optind]);
    }

    char *cfg_path = config_file_path();
    Config cfg;
    char errbuf[256];
    ConfigStatus cst = config_load(cfg_path, &cfg, errbuf, sizeof(errbuf));
    if (cst != CONFIG_OK) {
        die("export: %s", errbuf);
    }

    bool config_exists = path_exists(cfg_path);

    size_t name_count = 0;
    char **names = collect_all_shim_names(&cfg, &name_count);

    ZipWriter zw;
    zip_writer_init(&zw);

    bool have_anything = config_exists;
    size_t exported_shim_count = 0;
    char *final_path = NULL; /* set below; NULL is a no-op for cleanup's free() if we
                               * goto cleanup before reaching that point */

    if (config_exists) {
        char *data;
        size_t len;
        if (!read_whole_file(cfg_path, &data, &len)) {
            die("export: failed to read %s: %s", cfg_path, plat_strerror(errno));
        }
        zip_writer_add_file(&zw, "config.toml", data, len);
        free(data);
    }

    for (size_t i = 0; i < name_count; i++) {
        ShimEntry *entry = NULL;
        char *split_path = NULL;
        /* Fully parses the split file (not just a path lookup) to decide
         * SHIM_SOURCE_SPLIT vs. SHIM_SOURCE_CONFIG below -- and dies on a
         * malformed one, the same way config_load dying on a malformed
         * config.toml already would (see config.h's own doc comment on
         * this function). That's deliberate here too: aborting the whole
         * export on a config a human would also need to go fix is the
         * right call for a backup tool, not a silent partial backup. */
        ShimSource src = resolve_shim_entry(&cfg, names[i], &entry, &split_path);

        if (src == SHIM_SOURCE_CONFIG) {
            exported_shim_count++;
        } else if (src == SHIM_SOURCE_SPLIT) {
            char *data;
            size_t len;
            if (read_whole_file(split_path, &data, &len)) {
                have_anything = true;
                exported_shim_count++;
                char *arcname = split_config_filename(names[i]);
                zip_writer_add_file(&zw, arcname, data, len);
                free(arcname);
                free(data);
            } else {
                warn("export: failed to read %s, skipping '%s': %s", split_path, names[i],
                     plat_strerror(errno));
            }
            shim_entry_free(entry);
            free(entry);
        }
        free(split_path);
    }

    if (!have_anything) {
        warn_colored(ANSI_YELLOW,
                     "export: nothing to export -- no config.toml and no shims configured");
        goto cleanup;
    }

    char hostname_buf[256];
    if (!plat_hostname(hostname_buf, sizeof(hostname_buf))) {
        snprintf(hostname_buf, sizeof(hostname_buf), "%s", "unknown-host");
    }
    char *hostname = sanitize_for_filename(hostname_buf);
    char *timestamp = make_timestamp();

    char *manifest = build_manifest(hostname, timestamp, exported_shim_count);
    zip_writer_add_file(&zw, "manifest.toml", manifest, strlen(manifest));
    free(manifest);

    zip_writer_finish(&zw);

    char *effective_dir = cfg.backup_dir ? xstrdup(cfg.backup_dir) : default_backup_dir();
    const char *pattern = cfg.backup_name_pattern ? cfg.backup_name_pattern
                                                   : DEFAULT_BACKUP_NAME_PATTERN;
    char *rendered_name = render_name_pattern(pattern, hostname, timestamp);
    free(hostname);
    free(timestamp);

    /* The ".sz" extension is never part of the pattern -- always appended
     * here, into an exactly-sized allocation rather than a fixed buffer. */
    size_t default_name_len = strlen(rendered_name) + strlen(".sz") + 1;
    char *default_name = xmalloc(default_name_len);
    snprintf(default_name, default_name_len, "%s.sz", rendered_name);
    free(rendered_name);

    char *target_dir, *target_name;
    resolve_target(output_arg, effective_dir, default_name, auto_yes, &target_dir, &target_name);
    free(effective_dir);
    free(default_name);

    bool effective_override = cli_override || cfg.backup_override;
    final_path = effective_override ? path_join(target_dir, target_name)
                                     : resolve_collision(target_dir, target_name);
    free(target_dir);
    free(target_name);

    bool ok = write_file_atomic(final_path, zw.buf.data, zw.buf.len, 0600);
    if (!ok) {
        die("export: failed to write %s: %s", final_path, plat_strerror(errno));
    }

    bool colorize = stdout_is_color();
    info("%sexported to %s%s (%zu shim%s, %s)", colorize ? ANSI_GREEN : "", final_path,
         colorize ? ANSI_RESET : "", exported_shim_count, exported_shim_count == 1 ? "" : "s",
         config_exists ? "config.toml included" : "no config.toml");

cleanup:
    zip_writer_free(&zw);
    free(final_path);
    str_array_free(names, name_count);
    config_free(&cfg);
    free(cfg_path);
    return 0;
}
