/* SPDX-License-Identifier: BSD-3-Clause. MAME-derived YMZ280B; see ymz280b.LICENSE. */
#ifndef PCSX_ARCADE_YMZ280B_H
#define PCSX_ARCADE_YMZ280B_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define YMZ280B_DEFAULT_CLOCK 16934400u
#define YMZ280B_SAMPLE_DIVIDER 192u
typedef struct YMZ280B YMZ280B;
typedef struct YMZ280BConfig {
    uint32_t clock_hz;
    /* Borrowed read-only power-of-two ROM, <=16 MiB, mirrored over 24 bits. */
    const uint8_t *rom;
    size_t rom_size;
    void *opaque;
    void (*sample)(void *, float left, float right, uint64_t chip_cycle);
    void (*irq)(void *, int asserted, uint64_t chip_cycle);
} YMZ280BConfig;
enum { YMZ280B_OK=0, YMZ280B_ARGUMENT=-1, YMZ280B_NOMEM=-2, YMZ280B_TIME=-3, YMZ280B_REENTRANT=-4 };
enum { YMZ280B_DIAG_UNKNOWN_REGISTER=1, YMZ280B_DIAG_DSP=2 };
typedef struct YMZ280BStatus {
    uint64_t cycles, frames, voice_ends, irq_edges;
    uint32_t clock_hz, sample_rate, diagnostics;
    uint8_t status, irq, playing;
} YMZ280BStatus;
int ymz280b_create(const YMZ280BConfig *, YMZ280B **);
void ymz280b_destroy(YMZ280B *);
int ymz280b_sync_to_cycle(YMZ280B *, uint64_t cycle);
int ymz280b_read(YMZ280B *, unsigned offset, uint64_t cycle, uint8_t *);
int ymz280b_write(YMZ280B *, unsigned offset, uint8_t data, uint64_t cycle);
int ymz280b_reset(YMZ280B *, uint64_t cycle);
void ymz280b_get_status(const YMZ280B *, YMZ280BStatus *);
#ifdef __cplusplus
}
#endif
#endif
