#include "installation.h"

#include <stdlib.h>
#include <string.h>

#include "paths.h"
#include "shell.h"
#include "util.h"

#define BLOCK_TAG "shimback"
#define LEGACY_BLOCK_TAG "shimback-bin"

bool same_dir_path(const char *a, const char *b) {
    if (strcmp(a, b) == 0) {
        return true;
    }
    char *ca = canonicalize(a);
    char *cb = canonicalize(b);
    bool same = ca && cb && strcmp(ca, cb) == 0;
    free(ca);
    free(cb);
    return same;
}

static bool already_listed(const Installation *list, size_t count, const char *bin_dir) {
    for (size_t i = 0; i < count; i++) {
        if (same_dir_path(list[i].bin_dir, bin_dir)) {
            return true;
        }
    }
    return false;
}

/* Every directory recorded in any shell's PATH block, current and legacy
 * tag alike (repeats possible). */
static void collect_block_dirs(StrVec *dirs) {
    ShellKind kinds[3];
    size_t kind_count = shell_all_kinds(kinds);
    const char *tags[] = {BLOCK_TAG, LEGACY_BLOCK_TAG};
    for (size_t k = 0; k < kind_count; k++) {
        for (size_t t = 0; t < sizeof(tags) / sizeof(tags[0]); t++) {
            shell_read_block_dirs(kinds[k], tags[t], dirs);
        }
    }
}

static bool holds_shimback_binary(const char *dir) {
    char *filename = shimback_exe_name();
    char *binary = path_join(dir, filename);
    free(filename);
    bool ok = is_executable_file(binary) && looks_like_shimback_binary(binary);
    free(binary);
    return ok;
}

void find_stale_block_dirs(StrVec *out) {
    char *shim_dir = shim_bin_dir();
    StrVec dirs;
    strvec_init(&dirs);
    collect_block_dirs(&dirs);
    for (size_t i = 0; i < dirs.count; i++) {
        const char *dir = dirs.items[i];
        if (same_dir_path(dir, shim_dir) || holds_shimback_binary(dir)) {
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < out->count && !dup; j++) {
            dup = same_dir_path(out->items[j], dir);
        }
        if (!dup) {
            strvec_push(out, xstrdup(dir));
        }
    }
    strvec_free(&dirs);
    free(shim_dir);
}

size_t find_installations(Installation **out) {
    char *shim_dir = shim_bin_dir();
    StrVec dirs;
    strvec_init(&dirs);
    collect_block_dirs(&dirs);

    Installation *list = NULL;
    size_t count = 0;
    for (size_t i = 0; i < dirs.count; i++) {
        const char *dir = dirs.items[i];
        if (same_dir_path(dir, shim_dir) || already_listed(list, count, dir)) {
            continue;
        }
        char *filename = shimback_exe_name();
        char *binary = path_join(dir, filename);
        free(filename);
        if (!is_executable_file(binary) || !looks_like_shimback_binary(binary)) {
            free(binary);
            continue;
        }
        list = xrealloc(list, (count + 1) * sizeof(Installation));
        list[count].bin_dir = xstrdup(dir);
        list[count].binary = binary;
        list[count].prefix = dir_of(dir);
        count++;
    }

    strvec_free(&dirs);
    free(shim_dir);
    *out = list;
    return count;
}

void free_installations(Installation *list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(list[i].bin_dir);
        free(list[i].binary);
        free(list[i].prefix);
    }
    free(list);
}
