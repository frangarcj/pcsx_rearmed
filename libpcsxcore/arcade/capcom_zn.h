/*
 * Capcom ZN daughterboard adapter, based on MAME src/mame/sony/zn.cpp.
 * MAME source revision: deb2f54fe7300e7a192690318beb860159bfd52a
 * Copyright smf, R. Belmont (upstream does not specify years).
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LIBPCSXCORE_ARCADE_CAPCOM_ZN_H
#define LIBPCSXCORE_ARCADE_CAPCOM_ZN_H

#include <stddef.h>
#include <stdint.h>
#include "qsound_hle.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAPCOM_ZN_MAIN_FIXED_SIZE 0x400000u
#define CAPCOM_ZN_MAIN_BANK_SIZE 0x400000u
#define CAPCOM_ZN_MAIN_BANK_COUNT 16u
#define CAPCOM_ZN_COUNTRY_SIZE 0x80000u
#define CAPCOM_ZN_SOUND_ROM_BANK_BASE 0x8000u
#define CAPCOM_ZN_SOUND_BANK_SIZE 0x4000u
#define CAPCOM_ZN_SOUND_BANK_COUNT 16u
#define CAPCOM_ZN_SOUND_RAM_SIZE 0x1000u
#define CAPCOM_ZN_SOUND_CLOCK_HZ 8000000u
#define CAPCOM_ZN_IRQ_HZ 250u

typedef struct CapcomZn CapcomZn;

typedef enum capcom_zn_result {
    CAPCOM_ZN_FAULT = -2,
    CAPCOM_ZN_INVALID = -1,
    CAPCOM_ZN_UNMAPPED = 0,
    CAPCOM_ZN_HANDLED = 1
} capcom_zn_result_t;

enum {
    CAPCOM_ZN_FAULT_ROM_BOUNDS = 1u << 0,
    CAPCOM_ZN_FAULT_KICK_CALLBACK = 1u << 1,
    CAPCOM_ZN_FAULT_QSOUND_CALLBACK = 1u << 2
};

typedef struct capcom_zn_callbacks {
    void *opaque;
    /* Return the real active-low 32-bit MAME KICK1/KICK2 input-port value. */
    uint32_t (*read_kick)(void *opaque, unsigned player);
    /* QSound device adapter: register is 0xd000..0xd002; zero means failure. */
    int (*qsound_write)(void *opaque, uint16_t address, uint8_t data);
    /* QSound status/data callback for reads at 0xd007; zero means failure. */
    int (*qsound_read)(void *opaque, uint8_t *value);
} capcom_zn_callbacks_t;

typedef struct capcom_zn_config {
    /* Borrowed MAME `bankedroms` region, including its 4 MiB fixed prefix. */
    const uint8_t *main_rom;
    size_t main_rom_size;
    const uint8_t *country_rom;
    size_t country_rom_size;
    /* Borrowed MAME `audiocpu` region (fixed 32 KiB plus bank pages). */
    const uint8_t *sound_rom;
    size_t sound_rom_size;
    /* Optional borrowed real MAME-default QSound HLE device at 60 MHz.
     * Preferred over external QSound callbacks. Destroy never frees it. */
    QSoundHLE *qsound_hle;
} capcom_zn_config_t;

typedef struct capcom_zn_status {
    unsigned main_bank;
    size_t main_bank_offset;
    unsigned sound_bank;
    size_t sound_bank_offset;
    uint8_t sound_latch;
    int sound_latch_pending;
    int irq_asserted;
    uint64_t sound_cycles;
    uint64_t irq_ticks;
    uint64_t irq_acknowledgements;
    uint32_t faults;
    uint64_t z80_cycles, z80_instructions, z80_unsupported, z80_nmis;
    unsigned z80_pc;
    int64_t z80_cycle_debt;
    uint64_t qsound_sound_cycles;
    uint64_t qsound_cycles;
    unsigned qsound_cycle_fraction; /* Numerator remainder for 15/4 DSP cycles/T-state. */
    int qsound_hle_attached;
} capcom_zn_status_t;

/* ROM pointers and optional QSound HLE are borrowed. The caller must not
 * advance/reset/reclock/destroy an attached device independently. Required
 * QSound accesses fail closed when neither a device nor callbacks exist;
 * KICK accesses still require their external callback. */
CapcomZn *capcom_zn_create(const capcom_zn_config_t *config,
        const capcom_zn_callbacks_t *callbacks);
void capcom_zn_destroy(CapcomZn *board);
void capcom_zn_reset(CapcomZn *board);

/* PSX physical addresses or KSEG0/KSEG1 aliases; width must be 1, 2, or 4
 * and naturally aligned. Unmapped addresses belong to the caller's common ZN
 * I/O/BIOS map. Data lanes are little-endian. */
capcom_zn_result_t capcom_zn_main_read(CapcomZn *board, uint32_t address,
        unsigned width, uint32_t *value);
capcom_zn_result_t capcom_zn_main_write(CapcomZn *board, uint32_t address,
        unsigned width, uint32_t value);

/* Run the audio CPU by nominal Z80 T-states. The periodic IRQ is scheduled at
 * 250 Hz from the 8 MHz sound clock, as in MAME. Attached HLE is synchronized
 * before each access and to the nominal budget end, without Z80 overshoot. */
int capcom_zn_run_sound_cycles(CapcomZn *board, uint64_t cycles);
/* Side-effecting Z80 bus access helpers intended for diagnostics and tests. */
uint8_t capcom_zn_sound_read(CapcomZn *board, uint16_t address);
void capcom_zn_sound_write(CapcomZn *board, uint16_t address, uint8_t value);
uint8_t capcom_zn_sound_port_read(CapcomZn *board, uint16_t port);
void capcom_zn_sound_port_write(CapcomZn *board, uint16_t port, uint8_t value);
void capcom_zn_get_status(const CapcomZn *board, capcom_zn_status_t *status);

#ifdef __cplusplus
}
#endif

#endif
