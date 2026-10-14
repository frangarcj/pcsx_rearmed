/* SPDX-License-Identifier: BSD-3-Clause. Source bus contracts, no game media.
 */
#include "../libpcsxcore/arcade/simpbowl_io.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void wr(SimpBowlIO *d, unsigned off, unsigned v) {
  assert(simpbowl_io_write(d, 0x1f680080 + 2 * off, 2, v, 0xffff) == 1);
}
static unsigned rd(SimpBowlIO *d, unsigned off) {
  uint32_t v;
  assert(simpbowl_io_read(d, 0x1f680080 + 2 * off, 2, &v) == 1);
  return v;
}
static void address(SimpBowlIO *d, unsigned a) {
  wr(d, 1, a >> 1);
  wr(d, 2, (a >> 8) & 0xffff);
  wr(d, 3, a >> 15);
  if (a & 1)
    assert(rd(d, 4) == 0);
}
static void command(SimpBowlIO *d, unsigned a, unsigned v) {
  address(d, a);
  wr(d, 0, v);
}
static void unlock(SimpBowlIO *d, unsigned base, unsigned v) {
  command(d, base + 0x555, 0xaaaa);
  command(d, base + 0x2aa, 0x5555);
  command(d, base + 0x555, v);
}
int main(void) {
  uint8_t *b = malloc(SIMPBOWL_FLASH_BYTES);
  assert(b);
  memset(b, 255, SIMPBOWL_FLASH_BYTES);
  b[0x12] = 0x34;
  b[0x200012] = 0x56;
  b[0x400012] = 0x78;
  b[0x600012] = 0x9a;
  assert(!simpbowl_io_create(b, SIMPBOWL_FLASH_BYTES - 1));
  SimpBowlIO *d = simpbowl_io_create(b, SIMPBOWL_FLASH_BYTES);
  assert(d);
  address(d, 0x12);
  assert(rd(d, 0) == 0x5634);
  address(d, 0x200012);
  assert(rd(d, 0) == 0x9a78);
  address(d, 0x12);
  uint32_t v;
  assert(simpbowl_io_read(d, 0x1f680080, 4, &v) == 1 && v == 0x00005634);
  SimpBowlIOStatus st;
  simpbowl_io_status(d, &st);
  assert(st.address == 0x13); // one 16-bit delegate, not per-byte increments
  address(d, 0x12);
  assert(simpbowl_io_read(d, 0x1f680081, 1, &v) == 1 && v == 0x56);
  address(d, 0x12);
  wr(d, 4, 0);
  simpbowl_io_status(d, &st);
  assert(st.address == 0x12); // odd select is a read side effect
  assert(simpbowl_io_read(d, 0x1f680088, 1, &v) == 1);
  simpbowl_io_status(d, &st);
  assert(st.address == 0x13);
  unlock(d, 0, 0x9090);
  address(d, 0);
  assert(rd(d, 0) == 0x0404 && rd(d, 0) == 0xadad);
  command(d, 0, 0xf0f0);
  unlock(d, 0x200000, 0xa0a0);
  command(d, 0x200123, 0x2718);
  address(d, 0x200123);
  assert(rd(d, 0) == 0x2718);
  unlock(d, 0x200000, 0x8080);
  command(d, 0x200555, 0xaaaa);
  command(d, 0x2002aa, 0x5555);
  command(d, 0x200123, 0x3030);
  assert(!simpbowl_io_advance_to(d, 999999999));
  address(d, 0x200123);
  assert((rd(d, 0) & 0x4444) == 0x4444);
  assert(!simpbowl_io_advance_to(d, 1000000000));
  address(d, 0x200123);
  assert(rd(d, 0) == 0xffff);
  simpbowl_io_frame(d, 0xabc, 0x123);
  assert(simpbowl_io_read(d, 0x1f6800c0, 4, &v) == 1 && v == 0x0a00bc00);
  assert(simpbowl_io_read(d, 0x1f6800c4, 4, &v) == 1 && v == 0x01002300);
  assert(simpbowl_io_read(d, 0x1f6800c9, 1, &v) == 1 && v == 0);
  assert(simpbowl_io_read(d, 0x1f6800c0, 4, &v) == 1 && v == 0);
  simpbowl_io_frame(d, 0xac0, 0x120);
  assert(simpbowl_io_read(d, 0x1f6800c0, 4, &v) == 1 && v == 0x00000400);
  assert(simpbowl_io_read(d, 0x1f6800c4, 4, &v) == 1 && v == 0x0f00fd00);
  simpbowl_io_status(d, &st);
  assert(st.fault == 0 && st.frames == 2 && st.reset_reads == 1 &&
         st.writes > 0);
  simpbowl_io_destroy(d);
  d = simpbowl_io_create(b, SIMPBOWL_FLASH_BYTES);
  assert(d);
  assert(b[0x400123] == 255);
  simpbowl_io_destroy(d);
  free(b);
  puts("Simpsons source four flash chips, IDs, odd/address latches, delegate "
       "widths, program/timed erase and UPD4701 lanes/reset PASS");
}
