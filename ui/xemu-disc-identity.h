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

#ifndef XEMU_DISC_IDENTITY_H
#define XEMU_DISC_IDENTITY_H

#include "qemu/osdep.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XEMU_DISC_IDENTITY_HASH_SIZE 32
#define XEMU_DISC_IDENTITY_SERIALIZED_SIZE (16 + XEMU_DISC_IDENTITY_HASH_SIZE)

/*
 * Identifies a disc by its content: the certificate fields of default.xbe and
 * a SHA-256 of its header block (which holds the certificate and the digest
 * of every section).
 */
typedef struct XemuDiscIdentity {
    uint32_t title_id;
    uint32_t version;
    uint32_t disc_number;
    uint8_t xbe_header_hash[XEMU_DISC_IDENTITY_HASH_SIZE];
} XemuDiscIdentity;

/* Reads @len bytes at @offset of the disc image. Returns false on error. */
typedef bool (*XemuDiscReadFunc)(void *opaque, uint64_t offset, void *buf,
                                 size_t len);

/*
 * Finds default.xbe on an XDVDFS disc image and computes its identity.
 * Returns false if the image is not an Xbox disc or has no default.xbe.
 */
bool xemu_disc_identity_compute(XemuDiscReadFunc read, void *opaque,
                                uint64_t disc_size, XemuDiscIdentity *id);

bool xemu_disc_identity_equal(const XemuDiscIdentity *a,
                              const XemuDiscIdentity *b);

void xemu_disc_identity_serialize(
    const XemuDiscIdentity *id,
    uint8_t buf[XEMU_DISC_IDENTITY_SERIALIZED_SIZE]);
bool xemu_disc_identity_deserialize(const uint8_t *buf, size_t len,
                                    XemuDiscIdentity *id);

#ifdef __cplusplus
}
#endif

#endif
