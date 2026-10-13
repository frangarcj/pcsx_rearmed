/* SPDX-License-Identifier: BSD-3-Clause
 * copyright-holders:smf,windyfairy
 * Namco System 10 board maps and access sequences are derived from the MAME
 * namcos10.cpp driver at the revision documented in docs/namcos10-board.md.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Standalone API, ownership, and host byte-lane adapter: GPL-2.0-or-later.
 */
#include "namcos10_board.h"
#include "nor_flash.h"

#include <algorithm>
#include <cstring>
#include <new>

struct NS10Board {
    NS10BoardKind kind;
    char profile[64];
    uint8_t *program_rom;
    size_t program_rom_size;
    const uint8_t *memm_nand;
    size_t memm_nand_size;
    const uint8_t *memm_data;
    size_t memm_data_size;
    nor_flash_t *flash;
    NS10NAND **nands;
    size_t nand_count;
    NS10Crypto *crypto;
    uint8_t *shared_ram;
    uint32_t bank_index, nand_address, ctrl_reg;
    size_t selected_nand;
    int banked_region_mode;
    uint8_t nand_rnb_state[16];
    NS10IO *io;
};

static uint32_t physical(uint32_t a)
{
    if (a >= 0x80000000u && a < 0xc0000000u)
        return a & 0x1fffffffu;
    return a;
}

static int peripheral_unimplemented(uint32_t a)
{
    /* Only actual expansion apertures are unsupported. Unmapped holes in
     * the base I/O map (e.g. the BIOS writes 1fba0010) are not devices. */
    return (a >= 0x1fba0000u && a <= 0x1fba000bu) ||
        (a >= 0x1fba0012u && a <= 0x1fba001bu) ||
        (a >= 0x1fbc0000u && a <= 0x1fbc000fu);
}

static void nand_rnb_callback(NS10Board *b, size_t chip, int pin_state)
{
    if (chip < b->nand_count)
        b->nand_rnb_state[chip] = uint8_t(pin_state != 1);
}

static uint16_t load_le16(const uint8_t *p)
{
    return uint16_t(p[0] | (uint16_t(p[1]) << 8));
}

