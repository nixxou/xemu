/*
 * xemu User Interface
 *
 * Copyright (C) 2020-2022 Matt Borgerson
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

#ifndef XEMU_SNAPSHOTS_H
#define XEMU_SNAPSHOTS_H

#include "qemu/osdep.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "block/snapshot.h"
#include "xemu-disc-identity.h"
#include <epoxy/gl.h>

#define XEMU_SNAPSHOT_DATA_MAGIC 0x78656d75 // 'xemu'
#define XEMU_SNAPSHOT_DATA_VERSION 1

#define XEMU_SNAPSHOT_THUMBNAIL_WIDTH 160
#define XEMU_SNAPSHOT_THUMBNAIL_HEIGHT 120

extern const char **g_snapshot_shortcut_index_key_map[];

typedef struct XemuSnapshotData {
    char *disc_path;
    char *xbe_title_name;
    GLuint gl_thumbnail;
    bool has_disc_identity;
    XemuDiscIdentity disc_identity;

    // External storage only
    uint32_t title_id;
    uint64_t ram_size;
    char *hdd_path;
    char *xemu_version;
} XemuSnapshotData;

typedef enum XemuSnapshotDiscCheck {
    XEMU_SNAPSHOT_DISC_MATCH,
    XEMU_SNAPSHOT_DISC_NONE, // The snapshot was taken without a disc
    XEMU_SNAPSHOT_DISC_MISMATCH,
} XemuSnapshotDiscCheck;

// Implemented in xemu-snapshots.c
char *xemu_get_currently_loaded_disc_path(void);
bool xemu_get_currently_loaded_disc_identity(XemuDiscIdentity *id);
int xemu_snapshots_list(QEMUSnapshotInfo **info, XemuSnapshotData **extra_data,
                        Error **err);
void xemu_snapshots_load(const char *vm_name, Error **err);
void xemu_snapshots_save(const char *vm_name, Error **err);
void xemu_snapshots_delete(const char *vm_name, Error **err);
bool xemu_snapshots_using_external_storage(void);
const char *xemu_snapshots_get_key(const QEMUSnapshotInfo *info);
XemuSnapshotDiscCheck xemu_snapshots_check_disc(const XemuSnapshotData *data);

void xemu_snapshots_save_extra_data(QEMUFile *f);
bool xemu_snapshots_offset_extra_data(QEMUFile *f);
void xemu_snapshots_mark_dirty(void);

// Implemented in xemu-snapshots-external.c
int xemu_snapshots_external_list(QEMUSnapshotInfo **info,
                                 XemuSnapshotData **extra_data, Error **err);
bool xemu_snapshots_external_lookup(const char *ref, QEMUSnapshotInfo **info,
                                    XemuSnapshotData **data);
bool xemu_snapshots_external_read_info(const char *path,
                                       QEMUSnapshotInfo *info,
                                       XemuSnapshotData *data,
                                       bool load_thumbnail, Error **errp);
void xemu_snapshots_external_load(const char *ref, Error **errp);
void xemu_snapshots_external_load_file(const char *path, Error **errp);
void xemu_snapshots_external_save(const char *ref, Error **errp);
void xemu_snapshots_external_delete(const char *ref, Error **errp);
void xemu_snapshots_external_mark_dirty(void);
void xemu_snapshots_external_free_data(XemuSnapshotData *data);

// Implemented in xemu-thumbnail.cc
void xemu_snapshots_set_framebuffer_texture(GLuint tex, bool flip);
bool xemu_snapshots_load_png_to_texture(GLuint tex, void *buf, size_t size);
void *xemu_snapshots_create_framebuffer_thumbnail_png(size_t *size);

#ifdef __cplusplus
}
#endif

#endif
