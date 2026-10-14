/* GPL-2.0-or-later. Pinned MAME sony/zn.cpp raizing_zn_state sound wiring. */
#ifndef PCSX_RAIZING_SOUND_H
#define PCSX_RAIZING_SOUND_H
#include "ymf271.h"
#ifdef __cplusplus
extern "C" {
#endif
#define RAIZING_SOUND_CLOCK 12000000u
typedef struct RaizingSound RaizingSound;
typedef struct RaizingSoundConfig {
    const uint8_t *program, *samples; /* borrowed source regions */
    size_t program_size, sample_size;
    void *opaque;
    void (*sample)(void *, const float outputs[4], uint64_t chip_cycle);
} RaizingSoundConfig;
typedef struct RaizingSoundStatus {
    uint64_t cpu_cycles, commands, irq_requests, irq_acknowledgements, ymf_writes;
    uint32_t pc, d0, faults;
    uint16_t sr;
    uint8_t latch, pending, irq2;
    YMF271Status ymf;
} RaizingSoundStatus;
int raizing_sound_create(const RaizingSoundConfig *, RaizingSound **);
void raizing_sound_destroy(RaizingSound *);
int raizing_sound_sync(RaizingSound *, uint64_t cpu_cycle);
int raizing_sound_reset(RaizingSound *);
int raizing_sound_command(RaizingSound *, uint8_t data);
int raizing_sound_irq(RaizingSound *);
void raizing_sound_status(const RaizingSound *, RaizingSoundStatus *);
#ifdef __cplusplus
}
#endif
#endif
