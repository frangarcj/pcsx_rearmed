/* GPL-2.0-or-later. Native host adapter for MAME-derived Namco System 11. */
#ifndef PCSX_SYSTEM11_H
#define PCSX_SYSTEM11_H
#include <stdio.h>
#include "psxcommon.h"
int system11_load(const char *directory);
void system11_unload(void);
int system11_active(void);
void system11_mem_reset(void);
void system11_reset(void);
/* Uses the shared arcade board-thread option; Vita worker affinity: core 2. */
int system11_board_thread_start(void);
void system11_board_thread_stop(void);
int system11_board_thread_active(void);
void system11_audio_pump(void);
int system11_sync(u32 psx_cycle);
int system11_read(u32 address, unsigned width, u32 *value);
int system11_write(u32 address, unsigned width, u32 value);
int system11_set_input(const char *name, int pressed);
/* Exact 2080-byte AT28C16 data+ID image; active board required. */
int system11_eeprom_import(const unsigned char *image, size_t size);
int system11_eeprom_export(unsigned char *image, size_t size);
void system11_set_audio_output(void (*callback)(void *, const int16_t *, unsigned), void *opaque);
int system11_open_audio(const char *path);
int system11_close_audio(void);
int system11_selftest(void);
void system11_report(FILE *out);
#endif
