# External snapshot files

Status: opt-in feature. The default behaviour (snapshots stored inside the HDD
image) is unchanged.

## Motivation

xemu snapshots are QEMU internal snapshots: the VM state and a copy-on-write
point of the disk are stored inside the HDD qcow2 image
(`save_snapshot()`/`load_snapshot()` in `migration/savevm.c`, the image is
picked by `bdrv_all_find_vmstate_bs()`). This has a few consequences:

1. Any tool that rewrites the HDD image (repacking, converting, moving save
   games in and out of the image) silently destroys every snapshot.
2. A frontend cannot list, back up, compress or restore snapshots as files.
3. When loading, the DVD image path stored in the snapshot is compared with the
   mounted one as a string. The same disc served from another path triggers the
   "expected image" dialog.

## User-visible behaviour

New settings in `[general.snapshots]`:

| key        | values                     | default                     |
|------------|----------------------------|-----------------------------|
| `storage`  | `internal`, `external`     | `internal`                  |
| `dir`      | path                       | `<xemu data dir>/snapshots` |
| `compress` | bool                       | `true`                      |

`storage` can also be changed in the Snapshots tab ("Storage": Hard disk
image / Files). With `storage = 'external'`, every new snapshot is a
standalone file:

    <dir>/<TITLEID>/<name>.xsnap

`TITLEID` is the title id of the running XBE as 8 upper case hex digits
(`00000000` when no XBE is running). Characters that are not allowed in file
names are replaced with `_` in the file name; the real name is stored in the
file.

Snapshot names are scoped by title: saving `Quick` while running title A and
then while running title B creates two files. Name lookups (F5-F8 bindings,
`xemu_snapshots_load("Quick")`) look in the running title's folder first, then
in every folder (most recent first). Entries in the snapshot manager carry a
qualified id (`<TITLEID>/<file name>`) so the list always acts on the entry
that was clicked. As a result F5-F8 act as per-game slots with external
storage.

The snapshot manager lists the snapshots of both storages side by side
("HDD image" or "File" next to the date), and loading, replacing or deleting
an entry acts on the storage it lives in. `storage` decides where new
snapshots go (Create, Shift+F5-F8) and which storage a bare name is looked up
in first (F5-F8). Switching it moves nothing: internal snapshots stay in the
HDD image. Internal entries carry the id `HDD:<id>` in the list.

A frontend can restore a snapshot file at start:

    xemu -snapshot_file <path/to/file.xsnap> [-dvd_path <disc>]

The file is loaded once the machine is created, before it starts running
(the same point QEMU uses for `-loadvm`). The checks below apply; when one
fails the error is printed to the log and shown as an error notification,
nothing is modified, and the machine boots normally. `-snapshot_file` works
whatever `storage` is set to.

## Disc identity

The identity of a disc is read from the mounted image, not from guest memory:

- locate the XDVDFS volume descriptor (`MICROSOFT*XBOX*MEDIA` at sector 32,
  also probed at the game partition offset of full "redump" images);
- find `default.xbe` in the root directory;
- read its headers (`SizeOfHeaders` bytes, a few KB) and keep the title id,
  disc number and version from the certificate and a SHA-256 of the header
  block.

The header block contains the certificate and the SHA-1 digest of every
section, so its hash identifies the executable without reading the whole XBE.

At load, if the snapshot has a disc identity and the mounted disc has the same
one, the snapshot is loaded without asking, whatever its path. Otherwise the
existing dialog is shown with the stored path as a hint. Snapshots without an
identity (made by older builds) keep the path comparison.

The identity is also recorded in internal snapshots: it is appended after the
existing fields of the xemu data block. Older builds parse the fields they know
and ignore trailing bytes, and `xemu_snapshots_offset_extra_data()` skips the
block by its size, so the change is compatible both ways. This is a separate
commit.

## HDD state: options considered

The HDD state must be captured without writing to the user's image and
restored so that RAM and disk agree.

1. **Copy the top image file.** Simple, but it copies everything the file
   contains (internal snapshots, leaked clusters) and restoring means
   replacing a file the VM has open: close the BlockBackend's node, rename,
   reopen with the same options. The swap loses the internal snapshots stored
   in that file, and Windows refuses to rename over a file that is still open
   by another process (a frontend, an antivirus).
2. **Temporary internal snapshot, exported then deleted.** Still writes to the
   user's image (refcounts, snapshot table) and leaves it fragmented. Rejected:
   saving must not modify the image.
