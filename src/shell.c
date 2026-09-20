#include "shell.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "paths.h"
#include "util.h"

#define DEFAULT_TAG "shimback"

ShellKind detect_current_shell(void) {
    const char *shell = getenv("SHELL");
    if (!shell || shell[0] == '\0') {
        return SHELL_UNKNOWN;
    }
    const char *slash = strrchr(shell, '/');
    const char *base = slash ? slash + 1 : shell;
    if (strcmp(base, "zsh") == 0) {
        return SHELL_ZSH;
    }
    if (strcmp(base, "bash") == 0) {
        return SHELL_BASH;
    }
    if (strcmp(base, "fish") == 0) {
        return SHELL_FISH;
    }
    return SHELL_UNKNOWN;
}

bool shell_is_installed(ShellKind kind) {
    const char *name = shell_kind_name(kind);
    char *found = path_search(name, NULL, NULL);
    bool ok = found != NULL;
    free(found);
    return ok;
}

const char *shell_kind_name(ShellKind kind) {
    switch (kind) {
        case SHELL_ZSH: return "zsh";
        case SHELL_BASH: return "bash";
        case SHELL_FISH: return "fish";
        default: return "unknown";
    }
}

bool shell_kind_from_name(const char *name, ShellKind *out) {
    if (strcmp(name, "zsh") == 0) {
        *out = SHELL_ZSH;
        return true;
    }
    if (strcmp(name, "bash") == 0) {
        *out = SHELL_BASH;
        return true;
    }
    if (strcmp(name, "fish") == 0) {
        *out = SHELL_FISH;
        return true;
    }
    return false;
}

static char *read_file_or_empty(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return xstrdup("");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return xstrdup("");
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return xstrdup("");
    }
    char *buf = xmalloc((size_t)size + 1);
    size_t n = fread(buf, 1, (size_t)size, f);
    bool read_error = ferror(f) || n != (size_t)size;
    fclose(f);
    if (read_error) {
        /* A short read here (genuine I/O error, or the file changing size
         * underneath us) must not be silently treated as "here's the
         * whole file" -- every caller goes on to compute a modified
         * version of this content and write it straight back over the
         * user's actual shell startup file, so a truncated read here
         * would risk truncating *that* file for real. Refusing to
         * continue is the safe failure mode; nothing this project does is
         * worth risking someone's .zshrc for. */
        free(buf);
        die("failed to read %s completely (got %zu of %ld bytes) -- refusing to risk "
            "overwriting it with a truncated copy",
            path, n, size);
    }
    buf[n] = '\0';
    return buf;
}

/* Finds `marker` in `content` only where it occupies a complete line by
 * itself -- immediately preceded by the start of the string or a newline,
 * and immediately followed by a newline or the end of the string. Plain
 * strstr() would match the marker text anywhere it occurs, including
 * mid-line inside unrelated content (a comment, a string, a command) that
 * happens to contain the same bytes -- which could then make PATH setup
 * or uninstall treat content shimback never wrote as its own managed
 * block, and edit or delete it (see review.md). */
static const char *find_marker_line(const char *content, const char *marker) {
    size_t marker_len = strlen(marker);
    const char *p = content;
    while ((p = strstr(p, marker)) != NULL) {
        bool line_start = (p == content) || (p[-1] == '\n');
        bool line_end = (p[marker_len] == '\0') || (p[marker_len] == '\n');
        if (line_start && line_end) {
            return p;
        }
        p += 1;
    }
    return NULL;
}

/* Finds the tag-marked block in `content`: [*block_start, *block_end) covers
 * the whole block including both marker lines and the trailing newline;
 * [*body_start, *body_end) covers just the interior, between them. Returns
 * false if no such block is present (or it's malformed -- a start marker
 * with no matching end, which just warns and is treated as "not found" so
 * it's left alone rather than risk mangling it). */
