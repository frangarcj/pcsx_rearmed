/* Standalone Konami GV common hardware adapter.
 * MAME source revision deb2f54fe7300e7a192690318beb860159bfd52a.
 * SPDX-License-Identifier: BSD-3-Clause; see konamigv_LICENSE.txt.
 */
#ifndef LIBPCSXCORE_ARCADE_KONAMIGV_H
#define LIBPCSXCORE_ARCADE_KONAMIGV_H

#include "mb89371.h"
#include "ncr53cf96.h"
#include "nscsi_cd.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct konamigv KonamiGV;

enum { KONAMIGV_RAM_BYTES = 0x200000 };
enum { KONAMIGV_PORT_P1 = 0, KONAMIGV_PORT_P2 = 1, KONAMIGV_PORT_P3_P4 = 2 };

typedef uint32_t (*konamigv_input_read_cb)(void *opaque, unsigned port);
typedef uint8_t (*konamigv_ram_read_cb)(void *opaque, uint32_t address);
typedef void (*konamigv_ram_write_cb)(void *opaque, uint32_t address,
                                      uint8_t value);
typedef void (*konamigv_line_cb)(void *opaque, unsigned channel, int level);

typedef struct konamigv_config {
  void *opaque;
  konamigv_input_read_cb read_input;
  konamigv_ram_read_cb read_ram8;
  konamigv_ram_write_cb write_ram8;
  void (*irq10)(void *opaque, int asserted);
  konamigv_line_cb uart_txd[2];
  konamigv_line_cb uart_dtr[2];
  konamigv_line_cb uart_rts[2];
  konamigv_line_cb uart_rxready[2];
  konamigv_line_cb uart_txready[2];
  konamigv_line_cb uart_txempty[2];
  konamigv_line_cb uart_syndet[2];
  nscsi_cd_media_ops_t cd_media;
  /* Optional backend clock, synchronized at the actual SCSI audio command
   * timestamp before start/pause/status/gain, never just at a host frame. */
  int (*advance_media)(void *opaque, uint64_t now_ns);
  /* Optional exact-size 93C46 image (128 bytes); NULL/0 starts erased. */
  const uint8_t *eeprom_image;
  size_t eeprom_image_bytes;
} konamigv_config_t;

enum {
  KONAMIGV_FAULT_REENTRANT = 1u << 0,
  KONAMIGV_FAULT_INVALID_CONFIG = 1u << 1,
  KONAMIGV_FAULT_TIME_REWIND = 1u << 2,
  KONAMIGV_FAULT_TIME_OVERFLOW = 1u << 3,
  KONAMIGV_FAULT_EVENT_BUDGET = 1u << 4,
  KONAMIGV_FAULT_DMA_RANGE = 1u << 5,
  KONAMIGV_FAULT_RAM_BACKEND = 1u << 6,
  KONAMIGV_FAULT_BUS = 1u << 7
};

typedef struct konamigv_status {
  uint64_t now_ns;
  uint64_t dma_bytes_remaining;
  uint32_t faults;
  uint8_t dma_active;
  uint8_t dma_write_to_scsi;
  uint8_t drq;
  uint8_t eeprom_do;
  uint8_t eeprom_busy;
  uint32_t uart_faults;
  ncr53cf96_status_t ncr;
  nscsi_cdrom_status_t cdrom;
} konamigv_status_t;

/* Creates the shared GV board: NCR 53CF96 at 16 MHz / SCSI ID 7, Toshiba
 * XM-5401 at ID 4, MB89371 at 4 MHz, and 16-bit 93C46 EEPROM. Media and RAM
 * callbacks remain caller-owned. Input values are raw active-low port words. */
KonamiGV *konamigv_create(const konamigv_config_t *config);
void konamigv_destroy(KonamiGV *board);

/* Absolute monotonic nanoseconds. Schedules the earliest NCR cycle, CD target
 * timer, and DUART BRG edge; target/bus callbacks are flushed outside NCR
 * callbacks. */
int konamigv_advance_to(KonamiGV *board, uint64_t now_ns);
/* Applies the board machine reset lifecycle at the current virtual time. */
int konamigv_reset(KonamiGV *board);

/* Physical 32-bit-address-space transactions. Width is 1, 2, or 4 bytes;
 * mem_mask has the conventional PSX bit-lane mask. Unmapped transactions
 * return 0; handled transactions return 1. Reads fill inactive/open lanes
 * with the source program map's default zero. */
int konamigv_read(KonamiGV *board, uint32_t address, unsigned width,
                  uint32_t *value);
int konamigv_write(KonamiGV *board, uint32_t address, unsigned width,
                   uint32_t value, uint32_t mem_mask);

/* DMA channel 5 host transfer: size is a PSX word count; each byte is gated
 * by NCR DRQ and accesses the configured 2 MiB host RAM callbacks. */
int konamigv_dma5_read(KonamiGV *board, uint32_t ram_address,
                       int32_t word_count);
int konamigv_dma5_write(KonamiGV *board, uint32_t ram_address,
                        int32_t word_count);

/* External UART pins are timestamped in board virtual nanoseconds. */
int konamigv_uart_rxd(KonamiGV *board, unsigned channel, int level,
                      uint64_t now_ns);
int konamigv_uart_cts(KonamiGV *board, unsigned channel, int level,
                      uint64_t now_ns);
int konamigv_uart_dsr(KonamiGV *board, unsigned channel, int level,
                      uint64_t now_ns);
int konamigv_uart_txc(KonamiGV *board, unsigned channel, int level,
                      uint64_t now_ns);
int konamigv_uart_rxc(KonamiGV *board, unsigned channel, int level,
                      uint64_t now_ns);

int konamigv_eeprom_export(const KonamiGV *board, uint8_t *image,
                           size_t image_bytes);
int konamigv_eeprom_import(KonamiGV *board, const uint8_t *image,
                           size_t image_bytes);
void konamigv_get_status(const KonamiGV *board, konamigv_status_t *status);

#ifdef __cplusplus
}
#endif
#endif
