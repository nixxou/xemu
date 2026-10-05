/*
 * xemu disc identity
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

#include "xemu-disc-identity.h"
#include "xemu-xbe.h"

#include "qemu/bswap.h"

#define XDVDFS_SECTOR_SIZE 2048
#define XDVDFS_VOLUME_DESCRIPTOR_SECTOR 32
#define XDVDFS_MAGIC "MICROSOFT*XBOX*MEDIA"
#define XDVDFS_MAGIC_LEN 20
#define XDVDFS_DIRENT_HEADER_SIZE 14
#define XDVDFS_MAX_DIR_SIZE (16 * 1024 * 1024)

#define XBE_MAGIC 0x48454258 /* "XBEH" */
#define XBE_MAX_HEADERS_SIZE (1024 * 1024)

#define XEMU_DISC_IDENTITY_FORMAT 1

/*
 * Offset of the game partition for images that also contain the video
 * partition (XGD1, XGD2, XGD3). Plain XISO images start with the game
 * partition.
 */
static const uint64_t xdvdfs_partition_offsets[] = {
    0,
    0x18300000,
    0x0FD90000,
    0x02080000,
};

static bool find_default_xbe(XemuDiscReadFunc read, void *opaque,
                             uint64_t disc_size, uint64_t partition,
                             uint64_t *xbe_offset, uint32_t *xbe_size)
{
    uint8_t vd[XDVDFS_SECTOR_SIZE];
    uint64_t vd_offset =
        partition + XDVDFS_VOLUME_DESCRIPTOR_SECTOR * XDVDFS_SECTOR_SIZE;

    if (vd_offset + sizeof(vd) > disc_size ||
        !read(opaque, vd_offset, vd, sizeof(vd)) ||
        memcmp(vd, XDVDFS_MAGIC, XDVDFS_MAGIC_LEN) != 0 ||
        memcmp(vd + sizeof(vd) - XDVDFS_MAGIC_LEN, XDVDFS_MAGIC,
               XDVDFS_MAGIC_LEN) != 0) {
        return false;
    }

    uint64_t root_offset =
        partition + (uint64_t)ldl_le_p(vd + 20) * XDVDFS_SECTOR_SIZE;
    uint32_t root_size = ldl_le_p(vd + 24);
    if (root_size < XDVDFS_DIRENT_HEADER_SIZE ||
        root_size > XDVDFS_MAX_DIR_SIZE || root_offset > disc_size ||
        root_size > disc_size - root_offset) {
        return false;
    }

    g_autofree uint8_t *dir = g_malloc(root_size);
    if (!read(opaque, root_offset, dir, root_size)) {
        return false;
    }

    /*
     * The directory is a binary tree of entries; walk all of it rather than
     * relying on the sort order. Each entry is visited at most once.
     */
    uint32_t max_entries = root_size / XDVDFS_DIRENT_HEADER_SIZE + 1;
    g_autofree uint32_t *stack = g_new(uint32_t, max_entries);
    g_autofree uint8_t *visited = g_malloc0(root_size / 4 + 1);
    uint32_t sp = 0;
    stack[sp++] = 0;

    while (sp) {
        uint32_t offset = stack[--sp];
        if (offset > root_size - XDVDFS_DIRENT_HEADER_SIZE ||
            visited[offset / 4]) {
            continue;
        }
        visited[offset / 4] = 1;

        const uint8_t *e = dir + offset;
        uint16_t left = lduw_le_p(e + 0);
        uint16_t right = lduw_le_p(e + 2);
        if (left == 0xffff && right == 0xffff) {
            continue; /* Padding */
        }

        uint32_t sector = ldl_le_p(e + 4);
        uint32_t size = ldl_le_p(e + 8);
        uint8_t attributes = e[12];
        uint8_t name_len = e[13];
        if (name_len > root_size - offset - XDVDFS_DIRENT_HEADER_SIZE) {
            continue;
        }

        const char *name = (const char *)e + XDVDFS_DIRENT_HEADER_SIZE;
        if (!(attributes & 0x10) && name_len == strlen("default.xbe") &&
            g_ascii_strncasecmp(name, "default.xbe", name_len) == 0) {
            *xbe_offset = partition + (uint64_t)sector * XDVDFS_SECTOR_SIZE;
            *xbe_size = size;
            return *xbe_offset < disc_size && size <= disc_size - *xbe_offset;
        }

        if (left && sp < max_entries) {
            stack[sp++] = (uint32_t)left * 4;
        }
        if (right && sp < max_entries) {
            stack[sp++] = (uint32_t)right * 4;
        }
    }

    return false;
}

