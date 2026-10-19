/*
 * Capcom ZN daughterboard map from MAME src/mame/sony/zn.cpp.
 * MAME source revision: deb2f54fe7300e7a192690318beb860159bfd52a
 * Copyright smf, R. Belmont (upstream does not specify years).
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "capcom_zn.h"

#include "z80.h"

#include <stdlib.h>
#include <string.h>

#define CAPCOM_ZN_IRQ_PERIOD (CAPCOM_ZN_SOUND_CLOCK_HZ / CAPCOM_ZN_IRQ_HZ)

struct CapcomZn {
    capcom_zn_config_t config;
    capcom_zn_callbacks_t callbacks;
    Z80 *sound_cpu;
    uint8_t sound_ram[CAPCOM_ZN_SOUND_RAM_SIZE];
    uint8_t sound_latch;
    uint8_t main_bank;
    uint8_t sound_bank;
    uint8_t sound_latch_pending;
    uint8_t irq_asserted;
    uint32_t faults;
    uint64_t sound_cycles;
    uint64_t irq_remainder;
    uint64_t irq_ticks;
    uint64_t irq_acknowledgements;
    uint64_t z80_cycle_origin;
    uint64_t qsound_sound_cycles;
    uint64_t qsound_cycles;
    unsigned qsound_cycle_fraction;
    uint64_t run_sound_end;
    int running_sound;
};

/* Z80 callbacks precede their bus-cycle charge. Inside execute, cycles is
 * therefore the actual access timestamp; cycle_debt is still the initial
 * run credit. Outside execute, cycles can exceed nominal time through debt.
 * Only nominal budget time may be used to flush the stream at run return. */
static uint64_t qsound_access_time(CapcomZn *board)
{
    Z80Status status;
    uint64_t time;
    if (!board->running_sound)
        return board->sound_cycles;
    z80_get_status(board->sound_cpu, &status);
    time = status.cycles - board->z80_cycle_origin;
    return time < board->run_sound_end ? time : board->run_sound_end;
}

static void qsound_sync(CapcomZn *board, uint64_t time)
{
    uint64_t remaining;
    if (!board->config.qsound_hle || time <= board->qsound_sound_cycles)
        return;
    remaining = time - board->qsound_sound_cycles;
    while (remaining) {
        /* 60 MHz crystal / 8 MHz Z80 = 15/2 crystal clocks, and the
         * DSP machine clock is crystal/2. Keep the complete 15/4 phase
         * locally rather than relying on a borrowed device clock residue. */
        uint64_t part = remaining;
        uint64_t numerator, cycles;
        if (part > (UINT64_MAX - 3) / 15)
            part = (UINT64_MAX - 3) / 15;
        numerator = part * 15 + board->qsound_cycle_fraction;
        cycles = numerator / 4;
        board->qsound_cycle_fraction = (unsigned)(numerator % 4);
        qsound_hle_run_cycles(board->config.qsound_hle, cycles);
        board->qsound_cycles += cycles;
        board->qsound_sound_cycles += part;
        remaining -= part;
    }
}

static uint32_t canonical_address(uint32_t address)
{
    if (address >= 0x80000000u && address < 0xc0000000u)
        return address & 0x1fffffffu;
    return address;
}

static uint32_t width_mask(unsigned width)
{
    if (width == 1)
        return 0xffu;
    if (width == 2)
        return 0xffffu;
    return 0xffffffffu;
}

static int valid_width(uint32_t address, unsigned width)
{
    return (width == 1 || width == 2 || width == 4) &&
        (width == 1 || !(address & (width - 1)));
}

static int contains(uint32_t address, unsigned width, uint32_t base, uint32_t size)
{
    uint64_t end = (uint64_t)address + width;
    uint64_t region_end = (uint64_t)base + size;
    return address >= base && end <= region_end;
}

static uint8_t qsound_status_read(void *opaque, uint16_t address)
{
    CapcomZn *board = (CapcomZn *)opaque;
    uint8_t value = 0xff;
    if (board->config.qsound_hle) {
        qsound_sync(board, qsound_access_time(board));
        return qsound_hle_read(board->config.qsound_hle);
    }
    if (!board->callbacks.qsound_read ||
        !board->callbacks.qsound_read(board->callbacks.opaque, &value)) {
        board->faults |= CAPCOM_ZN_FAULT_QSOUND_CALLBACK;
        return 0xff;
    }
    (void)address;
    return value;
}

