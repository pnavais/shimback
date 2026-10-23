/* Note: this is a short-lived CLI command handler. Heap allocations here are
 * intentionally not freed before process exit -- the OS reclaims them, and
 * this is a standard, deliberate simplification for one-shot CLI tools. */
#include "commands.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../config.h"
#include "../paths.h"
#include "../platform/platform.h"
#include "../util.h"
#include "../zip.h"
#include "version.h" /* generated into <build>/generated -- see export.c's own identical
                       * comment on this same include. */

#define OPT_REPLACE 1000
#define OPT_DRY_RUN 1001

static const char *USAGE = "usage: shimback import <archive> [-y|--yes] [--replace] [--dry-run]\n";

/* ---- Small local helpers, mirroring export.c's/add.c's own ---- */

static bool read_whole_file(const char *path, char **out, size_t *out_len) {
    FILE *f = plat_fopen(path, "rb");
    if (!f) {
        return false;
    }
    long size;
    return read_open_file(f, out, out_len, &size) == FILE_READ_OK;
}

/* Backs up `path` to `backup_path` if `path` currently exists, so a later
 * failure can restore exactly what was there before this command
 * overwrites it. Returns true and does nothing if `path` doesn't exist yet
 * -- restore_from_backup() below knows to treat "no backup" as "delete
 * whatever got created" in that case. Mirrors add.c's own identical
 * helper (private there, so re-implemented here rather than shared --
 * see this feature's own plan). */
static bool backup_file_if_exists(const char *path, const char *backup_path) {
    if (access(path, F_OK) != 0) {
        return true;
    }
    return copy_file(path, backup_path);
}

static void restore_from_backup(const char *path, const char *backup_path) {
    if (access(backup_path, F_OK) == 0) {
        if (!plat_rename_replace(backup_path, path)) {
            warn("import: failed to restore %s from backup: %s", display_path(path),
                 plat_strerror(errno));
        }
    } else if (unlink(path) != 0 && errno != ENOENT) {
        warn("import: failed to remove %s while rolling back: %s", display_path(path),
             plat_strerror(errno));
    }
}

static void discard_backup(const char *backup_path) {
    if (unlink(backup_path) != 0 && errno != ENOENT) {
        warn("import: failed to remove rollback backup %s: %s", display_path(backup_path),
             plat_strerror(errno));
    }
}

/* "<path>.rollback.<pid>" -- same shape add.c's own rollback backups use. */
static char *make_backup_path(const char *path) {
    size_t len = strlen(path) + 32;
    char *backup_path = xmalloc(len);
    snprintf(backup_path, len, "%s.rollback.%d", path, (int)getpid());
    return backup_path;
}

/* A fresh, empty scratch directory for extracting archive members into
 * before handing them to config_load()/config_load_split() (both
 * file-path-only APIs -- there's no from-buffer variant). Mirrors
 * update.c's own identical TMPDIR/TMP-aware construction. Dies on
 * failure -- there's nothing useful to do without one. */
static char *make_scratch_dir(void) {
#ifdef _WIN32
    const char *tmp_root = plat_getenv("TEMP");
    if (!tmp_root || !tmp_root[0]) {
        tmp_root = plat_getenv("TMP");
    }
    if (!tmp_root || !tmp_root[0]) {
        tmp_root = ".";
    }
#else
    const char *tmp_root = plat_getenv("TMPDIR");
    if (!tmp_root || !tmp_root[0]) {
        tmp_root = "/tmp";
    }
#endif
    char tmpl[4096];
    snprintf(tmpl, sizeof(tmpl), "%s/shimback-import.XXXXXX", tmp_root);
    char *tmpdir = plat_mkdtemp(tmpl);
    if (!tmpdir) {
        die("import: failed to create a temporary directory: %s", plat_strerror(errno));
    }
    return xstrdup(tmpdir); /* plat_mkdtemp overwrites `tmpl` (a local array) in place --
                              * copy out before it goes out of scope. */
}

/* True if `path` is both executable and not a cycle back to shimback
 * itself -- source/fallback/a route's command are always stored as
 * already-resolved, canonical absolute paths (see add.c), so a direct
 * comparison against `self_exe` is enough, the same way add.c's own
 * resolve_target_or_die() checks for a cycle at write time. */
static bool path_is_healthy(const char *path, const char *self_exe) {
    return path && is_executable_file(path) && strcmp(path, self_exe) != 0;
}

