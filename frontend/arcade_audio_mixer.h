/* GPL-2.0-or-later. Two source-clock PCM streams mixed at 44.1 kHz.
 * Synchronous producer API: both streams start at the same reset epoch. */
#ifndef PCSX_ARCADE_AUDIO_MIXER_H
#define PCSX_ARCADE_AUDIO_MIXER_H
#include <stdint.h>
#define ARCADE_MIX_RING 8192
typedef struct ArcadeMixLane {
    uint64_t position, next_output, inputs, nonzero;
    unsigned clock, divider, head, count;
    float previous[2];
    int16_t ring[ARCADE_MIX_RING][2];
} ArcadeMixLane;
typedef struct ArcadeAudioMixer {
    ArcadeMixLane lane[2];
    uint64_t frames, clipped;
    unsigned gain;
    int error;
    void (*output)(void *, const int16_t *, unsigned);
    void *opaque;
} ArcadeAudioMixer;
void arcade_mix_init(ArcadeAudioMixer *, unsigned spu_clock,
    void (*output)(void *, const int16_t *, unsigned), void *opaque);
/* Daughterboard sample cadence is oscillator/divider; preserves exact ratios. */
void arcade_mix_init_rates(ArcadeAudioMixer *, unsigned spu_clock,
    unsigned daughter_clock, unsigned daughter_divider,
    void (*output)(void *, const int16_t *, unsigned), void *opaque);
void arcade_mix_reset(ArcadeAudioMixer *);
void arcade_mix_push(ArcadeAudioMixer *, unsigned lane, float left, float right);
#endif
