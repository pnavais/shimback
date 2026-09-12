/* Note: this is a short-lived CLI command handler. Heap allocations here are
 * intentionally not freed before process exit -- the OS reclaims them, and
 * this is a standard, deliberate simplification for one-shot CLI tools (see
 * add.c). */
#include "add_wizard.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../paths.h"
#include "../tui.h"

bool is_valid_shim_name(const char *name) {
    return name[0] != '\0' && strchr(name, '/') == NULL && strcmp(name, "shimback") != 0;
}

typedef enum {
    PAGE_NAME,
    PAGE_POLICY,
    PAGE_SOURCE,
    PAGE_FALLBACK,
    PAGE_PATTERNS,
    PAGE_EXIT_CODES,
    PAGE_ROUTE_ARGS,
    PAGE_STRIP_MATCHED,
    PAGE_REWRITE,
    PAGE_DIAGNOSTIC,
    PAGE_DONE,
} PageId;

static const char *const POLICY_NAMES[5] = {
    "exit-code", "heuristic", "exit-code-match", "route-args", "rewrite",
};
static const char *const POLICY_DESCRIPTIONS[5] = {
    "Fall back whenever source exits non-zero (the default).",
    "Fall back only when source's stderr matches a configured error pattern.",
    "Fall back only when source exits with one of the configured codes.",
    "Pick source or fallback up front, based on the invocation's arguments.",
    "Always run source, rewriting matched arguments first; no fallback used.",
};

/* Working state for the whole wizard: one field (plus a "committed yet"
 * flag) per possible page, mutated in place as pages get committed. */
typedef struct {
    char *name;
    bool name_set;
    Policy policy;
    bool policy_set;
    char *source_arg; /* NULL means "auto" -- source_set tracks whether that's final */
    bool source_set;
    char *fallback_arg;
    bool fallback_set;
    StrVec patterns;
    bool patterns_set;
    int *exit_codes;
    size_t exit_code_count;
    size_t exit_code_cap;
    bool exit_codes_set;
    StrVec route_args;
    bool route_args_set;
    bool strip_matched_args;
    bool strip_matched_set;
    StrVec rewrite_from;
    StrVec rewrite_to;
    bool rewrite_set;
    bool diagnostic;
    bool diagnostic_set;
} WizardState;

typedef struct {
    PageId *items;
    size_t count;
    size_t cap;
} History;

static void history_push(History *h, PageId p) {
    if (h->count == h->cap) {
        h->cap = h->cap == 0 ? 8 : h->cap * 2;
        h->items = xrealloc(h->items, h->cap * sizeof(PageId));
    }
    h->items[h->count++] = p;
}

static PageId next_page(const WizardState *st, PageId current) {
    switch (current) {
        case PAGE_NAME: return PAGE_POLICY;
        case PAGE_POLICY: return PAGE_SOURCE;
        case PAGE_SOURCE: return PAGE_FALLBACK;
        case PAGE_FALLBACK:
            switch (st->policy) {
                case POLICY_HEURISTIC: return PAGE_PATTERNS;
                case POLICY_EXIT_CODE_MATCH: return PAGE_EXIT_CODES;
                case POLICY_ROUTE_ARGS: return PAGE_ROUTE_ARGS;
                case POLICY_REWRITE: return PAGE_REWRITE;
                case POLICY_EXIT_CODE:
                default: return PAGE_DIAGNOSTIC;
            }
        case PAGE_PATTERNS: return PAGE_DIAGNOSTIC;
        case PAGE_EXIT_CODES: return PAGE_DIAGNOSTIC;
        case PAGE_ROUTE_ARGS: return PAGE_STRIP_MATCHED;
        case PAGE_STRIP_MATCHED: return PAGE_DIAGNOSTIC;
        case PAGE_REWRITE: return PAGE_DIAGNOSTIC;
        case PAGE_DIAGNOSTIC:
        case PAGE_DONE:
        default: return PAGE_DONE;
    }
}

