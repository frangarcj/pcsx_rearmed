/* SPDX-License-Identifier: BSD-3-Clause. Acclaim RAX; see rax_sound_LICENSE.txt. */
#ifndef PCSX_RAX_SOUND_H
#define PCSX_RAX_SOUND_H
#include "adsp2181.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RAX_SOUND_CLOCK 16670000u
typedef struct RaxSound RaxSound;
enum {
 RAX_FAULT_ROM_BOUNDS=1, RAX_FAULT_DMA_WRITE=2, RAX_FAULT_OVERLAY=4,
 RAX_FAULT_COMPANDING=8, RAX_FAULT_AUTOBUFFER=16, RAX_FAULT_RECURSIVE_DMA=32,
 RAX_FAULT_CPU=64
};
typedef struct RaxSoundConfig {
 const uint8_t *rom; size_t rom_size; /* Borrowed, immutable source RAX region. */
 void *opaque;
 /* Source DAC-rate float stereo, gain 1, int16 / 32767. No host resampling.
  * Called synchronously; callbacks may not reenter or destroy the board. */
 void (*audio)(void *,const float *interleaved,size_t frames,uint32_t rate);
} RaxSoundConfig;
typedef struct RaxSoundStatus {
 Adsp2181Status cpu;
 uint64_t bdma_transfers,bdma_words,bdma_completions,bdma_resets;
 uint64_t host_writes,host_reads,replies,first_reply_clock,sport_transmits,autobuffers;
 uint64_t dac_frames,dac_nonzero,dac_underruns,dac_overruns;
 uint32_t faults,rom_bank,sample_rate;
 uint16_t input_latch,output_latch,bdma_count,buffer_size,buffer_increment,buffer_base;
 uint8_t pf0,overlay,data_bank,dac_enabled[2],dma_pending;
} RaxSoundStatus;
int rax_sound_create(const RaxSoundConfig *,RaxSound **);
void rax_sound_destroy(RaxSound *);
int rax_sound_reset(RaxSound *);
/* All events use the elapsed DSP clock; instructions currently step one clock
 * to respect BDMA deadlines created by guest writes inside the CPU callback. */
int rax_sound_run(RaxSound *,uint64_t clocks);
int rax_sound_host_write(RaxSound *,uint16_t word);
int rax_sound_host_read(RaxSound *,uint16_t *word);
void rax_sound_status(const RaxSound *,RaxSoundStatus *);
/* Debug/component-test buses, with source side effects and address masks. */
uint32_t rax_sound_program_read(const RaxSound *,uint16_t);
int rax_sound_program_write(RaxSound *,uint16_t,uint32_t);
uint16_t rax_sound_data_read(const RaxSound *,uint16_t);
int rax_sound_data_write(RaxSound *,uint16_t,uint16_t);
int rax_sound_io_read(RaxSound *,uint16_t,uint16_t *);
int rax_sound_io_write(RaxSound *,uint16_t,uint16_t);
uint32_t rax_sound_cpu_register(const RaxSound *,unsigned reg);
int rax_sound_set_cpu_register(RaxSound *,unsigned reg,uint32_t value);
#ifdef __cplusplus
}
#endif
#endif
