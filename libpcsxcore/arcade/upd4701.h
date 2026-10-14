/* SPDX-License-Identifier: BSD-3-Clause
 * Source: MAME upd4701.cpp/.h at deb2f54fe7300e7a192690318beb860159bfd52a.
 * Copyright smf, AJR. See upd4701_LICENSE.txt. */
#ifndef PCSX_ARCADE_UPD4701_H
#define PCSX_ARCADE_UPD4701_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Upd4701 {
  uint16_t x, y, start_x, start_y, latch_x, latch_y, last_x, last_y;
  uint8_t cs, xy, ul, reset_x, reset_y, switches, latch_switches, cf;
} Upd4701;
void upd4701_init(Upd4701 *d);
/* Source frame notifier samples absolute 12-bit input port counts. */
void upd4701_update(Upd4701 *d, uint16_t x, uint16_t y);
void upd4701_recalibrate(Upd4701 *d, uint16_t x, uint16_t y);
void upd4701_add(Upd4701 *d, int16_t dx, int16_t dy);
void upd4701_cs(Upd4701 *d, int state);
void upd4701_xy(Upd4701 *d, int state);
void upd4701_ul(Upd4701 *d, int state);
void upd4701_reset_x(Upd4701 *d, int state);
void upd4701_reset_y(Upd4701 *d, int state);
uint8_t upd4701_reset_xy_read(Upd4701 *d, int side_effects_disabled);
uint8_t upd4701_data(const Upd4701 *d);
uint8_t upd4701_read_xy(Upd4701 *d, unsigned offset);
/* Source pins are active low: 0 presses, 1 releases. Mask: L=4,R=2,M=1. */
void upd4701_switch(Upd4701 *d, uint8_t mask, int state);
int upd4701_cf(const Upd4701 *d);
int upd4701_sf(const Upd4701 *d);
#ifdef __cplusplus
}
#endif
#endif
