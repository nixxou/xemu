/*
 * xemu external snapshot storage
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

#include "xemu-snapshots.h"
#include "xemu-snapshot-file.h"
#include "xemu-settings.h"
#include "xemu-version.h"
#include "xemu-xbe.h"

#include <glib/gstdio.h>
#include <xxhash.h>

#include "block/block-io.h"
#include "block/block_int-io.h"
#include "block/graph-lock.h"
#include "hw/boards.h"
#include "io/channel-buffer.h"
#include "migration/global_state.h"
#include "migration/qemu-file.h"
#include "migration/snapshot.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/memalign.h"
#include "qemu/units.h"
#include "system/block-backend.h"
#include "system/runstate.h"

#define DISK_FORMAT_VERSION 1
#define DISK_HEADER_SIZE 48
#define DISK_EXTENT_SIZE 24
#define DISK_IO_CHUNK (1024 * 1024)
#define DISK_FINGERPRINT_BLOCK (64 * 1024)
#define DISK_MAX_EXTENTS (4 * 1024 * 1024)

#define HDD_DEVICE_NAME "ide0-hd0"
#define MAX_FILE_STEM_LEN 100

typedef enum DiskBelowKind {
    DISK_BELOW_ZEROES = 0,
    DISK_BELOW_IMAGE = 1,
} DiskBelowKind;

typedef enum DiskExtentType {
    DISK_EXTENT_DATA = 1,
    DISK_EXTENT_ZERO = 2,
} DiskExtentType;

typedef struct DiskExtent {
    uint64_t offset;
    uint64_t length;
    uint32_t type;
} DiskExtent;

/* What the top layer of the HDD sits on */
typedef struct DiskBelow {
    uint32_t kind;
    uint8_t fingerprint[16];
    uint64_t size;
    char *name;
} DiskBelow;

typedef struct DiskHeader {
    uint64_t size;
    DiskBelow below;
    uint64_t num_extents;
} DiskHeader;

typedef struct ExternalSnapshot {
    char *path;
    QEMUSnapshotInfo info;
    XemuSnapshotData data;
} ExternalSnapshot;

static QEMUSnapshotInfo *external_info;
static XemuSnapshotData *external_data;
static char **external_paths;
static int external_len;
static bool external_dirty = true;

static struct {
    char *filename;
    int64_t mtime;
    int64_t file_size;
    uint64_t size;
    uint8_t fingerprint[16];
} fingerprint_cache;

void xemu_snapshots_external_mark_dirty(void)
{
    external_dirty = true;
}

static char *get_snapshots_dir(void)
{
    const char *dir = g_config.general.snapshots.dir;
    if (dir && dir[0]) {
        return g_strdup(dir);
    }
    return g_build_filename(xemu_settings_get_base_path(), "snapshots", NULL);
}

static uint32_t get_current_title_id(char **title_name)
{
    struct xbe *xbe = xemu_get_xbe_info();
    if (!xbe || !xbe->cert) {
        if (title_name) {
            *title_name = NULL;
        }
        return 0;
    }

    if (title_name) {
        *title_name = g_utf16_to_utf8(xbe->cert->m_title_name,
                                      ARRAY_SIZE(xbe->cert->m_title_name), NULL,
                                      NULL, NULL);
    }
    return ldl_le_p(&xbe->cert->m_titleid);
}

/* Turns a snapshot name into a file name valid on all host systems */
static char *name_to_file_stem(const char *name)
{
    static const char *const reserved[] = {
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
        "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
        "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };

    GString *s = g_string_new(NULL);
    for (const char *p = name; *p && s->len < MAX_FILE_STEM_LEN;
         p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (c < 0x20 || strchr("<>:\"/\\|?*", c)) {
            g_string_append_c(s, '_');
        } else {
            g_string_append_unichar(s, c);
        }
    }

    /* Windows drops trailing dots and spaces */
    while (s->len && (s->str[s->len - 1] == '.' || s->str[s->len - 1] == ' ')) {
        g_string_truncate(s, s->len - 1);
    }
    if (!s->len) {
        g_string_append_c(s, '_');
    }
    for (int i = 0; i < ARRAY_SIZE(reserved); i++) {
        if (!g_ascii_strcasecmp(s->str, reserved[i])) {
            g_string_append_c(s, '_');
            break;
        }
    }

    return g_string_free(s, false);
}

static BlockBackend *get_hdd_blk(void)
{
    BlockBackend *blk = blk_by_name(HDD_DEVICE_NAME);
    if (!blk || !blk_is_inserted(blk)) {
        return NULL;
    }
    return blk;
}

static BlockDriverState *get_hdd_top(BlockBackend *blk)
{
    GRAPH_RDLOCK_GUARD_MAINLOOP();
    return bdrv_skip_filters(blk_bs(blk));
}

static BdrvChild *get_below_child(BlockDriverState *top)
{
    GRAPH_RDLOCK_GUARD_MAINLOOP();
    return bdrv_cow_child(top);
}

/*
 * Fingerprint of the guest-visible content of an image, independent of how
 * it is allocated: blocks that read as zero are left out, the others are
 * hashed with their index.
 */
static bool compute_fingerprint(BdrvChild *child, uint64_t size,
                                uint8_t fingerprint[16], Error **errp)
{
    BlockDriverState *bs = child->bs;
    XXH3_state_t *state = XXH3_createState();
    XXH3_128bits_reset(state);
    uint8_t *buf = qemu_blockalign(bs, DISK_FINGERPRINT_BLOCK);
    bool ok = true;

    for (uint64_t offset = 0; offset < size;) {
        uint64_t len = MIN(DISK_FINGERPRINT_BLOCK, size - offset);
        int64_t pnum;
        int ret = bdrv_block_status_above(bs, NULL, offset, size - offset,
                                          &pnum, NULL, NULL);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "Failed to read backing image");
            ok = false;
            break;
        }

        if ((ret & BDRV_BLOCK_ZERO) && pnum >= len) {
            offset += MAX(QEMU_ALIGN_DOWN(pnum, DISK_FINGERPRINT_BLOCK), len);
            continue;
        }

        ret = bdrv_pread(child, offset, len, buf, 0);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "Failed to read backing image");
            ok = false;
            break;
        }
        if (!buffer_is_zero(buf, len)) {
            uint8_t index[8];
            stq_le_p(index, offset / DISK_FINGERPRINT_BLOCK);
            XXH3_128bits_update(state, index, sizeof(index));
            XXH3_128bits_update(state, buf, len);
        }
        offset += len;
    }

    uint8_t total[8];
    stq_le_p(total, size);
    XXH3_128bits_update(state, total, sizeof(total));

    XXH128_canonical_t canonical;
    XXH128_canonicalFromHash(&canonical, XXH3_128bits_digest(state));
    memcpy(fingerprint, canonical.digest, 16);

    qemu_vfree(buf);
    XXH3_freeState(state);
    return ok;
}

