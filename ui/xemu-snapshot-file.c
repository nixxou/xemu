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

#include "xemu-snapshot-file.h"

#include <zlib.h>

#include "qapi/error.h"
#include "qemu/bswap.h"

#define XEMU_SNAPSHOT_IO_CHUNK (1024 * 1024)

/* Fast level: snapshot files are written while the VM is paused */
#define XEMU_SNAPSHOT_ZLIB_LEVEL 1

struct XemuSnapshotWriter {
    int fd;
    char *path;
    bool compress;

    bool in_section;
    int64_t section_offset;
    XemuSnapshotSection section;
    z_stream zs;
    uint8_t *out_buf;
};

struct XemuSnapshotReader {
    int fd;
    char *path;
    int64_t file_size;

    bool in_section;
    int64_t next_section_offset;
    XemuSnapshotSection section;
    uint64_t stored_left;
    uint64_t raw_left;
    uint32_t crc;
    z_stream zs;
    uint8_t *in_buf;
    size_t in_len;
    size_t in_pos;
};

static bool write_full(int fd, const void *buf, size_t len, const char *path,
                       Error **errp)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t ret = write(fd, p, len);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_setg_errno(errp, errno, "Failed to write '%s'", path);
            return false;
        }
        p += ret;
        len -= ret;
    }
    return true;
}

static bool pwrite_full(int fd, const void *buf, size_t len, int64_t offset,
                        const char *path, Error **errp)
{
    int64_t cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0 || lseek(fd, offset, SEEK_SET) < 0) {
        error_setg_errno(errp, errno, "Failed to seek in '%s'", path);
        return false;
    }
    if (!write_full(fd, buf, len, path, errp)) {
        return false;
    }
    if (lseek(fd, cur, SEEK_SET) < 0) {
        error_setg_errno(errp, errno, "Failed to seek in '%s'", path);
        return false;
    }
    return true;
}

/* Returns the number of bytes read (< len at end of file), or -1 */
static ssize_t read_full(int fd, void *buf, size_t len, const char *path,
                         Error **errp)
{
    uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t ret = read(fd, p + done, len - done);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_setg_errno(errp, errno, "Failed to read '%s'", path);
            return -1;
        }
        if (ret == 0) {
            break;
        }
        done += ret;
    }
    return done;
}

static void encode_section_header(uint8_t *buf, const XemuSnapshotSection *s)
{
    memset(buf, 0, XEMU_SNAPSHOT_SECTION_HEADER_SIZE);
    stl_le_p(buf + 0, s->type);
    stl_le_p(buf + 4, s->flags);
    stq_le_p(buf + 8, s->stored_size);
    stq_le_p(buf + 16, s->raw_size);
    stl_le_p(buf + 24, s->crc);
}

static void decode_section_header(const uint8_t *buf, XemuSnapshotSection *s)
{
    s->type = ldl_le_p(buf + 0);
    s->flags = ldl_le_p(buf + 4);
    s->stored_size = ldq_le_p(buf + 8);
    s->raw_size = ldq_le_p(buf + 16);
    s->crc = ldl_le_p(buf + 24);
}

XemuSnapshotWriter *xemu_snapshot_writer_open(const char *path, bool compress,
                                              Error **errp)
{
    int fd = qemu_create(path, O_WRONLY | O_TRUNC | O_BINARY, 0644, errp);
    if (fd < 0) {
        return NULL;
    }

    uint8_t header[XEMU_SNAPSHOT_FILE_HEADER_SIZE] = { 0 };
    memcpy(header, XEMU_SNAPSHOT_FILE_MAGIC, 8);
    stl_le_p(header + 8, XEMU_SNAPSHOT_FILE_VERSION);
    stl_le_p(header + 12, XEMU_SNAPSHOT_FILE_HEADER_SIZE);

    if (!write_full(fd, header, sizeof(header), path, errp)) {
        qemu_close(fd);
        qemu_unlink(path);
        return NULL;
    }

    XemuSnapshotWriter *w = g_new0(XemuSnapshotWriter, 1);
    w->fd = fd;
    w->path = g_strdup(path);
    w->compress = compress;
    w->out_buf = g_malloc(XEMU_SNAPSHOT_IO_CHUNK);
    return w;
}

