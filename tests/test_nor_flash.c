#include "../libpcsxcore/arcade/nor_flash.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wr(nor_flash_t *f, uint32_t address, uint16_t value)
{
    assert(nor_flash_write(f, address, value) == NOR_FLASH_OK);
}

static uint16_t rd(nor_flash_t *f, uint32_t address)
{
    uint16_t value = 0;
    assert(nor_flash_read(f, address, &value) == NOR_FLASH_OK);
    return value;
}

static void amd_unlock(nor_flash_t *f, uint32_t command_address)
{
    wr(f, 0x555, 0xaa);
    wr(f, 0x2aa, 0x55);
    wr(f, command_address, 0x90);
}

static void amd_reset(nor_flash_t *f)
{
    wr(f, 0, 0xf0);
}

static void amd_program(nor_flash_t *f, uint32_t address, uint8_t value)
{
    wr(f, 0x555, 0xaa);
    wr(f, 0x2aa, 0x55);
    wr(f, 0x555, 0xa0);
    wr(f, address, value);
}

static void amd_sector_erase(nor_flash_t *f, uint32_t address)
{
    wr(f, 0x555, 0xaa); wr(f, 0x2aa, 0x55); wr(f, 0x555, 0x80);
    wr(f, 0x555, 0xaa); wr(f, 0x2aa, 0x55); wr(f, address, 0x30);
}

static void test_geometry_and_intel(void)
{
    const size_t size = 0x800000;
    uint8_t *image = (uint8_t *)malloc(size);
    nor_flash_t *f;
    uint16_t value;
    assert(image);
    memset(image, 0x5a, size);
    assert(nor_flash_create(NOR_FLASH_INTEL_28F640J5_X16, image, size - 1) == NULL);
    assert(nor_flash_create(NOR_FLASH_INTEL_28F640J5_X16, NULL, size) == NULL);
    f = nor_flash_create(NOR_FLASH_INTEL_28F640J5_X16, image, size);
    assert(f && nor_flash_bus_width(f) == 16);
    assert(nor_flash_image(f) == image && nor_flash_image_size(f) == size);
    assert(image[0] == 0x5a); /* constructor preserves borrowed image data */
    assert(nor_flash_read(f, (uint32_t)(size / 2), &value) == NOR_FLASH_ERR_RANGE);
    assert(nor_flash_write(f, (uint32_t)(size / 2), 0) == NOR_FLASH_ERR_RANGE);

    wr(f, 0, 0x90);
    assert(rd(f, 0) == 0x89 && rd(f, 1) == 0x15);
    assert(rd(f, 2) == 0 && rd(f, 3) == 0); /* MAME reports lock config clear */
    wr(f, 0, 0xff);
    wr(f, 0, 0x60); wr(f, 0, 0xf1);
    wr(f, 0, 0x90); assert(rd(f, 3) == 1);
    wr(f, 0, 0xff);
    wr(f, 0, 0x60); wr(f, 0, 0xd0);
    wr(f, 0, 0x90); assert(rd(f, 3) == 0);
    wr(f, 0, 0xff);

    /* Intel x16 program writes the following word and then reports ready. */
    image[0x200] = image[0x201] = 0xff;
    wr(f, 0x100, 0x40); wr(f, 0x100, 0x12f0);
    assert(rd(f, 0x100) == 0x80);
    wr(f, 0, 0xff);
    assert(rd(f, 0x100) == 0x12f0);
    wr(f, 0x100, 0x40); wr(f, 0x100, 0xffff);
    wr(f, 0, 0xff);
    assert(rd(f, 0x100) == 0x12f0); /* programming cannot set a zero bit */

    /* 0x20 / 0xd0 erases one 64 KiB block, with a one-second busy window. */
    wr(f, 0x20, 0x20); wr(f, 0x20, 0xd0);
    assert(image[0x40] == 0xff && image[0x41] == 0xff);
    assert(!nor_flash_ready(f));
    wr(f, 0, 0x70); assert(rd(f, 0) == 0);
    nor_flash_advance_us(f, 999999);
    assert(!nor_flash_ready(f) && rd(f, 0) == 0);
    nor_flash_advance_us(f, 1);
    assert(nor_flash_ready(f) && rd(f, 0) == 0x80);
    wr(f, 0, 0xff);

    wr(f, 0, 0x20);
    assert(nor_flash_write(f, 0, 0) == NOR_FLASH_ERR_SEQUENCE);
    assert(nor_flash_write(f, 0, 0x98) == NOR_FLASH_ERR_UNSUPPORTED);
    nor_flash_destroy(f);
    free(image);
}

