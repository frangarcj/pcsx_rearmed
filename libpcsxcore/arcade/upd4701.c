/* SPDX-License-Identifier: BSD-3-Clause
 * MAME upd4701.cpp/.h, deb2f54fe7300e7a192690318beb860159bfd52a.
 * Copyright smf, AJR. See upd4701_LICENSE.txt.
 * Explicit unsigned 16-bit wrap preserves source s16 counter arithmetic. */
#include "upd4701.h"
#include <string.h>
void upd4701_init(Upd4701 *d) {
  memset(d, 0, sizeof(*d));
  d->cs = d->cf = 1;
}
void upd4701_add(Upd4701 *d, int16_t dx, int16_t dy) {
  if (!d->reset_x && dx) {
    d->x = (uint16_t)(d->x + dx);
    if (d->cs && d->cf)
      d->cf = 0;
  }
  if (!d->reset_y && dy) {
    d->y = (uint16_t)(d->y + dy);
    if (d->cs && d->cf)
      d->cf = 0;
  }
}
void upd4701_update(Upd4701 *d, uint16_t x, uint16_t y) {
  x &= 0xfff;
  y &= 0xfff;
  upd4701_add(d, (int16_t)((int)x - d->last_x), (int16_t)((int)y - d->last_y));
  d->last_x = x;
  d->last_y = y;
}
void upd4701_recalibrate(Upd4701 *d, uint16_t x, uint16_t y) {
  d->last_x = x & 0xfff;
  d->last_y = y & 0xfff;
}
void upd4701_cs(Upd4701 *d, int state) {
  state = !!state;
  if (d->cs == state)
    return;
  d->cs = state;
  if (!state) {
    d->latch_x = (d->x - d->start_x) & 0xfff;
    d->latch_y = (d->y - d->start_y) & 0xfff;
    d->latch_switches = d->switches;
    if (d->switches)
      d->latch_switches |= 8;
    d->cf = 1;
  }
}
void upd4701_xy(Upd4701 *d, int state) { d->xy = !!state; }
void upd4701_ul(Upd4701 *d, int state) { d->ul = !!state; }
void upd4701_reset_x(Upd4701 *d, int state) {
  state = !!state;
  if (d->reset_x != state) {
    d->reset_x = state;
    if (state)
      d->start_x = d->x;
  }
}
void upd4701_reset_y(Upd4701 *d, int state) {
  state = !!state;
  if (d->reset_y != state) {
    d->reset_y = state;
    if (state)
      d->start_y = d->y;
  }
}
uint8_t upd4701_reset_xy_read(Upd4701 *d, int disabled) {
  if (!disabled) {
    upd4701_reset_x(d, 1);
    upd4701_reset_y(d, 1);
    upd4701_reset_x(d, 0);
    upd4701_reset_y(d, 0);
  }
  return 0;
}
uint8_t upd4701_data(const Upd4701 *d) {
  if (d->cs)
    return 0;
  uint16_t v = (d->xy ? d->latch_y : d->latch_x) | (d->latch_switches << 12);
  return d->ul ? v >> 8 : v;
}
uint8_t upd4701_read_xy(Upd4701 *d, unsigned offset) {
  int old = d->cs;
  upd4701_cs(d, 0);
  upd4701_xy(d, offset & 2);
  upd4701_ul(d, offset & 1);
  uint8_t result = upd4701_data(d);
  upd4701_cs(d, old);
  return result;
}
void upd4701_switch(Upd4701 *d, uint8_t mask, int state) {
  mask &= 7;
  if (state)
    d->switches &= ~mask;
  else
    d->switches |= mask;
}
int upd4701_cf(const Upd4701 *d) { return d->cf; }
int upd4701_sf(const Upd4701 *d) { return d->switches == 0; }
