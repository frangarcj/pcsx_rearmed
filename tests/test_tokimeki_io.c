/* SPDX-License-Identifier: BSD-3-Clause. Independent source serial/timer
 * contracts. */
#include "../libpcsxcore/arcade/tokimeki_io.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t now;
static unsigned captures, prints;
static uint8_t rgb[320 * 240 * 3];
static int video(void *p, const uint8_t **b, unsigned *w, unsigned *h,
                 unsigned *s) {
  (void)p;
  *b = rgb;
  *w = 320;
  *h = 240;
  *s = 960;
  captures++;
  return 1;
}
static void printed(void *p, const uint8_t *b, unsigned w, unsigned h,
                    unsigned stride) {
  (void)p;
  assert(w == 800 && h == 600 && stride == 2400);
  assert(b);
  prints++;
}
static void pulse(TokimekiIO *d, unsigned ticks) {
  tokimeki_io_serial_write(d, 1);
  now += ((uint64_t)ticks * 1000000000 + 4233599) / 4233600;
  assert(!tokimeki_io_advance_to(d, now));
  tokimeki_io_serial_write(d, 0);
  now += 1000;
  assert(!tokimeki_io_advance_to(d, now));
}
static void command(TokimekiIO *d, unsigned cmd, unsigned trailer,
                    int mismatch) {
  for (unsigned rep = 0; rep < 3; rep++) {
    pulse(d, 9600);
    unsigned v = cmd + (mismatch && rep == 1);
    for (unsigned b = 0; b < 7; b++)
      pulse(d, (v >> b) & 1 ? 4800 : 2400);
    for (unsigned b = 0; b < 8; b++)
      pulse(d, (trailer >> b) & 1 ? 4800 : 2400);
  }
}
int main(void) {
  for (unsigned i = 0; i < sizeof(rgb); i++)
    rgb[i] = (uint8_t)(i * 7 + 13);
  TokimekiIOConfig cfg = {0xf073, 0, NULL, video, printed};
  TokimekiIO *d = tokimeki_io_create(&cfg);
  assert(d);
  assert(!tokimeki_io_advance_to(d, 0));
  assert((tokimeki_io_serial_read(d) & 4) == 0);
  assert((tokimeki_io_serial_read(d) & 4) == 4);
  assert(!tokimeki_io_advance_to(d, 749999999));
  assert(tokimeki_io_serial_read(d) == 4);
  assert(!tokimeki_io_advance_to(d, 750000000));
  assert(tokimeki_io_serial_read(d) == 0);
  now = 750000000;
  for (unsigned b = 0; b < 16; b++) {
    assert(tokimeki_io_device_bit(d) == ((0xf073u >> (15 - b)) & 1));
    tokimeki_io_device_line(d, 1);
    tokimeki_io_device_line(d, 1);
    tokimeki_io_device_line(d, 0);
  }
  assert(tokimeki_io_device_bit(d) == 1);
  tokimeki_io_inputs(d, 31, 0xa5, 3);
  tokimeki_io_serial_write(d, 2);
  for (int b = 4; b >= 0; b--) {
    unsigned v = ((0x1a >> b) & 1) << 4;
    tokimeki_io_serial_write(d, v);
    tokimeki_io_serial_write(d, v | 0x20);
  }
  assert(!(tokimeki_io_serial_read(d) & 8));
  tokimeki_io_serial_write(d, 0);
  tokimeki_io_serial_write(d, 0x20);
  unsigned bits = 0;
  for (unsigned n = 0; n < 8; n++) {
    tokimeki_io_serial_write(d, 0);
    tokimeki_io_serial_write(d, 0x20);
    bits = bits << 1 | ((tokimeki_io_serial_read(d) >> 3) & 1);
  }
  assert(bits == 0xa5);
  tokimeki_io_inputs(d, 0, 0, 0);
  assert((tokimeki_io_serial_read(d) & 0xa0) == 0xa0);
  tokimeki_io_inputs(d, 31, 0, 3);
  command(d, 0x10, 0xf9, 1);
  command(d, 0x10, 0xf8, 0);
  TokimekiIOStatus st;
  tokimeki_io_status(d, &st);
  assert(st.rejected_packets == 2 && !st.captures);
  command(d, 0x10, 0xf9, 0);
  tokimeki_io_vblank(d, 0);
  assert(!captures);
  tokimeki_io_vblank(d, 1);
  assert(captures == 1);
  const uint8_t *page = tokimeki_io_page(d);
  assert(page[0] == 255 && !memcmp(page + (180 * 800 + 240) * 3, rgb, 3));
  command(d, 0x11, 0xf9, 0);
  assert(prints == 1 && (tokimeki_io_serial_read(d) & 64));
  tokimeki_io_status(d, &st);
  uint64_t deadline = st.now_ns - 1000 + 1000000000;
  assert(!tokimeki_io_advance_to(d, deadline - 1));
  assert(tokimeki_io_serial_read(d) & 64);
  assert(!tokimeki_io_advance_to(d, deadline));
  assert(!(tokimeki_io_serial_read(d) & 64));
  now = deadline;
  command(d, 0x17, 0xf9, 0);
  assert(tokimeki_io_page(d)[(180 * 800 + 240) * 3] == 255);
  tokimeki_io_status(d, &st);
  assert(st.printer_packets == 3 && st.captures == 1 && st.prints == 1 &&
         !st.fault);
  tokimeki_io_destroy(d);
  now = 0;
  cfg.manual_layout = 1;
  cfg.device_initial = 0xf0ba;
  d = tokimeki_io_create(&cfg);
  assert(d);
  for (unsigned n = 0; n < 16; n++) {
    command(d, 0x10, 0xf9, 0);
    tokimeki_io_vblank(d, 0);
    tokimeki_io_vblank(d, 1);
  }
  page = tokimeki_io_page(d);
  assert(!memcmp(page + (80 * 800 + 81) * 3, rgb + (40 * 320 + 1) * 3, 3));
  assert(!memcmp(page + (413 * 800 + 561) * 3, rgb + (40 * 320 + 1) * 3, 3));
  tokimeki_io_status(d, &st);
  assert(st.cell == 16 && st.captures == 16);
  tokimeki_io_reset(d);
  assert(tokimeki_io_device_bit(d) == 1 &&
         tokimeki_io_page(d)[(80 * 800 + 81) * 3] == 255);
  tokimeki_io_destroy(d);
  puts("Tokimeki source identity rotation, heartbeat timer, GSR serial bits, "
       "printer triplets, busy deadline and real-frame full/4x4 page capture "
       "PASS");
}
