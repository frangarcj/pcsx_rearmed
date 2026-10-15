// SPDX-License-Identifier: BSD-3-Clause
#include "../libpcsxcore/arcade/mas3507d.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

struct Rig {
    Mas3507D *chip;
    bool demand = false, reenter = false;
    unsigned sync = 0, requests = 0;
    Rig() : chip(mas3507d_create(frame_sync, data_demand, this)) { assert(chip); }
    ~Rig() { mas3507d_destroy(chip); }
    static void frame_sync(void *opaque, int state) {
        auto &r = *static_cast<Rig *>(opaque);
        if (state) r.sync++;
    }
    static void data_demand(void *opaque, int state) {
        auto &r = *static_cast<Rig *>(opaque);
        r.demand = state != 0;
        r.requests++;
        if (r.reenter) {
            assert(mas3507d_write_byte(r.chip, 0xaa) == -1);
            mas3507d_destroy(r.chip); // Also rejected while callback is active.
        }
    }
    Mas3507DStatus status() {
        Mas3507DStatus s{}; mas3507d_get_status(chip, &s); return s;
    }
    void pins(int scl, int sda) { assert(mas3507d_write_i2c(chip, scl, sda) == 0); }
    void start() { pins(0,1); pins(1,1); pins(1,0); pins(0,0); }
    void stop() { pins(0,0); pins(1,0); pins(1,1); }
    void send(uint8_t value) {
        for (int bit = 7; bit >= 0; bit--) { pins(0,(value >> bit) & 1); pins(1,(value >> bit) & 1); }
        pins(0,1); pins(1,1); pins(0,1);
    }
    unsigned read() {
        unsigned value = 0;
        for (unsigned bit = 0; bit < 8; bit++) {
            pins(0,1); pins(1,1); value = (value << 1) | (mas3507d_read_i2c(chip) & 1);
        }
        pins(0,0); pins(1,0); pins(0,0);
        return value;
    }
    void reg(unsigned address, unsigned value, unsigned device = 0x3a) {
        start(); send(uint8_t(device)); send(0x68); send(uint8_t(0x90 | (address >> 4)));
        send(uint8_t((address << 4) | (value & 15))); send(uint8_t(value >> 12)); send(uint8_t(value >> 4)); stop();
    }
    void gain(unsigned channel, unsigned value) {
        start(); send(0x3a); send(0x68); send(0xb0); send(0); send(0); send(1);
        send(7); send(uint8_t(channel ? 0xfb : 0xf8));
        send(uint8_t(value >> 8)); send(uint8_t(value)); send(0); send(uint8_t(value >> 16)); stop();
    }
    unsigned frame_count() {
        start(); send(0x3a); send(0x69); start(); send(0x3b);
        const unsigned high = read(), low = read(); stop();
        return (high << 8) | low;
    }
    std::vector<float> play(const std::vector<uint8_t> &bytes) {
        mas3507d_reset_playback(chip); demand = false; sync = requests = 0;
        float initial[2]; assert(mas3507d_render(chip, initial, 1) == 0);
        assert(initial[0] == 0 && initial[1] == 0 && demand);
        size_t consumed = 0;
        std::vector<float> result(88200 * 2);
        for (unsigned i = 0; i < 88200; i += 56) {
            while (demand && consumed < bytes.size())
                assert(mas3507d_write_byte(chip, bytes[consumed++]) == 0);
            const auto amount = std::min(56u, 88200 - i);
            assert(mas3507d_render(chip, result.data() + i * 2, amount) == 0);
            assert(status().buffered_bytes <= 0xe00);
        }
        assert(consumed == bytes.size());
        assert(!status().faults);
        return result;
    }
};

