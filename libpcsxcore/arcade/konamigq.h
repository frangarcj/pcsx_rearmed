/* Standalone Konami GQ common board adapter.
 * MAME source revision deb2f54fe7300e7a192690318beb860159bfd52a.
 * SPDX-License-Identifier: BSD-3-Clause; see konamigq_LICENSE.txt.
 */
#ifndef LIBPCSXCORE_ARCADE_KONAMIGQ_H
#define LIBPCSXCORE_ARCADE_KONAMIGQ_H

#include "ncr53cf96.h"
#include "nscsi_hd.h"
#include "tms57002.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct konamigq konamigq_t;

enum {
  KONAMIGQ_PSRAM_BYTES = 0x400000,
  KONAMIGQ_PCMRAM_BYTES = 0x380000,
  KONAMIGQ_SOUNDRAM_BYTES = 0x10000,
  KONAMIGQ_TMSRAM_BYTES = 0x40000,
  KONAMIGQ_SOUND_ROM_BYTES = 0x80000,
  KONAMIGQ_K054539_ROM_BYTES = 0x80000,
  KONAMIGQ_EEPROM_BYTES = 128
};

enum {
  KONAMIGQ_PORT_GUNX1 = 0,
  KONAMIGQ_PORT_GUNY1,
  KONAMIGQ_PORT_GUNX2,
  KONAMIGQ_PORT_GUNY2,
  KONAMIGQ_PORT_GUNX3,
  KONAMIGQ_PORT_GUNY3,
  KONAMIGQ_PORT_P1_P2,
  KONAMIGQ_PORT_P3_SERVICE,
  KONAMIGQ_PORT_DSW
};

typedef uint32_t (*konamigq_input_read_cb)(void *opaque, unsigned port);
typedef uint8_t (*konamigq_ram_read_cb)(void *opaque, uint32_t address);
typedef void (*konamigq_ram_write_cb)(void *opaque, uint32_t address,
                                      uint8_t value);
typedef void (*konamigq_line_cb)(void *opaque, int asserted);
typedef void (*konamigq_sound_irq_cb)(void *opaque, unsigned level,
                                      int asserted);
typedef void (*konamigq_uart_line_cb)(void *opaque, unsigned channel,
                                      int level);
typedef void (*konamigq_audio_cb)(void *opaque, unsigned chip,
                                  uint64_t sample_cycle, int32_t left,
                                  int32_t right);

typedef struct konamigq_config {
  void *opaque;
  konamigq_ram_read_cb read_psx_ram8;
  konamigq_ram_write_cb write_psx_ram8;
  konamigq_input_read_cb read_input;
  konamigq_line_cb psx_irq10;
  konamigq_sound_irq_cb sound_irq;
  konamigq_uart_line_cb uart_txd[2];
  konamigq_uart_line_cb uart_dtr[2];
  konamigq_uart_line_cb uart_rts[2];
  konamigq_uart_line_cb uart_rxready[2];
  konamigq_uart_line_cb uart_txready[2];
  konamigq_uart_line_cb uart_txempty[2];
  konamigq_uart_line_cb uart_syndet[2];
  konamigq_audio_cb k054539_audio;
  const uint8_t *sound_rom; /* exactly 512 KiB, CPU-visible byte order */
  size_t sound_rom_bytes;
  const uint8_t *k054539_rom; /* exactly 512 KiB, raw address-space order */
  size_t k054539_rom_bytes;
  const uint8_t *eeprom_image; /* optional exact 128-byte 93C46 image */
  size_t eeprom_image_bytes;
  nscsi_hd_config_t hard_disk;
} konamigq_config_t;

enum {
  KONAMIGQ_FAULT_REENTRANT = 1u << 0,
  KONAMIGQ_FAULT_TIME_REWIND = 1u << 1,
  KONAMIGQ_FAULT_TIME_OVERFLOW = 1u << 2,
  KONAMIGQ_FAULT_DMA_RANGE = 1u << 3,
  KONAMIGQ_FAULT_BUS = 1u << 4,
  KONAMIGQ_FAULT_SOUND_CPU = 1u << 5,
  KONAMIGQ_FAULT_DSP = 1u << 6,
  KONAMIGQ_FAULT_PCMRAM_RANGE = 1u << 7
};