/* Advances past the current (just-committed) page: reuses the next history
 * entry if we're editing a page that already had one (mid-history), or
 * computes and appends a fresh one if we're at the frontier. Returns true
 * if the wizard is now complete (the next page is PAGE_DONE). */
static bool advance_or_reuse(WizardState *st, History *hist, size_t *hist_pos) {
    if (*hist_pos == hist->count - 1) {
        PageId nxt = next_page(st, hist->items[*hist_pos]);
        if (nxt == PAGE_DONE) {
            return true;
        }
        history_push(hist, nxt);
        (*hist_pos)++;
    } else {
        (*hist_pos)++;
    }
    return false;
}

static void wizard_push_exit_code(WizardState *st, int value) {
    if (st->exit_code_count == st->exit_code_cap) {
        st->exit_code_cap = st->exit_code_cap == 0 ? 4 : st->exit_code_cap * 2;
        st->exit_codes = xrealloc(st->exit_codes, st->exit_code_cap * sizeof(int));
    }
    st->exit_codes[st->exit_code_count++] = value;
}

/* Changing the policy invalidates everything policy-shape-dependent that
 * came after it in the sequence -- rather than trying to migrate it,
 * simply reset it all back to "unset" and let the user re-enter it. */
static void reset_after_policy_change(WizardState *st) {
    free(st->source_arg);
    st->source_arg = NULL;
    st->source_set = false;
    free(st->fallback_arg);
    st->fallback_arg = NULL;
    st->fallback_set = false;
    strvec_free(&st->patterns);
    strvec_init(&st->patterns);
    st->patterns_set = false;
    free(st->exit_codes);
    st->exit_codes = NULL;
    st->exit_code_count = 0;
    st->exit_code_cap = 0;
    st->exit_codes_set = false;
    strvec_free(&st->route_args);
    strvec_init(&st->route_args);
    st->route_args_set = false;
    st->strip_matched_args = false;
    st->strip_matched_set = false;
    strvec_free(&st->rewrite_from);
    strvec_init(&st->rewrite_from);
    strvec_free(&st->rewrite_to);
    strvec_init(&st->rewrite_to);
    st->rewrite_set = false;
    st->diagnostic = false;
    st->diagnostic_set = false;
}

static bool page_present(const WizardSeed *seed, const WizardState *st, PageId p) {
    switch (p) {
        case PAGE_NAME: return seed->name != NULL;
        case PAGE_POLICY: return true;
        case PAGE_SOURCE: return true;
        case PAGE_FALLBACK: return seed->fallback_arg != NULL || st->policy == POLICY_REWRITE;
        case PAGE_PATTERNS: return seed->patterns->count > 0;
        case PAGE_EXIT_CODES: return seed->exit_code_count > 0;
        case PAGE_ROUTE_ARGS: return seed->route_args->count > 0;
        case PAGE_STRIP_MATCHED: return true;
        case PAGE_REWRITE: return seed->rewrite_from->count > 0;
        case PAGE_DIAGNOSTIC: return true;
        default: return false;
    }
}

static void auto_commit_page(WizardState *st, const WizardSeed *seed, PageId p) {
    switch (p) {
        case PAGE_NAME:
            st->name = xstrdup(seed->name);
            st->name_set = true;
            break;
        case PAGE_POLICY:
            st->policy = seed->policy;
            st->policy_set = true;
            break;
        case PAGE_SOURCE:
            free(st->source_arg);
            st->source_arg = seed->source_arg ? xstrdup(seed->source_arg) : NULL;
            st->source_set = true;
            break;
        case PAGE_FALLBACK:
            free(st->fallback_arg);
            st->fallback_arg = seed->fallback_arg ? xstrdup(seed->fallback_arg) : NULL;
            st->fallback_set = true;
            break;
        case PAGE_PATTERNS:
            for (size_t i = 0; i < seed->patterns->count; i++) {
                strvec_push(&st->patterns, xstrdup(seed->patterns->items[i]));
            }
            st->patterns_set = true;
            break;
        case PAGE_EXIT_CODES:
            for (size_t i = 0; i < seed->exit_code_count; i++) {
                wizard_push_exit_code(st, seed->exit_codes[i]);
            }
            st->exit_codes_set = true;
            break;
        case PAGE_ROUTE_ARGS:
            for (size_t i = 0; i < seed->route_args->count; i++) {
                strvec_push(&st->route_args, xstrdup(seed->route_args->items[i]));
            }
            st->route_args_set = true;
            break;
        case PAGE_STRIP_MATCHED:
            st->strip_matched_args = seed->strip_matched_args;
            st->strip_matched_set = true;
            break;
        case PAGE_REWRITE:
            for (size_t i = 0; i < seed->rewrite_from->count; i++) {
                strvec_push(&st->rewrite_from, xstrdup(seed->rewrite_from->items[i]));
                strvec_push(&st->rewrite_to, xstrdup(seed->rewrite_to->items[i]));
            }
            st->rewrite_set = true;
            break;
        case PAGE_DIAGNOSTIC:
            st->diagnostic = seed->diagnostic;
            st->diagnostic_set = true;
            break;
        default:
            break;
    }
}