static void test_gnet_intel_profile(nor_flash_profile_t profile, size_t size,
                                    uint8_t maker, uint16_t device)
{
    uint8_t *image = (uint8_t *)malloc(size);
    nor_flash_t *f;
    uint32_t target_word = 0x1234;
    uint32_t target_byte = target_word * 2;
    size_t dirty_offset = 0, dirty_length = 0;
    uint16_t value;
    assert(image);
    memset(image, 0x5a, size);
    assert(nor_flash_create(profile, image, size - 1) == NULL);
    f = nor_flash_create(profile, image, size);
    assert(f && nor_flash_bus_width(f) == 16);
    assert(nor_flash_image(f) == image && nor_flash_image_size(f) == size);
    assert(nor_flash_manufacturer_id(f) == maker);
    assert(nor_flash_device_id(f) == device);
    assert(nor_flash_sector_size(f, (uint32_t)(size / 4)) ==
           (profile == NOR_FLASH_INTEL_E28F400B_X16 ? 0x20000 : 0x10000));
    assert(nor_flash_read(f, (uint32_t)(size / 2), &value) == NOR_FLASH_ERR_RANGE);

    /* MAME constructors: E28F400B=(Intel,4471), TE28F160=(Sharp,d0). */
    wr(f, 0, 0x90);
    assert(rd(f, 0) == maker && rd(f, 1) == device);
    assert(rd(f, 2) == 0 && rd(f, 3) == 0);
    wr(f, 0, 0xff);

    /* The pinned intelfsh x16 FM_WRITEPART1 path assigns both bytes. */
    image[target_byte] = image[target_byte + 1] = 0x00;
    wr(f, target_word, 0x40);
    assert(!nor_flash_last_write_dirty_span(f, &dirty_offset, &dirty_length));
    wr(f, target_word, 0x12f0);
    assert(nor_flash_last_write_dirty_span(f, &dirty_offset, &dirty_length));
    assert(dirty_offset == target_byte && dirty_length == 2);
    assert(image[target_byte] == 0x12 && image[target_byte + 1] == 0xf0);
    assert(rd(f, target_word) == 0x80);
    wr(f, 0, 0xff);
    assert(rd(f, target_word) == 0x12f0);

    /* Intel block erase has 64 KiB sectors and a 1 s status interval. */
    wr(f, target_word, 0x20);
    wr(f, target_word, 0xd0);
    assert(nor_flash_last_write_dirty_span(f, &dirty_offset, &dirty_length));
    if (profile == NOR_FLASH_INTEL_E28F400B_X16) {
        assert(dirty_offset == 0 && dirty_length == 0x4000);
    } else {
        assert(dirty_offset == (target_byte & ~0xffffu) && dirty_length == 0x10000);
    }
    if (profile == NOR_FLASH_INTEL_E28F400B_X16) {
        assert(nor_flash_sector_size(f, 0) == 0x4000);
        assert(nor_flash_sector_size(f, 0x2000) == 0x2000);
        assert(nor_flash_sector_size(f, 0x3000) == 0x2000);
        assert(nor_flash_sector_size(f, 0x4000) == 0x18000);
        assert(image[0] == 0xff && image[0x3fff] == 0xff);
        assert(image[0x4000] == 0x5a); /* boot erase stops at 16 KiB */
    } else {
        assert(image[target_byte & ~0xffffu] == 0xff);
    }
    assert(!nor_flash_ready(f));
    wr(f, 0, 0x70);
    assert(rd(f, 0) == 0);
    nor_flash_advance_us(f, profile == NOR_FLASH_INTEL_E28F400B_X16 ? 300000 : 1000000);
    assert(nor_flash_ready(f) && rd(f, 0) == 0x80);
    wr(f, 0, 0xff);

    if (profile == NOR_FLASH_INTEL_E28F400B_X16) {
        image[0x8000] = image[0x1ffff] = 0;
        image[0x20000] = 0x5a;
        wr(f, 0x4000, 0x20); wr(f, 0x4000, 0xd0);
        assert(image[0x8000] == 0xff && image[0x1ffff] == 0xff);
        assert(image[0x20000] == 0x5a); /* 96 KiB main block boundary */
        assert(!nor_flash_ready(f));
        nor_flash_advance_us(f, 599999);
        assert(!nor_flash_ready(f));
        nor_flash_advance_us(f, 1);
        assert(nor_flash_ready(f));
    }

    /* Intel master lock command/status bits mirror MAME's FM_SETMASTER. */
    wr(f, 0, 0x60); wr(f, 0, 0xf1);
    wr(f, 0, 0x90); assert(rd(f, 3) == 1);
    wr(f, 0, 0xff);
    wr(f, 0, 0x60); wr(f, 0, 0xd0);
    wr(f, 0, 0x90); assert(rd(f, 3) == 0);
    wr(f, 0, 0xff);

    /* MAME's shared intelfsh AMD-compat sequence supports chip erase too. */
    image[0] = image[size - 1] = 0;
    wr(f, 0x555, 0xaa); wr(f, 0x2aa, 0x55); wr(f, 0x555, 0x80);
    wr(f, 0x555, 0xaa); wr(f, 0x2aa, 0x55); wr(f, 0x555, 0x10);
    assert(image[0] == 0xff && image[size - 1] == 0xff);
    assert(!nor_flash_ready(f));
    nor_flash_advance_us(f, 16000000);
    assert(nor_flash_ready(f));
    nor_flash_destroy(f);
    free(image);
}