static bool find_block(const char *content, const char *tag, const char *rc_path,
                        const char **block_start, const char **block_end,
                        const char **body_start, const char **body_end) {
    char mark_start[128];
    char mark_end[128];
    snprintf(mark_start, sizeof(mark_start), "# >>> %s >>>", tag);
    snprintf(mark_end, sizeof(mark_end), "# <<< %s <<<", tag);

    const char *s = find_marker_line(content, mark_start);
    if (!s) {
        return false;
    }
    const char *e = find_marker_line(s, mark_end);
    if (!e) {
        warn("found a %s start marker without a matching end marker in %s; leaving it alone", tag,
             rc_path);
        return false;
    }
    const char *after = e + strlen(mark_end);
    if (*after == '\n') {
        after++;
    }
    *block_start = s;
    *block_end = after;
    *body_start = s + strlen(mark_start) + 1; /* skip the marker line and its newline */
    *body_end = e;
    return true;
}

/* Appends `s` to `body` as a POSIX shell single-quoted literal -- the
 * standard shlex.quote-equivalent escaping: wrap in '...', and replace each
 * embedded literal quote with '\'' (close quote, escaped literal quote,
 * reopen quote). Inside single quotes nothing else is special to sh/bash/zsh
 * -- not $, backticks, backslashes, or double quotes -- so this is what
 * actually neutralizes a directory path containing shell metacharacters
 * before it's written into someone's startup file (see build_export_body,
 * which previously interpolated the raw path inside a double-quoted string, letting a path like `.../data"; rm -rf ~; #` inject
 * arbitrary commands that ran on the next shell startup). */
static void append_sh_squoted(DynBuf *body, const char *s) {
    dynbuf_append_char(body, '\'');
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '\'') {
            dynbuf_append_str(body, "'\\''");
        } else {
            dynbuf_append_char(body, *p);
        }
    }
    dynbuf_append_char(body, '\'');
}

/* Inverse of append_sh_squoted: reads one single-quoted literal starting at
 * *p (which must point at the opening quote), decoding the '\'' embedded-
 * quote escape back into a plain ', and advances *p past the closing quote.
 * Returns the newly allocated, unescaped string. */
static char *read_sh_squoted(const char **p) {
    (*p)++; /* opening quote */
    DynBuf out;
    dynbuf_init(&out);
    while (**p != '\0') {
        if (**p == '\'') {
            if ((*p)[1] == '\\' && (*p)[2] == '\'' && (*p)[3] == '\'') {
                dynbuf_append_char(&out, '\'');
                *p += 4;
                continue;
            }
            (*p)++; /* closing quote */
            break;
        }
        dynbuf_append_char(&out, **p);
        (*p)++;
    }
    char *s = xstrdup(dynbuf_cstr(&out));
    dynbuf_free(&out);
    return s;
}

/* Extracts the directory list from a block body shaped like
 * build_export_body's output (an `export PATH=` assignment
 * of colon-separated, individually single-quoted directories ahead of a
 * literal trailing "$PATH"), appending each into `out`. Best-effort: a body
 * with no such line just yields no directories, so callers can safely union
 * a new one in regardless. */
static void parse_existing_dirs(const char *body, size_t body_len, StrVec *out) {
    char *copy = xmalloc(body_len + 1);
    memcpy(copy, body, body_len);
    copy[body_len] = '\0';

    const char *marker = "export PATH=";
    const char *p = strstr(copy, marker);
    if (p) {
        p += strlen(marker);
        while (*p == '\'') {
            strvec_push(out, read_sh_squoted(&p));
            if (*p == ':') {
                p++;
            } else {
                break;
            }
        }
    }
    free(copy);
}

static void build_export_body(DynBuf *body, const StrVec *dirs) {
    dynbuf_append_str(body, "export PATH=");
    for (size_t i = 0; i < dirs->count; i++) {
        append_sh_squoted(body, dirs->items[i]);
        dynbuf_append_char(body, ':');
    }
    dynbuf_append_str(body, "\"$PATH\"\n");
}

static bool ends_with(const char *s, const char *suffix) {
    size_t slen = strlen(s);
    size_t suflen = strlen(suffix);
    return slen >= suflen && strcmp(s + slen - suflen, suffix) == 0;
}