bool xemu_snapshot_writer_begin_section(XemuSnapshotWriter *w, uint32_t type,
                                        Error **errp)
{
    assert(!w->in_section);

    w->section_offset = lseek(w->fd, 0, SEEK_CUR);
    if (w->section_offset < 0) {
        error_setg_errno(errp, errno, "Failed to seek in '%s'", w->path);
        return false;
    }

    memset(&w->section, 0, sizeof(w->section));
    w->section.type = type;
    w->section.flags = w->compress ? XEMU_SNAPSHOT_SECTION_FLAG_ZLIB : 0;
    w->section.crc = crc32(0, Z_NULL, 0);

    /* Placeholder, rewritten by xemu_snapshot_writer_end_section() */
    uint8_t header[XEMU_SNAPSHOT_SECTION_HEADER_SIZE];
    encode_section_header(header, &w->section);
    if (!write_full(w->fd, header, sizeof(header), w->path, errp)) {
        return false;
    }

    if (w->compress) {
        memset(&w->zs, 0, sizeof(w->zs));
        if (deflateInit(&w->zs, XEMU_SNAPSHOT_ZLIB_LEVEL) != Z_OK) {
            error_setg(errp, "Failed to initialize zlib");
            return false;
        }
    }

    w->in_section = true;
    return true;
}

static bool writer_deflate(XemuSnapshotWriter *w, int flush, Error **errp)
{
    int ret;
    do {
        w->zs.next_out = w->out_buf;
        w->zs.avail_out = XEMU_SNAPSHOT_IO_CHUNK;
        ret = deflate(&w->zs, flush);
        if (ret == Z_STREAM_ERROR) {
            error_setg(errp, "zlib compression failed");
            return false;
        }
        size_t produced = XEMU_SNAPSHOT_IO_CHUNK - w->zs.avail_out;
        if (!write_full(w->fd, w->out_buf, produced, w->path, errp)) {
            return false;
        }
        w->section.stored_size += produced;
    } while (w->zs.avail_out == 0 ||
             (flush == Z_FINISH && ret != Z_STREAM_END));
    return true;
}

bool xemu_snapshot_writer_write(XemuSnapshotWriter *w, const void *buf,
                                size_t len, Error **errp)
{
    assert(w->in_section);

    const uint8_t *p = buf;
    w->section.raw_size += len;

    while (len) {
        /* zlib takes 32-bit lengths */
        size_t n = MIN(len, XEMU_SNAPSHOT_IO_CHUNK);
        w->section.crc = crc32(w->section.crc, p, n);

        if (w->compress) {
            w->zs.next_in = (Bytef *)p;
            w->zs.avail_in = n;
            if (!writer_deflate(w, Z_NO_FLUSH, errp)) {
                return false;
            }
            assert(w->zs.avail_in == 0);
        } else {
            if (!write_full(w->fd, p, n, w->path, errp)) {
                return false;
            }
            w->section.stored_size += n;
        }

        p += n;
        len -= n;
    }

    return true;
}

bool xemu_snapshot_writer_end_section(XemuSnapshotWriter *w, Error **errp)
{
    assert(w->in_section);
    w->in_section = false;

    if (w->compress) {
        w->zs.next_in = NULL;
        w->zs.avail_in = 0;
        bool ok = writer_deflate(w, Z_FINISH, errp);
        deflateEnd(&w->zs);
        if (!ok) {
            return false;
        }
    }

    uint8_t header[XEMU_SNAPSHOT_SECTION_HEADER_SIZE];
    encode_section_header(header, &w->section);
    return pwrite_full(w->fd, header, sizeof(header), w->section_offset,
                       w->path, errp);
}

bool xemu_snapshot_writer_write_section(XemuSnapshotWriter *w, uint32_t type,
                                        const void *buf, size_t len,
                                        Error **errp)
{
    return xemu_snapshot_writer_begin_section(w, type, errp) &&
           xemu_snapshot_writer_write(w, buf, len, errp) &&
           xemu_snapshot_writer_end_section(w, errp);
}

