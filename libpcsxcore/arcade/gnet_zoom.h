/* GPL-2.0-or-later. G-Net flash router composed with the standalone Zoom board. */
#ifndef PCSX_ARCADE_GNET_ZOOM_H
#define PCSX_ARCADE_GNET_ZOOM_H

#include "zoom_board.h"
#include "rf5c296.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GNET_FIRM_FLASH_BYTES 0x200000u
#define GNET_ZOOM_PROGRAM_FLASH_BYTES ZOOM_BOOT_FLASH_BYTES
#define GNET_EPROM_BYTES 0x100000u

typedef struct gnet_zoom_board gnet_zoom_board;
typedef void (*gnet_zoom_soft_reset_fn)(void *opaque);

typedef enum gnet_zoom_result {
    GNET_ZOOM_OK = 0,
    GNET_ZOOM_UNMAPPED = 1,
    GNET_ZOOM_ARGUMENT = -1,
    GNET_ZOOM_NOMEM = -2,
    GNET_ZOOM_TIME = -3,
    GNET_ZOOM_DEVICE = -4,
    GNET_ZOOM_UNSUPPORTED = -5,
    GNET_ZOOM_READ_ONLY = -6,
    GNET_ZOOM_BUSY = -7
} gnet_zoom_result_t;

typedef struct gnet_zoom_config {
    /* Borrowed mutable raw ROM/NVRAM bytes in little-endian word order. */
    uint8_t *firm_flash;       /* TE28F160, 2 MiB */
    size_t firm_flash_size;
    uint8_t *zoom_program_flash; /* E28F400B, 512 KiB; MN boot view */
    size_t zoom_program_flash_size;
    uint8_t *sample_flash[3];  /* TE28F160, 2 MiB each */
    size_t sample_flash_size[3];
    const uint8_t *eprom;      /* 1 MiB read-only region visible in bank 2 */
    size_t eprom_size;
    unsigned jp1;              /* Source JP1 strap, 0 or 1. */
    void *reset_opaque;
    /* Called when the source MB3773 watchdog expires. Schedule reset after
     * this callback returns; do not reset/reenter/destroy the board here. */
    gnet_zoom_soft_reset_fn request_soft_reset;
    void *audio_opaque;
    zoom_audio_frame_fn audio_frame;
    /* Optional PC Card callback descriptor, copied during create. Its opaque
     * state is borrowed and must outlive this board. Callbacks run synchronously
     * and must not reenter this board, synchronize it, or destroy it. When
     * absent, both source RF5C296 windows keep returning UNSUPPORTED. This is
     * only the controller/card boundary; it does not provide ATA/security. */
    const rf5c296_card_ops_t *pc_card;
} gnet_zoom_config_t;

int gnet_zoom_create(const gnet_zoom_config_t *config, gnet_zoom_board **out);
void gnet_zoom_destroy(gnet_zoom_board *board);
/* Returns BUSY without freeing anything when called from an audio/device or
 * watchdog-reset callback, or while the child board is synchronizing. Retry
 * after the callback or synchronization returns. */
int gnet_zoom_try_destroy(gnet_zoom_board *board);

/* G-Net main-board control register 1fb40000. Reset is active-high on bit 4;
 * bit 2 selects the flash bank, and JP1 supplies bank bit 1. */
uint8_t gnet_zoom_control_read(const gnet_zoom_board *board);
int gnet_zoom_control_write(gnet_zoom_board *board, uint8_t data);
unsigned gnet_zoom_selected_bank(const gnet_zoom_board *board);
/* Runtime source JP1 input; bank selection is recomputed by control_w. */
int gnet_zoom_set_jp1(gnet_zoom_board *board, unsigned value);

/* 16-bit accesses in the G-Net flashbank window 1f000000-1f7fffff.
 * Returns UNMAPPED for unconnected space and UNSUPPORTED for the PCMCIA map
 * when no optional PC Card callbacks were supplied. */
int gnet_zoom_main_read16(gnet_zoom_board *board, uint32_t address,
                          uint16_t *value);
int gnet_zoom_main_write16(gnet_zoom_board *board, uint32_t address,
                           uint16_t data, uint16_t mem_mask);

/* Source taitogn_state::maincpu_program_map G-Net register routing. Addresses
 * must be physical PSX addresses; accesses are naturally aligned and width is
 * 1, 2, or 4. Flash reads split into x16 transactions and extract byte lanes;
 * flash writes currently require 16-bit accesses. Byte-wide control registers
 * accept wider CPU transactions on their connected low byte. Call
 * gnet_zoom_sync_to_cycle before accesses when the host is running the board.
 * A mapped register used at an unsupported width returns UNSUPPORTED. */
int gnet_zoom_bus_read(gnet_zoom_board *board, uint32_t address,
                       unsigned width, uint32_t *value);
int gnet_zoom_bus_write(gnet_zoom_board *board, uint32_t address,
                        unsigned width, uint32_t value);

/* Advances Zoom and NOR devices in the MN10200 internal-cycle domain
 * (6.25 MHz). The main-bus caller must synchronize before flash accesses. */
int gnet_zoom_sync_to_cycle(gnet_zoom_board *board, uint64_t internal_cycle);
int64_t gnet_zoom_run(gnet_zoom_board *board, uint64_t internal_cycles);

uint32_t gnet_zoom_sound_pc(const gnet_zoom_board *board);
uint32_t gnet_zoom_sound_d(const gnet_zoom_board *board, unsigned index);
uint32_t gnet_zoom_sound_ram_read(const gnet_zoom_board *board, unsigned offset);
uint32_t gnet_zoom_fault(const gnet_zoom_board *board);
int gnet_zoom_flash_ready(const gnet_zoom_board *board, unsigned chip);
uint64_t gnet_zoom_watchdog_reset_requests(const gnet_zoom_board *board);

#ifdef __cplusplus
}
#endif
#endif