static bool get_disk_below(BlockDriverState *top, DiskBelow *below,
                           Error **errp)
{
    memset(below, 0, sizeof(*below));

    BdrvChild *child = get_below_child(top);
    if (!child) {
        below->kind = DISK_BELOW_ZEROES;
        return true;
    }

    BlockDriverState *bs = child->bs;
    int64_t size = bdrv_getlength(bs);
    if (size < 0) {
        error_setg_errno(errp, -size, "Failed to get backing image size");
        return false;
    }

    below->kind = DISK_BELOW_IMAGE;
    below->size = size;
    below->name = g_strdup(bs->filename);

    /* Hashing the backing image is slow; it does not change while in use */
    GStatBuf st;
    bool cacheable = bs->filename[0] && g_stat(bs->filename, &st) == 0;
    if (cacheable && fingerprint_cache.filename &&
        !strcmp(fingerprint_cache.filename, bs->filename) &&
        fingerprint_cache.mtime == st.st_mtime &&
        fingerprint_cache.file_size == st.st_size &&
        fingerprint_cache.size == size) {
        memcpy(below->fingerprint, fingerprint_cache.fingerprint, 16);
        return true;
    }

    if (!compute_fingerprint(child, size, below->fingerprint, errp)) {
        return false;
    }

    if (cacheable) {
        g_free(fingerprint_cache.filename);
        fingerprint_cache.filename = g_strdup(bs->filename);
        fingerprint_cache.mtime = st.st_mtime;
        fingerprint_cache.file_size = st.st_size;
        fingerprint_cache.size = size;
        memcpy(fingerprint_cache.fingerprint, below->fingerprint, 16);
    }
    return true;
}

static bool disk_below_equal(const DiskBelow *a, const DiskBelow *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    return a->kind == DISK_BELOW_ZEROES ||
           (a->size == b->size && !memcmp(a->fingerprint, b->fingerprint, 16));
}

static void encode_disk_header(GByteArray *out, const DiskHeader *h)
{
    uint8_t buf[DISK_HEADER_SIZE] = { 0 };
    stl_le_p(buf + 0, DISK_FORMAT_VERSION);
    stq_le_p(buf + 8, h->size);
    stl_le_p(buf + 16, h->below.kind);
    memcpy(buf + 20, h->below.fingerprint, 16);
    stq_le_p(buf + 36, h->below.size);
    stl_le_p(buf + 44, h->below.name ? strlen(h->below.name) : 0);
    g_byte_array_append(out, buf, sizeof(buf));
    if (h->below.name) {
        g_byte_array_append(out, (const uint8_t *)h->below.name,
                            strlen(h->below.name));
    }

    uint8_t count[8];
    stq_le_p(count, h->num_extents);
    g_byte_array_append(out, count, sizeof(count));
}

static bool read_disk_header(XemuSnapshotReader *r, DiskHeader *h, Error **errp)
{
    uint8_t buf[DISK_HEADER_SIZE];
    memset(h, 0, sizeof(*h));
    if (!xemu_snapshot_reader_read(r, buf, sizeof(buf), errp)) {
        return false;
    }

    if (ldl_le_p(buf) != DISK_FORMAT_VERSION) {
        error_setg(errp, "Unsupported HDD state format version %" PRIu32,
                   ldl_le_p(buf));
        return false;
    }
    h->size = ldq_le_p(buf + 8);
    h->below.kind = ldl_le_p(buf + 16);
    memcpy(h->below.fingerprint, buf + 20, 16);
    h->below.size = ldq_le_p(buf + 36);

    uint32_t name_len = ldl_le_p(buf + 44);
    if (name_len > 64 * 1024) {
        error_setg(errp, "Invalid HDD state");
        return false;
    }
    h->below.name = g_malloc0(name_len + 1);
    if (!xemu_snapshot_reader_read(r, h->below.name, name_len, errp)) {
        return false;
    }

    uint8_t count[8];
    if (!xemu_snapshot_reader_read(r, count, sizeof(count), errp)) {
        return false;
    }
    h->num_extents = ldq_le_p(count);
    if (h->num_extents > DISK_MAX_EXTENTS) {
        error_setg(errp, "Invalid HDD state");
        return false;
    }

    return true;
}

