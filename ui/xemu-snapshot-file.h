/*
 * xemu snapshot file format
 *
 * Copyright (C) 2026 xemu contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef XEMU_SNAPSHOT_FILE_H
#define XEMU_SNAPSHOT_FILE_H

#include "qemu/osdep.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A snapshot file is a fixed header followed by a sequence of sections. Each
 * section payload may be zlib-compressed and is protected by a CRC-32 of the
 * uncompressed data. See docs/external-snapshots.md.
 */

#define XEMU_SNAPSHOT_FILE_MAGIC "XEMUSNAP"
#define XEMU_SNAPSHOT_FILE_VERSION 1
#define XEMU_SNAPSHOT_FILE_EXT ".xsnap"
#define XEMU_SNAPSHOT_FILE_HEADER_SIZE 32
#define XEMU_SNAPSHOT_SECTION_HEADER_SIZE 32

#define XEMU_SNAPSHOT_FOURCC(a, b, c, d)                            \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
     ((uint32_t)(d) << 24))

#define XEMU_SNAPSHOT_SECTION_META XEMU_SNAPSHOT_FOURCC('M', 'E', 'T', 'A')
#define XEMU_SNAPSHOT_SECTION_THUMBNAIL XEMU_SNAPSHOT_FOURCC('T', 'H', 'M', 'B')
#define XEMU_SNAPSHOT_SECTION_DISK XEMU_SNAPSHOT_FOURCC('D', 'I', 'S', 'K')
#define XEMU_SNAPSHOT_SECTION_VMSTATE XEMU_SNAPSHOT_FOURCC('V', 'M', 'S', 'T')

#define XEMU_SNAPSHOT_SECTION_FLAG_ZLIB (1u << 0)

/* Tags of the META section records */
typedef enum XemuSnapshotMetaTag {
    XEMU_SNAPSHOT_META_NAME = 1, /* UTF-8 */
    XEMU_SNAPSHOT_META_DATE_SEC = 2, /* u64 */
    XEMU_SNAPSHOT_META_DATE_NSEC = 3, /* u32 */
    XEMU_SNAPSHOT_META_VM_CLOCK_NSEC = 4, /* u64 */
    XEMU_SNAPSHOT_META_XEMU_VERSION = 5, /* UTF-8 */
    XEMU_SNAPSHOT_META_XEMU_COMMIT = 6, /* UTF-8 */
    XEMU_SNAPSHOT_META_RAM_SIZE = 7, /* u64 */
    XEMU_SNAPSHOT_META_TITLE_ID = 8, /* u32 */
    XEMU_SNAPSHOT_META_TITLE_NAME = 9, /* UTF-8 */
    XEMU_SNAPSHOT_META_DISC_PATH = 10, /* UTF-8 */
    XEMU_SNAPSHOT_META_DISC_IDENTITY = 11, /* XemuDiscIdentity, serialized */
    XEMU_SNAPSHOT_META_HDD_PATH = 12, /* UTF-8 */
} XemuSnapshotMetaTag;

typedef struct XemuSnapshotSection {
    uint32_t type;
    uint32_t flags;
    uint64_t stored_size;
    uint64_t raw_size;
    uint32_t crc;
} XemuSnapshotSection;

typedef struct XemuSnapshotWriter XemuSnapshotWriter;
typedef struct XemuSnapshotReader XemuSnapshotReader;

/*
 * Writer. The file is created (or truncated) at @path. Sections are written
 * one at a time: begin, any number of writes, end.
 */
XemuSnapshotWriter *xemu_snapshot_writer_open(const char *path, bool compress,
                                              Error **errp);
bool xemu_snapshot_writer_begin_section(XemuSnapshotWriter *w, uint32_t type,
                                        Error **errp);
bool xemu_snapshot_writer_write(XemuSnapshotWriter *w, const void *buf,
                                size_t len, Error **errp);
bool xemu_snapshot_writer_end_section(XemuSnapshotWriter *w, Error **errp);
bool xemu_snapshot_writer_write_section(XemuSnapshotWriter *w, uint32_t type,
                                        const void *buf, size_t len,
                                        Error **errp);
/* Flushes and closes the file. The writer is freed in all cases. */
bool xemu_snapshot_writer_close(XemuSnapshotWriter *w, Error **errp);
/* Closes and deletes the file. */
void xemu_snapshot_writer_abort(XemuSnapshotWriter *w);

/*
 * Reader. xemu_snapshot_reader_next_section() moves to the next section
 * (skipping what is left of the current one) and returns false at the end of
 * the file or on error (@errp set).
 */
XemuSnapshotReader *xemu_snapshot_reader_open(const char *path, Error **errp);
bool xemu_snapshot_reader_next_section(XemuSnapshotReader *r,
                                       XemuSnapshotSection *section,
                                       Error **errp);
/* Reads exactly @len bytes of uncompressed payload. */
bool xemu_snapshot_reader_read(XemuSnapshotReader *r, void *buf, size_t len,
                               Error **errp);
/* Reads the rest of the current section and checks its CRC. */
bool xemu_snapshot_reader_finish_section(XemuSnapshotReader *r, Error **errp);
/* Reads the whole current section and checks its CRC. */
uint8_t *xemu_snapshot_reader_read_section(XemuSnapshotReader *r, size_t *len,
                                           Error **errp);
/* Moves to the first section of type @type. */
bool xemu_snapshot_reader_find_section(XemuSnapshotReader *r, uint32_t type,
                                       XemuSnapshotSection *section,
                                       Error **errp);
void xemu_snapshot_reader_close(XemuSnapshotReader *r);

/* Reads every section and checks the CRCs. */
bool xemu_snapshot_file_verify(const char *path, Error **errp);

/* META section records */
void xemu_snapshot_meta_put(GByteArray *meta, uint16_t tag, const void *buf,
                            uint32_t len);
void xemu_snapshot_meta_put_str(GByteArray *meta, uint16_t tag,
                                const char *str);
void xemu_snapshot_meta_put_u32(GByteArray *meta, uint16_t tag, uint32_t v);
void xemu_snapshot_meta_put_u64(GByteArray *meta, uint16_t tag, uint64_t v);
/* Returns a pointer into @meta, or NULL if @tag is absent or malformed. */
const uint8_t *xemu_snapshot_meta_get(const uint8_t *meta, size_t meta_len,
                                      uint16_t tag, uint32_t *len);
char *xemu_snapshot_meta_get_str(const uint8_t *meta, size_t meta_len,
                                 uint16_t tag);
bool xemu_snapshot_meta_get_u32(const uint8_t *meta, size_t meta_len,
                                uint16_t tag, uint32_t *v);
bool xemu_snapshot_meta_get_u64(const uint8_t *meta, size_t meta_len,
                                uint16_t tag, uint64_t *v);

#ifdef __cplusplus
}
#endif

#endif
