/* SPDX-License-Identifier: BSD-3-Clause. MAME konamigv.cpp, R. Belmont/smf.
 * deb2f54fe7300e7a192690318beb860159bfd52a. See konamigv_LICENSE.txt. */
#include "simpbowl_io.h"
#include "nor_flash.h"
#include "upd4701.h"
#include <stdlib.h>
#include <string.h>
struct SimpBowlIO {
  nor_flash_t *flash[4];
  Upd4701 track;
  uint64_t ns;
  SimpBowlIOStatus status;
};
SimpBowlIO *simpbowl_io_create(uint8_t *images, size_t size) {
  if (!images || size != SIMPBOWL_FLASH_BYTES)
    return NULL;
  SimpBowlIO *d = calloc(1, sizeof(*d));
  if (!d)
    return NULL;
  upd4701_init(&d->track);
  for (unsigned i = 0; i < 4; i++) {
    d->flash[i] = nor_flash_create(NOR_FLASH_FUJITSU_MBM29F016A,
                                   images + i * SIMPBOWL_FLASH_CHIP_BYTES,
                                   SIMPBOWL_FLASH_CHIP_BYTES);
    if (!d->flash[i]) {
      simpbowl_io_destroy(d);
      return NULL;
    }
  }
  return d;
}
void simpbowl_io_destroy(SimpBowlIO *d) {
  if (d) {
    for (unsigned i = 0; i < 4; i++)
      nor_flash_destroy(d->flash[i]);
    free(d);
  }
}
int simpbowl_io_advance_to(SimpBowlIO *d, uint64_t ns) {
  if (!d || ns < d->ns) {
    if (d)
      d->status.fault = 1;
    return -1;
  }
  uint64_t us = ns / 1000 - d->ns / 1000;
  for (unsigned i = 0; i < 4; i++)
    nor_flash_advance_us(d->flash[i], us);
  d->ns = ns;
  return d->status.fault ? -1 : 0;
}
void simpbowl_io_frame(SimpBowlIO *d, uint16_t x, uint16_t y) {
  if (d) {
    upd4701_update(&d->track, x, y);
    d->status.frames++;
    d->status.x = x & 0xfff;
    d->status.y = y & 0xfff;
  }
}
static uint16_t flash_read(SimpBowlIO *d, unsigned offset) {
  if (offset == 4)
    d->status.address |= 1;
  if (offset != 0)
    return 0;
  unsigned chip = d->status.address >= 0x200000 ? 2 : 0;
  uint32_t a = d->status.address & 0x1fffff;
  uint16_t lo = 0, hi = 0;
  if (nor_flash_read(d->flash[chip], a, &lo) ||
      nor_flash_read(d->flash[chip + 1], a, &hi))
    d->status.fault = 1;
  d->status.address++;
  d->status.reads++;
  return (lo & 0xff) | (hi << 8);
}
static void flash_write(SimpBowlIO *d, unsigned offset, uint16_t value) {
  uint32_t *a = &d->status.address;
  switch (offset) {
  case 0: {
    unsigned chip = *a >= 0x200000 ? 2 : 0;
    uint32_t addr = *a & 0x1fffff;
    if (nor_flash_write(d->flash[chip], addr, value & 0xff) ||
        nor_flash_write(d->flash[chip + 1], addr, value >> 8))
      d->status.fault = 1;
    d->status.writes++;
    break;
  }
  case 1:
    *a = (uint32_t)value << 1;
    break;
  case 2:
    *a = (*a & 0xff00ff) | ((uint32_t)value << 8);
    break;
  case 3:
    *a = (*a & 0x00ffff) | ((uint32_t)value << 15);
    break;
  }
}
static int width_valid(unsigned w) { return w == 1 || w == 2 || w == 4; }
int simpbowl_io_read(SimpBowlIO *d, uint32_t a, unsigned w, uint32_t *v) {
  if (!d || !v || !width_valid(w) || a > UINT32_MAX - (w - 1))
    return 0;
  uint32_t out = 0;
  unsigned old_word = ~0u;
  uint16_t word = 0;
  int handled = 0;
  for (unsigned lane = 0; lane < w; lane++) {
    uint32_t at = a + lane;
    uint8_t b = 0;
    if (at >= 0x1f680080 && at < 0x1f680090) {
      unsigned index = (at - 0x1f680080) >> 1;
      if (index != old_word) {
        word = flash_read(d, index);
        old_word = index;
      }
      b = (uint8_t)(word >> ((at & 1) * 8));
      handled = 1;
    } else if (at >= 0x1f6800c0 && at < 0x1f6800c8) {
      if (at & 1)
        b = upd4701_read_xy(&d->track, (at - 0x1f6800c0) >> 1);
      handled = 1;
    } else if (at == 0x1f6800c9) {
      b = upd4701_reset_xy_read(&d->track, 0);
      d->status.reset_reads++;
      handled = 1;
    }
    out |= (uint32_t)b << (lane * 8);
  }
  if (!handled)
    return 0;
  *v = out;
  return d->status.fault ? -1 : 1;
}
int simpbowl_io_write(SimpBowlIO *d, uint32_t a, unsigned w, uint32_t value,
                      uint32_t mask) {
  if (!d || !width_valid(w) || a > UINT32_MAX - (w - 1))
    return 0;
  int handled = 0;
  unsigned pending = ~0u;
  uint16_t data = 0;
  for (unsigned lane = 0; lane < w; lane++) {
    uint32_t at = a + lane;
    if (at < 0x1f680080 || at >= 0x1f680090)
      continue;
    handled = 1;
    if (!(mask & (0xffu << (lane * 8))))
      continue;
    unsigned word = (at - 0x1f680080) >> 1;
    if (pending != word) {
      if (pending != ~0u)
        flash_write(d, pending, data);
      pending = word;
      data = 0;
    }
    data |= (uint16_t)(((value >> (lane * 8)) & 0xff) << ((at & 1) * 8));
  }
  if (pending != ~0u)
    flash_write(d, pending, data);
  return !handled ? 0 : d->status.fault ? -1 : 1;
}
void simpbowl_io_status(const SimpBowlIO *d, SimpBowlIOStatus *s) {
  if (s) {
    memset(s, 0, sizeof(*s));
    if (d)
      *s = d->status;
  }
}
