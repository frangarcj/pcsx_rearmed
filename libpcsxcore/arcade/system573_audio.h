/* GPL-2.0-or-later. PSX speaker routes at a shared 44.1 kHz output epoch.
 * Digital float input is already resampled by its CPU/DAC clock bridge. */
#ifndef PCSX_SYSTEM573_AUDIO_H
#define PCSX_SYSTEM573_AUDIO_H
#include <stdint.h>
#include <stddef.h>
#define SYSTEM573_AUDIO_RING 32768u
typedef void (*system573_audio_sink_t)(void *,const int16_t *,unsigned);
typedef struct System573Audio {
    int16_t cd[SYSTEM573_AUDIO_RING][2],spu[SYSTEM573_AUDIO_RING][2];
    float digital[SYSTEM573_AUDIO_RING][2];
    uint64_t cd_next,spu_next,next,frames,clipped,spu_inputs,cd_inputs,cd_nonzero,spu_nonzero;
    unsigned peak,faults;
    uint64_t digital_next,digital_inputs,digital_nonzero;
    int started,digital_enabled;
    float spu_gain;
    unsigned spu_swap;
    system573_audio_sink_t sink;void *opaque;
} System573Audio;
void system573_audio_init(System573Audio *a);
/* Shared host mixer: GV uses source SPU gain 0.75 with swapped channels.
 * Configure before source data; gain remains float until the final sum. */
int system573_audio_spu_route(System573Audio *a,float gain,unsigned swap);
void system573_audio_set_sink(System573Audio *a,system573_audio_sink_t sink,void *opaque);
/* Enable only at initial epoch, before any source data. */
int system573_audio_enable_digital(System573Audio *a);
int system573_audio_digital(System573Audio *a,uint64_t index,float left,float right);
int system573_audio_cd(System573Audio *a,uint64_t index,int16_t left,int16_t right);
int system573_audio_spu(System573Audio *a,uint64_t first,const int16_t *stereo,unsigned frames);
/* Flush only samples whose enabled source clocks have arrived. A future
 * SPU sample produced by its even-pair rounding remains pending. */
int system573_audio_flush(System573Audio *a);
#endif