static void writer_free(XemuSnapshotWriter *w)
{
    if (w->in_section && w->compress) {
        deflateEnd(&w->zs);
    }
    g_free(w->out_buf);
    g_free(w->path);
    g_free(w);
}

bool xemu_snapshot_writer_close(XemuSnapshotWriter *w, Error **errp)
{
    assert(!w->in_section);

    bool ok = true;
    if (qemu_fdatasync(w->fd) < 0) {
        error_setg_errno(errp, errno, "Failed to flush '%s'", w->path);
        ok = false;
    }
    if (qemu_close(w->fd) < 0 && ok) {
        error_setg_errno(errp, errno, "Failed to close '%s'", w->path);
        ok = false;
    }
    writer_free(w);
    return ok;
}

void xemu_snapshot_writer_abort(XemuSnapshotWriter *w)
{
    qemu_close(w->fd);
    qemu_unlink(w->path);
    writer_free(w);
}

XemuSnapshotReader *xemu_snapshot_reader_open(const char *path, Error **errp)
{
    int fd = qemu_open(path, O_RDONLY | O_BINARY, errp);
    if (fd < 0) {
        return NULL;
    }

    uint8_t header[XEMU_SNAPSHOT_FILE_HEADER_SIZE];
    ssize_t ret = read_full(fd, header, sizeof(header), path, errp);
    if (ret < 0) {
        goto fail;
    }
    if (ret != sizeof(header) ||
        memcmp(header, XEMU_SNAPSHOT_FILE_MAGIC, 8) != 0) {
        error_setg(errp, "'%s' is not an xemu snapshot file", path);
        goto fail;
    }

    uint32_t version = ldl_le_p(header + 8);
    uint32_t header_size = ldl_le_p(header + 12);
    if (version != XEMU_SNAPSHOT_FILE_VERSION) {
        error_setg(errp,
                   "'%s' uses snapshot file format version %" PRIu32
                   ", this build of xemu supports version %d",
                   path, version, XEMU_SNAPSHOT_FILE_VERSION);
        goto fail;
    }
    if (header_size < XEMU_SNAPSHOT_FILE_HEADER_SIZE) {
        error_setg(errp, "'%s' has an invalid header", path);
        goto fail;
    }

    int64_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size < 0) {
        error_setg_errno(errp, errno, "Failed to seek in '%s'", path);
        goto fail;
    }

    XemuSnapshotReader *r = g_new0(XemuSnapshotReader, 1);
    r->fd = fd;
    r->path = g_strdup(path);
    r->file_size = file_size;
    r->next_section_offset = header_size;
    r->in_buf = g_malloc(XEMU_SNAPSHOT_IO_CHUNK);
    return r;

fail:
    qemu_close(fd);
    return NULL;
}

static void reader_end_section(XemuSnapshotReader *r)
{
    if (r->in_section && (r->section.flags & XEMU_SNAPSHOT_SECTION_FLAG_ZLIB)) {
        inflateEnd(&r->zs);
    }
    r->in_section = false;
}

bool xemu_snapshot_reader_next_section(XemuSnapshotReader *r,
                                       XemuSnapshotSection *section,
                                       Error **errp)
{
    reader_end_section(r);

    if (r->next_section_offset == r->file_size) {
        return false;
    }

    uint8_t header[XEMU_SNAPSHOT_SECTION_HEADER_SIZE];
    if (lseek(r->fd, r->next_section_offset, SEEK_SET) < 0) {
        error_setg_errno(errp, errno, "Failed to seek in '%s'", r->path);
        return false;
    }
    ssize_t ret = read_full(r->fd, header, sizeof(header), r->path, errp);
    if (ret < 0) {
        return false;
    }
    if (ret != sizeof(header)) {
        error_setg(errp, "'%s' is truncated", r->path);
        return false;
    }

    decode_section_header(header, &r->section);
    int64_t payload_offset = r->next_section_offset + sizeof(header);
    if (r->section.stored_size > r->file_size - payload_offset) {
        error_setg(errp, "'%s' is truncated", r->path);
        return false;
    }
    if (!(r->section.flags & XEMU_SNAPSHOT_SECTION_FLAG_ZLIB) &&
        r->section.stored_size != r->section.raw_size) {
        error_setg(errp, "'%s' is corrupted", r->path);
        return false;
    }

    r->next_section_offset = payload_offset + r->section.stored_size;
    r->stored_left = r->section.stored_size;
    r->raw_left = r->section.raw_size;
    r->crc = crc32(0, Z_NULL, 0);
    r->in_len = r->in_pos = 0;

    if (r->section.flags & XEMU_SNAPSHOT_SECTION_FLAG_ZLIB) {
        memset(&r->zs, 0, sizeof(r->zs));
        if (inflateInit(&r->zs) != Z_OK) {
            error_setg(errp, "Failed to initialize zlib");
            return false;
        }
    }

    r->in_section = true;
    if (section) {
        *section = r->section;
    }
    return true;
}

