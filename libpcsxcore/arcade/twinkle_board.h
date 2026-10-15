/*
 * Twinkle sound-board foundation, derived from MAME's twinkle.cpp.
 * Copyright holders: smf and R. Belmont. See docs/twinkle-board.md.
 */
#ifndef PCSX_ARCADE_TWINKLE_BOARD_H
#define PCSX_ARCADE_TWINKLE_BOARD_H

#include "m68000.h"
#include "rf5c400.h"
#include "ata_storage.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

class TwinkleBoard {
public:
    static constexpr size_t ROM_BYTES = 0x80000;
    static constexpr size_t WAVE_RAM_BYTES = 0x2000000;
    static constexpr size_t AUDIO_RAM_BYTES = 0x40000;
    static constexpr size_t DPRAM_BYTES = 0x400;
    static constexpr uint32_t AUDIO_CPU_CLOCK = 16000000;

    enum Result : int {
        OK = 0,
        ARGUMENT = -1,
        UNSUPPORTED = -2,
        UNMAPPED = -3,
        REENTRANT = -4,
        TIME_OVERFLOW = -5,
        DEVICE_ERROR = -6
    };

    struct Config {
        const uint8_t *program_rom = nullptr; /* borrowed, exactly 512 KiB */
        size_t program_rom_size = 0;
        uint16_t *wave_ram = nullptr; /* borrowed, 16M host-order words */
        size_t wave_ram_bytes = 0;
        RF5C400Config rf5c400{}; /* sample callback is retained; memory hooks replaced */
        /* Optional original HDD sector/geometry/IDENTIFY backing. The board
         * owns the ATA device and replaces its line callbacks. Media pointers
         * remain borrowed; no disk/ready state is supplied when absent. */
        const ata_storage_config_t *hdd = nullptr;
        uint8_t spu_dsw = 0xff; /* source unknown active-low DIP defaults */
    };

    explicit TwinkleBoard(const Config &config);
    ~TwinkleBoard();
    TwinkleBoard(const TwinkleBoard &) = delete;
    TwinkleBoard &operator=(const TwinkleBoard &) = delete;

    bool valid() const { return error_ == OK; }
    int error() const { return error_; }
    /* Run the sound MC68000 by source CPU cycles. Any MMIO bus access
       synchronizes RF5C400 to the exact rational 16.9344/16 MHz phase first. */
    int64_t run(uint32_t cycles);
    int reset();
    int read16(uint32_t address, uint16_t mem_mask, uint16_t *value);
    int write16(uint32_t address, uint16_t data, uint16_t mem_mask);
    uint64_t cpu_cycles() const;
    uint64_t rf_cycles() const;
    uint32_t cpu_pc() const;
    uint16_t cpu_sr() const;
    bool callback_fault() const;
    bool cpu_bus_error() const;
    uint32_t last_bus_error_address() const { return last_bus_error_address_; }

    /* Main PSX dual-port mailbox side. Offsets are the physical 10-bit
       CY7C131 byte offsets. Main write 0x3fe asserts sound IRQ4; sound read
       0x3fe acknowledges it, matching MAME's CY7C131 special addresses. */
    int main_dpram_read(uint16_t offset, uint8_t *value);
    int main_dpram_write(uint16_t offset, uint8_t value);
    uint16_t control() const { return spu_control_; }
    uint16_t wave_bank() const { return wave_bank_; }
    uint32_t ata_dma_word_index() const { return spu_ata_dma_; }
    uint8_t led_outputs() const { return led_outputs_; }
    uint64_t ata_dma_words() const { return ata_dma_words_; }
    uint8_t ata_status() const { return ata_ ? ata_storage_status(ata_) : 0; }

private:
    static bool bus_read(void *, M68kSpace, uint32_t, uint16_t, uint16_t *);
    static bool bus_write(void *, M68kSpace, uint32_t, uint16_t, uint16_t);
    static uint16_t wave_read(void *, uint32_t);
    static void wave_write(void *, uint32_t, uint16_t);
    static void audio_sample(void *, const int32_t *, uint64_t);
    bool read_internal(uint32_t, uint16_t, uint16_t *);
    bool write_internal(uint32_t, uint16_t, uint16_t);
    bool sync_rf_to_cpu(uint64_t);
    bool sync_rf_to_tick(uint64_t);
    bool sync_devices_to_cpu(uint64_t);
    bool service_ata_dma();
    static void ata_irq(void *,int);
    static void ata_dmarq(void *,int);
    void set_irq(unsigned, bool);
    void control_write(uint16_t);
    void dpram_left_read(uint16_t);
    void dpram_left_write(uint16_t, uint8_t);
    uint16_t wave_read_word(uint32_t) const;
    void wave_write_word(uint32_t, uint16_t);

    const uint8_t *rom_;
    uint16_t *wave_;
    RF5C400Config rf_config_;
    void (*client_sample_)(void *, const int32_t *, uint64_t) = nullptr;
    void *client_sample_opaque_ = nullptr;
    std::array<uint8_t, AUDIO_RAM_BYTES> ram_{};
    std::array<uint8_t, DPRAM_BYTES> dpram_{};
    std::unique_ptr<M68000> cpu_;
    RF5C400 *rf_ = nullptr;
    ata_storage_t *ata_ = nullptr;
    bool ata_dmarq_ = false;
    uint64_t ata_dma_words_ = 0;
    uint8_t spu_dsw_ = 0xff;
    uint32_t last_bus_error_address_ = 0;
    uint64_t rf_cycle_ = 0;
    uint64_t next_irq1_cycle_ = 0;
    uint64_t irq_tick_ = 1;
    uint16_t spu_control_ = 0;
    uint16_t wave_bank_ = 0;
    uint32_t spu_ata_dma_ = 0;
    uint8_t led_outputs_ = 0;
    bool irq1_ = false, irq2_ = false, irq4_ = false, irq6_ = false;
    bool busy_ = false, callback_fault_ = false;
    int error_ = OK;
};

#endif
