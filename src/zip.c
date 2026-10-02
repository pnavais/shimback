#include "zip.h"

#include <stdint.h>
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
