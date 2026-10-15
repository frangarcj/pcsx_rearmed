/*
 * Twinkle sound-board wiring derived from MAME src/mame/konami/twinkle.cpp.
 * Copyright holders: smf and R. Belmont.
 * The original MAME driver is BSD-3-Clause; see docs/twinkle-board.md.
 */
#include "twinkle_board.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace {
constexpr uint64_t RF_NUM = 1323;   // 16.9344 MHz / 16 MHz = 1323 / 1250
constexpr uint64_t RF_DEN = 1250;
constexpr uint64_t IRQ_PERIOD_A = 266666; // 16 MHz / 60 Hz, fractional intervals below
constexpr uint64_t IRQ_PERIOD_NUMERATOR = 800000; // floor(n * 16 MHz / 60)
}

TwinkleBoard::TwinkleBoard(const Config &cfg)
    : rom_(cfg.program_rom), wave_(cfg.wave_ram), rf_config_(cfg.rf5c400)
{
    if (!rom_ || cfg.program_rom_size != ROM_BYTES || !wave_ ||
        cfg.wave_ram_bytes != WAVE_RAM_BYTES) {
        error_ = ARGUMENT;
        return;
    }
    spu_dsw_ = cfg.spu_dsw;
    if (cfg.hdd) {
        auto hdd = *cfg.hdd;
        hdd.device_kind = ATA_STORAGE_DEVICE_IDE_HDD;
        hdd.line_opaque = this;hdd.irq = ata_irq;hdd.dmarq = ata_dmarq;
        hdd.dasp = nullptr;hdd.pdiag = nullptr;
        if (ata_storage_create(&hdd,&ata_) != ATA_STORAGE_OK) {
            error_ = ARGUMENT;return;
        }
    }

    rf_config_.clock_hz = RF5C400_DEFAULT_CLOCK;
    rf_config_.cycle_origin = 0;
    rf_config_.sample_rom = nullptr;
    rf_config_.sample_rom_size = 0;
    rf_config_.read_word = wave_read;
    rf_config_.write_word = wave_write;
    rf_config_.memory_opaque = this;
    const auto client_sample = cfg.rf5c400.sample;
    void *client_opaque = cfg.rf5c400.sample_opaque;
    /* Store through a private closure-free pair by wrapping at construction. */
    rf_config_.sample = audio_sample;
    rf_config_.sample_opaque = this;
    client_sample_ = client_sample;
    client_sample_opaque_ = client_opaque;
    if (rf5c400_create(&rf_config_, &rf_) != RF5C400_OK) {
        error_ = DEVICE_ERROR;
        return;
    }

    M68000Bus bus{};
    bus.opaque = this;
    bus.read_word = bus_read;
    bus.write_word = bus_write;
    bus.use_default_autovectors = true;
    try {
        cpu_ = std::make_unique<M68000>(bus);
    } catch (...) {
        rf5c400_destroy(rf_);
        rf_ = nullptr;
        error_ = DEVICE_ERROR;
        return;
    }
    /* Timed CPU IRQ1 first expires after one 60 Hz period. */
    next_irq1_cycle_ = IRQ_PERIOD_A;
    cpu_->reset();
}

TwinkleBoard::~TwinkleBoard()
{
    cpu_.reset();
    if (rf_) rf5c400_destroy(rf_);
    ata_storage_destroy(ata_);
}

