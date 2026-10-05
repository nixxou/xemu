/*
 * Tests for the xemu disc identity
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

#include "qemu/bswap.h"
#include "ui/xemu-disc-identity.h"

/*
 * A minimal XDVDFS image: volume descriptor, a root directory and a
 * default.xbe with a certificate.
 */
typedef struct TestDisc {
    uint8_t *data;
    size_t len;
    uint64_t partition;
} TestDisc;

#define SECTOR 2048

static void put_dirent(uint8_t *dir, uint32_t offset, uint16_t left,
                       uint16_t right, uint32_t sector, uint32_t size,
                       uint8_t attributes, const char *name)
{
    uint8_t *e = dir + offset;
    stw_le_p(e + 0, left);
    stw_le_p(e + 2, right);
    stl_le_p(e + 4, sector);
    stl_le_p(e + 8, size);
    e[12] = attributes;
    e[13] = strlen(name);
    memcpy(e + 14, name, strlen(name));
}

static TestDisc *make_disc(uint64_t partition, uint32_t title_id,
                           uint32_t disc_number, const char *xbe_name,
                           uint8_t salt)
{
    TestDisc *d = g_new0(TestDisc, 1);
    d->partition = partition;
    d->len = 40 * SECTOR;
    d->data = g_malloc0(d->len);

    uint8_t *vd = d->data + 32 * SECTOR;
    memcpy(vd, "MICROSOFT*XBOX*MEDIA", 20);
    stl_le_p(vd + 20, 33);
    stl_le_p(vd + 24, SECTOR);
    memcpy(vd + SECTOR - 20, "MICROSOFT*XBOX*MEDIA", 20);

    /* Root: "media" with "audio" on the left and the XBE on the right */
    uint8_t *dir = d->data + 33 * SECTOR;
    memset(dir, 0xff, SECTOR);
    put_dirent(dir, 0, 32 / 4, 64 / 4, 35, SECTOR, 0x10, "media");
    put_dirent(dir, 32, 0, 0, 36, 10, 0x20, "audio.bin");
    put_dirent(dir, 64, 0, 0, 37, 3 * SECTOR, 0x20, xbe_name);

    uint8_t *xbe = d->data + 37 * SECTOR;
    const uint32_t base = 0x10000;
    stl_le_p(xbe + 0, 0x48454258);
    stl_le_p(xbe + 0x104, base);
    stl_le_p(xbe + 0x108, 0x1000);
    stl_le_p(xbe + 0x118, base + 0x200);
    uint8_t *cert = xbe + 0x200;
    stl_le_p(cert + 8, title_id);
    stl_le_p(cert + 0xa8, disc_number);
    stl_le_p(cert + 0xac, 0x100);
    xbe[0x800] = salt;

    return d;
}

static void free_disc(TestDisc *d)
{
    g_free(d->data);
    g_free(d);
}

static bool read_disc(void *opaque, uint64_t offset, void *buf, size_t len)
{
    TestDisc *d = opaque;
    memset(buf, 0, len);
    for (size_t i = 0; i < len; i++) {
        uint64_t o = offset + i;
        if (o >= d->partition && o - d->partition < d->len) {
            ((uint8_t *)buf)[i] = d->data[o - d->partition];
        }
    }
    return true;
}

static bool compute(TestDisc *d, XemuDiscIdentity *id)
{
    return xemu_disc_identity_compute(read_disc, d, d->partition + d->len, id);
}

static void test_disc_identity(void)
{
    XemuDiscIdentity a, b, c, e;

    TestDisc *xiso = make_disc(0, 0x4d530004, 1, "default.xbe", 0);
    g_assert_true(compute(xiso, &a));
    g_assert_cmphex(a.title_id, ==, 0x4d530004);
    g_assert_cmpuint(a.disc_number, ==, 1);
    g_assert_cmphex(a.version, ==, 0x100);

    /* Same XBE in a full image (game partition offset), name in caps */
    TestDisc *redump = make_disc(0x18300000, 0x4d530004, 1, "DEFAULT.XBE", 0);
    g_assert_true(compute(redump, &b));
    g_assert_true(xemu_disc_identity_equal(&a, &b));

    /* Different header content */
    TestDisc *other = make_disc(0, 0x4d530004, 1, "default.xbe", 1);
    g_assert_true(compute(other, &c));
    g_assert_false(xemu_disc_identity_equal(&a, &c));

    /* Other disc of the same title */
    TestDisc *disc2 = make_disc(0, 0x4d530004, 2, "default.xbe", 0);
    g_assert_true(compute(disc2, &c));
    g_assert_false(xemu_disc_identity_equal(&a, &c));

    /* No default.xbe */
    TestDisc *none = make_disc(0, 0x4d530004, 1, "other.xbe", 0);
    g_assert_false(compute(none, &e));

    /* Not an Xbox disc */
    TestDisc *blank = make_disc(0, 0, 0, "default.xbe", 0);
    memset(blank->data + 32 * SECTOR, 0, SECTOR);
    g_assert_false(compute(blank, &e));

    /* Directory entries pointing outside of the directory */
    TestDisc *broken = make_disc(0, 0, 0, "x", 0);
    stw_le_p(broken->data + 33 * SECTOR, 0xfff0);
    g_assert_false(compute(broken, &e));

    uint8_t buf[XEMU_DISC_IDENTITY_SERIALIZED_SIZE];
    xemu_disc_identity_serialize(&a, buf);
    g_assert_true(xemu_disc_identity_deserialize(buf, sizeof(buf), &e));
    g_assert_true(xemu_disc_identity_equal(&a, &e));
    g_assert_false(xemu_disc_identity_deserialize(buf, sizeof(buf) - 1, &e));

    free_disc(xiso);
    free_disc(redump);
    free_disc(other);
    free_disc(disc2);
    free_disc(none);
    free_disc(blank);
    free_disc(broken);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xemu-disc-identity/compute", test_disc_identity);
    return g_test_run();
}
