/*
 * Tests for the xemu snapshot file format
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

#include "qemu/osdep.h"

#include <glib/gstdio.h>

#include "qapi/error.h"
#include "qemu/bswap.h"
#include "ui/xemu-snapshot-file.h"

#define TEST_SECTION_UNKNOWN XEMU_SNAPSHOT_FOURCC('T', 'E', 'S', 'T')

static char *tmp_dir;

static char *tmp_path(const char *name)
{
    return g_build_filename(tmp_dir, name, NULL);
}

static uint8_t *make_payload(size_t len, uint32_t seed)
{
    uint8_t *buf = g_malloc(len);
    uint32_t x = seed;
    for (size_t i = 0; i < len; i++) {
        /* Compressible but not trivial */
        x = x * 1103515245 + 12345;
        buf[i] = (i % 4096 < 2048) ? 0 : (x >> 16);
    }
    return buf;
}

static void write_test_file(const char *path, bool compress, size_t big_len)
{
    g_autofree uint8_t *big = make_payload(big_len, 1);
    g_autoptr(GByteArray) meta = g_byte_array_new();
    xemu_snapshot_meta_put_str(meta, XEMU_SNAPSHOT_META_NAME, "Quick save");
    xemu_snapshot_meta_put_u32(meta, XEMU_SNAPSHOT_META_TITLE_ID, 0x4d530004);
    xemu_snapshot_meta_put_u64(meta, XEMU_SNAPSHOT_META_RAM_SIZE,
                               64 * 1024 * 1024);

    XemuSnapshotWriter *w =
        xemu_snapshot_writer_open(path, compress, &error_abort);
    g_assert_true(xemu_snapshot_writer_write_section(
        w, XEMU_SNAPSHOT_SECTION_META, meta->data, meta->len, &error_abort));
    g_assert_true(xemu_snapshot_writer_write_section(w, TEST_SECTION_UNKNOWN,
                                                     NULL, 0, &error_abort));

    /* Written in uneven pieces */
    g_assert_true(xemu_snapshot_writer_begin_section(
        w, XEMU_SNAPSHOT_SECTION_VMSTATE, &error_abort));
    size_t done = 0;
    for (size_t step = 1; done < big_len; step = step * 3 + 7) {
        size_t n = MIN(step, big_len - done);
        g_assert_true(
            xemu_snapshot_writer_write(w, big + done, n, &error_abort));
        done += n;
    }
    g_assert_true(xemu_snapshot_writer_end_section(w, &error_abort));
    g_assert_true(xemu_snapshot_writer_close(w, &error_abort));
}

static void test_roundtrip(bool compress)
{
    g_autofree char *path = tmp_path(compress ? "rt-z.xsnap" : "rt.xsnap");
    const size_t big_len = 3 * 1024 * 1024 + 123;
    write_test_file(path, compress, big_len);

    g_assert_true(xemu_snapshot_file_verify(path, &error_abort));

    XemuSnapshotReader *r = xemu_snapshot_reader_open(path, &error_abort);
    XemuSnapshotSection s;

    g_assert_true(xemu_snapshot_reader_next_section(r, &s, &error_abort));
    g_assert_cmphex(s.type, ==, XEMU_SNAPSHOT_SECTION_META);
    g_assert_cmpuint(!!(s.flags & XEMU_SNAPSHOT_SECTION_FLAG_ZLIB), ==,
                     compress);
    size_t meta_len;
    g_autofree uint8_t *meta =
        xemu_snapshot_reader_read_section(r, &meta_len, &error_abort);
    g_autofree char *name =
        xemu_snapshot_meta_get_str(meta, meta_len, XEMU_SNAPSHOT_META_NAME);
    g_assert_cmpstr(name, ==, "Quick save");
    uint32_t title_id;
    g_assert_true(xemu_snapshot_meta_get_u32(
        meta, meta_len, XEMU_SNAPSHOT_META_TITLE_ID, &title_id));
    g_assert_cmphex(title_id, ==, 0x4d530004);
    uint64_t ram_size;
    g_assert_true(xemu_snapshot_meta_get_u64(
        meta, meta_len, XEMU_SNAPSHOT_META_RAM_SIZE, &ram_size));
    g_assert_cmpuint(ram_size, ==, 64 * 1024 * 1024);
    g_assert_null(xemu_snapshot_meta_get_str(meta, meta_len,
                                             XEMU_SNAPSHOT_META_DISC_PATH));
    /* Wrong size for the type */
    g_assert_false(xemu_snapshot_meta_get_u64(
        meta, meta_len, XEMU_SNAPSHOT_META_TITLE_ID, &ram_size));

    /* Unknown sections can be skipped */
    g_assert_true(xemu_snapshot_reader_find_section(
        r, XEMU_SNAPSHOT_SECTION_VMSTATE, &s, &error_abort));
    g_assert_cmpuint(s.raw_size, ==, big_len);
    if (compress) {
        g_assert_cmpuint(s.stored_size, <, big_len);
    } else {
        g_assert_cmpuint(s.stored_size, ==, big_len);
    }

    g_autofree uint8_t *expected = make_payload(big_len, 1);
    g_autofree uint8_t *got = g_malloc(big_len);
    size_t done = 0;
    for (size_t step = 5; done < big_len; step = step * 2 + 1) {
        size_t n = MIN(step, big_len - done);
        g_assert_true(
            xemu_snapshot_reader_read(r, got + done, n, &error_abort));
        done += n;
    }
    g_assert_true(memcmp(expected, got, big_len) == 0);

    /* Reading past the end of the section fails */
    Error *err = NULL;
    uint8_t byte;
    g_assert_false(xemu_snapshot_reader_read(r, &byte, 1, &err));
    g_assert_nonnull(err);
    error_free(err);

    g_assert_true(xemu_snapshot_reader_finish_section(r, &error_abort));
    g_assert_false(xemu_snapshot_reader_next_section(r, &s, &error_abort));
    xemu_snapshot_reader_close(r);
    g_unlink(path);
}

