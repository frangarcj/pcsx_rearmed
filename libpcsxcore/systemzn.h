/* GPL-2.0-or-later. Native host adapter for Sony ZN / Capcom ZN boards. */
#ifndef PCSX_SYSTEMZN_H
#define PCSX_SYSTEMZN_H
#include <stddef.h>
#include <stdio.h>
#include "psxcommon.h"

int systemzn_load(const char *directory);
void systemzn_unload(void);
int systemzn_active(void);
/* Pinned MAME main CPU input clock; zero when the board is inactive. */
unsigned int systemzn_main_clock_hz(void);
void systemzn_mem_reset(void);
void systemzn_reset(void);
int systemzn_sync(u32 psx_cycle);
/* Daughterboard/common ZN/SIO0/SIO1 memory hooks, physical or KSEG aliases. */
int systemzn_read(u32 address, unsigned width, u32 *value);
int systemzn_write(u32 address, unsigned width, u32 value);
int systemzn_set_input(const char *control, int pressed);
/* Exact 2080-byte common ZN AT28C16 data+ID image. */
int systemzn_eeprom_import(const unsigned char *image, size_t size);
int systemzn_eeprom_export(unsigned char *image, size_t size);
/* QSound-only capture at the source HLE's integer 24038 Hz rate. */
void systemzn_set_audio_output(void (*callback)(void *, const int16_t *, unsigned), void *opaque);
int systemzn_open_audio(const char *path);
int systemzn_close_audio(void);
int systemzn_selftest(void);
void systemzn_report(FILE *out);
#endif