static bool zero(const std::vector<float> &pcm) {
    return std::all_of(pcm.begin(), pcm.end(), [](float v) { return v == 0; });
}
static void pins_and_invalid_data() {
    Rig r;
    assert(mas3507d_read_i2c(r.chip) == 3);
    assert(r.status().sample_rate == 44100 && r.status().channels == 2);
    r.reg(0xaa, 1, 0x52); assert(!r.status().muted); // Wrong device cannot change registers.
    r.reg(0xaa, 1); assert(r.status().muted);
    r.reg(0xaa, 0); assert(!r.status().muted);
    r.gain(0, 0); assert(r.status().gain_left == 0 && r.status().gain_right == 1);
    r.gain(1, 0xc0000); assert(r.status().gain_right == 1);
    mas3507d_reset_playback(r.chip); assert(r.status().gain_left == 0);
    mas3507d_reset(r.chip); assert(r.status().gain_left == 1 && !r.status().muted);
    const std::vector<uint8_t> junk(0xe00 + 1000, 0);
    for (auto value : junk) assert(mas3507d_write_byte(r.chip, value) == 0);
    assert(r.status().buffered_bytes == 0xe00);
    assert(r.frame_count() == 0);
    std::vector<float> silent(2048 * 2, 1);
    assert(mas3507d_render(r.chip, silent.data(), 2048) == 0);
    assert(zero(silent) && r.sync == 0 && r.status().decoded_frames == 0 && r.demand);
    assert(mas3507d_write_byte(nullptr, 0) == -1);
    assert(mas3507d_render(r.chip, nullptr, 1) == -1);
    assert(mas3507d_render(r.chip, silent.data(), 32769) == -1);
    assert(!r.status().faults);
    mas3507d_reset(r.chip); r.reenter = true;
    assert(mas3507d_render(r.chip, silent.data(), 1) == 0);
    assert(r.status().faults == 1 && r.status().buffered_bytes == 0);
}
static void decode_fixture(const char *filename) {
    FILE *f = std::fopen(filename, "rb"); assert(f);
    std::vector<uint8_t> bytes; int value;
    while ((value = std::fgetc(f)) != EOF) bytes.push_back(uint8_t(value));
    std::fclose(f); assert(!bytes.empty() && bytes.size() < 1000000);
    Rig r;
    const auto first = r.play(bytes);
    const auto s = r.status();
    assert(!zero(first) && s.decoded_frames > 3 && r.sync == s.decoded_frames);
    assert(r.frame_count() == (s.decoded_frames & 65535));
    assert(s.sample_rate >= 8000 && s.sample_rate <= 48000 && (s.channels == 1 || s.channels == 2 || s.channels == 0));
    for (auto v : first) assert(std::isfinite(v) && std::abs(v) <= 1.0f);
    assert(r.play(bytes) == first); // Reset includes bit reservoir/overlap/filter state.
    r.reg(0xaa, 1);
    assert(zero(r.play(bytes)) && r.status().decoded_frames == s.decoded_frames);
    r.reg(0xaa, 0); r.gain(0, 0);
    const auto left_muted = r.play(bytes);
    for (size_t i = 0; i < first.size(); i += 2) assert(left_muted[i] == 0 && left_muted[i + 1] == first[i + 1]);
    r.gain(0, 0xc0000); assert(r.play(bytes) == first);
    r.gain(1, 0xf0000);
    const auto attenuated = r.play(bytes);
    const float gain = r.status().gain_right;
    assert(gain > 0.2f && gain < 0.3f);
    for (size_t i = 0; i < first.size(); i += 2) {
        assert(attenuated[i] == first[i]);
        assert(attenuated[i + 1] == first[i + 1] * gain);
    }
    FILE *out = std::fopen((std::string(filename) + ".f32").c_str(), "wb"); assert(out);
    assert(std::fwrite(first.data(), sizeof(float), first.size(), out) == first.size()); std::fclose(out);
    std::printf("MAS3507D supplied MPEG: frames=%u rate=%u sync=%u, reset/mute/stereo gain PASS\n", s.decoded_frames, s.sample_rate, s.decoded_frames);
}
int main(int argc, char **argv) {
    pins_and_invalid_data();
    for (int i = 1; i < argc; i++) decode_fixture(argv[i]);
    std::puts("MAS3507D I2C controls, source FIFO/demand, invalid-data silence and callback reentry rejection PASS");
}