/* Walks the fixed page sequence from the start, silently auto-committing
 * every page whose data the seed already provides, stopping at the first
 * one that isn't -- that becomes the wizard's initial live/interactive
 * page. Everything auto-committed remains reachable (and editable) via the
 * Left arrow. */
static void pre_seed(WizardState *st, const WizardSeed *seed, History *hist) {
    PageId p = PAGE_NAME;
    for (;;) {
        if (!page_present(seed, st, p)) {
            history_push(hist, p);
            return;
        }
        auto_commit_page(st, seed, p);
        history_push(hist, p);
        PageId nxt = next_page(st, p);
        if (nxt == PAGE_DONE) {
            return; /* everything was already present; nothing left to ask */
        }
        p = nxt;
    }
}

/* ---- fzf integration (best-effort; a no-op wherever fzf isn't found) ---- */

static char *build_temp_path(const char *suffix) {
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || tmpdir[0] == '\0') {
        tmpdir = "/tmp";
    }
    return path_join(tmpdir, suffix);
}

/* Launches `fzf` interactively against `candidates`, via temp files (not
 * live pipes -- a large PATH-executable candidate list could otherwise
 * fill a pipe buffer and deadlock against a child that hasn't started
 * reading yet) for its stdin/stdout; fzf manages /dev/tty directly for its
 * own UI regardless. Returns the selected line (trimmed), or NULL if
 * nothing was selected (fzf not found, Esc inside fzf, or any I/O
 * failure). */
static char *run_fzf(const char *const *candidates, size_t count, const char *prompt) {
    char *fzf_path = path_search("fzf", NULL, NULL);
    if (!fzf_path) {
        return NULL;
    }

    char *in_path = build_temp_path("shimback_fzf_in_XXXXXX");
    char *out_path = build_temp_path("shimback_fzf_out_XXXXXX");
    int in_fd = mkstemp(in_path);
    int out_fd = in_fd >= 0 ? mkstemp(out_path) : -1;
    char *result = NULL;

    if (in_fd >= 0 && out_fd >= 0) {
        for (size_t i = 0; i < count; i++) {
            dprintf(in_fd, "%s\n", candidates[i]);
        }
        lseek(in_fd, 0, SEEK_SET);

        pid_t pid = fork();
        if (pid == 0) {
            dup2(in_fd, STDIN_FILENO);
            dup2(out_fd, STDOUT_FILENO);
            close(in_fd);
            close(out_fd);
            execl(fzf_path, fzf_path, "--height=~40%", "--reverse", "--prompt", prompt,
                  (char *)NULL);
            _exit(127);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);

            /* fzf saves/restores /dev/tty's termios itself around its own
             * run, but re-assert our own raw mode defensively before the
             * wizard's read loop resumes. */
            tui_raw_mode_enter();

            char buf[4096];
            lseek(out_fd, 0, SEEK_SET);
            ssize_t n = read(out_fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                char *newline = strchr(buf, '\n');
                if (newline) {
                    *newline = '\0';
                }
                if (buf[0] != '\0') {
                    result = xstrdup(buf);
                }
            }
        }
    }

    if (in_fd >= 0) {
        close(in_fd);
    }
    if (out_fd >= 0) {
        close(out_fd);
    }
    unlink(in_path);
    unlink(out_path);
    free(in_path);
    free(out_path);
    free(fzf_path);
    return result;
}

