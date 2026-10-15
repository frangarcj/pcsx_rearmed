/* GPL-2.0-or-later. CPU/DAC clock bridge and host linear resampling. */
#ifndef PCSX_SYSTEM573_DIGITAL_CLOCK_H
#define PCSX_SYSTEM573_DIGITAL_CLOCK_H
#include "system573_digital.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef int (*system573_digital_pcm_t)(void *,uint64_t,float,float);
typedef struct System573DigitalClock {
    System573Digital *chip;
    system573_digital_pcm_t sink;
    void *opaque;
    uint64_t target,previous_tick,next_input,next_output,inputs,outputs,nonzero;
    float previous[2];
    unsigned rate,rate_changes,faults,busy,initialized;
} System573DigitalClock;
/* Zero-initialize storage. Attach to a new device at time zero. The clock
 * owns render/advance ordering, not the device's lifetime or register access.
 * Output is float stereo at 44100 Hz with an absolute sample index. */
int system573_digital_clock_init(System573DigitalClock *,System573Digital *,system573_digital_pcm_t,void *);
/* Absolute core cycles at 33868800 Hz; no host-wall-time or future samples.
 * Common rational time units (169344000/s) divide every supported MPEG rate
 * and equal five units per CPU cycle. They are not a new hardware oscillator.
 * Rate changes apply to the interval after the sample that reveals the new
 * frame rate. Linear interpolation waits for the actual next source sample.
 * MMIO/reset callers first synchronize to their CPU cycle, then mutate the
 * device; they must not rewind/reinitialize this continuous output epoch. */
int system573_digital_clock_advance(System573DigitalClock *,uint64_t cycles);
#ifdef __cplusplus
}
#endif
#endif
