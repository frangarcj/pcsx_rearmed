/* GPL-2.0-or-later. Native Mac adapter for source-derived System 573. */
#ifndef PCSX_SYSTEM573_H
#define PCSX_SYSTEM573_H
#include "psxcommon.h"
#include "arcade/system573_digital.h"
#include <stddef.h>
#include <stdio.h>
int system573_load(const char *directory);
/* Explicit generated MIPS fixture. Never reached by the original-game loader. */
int system573_test_load(const char *directory);
int system573_test_load_digital(const char *directory);
void system573_unload(void);
int system573_active(void);
void system573_mem_reset(void);
void system573_reset(void);
int system573_sync(u32 cycle);
int system573_read(u32 address,unsigned width,u32 *value);
int system573_write(u32 address,unsigned width,u32 value);
int system573_set_input(const char *control,int pressed);
int system573_selftest(void);
int system573_dma5(u32 madr,u32 bcr,u32 chcr);
size_t system573_nvram_size(void);
int system573_has_cd(void);
int system573_has_digital(void);
/* Pure diagnostic copy; does not advance clocks or read mapped registers. */
int system573_get_digital_status(System573DigitalStatus *status, uint16_t *inputs);
unsigned system573_players(void);
void system573_set_audio_output(void (*sink)(void *,const int16_t *,unsigned),void *opaque);
int system573_audio_spu_output(const int16_t *stereo,unsigned frames,u32 end_cycle);
int system573_audio_flush_output(void);
void system573_report(FILE *out);
/* Versioned checksummed snapshot of eight flash chips, timekeeper, cassette,
 * and (CD profile) writable CR589 buffer. Query the active profile's size.
 * This constant retains the original Hyperbbc NV1 shape. Original media stay
 * immutable; transient protocol state is not NVRAM. */
#define SYSTEM573_NVRAM_SIZE (32u+0x1000000u+8192u+132u)
int system573_nvram_import(const unsigned char *image,size_t size);
int system573_nvram_export(unsigned char *image,size_t size);
#endif
