/* SPDX-License-Identifier: BSD-3-Clause. MAME konamigv.cpp, R. Belmont/smf.
 * deb2f54fe7300e7a192690318beb860159bfd52a. See konamigv_LICENSE.txt. */
#ifndef PCSX_BTCHAMP_IO_H
#define PCSX_BTCHAMP_IO_H
#include <stddef.h>
#include <stdint.h>
#define BTCHAMP_FLASH_BYTES 0x80000u
typedef struct BtChampIO BtChampIO;
typedef struct BtChampIOStatus {
  uint64_t flash_reads, flash_writes, track_reads, control_writes, frames;
  uint16_t track[2][2];
  uint32_t fault;
} BtChampIOStatus;
BtChampIO *btchamp_io_create(uint8_t *flash, size_t size);
void btchamp_io_destroy(BtChampIO *d);
int btchamp_io_advance_to(BtChampIO *d, uint64_t ns);
void btchamp_io_frame(BtChampIO *d, const uint16_t track[2][2]);
int btchamp_io_read(BtChampIO *d, uint32_t a, unsigned width, uint32_t *value);
int btchamp_io_write(BtChampIO *d, uint32_t a, unsigned width, uint32_t value, uint32_t mask);
void btchamp_io_status(const BtChampIO *d, BtChampIOStatus *status);
#endif