static void free_disk_header(DiskHeader *h)
{
    g_free(h->below.name);
    h->below.name = NULL;
}

static DiskExtent *read_disk_extents(XemuSnapshotReader *r, const DiskHeader *h,
                                     Error **errp)
{
    DiskExtent *extents = g_try_new(DiskExtent, h->num_extents);
    uint64_t end = 0;

    if (!extents && h->num_extents) {
        error_setg(errp, "Out of memory reading HDD state");
        return NULL;
    }

    for (uint64_t i = 0; i < h->num_extents; i++) {
        uint8_t buf[DISK_EXTENT_SIZE];
        if (!xemu_snapshot_reader_read(r, buf, sizeof(buf), errp)) {
            g_free(extents);
            return NULL;
        }
        extents[i].offset = ldq_le_p(buf);
        extents[i].length = ldq_le_p(buf + 8);
        extents[i].type = ldl_le_p(buf + 16);

        if (extents[i].offset < end || !extents[i].length ||
            extents[i].length > h->size ||
            extents[i].offset > h->size - extents[i].length ||
            (extents[i].type != DISK_EXTENT_DATA &&
             extents[i].type != DISK_EXTENT_ZERO)) {
            error_setg(errp, "Invalid HDD state");
            g_free(extents);
            return NULL;
        }
        end = extents[i].offset + extents[i].length;
    }

    return extents;
}

/*
 * Writes the HDD state: the extents of the top layer and their data. The VM
 * must be stopped. The image is only read.
 */
static bool save_disk(XemuSnapshotWriter *w, BlockBackend *blk, Error **errp)
{
    BlockDriverState *top = get_hdd_top(blk);
    DiskHeader h = { 0 };
    g_autoptr(GArray) extents = g_array_new(false, false, sizeof(DiskExtent));
    g_autoptr(GByteArray) header = g_byte_array_new();
    uint8_t *buf = NULL;
    bool ok = false;

    int64_t size = blk_getlength(blk);
    if (size < 0) {
        error_setg_errno(errp, -size, "Failed to get HDD image size");
        return false;
    }
    h.size = size;

    if (!get_disk_below(top, &h.below, errp)) {
        goto out;
    }

    for (int64_t offset = 0; offset < size;) {
        int64_t pnum;
        int ret =
            bdrv_block_status(top, offset, size - offset, &pnum, NULL, NULL);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "Failed to read HDD image");
            goto out;
        }
        assert(pnum > 0);

        if (ret & BDRV_BLOCK_ALLOCATED) {
            uint32_t type =
                (ret & BDRV_BLOCK_ZERO) ? DISK_EXTENT_ZERO : DISK_EXTENT_DATA;
            DiskExtent *last =
                extents->len ?
                    &g_array_index(extents, DiskExtent, extents->len - 1) :
                    NULL;
            if (last && last->type == type &&
                last->offset + last->length == offset) {
                last->length += pnum;
            } else {
                DiskExtent e = { .offset = offset,
                                 .length = pnum,
                                 .type = type };
                g_array_append_val(extents, e);
            }
        }
        offset += pnum;
    }
    h.num_extents = extents->len;

    encode_disk_header(header, &h);
    for (guint i = 0; i < extents->len; i++) {
        DiskExtent *e = &g_array_index(extents, DiskExtent, i);
        uint8_t e_buf[DISK_EXTENT_SIZE] = { 0 };
        stq_le_p(e_buf, e->offset);
        stq_le_p(e_buf + 8, e->length);
        stl_le_p(e_buf + 16, e->type);
        g_byte_array_append(header, e_buf, sizeof(e_buf));
    }

    if (!xemu_snapshot_writer_begin_section(w, XEMU_SNAPSHOT_SECTION_DISK,
                                            errp) ||
        !xemu_snapshot_writer_write(w, header->data, header->len, errp)) {
        goto out;
    }

    buf = blk_blockalign(blk, DISK_IO_CHUNK);
    for (guint i = 0; i < extents->len; i++) {
        DiskExtent *e = &g_array_index(extents, DiskExtent, i);
        if (e->type != DISK_EXTENT_DATA) {
            continue;
        }
        for (uint64_t done = 0; done < e->length;) {
            uint64_t len = MIN(DISK_IO_CHUNK, e->length - done);
            int ret = blk_pread(blk, e->offset + done, len, buf, 0);
            if (ret < 0) {
                error_setg_errno(errp, -ret, "Failed to read HDD image");
                goto out;
            }
            if (!xemu_snapshot_writer_write(w, buf, len, errp)) {
                goto out;
            }
            done += len;
        }
    }

    ok = xemu_snapshot_writer_end_section(w, errp);