static int fzf_pick_policy(void) {
    char *picked = run_fzf(POLICY_NAMES, 5, "policy> ");
    if (!picked) {
        return -1;
    }
    int idx = -1;
    for (int i = 0; i < 5; i++) {
        if (strcmp(picked, POLICY_NAMES[i]) == 0) {
            idx = i;
            break;
        }
    }
    free(picked);
    return idx;
}

static void collect_dedup(StrVec *out, const char *full_path) {
    for (size_t i = 0; i < out->count; i++) {
        if (strcmp(out->items[i], full_path) == 0) {
            return;
        }
    }
    strvec_push(out, xstrdup(full_path));
}

/* Every regular, executable file found by scanning each directory on
 * $PATH, deduplicated by full path -- the candidate list for the wizard's
 * fzf-based binary picker. Silently skips directories it can't open,
 * mirroring how a shell tolerates a bogus PATH entry. */
static StrVec list_path_executables(void) {
    StrVec out;
    strvec_init(&out);
    const char *path_env = getenv("PATH");
    if (!path_env) {
        return out;
    }
    char *copy = xstrdup(path_env);
    char *saveptr = NULL;
    for (char *dir = strtok_r(copy, ":", &saveptr); dir != NULL;
         dir = strtok_r(NULL, ":", &saveptr)) {
        DIR *d = opendir(dir);
        if (!d) {
            continue;
        }
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            char *full = path_join(dir, ent->d_name);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0) {
                collect_dedup(&out, full);
            }
            free(full);
        }
        closedir(d);
    }
    free(copy);
    return out;
}

static char *fzf_pick_binary(void) {
    StrVec execs = list_path_executables();
    if (execs.count == 0) {
        strvec_free(&execs);
        return NULL;
    }
    char *picked = run_fzf((const char *const *)execs.items, execs.count, "binary> ");
    strvec_free(&execs);
    return picked;
}

/* ---- rendering ---- */

static void print_breadcrumb(const WizardState *st, const History *hist, size_t hist_pos,
                              bool colorize) {
    const char *dim = colorize ? ANSI_DIM : "";
    const char *reset = colorize ? ANSI_RESET : "";
    for (size_t i = 0; i < hist_pos; i++) {
        switch (hist->items[i]) {
            case PAGE_NAME:
                printf("%s  name:       %s%s\n", dim, st->name ? st->name : "", reset);
                break;
            case PAGE_POLICY:
                printf("%s  policy:     %s%s\n", dim, policy_to_string(st->policy), reset);
                break;
            case PAGE_SOURCE:
                printf("%s  source:     %s%s\n", dim, st->source_arg ? st->source_arg : "(auto)",
                       reset);
                break;
            case PAGE_FALLBACK:
                printf("%s  fallback:   %s%s\n", dim,
                       st->fallback_arg ? st->fallback_arg : "(none)", reset);
                break;
            case PAGE_PATTERNS:
                printf("%s  patterns:   %zu configured%s\n", dim, st->patterns.count, reset);
                break;
            case PAGE_EXIT_CODES:
                printf("%s  exit codes: %zu configured%s\n", dim, st->exit_code_count, reset);
                break;
            case PAGE_ROUTE_ARGS:
                printf("%s  route args: %zu configured%s\n", dim, st->route_args.count, reset);
                break;
            case PAGE_STRIP_MATCHED:
                printf("%s  strip matched args: %s%s\n", dim,
                       st->strip_matched_args ? "yes" : "no", reset);
                break;
            case PAGE_REWRITE:
                printf("%s  rewrite rules: %zu configured%s\n", dim, st->rewrite_from.count,
                       reset);
                break;
            case PAGE_DIAGNOSTIC:
                printf("%s  diagnostic: %s%s\n", dim, st->diagnostic ? "yes" : "no", reset);
                break;
            default:
                break;
        }
    }
}