typedef struct konamigq_status {
  uint64_t now_ns;
  uint64_t sound_cpu_cycles;
  uint32_t sound_pc, sound_a7, sound_d0;
  uint16_t sound_sr;
  uint8_t sound_stopped, sound_bus_error, dsp_pc;
  uint64_t dsp_cycles;
  uint64_t k054539_cycles;
  uint64_t dma_bytes_remaining;
  uint32_t faults;
  uint8_t dma_active;
  uint8_t dma_write_to_scsi;
  uint8_t drq;
  uint8_t sound_cpu_reset;
  uint8_t sound_ctrl;
  uint8_t sound_intck;
  ncr53cf96_status_t ncr;
  nscsi_hd_status_t hard_disk;
} konamigq_status_t;

/* Creates the common GQ board. A real mounted hard disk is required: media
 * callbacks and CHS/sector geometry are passed to the NCR target at SCSI ID 0.
 * PSX RAM is caller-owned and follows MAME's 4 MiB GQ configuration. */
konamigq_t *konamigq_create(const konamigq_config_t *config);
void konamigq_destroy(konamigq_t *board);

/* Monotonic absolute virtual time. */
int konamigq_advance_to_ns(konamigq_t *board, uint64_t now_ns);
int konamigq_reset(konamigq_t *board);

/* PSX physical bus transactions. Width is 1, 2, or 4 bytes. mem_mask uses
 * PSX bit-lane semantics. */
int konamigq_read(konamigq_t *board, uint32_t address, unsigned width,
                  uint32_t *value);
int konamigq_write(konamigq_t *board, uint32_t address, unsigned width,
                   uint32_t value, uint32_t mem_mask);

/* PSX DMA channel 5 word count; transfer remains DRQ-gated and byte-sized. */
int konamigq_dma5_read(konamigq_t *board, uint32_t ram_address,
                       int32_t word_count);
int konamigq_dma5_write(konamigq_t *board, uint32_t ram_address,
                        int32_t word_count);

/* Direct sound-CPU bus transactions are useful for diagnostics; normal board
 * execution runs the embedded MC68000 to the requested virtual time. */
int konamigq_sound_read16(konamigq_t *board, uint32_t address,
                          uint16_t mem_mask, uint16_t *value);
int konamigq_sound_write16(konamigq_t *board, uint32_t address, uint16_t data,
                           uint16_t mem_mask);
int konamigq_uart_rxd(konamigq_t *board, unsigned channel, int level,
                      uint64_t now_ns);
int konamigq_uart_cts(konamigq_t *board, unsigned channel, int level,
                      uint64_t now_ns);
int konamigq_uart_dsr(konamigq_t *board, unsigned channel, int level,
                      uint64_t now_ns);
int konamigq_uart_txc(konamigq_t *board, unsigned channel, int level,
                      uint64_t now_ns);
int konamigq_uart_rxc(konamigq_t *board, unsigned channel, int level,
                      uint64_t now_ns);

/* Host-side DASP pins/data port and 48 kHz sample interface. The board does
 * not invent inputs to the unmodeled 058141 analog module. */
uint8_t konamigq_tms_data_read(konamigq_t *board);
int konamigq_tms_data_write(konamigq_t *board, uint8_t value);
int konamigq_tms_process_sample(konamigq_t *board, const float input[4],
                                float output[4]);

int konamigq_eeprom_export(const konamigq_t *board, uint8_t *image,
                           size_t image_bytes);
int konamigq_eeprom_import(konamigq_t *board, const uint8_t *image,
                           size_t image_bytes);
void konamigq_get_status(const konamigq_t *board, konamigq_status_t *status);

#ifdef __cplusplus
}
#endif
#endif
