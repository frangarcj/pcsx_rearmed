// SPDX-License-Identifier: BSD-3-Clause
// copyright-holders:smf,windyfairy
#include "system573_digital.h"
#include "konami_security.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <vector>

template<typename T> static unsigned BIT(T v, unsigned n) { return (v >> n) & 1; }
template<unsigned N, typename T, typename... Bits>
static T bitswap(T value, Bits... bits) {
    static_assert(sizeof...(bits) == N, "source bitswap width");
    T result = 0;
    for (unsigned bit : {unsigned(bits)...}) result = T((result << 1) | BIT(value, bit));
    return result;
}
// Source counters expose a 32-bit value. Define wrap rather than performing
// an undefined float-to-uint32 conversion after long emulated sessions.
static uint32_t wrapped_counter(double value) {
    return uint32_t(std::fmod(value, 4294967296.0));
}
struct DigitalTime {
    uint64_t ns = 0;
    static const DigitalTime zero, never;
    DigitalTime operator-(DigitalTime other) const { return {ns - other.ns}; }
    double as_double() const { return double(ns) / 1000000000.0; }
};
const DigitalTime DigitalTime::zero{0}, DigitalTime::never{UINT64_MAX};
struct DigitalMachine {
    uint64_t now = 0;
    DigitalTime time() const { return {now}; }
};
struct DigitalTimer {
    bool queued = false;
    void adjust(DigitalTime delay) { queued = delay.ns != UINT64_MAX; }
};
struct MasProxy {
    Mas3507D *chip = nullptr;
    unsigned *faults = nullptr;
    bool scl = true, sda = true;
    void reset_playback() { mas3507d_reset_playback(chip); }
    int i2c_scl_r() { return (mas3507d_read_i2c(chip) >> 1) & 1; }
    int i2c_sda_r() { return mas3507d_read_i2c(chip) & 1; }
    void i2c_scl_w(bool value) {
        scl = value;
        if (mas3507d_write_i2c(chip,scl,sda)) *faults |= SYSTEM573_DIGITAL_FAULT_DECODER;
    }
    void i2c_sda_w(bool value) {
        sda = value;
        if (mas3507d_write_i2c(chip,scl,sda)) *faults |= SYSTEM573_DIGITAL_FAULT_DECODER;
    }
    void sid_w(uint8_t value) {
        if (mas3507d_write_byte(chip,value)) *faults |= SYSTEM573_DIGITAL_FAULT_DECODER;
    }
};
struct DigitalSerial {
    KonamiDS2401 *chip = nullptr;
    DigitalMachine *clock = nullptr;
    unsigned *faults = nullptr;
    int read() {
        int value = konami_ds2401_read_pin(chip,clock->now/1000);
        if (value < 0) { *faults |= SYSTEM573_DIGITAL_FAULT_TIME; return 0; }
        return value;
    }
    void write(int value) {
        if (konami_ds2401_write_pin(chip,value,clock->now/1000)) *faults |= SYSTEM573_DIGITAL_FAULT_TIME;
    }
};
struct DigitalOutput {
    system573_digital_output_t sink = nullptr;
    void *opaque = nullptr;
    void operator()(unsigned offset, int level, unsigned) {
        if (sink) sink(opaque,offset,level);
    }
};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "system573_digital_mame.inc"
#pragma GCC diagnostic pop

