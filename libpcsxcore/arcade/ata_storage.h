/* Standalone ATA HLE mass storage port, based on MAME atahle/atastorage.
 * Source revision: deb2f54fe7300e7a192690318beb860159bfd52a.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef PCSX_ARCADE_ATA_STORAGE_H
#define PCSX_ARCADE_ATA_STORAGE_H

#include "gnet_pccard.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ata_storage ata_storage_t;
typedef int (*ata_storage_read_sector_fn)(void *opaque, uint32_t lba,
                                          void *buffer);
typedef int (*ata_storage_write_sector_fn)(void *opaque, uint32_t lba,
                                           const void *buffer);
typedef void (*ata_storage_line_fn)(void *opaque, int state);

typedef struct ata_storage_config {
  uint32_t sector_count;
  uint16_t cylinders;
  uint8_t heads;
  uint8_t sectors_per_track;
  uint64_t dma_transfer_time_ns;
  const char *serial;
  const char *firmware;
  const char *model;
  /* Optional source-provided ATA IDENTIFY payload, exactly 512 bytes. When
   * present it takes precedence over synthesized CompactFlash metadata. */
  const uint8_t *identify_data;
  size_t identify_size;
  const uint8_t *user_password;   /* 34 bytes; borrowed for lifetime */
  size_t user_password_size;
  const uint8_t *master_password; /* 34 bytes; borrowed for lifetime */
  size_t master_password_size;
  /* Sector callbacks use MAME's convention: nonzero means success. Each
   * callback receives/produces exactly 512 bytes. Media, line, and password
   * pointers are borrowed for the ATA object's lifetime. Identify bytes and
   * strings are copied/decoded during create. */
  void *media_opaque;
  ata_storage_read_sector_fn read_sector;
  ata_storage_write_sector_fn write_sector;
  void *line_opaque;
  ata_storage_line_fn irq;
  ata_storage_line_fn dmarq;
  ata_storage_line_fn dasp;
  ata_storage_line_fn pdiag;
  /* Zero keeps the historic CompactFlash implementation. Set this to IDE_HDD
   * for the pinned MAME hard-disk device behavior and IDENTIFY profile. */
  int device_kind;
} ata_storage_config_t;

enum {
  ATA_STORAGE_DEVICE_COMPACTFLASH = 0,
  ATA_STORAGE_DEVICE_IDE_HDD = 1
};

enum {
  ATA_STORAGE_OK = 0,
  ATA_STORAGE_ARGUMENT = -1,
  ATA_STORAGE_NOMEM = -2,
  ATA_STORAGE_BUSY = -3
};

int ata_storage_create(const ata_storage_config_t *config, ata_storage_t **out);
/* Destruction is refused while a caller callback or API operation is active.
 * Callbacks may inspect the ATA or try destruction, but must not write
 * registers/reset during a command; nested run_until returns BUSY. */
int ata_storage_try_destroy(ata_storage_t *ata);
void ata_storage_destroy(ata_storage_t *ata);
void ata_storage_reset(ata_storage_t *ata);
/* Time is caller-owned monotonic nanoseconds, must not move backward, and must
 * fit in INT64_MAX. Due busy and buffer-empty timers run in timestamp order;
 * equal deadlines use allocation order. Timer deadline addition saturates at
 * INT64_MAX rather than wrapping. */
int ata_storage_run_until(ata_storage_t *ata, uint64_t monotonic_ns);
uint64_t ata_storage_now(const ata_storage_t *ata);
/* Read-only scheduler deadline; UINT64_MAX means no active source timer.
 * Owners can split CPU execution at this deadline without polling guesses. */
uint64_t ata_storage_next_event_ns(const ata_storage_t *ata);

/* Native IDE buses preserve SET FEATURES-selected transfer width. PC Card
 * adapters pass 0/1 to override it according to their byte-lane bridge. */
enum { ATA_STORAGE_SOURCE_TRANSFER_MODE = -1 };
uint16_t ata_storage_command_r(ata_storage_t *ata, uint32_t offset,
                               uint16_t mem_mask, int eight_bit_data_transfers);
void ata_storage_command_w(ata_storage_t *ata, uint32_t offset, uint16_t data,
                           uint16_t mem_mask, int eight_bit_data_transfers);
uint16_t ata_storage_control_r(ata_storage_t *ata, uint32_t offset,
                               uint16_t mem_mask);
void ata_storage_control_w(ata_storage_t *ata, uint32_t offset, uint16_t data,
                           uint16_t mem_mask);
uint16_t ata_storage_dma_r(ata_storage_t *ata);
void ata_storage_dma_w(ata_storage_t *ata, uint16_t data);
void ata_storage_set_csel(ata_storage_t *ata, int state);
void ata_storage_set_dmack(ata_storage_t *ata, int state);
void ata_storage_set_dasp_in(ata_storage_t *ata, int state);
void ata_storage_set_pdiag_in(ata_storage_t *ata, int state);
uint8_t ata_storage_status(const ata_storage_t *ata);
uint8_t ata_storage_error(const ata_storage_t *ata);
uint16_t ata_storage_sector_count(const ata_storage_t *ata);
const uint16_t *ata_storage_identify(const ata_storage_t *ata);

/* G-Net PC Card bridge. Construct the ATA first, pass this descriptor to
 * gnet_pccard_create(), then associate the resulting card with the ATA. */
const gnet_pccard_ata_ops_t *ata_storage_gnet_pccard_ops(ata_storage_t *ata);
int ata_storage_set_pccard(ata_storage_t *ata, gnet_pccard_t *card);

#ifdef __cplusplus
}
#endif
#endif
