/* GPL-2.0-or-later. Native host adapter for pinned MAME Taito FX-1S/FX1Z. */
#ifndef PCSX_SYSTEMTAITO_H
#define PCSX_SYSTEMTAITO_H
#include <stdio.h>
#include "psxcommon.h"
int systemtaito_load(const char *directory);
int systemtaito_load_fx1z(const char *directory);
/* Explicit generated-media test path; normal FX1Z loading always audits ROMs. */
int systemtaito_test_load_fx1z(const char *directory);
void systemtaito_set_zoom_audio_output(void (*output)(void *, float, float, uint64_t), void *opaque);
void systemtaito_unload(void);
int systemtaito_active(void);
void systemtaito_mem_reset(void);
void systemtaito_reset(void);
/* -1 fault, 1 deferred watchdog soft reset pending, 0 normal/inactive.
 * Host handles 1 at an instruction/block boundary, then calls reset. */
int systemtaito_sync(u32 psx_cycle);
int systemtaito_reset_requested(void);
/* Boolean handled result; device faults are retained and reported by sync. */
int systemtaito_read(u32 address, unsigned width, u32 *value);
int systemtaito_write(u32 address, unsigned width, u32 value);
int systemtaito_set_input(const char *name, int pressed);
/* FX1S: 2080-byte AT28C16 image. FX1Z: AT28C16 followed by 512-byte FRAM (2592 bytes). */
int systemtaito_eeprom_import(const unsigned char *image, size_t size);
int systemtaito_eeprom_export(unsigned char *image, size_t size);
int systemtaito_open_audio(const char *path);
int systemtaito_close_audio(void);
/* Active: nondestructive map/geometry checks. Inactive: synthetic device traces.
 * Neither path claims game boot. */
int systemtaito_selftest(void);
void systemtaito_report(FILE *out);
#endif
