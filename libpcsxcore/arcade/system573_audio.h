/* GPL-2.0-or-later. System 573 source gain-1 stereo speaker routes.
 * Both independent sample clocks are 44.1 kHz; no resampling or channel swap. */
#ifndef PCSX_SYSTEM573_AUDIO_H
#define PCSX_SYSTEM573_AUDIO_H
#include <stdint.h>
#include <stddef.h>
#define SYSTEM573_AUDIO_RING 32768u
typedef void (*system573_audio_sink_t)(void *,const int16_t *,unsigned);
typedef struct System573Audio {
    int16_t cd[SYSTEM573_AUDIO_RING][2],spu[SYSTEM573_AUDIO_RING][2];
    uint64_t cd_next,spu_next,next,frames,clipped,spu_inputs,cd_inputs,cd_nonzero,spu_nonzero;
    unsigned peak,faults;
    int started;
    system573_audio_sink_t sink;void *opaque;
} System573Audio;
void system573_audio_init(System573Audio *a);
void system573_audio_set_sink(System573Audio *a,system573_audio_sink_t sink,void *opaque);
int system573_audio_cd(System573Audio *a,uint64_t index,int16_t left,int16_t right);
int system573_audio_spu(System573Audio *a,uint64_t first,const int16_t *stereo,unsigned frames);
/* Flush only samples whose two actual source clocks have arrived. A future
 * SPU sample produced by its even-pair rounding remains pending. */
int system573_audio_flush(System573Audio *a);
#endif