3. **Dirty bitmaps.** Persistent bitmaps are stored in the qcow2 image (writes
   to it); in-memory bitmaps do not survive a restart, so they cannot give a
   delta against an earlier snapshot file. Rejected.
4. **Delta against the previous snapshot file.** Small files, but they are no
   longer self-contained: deleting or losing one breaks the next ones.
   Rejected.
5. **Allocated extents of the top layer, relative to its backing chain
   (chosen).** For every range of the virtual disk, `bdrv_block_status()` on
   the top node tells whether the content comes from the top layer (data or
   zeroes) or from below. The file stores the top layer's extents and their
   data, plus a reference to what is below:
   - no backing file: below reads as zeroes, the file contains the whole disk
     content (only allocated ranges, so an 8 GB disk with 300 MB used is about
     300 MB before compression);
   - backing file (the overlay-per-game setup): below is the backing image,
     identified by its virtual size and a content fingerprint (XXH3-128 over
     its guest-visible content, cached per path/size/mtime for the session).
     The file only contains what the overlay holds, usually a few MB.

   This works the same for qcow2 overlays, standalone qcow2 images and raw
   images.

### Restore

Restoring writes into the image the VM has open, through its BlockBackend,
with the VM stopped. Nothing is reopened or renamed, so internal snapshots and
anything else in the image are kept.

The virtual disk is walked in order; a range is skipped when neither the
snapshot nor the current image has it in the top layer (both read the same
backing data). For other ranges the wanted content (snapshot data, zeroes, or
backing data) is compared with the current content and written only when it
differs:

- snapshot data: `blk_pwrite()`;
- zeroes: `blk_pwrite_zeroes(BDRV_REQ_MAY_UNMAP)`;
- backing content where the current image has its own data: the backing data
  is read and written into the top layer (qcow2 cannot "unallocate" a cluster
  over a backing file; a discard would turn it into a zero cluster).

The result is the exact guest-visible content of the disk at snapshot time.
The allocation map of the top layer may be a superset of the original one
(clusters allocated after the snapshot stay allocated, holding the backing
data). Writing only what differs avoids growing an image whose clusters are
shared with internal snapshots.

### Checks before anything is modified

Loading reads and verifies the whole file first (structure, CRC-32 of every
section), then checks:

- RAM size equal to the running machine's;
- HDD: same virtual size, and same "below" reference (no backing file in both,
  or a backing file with the same fingerprint);
