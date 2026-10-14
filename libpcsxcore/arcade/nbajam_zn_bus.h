/* GPL-2.0-or-later. Source NBA Jam Extreme ROM/SRAM/register map. */
#ifndef PCSX_NBAJAM_ZN_BUS_H
#define PCSX_NBAJAM_ZN_BUS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NbajamZnBus {
 const uint8_t *rom;size_t rom_size;
 uint8_t sram[32768];uint16_t latch[2];unsigned page,bank[2];
 uint64_t bank_writes,sram_reads,sram_writes,commands;uint32_t faults;
 void *opaque;int (*command)(void *,uint16_t);
} NbajamZnBus;
int nbajam_zn_bus_init(NbajamZnBus *,const uint8_t *,size_t,void *,int (*)(void *,uint16_t));
void nbajam_zn_bus_reset(NbajamZnBus *);
int nbajam_zn_bus_read(NbajamZnBus *,uint32_t,unsigned,uint32_t *);
int nbajam_zn_bus_write(NbajamZnBus *,uint32_t,unsigned,uint32_t);
/* Return a complete read-only ROM span for native instruction-fetch mapping.
 * Non-ROM spans, including SRAM, deliberately remain on the bus path. */
const uint8_t *nbajam_zn_bus_rom_span(const NbajamZnBus *,uint32_t,size_t);
#ifdef __cplusplus
}
#endif
#endif