static uint8_t sound_memory_read(void *opaque, uint16_t address)
{
    CapcomZn *board = (CapcomZn *)opaque;
    size_t offset;

    if (address < 0x8000) {
        offset = address;
        if (offset >= board->config.sound_rom_size) {
            board->faults |= CAPCOM_ZN_FAULT_ROM_BOUNDS;
            return 0xff;
        }
        return board->config.sound_rom[offset];
    }
    if (address < 0xc000) {
        offset = CAPCOM_ZN_SOUND_ROM_BANK_BASE +
            (size_t)board->sound_bank * CAPCOM_ZN_SOUND_BANK_SIZE +
            (address - 0x8000);
        if (offset >= board->config.sound_rom_size) {
            board->faults |= CAPCOM_ZN_FAULT_ROM_BOUNDS;
            return 0xff;
        }
        return board->config.sound_rom[offset];
    }
    if (address == 0xd007)
        return qsound_status_read(board, address);
    if (address >= 0xf000)
        return board->sound_ram[address - 0xf000];
    return 0xff;
}

static void sound_memory_write(void *opaque, uint16_t address, uint8_t value)
{
    CapcomZn *board = (CapcomZn *)opaque;
    if (address >= 0xd000 && address <= 0xd002) {
        if (board->config.qsound_hle) {
            qsound_sync(board, qsound_access_time(board));
            if (qsound_hle_write(board->config.qsound_hle, address - 0xd000, value))
                board->faults |= CAPCOM_ZN_FAULT_QSOUND_CALLBACK;
            return;
        }
        if (!board->callbacks.qsound_write ||
            !board->callbacks.qsound_write(board->callbacks.opaque, address, value))
            board->faults |= CAPCOM_ZN_FAULT_QSOUND_CALLBACK;
        return;
    }
    if (address == 0xd003) {
        board->sound_bank = value & 0x0f;
        return;
    }
    if (address >= 0xf000)
        board->sound_ram[address - 0xf000] = value;
}

static uint8_t sound_port_read(void *opaque, uint16_t port)
{
    CapcomZn *board = (CapcomZn *)opaque;
    if ((port & 0xff) != 0)
        return 0xff;
    board->sound_latch_pending = 0;
    z80_nmi(board->sound_cpu, 0);
    return board->sound_latch;
}

static void sound_port_write(void *opaque, uint16_t port, uint8_t value)
{
    (void)opaque;
    (void)port;
    (void)value;
}

static uint32_t sound_irq_ack(void *opaque)
{
    CapcomZn *board = (CapcomZn *)opaque;
    board->irq_acknowledgements++;
    board->irq_asserted = 0;
    z80_irq(board->sound_cpu, 0);
    return 0xff;
}

static int external_bank_read(CapcomZn *board, const uint8_t *rom, size_t rom_size,
        size_t offset, unsigned width, uint32_t *value)
{
    uint32_t result = 0;
    unsigned lane;
    if (offset > rom_size || width > rom_size - offset) {
        board->faults |= CAPCOM_ZN_FAULT_ROM_BOUNDS;
        return CAPCOM_ZN_FAULT;
    }
    for (lane = 0; lane < width; lane++)
        result |= (uint32_t)rom[offset + lane] << (8 * lane);
    *value = result;
    return CAPCOM_ZN_HANDLED;
}

CapcomZn *capcom_zn_create(const capcom_zn_config_t *config,
        const capcom_zn_callbacks_t *callbacks)
{
    CapcomZn *board;
    Z80Config sound_config;
    QSoundHLEStatus qsound_status;
    if (!config || !callbacks || !config->main_rom ||
        config->main_rom_size < CAPCOM_ZN_MAIN_FIXED_SIZE ||
        !config->country_rom || !config->country_rom_size ||
        !config->sound_rom || config->sound_rom_size < 0x8000)
        return NULL;
    if (config->qsound_hle &&
        (qsound_hle_get_status(config->qsound_hle, &qsound_status) ||
         qsound_status.clock_hz != 60000000u))
        return NULL;
    board = (CapcomZn *)calloc(1, sizeof(*board));
    if (!board)
        return NULL;
    board->config = *config;
    board->callbacks = *callbacks;
    memset(&sound_config, 0, sizeof(sound_config));
    sound_config.opaque = board;
    sound_config.memory_read = sound_memory_read;
    sound_config.memory_write = sound_memory_write;
    sound_config.io_read = sound_port_read;
    sound_config.io_write = sound_port_write;
    sound_config.irq_ack = sound_irq_ack;
    board->sound_cpu = z80_create(&sound_config);
    if (!board->sound_cpu) {
        free(board);
        return NULL;
    }
    return board;
}

