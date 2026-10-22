#ifndef SHIMBACK_ZIP_H
#define SHIMBACK_ZIP_H

#include <stdbool.h>
#include <stddef.h>

#include "util.h" /* DynBuf */

/* A minimal, dependency-free ZIP writer and reader: STORE method only (no
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

/* One file inside an opened archive -- `name` is owned, `data`/`size`
 * borrow directly into the buffer zip_reader_open() was given (never
 * copied: that buffer must outlive this ZipReader). */
typedef struct {
    char *name; /* owned */
    const char *data; /* borrowed */
    size_t size;
} ZipReaderEntry;

typedef struct {
    ZipReaderEntry *entries; /* owned array */
    size_t count;
} ZipReader;

/* Parses `data` (`len` bytes, NOT copied -- must outlive `zr`, and must not
 * be freed before zip_reader_free()) as a ZIP archive: scans backward from
 * the end for the end-of-central-directory (EOCD) signature (bounded to the
 * last 22 + 65535 bytes -- the maximum possible archive comment length,
 * same single-disk, no-ZIP64 assumption the writer itself makes), then
 * walks the central directory. For each entry, re-reads its own *local*
 * file header (not just the central directory's copy of it -- a local
 * header's name/extra-field lengths are the authoritative ones for locating
 * where its data actually starts, and this reader never assumes they agree
 * with the central directory's), bounds-checks every offset against `len`,
 * and rejects (false, with a human-readable message in errbuf) any of:
 * total input over a few MB (a `.sz` is config text; there's no legitimate
 * reason for one to be large, and this bounds worst-case scan work against
 * a hostile/corrupt input up front, before any parsing starts), a missing
 * or malformed EOCD record, a compression method other than 0 (STORE --
 * this reader never inflates DEFLATE or anything else), the encrypted or
 * trailing-data-descriptor general-purpose bits set, a compressed size
 * that doesn't equal the uncompressed size (only possible under STORE if
 * the two were tampered with independently), a name containing '/', '\\',
 * or a ".." component (never legitimate inside an archive this reader has
 * any reason to extract from -- see zip_reader_find()'s own callers, which
 * join `name` onto a path), a name duplicated across two entries, or an
 * entry whose actual bytes don't hash to its own recorded CRC-32. On any
 * failure, no entries are left allocated (zr is left zero-valued; callers
 * do not need to call zip_reader_free() after a failed open, though it's
 * harmless to do so anyway). */
bool zip_reader_open(const char *data, size_t len, ZipReader *zr, char *errbuf,
                      size_t errbuf_size);

/* Exact-match, case-sensitive lookup by arcname -- a linear search, fine
 * for the handful of entries a shimback archive ever has. NULL if absent. */
const ZipReaderEntry *zip_reader_find(const ZipReader *zr, const char *arcname);

/* Frees every entry's own name and the entries array itself. Safe to call
 * on a zero-valued ZipReader (e.g. after a failed zip_reader_open()), or
 * more than once. */
void zip_reader_free(ZipReader *zr);

#endif /* SHIMBACK_ZIP_H */