static bool reader_fill(XemuSnapshotReader *r, Error **errp)
{
    size_t n = MIN(r->stored_left, XEMU_SNAPSHOT_IO_CHUNK);
    ssize_t ret = read_full(r->fd, r->in_buf, n, r->path, errp);
    if (ret < 0) {
        return false;
    }
    if (ret != n) {
        error_setg(errp, "'%s' is truncated", r->path);
        return false;
    }
    r->stored_left -= n;
    r->in_len = n;
    r->in_pos = 0;
    return true;
}

bool xemu_snapshot_reader_read(XemuSnapshotReader *r, void *buf, size_t len,
                               Error **errp)
{
    assert(r->in_section);

    if (len > r->raw_left) {
        error_setg(errp, "'%s' is corrupted: section is too short", r->path);
        return false;
    }

    uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        if (r->in_pos == r->in_len) {
            if (!r->stored_left) {
                error_setg(errp, "'%s' is corrupted", r->path);
                return false;
            }
            if (!reader_fill(r, errp)) {
                return false;
            }
        }

        size_t n;
        if (r->section.flags & XEMU_SNAPSHOT_SECTION_FLAG_ZLIB) {
            r->zs.next_in = r->in_buf + r->in_pos;
            r->zs.avail_in = r->in_len - r->in_pos;
            r->zs.next_out = p + done;
            r->zs.avail_out = MIN(len - done, XEMU_SNAPSHOT_IO_CHUNK);
            uInt avail_out = r->zs.avail_out;
            int ret = inflate(&r->zs, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
                error_setg(errp, "'%s' is corrupted: %s", r->path,
                           r->zs.msg ? r->zs.msg : "zlib error");
                return false;
            }
            r->in_pos = r->in_len - r->zs.avail_in;
            n = avail_out - r->zs.avail_out;
            if (ret == Z_STREAM_END && done + n < len) {
                error_setg(errp, "'%s' is corrupted", r->path);
                return false;
            }
        } else {
            n = MIN(len - done, r->in_len - r->in_pos);
            memcpy(p + done, r->in_buf + r->in_pos, n);
            r->in_pos += n;
        }

        r->crc = crc32(r->crc, p + done, n);
        done += n;
    }

    r->raw_left -= len;
    return true;
}

bool xemu_snapshot_reader_finish_section(XemuSnapshotReader *r, Error **errp)
{
    assert(r->in_section);

    uint8_t *buf = g_malloc(XEMU_SNAPSHOT_IO_CHUNK);
    bool ok = true;
    while (r->raw_left && ok) {
        ok = xemu_snapshot_reader_read(
            r, buf, MIN(r->raw_left, XEMU_SNAPSHOT_IO_CHUNK), errp);
    }
    g_free(buf);

    if (ok && r->crc != r->section.crc) {
        error_setg(errp, "'%s' is corrupted: checksum mismatch", r->path);
        ok = false;
    }
    reader_end_section(r);
    return ok;
}

uint8_t *xemu_snapshot_reader_read_section(XemuSnapshotReader *r, size_t *len,
                                           Error **errp)
{
    assert(r->in_section);

    if (r->raw_left > SIZE_MAX - 1) {
        error_setg(errp, "'%s': section is too large", r->path);
        return NULL;
    }

    size_t size = r->raw_left;
    uint8_t *buf = g_try_malloc(size + 1);
    if (!buf) {
        error_setg(errp, "Out of memory reading '%s'", r->path);
        return NULL;
    }
    if (!xemu_snapshot_reader_read(r, buf, size, errp) ||
        !xemu_snapshot_reader_finish_section(r, errp)) {
        g_free(buf);
        return NULL;
    }

    buf[size] = 0;
    *len = size;
    return buf;
}

