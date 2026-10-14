/* GPL-2.0-or-later. Pinned MAME sony/zn.cpp CBaj Z80/YMZ/two-IDT7202 wiring. */
#ifndef PCSX_CBAJ_SOUND_H
#define PCSX_CBAJ_SOUND_H
#include "z80.h"
#include "ymz280b.h"
#include "fifo7200.h"
#ifdef __cplusplus
extern "C" {
#endif
#define CBAJ_SOUND_CLOCK 4000000u
typedef struct CbajSound CbajSound;
typedef struct CbajSoundConfig {
    const uint8_t *program, *samples; /* borrowed source regions */
    size_t program_size, sample_size;
    void *opaque;
    void (*sample)(void *, float left, float right, uint64_t chip_cycle);
} CbajSoundConfig;
typedef struct CbajSoundStatus {
    uint64_t cpu_cycles, ymz_writes;
    uint32_t faults;
    Z80Status z80;
    YMZ280BStatus ymz;
    Fifo7200Status fifo[2]; /* main->Z80, Z80->main */
} CbajSoundStatus;
int cbaj_sound_create(const CbajSoundConfig *, CbajSound **);
void cbaj_sound_destroy(CbajSound *);
int cbaj_sound_sync(CbajSound *, uint64_t cpu_cycle);
int cbaj_sound_reset(CbajSound *);
int cbaj_sound_send(CbajSound *, uint8_t data);
int cbaj_sound_receive(CbajSound *, uint8_t *data);
int cbaj_sound_main_status(CbajSound *, uint8_t *data);
void cbaj_sound_status(const CbajSound *, CbajSoundStatus *);
#ifdef __cplusplus
}
#endif
#endif
