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
/* Flash/NAND the game wrote. The image of a MEM(M) flash or MEM(N) NAND is
 * large and nearly unchanged, so the file holds only the 16 KiB chunks that
 * differ from the media the board was loaded with. Import (after
 * system10_load): 0 if applied or there is no file, -1 if the file does not
 * belong to these media or is damaged (nothing is changed). Export: writes the
 * file, or removes it if nothing differs; 0, or -1 on failure. */
int system10_persist_import(const char *path);
int system10_persist_export(const char *path);
/* For tests: a byte of the media that persist saves (target 0 is the MEM(M)
 * flash or the first MEM(N) NAND chip). -1 if there is no such byte. */
int system10_test_media_byte(size_t target, size_t offset);
int system10_test_media_poke(size_t target, size_t offset, uint8_t value);
int system10_set_input(const char *name, int pressed);
int system10_selftest(void);
void system10_report(FILE *out);

#endif
