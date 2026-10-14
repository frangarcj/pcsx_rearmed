/* GPL-2.0-or-later. Judge Dredd source ATA/gun daughterboard wiring. */
#ifndef PCSX_JDREDD_ZN_BUS_H
#define PCSX_JDREDD_ZN_BUS_H
#include "ata_storage.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct JdreddZnBus {
 ata_storage_t *ata;const uint8_t *rom;size_t rom_size;
 uint16_t gun_x[2],gun_y[2];uint8_t gun_mux;
 uint64_t reads[2],writes[2],vblanks,gun_latches,offscreen;
 uint32_t faults;void *opaque;void (*lightgun)(void *,uint16_t,uint16_t);
} JdreddZnBus;
int jdredd_zn_bus_init(JdreddZnBus *,ata_storage_t *,const uint8_t *,size_t,void *,void (*)(void *,uint16_t,uint16_t));
void jdredd_zn_bus_reset(JdreddZnBus *); /* Source preserves mux/inputs/ROM. */
void jdredd_zn_bus_vblank(JdreddZnBus *,int state);
int jdredd_zn_bus_gun(JdreddZnBus *,unsigned player,uint16_t x,uint16_t y);
int jdredd_zn_bus_read(JdreddZnBus *,uint32_t address,unsigned width,uint32_t *);
int jdredd_zn_bus_write(JdreddZnBus *,uint32_t address,unsigned width,uint32_t data);
#ifdef __cplusplus
}
#endif
#endif