int64_t TwinkleBoard::run(uint32_t cycles)
{
    if (!valid()) return error_;
    if (busy_) { callback_fault_ = true; return REENTRANT; }
    busy_ = true;
    uint32_t remaining = cycles;
    int64_t consumed_total = 0;
    auto advance_irq_deadline = [this]() -> bool {
        if (irq_tick_ == UINT64_MAX) return false;
        const uint64_t tick = irq_tick_ + 1;
        const uint64_t q = tick / 3, r = tick % 3;
        if (q > (UINT64_MAX - (r * IRQ_PERIOD_NUMERATOR) / 3) / IRQ_PERIOD_NUMERATOR)
            return false;
        irq_tick_ = tick;
        next_irq1_cycle_ = q * IRQ_PERIOD_NUMERATOR + (r * IRQ_PERIOD_NUMERATOR) / 3;
        return true;
    };
    while (remaining) {
        uint64_t now = cpu_->total_cycles();
        if (!sync_devices_to_cpu(now)) { busy_ = false;return error_ = DEVICE_ERROR; }
        if (now >= next_irq1_cycle_) {
            if (!advance_irq_deadline()) {
                busy_ = false;
                return TIME_OVERFLOW;
            }
            irq1_ = true;
            cpu_->set_irq(1, true);
            continue;
        }
        uint64_t step64 = std::min<uint64_t>(remaining, next_irq1_cycle_ - now);
        const uint64_t deadline = ata_storage_next_event_ns(ata_);
        if (deadline != UINT64_MAX) {
            /* ceil(ns * 16 MHz / 1 GHz), without overflowing a product. */
            const uint64_t at = (deadline / 125) * 2 + ((deadline % 125) * 2 + 124) / 125;
            if (at > now) step64 = std::min(step64,at-now);
        }
        const uint32_t step = uint32_t(step64);
        const int64_t consumed = cpu_->run(step);
        if (consumed < 0) {
            busy_ = false;
            return consumed;
        }
        if (consumed > INT64_MAX - consumed_total) {
            busy_ = false;
            return TIME_OVERFLOW;
        }
        consumed_total += consumed;
        remaining = uint64_t(consumed) >= remaining ? 0 : remaining - uint32_t(consumed);
        if (!consumed && step) break;
    }
    uint64_t now = cpu_->total_cycles();
    while (now >= next_irq1_cycle_) {
        if (!advance_irq_deadline()) {
            busy_ = false;
            return TIME_OVERFLOW;
        }
        irq1_ = true;
        cpu_->set_irq(1, true);
    }
    if (!sync_devices_to_cpu(now)) {
        busy_ = false;
        return TIME_OVERFLOW;
    }
    busy_ = false;
    return consumed_total;
}

int TwinkleBoard::reset()
{
    if (!valid()) return error_;
    if (busy_) { callback_fault_ = true; return REENTRANT; }
    busy_ = true;
    /* MAME machine_reset only resets its SCSI-DMA state. The sound CPU reset
       does not re-anchor the periodic timer or clear board registers/RAM. */
    cpu_->reset();
    busy_ = false;
    return OK;
}

int TwinkleBoard::read16(uint32_t address, uint16_t mask, uint16_t *value)
{
    if (!valid() || !value) return ARGUMENT;
    if (busy_) { callback_fault_ = true; return REENTRANT; }
    busy_ = true;
    const bool ok = read_internal(address, mask, value);
    busy_ = false;
    if (ok) return OK;
    if (error_ != OK) return error_;
    return address >= 0x300000 && address <= 0x30000f ? UNSUPPORTED : UNMAPPED;
}

int TwinkleBoard::write16(uint32_t address, uint16_t data, uint16_t mask)
{
    if (!valid()) return error_;
    if (busy_) { callback_fault_ = true; return REENTRANT; }
    busy_ = true;
    const bool ok = write_internal(address, data, mask);
    busy_ = false;
    if (!ok && error_ != OK) return error_;
    return ok ? OK : (address >= 0x300000 && address <= 0x30000f ? UNSUPPORTED : UNMAPPED);
}

uint64_t TwinkleBoard::cpu_cycles() const { return cpu_ ? cpu_->total_cycles() : 0; }
uint64_t TwinkleBoard::rf_cycles() const
{
    RF5C400Status status{};
    rf5c400_get_status(rf_, &status);
    return status.cycles;
}
uint32_t TwinkleBoard::cpu_pc() const { return cpu_ ? cpu_->pc() : 0; }
uint16_t TwinkleBoard::cpu_sr() const { return cpu_ ? cpu_->sr() : 0; }
bool TwinkleBoard::callback_fault() const { return callback_fault_ || (cpu_ && cpu_->callback_fault()); }
bool TwinkleBoard::cpu_bus_error() const { return cpu_ && cpu_->bus_error(); }