out:
    qemu_vfree(buf);
    free_disk_header(&h);
    return ok;
}

/* Checks that the snapshot's HDD state can be restored on the current HDD */
static bool check_disk(XemuSnapshotReader *r, BlockBackend *blk, DiskHeader *h,
                       Error **errp)
{
    if (!read_disk_header(r, h, errp)) {
        return false;
    }

    if (!blk) {
        error_setg(errp, "The snapshot contains an HDD state but no HDD image "
                         "is attached");
        return false;
    }

    int64_t size = blk_getlength(blk);
    if (size < 0) {
        error_setg_errno(errp, -size, "Failed to get HDD image size");
        return false;
    }
    if (size != h->size) {
        error_setg(errp,
                   "The HDD image size (%" PRId64 " bytes) does not match the "
                   "snapshot (%" PRIu64 " bytes)",
                   size, h->size);
        return false;
    }

    DiskBelow below;
    if (!get_disk_below(get_hdd_top(blk), &below, errp)) {
        return false;
    }
    bool equal = disk_below_equal(&below, &h->below);
    if (!equal) {
        if (h->below.kind == DISK_BELOW_ZEROES) {
            error_setg(errp, "The snapshot was taken with a standalone HDD "
                             "image, the current HDD image has a backing "
                             "file");
        } else if (below.kind == DISK_BELOW_ZEROES) {
            error_setg(errp,
                       "The snapshot was taken with an HDD image based on "
                       "'%s', the current HDD image has no backing file",
                       h->below.name);
        } else {
            error_setg(errp,
                       "The backing file of the HDD image ('%s') does not "
                       "match the one used by the snapshot ('%s')",
                       below.name, h->below.name);
        }
    }
    g_free(below.name);
    return equal;
}

static bool read_below(BdrvChild *below, uint64_t below_size, uint64_t offset,
                       uint64_t len, uint8_t *buf, Error **errp)
{
    uint64_t n = 0;
    if (below && offset < below_size) {
        n = MIN(len, below_size - offset);
        int ret = bdrv_pread(below, offset, n, buf, 0);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "Failed to read HDD backing image");
            return false;
        }
    }
    memset(buf + n, 0, len - n);
    return true;
}

/*
 * Makes the guest-visible content of the HDD equal to the snapshot's, only
 * writing ranges that differ. The VM must be stopped.
 */
static bool restore_disk(XemuSnapshotReader *r, BlockBackend *blk,
                         const DiskHeader *h, const DiskExtent *extents,
                         Error **errp)
{
    BlockDriverState *top = get_hdd_top(blk);
    BdrvChild *below = get_below_child(top);
    uint64_t below_size = below ? h->below.size : 0;
    uint8_t *want = blk_blockalign(blk, DISK_IO_CHUNK);
    uint8_t *have = blk_blockalign(blk, DISK_IO_CHUNK);
    uint64_t i = 0;
    bool ok = false;

    for (uint64_t offset = 0; offset < h->size;) {
        uint32_t type = 0; /* 0: content comes from below */
        uint64_t end = h->size;
        if (i < h->num_extents) {
            if (extents[i].offset <= offset) {
                type = extents[i].type;
                end = extents[i].offset + extents[i].length;
            } else {
                end = extents[i].offset;
            }
        }

        int64_t pnum;
        int ret =
            bdrv_block_status(top, offset, end - offset, &pnum, NULL, NULL);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "Failed to read HDD image");
            goto out;
        }

        if (!type && !(ret & BDRV_BLOCK_ALLOCATED)) {
            /* Both read the same backing data */
            offset += pnum;
            continue;
        }

        uint64_t len = MIN(pnum, DISK_IO_CHUNK);
        bool want_zero;
        if (type == DISK_EXTENT_DATA) {
            if (!xemu_snapshot_reader_read(r, want, len, errp)) {
                goto out;
            }
            want_zero = buffer_is_zero(want, len);
        } else if (type == DISK_EXTENT_ZERO) {
            want_zero = true;
        } else {
            if (!read_below(below, below_size, offset, len, want, errp)) {
                goto out;
            }
            want_zero = buffer_is_zero(want, len);
        }

        bool equal;
        if (want_zero && (ret & BDRV_BLOCK_ZERO)) {
            equal = true;
        } else {
            ret = blk_pread(blk, offset, len, have, 0);
            if (ret < 0) {
                error_setg_errno(errp, -ret, "Failed to read HDD image");
                goto out;
            }
            equal = want_zero ? buffer_is_zero(have, len) :
                                !memcmp(want, have, len);
        }

        if (!equal) {
            if (want_zero) {
                ret = blk_pwrite_zeroes(blk, offset, len, BDRV_REQ_MAY_UNMAP);
            } else {
                ret = blk_pwrite(blk, offset, len, want, 0);
            }
            if (ret < 0) {
                error_setg_errno(errp, -ret, "Failed to write HDD image");
                goto out;
            }
        }

        offset += len;
        if (type && offset == end) {
            i++;
        }
    }

    ok = blk_flush(blk) == 0;
    if (!ok) {
        error_setg(errp, "Failed to flush HDD image");
    }

