#include "../libpcsxcore/arcade/namcos10_board.h"
#include "test_namcos10_crypto_vectors.inc"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

static uint32_t readv(NS10Board *b, uint32_t a, unsigned w = 2)
{
    uint32_t v = 0xdeadbeef;
    assert(ns10_board_read(b, a, w, &v) == NS10_BOARD_OK);
    return v;
}
static void writev(NS10Board *b, uint32_t a, uint16_t v)
{
    assert(ns10_board_write(b, a, 2, v, 0xffff) == NS10_BOARD_OK);
}

static void memm_tests()
{
    std::vector<uint8_t> mainrom(0x400000, 0x55);
    std::vector<uint8_t> flash(0x800000, 0xff);
    std::vector<uint8_t> nand(0x800000, 0);
    std::vector<uint8_t> data(0x2000000, 0);
    for (unsigned i = 0; i < 32; i++) {
        nand[i * 2] = uint8_t(cipherwords[i]);
        nand[i * 2 + 1] = uint8_t(cipherwords[i] >> 8);
    }
    nand[0] = 0x57; nand[1] = 0x13;
    data[0] = 0x57; data[1] = 0x13;
    NS10BoardConfig cfg{};
    cfg.kind = NS10_BOARD_MEMM; cfg.game_profile = "mrdrilr2";
    cfg.program_rom = mainrom.data(); cfg.program_rom_size = mainrom.size();
    cfg.memm_flash_image = flash.data(); cfg.memm_flash_size = flash.size();
    cfg.memm_nand_region = nand.data(); cfg.memm_nand_region_size = nand.size();
    cfg.memm_data_region = data.data(); cfg.memm_data_region_size = data.size();
    NS10IO *io = nullptr;
    unsigned irq10_ticks = 0;
    NS10IOConfig io_cfg{};
    io_cfg.opaque = &irq10_ticks;
    io_cfg.exio_ident_code = 0xff;
    io_cfg.read_in1 = [](void *) -> uint32_t { return 0x1234abcd; };
    io_cfg.irq10 = [](void *p, int state) { assert(state == 1); ++*static_cast<unsigned *>(p); };
    assert(ns10_io_create(&io_cfg, &io) == NS10_IO_OK);
    cfg.io = io;
    NS10Board *b = nullptr;
    assert(ns10_board_create(&cfg, &b) == NS10_BOARD_OK && b);
    /* MAME init copies the first 0x62000 bytes and unscrambles little-endian
     * words. 0x1357 maps to 0xe9ef for the Mr. Driller 2 wiring. */
    assert(mainrom[0] == 0xef && mainrom[1] == 0xe9);
    assert(ns10_board_data_xor(b) == 0xaaaa);
    assert(readv(b, 0x1fb40000) == 0); /* bank_r switches to ROM-region path */
    assert(readv(b, 0x1f400000) == 0xe9ef);
    writev(b, 0x1fb40000, 0x7777); /* bank_w selects by address offset, not data */
    assert(readv(b, 0x1f400000) == 0xffff); /* Intel array read follows flash bytes */
    writev(b, 0x1f400000, 0x0090);
    assert(readv(b, 0x1f400000) == 0x0089 && readv(b, 0x1f400002) == 0x0015);
    writev(b, 0x1f400000, 0x00ff);
    writev(b, 0x1f400200, 0x0040);
    writev(b, 0x1f400200, 0x12f0);
    ns10_board_advance_us(b, 1);
    writev(b, 0x1f400000, 0x00ff);
    assert(readv(b, 0x1f400200) == 0x12f0); /* Intel x16 program through board bus */
    assert(readv(b, 0x1fb40000) == 0);
    for (unsigned i = 0; i < 32; i++) {
        nand[i * 2] = uint8_t(cipherwords[i]);
        nand[i * 2 + 1] = uint8_t(cipherwords[i] >> 8);
    }
    writev(b, 0x1f300000, 0x8003);
    assert(ns10_board_crypto_active(b));
    for (unsigned i = 0; i < 16; i++)
        assert(readv(b, 0x1f400000 + i * 2) == vectors[0].plain[i]);
    ns10_board_reset(b);
    writev(b, 0x1fb40020, 0); /* bank 0x10 selects data ROM bank 0 */
    assert(readv(b, 0x1f400000) == 0x1357);
    uint32_t value = 0x12345678;
    assert(ns10_board_read(b, 0x1fbf0000, 2, &value) == NS10_BOARD_OK && value == 1);
    assert(ns10_board_write(b, 0x1fba0010, 2, 0, 0xffff) == NS10_BOARD_UNMAPPED);
    assert(ns10_board_write(b, 0x1fba001a, 2, 0, 0xffff) == NS10_BOARD_UNMAPPED);
    assert(ns10_board_read(b, 0x1fbc0000, 2, &value) == NS10_BOARD_UNIMPLEMENTED);
    assert(ns10_board_write(b, 0x1fe00000, 2, 0, 0xffff) == NS10_BOARD_UNMAPPED);
    assert(ns10_board_read(b, 0x1fc00000, 2, &value) == NS10_BOARD_UNMAPPED);
    assert(ns10_board_read(b, 0x1f100000, 2, &value) == NS10_BOARD_UNMAPPED);
    writev(b, 0x1f300000, 0);
    /* The high byte reaches the control handler; IV 0 is unknown for this
     * type-1 CPLD, so it must fail explicitly rather than invent its key. */
    assert(ns10_board_write(b, 0x1f300001, 1, 0x80, 0xff) == NS10_BOARD_CRYPTO);
    assert(!ns10_board_crypto_active(b));
    writev(b, 0x1f300000, 0x8003);
    assert(ns10_board_crypto_active(b));
    assert(ns10_board_write(b, 0x1f300000, 1, 0, 0) == NS10_BOARD_OK);
    assert(ns10_board_crypto_active(b)); /* An inactive bus mask must not deactivate. */
    assert(ns10_board_write(b, 0x1f300000, 1, 0, 0xff) == NS10_BOARD_OK);
    assert(!ns10_board_crypto_active(b));
    assert(readv(b, 0xbfba0004, 4) == 0x1234abcd);
    writev(b, 0x1fba0002, 0);
    assert(readv(b, 0x1fba0002) == 3);
    ns10_board_reset(b);
    irq10_ticks = 0;
    ns10_board_advance_us(b, 0);
    assert(readv(b, 0x1fba001a) == 8 && irq10_ticks == 1);
    writev(b, 0x1fba0018, 0);
    ns10_board_advance_us(b, 9999);
    assert(readv(b, 0x1fba001a) == 0 && irq10_ticks == 1);
    ns10_board_advance_us(b, 1);
    assert(readv(b, 0x1fba001a) == 8 && irq10_ticks == 2);
    writev(b, 0x1fba0018, 0);
    assert(readv(b, 0x1fba001a) == 0);
    writev(b, 0x1fba0008, 0);
    assert(readv(b, 0x1fba0008) == 0);
    ns10_board_reset(b);
    assert(readv(b, 0x1fba0008) == 1 && readv(b, 0x1fba001a) == 0);
    ns10_board_destroy(b);
    assert(ns10_io_read(io, 0x1fba0008, 2, &value) == NS10_IO_OK && value == 1);
    ns10_io_destroy(io); /* Board destruction leaves the borrowed I/O alive. */
}

