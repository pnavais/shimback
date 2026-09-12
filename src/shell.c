#include "shell.h"

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
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static bool write_file_atomic(const char *path, const char *content, size_t len) {
    char tmp[4160];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return false;
    }
    size_t written = fwrite(content, 1, len, f);
    bool ok = written == len && fflush(f) == 0;
    fclose(f);
    if (!ok) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

/* Idempotently ensures the marker block tagged `tag` (e.g. "shimback" for
 * the shim dir, a distinct tag for any other directory shimback also needs
 * on PATH), wrapping `body` (the PATH-mutating shell snippet, already
 * newline-terminated), is present (and up to date) in `rc_path`. Distinct
 * tags get distinct markers, so multiple independently-managed blocks can
 * coexist in the same rc file without colliding. See shell.h. */
static bool inject_block(const char *rc_path, const char *body, const char *tag) {
    char mark_start[128];
    char mark_end[128];
    snprintf(mark_start, sizeof(mark_start), "# >>> %s >>>", tag);
    snprintf(mark_end, sizeof(mark_end), "# <<< %s <<<", tag);

    char *content = read_file_or_empty(rc_path);

    DynBuf desired;
    dynbuf_init(&desired);
    dynbuf_append_str(&desired, mark_start);
    dynbuf_append_char(&desired, '\n');
    dynbuf_append_str(&desired, body);
    dynbuf_append_str(&desired, mark_end);
    dynbuf_append_char(&desired, '\n');

    char *start = strstr(content, mark_start);
    bool ok;
    if (start) {
        char *end = strstr(start, mark_end);
        if (!end) {
            warn("found a %s start marker without a matching end marker in %s; leaving it alone",
                 tag, rc_path);
            free(content);
            dynbuf_free(&desired);
            return true;
        }
        char *after_end = end + strlen(mark_end);
        if (*after_end == '\n') {
            after_end++;
        }

        size_t existing_len = (size_t)(after_end - start);
        if (existing_len == desired.len && memcmp(start, desired.data, desired.len) == 0) {
            free(content);
            dynbuf_free(&desired);
            return true; /* already correct */
        }

        DynBuf out;
        dynbuf_init(&out);
        dynbuf_append(&out, content, (size_t)(start - content));
        dynbuf_append(&out, desired.data, desired.len);
        dynbuf_append_str(&out, after_end);
        ok = write_file_atomic(rc_path, out.data, out.len);
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
        ok = write_file_atomic(rc_path, out.data, out.len);
        dynbuf_free(&out);
    }

    free(content);
    dynbuf_free(&desired);
    return ok;
}

/* zsh-defer (https://github.com/romkatv/zsh-defer) lets plugin managers and
 * tools like mise queue their PATH-mutating activation to run asynchronously
 * after the whole rc file has sourced, which would otherwise let them clobber
 * our position on PATH regardless of where our block sits in the file. When
 * zsh-defer is available, queue our export through it too: since our block
 * runs later in a normally-ordered rc file than most such tools' own
 * activation lines, our deferred call is enqueued after theirs and so runs
 * after them, putting the shim dir back in front once the queue drains. */
static void build_zsh_body(DynBuf *body, const char *shim_dir) {
    dynbuf_append_str(body, "if command -v zsh-defer >/dev/null 2>&1; then\n");
    dynbuf_append_str(body, "    zsh-defer export PATH=\"");
    dynbuf_append_str(body, shim_dir);
    dynbuf_append_str(body, ":$PATH\"\n");
    dynbuf_append_str(body, "else\n");
    dynbuf_append_str(body, "    export PATH=\"");
    dynbuf_append_str(body, shim_dir);
    dynbuf_append_str(body, ":$PATH\"\n");
    dynbuf_append_str(body, "fi\n");
}

static bool ensure_zsh(const char *shim_dir, const char *tag) {
    char *home = home_dir();
    char *rc = path_join(home, ".zshrc");

    DynBuf body;
    dynbuf_init(&body);
    build_zsh_body(&body, shim_dir);

    bool ok = inject_block(rc, dynbuf_cstr(&body), tag);
    if (ok) {
        printf("zsh: PATH updated in %s\n", rc);
    } else {
        warn("failed to update %s", rc);
    }
    dynbuf_free(&body);
    free(rc);
    free(home);
    return ok;
}

static void build_bash_body(DynBuf *body, const char *shim_dir) {
    dynbuf_append_str(body, "export PATH=\"");
    dynbuf_append_str(body, shim_dir);
    dynbuf_append_str(body, ":$PATH\"\n");
}

static bool ensure_bash(const char *shim_dir, const char *tag) {
    static const char *candidates[] = {".bashrc", ".bash_profile", ".profile"};
    char *home = home_dir();
    bool any_exists = false;
    bool all_ok = true;

    DynBuf body;
    dynbuf_init(&body);
    build_bash_body(&body, shim_dir);
    const char *body_str = dynbuf_cstr(&body);

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        char *path = path_join(home, candidates[i]);
        if (access(path, F_OK) == 0) {
            any_exists = true;
            bool ok = inject_block(path, body_str, tag);
            if (ok) {
                printf("bash: PATH updated in %s\n", path);
            } else {
                warn("failed to update %s", path);
            }
            all_ok = all_ok && ok;
        }
        free(path);
    }

    if (!any_exists) {
        char *path = path_join(home, ".bashrc");
        bool ok = inject_block(path, body_str, tag);
        if (ok) {
            printf("bash: created %s with PATH update\n", path);
        } else {
            warn("failed to create %s", path);
        }
        all_ok = ok;
        free(path);
    }

    dynbuf_free(&body);
    free(home);
    return all_ok;
}

bool shell_ensure_path_tagged(ShellKind kind, const char *dir, const char *tag) {
    switch (kind) {
        case SHELL_ZSH:
            return ensure_zsh(dir, tag);
        case SHELL_BASH:
            return ensure_bash(dir, tag);
        case SHELL_FISH:
            printf("fish detected but not supported for automatic PATH injection in v0.1.0; "
                   "add manually via: fish_add_path %s\n",
                   dir);
            return true;
        case SHELL_UNKNOWN:
        default:
            printf("could not detect a supported shell; add this to your shell's startup file "
                   "manually:\n  export PATH=\"%s:$PATH\"\n",
                   dir);
            return true;
    }
}

bool shell_ensure_path(ShellKind kind, const char *shim_dir) {
    return shell_ensure_path_tagged(kind, shim_dir, DEFAULT_TAG);
}