void capcom_zn_destroy(CapcomZn *board)
{
    if (!board)
        return;
    z80_destroy(board->sound_cpu);
    free(board);
}

void capcom_zn_reset(CapcomZn *board)
{
    if (!board)
        return;
    /* MAME's generic_latch reset clears data_pending, but keeps its byte. */
    z80_nmi(board->sound_cpu, 0);
    z80_irq(board->sound_cpu, 0);
    board->main_bank = 0;
    board->sound_bank = 0;
    board->sound_latch_pending = 0;
    board->irq_asserted = 0;
    board->faults = 0;
    board->sound_cycles = 0;
    board->irq_remainder = 0;
    board->irq_ticks = 0;
    board->irq_acknowledgements = 0;
    z80_reset(board->sound_cpu);
    {
        Z80Status status;
        z80_get_status(board->sound_cpu, &status);
        board->z80_cycle_origin = status.cycles;
    }
    board->qsound_sound_cycles = 0;
    board->qsound_cycles = 0;
    board->qsound_cycle_fraction = 0;
    board->run_sound_end = 0;
    board->running_sound = 0;
    if (board->config.qsound_hle)
        qsound_hle_reset(board->config.qsound_hle);
}

capcom_zn_result_t capcom_zn_main_read(CapcomZn *board, uint32_t address,
        unsigned width, uint32_t *value)
{
    uint32_t physical;
    size_t offset;
    if (!board || !value)
        return CAPCOM_ZN_INVALID;
    physical = canonical_address(address);
    if (!valid_width(physical, width))
        return CAPCOM_ZN_INVALID;

    if (contains(physical, width, 0x1f000000, CAPCOM_ZN_MAIN_FIXED_SIZE))
        return (capcom_zn_result_t)external_bank_read(board, board->config.main_rom,
            board->config.main_rom_size, physical - 0x1f000000, width, value);
    if (contains(physical, width, 0x1f400000,
                CAPCOM_ZN_MAIN_BANK_SIZE)) {
        offset = CAPCOM_ZN_MAIN_FIXED_SIZE +
            (size_t)board->main_bank * CAPCOM_ZN_MAIN_BANK_SIZE +
            (physical - 0x1f400000);
        return (capcom_zn_result_t)external_bank_read(board, board->config.main_rom,
            board->config.main_rom_size, offset, width, value);
    }
    if (contains(physical, width, 0x1fb80000, CAPCOM_ZN_COUNTRY_SIZE))
        return (capcom_zn_result_t)external_bank_read(board, board->config.country_rom,
            board->config.country_rom_size, physical - 0x1fb80000, width, value);

    if ((physical >= 0x1fb40010 && physical <= 0x1fb40013) ||
        (physical >= 0x1fb40020 && physical <= 0x1fb40023)) {
        uint32_t base = physical < 0x1fb40020 ? 0x1fb40010 : 0x1fb40020;
        unsigned player = base == 0x1fb40010 ? 0 : 1;
        uint32_t port, lane = physical - base;
        if (!contains(physical, width, base, 4))
            return CAPCOM_ZN_UNMAPPED;
        if (!board->callbacks.read_kick) {
            board->faults |= CAPCOM_ZN_FAULT_KICK_CALLBACK;
            return CAPCOM_ZN_FAULT;
        }
        port = board->callbacks.read_kick(board->callbacks.opaque, player);
        *value = (port >> (lane * 8)) & width_mask(width);
        return CAPCOM_ZN_HANDLED;
    }
    return CAPCOM_ZN_UNMAPPED;
}