int TwinkleBoard::main_dpram_read(uint16_t offset, uint8_t *value)
{
    if (!valid() || !value || offset >= DPRAM_BYTES) return ARGUMENT;
    if (busy_) { callback_fault_ = true; return REENTRANT; }
    busy_ = true;
    *value = dpram_[offset];
    /* CY7C131 right-side read of 0x3ff acknowledges left-side IRQ; that
       callback is not wired on this board. */
    busy_ = false;
    return OK;
}

int TwinkleBoard::main_dpram_write(uint16_t offset, uint8_t value)
{
    if (!valid() || offset >= DPRAM_BYTES) return ARGUMENT;
    if (busy_) { callback_fault_ = true; return REENTRANT; }
    busy_ = true;
    dpram_[offset] = value;
    if (offset == 0x3fe) {
        irq4_ = true;
        cpu_->set_irq(4, true);
    }
    busy_ = false;
    return OK;
}

bool TwinkleBoard::bus_read(void *opaque, M68kSpace, uint32_t address,
                            uint16_t mask, uint16_t *value)
{
    auto *b=static_cast<TwinkleBoard *>(opaque);
    const bool ok=b->read_internal(address,mask,value);
    if(!ok)b->last_bus_error_address_=address;
    return ok;
}

bool TwinkleBoard::bus_write(void *opaque, M68kSpace, uint32_t address,
                             uint16_t data, uint16_t mask)
{
    auto *b=static_cast<TwinkleBoard *>(opaque);
    const bool ok=b->write_internal(address,data,mask);
    if(!ok)b->last_bus_error_address_=address;
    return ok;
}

uint16_t TwinkleBoard::wave_read(void *opaque, uint32_t address)
{
    return static_cast<TwinkleBoard *>(opaque)->wave_read_word(address);
}

void TwinkleBoard::wave_write(void *opaque, uint32_t address, uint16_t value)
{
    static_cast<TwinkleBoard *>(opaque)->wave_write_word(address, value);
}

void TwinkleBoard::audio_sample(void *opaque, const int32_t *outputs, uint64_t cycle)
{
    TwinkleBoard *b = static_cast<TwinkleBoard *>(opaque);
    if (b->client_sample_) b->client_sample_(b->client_sample_opaque_, outputs, cycle);
}

bool TwinkleBoard::sync_rf_to_cpu(uint64_t cycles)
{
    const uint64_t q = cycles / RF_DEN;
    const uint64_t r = cycles % RF_DEN;
    if (q > (UINT64_MAX - (r * RF_NUM) / RF_DEN) / RF_NUM) return false;
    const uint64_t target = q * RF_NUM + (r * RF_NUM) / RF_DEN;
    return sync_rf_to_tick(target);
}

bool TwinkleBoard::sync_rf_to_tick(uint64_t target)
{
    /* The 68K shim reports microcycle timestamps inside callbacks. They can
       be a few clocks ahead of the completed run budget; never rewind the
       sound stream when reconciling that budget afterward. */
    if (target <= rf_cycle_) return true;
    if (rf5c400_advance_to_cycle(rf_, target) != RF5C400_OK) return false;
    rf_cycle_ = target;
    return true;
}

void TwinkleBoard::ata_irq(void *opaque,int state)
{
    auto &b = *static_cast<TwinkleBoard *>(opaque);
    if (state && (b.spu_control_ & 0x0400)) b.set_irq(6,true);
}
void TwinkleBoard::ata_dmarq(void *opaque,int state)
{ static_cast<TwinkleBoard *>(opaque)->ata_dmarq_ = state != 0; }

bool TwinkleBoard::service_ata_dma()
{
    if (!ata_ || !ata_dmarq_) return true;
    ata_storage_set_dmack(ata_,1);
    while (ata_dmarq_) {
        const uint64_t word = uint64_t(wave_bank_) * 0x400000u + spu_ata_dma_;
        if (word >= WAVE_RAM_BYTES/2) {
            ata_storage_set_dmack(ata_,0);error_ = DEVICE_ERROR;return false;
        }
        /* Source increments a word index and stores native ATA words without
         * byte swapping. Bounds failure never consumes an unstoreable word. */
        wave_[word] = ata_storage_dma_r(ata_);
        spu_ata_dma_++;ata_dma_words_++;
    }
    ata_storage_set_dmack(ata_,0);return true;
}

