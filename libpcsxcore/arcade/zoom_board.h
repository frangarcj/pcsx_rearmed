/* Standalone Taito Zoom / G-Net sound board adapter. */
#ifndef PCSX_ARCADE_ZOOM_BOARD_H
#define PCSX_ARCADE_ZOOM_BOARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zoom_board zoom_board;

#define ZOOM_BOOT_FLASH_BYTES 0x80000u
#define ZOOM_SAMPLE_FLASH_BYTES 0x200000u
#define ZOOM_SHARED_BYTES 0x100u
#define ZOOM_SOUND_RAM_BYTES 0x20000u
#define ZOOM_DSP_DATA_BYTES 0x40000u

enum zoom_board_result {
    ZOOM_BOARD_OK = 0,
    ZOOM_BOARD_ARGUMENT = -1,
    ZOOM_BOARD_NOMEM = -2,
    ZOOM_BOARD_TIME = -3,
    ZOOM_BOARD_DEVICE = -4,
    ZOOM_BOARD_UNSUPPORTED = -5
};

enum zoom_board_fault {
    ZOOM_FAULT_NONE = 0,
    ZOOM_FAULT_FLASH_WRITE_UNSUPPORTED = 1,
    ZOOM_FAULT_FLASH_CALLBACK = 2,
    ZOOM_FAULT_TIME = 3,
    ZOOM_FAULT_DEVICE = 4,
    ZOOM_FAULT_BAD_BUS_ACCESS = 5,
    ZOOM_FAULT_MN_CPU = 6,
    ZOOM_FAULT_TMS_UNSUPPORTED = 7,
    ZOOM_FAULT_TMS_CALLBACK = 8,
    ZOOM_FAULT_ZSG_DIAGNOSTIC = 9
};

/* `chip` 0 is the 512 KiB E28F400B sound program flash; 1..3 are the three
 * 2 MiB TE28F160 sample flashes. Address is a 16-bit word index. For writes,
 * mem_mask uses MAME semantics: one bits are the lanes being written. The
 * adapter does not implement Intel flash command/status behavior itself. */
typedef int (*zoom_flash_read16_fn)(void *opaque, unsigned chip,
                                    uint32_t word_address, uint16_t *value);
typedef int (*zoom_flash_write16_fn)(void *opaque, unsigned chip,
                                     uint32_t word_address, uint16_t data,
                                     uint16_t mem_mask);
typedef void (*zoom_audio_frame_fn)(void *opaque, float left, float right,
                                    uint64_t zsg2_cycle);
typedef int (*zoom_time_sync_fn)(void *opaque, uint64_t internal_cycle);

typedef struct zoom_board_config {
    /* Borrowed raw, little-endian flash array images. Each image must have the
     * exact source geometry. They are read-only unless flash_write16 exists. */
    const uint8_t *boot_flash;
    size_t boot_flash_size;
    const uint8_t *sample_flash[3];
    size_t sample_flash_size[3];
    void *flash_opaque;
    zoom_flash_read16_fn flash_read16;
    zoom_flash_write16_fn flash_write16;
    void *audio_opaque;
    zoom_audio_frame_fn audio_frame;
    /* Called at the exact MN10200-domain timestamp before the corresponding
     * ZSG-2/TMS device advancement and any sample callbacks. */
    /* ROM-backed FX1Z: packed source ZSG2 region, exactly 4 or 6 MiB.
     * When present the flash sample callbacks/arrays are not used. */
    const uint8_t *sample_rom;
    size_t sample_rom_size;
    void *time_opaque;
    zoom_time_sync_fn time_sync;
} zoom_board_config;

int zoom_board_create(const zoom_board_config *config, zoom_board **out);
void zoom_board_destroy(zoom_board *board); /* not from a device callback */
/* True while run/synchronization is inside callbacks; a containing device can
 * use this to refuse destruction without freeing callback-owned state. */
int zoom_board_is_busy(const zoom_board *board);

/* The main-board sound reset input is active high. Flash boards start held in
 * reset, matching taitogn_state::driver_reset; ROM-backed FX1Z starts running.
 * Deasserting the input applies the
 * source Zoom device reset and starts the MN10200 at its reset PC. */
int zoom_board_set_sound_reset(zoom_board *board, int asserted);
/* Applies taito_zoom_device::device_reset at the current synchronized time:
 * resets ZSG2/TMS and main register address; shared/RAM/flash and absolute
 * clocks remain. */
int zoom_board_reset_devices(zoom_board *board);

/* Sound CPU clock is MN10200 internal cycles at 6.25 MHz. Consumed cycles may
 * exceed budget by one instruction, as in the underlying MAME CPU core. */
int64_t zoom_board_run(zoom_board *board, uint64_t internal_cycles);
/* Advances the sound CPU (unless held in reset) and all sound devices to at
 * least this absolute MN10200 internal-cycle boundary. Instruction overrun is
 * retained, so later calls at an earlier time never rewind the devices. */
int zoom_board_sync_to_cycle(zoom_board *board, uint64_t internal_cycle);

/* Main PSX bus 32-bit accessors for the source G-Net Zoom window. mem_mask
 * ones mark active/write lanes, as in MAME COMBINE_DATA. Unmapped reads return
 * 0xffffffff. Only the source shared-RAM lanes (0x00ff00ff) are connected. */
uint32_t zoom_board_main_read32(zoom_board *board, uint32_t address,
                                uint32_t mem_mask);
int zoom_board_main_write32(zoom_board *board, uint32_t address,
                            uint32_t data, uint32_t mem_mask);
uint16_t zoom_board_main_read16(zoom_board *board, uint32_t address,
                                uint16_t mem_mask);
int zoom_board_main_write16(zoom_board *board, uint32_t address,
                            uint16_t data, uint16_t mem_mask);

/* Diagnostic/source-state inspection. CPU getters do not advance time. */
uint32_t zoom_board_fault(const zoom_board *board);
void zoom_board_clear_fault(zoom_board *board);
uint64_t zoom_board_main_cycles(const zoom_board *board);
uint64_t zoom_board_audio_frames(const zoom_board *board);
void zoom_board_last_audio(const zoom_board *board, float stereo[2]);
uint8_t zoom_board_shared_read(const zoom_board *board, unsigned offset);
uint8_t zoom_board_sound_ram_read(const zoom_board *board, unsigned offset);
uint8_t zoom_board_dsp_ram_read(const zoom_board *board, unsigned offset);
uint32_t zoom_board_sound_pc(const zoom_board *board);
uint32_t zoom_board_sound_d(const zoom_board *board, unsigned index);
uint8_t zoom_board_tms_control(const zoom_board *board);
uint32_t zoom_board_mn_fault(const zoom_board *board);
uint16_t zoom_board_zsg_register(const zoom_board *board, unsigned channel,
                                 unsigned register_index);
uint32_t zoom_board_tms_program_word(const zoom_board *board, uint8_t address);
uint8_t zoom_board_tms_pc(const zoom_board *board);

#ifdef __cplusplus
}
#endif
#endif
