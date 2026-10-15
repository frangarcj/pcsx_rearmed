/* Native Twinkle bring-up. GPL-2.0-or-later. */
#ifndef PCSX_SYSTEMTWINKLE_H
#define PCSX_SYSTEMTWINKLE_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
int systemtwinkle_load(const char *);
int systemtwinkle_test_load(const char *);
void systemtwinkle_unload(void);
int systemtwinkle_active(void);
void systemtwinkle_mem_reset(void);
void systemtwinkle_reset(void);
int systemtwinkle_sync(uint32_t);
/* Attach after load and before execution. Sink callbacks must not reenter
 * the board; source channels/gains remain separate until final saturation. */
void systemtwinkle_set_audio_output(void (*)(void *,const int16_t *,unsigned),void *);
int systemtwinkle_audio_spu_output(const int16_t *,unsigned,uint32_t);
int systemtwinkle_audio_flush_output(void);
int systemtwinkle_read(uint32_t,unsigned,uint32_t *);
int systemtwinkle_write(uint32_t,unsigned,uint32_t,uint32_t);
int systemtwinkle_dma5(uint32_t,uint32_t,uint32_t);
int systemtwinkle_set_input(const char *,int);
unsigned systemtwinkle_analog_max(const char *);
int systemtwinkle_selftest(void);
void systemtwinkle_report(FILE *);
size_t systemtwinkle_nvram_size(void);
int systemtwinkle_nvram_import(const unsigned char *,size_t);
int systemtwinkle_nvram_export(unsigned char *,size_t);
#ifdef __cplusplus
}
#endif
#endif