static void test_amd_profile(nor_flash_profile_t profile, size_t size,
                             uint8_t maker, uint8_t device,
                             uint32_t sector, uint64_t sector_us,
                             int fujitsu_busy_all)
{
    uint8_t *image = (uint8_t *)malloc(size);
    nor_flash_t *f;
    uint16_t value;
    uint32_t target = sector * 2 + 17;
    uint8_t start_status, next_status;
    assert(image);
    memset(image, 0x00, size);
    f = nor_flash_create(profile, image, size);
    assert(f && nor_flash_bus_width(f) == 8);
    assert(nor_flash_manufacturer_id(f) == maker && nor_flash_device_id(f) == device);
    assert(nor_flash_sector_size(f, target) == sector);

    amd_unlock(f, 0x555);
    assert(rd(f, 0) == maker && rd(f, 1) == device && rd(f, 2) == 0);
    amd_reset(f);
    amd_program(f, target, 0x0f);
    assert(image[target] == 0x00); /* initial image had zero bits already */
    image[target] = 0xff;
    amd_program(f, target, 0x0f);
    assert(image[target] == 0x0f);
    amd_program(f, target, 0xff);
    assert(image[target] == 0x0f);

    image[target] = 0;
    image[target + 1] = 0x7e;
    image[0] = 0x11;
    amd_sector_erase(f, target);
    assert(image[target] == 0xff && image[target + 1] == 0xff);
    assert(!nor_flash_ready(f));
    start_status = (uint8_t)rd(f, target);
    next_status = (uint8_t)rd(f, target);
    assert((start_status ^ next_status) == 0x44); /* DQ6/DQ2 toggle */
    assert((rd(f, target - sector) == 0x4c) == !!fujitsu_busy_all);
    nor_flash_advance_us(f, sector_us - 1);
    assert(!nor_flash_ready(f));
    nor_flash_advance_us(f, 1);
    assert(nor_flash_ready(f) && rd(f, target) == 0xff);

    assert(nor_flash_read(f, (uint32_t)size, &value) == NOR_FLASH_ERR_RANGE);
    assert(nor_flash_write(f, (uint32_t)size, 0) == NOR_FLASH_ERR_RANGE);
    assert(nor_flash_write(f, 0, 0x98) == NOR_FLASH_ERR_UNSUPPORTED);

    /* Bad unlock order aborts the sequence and cannot program data. */
    wr(f, 0x555, 0xaa);
    assert(nor_flash_write(f, 0x123, 0x00) == NOR_FLASH_ERR_SEQUENCE);
    assert(image[0x123] == 0x00);

    if (profile == NOR_FLASH_AMD_29F040) {
        image[0] = 0;
        wr(f, 0x555, 0xaa); wr(f, 0x2aa, 0x55); wr(f, 0x555, 0x80);
        wr(f, 0x555, 0xaa); wr(f, 0x2aa, 0x55); wr(f, 0x555, 0x10);
        assert(image[0] == 0xff && !nor_flash_ready(f));
        nor_flash_advance_us(f, 15999999);
        assert(!nor_flash_ready(f));
        nor_flash_advance_us(f, 1);
        assert(nor_flash_ready(f));
    }
    nor_flash_destroy(f);
    free(image);
}