/* Merges `dir` into `dirs` in place: a plain union (skip if already
 * present) for most directories, except that shim_bin_dir() (see paths.c)
 * is always "<data dir>/shimback/bin" and can change between invocations if
 * $XDG_DATA_HOME/$HOME changes -- when `dir` is one, any existing entry
 * that's also one (there should be at most one) is replaced instead, so a
 * changed shim directory doesn't leave a stale, dead entry behind in PATH
 * forever. Anything else already present (e.g. install's own bin dir)
 * doesn't match that shape and is left alone. Shared by every shell's
 * ensure-path logic so they all treat a changed shim dir the same way. */
static void merge_dir_into(StrVec *dirs, const char *dir) {
    if (ends_with(dir, "/shimback/bin")) {
        for (size_t i = 0; i < dirs->count;) {
            if (ends_with(dirs->items[i], "/shimback/bin") && strcmp(dirs->items[i], dir) != 0) {
                free(dirs->items[i]);
                dirs->items[i] = dirs->items[dirs->count - 1];
                dirs->count--;
            } else {
                i++;
            }
        }
    }

    for (size_t i = 0; i < dirs->count; i++) {
        if (strcmp(dirs->items[i], dir) == 0) {
            return;
        }
    }
    strvec_push(dirs, xstrdup(dir));
}

/* Idempotently ensures `dir` is included in the marker block tagged `tag`
 * in `rc_path` -- unioning it with whatever directories are already there
 * (from an earlier add/init/install) rather than overwriting them, so those
 * commands can run in any order, each contributing its own directory,
 * without any of them clobbering what another already wrote. */
static bool ensure_dir_in_block(const char *rc_path, const char *tag, const char *dir) {
    char *content = read_file_or_empty(rc_path);

    const char *block_start = NULL;
    const char *block_end = NULL;
    const char *body_start = NULL;
    const char *body_end = NULL;
    bool found =
        find_block(content, tag, rc_path, &block_start, &block_end, &body_start, &body_end);

    StrVec dirs;
    strvec_init(&dirs);
    if (found) {
        parse_existing_dirs(body_start, (size_t)(body_end - body_start), &dirs);
    }

    merge_dir_into(&dirs, dir);

    DynBuf new_body;
    dynbuf_init(&new_body);
    build_export_body(&new_body, &dirs);
    strvec_free(&dirs);

    char mark_start[128];
    char mark_end[128];
    snprintf(mark_start, sizeof(mark_start), "# >>> %s >>>", tag);
    snprintf(mark_end, sizeof(mark_end), "# <<< %s <<<", tag);

    DynBuf desired;
    dynbuf_init(&desired);
    dynbuf_append_str(&desired, mark_start);
    dynbuf_append_char(&desired, '\n');
    dynbuf_append(&desired, new_body.data, new_body.len);
    dynbuf_append_str(&desired, mark_end);
    dynbuf_append_char(&desired, '\n');
    dynbuf_free(&new_body);

    bool ok;
    if (found) {
        size_t existing_len = (size_t)(block_end - block_start);
        if (existing_len == desired.len && memcmp(block_start, desired.data, desired.len) == 0) {
            dynbuf_free(&desired);
            free(content);
            return true; /* already correct */
        }
        DynBuf out;
        dynbuf_init(&out);
        dynbuf_append(&out, content, (size_t)(block_start - content));
        dynbuf_append(&out, desired.data, desired.len);
        dynbuf_append_str(&out, block_end);
        ok = write_file_atomic(rc_path, out.data, out.len, 0644);
        dynbuf_free(&out);
    } else {
        DynBuf out;
        dynbuf_init(&out);
        dynbuf_append_str(&out, content);
        if (out.len > 0 && out.data[out.len - 1] != '\n') {
            dynbuf_append_char(&out, '\n');
        }
        if (out.len > 0) {
            dynbuf_append_char(&out, '\n');
        }
        dynbuf_append(&out, desired.data, desired.len);
        ok = write_file_atomic(rc_path, out.data, out.len, 0644);
        dynbuf_free(&out);
    }

    dynbuf_free(&desired);
    free(content);
    return ok;
}

/* Removes the marker block tagged `tag` from `rc_path`, if present. A no-op
 * (returns true) if the file doesn't exist or has no such block. */
