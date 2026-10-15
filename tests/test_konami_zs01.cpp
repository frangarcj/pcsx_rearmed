// SPDX-License-Identifier: BSD-3-Clause
#include "../libpcsxcore/arcade/konami_zs01.h"
#include <array>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

using Packet = std::array<uint8_t, 12>;
using Key = std::array<uint8_t, 8>;
using Image = std::array<uint8_t, 140>;
static const Key id = {{0x9b, 0x22, 0x76, 0x8a, 0xfe, 0x45, 0x11, 0x01}};
static const Key response_key = {{0xf2, 0x31, 0x90, 0x77, 0xa8, 0xca, 0x04, 0x6e}};

// Independent byte-oriented host codec, rather than calling device internals.
static uint8_t rol(uint8_t v, unsigned n) {
    return uint8_t((unsigned(v) << n) | (unsigned(v) >> ((8 - n) & 7)));
}
static uint8_t encode_byte(uint8_t v, const uint8_t *key, uint8_t previous) {
    v = uint8_t((v ^ previous) + key[0]);
    for (unsigned i = 1; i < 8; i++)
        v = uint8_t(rol(v, key[i] >> 5) + (key[i] & 31));
    return v;
}
static uint8_t decode_byte(uint8_t v, const uint8_t *key, uint8_t previous) {
    for (unsigned i = 7; i; i--)
        v = rol(uint8_t(v - (key[i] & 31)), (8 - (key[i] >> 5)) & 7);
    return uint8_t(v - key[0]) ^ previous;
}
static uint16_t crc(const uint8_t *p, unsigned size) {
    uint16_t value = 0xffff;
    for (unsigned i = 0; i < size; i++) {
        value ^= uint16_t(p[i]) << 8;
        for (unsigned bit = 0; bit < 8; bit++)
            value = uint16_t((value << 1) ^ ((value & 0x8000) ? 0x1021 : 0));
    }
    return uint16_t(~value);
}
static void checksum(Packet &p) {
    const auto sum = crc(p.data(), 10);
    p[10] = uint8_t(sum >> 8);
    p[11] = uint8_t(sum);
}
static Packet encode(Packet p, const uint8_t *key) {
    uint8_t previous = 0xff;
    for (int i = 11; i >= 0; i--) {
        p[i] = encode_byte(p[i], key, previous);
        previous = p[i];
    }
    return p;
}
static Packet decode(Packet p, const uint8_t *key) {
    uint8_t previous = 0xff;
    for (int i = 11; i >= 0; i--) {
        const uint8_t encrypted = p[i];
        p[i] = decode_byte(encrypted, key, previous);
        previous = encrypted;
    }
    return p;
}
static Image image() {
    Image p{};
    p[0] = 0x5a; p[1] = 0x53; p[3] = 1;
    const Key command = {{0xed, 0x68, 0x50, 0x4b, 0xc6, 0x44, 0x48, 0x3e}};
    std::copy(command.begin(), command.end(), p.begin() + 4);
    std::copy(response_key.begin(), response_key.end(), p.begin() + 12);
    p[24] = 3; // Retry register.
    for (unsigned i = 28; i < p.size(); i++) p[i] = uint8_t(i * 47 + 13);
    return p;
}
struct Host {
    KonamiZS01 *chip;
    uint8_t previous_byte = 0;
    explicit Host(const Image &p, const Key &serial = id)
        : chip(konami_zs01_create(p.data(), p.size(), serial.data(), serial.size())) {
        assert(chip);
    }
    ~Host() { konami_zs01_destroy(chip); }
    void clock(int v) { konami_zs01_scl(chip, v); }
    void data(int v) { konami_zs01_sda(chip, v); }
    int output() { return konami_zs01_read_sda(chip); }
    void start() { clock(0); data(1); clock(1); data(0); clock(0); }
    void send(uint8_t byte) {
        for (int bit = 7; bit >= 0; bit--) {
            data((byte >> bit) & 1); clock(1); clock(1); clock(0);
        }
        data(1); clock(1); assert(output() == 0); clock(0);
    }
    uint8_t read(bool ack = true) {
        uint8_t value = 0;
        data(1);
        for (unsigned bit = 0; bit < 8; bit++) {
            clock(1); value = uint8_t((value << 1) | output()); clock(0);
        }
        data(!ack); clock(1); clock(0);
        return value;
    }
    Image saved() {
        Image p{};
        assert(konami_zs01_get_image(chip, p.data(), p.size()) == 0);
        return p;
    }
    Packet command(unsigned command, unsigned address, const Key &payload,
                   const uint8_t *command_key, const uint8_t *reply_key,
                   bool bad_crc = false, const uint8_t *data_key = nullptr) {
        Packet p{};
        p[0] = uint8_t(command); p[1] = uint8_t(address);
        std::copy(payload.begin(), payload.end(), p.begin() + 2);
        checksum(p);
        if (bad_crc) p[11] ^= 1;
        if (command & 4) {
            assert(data_key);
            uint8_t previous = previous_byte;
            for (unsigned i = 2; i < 10; i++) {
                p[i] = encode_byte(p[i], data_key, previous);
                previous = p[i];
            }
        }
        start();
        for (auto v : encode(p, command_key)) send(v);
        Packet answer{};
        for (auto &v : answer) v = read();
        answer = decode(answer, reply_key);
        assert(crc(answer.data(), 10) == uint16_t(answer[10] * 256 + answer[11]));
        previous_byte = answer[1];
        return answer;
    }
};
static Key slice(const Image &p, unsigned offset) {
    Key k{};
    std::copy_n(p.begin() + offset, k.size(), k.begin());
    return k;
}
static void expect_data(const Packet &p, const Key &data) {
    assert(p[0] == 0);
    assert(std::equal(data.begin(), data.end(), p.begin() + 2));
}
static void reset_response(Host &h, const Image &p) {
    konami_zs01_reset(h.chip);
    h.previous_byte = 0;
    konami_zs01_rst(h.chip, 1);
    for (unsigned byte = 0; byte < 4; byte++) {
        unsigned value = 0;
        for (unsigned bit = 0; bit < 8; bit++) {
            h.clock(1); h.clock(0);
            value = (value << 1) | h.output();
        }
        // Pinned source releases SDA on final reset bit, even if image bit is 0.
        assert(value == (unsigned(p[byte]) | (byte == 3 ? 1 : 0)));
    }
}
static void protocols(const Image &seed, const Key &serial) {
    Host h(seed, serial);
    reset_response(h, seed);
    const auto key = seed.data() + 4;
    for (unsigned page = 0; page < 14; page++)
        expect_data(h.command(1, page, response_key, key, response_key.data()), slice(seed, 28 + page * 8));
    Key reversed = serial;
    std::reverse(reversed.begin(), reversed.end());
    for (unsigned address : {0xfc, 0xfd})
        expect_data(h.command(1, address, response_key, key, response_key.data()), reversed);
    auto expected = seed;
    expected[25] = 0; // Valid commands reset password failure counter.
    const auto config_reply = h.command(1, 0xfe, response_key, key, response_key.data());
    expect_data(config_reply, slice(expected, 20));
    assert(h.saved() == expected);
    // Unknown reads retain the already-encrypted prior buffer, not prior plaintext.
    // This source quirk also changes the subsequent data-decrypt chaining byte.
    const auto stale = encode(config_reply, response_key.data());
    const auto unknown = h.command(1, 0x80, response_key, key, response_key.data());
    assert(unknown[0] == 0 && unknown[1] == stale[1]);
    assert(std::equal(stale.begin() + 2, stale.begin() + 10, unknown.begin() + 2));
}
static void mutations() {
    auto p = image(); Host h(p);
    const auto key = p.data() + 4;
    const Key zero{};
    // Before a read command supplies a response key, writes reply with zero key.
    expect_data(h.command(0, 0, id, key, zero.data()), zero);
    expect_data(h.command(1, 0, response_key, key, response_key.data()), id);
    for (unsigned page : {0u, 13u}) {
        const Key data = {{uint8_t(page), 0x45, 0x92, 0x37, 0xcc, 0x03, 0xfa, 0x81}};
        expect_data(h.command(4, page, data, key, response_key.data(), false, p.data() + 12), zero);
        expect_data(h.command(5, page, response_key, key, response_key.data(), false, p.data() + 12), data);
    }
    // Configure retries, replace data key, and use new key on encrypted requests.
    const Key config = {{7, 6, 5, 4, 4, 0, 3, 2}};
    expect_data(h.command(0, 0xfe, config, key, response_key.data()), zero);
    expect_data(h.command(1, 0xfe, response_key, key, response_key.data()), config);
    const Key new_key = {{0x91, 0x17, 0xa3, 0xef, 0x5c, 0x08, 0x76, 0x42}};
    expect_data(h.command(0, 0xff, new_key, key, response_key.data()), zero);
    expect_data(h.command(4, 8, id, key, response_key.data(), false, new_key.data()), zero);
    expect_data(h.command(5, 8, response_key, key, response_key.data(), false, new_key.data()), id);
    // Out-of-range writes do not alter data; reset preserves complete EEPROM.
    auto saved = h.saved();
    expect_data(h.command(0, 0x80, new_key, key, response_key.data()), zero);
    assert(h.saved() == saved);
    konami_zs01_reset(h.chip); assert(h.saved() == saved);
    Host restored(saved);
    expect_data(restored.command(5, 8, response_key, key, response_key.data(), false, new_key.data()), id);
    // Explicit erase clears only data and data key; retains command key/config.
    expect_data(h.command(0, 0xfd, zero, key, zero.data()), zero);
    saved = h.saved();
    assert(std::all_of(saved.begin() + 28, saved.end(), [](uint8_t v) { return v == 0; }));
    assert(slice(saved, 12) == zero && slice(saved, 4) == slice(p, 4));
    assert(slice(saved, 20) == config);
}
static void errors_and_pins() {
    auto p = image(); Host h(p);
    const auto key = p.data() + 4;
    const Key zero{};
    expect_data(h.command(1, 3, response_key, key, response_key.data()), slice(p, 52));
    auto wrong_key = slice(p, 4); wrong_key[0] ^= 0x80;
    assert(h.command(1, 0, response_key, wrong_key.data(), response_key.data())[0] == 2);
    assert(h.saved()[25] == 1 && slice(h.saved(), 28) == slice(p, 28));
    expect_data(h.command(1, 3, response_key, key, response_key.data()), slice(p, 52));
    assert(h.saved()[25] == 0);
    for (unsigned retry = 1; retry <= 3; retry++) {
        auto reply = h.command(0, 0, id, key, response_key.data(), true);
        assert(reply[0] == 2);
        assert(h.saved()[25] == retry);
        if (retry < 3) assert(slice(h.saved(), 28) == slice(p, 28));
    }
    auto saved = h.saved();
    assert(slice(saved, 12) == zero);
    assert(std::all_of(saved.begin() + 28, saved.end(), [](uint8_t v) { return v == 0; }));
    expect_data(h.command(1, 0, response_key, key, response_key.data()), zero);
    assert(h.saved()[25] == 0);
    // Deselection freezes the protocol and reports high; RST while CS high is ignored.
    konami_zs01_reset(h.chip); konami_zs01_cs(h.chip, 1);
    konami_zs01_rst(h.chip, 1);
    for (unsigned i = 0; i < 100; i++) { h.clock(i & 1); h.data(i & 1); assert(h.output() == 1); }
    konami_zs01_cs(h.chip, 0); konami_zs01_reset(h.chip);
    Packet request{}; request[0] = 1; request[1] = 0xfd;
    std::copy(response_key.begin(), response_key.end(), request.begin() + 2); checksum(request);
    h.start();
    unsigned sent = 0;
    for (auto v : encode(request, key)) {
        h.send(v);
        if (++sent == 5) {
            konami_zs01_cs(h.chip, 1);
            for (unsigned i = 0; i < 20; i++) { h.clock(i & 1); h.data(i & 1); assert(h.output() == 1); }
            h.clock(0); h.data(1); konami_zs01_cs(h.chip, 0);
        }
    }
    auto first = h.read(false); assert(h.read() == first); // NACK repeats current byte.
    for (unsigned i = 1; i < 12; i++) h.read();
    for (unsigned i = 0; i < 80; i++) { // Repeated START during an incomplete command.
        h.start(); h.send(uint8_t(i));
    }
    reset_response(h, p);
    expect_data(h.command(1, 0xfd, response_key, key, response_key.data()), Key{{1, 0x11, 0x45, 0xfe, 0x8a, 0x76, 0x22, 0x9b}});
}
int main(int argc, char **argv) {
    Image p = image();
    assert(!konami_zs01_create(nullptr, 140, id.data(), 8));
    assert(!konami_zs01_create(p.data(), 139, id.data(), 8));
    assert(!konami_zs01_create(p.data(), 140, nullptr, 8));
    assert(!konami_zs01_create(p.data(), 140, id.data(), 7));
    assert(konami_zs01_get_image(nullptr, p.data(), 140) == -1);
    { Host h(p); assert(konami_zs01_get_image(h.chip, nullptr, 140) == -1);
      assert(konami_zs01_get_image(h.chip, p.data(), 141) == -1); }
    protocols(p, id); mutations(); errors_and_pins();
    p[3] = 0; protocols(p, id);
    if (argc == 3) {
        Key original_id{};
        FILE *f = std::fopen(argv[1], "rb"); assert(f);
        assert(std::fread(p.data(), 1, p.size(), f) == p.size()); assert(std::fgetc(f) == EOF); std::fclose(f);
        f = std::fopen(argv[2], "rb"); assert(f);
        assert(std::fread(original_id.data(), 1, 8, f) == 8); assert(std::fgetc(f) == EOF); std::fclose(f);
        protocols(p, original_id);
        std::puts("ZS01 supplied-image pin readback PASS (component evidence only)");
    } else assert(argc == 1);
    std::puts("ZS01 encrypted pin commands, EEPROM, retries, serial and reset PASS");
}
