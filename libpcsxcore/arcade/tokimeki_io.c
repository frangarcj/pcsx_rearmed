/* SPDX-License-Identifier: BSD-3-Clause. MAME konamigv.cpp, R. Belmont/smf,
 * deb2f54fe7300e7a192690318beb860159bfd52a. See konamigv_LICENSE.txt. */
#include "tokimeki_io.h"
#include <stdlib.h>
#include <string.h>
struct TokimekiIO {
  TokimekiIOConfig cfg;
  TokimekiIOStatus s;
  uint64_t heartbeat_due, print_due, pulse_start;
  uint8_t heart, gsr, meta, heartbeat_signal, device_line, printer_bit, curbit,
      curbyte, packet[6], serial_val, serial_clk, vblank, pending;
  uint8_t page[800 * 600 * 3];
};
void tokimeki_io_reset(TokimekiIO *d) {
  if (!d)
    return;
  d->s.device = d->cfg.device_initial;
  d->s.sensor_data = 0;
  d->s.sensor_id = d->s.sensor_len = 0;
  d->serial_val = d->serial_clk = 0;
  d->heartbeat_signal = 1;
  d->printer_bit = d->curbit = d->curbyte = 0;
  d->s.printing = 0;
  d->s.cell = 0;
  d->vblank = 0;
  memset(d->packet, 0, 6);
  memset(d->page, 255, sizeof(d->page));
}
TokimekiIO *tokimeki_io_create(const TokimekiIOConfig *cfg) {
  if (!cfg)
    return NULL;
  TokimekiIO *d = calloc(1, sizeof(*d));
  if (!d)
    return NULL;
  d->cfg = *cfg;
  d->heart = 31;
  d->meta = 3;
  tokimeki_io_reset(d);
  return d;
}
void tokimeki_io_destroy(TokimekiIO *d) { free(d); }
int tokimeki_io_advance_to(TokimekiIO *d, uint64_t ns) {
  if (!d || ns < d->s.now_ns) {
    if (d)
      d->s.fault = 1;
    return -1;
  }
  unsigned budget = 0;
  while (d->heartbeat_due <= ns) {
    if (++budget > 1000000) {
      d->s.fault = 1;
      return -1;
    }
    uint64_t delay =
        (d->heart ? 60000u / (d->heart + 49u) : 100u) * UINT64_C(1000000);
    if (d->heart)
      d->heartbeat_signal = 0;
    d->s.heartbeat_ticks++;
    if (d->heartbeat_due > UINT64_MAX - delay) {
      d->s.fault = 1;
      return -1;
    }
    d->heartbeat_due += delay;
  }
  if (d->print_due && d->print_due <= ns) {
    d->s.printing = 0;
    d->print_due = 0;
  }
  d->s.now_ns = ns;
  return d->s.fault ? -1 : 0;
}
void tokimeki_io_inputs(TokimekiIO *d, unsigned heart, unsigned gsr,
                        unsigned meta) {
  if (d) {
    d->heart = heart & 255;
    d->gsr = gsr & 255;
    d->meta = meta & 3;
  }
}
void tokimeki_io_pulse(TokimekiIO *d, int state) {
  if (d && state)
    d->heartbeat_signal = 0;
}
void tokimeki_io_device_line(TokimekiIO *d, int state) {
  if (!d)
    return;
  state = !!state;
  if (state && !d->device_line)
    d->s.device = (uint16_t)((d->s.device << 1) | (d->s.device >> 15));
  d->device_line = state;
}
unsigned tokimeki_io_device_bit(const TokimekiIO *d) {
  return d ? d->s.device >> 15 : 0;
}
uint16_t tokimeki_io_serial_read(TokimekiIO *d) {
  if (!d)
    return 0;
  uint16_t v = d->heartbeat_signal << 2;
  d->heartbeat_signal = 1;
  if (d->s.sensor_id)
    v |= ((d->s.sensor_data >> 8) & 1) << 3;
  if (!(d->meta & 1))
    v |= 0x20;
  if (!(d->meta & 2))
    v |= 0x80;
  v |= d->s.printing << 6;
  d->s.reads++;
  return v;
}
static void packet_done(TokimekiIO *d) {
  if (d->packet[0] != d->packet[2] || d->packet[0] != d->packet[4] ||
      d->packet[1] != d->packet[3] || d->packet[1] != d->packet[5] ||
      d->packet[1] != 0xf9) {
    d->s.rejected_packets++;
    return;
  }
  unsigned cmd = d->packet[0];
  d->s.last_command = cmd;
  d->s.printer_packets++;
  if (cmd == 0x10)
    d->pending = 1;
  else if (cmd == 0x11) {
    if (d->s.now_ns > UINT64_MAX - UINT64_C(1000000000)) {
      d->s.fault = 1;
      return;
    }
    d->s.printing = 1;
    d->print_due = d->s.now_ns + UINT64_C(1000000000);
    d->s.prints++;
    if (d->cfg.printed)
      d->cfg.printed(d->cfg.opaque, d->page, 800, 600, 2400);
  } else if (cmd == 0x17) {
    memset(d->page, 255, sizeof(d->page));
    d->s.cell = 0;
  } else
    d->s.unknown_commands++;
}
void tokimeki_io_serial_write(TokimekiIO *d, uint16_t data) {
  if (!d)
    return;
  d->s.writes++;
  unsigned bit = data & 1;
  if (bit && !d->printer_bit)
    d->pulse_start = d->s.now_ns;
  else if (!bit && d->printer_bit) {
    uint64_t ns = d->s.now_ns - d->pulse_start;
    /* floor(duration * (67737600/16)); divide before multiplying to avoid
     * overflow. */
    uint64_t ticks =
        ns / UINT64_C(1000000000) * UINT64_C(4233600) +
        (ns % UINT64_C(1000000000)) * UINT64_C(4233600) / UINT64_C(1000000000);
    if (ticks >= 9000)
      d->curbit = 0;
    else {
      int one = ticks >= 4000 && ticks <= 5600;
      if (!d->curbit)
        d->packet[d->curbyte] = 0;
      d->packet[d->curbyte] |= one << d->curbit;
      d->curbit++;
      if ((!(d->curbyte & 1) && d->curbit == 7) ||
          ((d->curbyte & 1) && d->curbit == 8)) {
        d->curbyte++;
        d->curbit = 0;
      }
    }
    if (d->curbyte >= 6) {
      packet_done(d);
      d->curbyte = d->curbit = 0;
    }
  }
  d->printer_bit = bit;
  unsigned clk = (data >> 5) & 1;
  if (data & 2) {
    d->s.sensor_data = 0;
    d->s.sensor_id = 0;
    d->serial_val = 0;
    d->s.sensor_len = 0;
  } else if (!d->serial_clk && clk) {
    if (d->s.sensor_len < 5) {
      d->serial_val |= ((data >> 4) & 1) << (4 - d->s.sensor_len);
      if (d->s.sensor_len == 4) {
        d->s.sensor_id = d->serial_val;
        d->s.sensor_data = d->s.sensor_id == 0x1a ? d->gsr : 0;
        d->s.sensor_ids++;
      }
    } else if (d->s.sensor_len >= 6)
      d->s.sensor_data = (uint16_t)(d->s.sensor_data << 1);
    d->s.sensor_len++;
  }
  d->serial_clk = clk;
}
static void pixel(TokimekiIO *d, int x, int y, const uint8_t *p) {
  if (x >= 0 && x < 800 && y >= 0 && y < 600)
    memcpy(d->page + ((size_t)y * 800 + x) * 3, p, 3);
}
void tokimeki_io_vblank(TokimekiIO *d, int state) {
  if (!d)
    return;
  state = !!state;
  int rising = state && !d->vblank;
  d->vblank = state;
  if (!d->pending || !rising)
    return;
  const uint8_t *rgb = NULL;
  unsigned w = 0, h = 0, stride = 0;
  if (!d->cfg.capture ||
      !d->cfg.capture(d->cfg.opaque, &rgb, &w, &h, &stride) || !rgb ||
      w > 1024 || h > 1024 || !w || !h || stride < w * 3) {
    d->s.fault = 1;
    return;
  }
  d->pending = 0;
  if (d->cfg.manual_layout) {
    if (w <= 20 || h <= 56) {
      d->s.fault = 1;
      return;
    }
    unsigned cw = w - 20, ch = h - 56;
    uint32_t sx = (cw << 16) / 154, sy = (ch << 16) / 106;
    int ox = 81 + (d->s.cell % 4) * 160, oy = 80 + (d->s.cell / 4) * 111;
    for (unsigned y = 0; y < 106; y++)
      for (unsigned x = 0; x < 154; x++)
        pixel(d, ox + x, oy + y,
              rgb + (40 + ((uint64_t)y * sy >> 16)) * stride +
                  (1 + ((uint64_t)x * sx >> 16)) * 3);
    d->s.cell++;
  } else {
    memset(d->page, 255, sizeof(d->page));
    int ox = (800 - (int)w) / 2, oy = (600 - (int)h) / 2;
    for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++)
        pixel(d, ox + x, oy + y, rgb + (size_t)y * stride + x * 3);
  }
  d->s.captures++;
}
const uint8_t *tokimeki_io_page(const TokimekiIO *d) {
  return d ? d->page : NULL;
}
void tokimeki_io_status(const TokimekiIO *d, TokimekiIOStatus *s) {
  if (s) {
    memset(s, 0, sizeof(*s));
    if (d)
      *s = d->s;
  }
}
