/* SPDX-License-Identifier: BSD-3-Clause
 * MAME-derived YMF271 OPX adapter; see ymf271.LICENSE. */
#ifndef PCSX_ARCADE_YMF271_H
#define PCSX_ARCADE_YMF271_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define YMF271_DEFAULT_CLOCK 16934400u
#define YMF271_SAMPLE_DIVIDER 384u
typedef struct YMF271 YMF271;
typedef struct YMF271Config {
    uint32_t clock_hz;
    /* Borrowed power-of-two ROM, at most 8 MiB; mirrored over 23 address bits
     * exactly like MAME device_rom_interface. It remains read-only. */
    const uint8_t *rom;
    size_t rom_size;
    void *opaque;
    void (*sample)(void *, const float outputs[4], uint64_t chip_cycle);
    void (*irq)(void *, int asserted, uint64_t chip_cycle);
} YMF271Config;
enum { YMF271_OK=0, YMF271_ARGUMENT=-1, YMF271_NOMEM=-2, YMF271_TIME=-3, YMF271_REENTRANT=-4, YMF271_DEVICE=-5 };
enum { YMF271_DIAG_INVALID_REGISTER=1, YMF271_DIAG_PFM=2, YMF271_DIAG_ALTLOOP=4 };
typedef struct YMF271Status {
    uint64_t cycles, frames, timer_a_events, timer_b_events;
    uint32_t clock_hz, sample_rate, diagnostics, faults;
    uint8_t timer_status, irq_state;
} YMF271Status;
int ymf271_create(const YMF271Config *, YMF271 **);
void ymf271_destroy(YMF271 *);
/* Timestamped operations flush all prior samples/timer edges before changing
 * or reading state. The cycle domain is the chip input oscillator. */
int ymf271_sync_to_cycle(YMF271 *, uint64_t cycle);
int ymf271_read(YMF271 *, unsigned offset, uint64_t cycle, uint8_t *);
int ymf271_write(YMF271 *, unsigned offset, uint8_t data, uint64_t cycle);
int ymf271_reset(YMF271 *, uint64_t cycle);
void ymf271_get_status(const YMF271 *, YMF271Status *);
#ifdef __cplusplus
}
#endif
#endif