static void test_intel_buffer(void)
{
    uint8_t *image = malloc(0x800000);
    assert(image); memset(image, 0x5a, 0x800000);
    nor_flash_t *f = nor_flash_create(NOR_FLASH_INTEL_28F640J5_X16, image, 0x800000);
    size_t offset, length;
    wr(f, 0x20000, 0xe8); assert(rd(f, 0x20000) == 0x80);
    wr(f, 0x20000, 1); /* two x16 words */
    wr(f, 0x20000, 0x1234); wr(f, 0x20001, 0xabcd);
    assert(image[0x40000] == 0x5a);
    assert(!nor_flash_last_write_dirty_span(f, &offset, &length));
    wr(f, 0x20000, 0xd0);
    assert(nor_flash_last_write_dirty_span(f, &offset, &length));
    assert(offset == 0x40000 && length == 4);
    assert(rd(f, 0x20000) == 0x80); wr(f, 0, 0xff);
    assert(rd(f, 0x20000) == 0x1234 && rd(f, 0x20001) == 0xabcd);
    assert(rd(f, 0x1ffff) == 0x5a5a && rd(f, 0x20002) == 0x5a5a);
    /* Wrong confirmation and out-of-buffer addressing must abort atomically. */
    wr(f, 0x20000, 0xe8); wr(f, 0x20000, 0);
    wr(f, 0x20000, 0x0000); wr(f, 0x20000, 0x70);
    assert(rd(f, 0x20000) == 0x30); wr(f, 0, 0xff);
    assert(rd(f, 0x20000) == 0x1234);
    wr(f, 0x20000, 0xe8); wr(f, 0x20000, 1);
    wr(f, 0x20000, 0); wr(f, 0x20002, 0);
    assert(rd(f, 0x20000) == 0x30); wr(f, 0, 0xff);
    assert(rd(f, 0x20000) == 0x1234);
    wr(f, 0x3fffff, 0xe8); wr(f, 0x3fffff, 15);
    wr(f, 0x3fffff, 0);
    assert(rd(f, 0) == 0x30 && image[0x7fffff] == 0x5a);
    nor_flash_destroy(f); free(image);
    puts("Intel StrataFlash: buffered commit, dirty span, abort and bounds passed");
}

int main(void)
{
    test_geometry_and_intel();
    test_intel_buffer();
    test_gnet_intel_profile(NOR_FLASH_INTEL_E28F400B_X16, 0x80000,
                            0x89, 0x4471);
    test_gnet_intel_profile(NOR_FLASH_INTEL_TE28F160_X16, 0x200000,
                            0xb0, 0x00d0);
    test_amd_profile(NOR_FLASH_FUJITSU_MBM29F016A, 0x200000,
                     0x04, 0xad, 0x10000, 1000000, 1);
    test_amd_profile(NOR_FLASH_AMD_29F040, 0x80000,
                     0x01, 0xa4, 0x10000, 1000000, 0);
    test_amd_profile(NOR_FLASH_SST_39SF040, 0x80000,
                     0xbf, 0xb7, 0x1000, 125000, 0);
    puts("NOR flash synthetic tests passed");
    return 0;
}
