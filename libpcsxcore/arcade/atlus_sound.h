/* GPL-2.0-or-later. Pinned MAME sony/zn.cpp atlus_zn_state sound wiring. */
#ifndef PCSX_ATLUS_SOUND_H
#define PCSX_ATLUS_SOUND_H
#include "ymz280b.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ATLUS_SOUND_CLOCK 10000000u
typedef struct AtlusSound AtlusSound;
typedef struct AtlusSoundConfig {
    const uint8_t *program, *samples;
    size_t program_size, sample_size;
    void *opaque;
    void (*sample)(void *, float left, float right, uint64_t chip_cycle);
} AtlusSoundConfig;
typedef struct AtlusSoundStatus {
    uint64_t cpu_cycles, commands, latch_reads, irq3_ack, irq2_ack, ymz_writes;
    uint32_t pc, d0, faults;
    uint16_t sr, latch;
    uint8_t pending, irq2;
    YMZ280BStatus ymz;
} AtlusSoundStatus;
int atlus_sound_create(const AtlusSoundConfig *, AtlusSound **);
void atlus_sound_destroy(AtlusSound *);
int atlus_sound_sync(AtlusSound *, uint64_t cpu_cycle);
int atlus_sound_reset(AtlusSound *);
/* Source generic_latch_16 replaces the complete word; it has no mem_mask. */
int atlus_sound_command(AtlusSound *, uint16_t data);
void atlus_sound_status(const AtlusSound *, AtlusSoundStatus *);
#ifdef __cplusplus
}
#endif
#endif