struct System573Digital {
    std::vector<uint16_t> ram;
    DigitalMachine clock;
    DigitalTimer timer;
    MasProxy mas;
    DigitalSerial id;
    DigitalFpga fpga{};
    DigitalDio dio{};
    unsigned faults = 0;
    bool busy = false;
    explicit System573Digital() : ram(0x1000000,0) {}
    ~System573Digital() { mas3507d_destroy(mas.chip); konami_ds2401_destroy(id.chip); }
    static void frame_sync(void *opaque,int state) { static_cast<System573Digital *>(opaque)->fpga.mpeg_frame_sync(state); }
    static void demand(void *opaque,int state) { static_cast<System573Digital *>(opaque)->fpga.mas3507d_demand(state); }
    void drain() {
        unsigned events = 0;
        while (timer.queued && !faults) {
            if (++events > 0xe02) { faults |= SYSTEM573_DIGITAL_FAULT_SCHEDULER; break; }
            timer.queued = false;
            fpga.update_stream();
        }
    }
};
static bool enter(System573Digital *chip) {
    if (!chip) return false;
    if (chip->busy) { chip->faults |= SYSTEM573_DIGITAL_FAULT_REENTRY; return false; }
    chip->busy = true; return true;
}
static int leave(System573Digital *chip,int result) {
    chip->drain(); chip->busy = false;
    return chip->faults ? -1 : result;
}
extern "C" {
System573Digital *system573_digital_create(const uint8_t *id,size_t size,int solo,
    system573_digital_output_t output,void *opaque) {
    if (!id || size != 8 || (solo != 0 && solo != 1)) return nullptr;
    std::unique_ptr<System573Digital> chip;
    try { chip = std::make_unique<System573Digital>(); }
    catch (const std::bad_alloc &) { return nullptr; }
    chip->mas.chip = mas3507d_create(System573Digital::frame_sync,System573Digital::demand,chip.get());
    chip->id.chip = konami_ds2401_create(id,size);
    if (!chip->mas.chip || !chip->id.chip) return nullptr;
    chip->mas.faults = chip->id.faults = chip->fpga.faults = &chip->faults;
    chip->id.clock = chip->fpga.clock = &chip->clock;
    chip->fpga.m_stream_timer = &chip->timer;
    chip->fpga.mas3507d = &chip->mas;
    chip->fpga.is_ddrsbm_fpga = chip->dio.is_ddrsbm_fpga = solo != 0;
    chip->fpga.ram = chip->dio.ram = chip->ram.data();
    chip->dio.digital_id = &chip->id; chip->dio.k573fpga = &chip->fpga;
    chip->dio.output_cb.sink = output; chip->dio.output_cb.opaque = opaque;
    system573_digital_reset(chip.get());
    return chip.release();
}
void system573_digital_destroy(System573Digital *chip) {
    if (chip && chip->busy) { chip->faults |= SYSTEM573_DIGITAL_FAULT_REENTRY; return; }
    delete chip;
}
void system573_digital_reset(System573Digital *chip) {
    if (!enter(chip)) return;
    chip->timer.queued = false;
    chip->mas.scl = chip->mas.sda = true;
    mas3507d_reset(chip->mas.chip); konami_ds2401_reset(chip->id.chip);
    chip->dio.device_reset(); chip->fpga.device_reset();
    chip->busy = false;
}
int system573_digital_advance_to(System573Digital *chip,uint64_t ns) {
    if (!enter(chip)) return -1;
    if (ns < chip->clock.now || ns == UINT64_MAX) {
        chip->faults |= SYSTEM573_DIGITAL_FAULT_TIME; chip->busy = false; return -1;
    }
    chip->clock.now = ns;
    if (konami_ds2401_advance_to_us(chip->id.chip,ns/1000)) chip->faults |= SYSTEM573_DIGITAL_FAULT_TIME;
    return leave(chip,0);
}
int system573_digital_read16(System573Digital *chip,unsigned offset,uint16_t *data) {
    if (!data || offset >= 0x100 || (offset & 1) || !enter(chip)) return -1;
    chip->drain();
    return leave(chip,digital_read_reg(chip->dio,offset,*data) ? 1 : 0);
}
int system573_digital_write16(System573Digital *chip,unsigned offset,uint16_t data) {
    if (offset >= 0x100 || (offset & 1) || !enter(chip)) return -1;
    chip->drain();
    return leave(chip,digital_write_reg(chip->dio,offset,data) ? 1 : 0);
}
int system573_digital_render(System573Digital *chip,float *stereo,unsigned frames) {
    if (!stereo || frames > 32768 || !enter(chip)) return -1;
    chip->drain();
    if (mas3507d_render(chip->mas.chip,stereo,frames)) chip->faults |= SYSTEM573_DIGITAL_FAULT_DECODER;
    return leave(chip,0);
}
void system573_digital_get_status(const System573Digital *chip,System573DigitalStatus *s) {
    if (!chip || !s) return;
    const auto &f = chip->fpga; const auto &d = chip->dio;
    *s = {};
    s->now_ns = chip->clock.now; s->words_decrypted = f.words_decrypted; s->bytes_sent = f.bytes_sent; s->stream_hash = f.stream_hash;
    s->start = f.mp3_start_addr; s->end = f.mp3_end_addr; s->current = f.mp3_cur_addr;
    s->ram_read_address = d.ram_read_adr; s->ram_write_address = d.ram_adr; s->latched_counter = d.fpga_counter;
    s->fpga_control = f.fpga_status; s->mpeg_status = f.mpeg_status;
    s->frame_counter = f.mp3_frame_counter & 65535; s->crypto1 = f.crypto_key1; s->crypto2 = f.crypto_key2;
    s->crypto3 = f.crypto_key3; s->pending_bytes = f.mp3_remaining_bytes; s->network_id = d.network_id;
    std::memcpy(s->output,d.output_data,8); s->faults = chip->faults;
    mas3507d_get_status(chip->mas.chip,&s->decoder);
    if (s->decoder.faults) s->faults |= SYSTEM573_DIGITAL_FAULT_DECODER;
}
}
