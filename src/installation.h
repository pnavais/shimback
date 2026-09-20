#ifndef SHIMBACK_INSTALLATION_H
#define SHIMBACK_INSTALLATION_H

#include <stdbool.h>
#include <stddef.h>

#include "util.h"

/* One `shimback install`, as recorded in the shell startup files' PATH
 * blocks: `install` writes `<prefix>/bin` into the block right next to the
 * shim directory, so a directory in there that actually holds a shimback
 * binary *is* an installation. */
typedef struct {
    char *bin_dir; /* <prefix>/bin, as recorded in the block */
    char *binary;  /* <bin_dir>/shimback */
    char *prefix;  /* the directory above bin_dir */
} Installation;

/* Finds every installation recorded in any shell's PATH block (current
 * "shimback" tag and the legacy separate "shimback-bin" one), across all
 * three shells' files. A recorded directory only counts if it really
 * contains something that looks like a shimback binary -- a stale entry
 * whose binary has since been deleted is not an installation. Distinct
 * directories only. Returns the count; *out receives a heap array (NULL
 * when the count is 0) to release with free_installations(). */
size_t find_installations(Installation **out);

void free_installations(Installation *list, size_t count);

/* Appends to `out` every directory recorded in a PATH block (besides the shim
 * directory) that does NOT hold a shimback binary -- a stale entry, e.g. from
 * an installation whose binary was later deleted by hand. Distinct only. */
void find_stale_block_dirs(StrVec *out);

/* True if `a` and `b` name the same directory (canonicalized when both
 * exist, else compared as given). */
bool same_dir_path(const char *a, const char *b);

#endif /* SHIMBACK_INSTALLATION_H */
