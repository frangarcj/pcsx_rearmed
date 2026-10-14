/* SPDX-License-Identifier: BSD-3-Clause. MAME konamigv.cpp, R. Belmont/smf.
 * Source revision deb2f54fe7300e7a192690318beb860159bfd52a.
 * See konamigv_LICENSE.txt. */
#ifndef PCSX_ARCADE_SIMPBOWL_IO_H
#define PCSX_ARCADE_SIMPBOWL_IO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct SimpBowlIO SimpBowlIO;
enum {
  SIMPBOWL_FLASH_CHIPS = 4,
  SIMPBOWL_FLASH_CHIP_BYTES = 0x200000,
  SIMPBOWL_FLASH_BYTES = 0x800000
};
/* Borrow exactly four consecutive caller-owned x8 flash images. */
SimpBowlIO *simpbowl_io_create(uint8_t *images, size_t size);
void simpbowl_io_destroy(SimpBowlIO *d);
int simpbowl_io_advance_to(SimpBowlIO *d, uint64_t ns);
/* Source frame notifier, before CPU access, with actual 12-bit port counts. */
void simpbowl_io_frame(SimpBowlIO *d, uint16_t x, uint16_t y);
int simpbowl_io_read(SimpBowlIO *d, uint32_t address, unsigned width,
                     uint32_t *value);
int simpbowl_io_write(SimpBowlIO *d, uint32_t address, unsigned width,
                      uint32_t value, uint32_t mask);
typedef struct SimpBowlIOStatus {
  uint64_t reads, writes, frames, reset_reads;
  uint32_t address, fault;
  uint16_t x, y;
} SimpBowlIOStatus;
void simpbowl_io_status(const SimpBowlIO *d, SimpBowlIOStatus *s);
#ifdef __cplusplus
}
#endif
#endif