static bool compute_xbe_identity(XemuDiscReadFunc read, void *opaque,
                                 uint64_t xbe_offset, uint32_t xbe_size,
                                 XemuDiscIdentity *id)
{
    struct xbe_header header;
    if (xbe_size < sizeof(header) ||
        !read(opaque, xbe_offset, &header, sizeof(header)) ||
        ldl_le_p(&header.m_magic) != XBE_MAGIC) {
        return false;
    }

    uint32_t base = ldl_le_p(&header.m_base);
    uint32_t headers_size = ldl_le_p(&header.m_sizeof_headers);
    uint32_t cert_addr = ldl_le_p(&header.m_certificate_addr);
    /* Older XBEs have a shorter certificate, only read up to m_version */
    uint64_t cert_end = (uint64_t)cert_addr - base +
                        offsetof(struct xbe_certificate, m_version) +
                        sizeof(uint32_t);
    if (headers_size < sizeof(header) || headers_size > XBE_MAX_HEADERS_SIZE ||
        headers_size > xbe_size || cert_addr < base ||
        cert_end > headers_size) {
        return false;
    }

    g_autofree uint8_t *headers = g_malloc(headers_size);
    if (!read(opaque, xbe_offset, headers, headers_size)) {
        return false;
    }

    const struct xbe_certificate *cert =
        (const struct xbe_certificate *)(headers + (cert_addr - base));
    id->title_id = ldl_le_p(&cert->m_titleid);
    id->version = ldl_le_p(&cert->m_version);
    id->disc_number = ldl_le_p(&cert->m_disk_number);

    gsize digest_len = sizeof(id->xbe_header_hash);
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, headers, headers_size);
    g_checksum_get_digest(checksum, id->xbe_header_hash, &digest_len);
    g_checksum_free(checksum);
    assert(digest_len == sizeof(id->xbe_header_hash));

    return true;
}

bool xemu_disc_identity_compute(XemuDiscReadFunc read, void *opaque,
                                uint64_t disc_size, XemuDiscIdentity *id)
{
    for (int i = 0; i < ARRAY_SIZE(xdvdfs_partition_offsets); i++) {
        uint64_t xbe_offset;
        uint32_t xbe_size;
        if (find_default_xbe(read, opaque, disc_size,
                             xdvdfs_partition_offsets[i], &xbe_offset,
                             &xbe_size)) {
            memset(id, 0, sizeof(*id));
            return compute_xbe_identity(read, opaque, xbe_offset, xbe_size, id);
        }
    }
    return false;
}

bool xemu_disc_identity_equal(const XemuDiscIdentity *a,
                              const XemuDiscIdentity *b)
{
    return a->title_id == b->title_id && a->version == b->version &&
           a->disc_number == b->disc_number &&
           !memcmp(a->xbe_header_hash, b->xbe_header_hash,
                   sizeof(a->xbe_header_hash));
}

void xemu_disc_identity_serialize(
    const XemuDiscIdentity *id, uint8_t buf[XEMU_DISC_IDENTITY_SERIALIZED_SIZE])
{
    stl_le_p(buf + 0, XEMU_DISC_IDENTITY_FORMAT);
    stl_le_p(buf + 4, id->title_id);
    stl_le_p(buf + 8, id->version);
    stl_le_p(buf + 12, id->disc_number);
    memcpy(buf + 16, id->xbe_header_hash, sizeof(id->xbe_header_hash));
}

bool xemu_disc_identity_deserialize(const uint8_t *buf, size_t len,
                                    XemuDiscIdentity *id)
{
    if (len < XEMU_DISC_IDENTITY_SERIALIZED_SIZE ||
        ldl_le_p(buf) != XEMU_DISC_IDENTITY_FORMAT) {
        return false;
    }
    id->title_id = ldl_le_p(buf + 4);
    id->version = ldl_le_p(buf + 8);
    id->disc_number = ldl_le_p(buf + 12);
    memcpy(id->xbe_header_hash, buf + 16, sizeof(id->xbe_header_hash));
    return true;
}