out:
    qemu_vfree(want);
    qemu_vfree(have);
    return ok;
}

static bool save_vm_state(XemuSnapshotWriter *w, Error **errp)
{
    QIOChannelBuffer *bioc = qio_channel_buffer_new(64 * 1024 * 1024);
    QEMUFile *f = qemu_file_new_output(QIO_CHANNEL(bioc));
    bool ok = xemu_save_vm_state(f, errp);
    if (ok) {
        int ret = qemu_fflush(f);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "Failed to write VM state");
            ok = false;
        }
    }

    /* Closing the stream frees the buffer */
    if (ok) {
        ok = xemu_snapshot_writer_write_section(
            w, XEMU_SNAPSHOT_SECTION_VMSTATE, bioc->data, bioc->usage, errp);
    }

    qemu_fclose(f);
    object_unref(OBJECT(bioc));
    return ok;
}

static bool load_vm_state(XemuSnapshotReader *r, Error **errp)
{
    if (!xemu_snapshot_reader_find_section(r, XEMU_SNAPSHOT_SECTION_VMSTATE,
                                           NULL, errp)) {
        if (errp && !*errp) {
            error_setg(errp, "The snapshot has no VM state");
        }
        return false;
    }

    size_t len;
    uint8_t *data = xemu_snapshot_reader_read_section(r, &len, errp);
    if (!data) {
        return false;
    }

    QIOChannelBuffer *bioc = qio_channel_buffer_new(0);
    g_free(bioc->data);
    bioc->data = data;
    bioc->capacity = len + 1;
    bioc->usage = len;
    QEMUFile *f = qemu_file_new_input(QIO_CHANNEL(bioc));
    object_unref(OBJECT(bioc));

    return xemu_load_vm_state(f, errp);
}

static uint64_t get_ram_size(void)
{
    return current_machine ? current_machine->ram_size : 0;
}

static void build_meta(GByteArray *meta, const char *name,
                       const QEMUSnapshotInfo *sn, uint32_t title_id,
                       const char *title_name, BlockBackend *hdd)
{
    xemu_snapshot_meta_put_str(meta, XEMU_SNAPSHOT_META_NAME, name);
    xemu_snapshot_meta_put_u64(meta, XEMU_SNAPSHOT_META_DATE_SEC, sn->date_sec);
    xemu_snapshot_meta_put_u32(meta, XEMU_SNAPSHOT_META_DATE_NSEC,
                               sn->date_nsec);
    xemu_snapshot_meta_put_u64(meta, XEMU_SNAPSHOT_META_VM_CLOCK_NSEC,
                               sn->vm_clock_nsec);
    xemu_snapshot_meta_put_str(meta, XEMU_SNAPSHOT_META_XEMU_VERSION,
                               xemu_version);
    xemu_snapshot_meta_put_str(meta, XEMU_SNAPSHOT_META_XEMU_COMMIT,
                               xemu_commit);
    xemu_snapshot_meta_put_u64(meta, XEMU_SNAPSHOT_META_RAM_SIZE,
                               get_ram_size());
    xemu_snapshot_meta_put_u32(meta, XEMU_SNAPSHOT_META_TITLE_ID, title_id);
    xemu_snapshot_meta_put_str(meta, XEMU_SNAPSHOT_META_TITLE_NAME, title_name);

    g_autofree char *disc_path = xemu_get_currently_loaded_disc_path();
    xemu_snapshot_meta_put_str(meta, XEMU_SNAPSHOT_META_DISC_PATH, disc_path);

    XemuDiscIdentity disc_id;
    if (disc_path && xemu_get_currently_loaded_disc_identity(&disc_id)) {
        uint8_t buf[XEMU_DISC_IDENTITY_SERIALIZED_SIZE];
        xemu_disc_identity_serialize(&disc_id, buf);
        xemu_snapshot_meta_put(meta, XEMU_SNAPSHOT_META_DISC_IDENTITY, buf,
                               sizeof(buf));
    }

    if (hdd) {
        xemu_snapshot_meta_put_str(meta, XEMU_SNAPSHOT_META_HDD_PATH,
                                   blk_bs(hdd)->filename);
    }
}

void xemu_snapshots_external_free_data(XemuSnapshotData *data)
{
    g_free(data->disc_path);
    g_free(data->xbe_title_name);
    g_free(data->hdd_path);
    g_free(data->xemu_version);
    if (data->gl_thumbnail) {
        glDeleteTextures(1, &data->gl_thumbnail);
    }
    memset(data, 0, sizeof(*data));
}

