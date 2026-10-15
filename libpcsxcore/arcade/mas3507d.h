/* SPDX-License-Identifier: BSD-3-Clause
 * MAME MAS3507D high-level model, Olivier Galibert / windyfairy.
 */
#ifndef PCSX_MAS3507D_H
#define PCSX_MAS3507D_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Mas3507D Mas3507D;
typedef void (*mas3507d_pin_sink_t)(void *, int);
typedef struct Mas3507DStatus {
    unsigned buffered_bytes, decoded_frames, pending_samples, sample_rate, channels;
    unsigned muted, faults;
    float gain_left, gain_right;
} Mas3507DStatus;
/* Callbacks report every source notification, including repeated demand=1.
 * Schedule further work after the current call. Mutation from callbacks is
 * rejected; status inspection is allowed. The device is not thread-safe. */
Mas3507D *mas3507d_create(mas3507d_pin_sink_t sync, mas3507d_pin_sink_t demand, void *opaque);
void mas3507d_destroy(Mas3507D *chip);
void mas3507d_reset(Mas3507D *chip);
void mas3507d_reset_playback(Mas3507D *chip);
int mas3507d_write_i2c(Mas3507D *chip, int scl, int sda);
/* Same output AND host pins as source. Bit1=SCL, bit0=SDA. */
unsigned mas3507d_read_i2c(const Mas3507D *chip);
int mas3507d_write_byte(Mas3507D *chip, uint8_t byte);
/* Render at the current source sample rate. Interleaved float stereo, source
 * 1/32768 scale and programmed gains/mute. Undecodable/empty data yields zero;
 * it never advances decoded-frame or successful-sync counters. Rate changes
 * are reported in status, not silently resampled here. Returns 0 or -1. */
int mas3507d_render(Mas3507D *chip, float *stereo, unsigned frames);
void mas3507d_get_status(const Mas3507D *chip, Mas3507DStatus *status);
#ifdef __cplusplus
}
#endif
#endif
