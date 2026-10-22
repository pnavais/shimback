#include "zip.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform/platform.h"

/* One already-written local file header's worth of bookkeeping, needed
 * again when the central directory is appended at zip_writer_finish()
 * time -- ZIP's central directory is a second, trailing index over
 * everything already written, not something that can be emitted
 * incrementally alongside each file. */
struct ZipCentralEntry {
    char *arcname;    /* owned */
    uint32_t crc;
    uint32_t size;
    uint32_t offset;  /* local header's own offset within the archive */
    uint16_t dos_time;
    uint16_t dos_date;
};

/* CRC-32 (IEEE 802.3, ZIP's required checksum) -- the standard
 * reflected/reversed polynomial 0xEDB88320, table-based. Lazily built
 * once; this project's CLI commands are single-threaded throughout (see
 * zip.h's own top comment), so no locking is needed around the one-time
 * build. */
static uint32_t crc32_table[256];
static bool crc32_table_ready = false;

static void crc32_build_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_ready = true;
}

static uint32_t crc32_bytes(const unsigned char *data, size_t len) {
    if (!crc32_table_ready) {
        crc32_build_table();
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ZIP fields are always little-endian, regardless of the host's own byte
 * order -- appended byte-by-byte rather than via a cast + memcpy of a
 * native uint16_t/uint32_t, which would be wrong on a big-endian host
 * (none this project targets today, but this is free correctness, not
 * speculative complexity). */
static void append_u16(DynBuf *buf, uint16_t v) {
    char b[2] = {(char)(v & 0xFFu), (char)((v >> 8) & 0xFFu)};
    dynbuf_append(buf, b, 2);
}

static void append_u32(DynBuf *buf, uint32_t v) {
    char b[4] = {(char)(v & 0xFFu), (char)((v >> 8) & 0xFFu), (char)((v >> 16) & 0xFFu),
                 (char)((v >> 24) & 0xFFu)};
    dynbuf_append(buf, b, 4);
}

/* ZIP's required MS-DOS date/time encoding, derived from the local time
 * at the moment each entry is added. Not meaningful before 1980 (DOS
 * date's epoch) or after 2107 (its 7-bit year field overflows) -- neither
 * bound is reachable by a real clock in this project's lifetime, so no
 * clamping/validation is done. */
static void dos_datetime_now(uint16_t *out_time, uint16_t *out_date) {
    time_t now = time(NULL);
    struct tm tmv = *plat_localtime(&now); /* copied out immediately; plat_localtime()'s static
                                        * buffer is fine here since this project's CLI
                                        * commands are single-threaded throughout. */
    *out_time = (uint16_t)((tmv.tm_hour << 11) | (tmv.tm_min << 5) | (tmv.tm_sec / 2));
    *out_date = (uint16_t)(((tmv.tm_year - 80) << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
}

void zip_writer_init(ZipWriter *zw) {
    dynbuf_init(&zw->buf);
    zw->entries = NULL;
    zw->count = 0;
    zw->cap = 0;
}

void zip_writer_add_file(ZipWriter *zw, const char *arcname, const char *data, size_t len) {
    uint16_t dos_time, dos_date;
    dos_datetime_now(&dos_time, &dos_date);
    uint32_t crc = crc32_bytes((const unsigned char *)data, len);
    uint32_t offset = (uint32_t)zw->buf.len;
    uint16_t name_len = (uint16_t)strlen(arcname);

    append_u32(&zw->buf, 0x04034b50u); /* local file header signature */
    append_u16(&zw->buf, 20);           /* version needed to extract */
    append_u16(&zw->buf, 0);            /* general purpose bit flag */
    append_u16(&zw->buf, 0);            /* compression method: stored */
    append_u16(&zw->buf, dos_time);
    append_u16(&zw->buf, dos_date);
    append_u32(&zw->buf, crc);
    append_u32(&zw->buf, (uint32_t)len); /* compressed size == uncompressed (stored) */
    append_u32(&zw->buf, (uint32_t)len);
    append_u16(&zw->buf, name_len);
    append_u16(&zw->buf, 0); /* extra field length */
    dynbuf_append(&zw->buf, arcname, name_len);
    if (len > 0) {
        dynbuf_append(&zw->buf, data, len);
    }

    if (zw->count == zw->cap) {
        zw->cap = zw->cap == 0 ? 8 : zw->cap * 2;
        zw->entries = xrealloc(zw->entries, zw->cap * sizeof(*zw->entries));
    }
    struct ZipCentralEntry *e = &zw->entries[zw->count++];
    e->arcname = xstrdup(arcname);
    e->crc = crc;
    e->size = (uint32_t)len;
    e->offset = offset;
    e->dos_time = dos_time;
    e->dos_date = dos_date;
}

void zip_writer_finish(ZipWriter *zw) {
    uint32_t central_start = (uint32_t)zw->buf.len;

    for (size_t i = 0; i < zw->count; i++) {
        struct ZipCentralEntry *e = &zw->entries[i];
        uint16_t name_len = (uint16_t)strlen(e->arcname);

        append_u32(&zw->buf, 0x02014b50u); /* central file header signature */
        append_u16(&zw->buf, 20);           /* version made by */
        append_u16(&zw->buf, 20);           /* version needed to extract */
        append_u16(&zw->buf, 0);            /* general purpose bit flag */
        append_u16(&zw->buf, 0);            /* compression method: stored */
        append_u16(&zw->buf, e->dos_time);
        append_u16(&zw->buf, e->dos_date);
        append_u32(&zw->buf, e->crc);
        append_u32(&zw->buf, e->size);
        append_u32(&zw->buf, e->size);
        append_u16(&zw->buf, name_len);
        append_u16(&zw->buf, 0); /* extra field length */
        append_u16(&zw->buf, 0); /* file comment length */
        append_u16(&zw->buf, 0); /* disk number start */
        append_u16(&zw->buf, 0); /* internal file attributes */
        append_u32(&zw->buf, 0); /* external file attributes */
        append_u32(&zw->buf, e->offset);
        dynbuf_append(&zw->buf, e->arcname, name_len);
    }

    uint32_t central_size = (uint32_t)zw->buf.len - central_start;

    append_u32(&zw->buf, 0x06054b50u); /* end of central directory signature */
    append_u16(&zw->buf, 0);           /* number of this disk */
    append_u16(&zw->buf, 0);           /* disk where central directory starts */
    append_u16(&zw->buf, (uint16_t)zw->count); /* central dir records, this disk */
    append_u16(&zw->buf, (uint16_t)zw->count); /* central dir records, total */
    append_u32(&zw->buf, central_size);
    append_u32(&zw->buf, central_start);
    append_u16(&zw->buf, 0); /* comment length */

    for (size_t i = 0; i < zw->count; i++) {
        free(zw->entries[i].arcname);
    }
    free(zw->entries);
    zw->entries = NULL;
    zw->count = 0;
    zw->cap = 0;
}

void zip_writer_free(ZipWriter *zw) {
    for (size_t i = 0; i < zw->count; i++) {
        free(zw->entries[i].arcname);
    }
    free(zw->entries);
    zw->entries = NULL;
    zw->count = 0;
    zw->cap = 0;
    dynbuf_free(&zw->buf);
}

/* ---- Reader ---- */

/* A `.sz` is config text -- a few MB is already absurdly generous for one.
 * Rejecting anything bigger up front bounds the EOCD backward-scan's and
 * central-directory walk's worst-case work against a hostile/corrupt input
 * before any parsing even starts. */
#define ZIP_READER_MAX_SIZE (16u * 1024 * 1024)

#define EOCD_SIG 0x06054b50u
#define CENTRAL_SIG 0x02014b50u
#define LOCAL_SIG 0x04034b50u
#define EOCD_FIXED_SIZE 22
#define CENTRAL_FIXED_SIZE 46
#define LOCAL_FIXED_SIZE 30
/* The EOCD record's own "comment length" field is 16 bits, so a comment
 * (and therefore the gap between the true EOCD signature and the end of
 * the buffer) can never exceed this -- the backward scan only needs to
 * search that far. */
#define EOCD_MAX_COMMENT 65535u

static uint16_t read_u16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* True if `name` (length `len`, NOT NUL-terminated -- straight out of the
 * archive, not yet copied) is safe to ever join onto a filesystem path:
 * no '/' or '\\' component separator, and no ".." component. Entries this
 * reader hands back are always flat, single-component names in practice
 * (shimback's own writer never produces anything else), but this is
 * enforced defensively rather than assumed, since a caller (import.c)
 * does join a returned name onto a directory path. */
static bool name_is_safe(const char *name, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '/' || name[i] == '\\') {
            return false;
        }
    }
    /* No separator found above, so this is a single path component -- the
     * only shape ".." could matter in is being the *whole* name. */
    return !(len == 2 && name[0] == '.' && name[1] == '.');
}

static void zip_reader_zero(ZipReader *zr) {
    zr->entries = NULL;
    zr->count = 0;
}

static void zip_reader_free_entries(ZipReader *zr) {
    for (size_t i = 0; i < zr->count; i++) {
        free(zr->entries[i].name);
    }
    free(zr->entries);
    zip_reader_zero(zr);
}

bool zip_reader_open(const char *data, size_t len, ZipReader *zr, char *errbuf,
                      size_t errbuf_size) {
    zip_reader_zero(zr);

    if (len > ZIP_READER_MAX_SIZE) {
        snprintf(errbuf, errbuf_size, "archive is too large (%zu bytes, max %u)", len,
                 ZIP_READER_MAX_SIZE);
        return false;
    }
    if (len < EOCD_FIXED_SIZE) {
        snprintf(errbuf, errbuf_size, "too small to be a zip archive");
        return false;
    }

    const unsigned char *bytes = (const unsigned char *)data;

    /* Scan backward for the EOCD signature, verifying at each candidate
     * that its own "comment length" field exactly reaches the end of the
     * buffer -- the standard technique for telling a genuine EOCD record
     * apart from the same four magic bytes occurring by coincidence
     * earlier in some entry's own data. */
    size_t search_floor = (len > EOCD_FIXED_SIZE + EOCD_MAX_COMMENT)
                               ? len - EOCD_FIXED_SIZE - EOCD_MAX_COMMENT
                               : 0;
    size_t eocd_off = (size_t)-1;
    for (size_t off = len - EOCD_FIXED_SIZE;; off--) {
        if (read_u32(bytes + off) == EOCD_SIG) {
            uint16_t comment_len = read_u16(bytes + off + 20);
            if (off + EOCD_FIXED_SIZE + comment_len == len) {
                eocd_off = off;
                break;
            }
        }
        if (off == search_floor) {
            break;
        }
    }
    if (eocd_off == (size_t)-1) {
        snprintf(errbuf, errbuf_size,
                 "no end-of-central-directory record found -- not a zip file, or truncated");
        return false;
    }

    uint16_t total_entries = read_u16(bytes + eocd_off + 10);
    uint32_t central_size = read_u32(bytes + eocd_off + 12);
    uint32_t central_offset = read_u32(bytes + eocd_off + 16);

    if ((uint64_t)central_offset + central_size > eocd_off) {
        snprintf(errbuf, errbuf_size, "central directory doesn't fit before the end-of-central-"
                                       "directory record -- malformed archive");
        return false;
    }

    ZipReaderEntry *out = total_entries > 0 ? xmalloc(total_entries * sizeof(ZipReaderEntry)) : NULL;
    size_t out_count = 0;
    bool ok = true;
    size_t pos = central_offset;
    size_t central_end = central_offset + central_size;

    for (uint16_t i = 0; i < total_entries && ok; i++) {
        if (pos + CENTRAL_FIXED_SIZE > central_end || pos + CENTRAL_FIXED_SIZE > len) {
            snprintf(errbuf, errbuf_size, "central directory entry %u: truncated", i);
            ok = false;
            break;
        }
        if (read_u32(bytes + pos) != CENTRAL_SIG) {
            snprintf(errbuf, errbuf_size, "central directory entry %u: bad signature", i);
            ok = false;
            break;
        }

        uint16_t gp_flags = read_u16(bytes + pos + 8);
        uint16_t method = read_u16(bytes + pos + 10);
        uint32_t crc = read_u32(bytes + pos + 16);
        uint32_t compressed_size = read_u32(bytes + pos + 20);
        uint32_t uncompressed_size = read_u32(bytes + pos + 24);
        uint16_t name_len = read_u16(bytes + pos + 28);
        uint16_t extra_len = read_u16(bytes + pos + 30);
        uint16_t comment_len = read_u16(bytes + pos + 32);
        uint32_t local_offset = read_u32(bytes + pos + 42);

        size_t name_pos = pos + CENTRAL_FIXED_SIZE;
        size_t entry_end = name_pos + name_len + extra_len + comment_len;
        if (entry_end > central_end || entry_end > len) {
            snprintf(errbuf, errbuf_size, "central directory entry %u: truncated name/extra/"
                                           "comment field",
                     i);
            ok = false;
            break;
        }

        /* bit 0: encrypted. bit 3: trailing data descriptor (streamed
         * output -- sizes/CRC live after the data instead of in this
         * header), neither of which this reader supports. */
        if ((gp_flags & 0x0001u) != 0 || (gp_flags & 0x0008u) != 0) {
            snprintf(errbuf, errbuf_size,
                     "entry %u uses an unsupported zip feature (encryption or a streamed "
                     "data descriptor)",
                     i);
            ok = false;
            break;
        }
        if (method != 0) {
            snprintf(errbuf, errbuf_size,
                     "entry %u is compressed (method %u) -- only uncompressed (STORE) entries "
                     "are supported; this doesn't look like an archive shimback export wrote",
                     i, method);
            ok = false;
            break;
        }
        if (compressed_size != uncompressed_size) {
            snprintf(errbuf, errbuf_size, "entry %u: compressed size doesn't match uncompressed "
                                           "size for a STORE entry -- malformed archive",
                     i);
            ok = false;
            break;
        }
        if (!name_is_safe((const char *)bytes + name_pos, name_len)) {
            snprintf(errbuf, errbuf_size, "entry %u has an unsafe name", i);
            ok = false;
            break;
        }
        for (size_t j = 0; j < out_count; j++) {
            if (strlen(out[j].name) == name_len &&
                memcmp(out[j].name, bytes + name_pos, name_len) == 0) {
                snprintf(errbuf, errbuf_size, "duplicate entry name in archive");
                ok = false;
                break;
            }
        }
        if (!ok) {
            break;
        }

        /* Locate the entry's data via its own *local* file header -- its
         * name/extra-field lengths are authoritative for this, not the
         * central directory's own copy of them (see zip_reader_open's own
         * doc comment). */
        if ((uint64_t)local_offset + LOCAL_FIXED_SIZE > len) {
            snprintf(errbuf, errbuf_size, "entry %u: local file header out of range", i);
            ok = false;
            break;
        }
        if (read_u32(bytes + local_offset) != LOCAL_SIG) {
            snprintf(errbuf, errbuf_size, "entry %u: local file header has a bad signature", i);
            ok = false;
            break;
        }
        uint16_t local_name_len = read_u16(bytes + local_offset + 26);
        uint16_t local_extra_len = read_u16(bytes + local_offset + 28);
        uint64_t data_start =
            (uint64_t)local_offset + LOCAL_FIXED_SIZE + local_name_len + local_extra_len;
        uint64_t data_end = data_start + uncompressed_size;
        if (data_end > len) {
            snprintf(errbuf, errbuf_size, "entry %u: data extends past the end of the archive", i);
            ok = false;
            break;
        }

        const char *entry_data = data + data_start;
        uint32_t actual_crc = crc32_bytes((const unsigned char *)entry_data, uncompressed_size);
        if (actual_crc != crc) {
            snprintf(errbuf, errbuf_size, "entry %u failed its CRC-32 check -- corrupt archive",
                     i);
            ok = false;
            break;
        }

        out[out_count].name = xstrndup((const char *)bytes + name_pos, name_len);
        out[out_count].data = entry_data;
        out[out_count].size = uncompressed_size;
        out_count++;

        pos = entry_end;
    }

    if (!ok) {
        for (size_t j = 0; j < out_count; j++) {
            free(out[j].name);
        }
        free(out);
        return false;
    }

    zr->entries = out;
    zr->count = out_count;
    return true;
}

const ZipReaderEntry *zip_reader_find(const ZipReader *zr, const char *arcname) {
    for (size_t i = 0; i < zr->count; i++) {
        if (strcmp(zr->entries[i].name, arcname) == 0) {
            return &zr->entries[i];
        }
    }
    return NULL;
}

void zip_reader_free(ZipReader *zr) {
    zip_reader_free_entries(zr);
}