static void parse_meta(const uint8_t *meta, size_t len, QEMUSnapshotInfo *info,
                       XemuSnapshotData *data)
{
    g_autofree char *name =
        xemu_snapshot_meta_get_str(meta, len, XEMU_SNAPSHOT_META_NAME);
    if (name) {
        pstrcpy(info->name, sizeof(info->name), name);
    }

    uint64_t v64;
    uint32_t v32;
    if (xemu_snapshot_meta_get_u64(meta, len, XEMU_SNAPSHOT_META_DATE_SEC,
                                   &v64)) {
        info->date_sec = v64;
    }
    if (xemu_snapshot_meta_get_u32(meta, len, XEMU_SNAPSHOT_META_DATE_NSEC,
                                   &v32)) {
        info->date_nsec = v32;
    }
    if (xemu_snapshot_meta_get_u64(meta, len, XEMU_SNAPSHOT_META_VM_CLOCK_NSEC,
                                   &v64)) {
        info->vm_clock_nsec = v64;
    }
    if (xemu_snapshot_meta_get_u64(meta, len, XEMU_SNAPSHOT_META_RAM_SIZE,
                                   &v64)) {
        data->ram_size = v64;
    }
    if (xemu_snapshot_meta_get_u32(meta, len, XEMU_SNAPSHOT_META_TITLE_ID,
                                   &v32)) {
        data->title_id = v32;
    }

    data->external = true;
    data->xbe_title_name =
        xemu_snapshot_meta_get_str(meta, len, XEMU_SNAPSHOT_META_TITLE_NAME);
    data->disc_path =
        xemu_snapshot_meta_get_str(meta, len, XEMU_SNAPSHOT_META_DISC_PATH);
    data->hdd_path =
        xemu_snapshot_meta_get_str(meta, len, XEMU_SNAPSHOT_META_HDD_PATH);
    data->xemu_version =
        xemu_snapshot_meta_get_str(meta, len, XEMU_SNAPSHOT_META_XEMU_VERSION);

    uint32_t id_len;
    const uint8_t *id = xemu_snapshot_meta_get(
        meta, len, XEMU_SNAPSHOT_META_DISC_IDENTITY, &id_len);
    if (id) {
        data->has_disc_identity =
            xemu_disc_identity_deserialize(id, id_len, &data->disc_identity);
    }
}

bool xemu_snapshots_external_read_info(const char *path, QEMUSnapshotInfo *info,
                                       XemuSnapshotData *data,
                                       bool load_thumbnail, Error **errp)
{
    memset(info, 0, sizeof(*info));
    memset(data, 0, sizeof(*data));

    XemuSnapshotReader *r = xemu_snapshot_reader_open(path, errp);
    if (!r) {
        return false;
    }

    bool ok = false;
    XemuSnapshotSection s;
    Error *local_err = NULL;
    while (xemu_snapshot_reader_next_section(r, &s, &local_err)) {
        if (s.type == XEMU_SNAPSHOT_SECTION_META) {
            size_t len;
            g_autofree uint8_t *meta =
                xemu_snapshot_reader_read_section(r, &len, &local_err);
            if (!meta) {
                break;
            }
            parse_meta(meta, len, info, data);
            ok = true;
        } else if (s.type == XEMU_SNAPSHOT_SECTION_THUMBNAIL &&
                   load_thumbnail) {
            size_t len;
            g_autofree uint8_t *png =
                xemu_snapshot_reader_read_section(r, &len, &local_err);
            if (!png) {
                break;
            }
            GLuint tex;
            glGenTextures(1, &tex);
            if (xemu_snapshots_load_png_to_texture(tex, png, len)) {
                data->gl_thumbnail = tex;
            } else {
                glDeleteTextures(1, &tex);
            }
        } else if (s.type == XEMU_SNAPSHOT_SECTION_VMSTATE) {
            info->vm_state_size = s.raw_size;
            break;
        }
    }
    xemu_snapshot_reader_close(r);

    if (local_err) {
        error_propagate(errp, local_err);
        ok = false;
    } else if (!ok) {
        error_setg(errp, "'%s' has no metadata", path);
    }

    if (!ok) {
        xemu_snapshots_external_free_data(data);
    }
    return ok;
}

static void free_list(void)
{
    for (int i = 0; i < external_len; i++) {
        xemu_snapshots_external_free_data(&external_data[i]);
        g_free(external_paths[i]);
    }
    g_free(external_info);
    g_free(external_data);
    g_free(external_paths);
    external_info = NULL;
    external_data = NULL;
    external_paths = NULL;
    external_len = 0;
}

static gint compare_snapshots(gconstpointer a, gconstpointer b)
{
    const ExternalSnapshot *sa = a, *sb = b;
    if (sa->info.date_sec != sb->info.date_sec) {
        return sa->info.date_sec < sb->info.date_sec ? -1 : 1;
    }
    if (sa->info.date_nsec != sb->info.date_nsec) {
        return sa->info.date_nsec < sb->info.date_nsec ? -1 : 1;
    }
    return g_strcmp0(sa->path, sb->path);
}

static void scan_snapshots(GArray *out)
{
    g_autofree char *dir = get_snapshots_dir();
    GDir *top = g_dir_open(dir, 0, NULL);
    if (!top) {
        return;
    }

    const char *title_dir_name;
    while ((title_dir_name = g_dir_read_name(top))) {
        g_autofree char *title_dir =
            g_build_filename(dir, title_dir_name, NULL);
        GDir *d = g_dir_open(title_dir, 0, NULL);
        if (!d) {
            continue;
        }

        const char *file_name;
        while ((file_name = g_dir_read_name(d))) {
            if (!g_str_has_suffix(file_name, XEMU_SNAPSHOT_FILE_EXT)) {
                continue;
            }

            ExternalSnapshot s = { 0 };
            s.path = g_build_filename(title_dir, file_name, NULL);
            Error *err = NULL;
            if (!xemu_snapshots_external_read_info(s.path, &s.info, &s.data,
                                                   true, &err)) {
                warn_report_err(err);
                g_free(s.path);
                continue;
            }

            g_autofree char *stem = g_strndup(
                file_name, strlen(file_name) - strlen(XEMU_SNAPSHOT_FILE_EXT));
            snprintf(s.info.id_str, sizeof(s.info.id_str), "%s/%s",
                     title_dir_name, stem);
            if (!s.info.name[0]) {
                pstrcpy(s.info.name, sizeof(s.info.name), stem);
            }
            g_array_append_val(out, s);
        }
        g_dir_close(d);
    }
    g_dir_close(top);
}