static bool remove_block(const char *rc_path, const char *tag) {
    char *content = read_file_or_empty(rc_path);
    const char *block_start = NULL;
    const char *block_end = NULL;
    const char *body_start = NULL;
    const char *body_end = NULL;
    if (!find_block(content, tag, rc_path, &block_start, &block_end, &body_start, &body_end)) {
        free(content);
        return true;
    }
    (void)body_start;
    (void)body_end;

    DynBuf out;
    dynbuf_init(&out);
    dynbuf_append(&out, content, (size_t)(block_start - content));
    dynbuf_append_str(&out, block_end);
    bool ok = write_file_atomic(rc_path, out.data, out.len, 0644);
    dynbuf_free(&out);
    free(content);
    return ok;
}

/* Most zsh setups (oh-my-zsh, prezto, plain hand-rolled ones) source a
 * ~/.zshrc.local from ~/.zshrc for machine-local overrides, kept out of a
 * dotfiles repo. When present, that's the more appropriate place for our
 * PATH block than ~/.zshrc itself -- same reasoning both when adding it and
 * when removing it, so the two never disagree about where to look. */
static char *zsh_rc_path(const char *home) {
    char *local = path_join(home, ".zshrc.local");
    if (access(local, F_OK) == 0) {
        return local;
    }
    free(local);
    return path_join(home, ".zshrc");
}

bool shell_zsh_block_needs_migration(const char *tag) {
    char *home = home_dir();
    char *local = path_join(home, ".zshrc.local");
    char *rc = path_join(home, ".zshrc");

    bool needs = false;
    if (access(local, F_OK) == 0) {
        char *local_content = read_file_or_empty(local);
        char *rc_content = read_file_or_empty(rc);
        const char *bs, *be, *bods, *bode;
        bool local_has = find_block(local_content, tag, local, &bs, &be, &bods, &bode);
        bool rc_has = find_block(rc_content, tag, rc, &bs, &be, &bods, &bode);
        needs = rc_has && !local_has;
        free(local_content);
        free(rc_content);
    }

    free(local);
    free(rc);
    free(home);
    return needs;
}

bool shell_zsh_migrate_block_to_local(const char *tag) {
    char *home = home_dir();
    char *rc = path_join(home, ".zshrc");
    char *local = path_join(home, ".zshrc.local");

    char *rc_content = read_file_or_empty(rc);
    const char *block_start, *block_end, *body_start, *body_end;
    bool found = find_block(rc_content, tag, rc, &block_start, &block_end, &body_start, &body_end);
    if (!found) {
        free(rc_content);
        free(rc);
        free(local);
        free(home);
        return false;
    }

    StrVec dirs;
    strvec_init(&dirs);
    parse_existing_dirs(body_start, (size_t)(body_end - body_start), &dirs);
    free(rc_content);

    bool ok = true;
    for (size_t i = 0; i < dirs.count && ok; i++) {
        ok = ensure_dir_in_block(local, tag, dirs.items[i]);
    }
    strvec_free(&dirs);

    if (ok) {
        ok = remove_block(rc, tag);
    }

    free(rc);
    free(local);
    free(home);
    return ok;
}

/* fish auto-sources every *.fish snippet dropped into
 * $XDG_CONFIG_HOME/fish/conf.d (falling back to ~/.config/fish/conf.d) at
 * shell startup, alphabetically -- so unlike zsh/bash, which share one rc
 * file our marker-block logic edits in place, fish gives shimback an
 * entire file of its own to own outright: no markers needed, nothing else
 * in the file to avoid clobbering. */
static char *fish_config_dir(void) {
    char *base = xdg_config_home();
    if (!base) {
        char *home = home_dir();
        base = path_join(home, ".config");
        free(home);
    }
    char *fish_dir = path_join(base, "fish");
    free(base);
    return fish_dir;
}

static char *fish_snippet_path(const char *tag) {
    char *fish_dir = fish_config_dir();
    char *confd_dir = path_join(fish_dir, "conf.d");
    free(fish_dir);
    char filename[128];
    snprintf(filename, sizeof(filename), "%s.fish", tag);
    char *path = path_join(confd_dir, filename);
    free(confd_dir);
    return path;
}