static void store_le16(uint8_t *p, uint16_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

static uint32_t lane_mask(unsigned bytes)
{
    return bytes == 1 ? 0xffu : bytes == 2 ? 0xffffu : 0xffffffffu;
}

static uint16_t combine(uint16_t old_value, uint16_t data, uint16_t mem_mask)
{
    /* MAME COMBINE_DATA convention: one bits of mem_mask are written. */
    return uint16_t((old_value & ~mem_mask) | (data & mem_mask));
}

static int read_memm_word(NS10Board *b, uint32_t address, uint16_t *value)
{
    uint32_t offset = (address - 0x1f400000u) >> 1;
    uint64_t word_address = uint64_t(b->bank_index) * 0x100000u + offset;
    const uint8_t *region;
    size_t region_size;
    uint16_t raw;
    if (b->bank_index >= 0x20)
        return NS10_BOARD_RANGE;
    if (!b->banked_region_mode) {
        if (nor_flash_read(b->flash, uint32_t(word_address), value) != NOR_FLASH_OK)
            return NS10_BOARD_FLASH;
        return NS10_BOARD_OK;
    }
    if (b->bank_index < 0x10) {
        region = b->memm_nand;
        region_size = b->memm_nand_size;
    } else {
        uint64_t base = uint64_t(b->bank_index - 0x10) * 0x200000u;
        if (base + 0x200000u > b->memm_data_size)
            return NS10_BOARD_RANGE;
        region = b->memm_data + base;
        region_size = b->memm_data_size - size_t(base);
        word_address = offset;
    }
    if (word_address * 2 + 1 >= region_size)
        return NS10_BOARD_RANGE;
    raw = load_le16(region + size_t(word_address) * 2);
    *value = ns10_crypto_memm_bank_word(b->crypto, raw, b->bank_index,
                                        !b->banked_region_mode);
    return NS10_BOARD_OK;
}

static NS10NAND *selected_nand(NS10Board *b)
{
    return b->selected_nand < b->nand_count ? b->nands[b->selected_nand] : nullptr;
}

static int read_word(NS10Board *b, uint32_t address, uint16_t *value)
{
    if (address >= 0x1fbf0000u && address <= 0x1fbf000eu) {
        *value = 1; /* MAME cdrom_unk_r: board drive-presence probe. */
        return NS10_BOARD_OK;
    }
    if (peripheral_unimplemented(address)) return NS10_BOARD_UNIMPLEMENTED;
    if (b->kind == NS10_BOARD_MEMM) {
        if (address >= 0x1f400000u && address <= 0x1f5ffffeu)
            return read_memm_word(b, address, value);
        if (address >= 0x1fb40000u && address <= 0x1fb5fffeu) {
            b->banked_region_mode = 1;
            *value = 0; /* MAME's bank_r predictor. */
            return NS10_BOARD_OK;
        }
        return NS10_BOARD_UNMAPPED;
    }

    if (address >= 0x1f500000u && address <= 0x1f5ffffeu) {
        size_t offset = address - 0x1f500000u;
        *value = load_le16(b->shared_ram + offset);
        return NS10_BOARD_OK;
    }
    switch (address) {
    case 0x1f400000u:
        if (!selected_nand(b)) return NS10_BOARD_RANGE;
        *value = b->nand_rnb_state[b->selected_nand];
        return NS10_BOARD_OK;
    case 0x1f450000u: {
        NS10NAND *nand = selected_nand(b);
        if (!nand) return NS10_BOARD_RANGE;
        *value = ns10_crypto_nand_read_word(b->crypto, nand,
            unsigned(b->selected_nand), b->nand_address);
        return NS10_BOARD_OK;
    }
    case 0x1f470000u:
        *value = uint16_t(b->ctrl_reg);
        return NS10_BOARD_OK;
    default:
        return NS10_BOARD_UNMAPPED;
    }
}

static int write_word(NS10Board *b, uint32_t address, uint16_t data, uint16_t mem_mask)
{
    if (peripheral_unimplemented(address)) return NS10_BOARD_UNIMPLEMENTED;
    if (b->kind == NS10_BOARD_MEMM) {
        if (address == 0x1f300000u) {
            NS10ProfileInfo info;
            if (ns10_crypto_lookup(b->profile, &info) != NS10_OK)
                return NS10_BOARD_CRYPTO;
            if (!info.type) return NS10_BOARD_OK; /* MAME leaves absent CPLDs inert. */
            {
                int result = ns10_crypto_control_write(b->crypto, data);
                return result == NS10_OK ? NS10_BOARD_OK : NS10_BOARD_CRYPTO;
            }
        }
        if (address >= 0x1f400000u && address <= 0x1f5ffffeu) {
            uint32_t offset = (address - 0x1f400000u) >> 1;
            if (b->banked_region_mode) return NS10_BOARD_OK; /* MAME ignores writes in ROM mode. */
            return nor_flash_write(b->flash, offset, data) == NOR_FLASH_OK
                ? NS10_BOARD_OK : NS10_BOARD_FLASH;
        }
        if (address >= 0x1fb40000u && address <= 0x1fb5fffeu) {
            b->bank_index = (address - 0x1fb40000u) >> 1;
            b->banked_region_mode = b->bank_index >= 0x10;
            return NS10_BOARD_OK;
        }
        return NS10_BOARD_UNMAPPED;
    }

    if (address >= 0x1f500000u && address <= 0x1f5ffffeu) {
        size_t offset = address - 0x1f500000u;
        uint16_t old_value = load_le16(b->shared_ram + offset);
        store_le16(b->shared_ram + offset, combine(old_value, data, mem_mask));
        return NS10_BOARD_OK;
    }
    switch (address) {
    case 0x1f300000u:
    case 0x1f380000u: {
        NS10ProfileInfo info;
        if (ns10_crypto_lookup(b->profile, &info) != NS10_OK)
            return NS10_BOARD_CRYPTO;
        if (!info.type) return NS10_BOARD_OK;
        return ns10_crypto_control_write(b->crypto, data) == NS10_OK
            ? NS10_BOARD_OK : NS10_BOARD_CRYPTO;
    }
    case 0x1f410000u:
    case 0x1f420000u:
    case 0x1f430000u:
    case 0x1f440000u: {
        /* These four delegates are 8-bit, mapped only on the low byte. */
        if (!(mem_mask & 0xff)) return NS10_BOARD_OK;
        NS10NAND *nand = selected_nand(b);
        if (!nand) return NS10_BOARD_RANGE;
        uint8_t byte = uint8_t(data);
        if (address == 0x1f410000u) {
            if (byte == 0x30) {
                NS10NANDState state{};
                ns10_nand_get_state(nand, &state);
                /* The fitted K9F2808 has one column cycle. MAME enters its
                 * second-read mode here; the byte-stream model already has
                 * the addressed page and needs no pointer mutation. */
                return state.mode == 1 && state.address_cycle >= 3
                    ? NS10_BOARD_OK : NS10_BOARD_UNSUPPORTED;
            }
            int result = ns10_nand_command(nand, byte);
            if (result == NS10_OK && (byte == 0xff || byte == 0x10 || byte == 0x15 || byte == 0xd0)) {
                /* nandflash.cpp reset drives R/B low then high; the board
                 * callback stores `state != 1`, as MAME does. */
                nand_rnb_callback(b, b->selected_nand, 0);
                nand_rnb_callback(b, b->selected_nand, 1);
            }
            return result == NS10_OK ? NS10_BOARD_OK : NS10_BOARD_UNSUPPORTED;
        }
        ns10_nand_address(nand, byte);
        if (address == 0x1f420000u)
            b->nand_address = (b->nand_address & 0xffff00u) | byte;
        else if (address == 0x1f430000u)
            b->nand_address = (b->nand_address & 0xff00ffu) | (uint32_t(byte) << 8);
        else
            b->nand_address = (b->nand_address & 0x00ffffu) | (uint32_t(byte) << 16);
        return NS10_BOARD_OK;
    }
    case 0x1f450000u:
        if (!selected_nand(b)) return NS10_BOARD_RANGE;
        /* The source always emits two high-byte-first writes, even for a
         * partial CPU word mask; NAND data wires do not follow host lanes. */
        if (ns10_nand_write_byte(selected_nand(b), uint8_t(data >> 8)) != NS10_OK ||
            ns10_nand_write_byte(selected_nand(b), uint8_t(data)) != NS10_OK)
            return NS10_BOARD_UNSUPPORTED;
        return NS10_BOARD_OK;
    case 0x1f460000u:
        if (b->ctrl_reg & 4) return NS10_BOARD_OK;
        if (data >= b->nand_count) return NS10_BOARD_RANGE;
        b->selected_nand = data;
        return NS10_BOARD_OK;
    case 0x1f470000u:
        b->ctrl_reg = data; /* The pinned MAME handler assigns data directly. */
        return NS10_BOARD_OK;
    case 0x1f468000u:
        if (std::strcmp(b->profile, "ippo") && std::strcmp(b->profile, "ippo2"))
            return NS10_BOARD_UNMAPPED;
        ns10_crypto_dataxor_write(b->crypto, data);
        return NS10_BOARD_OK;
    default:
        return NS10_BOARD_UNMAPPED;
    }
}

static int bus_read_byte(NS10Board *b, uint32_t address, uint8_t *out)
{
    uint32_t a = physical(address);
    uint16_t word;
    int result;
    if (peripheral_unimplemented(a)) return NS10_BOARD_UNIMPLEMENTED;
    if (b->kind == NS10_BOARD_MEMN && a >= 0x1f500000u && a < 0x1f600000u) {
        *out = b->shared_ram[a - 0x1f500000u];
        return NS10_BOARD_OK;
    }
    if (b->kind == NS10_BOARD_MEMM &&
        ((a >= 0x1f400000u && a < 0x1f600000u) ||
         (a >= 0x1fb40000u && a < 0x1fb60000u))) {
        result = read_word(b, a & ~1u, &word);
        if (result == NS10_BOARD_OK) *out = uint8_t(word >> ((a & 1) * 8));
        return result;
    }
    if (b->kind == NS10_BOARD_MEMN &&
        (a == 0x1f400000u || a == 0x1f400001u ||
         a == 0x1f450000u || a == 0x1f450001u ||
         a == 0x1f470000u || a == 0x1f470001u)) {
        result = read_word(b, a & ~1u, &word);
        if (result == NS10_BOARD_OK) *out = uint8_t(word >> ((a & 1) * 8));
        return result;
    }
    return NS10_BOARD_UNMAPPED;
}

static int bus_write_byte(NS10Board *b, uint32_t address, uint8_t byte, uint8_t byte_mask)
{
    uint32_t a = physical(address);
    uint16_t data = uint16_t(byte) << ((a & 1) * 8);
    uint16_t mask = uint16_t(byte_mask) << ((a & 1) * 8);
    if (peripheral_unimplemented(a)) return NS10_BOARD_UNIMPLEMENTED;
    if (b->kind == NS10_BOARD_MEMN && a >= 0x1f500000u && a < 0x1f600000u) {
        size_t offset = a - 0x1f500000u;
        b->shared_ram[offset] = uint8_t((b->shared_ram[offset] & ~byte_mask) | (byte & byte_mask));
        return NS10_BOARD_OK;
    }
    if (b->kind == NS10_BOARD_MEMM &&
        ((a >= 0x1f300000u && a < 0x1f300002u) || (a >= 0x1f400000u && a < 0x1f600000u) ||
         (a >= 0x1fb40000u && a < 0x1fb60000u)))
        return write_word(b, a & ~1u, data, mask);
    if (b->kind == NS10_BOARD_MEMN &&
        ((a >= 0x1f300000u && a < 0x1f300002u) || (a >= 0x1f380000u && a < 0x1f380002u) ||
         a == 0x1f410000u || a == 0x1f420000u ||
         a == 0x1f430000u || a == 0x1f440000u ||
         a == 0x1f450000u || a == 0x1f450001u ||
         (a >= 0x1f460000u && a < 0x1f460002u) || (a >= 0x1f470000u && a < 0x1f470002u) ||
         (a >= 0x1f468000u && a < 0x1f468002u)))
        return write_word(b, a & ~1u, data, mask);
    return NS10_BOARD_UNMAPPED;
}

extern "C" {
int ns10_board_create(const NS10BoardConfig *cfg, NS10Board **out)
{
    NS10Board *b;
    NS10ProfileInfo info;
    size_t i;
    if (out) *out = nullptr;
    if (!cfg || !out || !cfg->game_profile || !cfg->program_rom ||
        cfg->program_rom_size != 0x400000 ||
        (cfg->kind != NS10_BOARD_MEMM && cfg->kind != NS10_BOARD_MEMN))
        return NS10_BOARD_ARGUMENT;
    if (ns10_crypto_lookup(cfg->game_profile, &info) != NS10_OK)
        return NS10_BOARD_PROFILE;
    if ((cfg->kind == NS10_BOARD_MEMM && info.type != NS10_MEMM_TYPE1) ||
        (cfg->kind == NS10_BOARD_MEMN && info.type != NS10_PASSIVE_ONLY &&
         info.type != NS10_MEMN_TYPE2 && info.type != NS10_MEMN_TYPE2_TABLE))
        return NS10_BOARD_PROFILE;
    if (cfg->kind == NS10_BOARD_MEMM) {
        if (!cfg->memm_flash_image || cfg->memm_flash_size != 0x800000 ||
            !cfg->memm_nand_region || cfg->memm_nand_region_size < 0x800000 ||
            !cfg->memm_data_region || cfg->memm_data_region_size < 0x200000 ||
            (cfg->memm_data_region_size & 1) || cfg->nand_chip_count)
            return NS10_BOARD_GEOMETRY;
    } else {
        if (!cfg->nand_chips || !cfg->nand_chip_count || cfg->nand_chip_count > 16 ||
            cfg->memm_flash_image || cfg->memm_data_region || cfg->memm_nand_region)
            return NS10_BOARD_GEOMETRY;
        for (i = 0; i < cfg->nand_chip_count; i++)
            if (!cfg->nand_chips[i].bytes || cfg->nand_chips[i].pages < 0x2020 ||
                cfg->nand_chips[i].size != size_t(cfg->nand_chips[i].pages) * 528)
                return NS10_BOARD_GEOMETRY;
    }
    b = new (std::nothrow) NS10Board{};
    if (!b) return NS10_BOARD_NOMEM;
    b->kind = cfg->kind;
    std::strncpy(b->profile, cfg->game_profile, sizeof(b->profile) - 1);
    b->program_rom = cfg->program_rom;
    b->program_rom_size = cfg->program_rom_size;
    b->memm_nand = cfg->memm_nand_region;
    b->memm_nand_size = cfg->memm_nand_region_size;
    b->memm_data = cfg->memm_data_region;
    b->memm_data_size = cfg->memm_data_region_size;
    b->crypto = ns10_crypto_create(cfg->game_profile, cfg->crypto_table, cfg->crypto_table_size);
    b->shared_ram = new (std::nothrow) uint8_t[0x100000]();
    if (cfg->kind == NS10_BOARD_MEMM)
        b->flash = nor_flash_create(NOR_FLASH_INTEL_28F640J5_X16,
            cfg->memm_flash_image, cfg->memm_flash_size);
    else {
        b->nands = new (std::nothrow) NS10NAND *[cfg->nand_chip_count]();
        b->nand_count = cfg->nand_chip_count;
        if (b->nands)
            for (i = 0; i < b->nand_count; i++) {
                b->nands[i] = ns10_nand_create(&cfg->nand_chips[i]);
                if (!b->nands[i]) break;
            }
    }
    if (!b->crypto || !b->shared_ram ||
        (cfg->kind == NS10_BOARD_MEMM && !b->flash) ||
        (cfg->kind == NS10_BOARD_MEMN && (!b->nands || i != b->nand_count))) {
        ns10_board_destroy(b);
        return NS10_BOARD_NOMEM;
    }
    if (cfg->kind == NS10_BOARD_MEMM) {
        std::memcpy(b->program_rom, cfg->memm_nand_region, 0x62000);
        for (i = 0; i < 0x62000 / 2; i++) {
            uint16_t raw = load_le16(b->program_rom + i * 2);
            store_le16(b->program_rom + i * 2, ns10_crypto_unscramble(b->crypto, raw));
        }
    } else {
        const uint8_t *nand = cfg->nand_chips[0].bytes;
        const auto copy_pages = [b, nand](size_t start_page, size_t count, size_t dest) {
            for (size_t page = start_page; page < start_page + count; page++) {
                size_t base = page * 528;
                for (size_t i = 0; i < 0x200; i += 2) {
                    uint16_t raw = uint16_t((uint16_t(nand[base + i]) << 8) | nand[base + i + 1]);
                    uint16_t plain = ns10_crypto_unscramble(b->crypto, raw);
                    store_le16(b->program_rom + dest, plain);
                    dest += 2;
                }
            }
        };
        copy_pages(0x40, 0xe0, 0);
        copy_pages(0x120, 0x1f00, 0x20000);
    }
    b->io = cfg->io;
    ns10_board_reset(b);
    *out = b;
    return NS10_BOARD_OK;
}

void ns10_board_destroy(NS10Board *b)
{
    if (!b) return;
    if (b->flash) nor_flash_destroy(b->flash);
    if (b->nands) {
        for (size_t i = 0; i < b->nand_count; i++) ns10_nand_destroy(b->nands[i]);
        delete[] b->nands;
    }
    ns10_crypto_destroy(b->crypto);
    delete[] b->shared_ram;
    delete b;
}

void ns10_board_reset(NS10Board *b)
{
    if (!b) return;
    if (b->io) ns10_io_reset(b->io);
    b->bank_index = b->nand_address = b->ctrl_reg = 0;
    b->selected_nand = 0;
    b->banked_region_mode = 0;
    std::memset(b->shared_ram, 0, 0x100000);
    ns10_crypto_reset(b->crypto);
    std::memset(b->nand_rnb_state, 0, sizeof(b->nand_rnb_state));
    for (size_t i = 0; i < b->nand_count; i++) ns10_nand_reset(b->nands[i]);
}

int ns10_board_read(NS10Board *b, uint32_t address, unsigned width, uint32_t *value)
{
    uint32_t result = 0;
    int status = NS10_BOARD_OK;
    if (!b || !value || (width != 1 && width != 2 && width != 4))
        return NS10_BOARD_ARGUMENT;
    if (width > 1 && (address & 1)) return NS10_BOARD_ARGUMENT;
    if (b->io) {
        int result = ns10_io_read(b->io, address, width, value);
        if (result == NS10_IO_OK) return NS10_BOARD_OK;
        if (result != NS10_IO_UNMAPPED) return NS10_BOARD_ARGUMENT;
        /* Read-only/write-only delegates and holes have no opposite handler
         * in MAME; do not mistake them for an unsupported expansion. */
        if (physical(address) >= 0x1fba0000u && physical(address) < 0x1fba001cu)
            return NS10_BOARD_UNMAPPED;
    }
    for (unsigned i = 0; i < width; ) {
        if (width - i >= 2) {
            uint16_t word;
            status = read_word(b, physical(address + i), &word);
            if (status != NS10_BOARD_OK) return status;
            result |= uint32_t(word) << (i * 8);
            i += 2;
        } else {
            uint8_t byte;
            status = bus_read_byte(b, address + i, &byte);
            if (status != NS10_BOARD_OK) return status;
            result |= uint32_t(byte) << (i * 8);
            i++;
        }
    }
    *value = result;
    return NS10_BOARD_OK;
}

int ns10_board_write(NS10Board *b, uint32_t address, unsigned width,
                     uint32_t value, uint32_t mem_mask)
{
    if (!b || (width != 1 && width != 2 && width != 4)) return NS10_BOARD_ARGUMENT;
    if (width > 1 && (address & 1)) return NS10_BOARD_ARGUMENT;
    if (mem_mask & ~lane_mask(width))
        return NS10_BOARD_ARGUMENT;
    if (b->io) {
        int result = ns10_io_write(b->io, address, width, value, mem_mask);
        if (result == NS10_IO_OK) return NS10_BOARD_OK;
        if (result != NS10_IO_UNMAPPED) return NS10_BOARD_ARGUMENT;
        /* Read-only/write-only delegates and holes have no opposite handler
         * in MAME; do not mistake them for an unsupported expansion. */
        if (physical(address) >= 0x1fba0000u && physical(address) < 0x1fba001cu)
            return NS10_BOARD_UNMAPPED;
    }
    for (unsigned i = 0; i < width; ) {
        int status;
        if (width - i >= 2) {
            uint16_t mask = uint16_t(mem_mask >> (i * 8));
            status = mask ? write_word(b, physical(address + i), uint16_t(value >> (i * 8)), mask) : NS10_BOARD_OK;
            i += 2;
        } else {
            uint8_t mask = uint8_t(mem_mask >> (i * 8));
            status = mask ? bus_write_byte(b, address + i, uint8_t(value >> (i * 8)), mask) : NS10_BOARD_OK;
            i++;
        }
        if (status != NS10_BOARD_OK) return status;
    }
    return NS10_BOARD_OK;
}

int ns10_board_dma_read(NS10Board *b, uint8_t *destination, size_t bytes)
{
    if (!b || !destination || b->kind != NS10_BOARD_MEMN || (bytes & 3))
        return NS10_BOARD_ARGUMENT;
    if (!selected_nand(b)) return NS10_BOARD_RANGE;
    for (size_t i = 0; i < bytes; i += 2) {
        uint16_t word = ns10_crypto_nand_read_word(b->crypto,
            selected_nand(b), unsigned(b->selected_nand), b->nand_address);
        store_le16(destination + i, word);
    }
    return NS10_BOARD_OK;
}

void ns10_board_advance_us(NS10Board *b, uint64_t us)
{
    if (b && b->flash) nor_flash_advance_us(b->flash, us);
    if (b && b->io) ns10_io_advance_us(b->io, us);
}
int ns10_board_crypto_active(const NS10Board *b)
{
    return b ? ns10_crypto_active(b->crypto) : 0;
}
uint16_t ns10_board_data_xor(const NS10Board *b)
{
    NS10CryptoState state{};
    if (b) ns10_crypto_get_state(b->crypto, &state);
    return state.data_xor;
}
unsigned ns10_board_selected_nand(const NS10Board *b)
{
    return b ? unsigned(b->selected_nand) : 0;
}
const char *ns10_board_profile(const NS10Board *b)
{
    return b ? b->profile : nullptr;
}
}
