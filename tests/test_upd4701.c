/* SPDX-License-Identifier: BSD-3-Clause. Independent source counter contracts.
 */
#include "../libpcsxcore/arcade/upd4701.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  Upd4701 d;
  upd4701_init(&d);
  assert(upd4701_data(&d) == 0 && upd4701_cf(&d) && upd4701_sf(&d));
  upd4701_update(&d, 0xabc, 0x123);
  assert(!upd4701_cf(&d));
  assert(upd4701_read_xy(&d, 0) == 0xbc && upd4701_read_xy(&d, 1) == 0x0a);
  assert(upd4701_read_xy(&d, 2) == 0x23 && upd4701_read_xy(&d, 3) == 0x01 &&
         upd4701_cf(&d));
  upd4701_cs(&d, 0);
  upd4701_add(&d, 17, -17);
  assert(upd4701_read_xy(&d, 0) == 0xbc);
  upd4701_cs(&d, 1);
  assert(upd4701_read_xy(&d, 0) == 0xcd && upd4701_read_xy(&d, 2) == 0x12);
  upd4701_switch(&d, 4, 0);
  assert(!upd4701_sf(&d));
  assert(upd4701_read_xy(&d, 1) == 0xca);
  upd4701_switch(&d, 2, 0);
  upd4701_switch(&d, 1, 0);
  assert(upd4701_read_xy(&d, 3) == 0xf1);
  upd4701_switch(&d, 7, 1);
  assert(upd4701_sf(&d));
  assert(upd4701_reset_xy_read(&d, 1) == 0 && upd4701_read_xy(&d, 0) == 0xcd);
  assert(upd4701_reset_xy_read(&d, 0) == 0 && upd4701_read_xy(&d, 0) == 0 &&
         upd4701_read_xy(&d, 2) == 0);
  upd4701_reset_x(&d, 1);
  upd4701_add(&d, 40, -1);
  upd4701_reset_x(&d, 0);
  assert(upd4701_read_xy(&d, 0) == 0 && upd4701_read_xy(&d, 2) == 255 &&
         upd4701_read_xy(&d, 3) == 15);
  upd4701_add(&d, 4095, 1);
  assert(upd4701_read_xy(&d, 0) == 255 && upd4701_read_xy(&d, 1) == 15);
  upd4701_add(&d, 1, 0);
  assert(upd4701_read_xy(&d, 0) == 0 && upd4701_read_xy(&d, 1) == 0);
  upd4701_init(&d);
  upd4701_update(&d, 4095, 4095);
  upd4701_update(&d, 0, 0);
  assert(upd4701_read_xy(&d, 0) == 0 && upd4701_read_xy(&d, 2) == 0);
  upd4701_recalibrate(&d, 50, 60);
  upd4701_update(&d, 55, 58);
  assert(upd4701_read_xy(&d, 0) == 5 && upd4701_read_xy(&d, 2) == 254 &&
         upd4701_read_xy(&d, 3) == 15);
  puts("UPD4701 source latch, byte select, switches, counter/reset/frame wrap "
       "and recalibration PASS");
}