int xemu_snapshots_external_list(QEMUSnapshotInfo **info,
                                 XemuSnapshotData **extra_data, Error **err)
{
    if (external_dirty || !external_info) {
        free_list();

        g_autoptr(GArray) found =
            g_array_new(false, false, sizeof(ExternalSnapshot));
        scan_snapshots(found);
        g_array_sort(found, compare_snapshots);

        external_len = found->len;
        external_info = g_new0(QEMUSnapshotInfo, MAX(external_len, 1));
        external_data = g_new0(XemuSnapshotData, MAX(external_len, 1));
        external_paths = g_new0(char *, MAX(external_len, 1));
        for (int i = 0; i < external_len; i++) {
            ExternalSnapshot *s = &g_array_index(found, ExternalSnapshot, i);
            external_info[i] = s->info;
            external_data[i] = s->data;
            external_paths[i] = s->path;
        }
        external_dirty = false;
    }

    if (info) {
        *info = external_info;
    }
    if (extra_data) {
        *extra_data = external_data;
    }
    return external_len;
}

/*
 * Resolves a snapshot reference: an id from the list ("TITLEID/file"), or a
 * name, looked up in the running title's folder first and then in all
 * folders, most recent first.
 */
static int find_snapshot(const char *ref)
{
    Error *err = NULL;
    xemu_snapshots_external_list(NULL, NULL, &err);
    error_free(err);

    for (int i = 0; i < external_len; i++) {
        if (!strcmp(external_info[i].id_str, ref)) {
            return i;
        }
    }

    uint32_t title_id = get_current_title_id(NULL);
    for (int i = external_len - 1; i >= 0; i--) {
        if (external_data[i].title_id == title_id &&
            !strcmp(external_info[i].name, ref)) {
            return i;
        }
    }
    for (int i = external_len - 1; i >= 0; i--) {
        if (!strcmp(external_info[i].name, ref)) {
            return i;
        }
    }
    return -1;
}

void xemu_snapshots_external_save(const char *ref, Error **errp)
{
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *name = NULL;
    g_autofree char *replaced_path = NULL;

    if (ref) {
        int i = find_snapshot(ref);
        if (i >= 0 && !strcmp(external_info[i].id_str, ref)) {
            name = g_strdup(external_info[i].name);
            replaced_path = g_strdup(external_paths[i]);
        } else {
            name = g_strdup(ref);
        }
    } else {
        name = g_date_time_format(now, "vm-%Y%m%d%H%M%S");
    }

    g_autofree char *title_name = NULL;
    uint32_t title_id = get_current_title_id(&title_name);
    g_autofree char *base_dir = get_snapshots_dir();
    g_autofree char *title_dir_name = g_strdup_printf("%08" PRIX32, title_id);
    g_autofree char *title_dir =
        g_build_filename(base_dir, title_dir_name, NULL);
    if (g_mkdir_with_parents(title_dir, 0755) < 0) {
        error_setg_errno(errp, errno, "Failed to create directory '%s'",
                         title_dir);
        return;
    }

    g_autofree char *stem = name_to_file_stem(name);
    g_autofree char *file_name =
        g_strdup_printf("%s%s", stem, XEMU_SNAPSHOT_FILE_EXT);
    g_autofree char *path = g_build_filename(title_dir, file_name, NULL);
    g_autofree char *tmp_path = g_strdup_printf("%s.tmp", path);

    QEMUSnapshotInfo sn = { 0 };
    sn.date_sec = g_date_time_to_unix(now);
    sn.date_nsec = g_date_time_get_microsecond(now) * 1000;

    size_t thumbnail_size = 0;
    g_autofree void *thumbnail =
        xemu_snapshots_create_framebuffer_thumbnail_png(&thumbnail_size);

    BlockBackend *hdd = get_hdd_blk();
    RunState saved_state = runstate_get();
    global_state_store();
    vm_stop(RUN_STATE_SAVE_VM);

    sn.vm_clock_nsec = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    g_autoptr(GByteArray) meta = g_byte_array_new();
    build_meta(meta, name, &sn, title_id, title_name, hdd);

    XemuSnapshotWriter *w = xemu_snapshot_writer_open(
        tmp_path, g_config.general.snapshots.compress, errp);
    bool ok = w != NULL;
    ok = ok && xemu_snapshot_writer_write_section(w, XEMU_SNAPSHOT_SECTION_META,
                                                  meta->data, meta->len, errp);
    if (ok && thumbnail) {
        ok = xemu_snapshot_writer_write_section(
            w, XEMU_SNAPSHOT_SECTION_THUMBNAIL, thumbnail, thumbnail_size,
            errp);
    }
    if (ok && hdd) {
        ok = save_disk(w, hdd, errp);
    }
    ok = ok && save_vm_state(w, errp);

    vm_resume(saved_state);

    if (w) {
        if (ok) {
            ok = xemu_snapshot_writer_close(w, errp);
        } else {
            xemu_snapshot_writer_abort(w);
        }
    }

    if (ok && g_rename(tmp_path, path) < 0) {
        error_setg_errno(errp, errno, "Failed to rename '%s' to '%s'", tmp_path,
                         path);
        ok = false;
    }
    if (!ok) {
        g_unlink(tmp_path);
        return;
    }

    /* Replacing an entry of another title moves it to the running title */
    if (replaced_path && strcmp(replaced_path, path)) {
        g_unlink(replaced_path);
    }

    external_dirty = true;
}