/* fish equivalents of append_sh_squoted/read_sh_squoted: inside fish's
 * single quotes, only \\ and \' are recognized escapes (everything else,
 * including $ and the "(...)" command-substitution syntax, is literal) --
 * so those are exactly what need escaping here, in that order (backslash
 * first, so a backslash introduced by quoting the original quote character
 * doesn't itself get re-escaped). Without this, a directory path containing
 * `(` and `)` would have been evaluated as a fish command substitution the
 * next time the snippet was sourced. */
static void append_fish_squoted(DynBuf *body, const char *s) {
    dynbuf_append_char(body, '\'');
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '\\' || *p == '\'') {
            dynbuf_append_char(body, '\\');
        }
        dynbuf_append_char(body, *p);
    }
    dynbuf_append_char(body, '\'');
}

static char *read_fish_squoted(const char **p) {
    (*p)++; /* opening quote */
    DynBuf out;
    dynbuf_init(&out);
    while (**p != '\0' && **p != '\'') {
        if (**p == '\\' && ((*p)[1] == '\\' || (*p)[1] == '\'')) {
            dynbuf_append_char(&out, (*p)[1]);
            *p += 2;
            continue;
        }
        dynbuf_append_char(&out, **p);
        (*p)++;
    }
    if (**p == '\'') {
        (*p)++;
    }
    char *s = xstrdup(dynbuf_cstr(&out));
    dynbuf_free(&out);
    return s;
}

/* Extracts the directory list from a snippet shaped like build_fish_body's
 * output (a `set -gx PATH ...` line listing space-separated, individually
 * single-quoted directories ahead of a literal trailing $PATH), mirroring
 * parse_existing_dirs. Best-effort: a file with no such line just yields no
 * directories. */
static void parse_existing_fish_dirs(const char *content, StrVec *out) {
    const char *marker = "set -gx PATH";
    const char *p = strstr(content, marker);
    if (!p) {
        return;
    }
    p += strlen(marker);
    for (;;) {
        while (*p == ' ') {
            p++;
        }
        if (*p != '\'') {
            break;
        }
        strvec_push(out, read_fish_squoted(&p));
    }
}

static void build_fish_body(DynBuf *body, const StrVec *dirs) {
    dynbuf_append_str(body, "# Managed by shimback -- changes here will be overwritten.\n");
    dynbuf_append_str(body, "set -gx PATH");
    for (size_t i = 0; i < dirs->count; i++) {
        dynbuf_append_char(body, ' ');
        append_fish_squoted(body, dirs->items[i]);
    }
    dynbuf_append_str(body, " $PATH\n");
}

/* Idempotently ensures `dir` is included in `tag`'s fish snippet, unioning
 * it with whatever's already there the same way ensure_dir_in_block does
 * for zsh/bash (see merge_dir_into) -- so add/init/install can run in any
 * order and share one snippet without clobbering each other. Since the
 * snippet is entirely shimback's own file (unlike the zsh/bash marker
 * block, which shares a file with everything else in someone's rc), it's
 * simply rewritten in full each time rather than patched in place. */
static bool ensure_fish(const char *dir, const char *tag, bool verbose) {
    char *path = fish_snippet_path(tag);
    char *confd_dir = dir_of(path);
    if (!mkdir_p(confd_dir)) {
        warn("failed to create %s", confd_dir);
        free(confd_dir);
        free(path);
        return false;
    }
    free(confd_dir);

    char *content = read_file_or_empty(path);
    StrVec dirs;
    strvec_init(&dirs);
    parse_existing_fish_dirs(content, &dirs);
    free(content);

    merge_dir_into(&dirs, dir);

    DynBuf desired;
    dynbuf_init(&desired);
    build_fish_body(&desired, &dirs);
    strvec_free(&dirs);

    bool ok = write_file_atomic(path, desired.data, desired.len, 0644);
    dynbuf_free(&desired);

    if (ok) {
        if (verbose) {
            printf("fish: PATH updated in %s\n", path);
        }
    } else {
        warn("failed to update %s", path);
    }
    free(path);
    return ok;
}

/* Removes the fish snippet entirely -- the inverse of ensure_fish. Since
 * shimback owns the whole file, "removing our block" here just means
 * deleting it. A no-op if it's already gone. */