/* `name` and `shim_dir` are only needed for the auto-source case (no
 * `e->source` at all): dispatch resolves that fresh from $PATH every run,
 * skipping `shim_dir` and anything resolving to shimback itself (see
 * path_search, paths.h) -- a shim whose own command genuinely isn't on
 * $PATH outside the shim directory deserves the same "flag it" treatment
 * every other broken path gets, not a free pass just because it was never
 * frozen to a path at `add` time. */
static bool shim_entry_is_healthy(const char *name, const ShimEntry *e, const char *shim_dir,
                                   const char *self_exe) {
    if (e->source) {
        if (!path_is_healthy(e->source, self_exe)) {
            return false;
        }
    } else {
        char *resolved = path_search(name, shim_dir, self_exe);
        bool found = resolved != NULL;
        free(resolved);
        if (!found) {
            return false;
        }
    }
    if (e->fallback && !path_is_healthy(e->fallback, self_exe)) {
        return false;
    }
    for (size_t i = 0; i < e->route_count; i++) {
        if (!path_is_healthy(e->routes[i].command, self_exe)) {
            return false;
        }
    }
    return true;
}

/* ---- manifest.toml: a tiny, dedicated parser for its 5 fixed keys, not
 * config_load_split() -- that function has no slots for any of these
 * fields and would warn on every single one as an unknown key; the
 * manifest is small enough to parse directly out of the archive's own
 * in-memory buffer, with no temp file needed. Any key this doesn't
 * recognize is silently skipped, forward-compatible with a future
 * manifest field this build predates. ---- */

typedef struct {
    int format_version; /* -1 if missing or unparseable */
    char *shimback_version; /* owned, sanitized for display; NULL if missing */
    int shim_count; /* -1 if missing or unparseable */
} Manifest;

/* Strips a surrounding pair of '"' characters, if present (manifest.toml's
 * string fields are always plain double-quoted TOML strings, generated by
 * this project's own build_manifest() -- no escape handling is needed for
 * values that are never anything but a sanitized hostname or a version
 * string). */
static char *unquote(const char *value) {
    size_t len = strlen(value);
    if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
        return xstrndup(value + 1, len - 2);
    }
    return xstrdup(value);
}

/* Replaces any control character with '?' in place -- manifest content
 * comes from an archive that could, in principle, be hand-edited or
 * corrupt, and shimback_version ends up echoed back in an info() line
 * (see cmd_import), so it's sanitized before that ever happens. */
static void sanitize_in_place(char *s) {
    for (char *p = s; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) {
            *p = '?';
        }
    }
}

static void parse_manifest(const char *data, size_t len, Manifest *out) {
    out->format_version = -1;
    out->shimback_version = NULL;
    out->shim_count = -1;

    char *buf = xstrndup(data, len);
    char *cursor = buf;
    while (cursor != NULL && *cursor != '\0') {
        char *nl = strchr(cursor, '\n');
        char *line = cursor;
        cursor = nl ? nl + 1 : NULL;
        if (nl) {
            *nl = '\0';
        }

        while (*line == ' ' || *line == '\t' || *line == '\r') {
            line++;
        }
        size_t llen = strlen(line);
        while (llen > 0 &&
               (line[llen - 1] == ' ' || line[llen - 1] == '\t' || line[llen - 1] == '\r')) {
            line[--llen] = '\0';
        }
        if (*line == '\0') {
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) {
            continue; /* malformed line -- not fatal, same forward-compat spirit as below */
        }
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        size_t klen = strlen(key);
        while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t')) {
            key[--klen] = '\0';
        }
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        size_t vlen = strlen(value);
        while (vlen > 0 &&
               (value[vlen - 1] == ' ' || value[vlen - 1] == '\t' || value[vlen - 1] == '\r')) {
            value[--vlen] = '\0';
        }

        if (strcmp(key, "format_version") == 0) {
            parse_nonneg_int(value, &out->format_version);
        } else if (strcmp(key, "shim_count") == 0) {
            parse_nonneg_int(value, &out->shim_count);
        } else if (strcmp(key, "shimback_version") == 0) {
            free(out->shimback_version);
            out->shimback_version = unquote(value);
            sanitize_in_place(out->shimback_version);
        }
        /* hostname/exported_at: parsed by nothing here -- this command has
         * no use for either, unlike shimback_version/format_version. */
    }
    free(buf);
}

/* ---- The import plan: one candidate per shim the archive contributes ---- */