bool TwinkleBoard::sync_devices_to_cpu(uint64_t cycle)
{
    if (ata_) {
        if (cycle > UINT64_MAX/125) return false;
        const uint64_t ns = cycle*125/2;
        uint64_t deadline;
        while ((deadline=ata_storage_next_event_ns(ata_)) <= ns) {
            const uint64_t tick=(deadline/1000000000)*RF5C400_DEFAULT_CLOCK +
                ((deadline%1000000000)*RF5C400_DEFAULT_CLOCK)/1000000000;
            if (!sync_rf_to_tick(tick) || ata_storage_run_until(ata_,deadline) || !service_ata_dma()) return false;
        }
        if (ns >= ata_storage_now(ata_) && ata_storage_run_until(ata_,ns)) return false;
        if (!service_ata_dma()) return false;
        deadline=ata_storage_next_event_ns(ata_);
        if (cpu_ && deadline != UINT64_MAX) {
            const uint64_t at=(deadline/125)*2+((deadline%125)*2+124)/125;
            if (at>cycle) cpu_->limit_run_to_cycle(at);
        }
    }
    return sync_rf_to_cpu(cycle);
}

void TwinkleBoard::set_irq(unsigned level, bool asserted)
{
    switch (level) {
    case 1: irq1_ = asserted; break;
    case 2: irq2_ = asserted; break;
    case 4: irq4_ = asserted; break;
    case 6: irq6_ = asserted; break;
    default: return;
    }
    if (cpu_) cpu_->set_irq(level, asserted);
}

void TwinkleBoard::control_write(uint16_t data)
{
    if (!(data & 0x0080) && (spu_control_ & 0x0080)) set_irq(1, false);
    else if (!(data & 0x0100) && (spu_control_ & 0x0100)) set_irq(2, false);
    else if (!(data & 0x0200) && (spu_control_ & 0x0200)) set_irq(4, false);
    else if (!(data & 0x0400) && (spu_control_ & 0x0400)) set_irq(6, false);
    spu_control_ = data;
}

void TwinkleBoard::dpram_left_read(uint16_t offset)
{
    if (offset == 0x3fe) set_irq(4, false);
}

void TwinkleBoard::dpram_left_write(uint16_t offset, uint8_t value)
{
    if (offset < DPRAM_BYTES) dpram_[offset] = value;
}

uint16_t TwinkleBoard::wave_read_word(uint32_t byte_address) const
{
    if (byte_address > RF5C400_ADDRESS_MASK || (byte_address & 1)) return 0xffff;
    return wave_[byte_address >> 1];
}

void TwinkleBoard::wave_write_word(uint32_t byte_address, uint16_t value)
{
    if (byte_address > RF5C400_ADDRESS_MASK || (byte_address & 1)) return;
    wave_[byte_address >> 1] = value;
}

