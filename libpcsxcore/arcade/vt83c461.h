/* SPDX-License-Identifier: BSD-3-Clause. MAME VIA/32-bit IDE port; see LICENSE. */
#ifndef PCSX_VT83C461_H
#define PCSX_VT83C461_H
#include "ata_storage.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Vt83c461 Vt83c461;
typedef struct Vt83c461Status {
    uint8_t unknown, selected, config[16];
    uint64_t config_reads, config_writes, cs0_reads, cs0_writes, cs1_reads, cs1_writes;
    uint32_t diagnostics;
} Vt83c461Status;
/* Borrowed master HDD; no slave is instantiated by source Primal Rage II. */
int vt83c461_create(ata_storage_t *master, Vt83c461 **out);
void vt83c461_destroy(Vt83c461 *);
/* Source configuration registers have no reset override and persist. */
void vt83c461_reset(Vt83c461 *);
uint32_t vt83c461_config_read(Vt83c461 *, uint32_t offset);
void vt83c461_config_write(Vt83c461 *, uint32_t offset, uint32_t data);
uint32_t vt83c461_read_cs0(Vt83c461 *, uint32_t offset, uint32_t mask);
void vt83c461_write_cs0(Vt83c461 *, uint32_t offset, uint32_t data, uint32_t mask);
uint32_t vt83c461_read_cs1(Vt83c461 *, uint32_t offset, uint32_t mask);
void vt83c461_write_cs1(Vt83c461 *, uint32_t offset, uint32_t data, uint32_t mask);
void vt83c461_status(const Vt83c461 *, Vt83c461Status *);
#ifdef __cplusplus
}
#endif
#endif