static bool remove_fish(const char *tag) {
    char *path = fish_snippet_path(tag);
    if (access(path, F_OK) != 0) {
        free(path);
        return true;
    }
    bool ok = unlink(path) == 0;
    if (!ok) {
        warn("failed to remove %s: %s", path, strerror(errno));
    }
    free(path);
    return ok;
}

static bool ensure_zsh(const char *dir, const char *tag, bool verbose) {
    char *home = home_dir();
    char *rc = zsh_rc_path(home);
    bool ok = ensure_dir_in_block(rc, tag, dir);
    if (ok) {
        if (verbose) {
            printf("zsh: PATH updated in %s\n", rc);
        }
    } else {
        warn("failed to update %s", rc);
    }
    free(rc);
    free(home);
    return ok;
}

static bool ensure_bash(const char *dir, const char *tag, bool verbose) {
    static const char *candidates[] = {".bashrc", ".bash_profile", ".profile"};
    char *home = home_dir();
    bool any_exists = false;
    bool all_ok = true;

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        char *path = path_join(home, candidates[i]);
        if (access(path, F_OK) == 0) {
            any_exists = true;
            bool ok = ensure_dir_in_block(path, tag, dir);
            if (ok) {
                if (verbose) {
                    printf("bash: PATH updated in %s\n", path);
                }
            } else {
                warn("failed to update %s", path);
            }
            all_ok = all_ok && ok;
        }
        free(path);
    }

    if (!any_exists) {
        char *path = path_join(home, ".bashrc");
        bool ok = ensure_dir_in_block(path, tag, dir);
        if (ok) {
            if (verbose) {
                printf("bash: created %s with PATH update\n", path);
            }
        } else {
            warn("failed to create %s", path);
        }
        all_ok = ok;
        free(path);
    }

    free(home);
    return all_ok;
}

/* Removes our block from both ~/.zshrc.local and ~/.zshrc, not just
 * whichever zsh_rc_path() would currently pick -- ensure_zsh only ever
 * writes to one of them, but which one can change across the shim's
 * lifetime if ~/.zshrc.local is created or deleted later, and removing a
 * block that isn't there is already a harmless no-op. */
static bool remove_zsh(const char *tag) {
    char *home = home_dir();
    char *local = path_join(home, ".zshrc.local");
    char *rc = path_join(home, ".zshrc");
    bool ok = remove_block(local, tag) && remove_block(rc, tag);
    free(local);
    free(rc);
    free(home);
    return ok;
}

static bool remove_bash(const char *tag) {
    static const char *candidates[] = {".bashrc", ".bash_profile", ".profile"};
    char *home = home_dir();
    bool all_ok = true;

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        char *path = path_join(home, candidates[i]);
        if (access(path, F_OK) == 0) {
            all_ok = remove_block(path, tag) && all_ok;
        }
        free(path);
    }

    free(home);
    return all_ok;
}

bool shell_ensure_path_tagged(ShellKind kind, const char *dir, const char *tag, bool verbose) {
    switch (kind) {
        case SHELL_ZSH:
            return ensure_zsh(dir, tag, verbose);
        case SHELL_BASH:
            return ensure_bash(dir, tag, verbose);
        case SHELL_FISH:
            return ensure_fish(dir, tag, verbose);
        case SHELL_UNKNOWN:
        default:
            printf("could not detect a supported shell; add this to your shell's startup file "
                   "manually:\n  export PATH=\"%s:$PATH\"\n",
                   dir);
            return true;
    }
}

bool shell_ensure_path(ShellKind kind, const char *dir, bool verbose) {
    return shell_ensure_path_tagged(kind, dir, DEFAULT_TAG, verbose);
}

bool shell_remove_path_tagged(ShellKind kind, const char *tag) {
    switch (kind) {
        case SHELL_ZSH:
            return remove_zsh(tag);
        case SHELL_BASH:
            return remove_bash(tag);
        case SHELL_FISH:
            return remove_fish(tag);
        case SHELL_UNKNOWN:
        default:
            /* shimback never wrote a block for this (see shell_ensure_path_tagged),
             * so there's nothing to remove. */
            return true;
    }
}