static void nand_program(NS10Board *b, uint32_t col, uint8_t row, uint8_t page)
{
    writev(b, 0x1f410000, 0x0000);
    writev(b, 0x1f420000, uint16_t(col));
    writev(b, 0x1f430000, row);
    writev(b, 0x1f440000, page);
}

static void memn_tests()
{
    const uint32_t pages = 0x2020;
    std::vector<uint8_t> nand0(size_t(pages) * 528, 0xff);
    std::vector<uint8_t> nand1(size_t(pages) * 528, 0xff);
    std::vector<uint8_t> mainrom(0x400000, 0x55);
    const uint8_t *images[] = {nand0.data(), nand1.data()};
    for (unsigned i = 0; i < 32; i++) {
        size_t off = size_t(0x40) * 528 + (i + 1) * 2;
        nand0[off] = uint8_t(cipherwords[i] >> 8); /* NAND wire: high byte first */
        nand0[off + 1] = uint8_t(cipherwords[i]);
        nand1[off] = uint8_t(cipherwords[i] >> 8);
        nand1[off + 1] = uint8_t(cipherwords[i]);
    }
    nand0[0x40 * 528] = 0x13;
    nand0[0x40 * 528 + 1] = 0x57;
    NS10NANDConfig chips[2] = {
        {images[0], nand0.size(), pages, 0xec, 0x73, 1, nullptr},
        {images[1], nand1.size(), pages, 0xec, 0x73, 1, nullptr}
    };
    NS10BoardConfig cfg{};
    cfg.kind = NS10_BOARD_MEMN; cfg.game_profile = "gahaha";
    cfg.program_rom = mainrom.data(); cfg.program_rom_size = mainrom.size();
    cfg.nand_chips = chips; cfg.nand_chip_count = 2;
    NS10Board *b = nullptr;
    assert(ns10_board_create(&cfg, &b) == NS10_BOARD_OK && b);
    assert(ns10_board_write(b, 0x1f500000, 2, 0x1234, 0xffff) == NS10_BOARD_OK);
    assert(ns10_board_write(b, 0x1f500000, 2, 0x00ab, 0x00ff) == NS10_BOARD_OK);
    assert(readv(b, 0x1f500000) == 0x12ab); /* one-bits-written COMBINE_DATA */
    assert(mainrom[0] == 0xfe && mainrom[1] == 0xb6); /* independent passive vector */
    assert(mainrom[0x20000] != 0x55); /* second MAME boot-copy range */
    assert(ns10_board_data_xor(b) == 0xaaaa);
    assert(ns10_board_write(b, 0x1f500000, 2, 0x1111, 0xffff) == NS10_BOARD_OK);
    assert(ns10_board_write(b, 0x1f500000, 2, 0xffff, 0) == NS10_BOARD_OK);
    assert(readv(b, 0x1f500000) == 0x1111);
    assert(ns10_board_write(b, 0x1f500000, 1, 0xff, 0xf0) == NS10_BOARD_OK);
    assert(ns10_board_write(b, 0x1f500001, 1, 0x55, 0x0f) == NS10_BOARD_OK);
    assert(readv(b, 0x1f500000) == 0x15f1);
    assert(ns10_board_write(b, 0x1f410000, 2, 0x8000, 0xff00) == NS10_BOARD_OK);
    assert(readv(b, 0x1f400000) == 0); /* MAME reset-state callback register */
    assert(ns10_board_write(b, 0x1f410000, 2, 0x0030, 0xffff) == NS10_BOARD_UNSUPPORTED);
    writev(b, 0x1f410000, 0x00ff);
    assert(readv(b, 0x1f400000) == 0); /* NAND callbacks 0,1 map to stored 1,0 */
    nand_program(b, 0, 0x40, 0);
    writev(b, 0x1f410000, 0x0030); /* valid single-column read confirm */
    assert(readv(b, 0x1f400000) == 0); /* 0->1 callback pulse, stored active-low */
    assert(readv(b, 0x1f450000) == 0xb6fe); /* wire byte order + passive block decode */
    assert(ns10_board_write(b, 0x1f410000, 2, 0x0080, 0xffff) == NS10_BOARD_UNSUPPORTED);
    assert(readv(b, 0x1f400000) == 0);
    assert(ns10_board_write(b, 0x1f460000, 2, 1, 0xffff) == NS10_BOARD_OK);
    assert(ns10_board_selected_nand(b) == 1);
    assert(readv(b, 0x1f400000) == 0); /* R/B state is tracked per chip */
    writev(b, 0x1f470000, 4);
    writev(b, 0x1f460000, 0);
    assert(ns10_board_selected_nand(b) == 1); /* controller lock bit */
    writev(b, 0x1f470000, 0);
    writev(b, 0x1f460000, 0);
    assert(ns10_board_selected_nand(b) == 0);
    writev(b, 0x1f300000, 0x8000); /* IV0 from the MAME profile */
    assert(ns10_board_crypto_active(b));
    nand_program(b, 2, 0x40, 0);
    uint8_t dma[64]{};
    assert(ns10_board_dma_read(b, dma, sizeof(dma)) == NS10_BOARD_OK);
    for (unsigned i = 0; i < 32; i++) {
        assert(dma[i * 2] == uint8_t(vectors[23].plain[i]));
        assert(dma[i * 2 + 1] == uint8_t(vectors[23].plain[i] >> 8));
    }
    /* Bank/protection peripheral maps are deliberately errors, not fake data. */
    uint32_t value = 0;
    assert(ns10_board_read(b, 0x1fba0004, 2, &value) == NS10_BOARD_UNIMPLEMENTED);
    assert(ns10_board_read(b, 0x1fe00000, 2, &value) == NS10_BOARD_UNMAPPED);
    assert(ns10_board_read(b, 0x1fc00000, 2, &value) == NS10_BOARD_UNMAPPED);
    assert(ns10_board_write(b, 0x1f450000, 2, 0x1234, 0xffff) == NS10_BOARD_UNSUPPORTED);
    ns10_board_destroy(b);

    // Optional writable NAND must preserve MAME's two high-byte-first data
    // strobes through the actual CPU-visible controller, not host word lanes.
    chips[0].writable_bytes = nand0.data();
    assert(ns10_board_create(&cfg, &b) == NS10_BOARD_OK);
    writev(b, 0x1f410000, 0x60);
    writev(b, 0x1f420000, 40); writev(b, 0x1f430000, 0);
    writev(b, 0x1f410000, 0xd0);
    assert(readv(b, 0x1f400000) == 0); // completed R/B low/high pulse
    writev(b, 0x1f410000, 0x00); writev(b, 0x1f410000, 0x80);
    writev(b, 0x1f420000, 0); writev(b, 0x1f430000, 40); writev(b, 0x1f440000, 0);
    writev(b, 0x1f450000, 0x1234); writev(b, 0x1f410000, 0x10);
    assert(nand0[40 * 528] == 0x12 && nand0[40 * 528 + 1] == 0x34);
    ns10_board_reset(b);
    nand_program(b, 0, 40, 0);
    assert(readv(b, 0x1f450000) == 0x1234); // settings block bypasses passive crypto
    ns10_board_destroy(b);
    chips[0].writable_bytes = nullptr;

    cfg.game_profile = "ippo";
    b = nullptr;
    assert(ns10_board_create(&cfg, &b) == NS10_BOARD_OK && b);
    assert(ns10_board_data_xor(b) == 0x5300);
    writev(b, 0x1f468000, 0x1234);
    assert(ns10_board_data_xor(b) == 0x3412); /* MEM/IO byte-swapped XOR register */
    writev(b, 0x1f300000, 0x8000); /* absent MAME decrypter stays inert */
    assert(!ns10_board_crypto_active(b));
    ns10_board_destroy(b);
}

int main()
{
    memm_tests();
    memn_tests();
    std::puts("Namco System 10 MEM(M)/MEM(N) board maps, NAND boot copy, crypto vectors, DMA and explicit peripheral errors PASS");
}