static void print_list_page(const char *label, const StrVec *list, const char *input) {
    printf("%s (at least one required; blank line to finish once you have one):\n", label);
    for (size_t i = 0; i < list->count; i++) {
        printf("  %zu. %s\n", i + 1, list->items[i]);
    }
    printf("> %s\n", input);
}

static void render_page(const WizardState *st, const History *hist, size_t hist_pos, PageId page,
                         const char *input, int policy_highlight, const char *error_msg) {
    bool colorize = stdout_is_color();
    const char *hdr = colorize ? ANSI_BOLD ANSI_YELLOW : "";
    const char *reset = colorize ? ANSI_RESET : "";
    const char *err_color = colorize ? ANSI_RED : "";
    const char *hl_color = colorize ? ANSI_BOLD ANSI_GREEN : "";

    tui_clear_screen();
    printf("%sshimback add -- interactive wizard%s\n\n", hdr, reset);

    print_breadcrumb(st, hist, hist_pos, colorize);
    if (hist_pos > 0) {
        printf("\n");
    }

    switch (page) {
        case PAGE_NAME:
            printf("Shim name:\n> %s\n", input);
            break;
        case PAGE_POLICY:
            printf("Policy:\n");
            for (int i = 0; i < 5; i++) {
                bool hl = (i == policy_highlight);
                if (hl) {
                    printf("> %s%s%s\n", hl_color, POLICY_NAMES[i], reset);
                } else {
                    printf("  %s\n", POLICY_NAMES[i]);
                }
            }
            printf("\n%s\n", POLICY_DESCRIPTIONS[policy_highlight]);
            break;
        case PAGE_SOURCE:
            printf("Source command (optional -- blank means \"auto\", resolved from $PATH at "
                   "run time):\n> %s\n",
                   input);
            break;
        case PAGE_FALLBACK:
            printf("Fallback command%s:\n> %s\n",
                   st->policy == POLICY_REWRITE ? " (optional -- unused by policy rewrite)" : "",
                   input);
            break;
        case PAGE_PATTERNS:
            print_list_page("Error patterns", &st->patterns, input);
            break;
        case PAGE_ROUTE_ARGS:
            print_list_page("Route args", &st->route_args, input);
            break;
        case PAGE_EXIT_CODES:
            printf("Exit codes (at least one required; blank line to finish once you have "
                   "one):\n");
            for (size_t i = 0; i < st->exit_code_count; i++) {
                printf("  %zu. %d\n", i + 1, st->exit_codes[i]);
            }
            printf("> %s\n", input);
            break;
        case PAGE_REWRITE:
            printf("Rewrite rules, as <from>=<to> (at least one required; blank line to finish "
                   "once you have one):\n");
            for (size_t i = 0; i < st->rewrite_from.count; i++) {
                printf("  %zu. %s=%s\n", i + 1, st->rewrite_from.items[i],
                       st->rewrite_to.items[i]);
            }
            printf("> %s\n", input);
            break;
        case PAGE_STRIP_MATCHED:
            printf("Strip matched route args before forwarding? [y/N]\n> %s\n", input);
            break;
        case PAGE_DIAGNOSTIC:
            printf("Print a one-line diagnostic when this shim falls back? [y/N]\n> %s\n", input);
            break;
        default:
            break;
    }

    if (error_msg[0] != '\0') {
        printf("\n%s%s%s\n", err_color, error_msg, reset);
    }

    printf("\n[Enter] confirm & continue   [<-] back   [->] forward");
    if (page == PAGE_POLICY || page == PAGE_SOURCE || page == PAGE_FALLBACK) {
        printf("   [Tab] fzf");
    }
    printf("   [Esc/^C] abort\n");
    fflush(stdout);
}

/* ---- main loop ---- */

