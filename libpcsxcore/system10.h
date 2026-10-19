/* GPL-2.0-or-later. Native host adapter for MAME-derived Namco System 10. */
#ifndef PCSX_SYSTEM10_H
#define PCSX_SYSTEM10_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int system10_load(const char *directory);
void system10_unload(void);
int system10_active(void);
void system10_mem_reset(void);
void system10_reset(void);
int system10_sync(uint32_t psx_cycle);
/* Return 1 when handled, 0 when unmapped. Board faults are reported as
 * handled with an open-bus value so the core does not fall through. */
int system10_read(uint32_t address, unsigned width, uint32_t *value);
int system10_write(uint32_t address, unsigned width, uint32_t value,
                   uint32_t mem_mask);
/* Opcode fetch hook for dynamically banked MEM(M) ROM and MEM(N) shared RAM;
 * ordinary code ROM is copied into the normal 4 MiB PSX ROM map. */
int system10_fetch(uint32_t address, uint32_t *value);
/* DMA channel 5 read callback; destination is the caller's PSX RAM span. */
int system10_dma_read(uint8_t *destination, size_t bytes);
/* Validate DMA5 mode, source and full destination span before transferring. */
int system10_dma5(uint32_t madr, uint32_t bcr, uint32_t chcr);
int system10_set_input(const char *name, int pressed);
int system10_selftest(void);
void system10_report(FILE *out);

#endif
