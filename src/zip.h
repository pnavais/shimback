#ifndef SHIMBACK_ZIP_H
#define SHIMBACK_ZIP_H

#include <stdbool.h>
#include <stddef.h>

#include "util.h" /* DynBuf */

/* A minimal, dependency-free ZIP writer: STORE method only (no
 * compression -- see export.c's own comment for why that's the right
 * tradeoff for small text config files), but a real, standard ZIP file
 * any off-the-shelf tool (Explorer, Archive Utility, unzip) can open.
 * Mirrors sha256.c's own reasoning for hand-rolling a small, well-known
 * format rather than taking on this project's first external dependency.
 *
 * Builds the whole archive into an in-memory DynBuf (`buf`, below) rather
 * than streaming to a FILE* -- matching how config.c's render_config
 * already works, and letting a caller finish the archive then hand
 * `buf.data`/`buf.len` straight to write_file_atomic() (paths.h) for the
 * same temp-file-plus-rename discipline every other on-disk write in this
 * codebase already gets, instead of zip.c needing its own. Realistic
 * config backups are a handful of KB at most, so holding the whole thing
 * in memory is never a concern.
 *
 * Not safe for concurrent use on one ZipWriter from multiple threads --
 * this project's CLI commands are single-threaded throughout, same as
 * every other stateful helper here (DynBuf, StrVec, ...). */
typedef struct {
    DynBuf buf;
    struct ZipCentralEntry *entries; /* owned dynamic array */
    size_t count;
    size_t cap;
} ZipWriter;

/* Zero-initializes `zw`, ready for zip_writer_add_file() calls. */
void zip_writer_init(ZipWriter *zw);

/* Adds one file, stored uncompressed, at `arcname` inside the archive
 * (e.g. "config.toml", "sed-config.toml") with `len` bytes of `data`. */
void zip_writer_add_file(ZipWriter *zw, const char *arcname, const char *data, size_t len);

/* Appends the central directory and end-of-central-directory record to
 * `zw->buf`, completing the archive -- after this, `zw->buf.data`/
 * `zw->buf.len` is the whole, valid zip file, ready to write out (e.g.
 * via write_file_atomic()). Frees the (no-longer-needed) per-entry
 * bookkeeping array; `zw->buf` itself is untouched by this and must
 * still be freed separately (zip_writer_free(), or a direct
 * dynbuf_free(&zw->buf)) once the caller is done reading it. */
void zip_writer_finish(ZipWriter *zw);

/* Frees `zw->buf` and any per-entry bookkeeping not already freed by
 * zip_writer_finish(). Safe to call whether or not finish() was called. */
void zip_writer_free(ZipWriter *zw);

#endif /* SHIMBACK_ZIP_H */
