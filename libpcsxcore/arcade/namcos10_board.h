/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PCSX_ARCADE_NAMCOS10_BOARD_H
#define PCSX_ARCADE_NAMCOS10_BOARD_H

#include <stddef.h>
#include <stdint.h>
#include "namcos10_crypto.h"
#include "namcos10_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NS10Board NS10Board;

typedef enum NS10BoardKind {
    NS10_BOARD_MEMM = 1,
    NS10_BOARD_MEMN = 2
} NS10BoardKind;

typedef enum NS10BoardResult {
    NS10_BOARD_OK = 0,
    NS10_BOARD_UNMAPPED = -100,
    NS10_BOARD_UNIMPLEMENTED = -101,
    NS10_BOARD_ARGUMENT = -102,
    NS10_BOARD_PROFILE = -103,
    NS10_BOARD_GEOMETRY = -104,
    NS10_BOARD_RANGE = -105,
    NS10_BOARD_UNSUPPORTED = -106,
    NS10_BOARD_NOMEM = -107,
    NS10_BOARD_CRYPTO = -108,
    NS10_BOARD_FLASH = -109
} NS10BoardResult;

typedef struct NS10BoardConfig {
    NS10BoardKind kind;
    const char *game_profile;
    /* Writable 4 MiB MAME maincpu:rom region; boot data is copied into it. */
    uint8_t *program_rom;
    size_t program_rom_size;
    /* MEM(M): two distinct MAME regions. The flash image is borrowed and
     * writable; nand_region is its immutable raw banked-ROM image. */
    uint8_t *memm_flash_image;
    size_t memm_flash_size;
    const uint8_t *memm_nand_region;
    size_t memm_nand_region_size;
    const uint8_t *memm_data_region;
    size_t memm_data_region_size;
    /* MEM(N): caller-owned small-page dumps, including 16 OOB bytes/page. */
    const NS10NANDConfig *nand_chips;
    size_t nand_chip_count;
    /* Optional caller-owned decryption table for table-based profiles. */
    const uint8_t *crypto_table;
    size_t crypto_table_size;
    /* Optional borrowed base I/O device; reset and elapsed time are driven
     * by this board. Caller retains ownership and wires PSX INT10. */
    NS10IO *io;
    /* Optional executable-window invalidation, in physical PSX bytes. */
    void (*code_changed)(void *opaque, uint32_t address, uint32_t bytes);
    void *code_opaque;
} NS10BoardConfig;

/* All ROM/NAND pointers remain caller-owned. Creation rejects missing or
 * malformed media; no synthetic fill data or fake key is generated. */
int ns10_board_create(const NS10BoardConfig *config, NS10Board **out_board);
void ns10_board_destroy(NS10Board *board);
void ns10_board_reset(NS10Board *board);
/* Reads/writes physical PSX addresses. Width is 1, 2, or 4 bytes and returned
 * values use the CPU's little-endian byte layout. KSEG0/KSEG1 aliases work. */
int ns10_board_read(NS10Board *board, uint32_t address, unsigned width,
                    uint32_t *value);
int ns10_board_write(NS10Board *board, uint32_t address, unsigned width,
                     uint32_t value, uint32_t mem_mask);
/* MEM(N) DMA channel 5: generates `bytes` bytes in little-endian PSX RAM
 * order from NAND data words. The caller places the buffer at DMA MADR. */
int ns10_board_dma_read(NS10Board *board, uint8_t *destination, size_t bytes);
/* Side-effect-free code snapshot. Returns 1 for stable bytes, -1 if opcode
 * reads must go through the live bus (active stream crypto/flash status),
 * or 0 outside an executable board window. Never advances a crypto stream. */
int ns10_board_code_copy(NS10Board *board, uint32_t address,
                         uint8_t *destination, size_t bytes);
/* Advance Intel flash and the connected base I/O timer. */
void ns10_board_advance_us(NS10Board *board, uint64_t microseconds);
int ns10_board_crypto_active(const NS10Board *board);
uint16_t ns10_board_data_xor(const NS10Board *board);
unsigned ns10_board_selected_nand(const NS10Board *board);
const char *ns10_board_profile(const NS10Board *board);

#ifdef __cplusplus
}
#endif
#endif
