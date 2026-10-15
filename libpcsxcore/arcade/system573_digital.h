/* SPDX-License-Identifier: BSD-3-Clause
 * Pinned MAME Konami digital I/O/FPGA model, smf/windyfairy.
 */
#ifndef PCSX_SYSTEM573_DIGITAL_H
#define PCSX_SYSTEM573_DIGITAL_H
#include <stddef.h>
#include <stdint.h>
#include "mas3507d.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct System573Digital System573Digital;
typedef void (*system573_digital_output_t)(void *, unsigned line, int level);
enum {
    SYSTEM573_DIGITAL_RAM_BYTES = 0x2000000,
    SYSTEM573_DIGITAL_FAULT_RAM = 2,
    SYSTEM573_DIGITAL_FAULT_TIME = 4,
    SYSTEM573_DIGITAL_FAULT_REENTRY = 8,
    SYSTEM573_DIGITAL_FAULT_SCHEDULER = 16,
    SYSTEM573_DIGITAL_FAULT_DECODER = 32
};
typedef struct System573DigitalStatus {
    uint64_t now_ns, words_decrypted, bytes_sent, stream_hash;
    uint32_t start, end, current, ram_read_address, ram_write_address, latched_counter;
    uint16_t fpga_control, mpeg_status, frame_counter, crypto1, crypto2, network_id;
    uint8_t crypto3, pending_bytes, output[8];
    unsigned faults;
    Mas3507DStatus decoder;
} System573DigitalStatus;
/* Requires the actual eight-byte digital board ID, not the cassette serial.
 * Allocates source-default zeroed 32 MiB volatile RAM. solo_bass_mix=0 or 1
 * selects the source FPGA variant. Outputs are source-permuted logical lines.
 * Callbacks may inspect status but must schedule mutations after returning. */
System573Digital *system573_digital_create(const uint8_t *id, size_t id_size,
    int solo_bass_mix, system573_digital_output_t output, void *opaque);
void system573_digital_destroy(System573Digital *chip);
/* Source reset preserves volatile RAM backing; clears device state, not
 * latched host-interface fault diagnostics. Time remains monotonic. */
void system573_digital_reset(System573Digital *chip);
int system573_digital_advance_to(System573Digital *chip, uint64_t now_ns);
/* Logical byte offsets in the source 0x100-byte aperture. Halfword accesses;
 * returns 1 mapped, 0 unmapped, -1 invalid/host fault. No invented open bus. */
int system573_digital_read16(System573Digital *chip, unsigned offset, uint16_t *data);
int system573_digital_write16(System573Digital *chip, unsigned offset, uint16_t data);
/* Source-rate float stereo. Advance emulated time separately. Decoder demand
 * schedules zero-delay source FPGA byte events, drained outside callbacks.
 * This component does not yet provide PSX MMIO, speaker mixing or resampling. */
int system573_digital_render(System573Digital *chip, float *stereo, unsigned frames);
void system573_digital_get_status(const System573Digital *chip, System573DigitalStatus *status);
#ifdef __cplusplus
}
#endif
#endif
