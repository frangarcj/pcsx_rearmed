// SPDX-License-Identifier: BSD-3-Clause
#include "../libpcsxcore/arcade/namcos10_crypto.h"
#include "test_namcos10_crypto_vectors.inc"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
static const uint8_t table[] = {0x04, 0x9d};
static NS10Crypto *create(const char *name) {
    auto c = ns10_crypto_create(name, table, sizeof(table));
    assert(c);
    return c;
}
static NS10CryptoState state(NS10Crypto *c) {
    NS10CryptoState s{};
    ns10_crypto_get_state(c, &s);
    return s;
}
static void crypto_tests() {
    unsigned count = 0;
    for (auto &v : vectors) {
        auto c = create(v.name);
        assert(ns10_crypto_control_write(c, 0x8000 | v.iv) == NS10_OK);
        for (unsigned i = 0; i < 32; i++) {
            auto got = ns10_crypto_decrypt_word(c, cipherwords[i]);
            if (got != v.plain[i]) {
                std::fprintf(stderr, "Profile %s IV%u index%u got%04x expected%04x\n", v.name, v.iv,
                             i, got, v.plain[i]);
                assert(false);
            }
        }
        auto saved = state(c);
        uint16_t continuation[32];
        for (unsigned i = 0; i < 32; i++)
            continuation[i] = ns10_crypto_decrypt_word(c, cipherwords[i]);
        assert(ns10_crypto_set_state(c, &saved) == NS10_OK);
        for (unsigned i = 0; i < 32; i++)
            assert(ns10_crypto_decrypt_word(c, cipherwords[i]) == continuation[i]);
        ns10_crypto_reset(c);
        assert(!ns10_crypto_active(c));
        assert(ns10_crypto_control_write(c, 0x8000 | v.iv) == NS10_OK);
        for (unsigned i = 0; i < 32; i++)
            assert(ns10_crypto_decrypt_word(c, cipherwords[i]) == v.plain[i]);
        assert(ns10_crypto_control_write(c, 0) == NS10_OK);
        auto before = state(c);
        assert(ns10_crypto_decrypt_word(c, 0x9876) == 0x9876);
        auto after = state(c);
        assert(before.mask == after.mask && before.counter == after.counter &&
               before.cipher_history == after.cipher_history &&
               before.nonlinear_bit == after.nonlinear_bit);
        ns10_crypto_destroy(c);
        count++;
    }
    for (auto v : passive_vectors) {
        auto c = create(v.name);
        assert(ns10_crypto_unscramble(c, 0x1357) == v.result);
        ns10_crypto_destroy(c);
    }
    {
        auto c = create("mrdrilr2j");
        assert(ns10_crypto_control_write(c, 0x8003) == NS10_OK);
        for (unsigned i = 0; i < 320; i++)
            assert(ns10_crypto_decrypt_word(c, cipherwords[i % 32]) == type1_wrap[i]);
        assert(state(c).counter == 64);
        ns10_crypto_destroy(c);
    }
    {
        auto c = create("mrdrilr2");
        assert(ns10_crypto_control_write(c, 0x8000) == NS10_UNKNOWN_IV);
        assert(!ns10_crypto_active(c));
        ns10_crypto_destroy(c);
        c = create("squizchs");
        assert(ns10_crypto_control_write(c, 0x8000) == NS10_UNKNOWN_IV);
        assert(ns10_crypto_control_write(c, 0x8001) == NS10_OK);
        ns10_crypto_destroy(c);
        c = create("gegemdb");
        assert(ns10_crypto_control_write(c, 0x8000) == NS10_UNKNOWN_ENCRYPTION);
        ns10_crypto_destroy(c);
        c = ns10_crypto_create("gunbalina", nullptr, 0);
        assert(c && ns10_crypto_control_write(c, 0x8000) == NS10_MISSING_TABLE);
        ns10_crypto_destroy(c);
        assert(!ns10_crypto_create("imaginary_game", nullptr, 0));
    }
    {
        auto c = create("ptblank3"), fresh = create("ptblank3");
        assert(ns10_crypto_control_write(c, 0x8000) == 0);
        for (unsigned i = 0; i < 5; i++)
            ns10_crypto_decrypt_word(c, cipherwords[i]);
        assert(state(c).nonlinear_bit == 5);
        assert(ns10_crypto_control_write(c, 0x8000) == 0 && state(c).nonlinear_bit == 5);
        assert(ns10_crypto_control_write(fresh, 0x8000) == 0);
        assert(ns10_crypto_decrypt_word(c, 0x1234) == ns10_crypto_decrypt_word(fresh, 0x1234));
        assert((ns10_crypto_decrypt_word(c, 0x5678) ^ ns10_crypto_decrypt_word(fresh, 0x5678)) ==
               0x800);
        ns10_crypto_reset(c);
        assert(state(c).nonlinear_bit == 0);
        ns10_crypto_destroy(c);
        ns10_crypto_destroy(fresh);
    }
    {
        auto c = create("ippo");
        ns10_crypto_dataxor_write(c, 0x1234);
        assert(state(c).data_xor == 0x3412);
        assert(ns10_crypto_unscramble(c, 0xaaaa) == (0xaaaa ^ 0x3412));
        NS10ProfileInfo info{};
        assert(ns10_crypto_lookup("ns10_ippo", &info) == 0 && info.default_data_xor == 0x5300);
        ns10_crypto_destroy(c);
    }
    {
        auto c = create("mrdrilr2");
        assert(ns10_crypto_memm_bank_word(c, 0x1234, 0, 1) == 0x1234);
        assert(ns10_crypto_memm_bank_word(c, 0x1234, 0, 0) == ns10_crypto_unscramble(c, 0x1234));
        assert(ns10_crypto_memm_bank_word(c, 0x1234, 0x10, 0) == 0x1234);
        assert(ns10_crypto_control_write(c, 0x8003) == 0);
        assert(ns10_crypto_memm_bank_word(c, 0x1234, 0, 1) == 0x1234 && state(c).counter == 0);
        assert(ns10_crypto_memm_bank_word(c, 0x1234, 0x10, 0) != 0x1234 && state(c).counter == 1);
        ns10_crypto_destroy(c);
    }
    assert(ns10_crypto_profile_count() == 35);
    std::printf(
        "System10 crypto: %u independent fixed-vector streams, reset/snapshot/reactivation, "
        "counter/table wrapping and 35 passive profiles passed\n",
        count);
}
static void address(NS10NAND *n, uint8_t command, unsigned column, unsigned page) {
    assert(ns10_nand_command(n, command) == 0);
    ns10_nand_address(n, column);
    ns10_nand_address(n, page);
    ns10_nand_address(n, page >> 8);
}
static NS10NANDState nstate(NS10NAND *n) {
    NS10NANDState s{};
    ns10_nand_get_state(n, &s);
    return s;
}
static void nand_tests() {
    std::vector<uint8_t> bytes(528 * 4);
    for (unsigned p = 0; p < 4; p++)
        for (unsigned c = 0; c < 528; c++)
            bytes[p * 528 + c] = uint8_t(p * 31 + c);
    NS10NANDConfig cfg = {bytes.data(), bytes.size(), 4, 0xec, 0x73, 0, nullptr};
    auto n = ns10_nand_create(&cfg);
    assert(n);
    address(n, 0x00, 0x10, 2);
    assert(ns10_nand_read_word(n) == 0x4e4f);
    assert(nstate(n).column == 0x12);
    address(n, 0x01, 5, 3);
    assert(ns10_nand_read_byte(n) == uint8_t(3 * 31 + 261));
    address(n, 0x50, 0x24, 1);
    assert(nstate(n).column == 516);
    assert(ns10_nand_read_byte(n) == uint8_t(31 + 516));
    address(n, 0x50, 15, 0);
    assert(ns10_nand_read_byte(n) == 15);
    assert(ns10_nand_read_byte(n) == 0xff);
    address(n, 0, 0, 4);
    assert(ns10_nand_read_byte(n) == 0xff); // no out-of-dump memory read
    assert(ns10_nand_command(n, 0x90) == 0);
    ns10_nand_address(n, 0);
    assert(ns10_nand_read_byte(n) == 0xec && ns10_nand_read_byte(n) == 0x73 &&
           ns10_nand_read_byte(n) == 0);
    assert(ns10_nand_command(n, 0x70) == 0 && ns10_nand_read_byte(n) == 0xc0);
    assert(ns10_nand_command(n, 0x80) == NS10_UNSUPPORTED_COMMAND &&
           nstate(n).unsupported_commands == 1);
    assert(ns10_nand_command(n, 0xff) == 0 && nstate(n).unsupported_commands == 0);
    {
        auto c = create("chocovdr");
        address(n, 0, 0, 0);
        uint16_t raw = 0x0001;
        assert(ns10_crypto_nand_read_word(c, n, 0, 0) == raw); // chip0block0 plaintext
        address(n, 0, 0, 0);
        assert(ns10_crypto_nand_read_word(c, n, 0, 63 << 8) == raw); // block1 plaintext
        address(n, 0, 0, 0);
        assert(ns10_crypto_nand_read_word(c, n, 0, 64 << 8) == ns10_crypto_unscramble(c, raw));
        address(n, 0, 0, 0);
        assert(ns10_crypto_nand_read_word(c, n, 1, 0) == ns10_crypto_unscramble(c, raw));
        assert(ns10_crypto_control_write(c, 0x8000) == 0);
        address(n, 0, 0, 0);
        assert(ns10_crypto_nand_read_word(c, n, 0, 0) ==
               raw); // active stream starts mask0 even in specialblocks
        assert(state(c).cipher_history == raw);
        ns10_crypto_destroy(c);
    }
    ns10_nand_destroy(n);
    cfg.sequential_row_read = 1;
    n = ns10_nand_create(&cfg);
    assert(n);
    address(n, 0x50, 15, 3);
    assert(ns10_nand_read_byte(n) == uint8_t(3 * 31 + 527));
    assert(nstate(n).page == 0 && nstate(n).column == 512);
    assert(ns10_nand_read_byte(n) == 0);
    address(n, 1, 255, 0);
    for (unsigned i = 511; i < 528; i++)
        assert(ns10_nand_read_byte(n) == uint8_t(i));
    assert(nstate(n).page == 1 && nstate(n).column == 0);
    assert(ns10_nand_read_byte(n) == 31);
    ns10_nand_destroy(n);
    cfg.size--;
    assert(!ns10_nand_create(&cfg));
    std::puts("System10 NAND: real dump reads, address/pointer modes, OOB/page boundaries, "
              "ID/status, sequential rows and MEMN crypto pipeline passed");
}
static void nand_write_tests() {
    std::vector<uint8_t> bytes(64 * 528, 0x5a);
    NS10NANDConfig cfg = {bytes.data(), bytes.size(), 64, 0xec, 0x73, 0, bytes.data()};
    auto n = ns10_nand_create(&cfg);
    assert(n);
    assert(ns10_nand_command(n, 0x60) == NS10_OK);
    ns10_nand_address(n, 47); ns10_nand_address(n, 0);
    assert(ns10_nand_command(n, 0xd0) == NS10_OK);
    for (unsigned i = 0; i < 32 * 528; i++) assert(bytes[i] == 0x5a);
    for (unsigned i = 32 * 528; i < bytes.size(); i++) assert(bytes[i] == 0xff);
    assert(ns10_nand_command(n, 0x70) == NS10_OK && ns10_nand_read_byte(n) == 0xc0);
    // A page buffer must stay invisible until confirm, and programming only
    // clears bits. Neither adjacent bytes nor OOB may be lost.
    ns10_nand_command(n, 0); ns10_nand_command(n, 0x80);
    ns10_nand_address(n, 1); ns10_nand_address(n, 40); ns10_nand_address(n, 0);
    assert(ns10_nand_write_byte(n, 0x12) == NS10_OK);
    assert(ns10_nand_write_byte(n, 0x34) == NS10_OK);
    assert(bytes[40 * 528 + 1] == 0xff);
    assert(ns10_nand_command(n, 0x10) == NS10_OK);
    assert(bytes[40 * 528] == 0xff && bytes[40 * 528 + 1] == 0x12 &&
        bytes[40 * 528 + 2] == 0x34 && bytes[40 * 528 + 3] == 0xff);
    ns10_nand_command(n, 0x80);
    ns10_nand_address(n, 1); ns10_nand_address(n, 40); ns10_nand_address(n, 0);
    ns10_nand_write_byte(n, 0xf0); ns10_nand_command(n, 0x10);
    assert(bytes[40 * 528 + 1] == 0x10);
    ns10_nand_command(n, 0x50); ns10_nand_command(n, 0x80);
    ns10_nand_address(n, 15); ns10_nand_address(n, 40); ns10_nand_address(n, 0);
    ns10_nand_write_byte(n, 0xab); ns10_nand_write_byte(n, 0xcd);
    assert(ns10_nand_command(n, 0x10) == NS10_OK);
    assert(bytes[40 * 528 + 527] == 0xab && bytes[40 * 528 + 512] == 0xcd);
    ns10_nand_reset(n);
    address(n, 0, 1, 40); assert(ns10_nand_read_word(n) == 0x1034);
    address(n, 0x50, 15, 40); assert(ns10_nand_read_byte(n) == 0xab);
    // Reset aborts pending data without undoing completed settings writes.
    ns10_nand_command(n, 0); ns10_nand_command(n, 0x80);
    ns10_nand_address(n, 1); ns10_nand_address(n, 40); ns10_nand_address(n, 0);
    ns10_nand_write_byte(n, 0); ns10_nand_reset(n);
    assert(bytes[40 * 528 + 1] == 0x10);
    assert(ns10_nand_command(n, 0x60) == NS10_OK);
    ns10_nand_address(n, 0xff); ns10_nand_address(n, 0xff);
    assert(ns10_nand_command(n, 0xd0) == NS10_INVALID_ARGUMENT);
    assert(bytes[40 * 528 + 1] == 0x10);
    ns10_nand_destroy(n);
    cfg.writable_bytes = nullptr; n = ns10_nand_create(&cfg);
    assert(ns10_nand_command(n, 0x60) == NS10_UNSUPPORTED_COMMAND);
    assert(ns10_nand_write_byte(n, 0) == NS10_UNSUPPORTED_COMMAND);
    ns10_nand_destroy(n);
    std::puts("System10 NAND: buffered program, OOB wrap, block erase, reset persistence and bounds passed");
}

int main() {
    crypto_tests();
    nand_tests();
    nand_write_tests();
}
