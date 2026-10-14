/* SPDX-License-Identifier: BSD-3-Clause. MAME konamigv.cpp, R. Belmont/smf.
 * deb2f54fe7300e7a192690318beb860159bfd52a. See konamigv_LICENSE.txt. */
#ifndef PCSX_KDEADEYE_IO_H
#define PCSX_KDEADEYE_IO_H
#include <stddef.h>
#include <stdint.h>
#define KDEADEYE_FLASH_BYTES 0x80000u
#define KDEADEYE_FLASH_SHA1 "67e5f76eb2b938942bd67dbb30508d4603e35f15"
typedef struct KDeadEyeIO KDeadEyeIO;
typedef struct KDeadEyeIOStatus {
  uint64_t flash_reads, flash_writes, gun_reads, button_reads, noop_writes;
  uint16_t gun[2][2], buttons;
  uint32_t fault;
} KDeadEyeIOStatus;
KDeadEyeIO *kdeadeye_io_create(uint8_t *flash, size_t size);
void kdeadeye_io_destroy(KDeadEyeIO *d);
int kdeadeye_io_advance_to(KDeadEyeIO *d, uint64_t ns);
/* Raw source port values, including offscreen coordinates; no hit simulation. */
int kdeadeye_io_set_axis(KDeadEyeIO *d, unsigned player, unsigned axis, unsigned value);
int kdeadeye_io_trigger(KDeadEyeIO *d, unsigned player, int pressed);
int kdeadeye_io_read(KDeadEyeIO *d, uint32_t address, unsigned width, uint32_t *value);
int kdeadeye_io_write(KDeadEyeIO *d, uint32_t address, unsigned width, uint32_t value, uint32_t mask);
void kdeadeye_io_status(const KDeadEyeIO *d, KDeadEyeIOStatus *status);
#endif
