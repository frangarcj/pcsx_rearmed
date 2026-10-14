/* SPDX-License-Identifier: BSD-3-Clause. MAME sony/zn.cpp, smf/R. Belmont.
 * deb2f54fe7300e7a192690318beb860159bfd52a. See sony_zn_LICENSE.txt. */
#ifndef PCSX_METRO_ZN_BUS_H
#define PCSX_METRO_ZN_BUS_H
#include <stddef.h>
#include <stdint.h>
#define METRO_ZN_ROM_BYTES 0x2c00000u
/* This reproduces the pinned driver's provisional MCU handlers. It does not
 * emulate the undumped H8/3644, its drive authentication or ATA data transport. */
typedef struct MetroZnBus {
    const uint8_t *rom;size_t rom_size;
    uint16_t command;uint8_t bank;
    uint64_t reads[4],writes[4],bank_writes,provisional_status_reads,unpopulated_reads;
} MetroZnBus;
int metro_zn_bus_init(MetroZnBus *,const uint8_t *,size_t);
void metro_zn_bus_reset(MetroZnBus *); /* Bank 1; source preserves command. */
const uint8_t *metro_zn_bus_rom_span(const MetroZnBus *,uint32_t,size_t);
int metro_zn_bus_read(MetroZnBus *,uint32_t,unsigned,uint32_t *);
int metro_zn_bus_write(MetroZnBus *,uint32_t,unsigned,uint32_t);
#endif
