#include "shell.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "paths.h"
#include "platform/platform.h"
#include "util.h"

#define DEFAULT_TAG "shimback"

ShellKind detect_current_shell(void) {
#ifdef _WIN32
    /* Windows has no $SHELL-style env var -- the immediate parent process
     * (see plat_parent_process_name) is the nearest equivalent. Both
     * PowerShell editions map to the same SHELL_POWERSHELL; which profile
     * file(s) that actually touches is decided later, in
     * ensure_powershell, based on which editions are actually installed,
     * not on which one happened to be detected here. */
    char *parent = plat_parent_process_name();
    if (!parent) {
        return SHELL_UNKNOWN;
    }
    ShellKind kind = SHELL_UNKNOWN;
    if (strcmp(parent, "powershell.exe") == 0 || strcmp(parent, "pwsh.exe") == 0) {
        kind = SHELL_POWERSHELL;
    } else if (strcmp(parent, "cmd.exe") == 0) {
        kind = SHELL_CMD;
    }
    free(parent);
    return kind;
#else
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
#endif
}

bool shell_is_installed(ShellKind kind) {
#ifdef _WIN32
    if (kind == SHELL_POWERSHELL) {
        char *pwsh = path_search("pwsh", NULL, NULL);
        char *winps = path_search("powershell", NULL, NULL);
        bool ok = pwsh != NULL || winps != NULL;
        free(pwsh);
        free(winps);
        return ok;
    }
#endif
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
        case SHELL_POWERSHELL: return "powershell";
        case SHELL_CMD: return "cmd";
        default: return "unknown";
    }
}

