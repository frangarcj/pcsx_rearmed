/* SPDX-License-Identifier: BSD-3-Clause. Source: MAME konamigv.cpp,
 * R. Belmont/smf, deb2f54fe7300e7a192690318beb860159bfd52a.
 * See konamigv_LICENSE.txt. */
#ifndef PCSX_ARCADE_TOKIMEKI_IO_H
#define PCSX_ARCADE_TOKIMEKI_IO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct TokimekiIO TokimekiIO;
enum { TOKIMEKI_PAGE_WIDTH = 800, TOKIMEKI_PAGE_HEIGHT = 600 };
typedef int (*tokimeki_video_cb)(void *, const uint8_t **rgb, unsigned *w,
                                 unsigned *h, unsigned *stride);
typedef struct TokimekiIOConfig {
  uint16_t device_initial;
  int manual_layout;
  void *opaque;
  tokimeki_video_cb capture;
  void (*printed)(void *, const uint8_t *, unsigned, unsigned, unsigned);
} TokimekiIOConfig;
typedef struct TokimekiIOStatus {
  uint64_t now_ns, reads, writes, sensor_ids, heartbeat_ticks, printer_packets,
      rejected_packets, captures, prints, unknown_commands;
  uint16_t device, sensor_data;
  uint8_t sensor_id, sensor_len, printing, last_command;
  uint32_t cell, fault;
} TokimekiIOStatus;
TokimekiIO *tokimeki_io_create(const TokimekiIOConfig *cfg);
void tokimeki_io_destroy(TokimekiIO *d);
void tokimeki_io_reset(TokimekiIO *d);
int tokimeki_io_advance_to(TokimekiIO *d, uint64_t ns);
void tokimeki_io_inputs(TokimekiIO *d, unsigned heartbeat, unsigned gsr,
                        unsigned printer_meta);
void tokimeki_io_pulse(TokimekiIO *d, int state);
void tokimeki_io_device_line(TokimekiIO *d, int state);
unsigned tokimeki_io_device_bit(const TokimekiIO *d);
uint16_t tokimeki_io_serial_read(TokimekiIO *d);
void tokimeki_io_serial_write(TokimekiIO *d, uint16_t data);
void tokimeki_io_vblank(TokimekiIO *d, int state);
const uint8_t *tokimeki_io_page(const TokimekiIO *d);
void tokimeki_io_status(const TokimekiIO *d, TokimekiIOStatus *s);
#ifdef __cplusplus
}
#endif
#endif