void xemu_snapshots_external_delete(const char *ref, Error **errp)
{
    int i = find_snapshot(ref);
    if (i < 0) {
        error_setg(errp, "Snapshot '%s' does not exist", ref);
        return;
    }

    const char *path = external_paths[i];
    if (g_unlink(path) < 0) {
        error_setg_errno(errp, errno, "Failed to delete '%s'", path);
        return;
    }

    /* Only succeeds when the title folder is empty */
    g_autofree char *dir = g_path_get_dirname(path);
    g_rmdir(dir);

    external_dirty = true;
}

static bool read_meta_section(const char *path, uint8_t **meta, size_t *len,
                              Error **errp)
{
    XemuSnapshotReader *r = xemu_snapshot_reader_open(path, errp);
    if (!r) {
        return false;
    }

    Error *local_err = NULL;
    *meta = NULL;
    if (xemu_snapshot_reader_find_section(r, XEMU_SNAPSHOT_SECTION_META, NULL,
                                          &local_err)) {
        *meta = xemu_snapshot_reader_read_section(r, len, &local_err);
    }
    xemu_snapshot_reader_close(r);

    if (local_err) {
        error_propagate(errp, local_err);
        return false;
    }
    if (!*meta) {
        error_setg(errp, "'%s' has no metadata", path);
        return false;
    }
    return true;
}

void xemu_snapshots_external_load_file(const char *path, Error **errp)
{
    /* Nothing is modified until the whole file has been validated */
    if (!xemu_snapshot_file_verify(path, errp)) {
        return;
    }

    g_autofree uint8_t *meta = NULL;
    size_t meta_len;
    if (!read_meta_section(path, &meta, &meta_len, errp)) {
        return;
    }

    uint64_t ram_size;
    if (xemu_snapshot_meta_get_u64(meta, meta_len, XEMU_SNAPSHOT_META_RAM_SIZE,
                                   &ram_size) &&
        ram_size != get_ram_size()) {
        error_setg(errp,
                   "The snapshot was taken with %" PRIu64 " MiB of RAM, the "
                   "system has %" PRIu64 " MiB",
                   ram_size / MiB, get_ram_size() / MiB);
        return;
    }

    g_autofree char *version = xemu_snapshot_meta_get_str(
        meta, meta_len, XEMU_SNAPSHOT_META_XEMU_VERSION);
    if (version && strcmp(version, xemu_version)) {
        warn_report("Snapshot '%s' was created by xemu %s, this is xemu %s",
                    path, version, xemu_version);
    }

    XemuSnapshotReader *r = xemu_snapshot_reader_open(path, errp);
    if (!r) {
        return;
    }

    BlockBackend *hdd = get_hdd_blk();
    DiskHeader h = { 0 };
    DiskExtent *extents = NULL;
    bool has_disk = false;
    Error *local_err = NULL;

    if (xemu_snapshot_reader_find_section(r, XEMU_SNAPSHOT_SECTION_DISK, NULL,
                                          &local_err)) {
        has_disk = true;
        if (!check_disk(r, hdd, &h, &local_err)) {
            goto out;
        }
        extents = read_disk_extents(r, &h, &local_err);
        if (!extents) {
            goto out;
        }
    } else if (local_err) {
        goto out;
    } else if (hdd) {
        error_setg(&local_err, "The snapshot was taken without an HDD image "
                               "but one is attached");
        goto out;
    }

    bool was_running = runstate_is_running();
    if (was_running) {
        vm_stop(RUN_STATE_RESTORE_VM);
    }

    if (has_disk && !restore_disk(r, hdd, &h, extents, &local_err)) {
        error_prepend(&local_err, "Failed to restore the HDD state, the "
                                  "machine should be reset: ");
        goto out;
    }

    if (!load_vm_state(r, &local_err)) {
        goto out;
    }

    if (was_running) {
        vm_start();
    }

out:
    if (local_err) {
        error_propagate(errp, local_err);
    }
    g_free(extents);
    free_disk_header(&h);
    xemu_snapshot_reader_close(r);
}

void xemu_snapshots_external_load(const char *ref, Error **errp)
{
    int i = find_snapshot(ref);
    if (i < 0) {
        error_setg(errp, "Snapshot '%s' does not exist", ref);
        return;
    }

    g_autofree char *path = g_strdup(external_paths[i]);
    xemu_snapshots_external_load_file(path, errp);
}
