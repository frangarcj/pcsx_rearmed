/* GPL-2.0-or-later. Native host integration for the shared Konami GV board. */
#ifndef PCSX_SYSTEMGV_H
#define PCSX_SYSTEMGV_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int systemgv_load(const char *directory);
/* Test-only boot path. The BIOS image is generated in memory and is never
 * accepted by the source-profile loader; `chd_path` must still be a real CHD. */
int systemgv_test_load(const char *chd_path);
int systemgv_test_load_tokimeki(const char *chd_path);
int systemgv_test_load_simpbowl(const char *chd_path);
int systemgv_test_load_kdeadeye(const char *chd_path);
void systemgv_unload(void);
int systemgv_active(void);
void systemgv_mem_reset(void);
void systemgv_reset(void);
int systemgv_sync(uint32_t psx_cycle);
void systemgv_set_audio_output(void (*sink)(void *,const int16_t *,unsigned),void *opaque);
int systemgv_audio_spu_output(const int16_t *stereo,unsigned frames,uint32_t end_cycle);
int systemgv_audio_flush_output(void);
int systemgv_read(uint32_t address, unsigned width, uint32_t *value);
int systemgv_write(uint32_t address, unsigned width, uint32_t value,
                   uint32_t mem_mask);
int systemgv_dma5(uint32_t madr, uint32_t bcr, uint32_t chcr);
const char *systemgv_flash_path(void);
int systemgv_flush_flash(void);
int systemgv_has_guns(void);
int systemgv_set_gun_position(unsigned player,unsigned x,unsigned y);
int systemgv_has_trackball(void);
unsigned systemgv_trackball_players(void);
int systemgv_trackball_reverse_x(void);
int systemgv_trackball_delta_player(unsigned player,int x,int y);
int systemgv_has_sensors(void);
int systemgv_sensor_values(unsigned *heartbeat,unsigned *gsr);
typedef int (*systemgv_printer_capture_cb)(void *,const uint8_t **,unsigned *,unsigned *,unsigned *);
int systemgv_set_printer_video(systemgv_printer_capture_cb,void *opaque,const char *directory);
void systemgv_vblank(int state);
unsigned systemgv_analog_max(const char *name);
int systemgv_set_analog(const char *name,unsigned value);
int systemgv_trackball_delta(int x,int y);
void systemgv_frame(void);
int systemgv_set_input(const char *name, int pressed);
int systemgv_eeprom_import(const unsigned char *image, size_t size);
int systemgv_eeprom_export(unsigned char *image, size_t size);
int systemgv_selftest(void);
/* Destructive test-only SCSI data-out regression, rejected for real profiles. */
int systemgv_test_dma_write(void);
uint64_t systemgv_dma_pending_bytes(void);
void systemgv_report(FILE *out);

#endif