typedef enum {
    CLASS_NEW,      /* no local configuration anywhere for this name */
    CLASS_IDENTICAL, /* local configuration exists and matches byte-for-byte */
    CLASS_CONFLICT, /* local configuration exists and differs */
} CandidateClass;

typedef struct {
    char *name;             /* alias of entry.name -- not separately owned */
    bool archive_is_split;  /* true: from a "<name>-config.toml" member; false: from the
                              * archive's own config.toml [shims.<name>] section */
    ShimEntry entry;        /* owned; the archive's own parsed entry */
    bool healthy;            /* computed once, from the archive's own content alone --
                              * source/fallback/every route command actually resolve here */
    CandidateClass klass;
    bool accepted;           /* final decision: will phase 2 actually write this */
    bool had_local;          /* Phase-1 snapshot: true if klass != CLASS_NEW */
    ShimEntry local_snapshot; /* only meaningful if had_local -- the locally-resolved entry
                               * at Phase-1 classification time, re-checked against a fresh
                               * load in Phase 2 before anything is written (see cmd_import) */
    bool write_failed;       /* Phase 2: this shim's own split-file write failed */
    char *split_target_path; /* Phase 2: only set once its split file write succeeds --
                               * needed to roll it back if the later config.toml save fails */
    char *split_backup_path;
    bool symlink_failed;     /* Phase 2: ensure_shim_link() itself failed */
    bool symlink_occupied;   /* Phase 2: its symlink path is occupied by a non-shimback file */
} ImportCandidate;

static ImportCandidate *push_candidate(ImportCandidate **cands, size_t *count, size_t *cap) {
    if (*count == *cap) {
        *cap = *cap == 0 ? 8 : *cap * 2;
        *cands = xrealloc(*cands, *cap * sizeof(ImportCandidate));
    }
    ImportCandidate *c = &(*cands)[(*count)++];
    memset(c, 0, sizeof(*c));
    return c;
}