static void test_roundtrip_plain(void)
{
    test_roundtrip(false);
}

static void test_roundtrip_zlib(void)
{
    test_roundtrip(true);
}

/* A negative @offset is a fraction of the file size, in percent */
static void corrupt_file(const char *path, gssize offset, uint8_t xor)
{
    g_autofree char *contents = NULL;
    gsize len;
    g_assert_true(g_file_get_contents(path, &contents, &len, NULL));
    if (offset < 0) {
        offset = len * -offset / 100;
    }
    g_assert_cmpint(offset, <, len);
    contents[offset] ^= xor;
    g_assert_true(g_file_set_contents(path, contents, len, NULL));
}

static void test_corruption(gconstpointer opaque)
{
    bool compress = GPOINTER_TO_INT(opaque);
    g_autofree char *path = tmp_path("corrupt.xsnap");
    Error *err = NULL;

    /* Payload of the last section */
    write_test_file(path, compress, 100000);
    corrupt_file(path, -75, 0x40);
    g_assert_false(xemu_snapshot_file_verify(path, &err));
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;

    /* Magic */
    write_test_file(path, compress, 1000);
    corrupt_file(path, 0, 0xff);
    g_assert_null(xemu_snapshot_reader_open(path, &err));
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;

    /* Version */
    write_test_file(path, compress, 1000);
    corrupt_file(path, 8, 0x02);
    g_assert_null(xemu_snapshot_reader_open(path, &err));
    g_assert_nonnull(strstr(error_get_pretty(err), "version"));
    error_free(err);
    err = NULL;

    /* Truncated */
    write_test_file(path, compress, 100000);
    g_autofree char *contents = NULL;
    gsize len;
    g_assert_true(g_file_get_contents(path, &contents, &len, NULL));
    g_assert_true(g_file_set_contents(path, contents, len - 100, NULL));
    g_assert_false(xemu_snapshot_file_verify(path, &err));
    g_assert_nonnull(err);
    error_free(err);

    g_unlink(path);
}

static void test_writer_abort(void)
{
    g_autofree char *path = tmp_path("abort.xsnap");
    XemuSnapshotWriter *w = xemu_snapshot_writer_open(path, true, &error_abort);
    g_assert_true(xemu_snapshot_writer_begin_section(
        w, XEMU_SNAPSHOT_SECTION_DISK, &error_abort));
    g_assert_true(xemu_snapshot_writer_write(w, "abc", 3, &error_abort));
    xemu_snapshot_writer_abort(w);
    g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
}

static void test_meta_malformed(void)
{
    g_autoptr(GByteArray) meta = g_byte_array_new();
    xemu_snapshot_meta_put_str(meta, XEMU_SNAPSHOT_META_NAME, "abc");
    xemu_snapshot_meta_put_str(meta, XEMU_SNAPSHOT_META_TITLE_NAME, "def");

    /* Truncated record */
    uint32_t len;
    g_assert_nonnull(xemu_snapshot_meta_get(meta->data, meta->len,
                                            XEMU_SNAPSHOT_META_NAME, &len));
    g_assert_null(xemu_snapshot_meta_get(meta->data, meta->len - 1,
                                         XEMU_SNAPSHOT_META_TITLE_NAME, &len));

    /* Record length larger than the buffer */
    stl_le_p(meta->data + 2, 1000);
    g_assert_null(xemu_snapshot_meta_get(meta->data, meta->len,
                                         XEMU_SNAPSHOT_META_TITLE_NAME, &len));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    tmp_dir = g_dir_make_tmp("test-xemu-snapshot-XXXXXX", NULL);
    g_assert_nonnull(tmp_dir);

    g_test_add_func("/xemu-snapshot-file/roundtrip/plain",
                    test_roundtrip_plain);
    g_test_add_func("/xemu-snapshot-file/roundtrip/zlib", test_roundtrip_zlib);
    g_test_add_data_func("/xemu-snapshot-file/corruption/plain",
                         GINT_TO_POINTER(false), test_corruption);
    g_test_add_data_func("/xemu-snapshot-file/corruption/zlib",
                         GINT_TO_POINTER(true), test_corruption);
    g_test_add_func("/xemu-snapshot-file/writer-abort", test_writer_abort);
    g_test_add_func("/xemu-snapshot-file/meta-malformed", test_meta_malformed);

    int ret = g_test_run();

    g_rmdir(tmp_dir);
    g_free(tmp_dir);
    return ret;
}
