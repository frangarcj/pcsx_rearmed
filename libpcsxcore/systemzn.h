/* GPL-2.0-or-later. Native host adapter for source-derived Sony ZN daughterboards. */
#ifndef PCSX_SYSTEMZN_H
#define PCSX_SYSTEMZN_H
#include <stddef.h>
#include <stdio.h>
#include "psxcommon.h"

int systemzn_load(const char *directory);
int systemzn_load_tps(const char *directory);
/* Explicit synthetic host test path, never used by the normal TPS loader. */
int systemzn_test_load_tps(const char *directory);
int systemzn_tps_active(void);
int systemzn_load_raizing(const char *directory);
int systemzn_test_load_raizing(const char *directory);
int systemzn_raizing_active(void);
int systemzn_load_visco(const char *directory);
int systemzn_test_load_visco(const char *directory);
int systemzn_visco_active(void);
int systemzn_load_atlus(const char *directory);
int systemzn_test_load_atlus(const char *directory);
int systemzn_atlus_active(void);
unsigned systemzn_rotation(void);
void systemzn_set_ymz_audio_output(void (*callback)(void *, float, float, uint64_t), void *opaque);
void systemzn_set_ymf_audio_output(void (*callback)(void *, float, float, uint64_t), void *opaque);
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
