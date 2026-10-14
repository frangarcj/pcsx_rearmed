/* GPL-2.0-or-later. Native Konami System GQ integration. */
#ifndef PCSX_SYSTEMGQ_H
#define PCSX_SYSTEMGQ_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
int systemgq_load(const char *directory);
/* Explicit synthetic-only BIOS/ROM guest fixtures; normal loading audits
 * hashes. */
int systemgq_test_load(const char *directory);
void systemgq_unload(void);
int systemgq_active(void);
void systemgq_mem_reset(void);
void systemgq_reset(void);
int systemgq_sync(uint32_t cycle);
int systemgq_read(uint32_t address, unsigned width, uint32_t *value);
int systemgq_write(uint32_t address, unsigned width, uint32_t value,
                   uint32_t mask);
int systemgq_dma5(uint32_t madr, uint32_t bcr, uint32_t chcr);
int systemgq_set_input(const char *name, int pressed);
unsigned systemgq_analog_max(const char *name);
int systemgq_set_analog(const char *name, unsigned value);
int systemgq_set_gun_position(unsigned player, unsigned x, unsigned y);
int systemgq_eeprom_import(const unsigned char *image, size_t size);
int systemgq_eeprom_export(unsigned char *image, size_t size);
void systemgq_set_audio_output(void (*sink)(void *, const int16_t *, unsigned),
                               void *opaque);
int systemgq_open_audio(const char *path);
int systemgq_close_audio(void);
int systemgq_flush_disk(void);
int systemgq_selftest(void);
void systemgq_report(FILE *out);
#endif