capcom_zn_result_t capcom_zn_main_write(CapcomZn *board, uint32_t address,
        unsigned width, uint32_t value)
{
    uint32_t physical;
    if (!board)
        return CAPCOM_ZN_INVALID;
    physical = canonical_address(address);
    if (!valid_width(physical, width))
        return CAPCOM_ZN_INVALID;
    if (physical == 0x1fb00000)
        board->main_bank = (uint8_t)value & 0x0f;
    else if (physical == 0x1fb60000) {
        board->sound_latch = (uint8_t)value;
        if (!board->sound_latch_pending) {
            board->sound_latch_pending = 1;
            z80_nmi(board->sound_cpu, 1);
        }
    } else
        return CAPCOM_ZN_UNMAPPED;
    return CAPCOM_ZN_HANDLED;
}

int capcom_zn_run_sound_cycles(CapcomZn *board, uint64_t cycles)
{
    if (!board)
        return CAPCOM_ZN_INVALID;
    while (cycles) {
        uint64_t until_irq = CAPCOM_ZN_IRQ_PERIOD - board->irq_remainder;
        uint64_t slice = cycles < until_irq ? cycles : until_irq;
        board->run_sound_end = board->sound_cycles + slice;
        board->running_sound = 1;
        z80_run_cycles(board->sound_cpu, slice);
        board->running_sound = 0;
        board->sound_cycles += slice;
        qsound_sync(board, board->sound_cycles);
        board->irq_remainder += slice;
        cycles -= slice;
        if (board->irq_remainder == CAPCOM_ZN_IRQ_PERIOD) {
            board->irq_remainder = 0;
            board->irq_ticks++;
            if (!board->irq_asserted) {
                board->irq_asserted = 1;
                z80_irq(board->sound_cpu, 1);
            }
        }
    }
    return board->faults ? CAPCOM_ZN_FAULT : CAPCOM_ZN_HANDLED;
}

uint8_t capcom_zn_sound_read(CapcomZn *board, uint16_t address)
{
    return board ? sound_memory_read(board, address) : 0xff;
}

void capcom_zn_sound_write(CapcomZn *board, uint16_t address, uint8_t value)
{
    if (board)
        sound_memory_write(board, address, value);
}

uint8_t capcom_zn_sound_port_read(CapcomZn *board, uint16_t port)
{
    return board ? sound_port_read(board, port) : 0xff;
}

void capcom_zn_sound_port_write(CapcomZn *board, uint16_t port, uint8_t value)
{
    if (board)
        sound_port_write(board, port, value);
}

void capcom_zn_get_status(const CapcomZn *board, capcom_zn_status_t *status)
{
    Z80Status z80_status;
    if (!board || !status)
        return;
    status->main_bank = board->main_bank;
    status->main_bank_offset = CAPCOM_ZN_MAIN_FIXED_SIZE +
        (size_t)board->main_bank * CAPCOM_ZN_MAIN_BANK_SIZE;
    status->sound_bank = board->sound_bank;
    status->sound_bank_offset = CAPCOM_ZN_SOUND_ROM_BANK_BASE +
        (size_t)board->sound_bank * CAPCOM_ZN_SOUND_BANK_SIZE;
    status->sound_latch = board->sound_latch;
    status->sound_latch_pending = board->sound_latch_pending;
    status->irq_asserted = board->irq_asserted;
    status->sound_cycles = board->sound_cycles;
    status->irq_ticks = board->irq_ticks;
    status->irq_acknowledgements = board->irq_acknowledgements;
    status->faults = board->faults;
    z80_get_status(board->sound_cpu, &z80_status);
    status->z80_cycles = z80_status.cycles - board->z80_cycle_origin;
    status->z80_instructions = z80_status.instructions;
    status->z80_unsupported = z80_status.unsupported_operations;
    status->z80_nmis = z80_status.nmis;
    status->z80_pc = z80_status.pc;
    status->z80_cycle_debt = z80_status.cycle_debt;
    status->qsound_sound_cycles = board->qsound_sound_cycles;
    status->qsound_cycles = board->qsound_cycles;
    status->qsound_cycle_fraction = board->qsound_cycle_fraction;
    status->qsound_hle_attached = board->config.qsound_hle != NULL;
}