bool run_add_wizard(const WizardSeed *seed, WizardResult *out) {
    WizardState st = {0};
    strvec_init(&st.patterns);
    strvec_init(&st.route_args);
    strvec_init(&st.rewrite_from);
    strvec_init(&st.rewrite_to);
    st.policy = seed->policy;

    History hist = {0};
    pre_seed(&st, seed, &hist);
    size_t hist_pos = hist.count - 1;

    if (!tui_raw_mode_enter()) {
        strvec_free(&st.patterns);
        strvec_free(&st.route_args);
        strvec_free(&st.rewrite_from);
        strvec_free(&st.rewrite_to);
        free(st.exit_codes);
        free(st.name);
        free(st.source_arg);
        free(st.fallback_arg);
        free(hist.items);
        return false;
    }

    char *self_exe = self_exe_path();

    char input[4096] = "";
    size_t input_len = 0;
    int policy_highlight = (int)st.policy;
    char error_msg[256] = "";
    size_t last_rendered_pos = (size_t)-1;

    bool done = false;
    bool aborted = false;

    while (!done && !aborted) {
        PageId page = hist.items[hist_pos];

        if (hist_pos != last_rendered_pos) {
            input[0] = '\0';
            input_len = 0;
            error_msg[0] = '\0';
            switch (page) {
                case PAGE_NAME:
                    if (st.name) {
                        snprintf(input, sizeof(input), "%s", st.name);
                        input_len = strlen(input);
                    }
                    break;
                case PAGE_POLICY:
                    policy_highlight = (int)st.policy;
                    break;
                case PAGE_SOURCE:
                    if (st.source_arg) {
                        snprintf(input, sizeof(input), "%s", st.source_arg);
                        input_len = strlen(input);
                    }
                    break;
                case PAGE_FALLBACK:
                    if (st.fallback_arg) {
                        snprintf(input, sizeof(input), "%s", st.fallback_arg);
                        input_len = strlen(input);
                    }
                    break;
                case PAGE_STRIP_MATCHED:
                    /* Pre-fill with the previously committed answer so
                     * revisiting and pressing Enter unchanged reconfirms it
                     * instead of silently resetting a prior "yes" to "no"
                     * (the commit logic below derives the boolean purely
                     * from this buffer's content). */
                    if (st.strip_matched_set && st.strip_matched_args) {
                        snprintf(input, sizeof(input), "y");
                        input_len = 1;
                    }
                    break;
                case PAGE_DIAGNOSTIC:
                    if (st.diagnostic_set && st.diagnostic) {
                        snprintf(input, sizeof(input), "y");
                        input_len = 1;
                    }
                    break;
                default:
                    break;
            }
            last_rendered_pos = hist_pos;
        }

        render_page(&st, &hist, hist_pos, page, input, policy_highlight, error_msg);
        error_msg[0] = '\0';

        TuiKey key = tui_read_key();

        if (key.type == TUI_KEY_ABORT) {
            aborted = true;
            continue;
        }
        if (key.type == TUI_KEY_LEFT) {
            if (hist_pos > 0) {
                hist_pos--;
            }
            continue;
        }
        if (key.type == TUI_KEY_RIGHT) {
            if (hist_pos + 1 < hist.count) {
                hist_pos++;
            }
            continue;
        }

        if (page == PAGE_POLICY) {
            if (key.type == TUI_KEY_UP) {
                policy_highlight = (policy_highlight + 4) % 5;
                continue;
            }
            if (key.type == TUI_KEY_DOWN) {
                policy_highlight = (policy_highlight + 1) % 5;
                continue;
            }
            if (key.type == TUI_KEY_TAB) {
                int picked = fzf_pick_policy();
                if (picked >= 0) {
                    policy_highlight = picked;
                }
                continue;
            }
            if (key.type == TUI_KEY_ENTER) {
                Policy new_policy = (Policy)policy_highlight;
                bool changed = st.policy_set && new_policy != st.policy;
                st.policy = new_policy;
                st.policy_set = true;
                if (changed) {
                    reset_after_policy_change(&st);
                    hist.count = hist_pos + 1;
                }
                if (advance_or_reuse(&st, &hist, &hist_pos)) {
                    done = true;
                }
                continue;
            }
            continue;
        }

        if ((page == PAGE_SOURCE || page == PAGE_FALLBACK) && key.type == TUI_KEY_TAB) {
            char *picked = fzf_pick_binary();
            if (picked) {
                snprintf(input, sizeof(input), "%s", picked);
                input_len = strlen(input);
                free(picked);
            }
            continue;
        }

        if (key.type == TUI_KEY_CHAR) {
            if (input_len + 1 < sizeof(input)) {
                input[input_len++] = key.ch;
                input[input_len] = '\0';
            }
            continue;
        }
        if (key.type == TUI_KEY_BACKSPACE) {
            if (input_len > 0) {
                input[--input_len] = '\0';
            }
            continue;
        }
        if (key.type != TUI_KEY_ENTER) {
            continue;
        }

        switch (page) {
            case PAGE_NAME:
                if (input_len == 0) {
                    snprintf(error_msg, sizeof(error_msg), "Name is required.");
                } else if (!is_valid_shim_name(input)) {
                    snprintf(error_msg, sizeof(error_msg), "Invalid shim name '%s'.", input);
                } else {
                    free(st.name);
                    st.name = xstrdup(input);
                    st.name_set = true;
                    if (advance_or_reuse(&st, &hist, &hist_pos)) {
                        done = true;
                    }
                }
                break;

            case PAGE_SOURCE:
                if (input_len == 0) {
                    free(st.source_arg);
                    st.source_arg = NULL;
                    st.source_set = true;
                    if (advance_or_reuse(&st, &hist, &hist_pos)) {
                        done = true;
                    }
                } else {
                    char *resolved = resolve_binary_arg(input);
                    if (!resolved) {
                        snprintf(error_msg, sizeof(error_msg),
                                 "'%s' does not exist, is not executable, or isn't on $PATH.",
                                 input);
                        input[0] = '\0';
                        input_len = 0;
                    } else if (strcmp(resolved, self_exe) == 0) {
                        snprintf(error_msg, sizeof(error_msg),
                                 "'%s' resolves back to the shimback binary itself.", input);
                        free(resolved);
                        input[0] = '\0';
                        input_len = 0;
                    } else {
                        free(resolved);
                        free(st.source_arg);
                        st.source_arg = xstrdup(input);
                        st.source_set = true;
                        if (advance_or_reuse(&st, &hist, &hist_pos)) {
                            done = true;
                        }
                    }
                }
                break;

            case PAGE_FALLBACK: {
                bool optional = (st.policy == POLICY_REWRITE);
                if (input_len == 0) {
                    if (!optional) {
                        snprintf(error_msg, sizeof(error_msg),
                                 "Fallback is required (except with policy rewrite).");
                    } else {
                        free(st.fallback_arg);
                        st.fallback_arg = NULL;
                        st.fallback_set = true;
                        if (advance_or_reuse(&st, &hist, &hist_pos)) {
                            done = true;
                        }
                    }
                    break;
                }
                char *resolved = resolve_binary_arg(input);
                if (!resolved) {
                    snprintf(error_msg, sizeof(error_msg),
                             "'%s' does not exist, is not executable, or isn't on $PATH.", input);
                    input[0] = '\0';
                    input_len = 0;
                    break;
                }
                if (strcmp(resolved, self_exe) == 0) {
                    snprintf(error_msg, sizeof(error_msg),
                             "'%s' resolves back to the shimback binary itself.", input);
                    free(resolved);
                    input[0] = '\0';
                    input_len = 0;
                    break;
                }
                if (st.source_arg) {
                    char *resolved_source = resolve_binary_arg(st.source_arg);
                    if (resolved_source && strcmp(resolved_source, resolved) == 0) {
                        snprintf(error_msg, sizeof(error_msg),
                                 "Source and fallback both resolve to '%s'.", resolved);
                        free(resolved_source);
                        free(resolved);
                        input[0] = '\0';
                        input_len = 0;
                        break;
                    }
                    free(resolved_source);
                }
                free(resolved);
                free(st.fallback_arg);
                st.fallback_arg = xstrdup(input);
                st.fallback_set = true;
                if (advance_or_reuse(&st, &hist, &hist_pos)) {
                    done = true;
                }
                break;
            }

            case PAGE_PATTERNS:
            case PAGE_ROUTE_ARGS: {
                StrVec *list = (page == PAGE_PATTERNS) ? &st.patterns : &st.route_args;
                if (input_len == 0) {
                    if (list->count == 0) {
                        snprintf(error_msg, sizeof(error_msg), "At least one is required.");
                    } else {
                        if (page == PAGE_PATTERNS) {
                            st.patterns_set = true;
                        } else {
                            st.route_args_set = true;
                        }
                        if (advance_or_reuse(&st, &hist, &hist_pos)) {
                            done = true;
                        }
                    }
                } else {
                    strvec_push(list, xstrdup(input));
                    input[0] = '\0';
                    input_len = 0;
                }
                break;
            }

            case PAGE_EXIT_CODES:
                if (input_len == 0) {
                    if (st.exit_code_count == 0) {
                        snprintf(error_msg, sizeof(error_msg), "At least one is required.");
                    } else {
                        st.exit_codes_set = true;
                        if (advance_or_reuse(&st, &hist, &hist_pos)) {
                            done = true;
                        }
                    }
                } else {
                    char *end;
                    long v = strtol(input, &end, 10);
                    if (*input == '\0' || *end != '\0' || v < 0 || v > 255) {
                        snprintf(error_msg, sizeof(error_msg),
                                 "Must be an integer between 0 and 255.");
                        input[0] = '\0';
                        input_len = 0;
                    } else {
                        wizard_push_exit_code(&st, (int)v);
                        input[0] = '\0';
                        input_len = 0;
                    }
                }
                break;

            case PAGE_REWRITE:
                if (input_len == 0) {
                    if (st.rewrite_from.count == 0) {
                        snprintf(error_msg, sizeof(error_msg), "At least one is required.");
                    } else {
                        st.rewrite_set = true;
                        if (advance_or_reuse(&st, &hist, &hist_pos)) {
                            done = true;
                        }
                    }
                } else {
                    char *eq = strchr(input, '=');
                    if (!eq || eq == input) {
                        snprintf(error_msg, sizeof(error_msg), "Must be '<from>=<to>'.");
                        input[0] = '\0';
                        input_len = 0;
                    } else {
                        strvec_push(&st.rewrite_from, xstrndup(input, (size_t)(eq - input)));
                        strvec_push(&st.rewrite_to, xstrdup(eq + 1));
                        input[0] = '\0';
                        input_len = 0;
                    }
                }
                break;

            case PAGE_STRIP_MATCHED:
                st.strip_matched_args = (input_len > 0 && (input[0] == 'y' || input[0] == 'Y'));
                st.strip_matched_set = true;
                if (advance_or_reuse(&st, &hist, &hist_pos)) {
                    done = true;
                }
                break;

            case PAGE_DIAGNOSTIC:
                st.diagnostic = (input_len > 0 && (input[0] == 'y' || input[0] == 'Y'));
                st.diagnostic_set = true;
                if (advance_or_reuse(&st, &hist, &hist_pos)) {
                    done = true;
                }
                break;

            default:
                break;
        }
    }

    tui_raw_mode_exit();
    tui_clear_screen();
    free(hist.items);

    if (aborted) {
        strvec_free(&st.patterns);
        strvec_free(&st.route_args);
        strvec_free(&st.rewrite_from);
        strvec_free(&st.rewrite_to);
        free(st.exit_codes);
        free(st.name);
        free(st.source_arg);
        free(st.fallback_arg);
        return false;
    }

    out->name = st.name;
    out->source_arg = st.source_arg;
    out->fallback_arg = st.fallback_arg;
    out->policy = st.policy;
    out->patterns = st.patterns;
    out->exit_codes = st.exit_codes;
    out->exit_code_count = st.exit_code_count;
    out->route_args = st.route_args;
    out->strip_matched_args = st.strip_matched_args;
    out->rewrite_from = st.rewrite_from;
    out->rewrite_to = st.rewrite_to;
    out->diagnostic = st.diagnostic;
    return true;
}