size_t shell_all_kinds(ShellKind *out) {
#ifdef _WIN32
    out[0] = SHELL_POWERSHELL;
    out[1] = SHELL_CMD;
    return 2;
#else
    out[0] = SHELL_ZSH;
    out[1] = SHELL_BASH;
    out[2] = SHELL_FISH;
    return 3;
#endif
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
    if (strcmp(name, "powershell") == 0) {
        *out = SHELL_POWERSHELL;
        return true;
    }
    if (strcmp(name, "cmd") == 0) {
        *out = SHELL_CMD;
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
                        const char *comment_prefix,
                        const char **block_start, const char **block_end,
                        const char **body_start, const char **body_end) {
    char mark_start[128];
    char mark_end[128];
    snprintf(mark_start, sizeof(mark_start), "%s >>> %s >>>", comment_prefix, tag);
    snprintf(mark_end, sizeof(mark_end), "%s <<< %s <<<", comment_prefix, tag);

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
 * before it's written into someone's startup file (see build_zsh_body /
 * build_bash_body, which previously interpolated the raw path inside a
 * double-quoted string, letting a path like `.../data"; rm -rf ~; #` inject
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
 * build_zsh_body's/build_bash_body's output (an `export PATH=` assignment
 * of colon-separated, individually single-quoted directories ahead of a
 * literal trailing "$PATH"), appending each into `out`. Best-effort: a body
 * with no such line just yields no directories, so callers can safely union
 * a new one in regardless. */
static void parse_existing_dirs(const char *body, size_t body_len, StrVec *out) {
    char *copy = xmalloc(body_len + 1);
    memcpy(copy, body, body_len);
    copy[body_len] = '\0';

    /* A zsh block carries the same directories twice -- once in the
     * zsh-defer branch (whose format has changed over time) and once as a
     * plain `export PATH=` in its else branch, whose format never has. Read
     * that one; a bash body has just the single plain assignment. */
    const char *marker = "export PATH=";
    const char *else_marker = "else\n    export PATH=";
    const char *p = strstr(copy, else_marker);
    if (p) {
        p += strlen(else_marker);
    } else {
        p = strstr(copy, marker);
        if (p) {
            p += strlen(marker);
        }
    }
    if (p) {
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

/* Appends `s` escaped for use inside a double-quoted string: the four
 * characters that stay special there (\, ", $, `) get a backslash. */
static void append_dq_escaped(DynBuf *out, const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '\\' || *p == '"' || *p == '$' || *p == '`') {
            dynbuf_append_char(out, '\\');
        }
        dynbuf_append_char(out, *p);
    }
}

/* zsh-defer (https://github.com/romkatv/zsh-defer) lets plugin managers and
 * tools like mise queue their PATH-mutating activation to run asynchronously
 * after the whole rc file has sourced, which would otherwise let them clobber
 * our position on PATH regardless of where our block sits in the file. When
 * zsh-defer is available, queue our export through it too: since our block
 * runs later in a normally-ordered rc file than most such tools' own
 * activation lines, our deferred call is enqueued after theirs and so runs
 * after them, putting our directories back in front once the queue drains.
 *
 * The export goes in as `zsh-defer -c '<command>'`, NOT `zsh-defer export
 * PATH=...:"$PATH"`: in the latter, "$PATH" is expanded when the call is
 * *queued*, so the deferred export would later overwrite PATH with that
 * stale snapshot and silently discard whatever the earlier deferred entries
 * (e.g. mise's) had added in between. Inside the single-quoted -c string
 * "$PATH" is only expanded when the deferred command actually runs.
 *
 * That string is eval'd, so each directory is escaped for double quotes
 * (append_dq_escaped) and the whole command is then single-quoted, keeping
 * a path with shell metacharacters inert at both levels. */
static void build_zsh_body(DynBuf *body, const StrVec *dirs) {
    DynBuf inner;
    dynbuf_init(&inner);
    dynbuf_append_str(&inner, "export PATH=\"");
    for (size_t i = 0; i < dirs->count; i++) {
        append_dq_escaped(&inner, dirs->items[i]);
        dynbuf_append_char(&inner, ':');
    }
    dynbuf_append_str(&inner, "$PATH\"");

    dynbuf_append_str(body, "if command -v zsh-defer >/dev/null 2>&1; then\n");
    dynbuf_append_str(body, "    zsh-defer -c ");
    append_sh_squoted(body, dynbuf_cstr(&inner));
    dynbuf_append_char(body, '\n');
    dynbuf_free(&inner);

    dynbuf_append_str(body, "else\n");
    dynbuf_append_str(body, "    export PATH=");
    for (size_t i = 0; i < dirs->count; i++) {
        append_sh_squoted(body, dirs->items[i]);
        dynbuf_append_char(body, ':');
    }
    dynbuf_append_str(body, "\"$PATH\"\n");
    dynbuf_append_str(body, "fi\n");
}

static void build_bash_body(DynBuf *body, const StrVec *dirs) {
    dynbuf_append_str(body, "export PATH=");
    for (size_t i = 0; i < dirs->count; i++) {
        append_sh_squoted(body, dirs->items[i]);
        dynbuf_append_char(body, ':');
    }
    dynbuf_append_str(body, "\"$PATH\"\n");
}

typedef enum { BODY_BASH, BODY_ZSH, BODY_POWERSHELL } BodyStyle;

/* PowerShell equivalent of append_sh_squoted/read_sh_squoted: inside a
 * PowerShell single-quoted string, only an embedded ' is special, escaped
 * by doubling it ('') -- nothing else (not $, backticks, or double quotes)
 * is interpreted there, matching the same safety property the POSIX
 * shells' single-quote handling already relies on. */
static void append_ps_squoted(DynBuf *body, const char *s) {
    dynbuf_append_char(body, '\'');
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '\'') {
            dynbuf_append_str(body, "''");
        } else {
            dynbuf_append_char(body, *p);
        }
    }
    dynbuf_append_char(body, '\'');
}

static char *read_ps_squoted(const char **p) {
    (*p)++; /* opening quote */
    DynBuf out;
    dynbuf_init(&out);
    while (**p != '\0') {
        if (**p == '\'') {
            if ((*p)[1] == '\'') {
                dynbuf_append_char(&out, '\'');
                *p += 2;
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

/* `$env:Path = '<dir1>' + ';' + '<dir2>' + ';' + $env:Path` -- each
 * directory individually single-quoted (append_ps_squoted) and
 * concatenated with PowerShell's `+` operator, mirroring build_bash_body's
 * per-directory quoting rather than building one quoted/joined list, so
 * parse_existing_ps_dirs can read it back the same simple way
 * parse_existing_dirs does. */
static void build_ps_body(DynBuf *body, const StrVec *dirs) {
    dynbuf_append_str(body, "$env:Path = ");
    for (size_t i = 0; i < dirs->count; i++) {
        append_ps_squoted(body, dirs->items[i]);
        dynbuf_append_str(body, " + ';' + ");
    }
    dynbuf_append_str(body, "$env:Path\n");
}

#define PS_BODY_JOINER " + ';' + "

/* Extracts the directory list from a block body shaped like build_ps_body's
 * output, mirroring parse_existing_dirs. Best-effort: a body with no such
 * line just yields no directories. */
static void parse_existing_ps_dirs(const char *body, size_t body_len, StrVec *out) {
    char *copy = xmalloc(body_len + 1);
    memcpy(copy, body, body_len);
    copy[body_len] = '\0';

    const char *marker = "$env:Path = ";
    const char *p = strstr(copy, marker);
    if (p) {
        p += strlen(marker);
        while (*p == '\'') {
            strvec_push(out, read_ps_squoted(&p));
            if (strncmp(p, PS_BODY_JOINER, strlen(PS_BODY_JOINER)) == 0) {
                p += strlen(PS_BODY_JOINER);
            } else {
                break;
            }
        }
    }
    free(copy);
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
 * without any of them clobbering what another already wrote. `style` picks
 * the zsh-defer-aware body, the plain bash one, or the PowerShell one. */
static bool ensure_dir_in_block(const char *rc_path, const char *tag, const char *dir,
                                 BodyStyle style) {
    char *content = read_file_or_empty(rc_path);

    const char *block_start = NULL;
    const char *block_end = NULL;
    const char *body_start = NULL;
    const char *body_end = NULL;
    bool found =
        find_block(content, tag, rc_path, "#", &block_start, &block_end, &body_start, &body_end);

    StrVec dirs;
    strvec_init(&dirs);
    if (found) {
        if (style == BODY_POWERSHELL) {
            parse_existing_ps_dirs(body_start, (size_t)(body_end - body_start), &dirs);
        } else {
            parse_existing_dirs(body_start, (size_t)(body_end - body_start), &dirs);
        }
    }

    merge_dir_into(&dirs, dir);

    DynBuf new_body;
    dynbuf_init(&new_body);
    switch (style) {
        case BODY_ZSH: build_zsh_body(&new_body, &dirs); break;
        case BODY_POWERSHELL: build_ps_body(&new_body, &dirs); break;
        case BODY_BASH:
        default: build_bash_body(&new_body, &dirs); break;
    }
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
    if (!find_block(content, tag, rc_path, "#", &block_start, &block_end, &body_start,
                     &body_end)) {
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
        bool local_has = find_block(local_content, tag, local, "#", &bs, &be, &bods, &bode);
        bool rc_has = find_block(rc_content, tag, rc, "#", &bs, &be, &bods, &bode);
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
    bool found =
        find_block(rc_content, tag, rc, "#", &block_start, &block_end, &body_start, &body_end);
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
        ok = ensure_dir_in_block(local, tag, dirs.items[i], BODY_ZSH);
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
    bool ok = ensure_dir_in_block(rc, tag, dir, BODY_ZSH);
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
            bool ok = ensure_dir_in_block(path, tag, dir, BODY_BASH);
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
        bool ok = ensure_dir_in_block(path, tag, dir, BODY_BASH);
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

/* Reads the directories out of `rc_path`'s zsh/bash-style marker block. */
static void read_block_dirs_from_rc(const char *rc_path, const char *tag, StrVec *out) {
    char *content = read_file_or_empty(rc_path);
    const char *block_start = NULL;
    const char *block_end = NULL;
    const char *body_start = NULL;
    const char *body_end = NULL;
    if (find_block(content, tag, rc_path, "#", &block_start, &block_end, &body_start,
                    &body_end)) {
        parse_existing_dirs(body_start, (size_t)(body_end - body_start), out);
    }
    free(content);
}

#ifdef _WIN32

/* PowerShell's own $PROFILE.CurrentUserAllHosts, computed directly (via
 * plat_documents_dir()) rather than by asking a spawned powershell/pwsh
 * process -- both editions define it as "<Documents special
 * folder>\<edition subfolder>\profile.ps1", and computing it this way
 * avoids a subprocess spawn on every ensure_path call (e.g. every
 * `shimback add`), while still tracking OneDrive-redirected/relocated
 * Documents folders the same way PowerShell's own lookup would. Returns
 * NULL if the Documents folder itself can't be determined. */
static char *powershell_profile_path(const char *edition_subdir) {
    char *docs = plat_documents_dir();
    if (!docs) {
        return NULL;
    }
    char *edition_dir = path_join(docs, edition_subdir);
    free(docs);
    char *profile = path_join(edition_dir, "profile.ps1");
    free(edition_dir);
    return profile;
}

static void read_ps_block_dirs_from_profile(const char *profile_path, const char *tag,
                                             StrVec *out) {
    char *content = read_file_or_empty(profile_path);
    const char *block_start = NULL;
    const char *block_end = NULL;
    const char *body_start = NULL;
    const char *body_end = NULL;
    if (find_block(content, tag, profile_path, "#", &block_start, &block_end, &body_start,
                    &body_end)) {
        parse_existing_ps_dirs(body_start, (size_t)(body_end - body_start), out);
    }
    free(content);
}

/* Ensures `dir` is on PATH for every installed PowerShell edition: Windows
 * PowerShell (powershell.exe, always available on a stock Windows
 * install) and/or PowerShell 7+ (pwsh.exe, an optional separate install),
 * touching whichever profile(s) correspond to an edition actually found on
 * PATH -- since which edition ends up running a given session can't be
 * known in advance from here, both get the same idempotent block if both
 * are installed, so it works regardless of which one the user launches.
 * Also persists `dir` into HKCU\Environment\Path as the reachability
 * fallback layer (see platform.h). */
static bool ensure_powershell(const char *dir, const char *tag, bool verbose) {
    char *pwsh = path_search("pwsh", NULL, NULL);
    char *winps = path_search("powershell", NULL, NULL);

    bool all_ok = true;
    bool touched_any = false;

    if (winps || !pwsh) {
        char *profile = powershell_profile_path("WindowsPowerShell");
        if (profile) {
            char *profile_dir = dir_of(profile);
            mkdir_p(profile_dir);
            free(profile_dir);
            bool ok = ensure_dir_in_block(profile, tag, dir, BODY_POWERSHELL);
            all_ok = ok && all_ok;
            touched_any = true;
            if (ok) {
                if (verbose) {
                    printf("powershell: PATH updated in %s\n", profile);
                }
            } else {
                warn("failed to update %s", profile);
            }
            free(profile);
        }
    }
    if (pwsh) {
        char *profile = powershell_profile_path("PowerShell");
        if (profile) {
            char *profile_dir = dir_of(profile);
            mkdir_p(profile_dir);
            free(profile_dir);
            bool ok = ensure_dir_in_block(profile, tag, dir, BODY_POWERSHELL);
            all_ok = ok && all_ok;
            touched_any = true;
            if (ok) {
                if (verbose) {
                    printf("pwsh: PATH updated in %s\n", profile);
                }
            } else {
                warn("failed to update %s", profile);
            }
            free(profile);
        }
    }
    free(pwsh);
    free(winps);

    if (!touched_any) {
        warn("could not determine a PowerShell profile location (Documents folder lookup "
             "failed)");
        all_ok = false;
    }

    plat_win_userenv_path_add(dir);
    return all_ok;
}

static bool remove_powershell(const char *tag) {
    StrVec dirs;
    strvec_init(&dirs);

    char *winps_profile = powershell_profile_path("WindowsPowerShell");
    char *pwsh_profile = powershell_profile_path("PowerShell");
    if (winps_profile) {
        read_ps_block_dirs_from_profile(winps_profile, tag, &dirs);
    }
    if (pwsh_profile) {
        read_ps_block_dirs_from_profile(pwsh_profile, tag, &dirs);
    }

    bool ok = true;
    if (winps_profile) {
        ok = remove_block(winps_profile, tag) && ok;
    }
    if (pwsh_profile) {
        ok = remove_block(pwsh_profile, tag) && ok;
    }

    for (size_t i = 0; i < dirs.count; i++) {
        plat_win_userenv_path_remove(dirs.items[i]);
    }
    strvec_free(&dirs);
    free(winps_profile);
    free(pwsh_profile);
    return ok;
}

/* cmd.exe's AutoRun executes only the *first line* of its registry value --
 * confirmed by testing (see windows-port.md Phase 5): an embedded newline
 * is NOT a further command the way a batch file's lines are, so the whole
 * managed block has to be one single line, its commands chained with '&'.
 * That also rules out `rem` as a marker: REM consumes the rest of the
 * physical line regardless of any '&' that follows (also confirmed by
 * testing), so a `rem` marker followed by more '&'-joined commands would
 * silently swallow all of them, breaking anything appended after it. Two
 * harmless `set` assignments -- shimback_block_<tag>=begin/end -- serve as
 * the markers instead; the env-var side effect they leave in every new
 * cmd.exe session is a deliberate, accepted trade for a marker that
 * actually works inside a single '&'-joined line. `tag` is always one of
 * this codebase's own fixed literals ("shimback"/"shimback-bin"), never
 * arbitrary input, so it's safe to embed directly in the variable name
 * without escaping. */
static void autorun_markers(const char *tag, char *start_marker, size_t start_cap,
                             char *end_marker, size_t end_cap) {
    snprintf(start_marker, start_cap, "set \"shimback_block_%s=begin\"&", tag);
    snprintf(end_marker, end_cap, "&set \"shimback_block_%s=end\"&", tag);
}

/* Finds `tag`'s managed segment within `content` (cmd.exe's single-line
 * AutoRun value) via a plain substring search, not the line-anchored
 * find_marker_line used elsewhere -- there are no lines here. [*seg_start,
 * *seg_end) covers the whole segment (both markers and the payload
 * between them); [*body_start, *body_end) covers just the payload.
 * Returns false if not present (or malformed -- a start marker with no
 * matching end, left alone rather than risk mangling it, same policy as
 * find_block). */
static bool find_autorun_segment(const char *content, const char *tag, const char **seg_start,
                                  const char **seg_end, const char **body_start,
                                  const char **body_end) {
    char start_marker[160];
    char end_marker[160];
    autorun_markers(tag, start_marker, sizeof(start_marker), end_marker, sizeof(end_marker));

    const char *s = strstr(content, start_marker);
    if (!s) {
        return false;
    }
    const char *body = s + strlen(start_marker);
    const char *e = strstr(body, end_marker);
    if (!e) {
        warn("found a shimback AutoRun start marker without a matching end marker; leaving it "
             "alone");
        return false;
    }
    *seg_start = s;
    *body_start = body;
    *body_end = e;
    *seg_end = e + strlen(end_marker);
    return true;
}

/* cmd.exe payload: `set "PATH=<dir1>;<dir2>;%PATH%"` -- the whole
 * assignment quoted as one unit (cmd.exe's own idiom for a value
 * containing spaces), with %PATH% expanded by cmd.exe itself when the line
 * runs, not by anything here. Dies if any directory contains a '"':
 * cmd.exe's quoting has no escape for an embedded quote inside "..." (it
 * just ends the quoted region early), so a '"' in --prefix or a shim
 * directory would let arbitrary extra commands be appended to this line
 * and run on every new cmd.exe session -- the same class of injection
 * append_sh_squoted/append_ps_squoted close for the POSIX/PowerShell cases
 * (see review.md), just with no in-band escape available here to
 * neutralize it with instead. */
static void build_cmd_body(DynBuf *body, const StrVec *dirs) {
    for (size_t i = 0; i < dirs->count; i++) {
        if (strchr(dirs->items[i], '"') != NULL) {
            die("refusing to write '%s' into the cmd.exe AutoRun PATH block -- a '\"' in a "
                "directory name can't be safely quoted there",
                dirs->items[i]);
        }
    }
    dynbuf_append_str(body, "set \"PATH=");
    for (size_t i = 0; i < dirs->count; i++) {
        dynbuf_append_str(body, dirs->items[i]);
        dynbuf_append_char(body, ';');
    }
    dynbuf_append_str(body, "%PATH%\"");
}

/* Extracts the directory list from a payload shaped like build_cmd_body's
 * output, mirroring parse_existing_dirs. Best-effort: a payload with no
 * such assignment just yields no directories. */
static void parse_existing_cmd_dirs(const char *body, size_t body_len, StrVec *out) {
    char *copy = xmalloc(body_len + 1);
    memcpy(copy, body, body_len);
    copy[body_len] = '\0';

    const char *marker = "set \"PATH=";
    const char *p = strstr(copy, marker);
    if (p) {
        p += strlen(marker);
        const char *end = strstr(p, "%PATH%\"");
        if (end) {
            char *segment = xstrndup(p, (size_t)(end - p));
            char *saveptr = NULL;
            char *tok = strtok_r(segment, ";", &saveptr);
            while (tok) {
                if (tok[0] != '\0') {
                    strvec_push(out, xstrdup(tok));
                }
                tok = strtok_r(NULL, ";", &saveptr);
            }
            free(segment);
        }
    }
    free(copy);
}

/* Registry-backed sibling of ensure_dir_in_block: same
 * find/parse/build/splice shape, but against the single-line AutoRun
 * *string* (plat_win_autorun_get/set) via find_autorun_segment instead of
 * the line-anchored find_block. */
static bool ensure_dir_in_autorun(const char *tag, const char *dir, bool verbose) {
    char *content = plat_win_autorun_get();

    const char *seg_start = NULL;
    const char *seg_end = NULL;
    const char *body_start = NULL;
    const char *body_end = NULL;
    bool found = find_autorun_segment(content, tag, &seg_start, &seg_end, &body_start, &body_end);

    StrVec dirs;
    strvec_init(&dirs);
    if (found) {
        parse_existing_cmd_dirs(body_start, (size_t)(body_end - body_start), &dirs);
    }
    merge_dir_into(&dirs, dir);

    DynBuf payload;
    dynbuf_init(&payload);
    build_cmd_body(&payload, &dirs);
    strvec_free(&dirs);

    char start_marker[160];
    char end_marker[160];
    autorun_markers(tag, start_marker, sizeof(start_marker), end_marker, sizeof(end_marker));

    DynBuf desired;
    dynbuf_init(&desired);
    dynbuf_append_str(&desired, start_marker);
    dynbuf_append(&desired, payload.data, payload.len);
    dynbuf_append_str(&desired, end_marker);
    dynbuf_free(&payload);

    bool ok;
    if (found) {
        size_t existing_len = (size_t)(seg_end - seg_start);
        if (existing_len == desired.len && memcmp(seg_start, desired.data, desired.len) == 0) {
            dynbuf_free(&desired);
            free(content);
            return true; /* already correct */
        }
        DynBuf out;
        dynbuf_init(&out);
        dynbuf_append(&out, content, (size_t)(seg_start - content));
        dynbuf_append(&out, desired.data, desired.len);
        dynbuf_append_str(&out, seg_end);
        ok = plat_win_autorun_set(dynbuf_cstr(&out));
        dynbuf_free(&out);
    } else {
        /* Prepended, not appended: our block's own trailing '&' correctly
         * chains into whatever (if anything) already occupies AutoRun
         * either way, and prepending guarantees our PATH change is in
         * effect before any later command that might itself depend on
         * PATH runs. */
        DynBuf out;
        dynbuf_init(&out);
        dynbuf_append(&out, desired.data, desired.len);
        dynbuf_append_str(&out, content);
        ok = plat_win_autorun_set(dynbuf_cstr(&out));
        dynbuf_free(&out);
    }
    dynbuf_free(&desired);
    free(content);

    if (ok) {
        if (verbose) {
            printf("cmd: AutoRun updated (HKCU\\Software\\Microsoft\\Command "
                   "Processor\\AutoRun)\n");
        }
    } else {
        warn("failed to update the cmd.exe AutoRun registry value");
    }
    return ok;
}

static void read_autorun_block_dirs(const char *tag, StrVec *out) {
    char *content = plat_win_autorun_get();
    const char *seg_start = NULL;
    const char *seg_end = NULL;
    const char *body_start = NULL;
    const char *body_end = NULL;
    if (find_autorun_segment(content, tag, &seg_start, &seg_end, &body_start, &body_end)) {
        parse_existing_cmd_dirs(body_start, (size_t)(body_end - body_start), out);
    }
    free(content);
}

static bool remove_autorun_block(const char *tag) {
    char *content = plat_win_autorun_get();
    const char *seg_start = NULL;
    const char *seg_end = NULL;
    const char *body_start = NULL;
    const char *body_end = NULL;
    if (!find_autorun_segment(content, tag, &seg_start, &seg_end, &body_start, &body_end)) {
        free(content);
        return true;
    }
    DynBuf out;
    dynbuf_init(&out);
    dynbuf_append(&out, content, (size_t)(seg_start - content));
    dynbuf_append_str(&out, seg_end);
    /* dynbuf_cstr(), not out.data directly: an empty DynBuf (the common
     * case here -- our block was the only thing in AutoRun) never had
     * anything appended to it, so .data is NULL; plat_win_autorun_set(NULL)
     * fails via utf8_to_wide(NULL) instead of writing an empty value,
     * silently leaving the stale block in place. Found by testing --
     * `uninstall --full` looked like it succeeded but the registry value
     * never actually changed. */
    bool ok = plat_win_autorun_set(dynbuf_cstr(&out));
    dynbuf_free(&out);
    free(content);
    return ok;
}

static bool ensure_cmd(const char *dir, const char *tag, bool verbose) {
    bool ok = ensure_dir_in_autorun(tag, dir, verbose);
    plat_win_userenv_path_add(dir);
    return ok;
}

static bool remove_cmd(const char *tag) {
    StrVec dirs;
    strvec_init(&dirs);
    read_autorun_block_dirs(tag, &dirs);

    bool ok = remove_autorun_block(tag);
    for (size_t i = 0; i < dirs.count; i++) {
        plat_win_userenv_path_remove(dirs.items[i]);
    }
    strvec_free(&dirs);
    return ok;
}

#endif /* _WIN32 */

void shell_read_block_dirs(ShellKind kind, const char *tag, StrVec *out) {
    char *home = home_dir();
    switch (kind) {
        case SHELL_ZSH: {
            static const char *candidates[] = {".zshrc.local", ".zshrc"};
            for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
                char *path = path_join(home, candidates[i]);
                read_block_dirs_from_rc(path, tag, out);
                free(path);
            }
            break;
        }
        case SHELL_BASH: {
            static const char *candidates[] = {".bashrc", ".bash_profile", ".profile"};
            for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
                char *path = path_join(home, candidates[i]);
                read_block_dirs_from_rc(path, tag, out);
                free(path);
            }
            break;
        }
        case SHELL_FISH: {
            char *path = fish_snippet_path(tag);
            char *content = read_file_or_empty(path);
            parse_existing_fish_dirs(content, out);
            free(content);
            free(path);
            break;
        }
#ifdef _WIN32
        case SHELL_POWERSHELL: {
            char *winps_profile = powershell_profile_path("WindowsPowerShell");
            char *pwsh_profile = powershell_profile_path("PowerShell");
            if (winps_profile) {
                read_ps_block_dirs_from_profile(winps_profile, tag, out);
            }
            if (pwsh_profile) {
                read_ps_block_dirs_from_profile(pwsh_profile, tag, out);
            }
            free(winps_profile);
            free(pwsh_profile);
            break;
        }
        case SHELL_CMD:
            read_autorun_block_dirs(tag, out);
            break;
#else
        case SHELL_POWERSHELL:
        case SHELL_CMD:
            break;
#endif
        case SHELL_UNKNOWN:
        default: break;
    }
    free(home);
}

bool shell_ensure_path_tagged(ShellKind kind, const char *dir, const char *tag, bool verbose) {
    switch (kind) {
        case SHELL_ZSH:
            return ensure_zsh(dir, tag, verbose);
        case SHELL_BASH:
            return ensure_bash(dir, tag, verbose);
        case SHELL_FISH:
            return ensure_fish(dir, tag, verbose);
#ifdef _WIN32
        case SHELL_POWERSHELL:
            return ensure_powershell(dir, tag, verbose);
        case SHELL_CMD:
            return ensure_cmd(dir, tag, verbose);
#else
        case SHELL_POWERSHELL:
        case SHELL_CMD:
#endif
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
#ifdef _WIN32
        case SHELL_POWERSHELL:
            return remove_powershell(tag);
        case SHELL_CMD:
            return remove_cmd(tag);
#else
        case SHELL_POWERSHELL:
        case SHELL_CMD:
#endif
        case SHELL_UNKNOWN:
        default:
            /* shimback never wrote a block for this (see shell_ensure_path_tagged),
             * so there's nothing to remove. */
            return true;
    }
}