bool xemu_snapshot_reader_find_section(XemuSnapshotReader *r, uint32_t type,
                                       XemuSnapshotSection *section,
                                       Error **errp)
{
    XemuSnapshotSection s;
    while (xemu_snapshot_reader_next_section(r, &s, errp)) {
        if (s.type == type) {
            if (section) {
                *section = s;
            }
            return true;
        }
    }
    return false;
}

void xemu_snapshot_reader_close(XemuSnapshotReader *r)
{
    reader_end_section(r);
    qemu_close(r->fd);
    g_free(r->in_buf);
    g_free(r->path);
    g_free(r);
}

bool xemu_snapshot_file_verify(const char *path, Error **errp)
{
    XemuSnapshotReader *r = xemu_snapshot_reader_open(path, errp);
    if (!r) {
        return false;
    }

    Error *local_err = NULL;
    bool ok = true;
    while (xemu_snapshot_reader_next_section(r, NULL, &local_err)) {
        if (!xemu_snapshot_reader_finish_section(r, &local_err)) {
            break;
        }
    }
    if (local_err) {
        error_propagate(errp, local_err);
        ok = false;
    }

    xemu_snapshot_reader_close(r);
    return ok;
}

void xemu_snapshot_meta_put(GByteArray *meta, uint16_t tag, const void *buf,
                            uint32_t len)
{
    uint8_t header[6];
    stw_le_p(header, tag);
    stl_le_p(header + 2, len);
    g_byte_array_append(meta, header, sizeof(header));
    if (len) {
        g_byte_array_append(meta, buf, len);
    }
}

void xemu_snapshot_meta_put_str(GByteArray *meta, uint16_t tag, const char *str)
{
    if (str) {
        xemu_snapshot_meta_put(meta, tag, str, strlen(str));
    }
}

void xemu_snapshot_meta_put_u32(GByteArray *meta, uint16_t tag, uint32_t v)
{
    uint8_t buf[4];
    stl_le_p(buf, v);
    xemu_snapshot_meta_put(meta, tag, buf, sizeof(buf));
}

void xemu_snapshot_meta_put_u64(GByteArray *meta, uint16_t tag, uint64_t v)
{
    uint8_t buf[8];
    stq_le_p(buf, v);
    xemu_snapshot_meta_put(meta, tag, buf, sizeof(buf));
}

const uint8_t *xemu_snapshot_meta_get(const uint8_t *meta, size_t meta_len,
                                      uint16_t tag, uint32_t *len)
{
    size_t offset = 0;
    while (meta_len - offset >= 6) {
        uint16_t t = lduw_le_p(meta + offset);
        uint32_t l = ldl_le_p(meta + offset + 2);
        offset += 6;
        if (l > meta_len - offset) {
            return NULL;
        }
        if (t == tag) {
            *len = l;
            return meta + offset;
        }
        offset += l;
    }
    return NULL;
}

char *xemu_snapshot_meta_get_str(const uint8_t *meta, size_t meta_len,
                                 uint16_t tag)
{
    uint32_t len;
    const uint8_t *p = xemu_snapshot_meta_get(meta, meta_len, tag, &len);
    return p ? g_strndup((const char *)p, len) : NULL;
}

bool xemu_snapshot_meta_get_u32(const uint8_t *meta, size_t meta_len,
                                uint16_t tag, uint32_t *v)
{
    uint32_t len;
    const uint8_t *p = xemu_snapshot_meta_get(meta, meta_len, tag, &len);
    if (!p || len != 4) {
        return false;
    }
    *v = ldl_le_p(p);
    return true;
}

bool xemu_snapshot_meta_get_u64(const uint8_t *meta, size_t meta_len,
                                uint16_t tag, uint64_t *v)
{
    uint32_t len;
    const uint8_t *p = xemu_snapshot_meta_get(meta, meta_len, tag, &len);
    if (!p || len != 8) {
        return false;
    }
    *v = ldq_le_p(p);
    return true;
}