static bool candidates_has_name(ImportCandidate *cands, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(cands[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Strips a "-config.toml" suffix from `arcname`, returning the shim name
 * it implies (newly allocated), or NULL if `arcname` doesn't end in that
 * suffix at all -- mirrors split_config_filename()'s own format, from the
 * other direction. */
static char *name_from_split_arcname(const char *arcname) {
    static const char *suffix = "-config.toml";
    size_t slen = strlen(suffix);
    size_t alen = strlen(arcname);
    if (alen <= slen || strcmp(arcname + alen - slen, suffix) != 0) {
        return NULL;
    }
    return xstrndup(arcname, alen - slen);
}

int cmd_import(int argc, char **argv) {
    bool auto_yes = false;
    bool replace = false;
    bool dry_run = false;

    static struct option long_opts[] = {
        {"yes", no_argument, 0, 'y'},
        {"replace", no_argument, 0, OPT_REPLACE},
        {"dry-run", no_argument, 0, OPT_DRY_RUN},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "y", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'y': auto_yes = true; break;
            case OPT_REPLACE: replace = true; break;
            case OPT_DRY_RUN: dry_run = true; break;
            default:
                fprintf(stderr, "%s", USAGE);
                return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "%s", USAGE);
        die("import: missing archive path");
    }
    const char *archive_path = argv[optind++];
    if (optind < argc) {
        die("import: unexpected extra argument '%s'", argv[optind]);
    }

    bool colorize = stdout_is_color();

    /* ---- Phase 0: read, open, and sanity-check the archive ---- */

    char *data;
    size_t data_len;
    if (!read_whole_file(archive_path, &data, &data_len)) {
        die("import: failed to read %s: %s", display_path(archive_path), plat_strerror(errno));
    }

    ZipReader zr;
    char zerrbuf[256];
    if (!zip_reader_open(data, data_len, &zr, zerrbuf, sizeof(zerrbuf))) {
        die("import: %s does not look like a valid zip archive: %s", display_path(archive_path),
            zerrbuf);
    }

    const ZipReaderEntry *manifest_entry = zip_reader_find(&zr, "manifest.toml");
    if (!manifest_entry) {
        die("import: %s has no manifest.toml -- this doesn't look like a shimback export (or "
            "it's corrupt)",
            display_path(archive_path));
    }
    Manifest manifest;
    parse_manifest(manifest_entry->data, manifest_entry->size, &manifest);
    if (manifest.format_version != 1) {
        if (manifest.format_version < 0) {
            die("import: %s's manifest.toml has no readable format_version",
                display_path(archive_path));
        }
        die("import: %s was exported in a format this shimback build doesn't understand "
            "(format_version %d, expected 1) -- try a newer shimback",
            display_path(archive_path), manifest.format_version);
    }
    if (manifest.shimback_version && strcmp(manifest.shimback_version, SHIMBACK_VERSION) != 0) {
        bool colorize = stdout_is_color();
        info("note: " COLOR_PATH_FMT " was exported by shimback %s (this build is %s)",
             COLOR_PATH_ARGS(archive_path, colorize), manifest.shimback_version, SHIMBACK_VERSION);
    }

    char *self_exe = self_exe_path();
    char *shim_dir = shim_bin_dir();
    char *scratch_dir = make_scratch_dir();

    /* ---- Phase 1: extract, classify, decide -- no lock, no writes yet ----
     *
     * One scratch directory for the whole extraction phase (not a fresh
     * mkdtemp() per member): each member is written under its own arcname,
     * loaded, and unlinked again immediately, so at most one real file is
     * ever in it at a time -- `scratch_dir` itself is rmdir()'d once,
     * right after the loop below, rather than being left behind empty
     * after every single import/dry-run the way re-creating it per member
     * would. A die() reached mid-extraction (a malformed member) still
     * leaves the now-otherwise-empty directory behind -- a rare-case,
     * cosmetic gap (an empty dir under /tmp), not a correctness one. */

    ImportCandidate *cands = NULL;
    size_t cand_count = 0, cand_cap = 0;

    const ZipReaderEntry *cfg_entry = zip_reader_find(&zr, "config.toml");
    Config archive_cfg;
    config_init(&archive_cfg);
    if (cfg_entry) {
        char *cfg_tmp = path_join(scratch_dir, "config.toml");
        if (!write_file_atomic(cfg_tmp, cfg_entry->data, cfg_entry->size, 0600)) {
            die("import: failed to extract config.toml to a scratch file: %s",
                plat_strerror(errno));
        }
        char errbuf[256];
        ConfigStatus cst = config_load(cfg_tmp, &archive_cfg, errbuf, sizeof(errbuf));
        unlink(cfg_tmp);
        free(cfg_tmp);
        if (cst != CONFIG_OK) {
            die("import: %s's config.toml is invalid: %s", display_path(archive_path), errbuf);
        }
    }

    /* Split members first, so the archive's own split-wins-over-config.toml
     * rule (mirroring resolve_shim_entry()) can be applied when the
     * config.toml-derived entries are considered next. */
    for (size_t i = 0; i < zr.count; i++) {
        const char *arcname = zr.entries[i].name;
        if (strcmp(arcname, "config.toml") == 0 || strcmp(arcname, "manifest.toml") == 0) {
            continue;
        }
        char *name = name_from_split_arcname(arcname);
        if (!name) {
            warn("import: archive member '%s' isn't recognized, ignoring", arcname);
            continue;
        }
        if (!is_valid_shim_name(name)) {
            warn("import: archive member '%s' implies an invalid shim name, ignoring", arcname);
            free(name);
            continue;
        }

        char *split_tmp = path_join(scratch_dir, arcname);
        if (!write_file_atomic(split_tmp, zr.entries[i].data, zr.entries[i].size, 0600)) {
            die("import: failed to extract %s to a scratch file: %s", arcname,
                plat_strerror(errno));
        }

        ShimEntry entry;
        memset(&entry, 0, sizeof(entry));
        entry.name = xstrdup(name);
        entry.policy = POLICY_EXIT_CODE;
        char errbuf[256];
        ConfigStatus cst = config_load_split(split_tmp, &entry, errbuf, sizeof(errbuf));
        unlink(split_tmp);
        free(split_tmp);
        if (cst != CONFIG_OK) {
            die("import: %s's %s is invalid: %s", display_path(archive_path), arcname, errbuf);
        }

        ImportCandidate *c = push_candidate(&cands, &cand_count, &cand_cap);
        c->entry = entry;
        c->name = c->entry.name;
        c->archive_is_split = true;
        c->healthy = shim_entry_is_healthy(c->name, &c->entry, shim_dir, self_exe);
        free(name);
    }
    rmdir(scratch_dir);
    free(scratch_dir);

    for (size_t i = 0; i < archive_cfg.count; i++) {
        if (candidates_has_name(cands, cand_count, archive_cfg.shims[i].name)) {
            /* The split member for this name already won; the archive's own
             * config.toml copy is discarded, same as resolve_shim_entry()
             * would do locally. */
            continue;
        }
        ImportCandidate *c = push_candidate(&cands, &cand_count, &cand_cap);
        c->entry = archive_cfg.shims[i]; /* struct copy -- takes over its owned pointers;
                                           * archive_cfg itself is never freed (see this
                                           * file's own top-of-file leak comment) */
        c->name = c->entry.name;
        c->archive_is_split = false;
        c->healthy = shim_entry_is_healthy(c->name, &c->entry, shim_dir, self_exe);
    }

    if (manifest.shim_count >= 0 && (size_t)manifest.shim_count != cand_count) {
        warn("import: manifest.toml claims %d shim(s), but %zu were actually found in the "
             "archive -- continuing anyway",
             manifest.shim_count, cand_count);
    }

    char *local_cfg_path = config_file_path();
    Config local_snapshot_cfg;
    char errbuf[256];
    if (config_load(local_cfg_path, &local_snapshot_cfg, errbuf, sizeof(errbuf)) != CONFIG_OK) {
        die("import: existing config at %s is invalid: %s", display_path(local_cfg_path), errbuf);
    }

    for (size_t i = 0; i < cand_count; i++) {
        ImportCandidate *c = &cands[i];
        ShimEntry *local_entry = NULL;
        char *local_split_path = NULL;
        ShimSource src =
            resolve_shim_entry(&local_snapshot_cfg, c->name, &local_entry, &local_split_path);
        if (src == SHIM_SOURCE_ORPHAN) {
            c->klass = CLASS_NEW;
            c->had_local = false;
            c->accepted = true;
        } else {
            c->had_local = true;
            c->local_snapshot = *local_entry; /* struct copy -- local_snapshot_cfg/the
                                                * heap-allocated split entry are never freed,
                                                * so these borrowed pointers stay valid */
            if (shim_entry_content_equal(local_entry, &c->entry)) {
                c->klass = CLASS_IDENTICAL;
                c->accepted = true;
            } else {
                c->klass = CLASS_CONFLICT;
                c->accepted = false; /* resolved below */
            }
        }
    }

    size_t removal_count = 0, removal_cap = 0;
    char **removal_names = NULL;
    if (replace) {
        if (cand_count == 0) {
            die("import --replace: %s has no shims to import -- refusing to remove every local "
                "shim",
                display_path(archive_path));
        }
        size_t local_name_count = 0;
        char **local_names = collect_all_shim_names(&local_snapshot_cfg, &local_name_count);
        for (size_t i = 0; i < local_name_count; i++) {
            if (candidates_has_name(cands, cand_count, local_names[i])) {
                continue;
            }
            ShimEntry *e = NULL;
            char *sp = NULL;
            if (resolve_shim_entry(&local_snapshot_cfg, local_names[i], &e, &sp) ==
                SHIM_SOURCE_ORPHAN) {
                continue; /* never remove a bare orphan symlink -- doctor's job, not import's */
            }
            if (removal_count == removal_cap) {
                removal_cap = removal_cap == 0 ? 8 : removal_cap * 2;
                removal_names = xrealloc(removal_names, removal_cap * sizeof(char *));
            }
            removal_names[removal_count++] = local_names[i];
        }
    }

    if (dry_run) {
        for (size_t i = 0; i < cand_count; i++) {
            ImportCandidate *c = &cands[i];
            const char *verb = c->klass == CLASS_NEW      ? "would import"
                                : c->klass == CLASS_IDENTICAL ? "already up to date"
                                                            : "would ask (conflicts locally)";
            info("  %s: %s%s", c->name, verb, c->healthy ? "" : " [source/fallback path broken]");
        }
        if (replace && removal_count > 0) {
            info("--replace would remove %zu local-only shim(s):", removal_count);
            for (size_t i = 0; i < removal_count; i++) {
                info("  %s", removal_names[i]);
            }
        }
        info("--dry-run: nothing written");
        return 0;
    }

    for (size_t i = 0; i < cand_count; i++) {
        ImportCandidate *c = &cands[i];
        if (c->klass != CLASS_CONFLICT) {
            continue;
        }
        if (auto_yes) {
            c->accepted = true;
            continue;
        }
        printf("  '%s' is already configured locally with different settings. Overwrite with "
               "the archived version? [y/N] ",
               c->name);
        bool eof;
        c->accepted = read_yes_no(&eof);
        if (eof) {
            printf("  Leaving '%s' as-is.\n", c->name);
        }
    }

    bool replace_confirmed = false;
    if (replace && removal_count > 0) {
        if (auto_yes) {
            replace_confirmed = true;
        } else {
            printf("  --replace would remove %zu local-only shim(s) not present in the "
                   "archive:\n",
                   removal_count);
            for (size_t i = 0; i < removal_count; i++) {
                printf("    %s\n", removal_names[i]);
            }
            printf("  Remove them? [y/N] ");
            bool eof;
            replace_confirmed = read_yes_no(&eof);
        }
    }

    /* ---- Phase 2: commit -- lock held throughout ---- */

    if (!mkdir_p(shim_dir)) {
        die("import: failed to create shim directory %s", display_path(shim_dir));
    }
    int shim_lock_fd = shim_dir_lock_acquire(shim_dir);
    if (shim_lock_fd < 0) {
        die("import: failed to acquire the shim directory lock on %s: %s", display_path(shim_dir),
            plat_strerror(errno));
    }

    Config local_cfg;
    if (config_load(local_cfg_path, &local_cfg, errbuf, sizeof(errbuf)) != CONFIG_OK) {
        die("import: existing config at %s is invalid: %s", display_path(local_cfg_path), errbuf);
    }

    /* Re-check every accepted decision against this fresh, locked load --
     * closes the narrow window between Phase 1's unlocked snapshot and
     * this lock (same accepted-risk class add.c/doctor.c already carry
     * elsewhere for a single shim; this is its N-shim equivalent). Abort
     * the whole import rather than attempt a partial reconciliation. */
    for (size_t i = 0; i < cand_count; i++) {
        ImportCandidate *c = &cands[i];
        if (c->klass == CLASS_CONFLICT && !c->accepted) {
            continue; /* declined -- this import never touches it either way */
        }
        /* IDENTICAL candidates are rechecked too, not skipped: Phase 2
         * still calls ensure_shim_link() for them (see below), and if a
         * concurrent `remove` fully deleted this shim (config and
         * symlink) in the window since Phase 1's snapshot, blindly
         * recreating just its symlink here would reintroduce exactly the
         * orphan-symlink problem remove.c's own ordering exists to avoid
         * -- fresh_had_local != c->had_local already catches that case. */
        ShimEntry *fresh_entry = NULL;
        char *fresh_split_path = NULL;
        ShimSource fresh_src = resolve_shim_entry(&local_cfg, c->name, &fresh_entry, &fresh_split_path);
        bool fresh_had_local = fresh_src != SHIM_SOURCE_ORPHAN;
        bool mismatch = fresh_had_local != c->had_local;
        if (!mismatch && fresh_had_local) {
            mismatch = !shim_entry_content_equal(fresh_entry, &c->local_snapshot);
        }
        if (mismatch) {
            die("import: local config changed since this import's plan was built (e.g. '%s') -- "
                "re-run `shimback import` to build a fresh plan",
                c->name);
        }
    }
    if (replace_confirmed) {
        for (size_t i = 0; i < removal_count; i++) {
            ShimEntry *fresh_entry = NULL;
            char *fresh_split_path = NULL;
            if (resolve_shim_entry(&local_cfg, removal_names[i], &fresh_entry, &fresh_split_path) ==
                SHIM_SOURCE_ORPHAN) {
                die("import --replace: local config changed since this import's plan was built "
                    "('%s' is no longer configured) -- re-run `shimback import` to build a fresh "
                    "plan",
                    removal_names[i]);
            }
        }
    }

    /* Split files are written FIRST, before config.toml is touched at all
     * -- mirrors add.c's own --split-config ordering (split write, THEN
     * remove the config.toml entry and save), for the same reason: a
     * candidate transitioning from config.toml-backed locally to
     * split-backed in the archive must never have its config.toml entry
     * removed before its replacement split file is durably on disk. Each
     * candidate's own write is still independent of every other's (one
     * failing doesn't block another from succeeding) -- only the ordering
     * relative to config.toml changes here, not the per-candidate
     * independence itself. A write that fails here is excluded from every
     * config.toml change below, leaving that candidate's prior local state
     * (whatever it was) completely untouched. */
    for (size_t i = 0; i < cand_count; i++) {
        ImportCandidate *c = &cands[i];
        if (!c->archive_is_split || c->klass == CLASS_IDENTICAL ||
            (c->klass == CLASS_CONFLICT && !c->accepted)) {
            continue;
        }
        char *target_path = resolve_split_config_path(c->name);
        if (!target_path) {
            char *cfg_dir = dir_of(local_cfg_path);
            char *fname = split_config_filename(c->name);
            target_path = path_join(cfg_dir, fname);
            free(cfg_dir);
            free(fname);
        }
        char *split_backup_path = make_backup_path(target_path);
        if (!backup_file_if_exists(target_path, split_backup_path)) {
            warn("import: failed to back up existing split config %s -- '%s' not imported",
                 display_path(target_path), c->name);
            c->write_failed = true;
            free(target_path);
            free(split_backup_path);
            continue;
        }
        ConfigStatus sst = config_save_split(&c->entry, target_path, errbuf, sizeof(errbuf));
        if (sst != CONFIG_OK) {
            restore_from_backup(target_path, split_backup_path);
            warn("import: failed to save split config for '%s': %s", c->name, errbuf);
            c->write_failed = true;
            free(target_path);
            free(split_backup_path);
            continue;
        }
        /* Not discarded yet -- kept in case config.toml's own save below
         * fails and this write needs rolling back too. */
        c->split_target_path = target_path;
        c->split_backup_path = split_backup_path;
    }

    char *cfg_backup_path = make_backup_path(local_cfg_path);
    if (!backup_file_if_exists(local_cfg_path, cfg_backup_path)) {
        for (size_t i = 0; i < cand_count; i++) {
            if (cands[i].split_target_path) {
                restore_from_backup(cands[i].split_target_path, cands[i].split_backup_path);
            }
        }
        die("import: failed to back up existing config %s before updating it",
            display_path(local_cfg_path));
    }

    for (size_t i = 0; i < cand_count; i++) {
        ImportCandidate *c = &cands[i];
        if (c->klass == CLASS_IDENTICAL || (c->klass == CLASS_CONFLICT && !c->accepted) ||
            c->write_failed) {
            continue;
        }
        if (c->archive_is_split) {
            if (c->split_target_path) { /* its own write above actually succeeded */
                config_remove(&local_cfg, c->name);
            }
        } else {
            size_t idx = config_upsert(&local_cfg, c->name);
            shim_entry_free(&local_cfg.shims[idx]);
            local_cfg.shims[idx] = c->entry; /* ownership transferred */
        }
    }
    if (replace_confirmed) {
        for (size_t i = 0; i < removal_count; i++) {
            config_remove(&local_cfg, removal_names[i]);
        }
    }

    ConfigStatus save_st = config_save(&local_cfg, local_cfg_path, errbuf, sizeof(errbuf));
    if (save_st != CONFIG_OK) {
        restore_from_backup(local_cfg_path, cfg_backup_path);
        /* The whole commit aborts together: every split file already
         * written above must revert too, or a shim that was meant to move
         * from config.toml to a split file would end up with neither --
         * its old config.toml entry restored here, but its split file
         * already left holding the archive's new content instead of
         * whatever (if anything) was there before. */
        for (size_t i = 0; i < cand_count; i++) {
            if (cands[i].split_target_path) {
                restore_from_backup(cands[i].split_target_path, cands[i].split_backup_path);
            }
        }
        die("import: failed to save config: %s", errbuf);
    }
    discard_backup(cfg_backup_path);
    for (size_t i = 0; i < cand_count; i++) {
        if (cands[i].split_target_path) {
            discard_backup(cands[i].split_backup_path);
        }
    }

    /* A candidate now landing as config.toml-backed may have previously
     * been split-backed locally (or still have a stray split file from
     * some earlier, unrelated `add --split-config`) -- only safe to sweep
     * now that config.toml itself is durably saved, mirrors add.c's own
     * identical non-split-re-add cleanup ordering ("only sweep stale split
     * files once everything else has already landed safely"). */
    for (size_t i = 0; i < cand_count; i++) {
        ImportCandidate *c = &cands[i];
        if (c->archive_is_split || c->klass == CLASS_IDENTICAL ||
            (c->klass == CLASS_CONFLICT && !c->accepted)) {
            continue;
        }
        remove_split_configs(c->name);
    }

    /* --replace's removal is a real removal, not just dropping the config
     * entry -- mirrors remove.c's own symlink-then-split-file cleanup
     * sequence (config saved first, above; only now, with that durable, is
     * the filesystem half touched), so a removed shim doesn't leave behind
     * exactly the kind of dangling orphan symlink this command otherwise
     * goes out of its way not to touch (see the local-only removal list
     * itself, which already excludes a *pre-existing* orphan for the same
     * reason). */
    if (replace_confirmed) {
        for (size_t i = 0; i < removal_count; i++) {
            char *shim_file = shim_file_name(removal_names[i]);
            char *symlink_path = path_join(shim_dir, shim_file);
            free(shim_file);
            if (access(symlink_path, F_OK) == 0) {
                if (is_shim_dir_entry(symlink_path)) {
                    if (unlink(symlink_path) != 0) {
                        warn("import --replace: failed to remove symlink %s: %s",
                             display_path(symlink_path), plat_strerror(errno));
                    }
                } else {
                    warn("import --replace: %s is not a shimback-managed symlink; leaving it "
                         "alone",
                         display_path(symlink_path));
                }
            }
            free(symlink_path);
            remove_split_configs(removal_names[i]);
        }
    }

    /* Symlinks: created for every shim this import's own archive
     * contributed -- including an IDENTICAL one (ensure_shim_link() is a
     * no-op when the symlink is already healthy, so this is safe to call
     * unconditionally), so a shim whose config.toml survived but whose
     * symlink directory got wiped independently still comes back working.
     * Still scoped to exactly the archive's own shim names, never a
     * blanket doctor-fix-style sweep of anything else. */
    for (size_t i = 0; i < cand_count; i++) {
        ImportCandidate *c = &cands[i];
        if ((c->klass == CLASS_CONFLICT && !c->accepted) || c->write_failed) {
            continue;
        }
        bool was_missing;
        bool occupied;
        char link_errmsg[1024];
        ShimLinkFixStatus st = ensure_shim_link(shim_dir, c->name, self_exe, "import",
                                                &was_missing, &occupied, link_errmsg,
                                                sizeof(link_errmsg));
        if (st == SHIM_LINK_FAILED) {
            c->symlink_failed = true;
            warn("%s", link_errmsg);
        } else if (occupied) {
            c->symlink_occupied = true;
        }
    }

    shim_dir_lock_release(shim_lock_fd);

    /* ---- Summary ---- */

    size_t n_new = 0, n_updated = 0, n_unchanged = 0, n_skipped = 0, n_removed = 0, n_flagged = 0,
           n_failed = 0;
    for (size_t i = 0; i < cand_count; i++) {
        ImportCandidate *c = &cands[i];
        if (c->write_failed) {
            n_failed++;
            continue;
        }
        if (c->klass == CLASS_IDENTICAL) {
            n_unchanged++;
        } else if (c->klass == CLASS_CONFLICT && !c->accepted) {
            n_skipped++;
        } else if (c->klass == CLASS_NEW) {
            n_new++;
        } else {
            n_updated++;
        }
        if (!(c->klass == CLASS_CONFLICT && !c->accepted) &&
            (!c->healthy || c->symlink_failed || c->symlink_occupied)) {
            n_flagged++;
            if (!c->healthy) {
                warn_colored(ANSI_YELLOW,
                             "'%s': configured, but its source/fallback/a route path doesn't "
                             "resolve on this machine -- `shimback doctor` will report it",
                             c->name);
            }
            if (c->symlink_occupied) {
                warn_colored(ANSI_YELLOW,
                             "'%s': configured, but its symlink path is occupied by a non-"
                             "shimback file -- not created",
                             c->name);
            }
        }
    }
    if (replace_confirmed) {
        n_removed = removal_count;
    }

    info("%s%zu imported, %zu updated, %zu unchanged, %zu skipped, %zu removed, %zu flagged, "
         "%zu failed%s",
         colorize ? ANSI_GREEN : "", n_new, n_updated, n_unchanged, n_skipped, n_removed,
         n_flagged, n_failed, colorize ? ANSI_RESET : "");
    recommend("Run `shimback doctor` to check the rest of your setup.");

    /* A flagged shim was still imported -- reported, not a failure of this
     * command. A failed one's own write didn't land (its backup was
     * restored above, so it's left exactly as it was locally, not
     * configured nowhere), but that candidate's own import attempt
     * genuinely didn't happen, which import's own exit code should
     * reflect, the same way `remove` returns non-zero for its own
     * partial-failure case. */
    return n_failed > 0 ? 1 : 0;
}
