// SPDX-License-Identifier: BSD-3-Clause
// copyright-holders:Olivier Galibert,windyfairy
#include "mas3507d.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
using std::log10;
using std::powf;
using std::round;
template<typename T> static unsigned BIT(T value, unsigned bit) {
    return bit < sizeof(T) * 8 ? (value >> bit) & 1 : 0;
}
struct MasPin {
    mas3507d_pin_sink_t sink = nullptr;
    void *opaque = nullptr;
    void operator()(int level) { if (sink) sink(opaque, level); }
};
struct MasStream {
    using sample_t = float;
    float *output = nullptr;
    unsigned count = 0, rate = 44100;
    int samples() const { return int(count); }
    void set_sample_rate(int value) { rate = unsigned(value); }
    void put(unsigned channel, unsigned position, float value) {
        assert(channel < 2 && position < count);
        output[position * 2 + channel] = value;
    }
};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "mas3507d_mame/mas_methods.inc"
#pragma GCC diagnostic pop

struct Mas3507D {
    MasDevice device{};
    MasStream stream;
    bool busy = false;
    unsigned faults = 0;
};
static bool enter(Mas3507D *chip) {
    if (!chip) return false;
    if (chip->busy) { chip->faults |= 1; return false; }
    chip->busy = true;
    return true;
}
extern "C" {
Mas3507D *mas3507d_create(mas3507d_pin_sink_t sync, mas3507d_pin_sink_t demand, void *opaque) {
    std::unique_ptr<Mas3507D> chip(new (std::nothrow) Mas3507D);
    if (!chip) return nullptr;
    try { chip->device.mp3dec = std::make_unique<mp3_audio>(chip->device.mp3data.data()); }
    catch (const std::bad_alloc &) { return nullptr; }
    chip->device.stream = &chip->stream;
    chip->device.cb_mpeg_frame_sync.sink = sync;
    chip->device.cb_mpeg_frame_sync.opaque = opaque;
    chip->device.cb_demand.sink = demand;
    chip->device.cb_demand.opaque = opaque;
    chip->device.device_reset();
    return chip.release();
}
void mas3507d_destroy(Mas3507D *chip) {
    if (chip && chip->busy) { chip->faults |= 1; return; }
    delete chip;
}
void mas3507d_reset(Mas3507D *chip) {
    if (!enter(chip)) return;
    chip->device.device_reset(); chip->busy = false;
}
void mas3507d_reset_playback(Mas3507D *chip) {
    if (!enter(chip)) return;
    chip->device.reset_playback(); chip->busy = false;
}
int mas3507d_write_i2c(Mas3507D *chip, int scl, int sda) {
    if (!enter(chip)) return -1;
    chip->device.i2c_scl_w(scl != 0);
    chip->device.i2c_sda_w(sda != 0);
    chip->busy = false; return 0;
}
unsigned mas3507d_read_i2c(const Mas3507D *chip) {
    if (!chip) return 3;
    // Source pin read methods do not change device state.
    auto &c = const_cast<MasDevice &>(chip->device);
    return (c.i2c_scl_r() << 1) | c.i2c_sda_r();
}
int mas3507d_write_byte(Mas3507D *chip, uint8_t byte) {
    if (!enter(chip)) return -1;
    chip->device.sid_w(byte); chip->busy = false; return 0;
}
int mas3507d_render(Mas3507D *chip, float *stereo, unsigned frames) {
    if (!stereo || frames > 32768 || !enter(chip)) return -1;
    std::fill_n(stereo, size_t(frames) * 2, 0.0f);
    chip->stream.output = stereo; chip->stream.count = frames;
    chip->device.sound_stream_update(chip->stream);
    chip->stream.output = nullptr; chip->stream.count = 0;
    chip->busy = false; return 0;
}
void mas3507d_get_status(const Mas3507D *chip, Mas3507DStatus *s) {
    if (!chip || !s) return;
    const auto &c = chip->device;
    *s = {c.mp3data_count, c.decoded_frame_count, unsigned(c.sample_count),
          chip->stream.rate, unsigned(c.frame_channels), unsigned(c.is_muted), chip->faults,
          c.gain_ll, c.gain_rr};
}
}