- disc identity (UI: dialog, CLI: error);
- the HDD image path differs from the recorded one: the UI asks for
  confirmation (the load replaces that image's content), the CLI logs a
  warning.

Only then is the VM stopped, the disk restored and the VM state loaded
(`qemu_system_reset()` + `qemu_loadvm_state()`, as `load_snapshot()` does).
If the VM state fails to load after the disk was restored, the machine is left
paused with the error shown, like a failed internal snapshot load.

A missing HDD (no image configured, or a snapshot taken without one) only
matches a snapshot without disk section, and the other way round.

## File format (version 1)

All integers are little-endian.

    File header (32 bytes)
      0  char[8]  magic "XEMUSNAP"
      8  u32      format version (1)
     12  u32      header size (32)
     16  u32      flags (0)
     20  u32      reserved
     24  u64      reserved

    Section (repeated until end of file)
      0  u32      type (FourCC)
      4  u32      flags: bit 0 = payload is zlib-compressed
      8  u64      stored size (bytes following this 32-byte header)
     16  u64      raw size (uncompressed payload size)
     24  u32      CRC-32 of the raw payload
     28  u32      reserved

Unknown section types and unknown flags in reserved fields are skipped, so
readers stay compatible with additions. Sections are written in this order so
that listing only reads the beginning of each file:

- `META`: list of `u16 tag, u32 length, bytes` records:

  | tag | content                                        |
  |-----|------------------------------------------------|
  | 1   | name (UTF-8)                                   |
  | 2   | creation time, seconds since the epoch (u64)   |
  | 3   | creation time, nanoseconds (u32)               |
  | 4   | VM clock, ns (u64)                             |
  | 5   | xemu version (UTF-8)                           |
  | 6   | xemu commit (UTF-8)                            |
  | 7   | RAM size in bytes (u64)                        |
  | 8   | title id (u32)                                 |
  | 9   | title name (UTF-8)                             |
  | 10  | disc image path, hint only (UTF-8)             |
  | 11  | disc identity (see below)                      |
  | 12  | HDD image path, hint only (UTF-8)              |

  Disc identity (48 bytes): `u32 format (1), u32 title id, u32 version,
  u32 disc number, u8[32] SHA-256 of the default.xbe header block`.
- `THMB`: the thumbnail PNG (`xemu_snapshots_create_framebuffer_thumbnail_png`).
- `DISK`: HDD state, absent when no HDD is attached:

      0  u32      format version (1)
      4  u32      reserved
      8  u64      virtual size of the disk
     16  u32      below: 0 = zeroes (no backing file), 1 = backing image
     20  u8[16]   XXH3-128 fingerprint of the backing image content
     36  u64      backing image virtual size
     44  u32      length of the backing file name, then the name (hint only)
         u64      extent count, then for each extent:
                  u64 offset, u64 length, u32 type (1 = data, 2 = zero),
                  u32 reserved
         ...      data of the data extents, in table order

- `VMST`: QEMU's savevm stream, as written by `qemu_savevm_state()` (the same
  stream internal snapshots store, including xemu's own data block).

Compression uses zlib (always linked into QEMU; zstd is optional and not part
of the Windows toolchain), at a fast level. `compress = false` writes the
payloads as is.

## Code layout

- `ui/xemu-snapshots.c`: public `xemu_snapshots_*` functions list both
  storages and dispatch each entry to the internal implementation (unchanged)
  or the external one.
- `ui/xemu-snapshots-external.c`: external storage (directory layout, save,
  load, delete, list, HDD capture/restore).
- `ui/xemu-snapshot-file.c`: file format reader/writer, no dependency on the
  machine, covered by unit tests.
- `ui/xemu-disc-identity.c`: XDVDFS lookup of `default.xbe` and identity,
  covered by unit tests.
- `migration/savevm.c`: two small `XBOX`-only helpers to save/load the VM state
  to/from a `QEMUFile`, reusing `qemu_savevm_state()`/`qemu_loadvm_state()`.
- `system/vl.c`: `-snapshot_file`.

The VM state is serialized to memory (`QIOChannelBuffer`) and then compressed
into the file; loading decompresses it to memory. This costs a transient
buffer of the size of the VM state (about 70 MB for a 64 MB machine).

## Measurements

64 MB machine, zlib level 1, on a 2.1 GHz Xeon vCPU where `gzip -1` runs at
about 50 MB/s (a desktop CPU is 2-3 times faster). Sizes in MiB.

| Content                                          | Raw   | Compressed |
|--------------------------------------------------|-------|------------|
| VM state, RAM filled with program binaries       | 68.5  | 25.6       |
| VM state, 60 MB of random data in RAM            | 65.5  | 60.2       |
| VM state, mostly empty RAM                       | 5.6   | 0.07       |
| HDD, standalone 8 GB image with 300 MiB used     | 300.0 | 119.6      |
| HDD, overlay with 3 MiB written                  | 3.0   | depends on data |

A whole file for a standalone image with 300 MiB used and RAM filled with
binaries is 368.5 MiB uncompressed and 145.2 MiB compressed. With an overlay,
the file is mostly the VM state (25-60 MiB compressed), plus what the overlay
holds; games that use the X/Y/Z cache partitions can write hundreds of MB
there.

Time with the VM paused, same machine, standalone image with 300 MiB used:

|             | Compressed | Uncompressed |
|-------------|------------|--------------|
| Save        | 7.0 s      | 2.0 s        |
| Load        | 4.5 s      | 0.6 s        |

Loading includes the verification pass. When the disk did not change since
the snapshot, nothing is written. Compression dominates; an overlay with a
few MB takes well under a second either way.

## Limits

- The UI freezes while a snapshot is saved or loaded (the work runs on the UI
  thread with the VM paused, as for internal snapshots).

- Saving takes as long as reading the top layer of the HDD (the VM is paused
  meanwhile). It is fast for overlays and proportional to used space for
  standalone images. Internal snapshots are instant on the disk side.
- Each file of a standalone (non-overlay) HDD contains the whole used part of
  the disk. Using an overlay over a base image keeps the files small.
- The VM state is QEMU's migration stream: it is tied to the xemu version that
  wrote it like internal snapshots. The version is recorded and a mismatch is
  reported, but loading is attempted.
- The backing image must not change after a snapshot is taken; the
  fingerprint check refuses to load if it did.
- Discs that share the same `default.xbe` (some multi-disc sets) are
  distinguished by the disc number in the certificate only.