bool TwinkleBoard::read_internal(uint32_t address, uint16_t mask, uint16_t *value)
{
    if (!value || !cpu_) return false;
    uint64_t cycle = 0;
    if (!cpu_->current_cycle(cycle) || !sync_devices_to_cpu(cycle)) return false;
    address &= 0x00ffffff;
    if (address & 1) return false;
    if (address <= 0x07fffe) {
        *value = uint16_t((uint16_t(rom_[address]) << 8) | rom_[address + 1]);
        return true;
    }
    if (address >= 0x100000 && address <= 0x13fffe) {
        const size_t o = address - 0x100000;
        *value = uint16_t((uint16_t(ram_[o]) << 8) | ram_[o + 1]);
        return true;
    }
    if (address >= 0x280000 && address <= 0x2807fe) {
        const uint16_t off = uint16_t((address - 0x280000) >> 1);
        *value = 0; // Source address-space default for the unmapped high lane.
        if (mask & 0x00ff) {
            dpram_left_read(off);
            *value = dpram_[off];
        }
        return true;
    }
    if (address == 0x200000) { *value = spu_dsw_;return true; }
    if (address >= 0x300000 && address <= 0x30000f) {
        if (!ata_) return false;
        *value=ata_storage_command_r(ata_,(address-0x300000)>>1,mask,ATA_STORAGE_SOURCE_TRANSFER_MODE);
        return service_ata_dma();
    }
    if (address >= 0x400000 && address <= 0x400ffe) {
        return rf5c400_read(rf_, (address - 0x400000) >> 1, mask, value) == RF5C400_OK;
    }
    if ((address >= 0x240000 && address <= 0x240002) ||
        (address >= 0x250000 && address <= 0x250002) || address == 0x260000 || address == 0x34000e) {
        *value = 0;return true; // Source nopr / unmapped read defaults to zero.
    }
    if (address >= 0x800000 && address <= 0xfffffe) {
        const uint32_t offset_words = (address - 0x800000) >> 1;
        const uint64_t word = uint64_t(wave_bank_) * 0x400000u + offset_words;
        if (word >= WAVE_RAM_BYTES / 2) return false;
        *value = wave_[word];
        return true;
    }
    /* SPU DIP switches and unimplemented board devices are open/unmapped. */
    return false;
}

bool TwinkleBoard::write_internal(uint32_t address, uint16_t data, uint16_t mask)
{
    uint64_t cycle = 0;
    if (!cpu_ || !cpu_->current_cycle(cycle) || !sync_devices_to_cpu(cycle)) return false;
    address &= 0x00ffffff;
    if (address & 1) return false;
    if (address >= 0x100000 && address <= 0x13fffe) {
        const size_t o = address - 0x100000;
        const uint16_t old = uint16_t((uint16_t(ram_[o]) << 8) | ram_[o + 1]);
        const uint16_t merged = uint16_t((old & ~mask) | (data & mask));
        ram_[o] = uint8_t(merged >> 8);
        ram_[o + 1] = uint8_t(merged);
        return true;
    }
    if (address == 0x220000) {
        led_outputs_ = uint8_t(~data); // MAME drives active-low SPU LEDs
        return true;
    }
    if (address >= 0x230000 && address <= 0x230002) {
        control_write(data);
        return true;
    }
    if (address >= 0x240000 && address <= 0x240002) {
        spu_ata_dma_ = (spu_ata_dma_ & 0xffff0000u) | data;
        return true;
    }
    if (address >= 0x250000 && address <= 0x250002) {
        spu_ata_dma_ = (spu_ata_dma_ & 0x0000ffffu) | (uint32_t(data) << 16);
        return true;
    }
    if (address == 0x260000) {
        /* MAME's handler assigns the register directly and ignores mem_mask. */
        wave_bank_ = data;
        return true;
    }
    if (address >= 0x280000 && address <= 0x2807fe) {
        const uint16_t off = uint16_t((address - 0x280000) >> 1);
        if (mask & 0x00ff) dpram_left_write(off, uint8_t(data));
        return true;
    }
    if (address >= 0x300000 && address <= 0x30000f) {
        if (!ata_) return false;
        ata_storage_command_w(ata_,(address-0x300000)>>1,data,mask,ATA_STORAGE_SOURCE_TRANSFER_MODE);
        return sync_devices_to_cpu(cycle);
    }
    if (address == 0x34000e) return true; // explicitly nopw in source
    if (address >= 0x400000 && address <= 0x400ffe) {
        return rf5c400_write(rf_, (address - 0x400000) >> 1, data, mask) == RF5C400_OK;
    }
    if (address >= 0x800000 && address <= 0xfffffe) {
        const uint32_t offset_words = (address - 0x800000) >> 1;
        const uint64_t word = uint64_t(wave_bank_) * 0x400000u + offset_words;
        if (word >= WAVE_RAM_BYTES / 2) return false;
        const uint16_t old = wave_[word];
        wave_[word] = uint16_t((old & ~mask) | (data & mask));
        return true;
    }
    return false;
}
