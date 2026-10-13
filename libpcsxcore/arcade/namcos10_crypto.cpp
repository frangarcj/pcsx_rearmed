// SPDX-License-Identifier: BSD-3-Clause
// Crypto copyright Andreas Naive; profile driver copyright smf,windyfairy.
// NAND read behavior copyright Raphael Nabet.
#include "namcos10_crypto.h"
#include <cstring>
#include <new>

template <unsigned Width, typename T, typename... Bits>
static uint16_t bitswap(T data, Bits... bits) {
    const unsigned order[] = {unsigned(bits)...};
    static_assert(sizeof...(bits) == Width, "bitswap width mismatch");
    uint16_t output = 0;
    for (unsigned i = 0; i < Width; i++)
        output = (output << 1) | ((uint64_t(data) >> order[i]) & 1);
    return output;
}
static unsigned BIT(uint64_t v, unsigned b) { return (v >> b) & 1; }
static int gf2_reduce(uint64_t v) { return __builtin_parityll(v); }
struct Logic {
    uint64_t eMask[16], dMask[16];
    uint16_t xMask;
    uint16_t (*nonlinear)(uint64_t, uint64_t);
    uint64_t (*iv)(int);
};
struct TableLogic {
    uint64_t eMask[16], dMask[16];
    uint16_t xMask;
    uint16_t (*nonlinear)(uint16_t);
    uint64_t (*iv)(int);
};
struct Profile {
    const char *name;
    unsigned type;
    uint16_t known_iv_mask, data_xor;
    unsigned permutation[16];
    const Logic *logic;
    const TableLogic *table_logic;
};
struct Alias {
    const char *name;
    const Profile *profile;
};
using u64 = uint64_t;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "namcos10_crypto_profiles.inc"
#pragma GCC diagnostic pop
struct NS10Crypto {
    const Profile *profile;
    const uint8_t *table;
    size_t table_size;
    NS10CryptoState state;
};
struct NS10NAND {
    NS10NANDConfig config;
    NS10NANDState state;
    uint8_t page_buffer[528];
    unsigned program_bytes;
};
static const Profile *lookup(const char *name) {
    if (!name)
        return nullptr;
    if (!std::strncmp(name, "ns10_", 5))
        name += 5;
    for (auto p : profiles)
        if (!std::strcmp(name, p->name))
            return p;
    for (auto a : aliases)
        if (!std::strcmp(name, a.name))
            return a.profile;
    return nullptr;
}
static void info(const Profile *p, NS10ProfileInfo *out) {
    out->name = p->name;
    out->type = p->type;
    out->known_iv_mask = p->known_iv_mask;
    out->default_data_xor = p->data_xor;
}
static uint16_t type1_decrypt(NS10CryptoState &s, uint16_t cipherword) {
    uint16_t plainword =
        s.mask ^ bitswap<16>(cipherword, 9, 13, 15, 7, 14, 8, 6, 10, 11, 12, 3, 5, 0, 1, 4, 2);
    uint8_t counter = s.counter;
    uint16_t nbs = (BIT(counter, 4) << 15) ^ ((BIT(cipherword, 2) ^ BIT(cipherword, 5)) << 14) ^
                   (BIT(cipherword, 0) << 13) ^ ((BIT(cipherword, 4) | BIT(cipherword, 5)) << 12) ^
                   (BIT(counter, 0) << 11) ^ (BIT(cipherword, 6) << 10) ^
                   ((BIT(cipherword, 4) & BIT(counter, 1)) << 8) ^ (BIT(counter, 3) << 6) ^
                   ((BIT(cipherword, 3) | BIT(counter, 7)) << 5) ^
                   ((BIT(cipherword, 2) ^ BIT(counter, 3)) << 4) ^ (BIT(counter, 2) << 3) ^
                   ((BIT(cipherword, 7) & BIT(counter, 7)) << 2) ^ (BIT(counter, 5) << 1) ^
                   ((BIT(cipherword, 7) | BIT(counter, 1)));
    s.mask = nbs ^ bitswap<16>(cipherword, 6, 11, 3, 1, 13, 5, 15, 10, 2, 9, 8, 4, 0, 12, 7, 14) ^
             bitswap<16>(plainword, 9, 7, 5, 2, 14, 4, 13, 8, 0, 15, 10, 1, 3, 6, 12, 11) ^ 0xecbe;
    ++s.counter;
    return plainword;
}
extern "C" {
NS10Crypto *ns10_crypto_create(const char *name, const uint8_t *table, size_t size) {
    auto p = lookup(name);
    if (!p || (size && !table) || size > UINT64_MAX / 8)
        return nullptr;
    auto c = new (std::nothrow) NS10Crypto{p, table, size, {}};
    if (c)
        ns10_crypto_reset(c);
    return c;
}
void ns10_crypto_destroy(NS10Crypto *c) { delete c; }
size_t ns10_crypto_profile_count(void) { return sizeof(profiles) / sizeof(*profiles); }
int ns10_crypto_profile_info(size_t n, NS10ProfileInfo *out) {
    if (!out || n >= ns10_crypto_profile_count())
        return NS10_INVALID_ARGUMENT;
    info(profiles[n], out);
    return NS10_OK;
}
int ns10_crypto_lookup(const char *name, NS10ProfileInfo *out) {
    auto p = lookup(name);
    if (!p)
        return NS10_UNKNOWN_PROFILE;
    if (!out)
        return NS10_INVALID_ARGUMENT;
    info(p, out);
    return NS10_OK;
}
void ns10_crypto_reset(NS10Crypto *c) {
    if (c) {
        std::memset(&c->state, 0, sizeof(c->state));
        c->state.data_xor = c->profile->data_xor;
    }
}
int ns10_crypto_control_write(NS10Crypto *c, uint16_t value) {
    if (!c)
        return NS10_INVALID_ARGUMENT;
    if (!(value & 0x8000)) {
        c->state.active = 0;
        return NS10_OK;
    }
    unsigned iv = value & 15;
    auto p = c->profile;
    if (!p->type)
        return NS10_UNKNOWN_ENCRYPTION;
    if (!(p->known_iv_mask & (1u << iv)))
        return NS10_UNKNOWN_IV;
    if (p->type == 3 && !c->table_size)
        return NS10_MISSING_TABLE;
    static const uint8_t type1_sbox[16] = {16, 16, 16, 0, 4, 9, 16, 16, 16, 8, 16, 1, 16, 9, 16, 5};
    static const uint8_t type2_sbox[16] = {0, 12, 13, 6, 2, 4, 9, 8, 11, 1, 7, 15, 10, 5, 14, 3};
    c->state.mask = 0;
    c->state.counter = 0;
    c->state.iv = iv;
    c->state.plain_history = 0;
    if (p->type == 1)
        c->state.mask = type1_sbox[iv];
    else {
        auto fn = p->type == 3 ? p->table_logic->iv : p->logic->iv;
        c->state.cipher_history = fn ? fn(iv)
                                     : bitswap<16>(type2_sbox[iv], 3, 16, 16, 2, 1, 16, 16, 0, 16,
                                                   16, 16, 16, 16, 16, 16, 16);
    }
    // MAME table cursor resets on device reset, NOT on stream activation.
    c->state.active = 1;
    return NS10_OK;
}
int ns10_crypto_active(const NS10Crypto *c) { return c ? c->state.active : 0; }
uint16_t ns10_crypto_decrypt_word(NS10Crypto *c, uint16_t word) {
    if (!c || !c->state.active)
        return word;
    auto &s = c->state;
    auto p = c->profile;
    if (p->type == 1)
        return type1_decrypt(s, word);
    uint16_t plain = word ^ s.mask;
    s.cipher_history = (s.cipher_history << 16) | word;
    s.plain_history = (s.plain_history << 16) | plain;
    const uint64_t *e = p->type == 3 ? p->table_logic->eMask : p->logic->eMask;
    const uint64_t *d = p->type == 3 ? p->table_logic->dMask : p->logic->dMask;
    uint16_t mask = p->type == 3 ? p->table_logic->xMask : p->logic->xMask;
    for (unsigned i = 0; i < 16; i++)
        mask ^= uint16_t(gf2_reduce(e[i] & s.cipher_history) ^ gf2_reduce(d[i] & s.plain_history))
                << i;
    if (p->type == 3) {
        unsigned bit = (c->table[s.nonlinear_bit / 8] >> (7 - (s.nonlinear_bit % 8))) & 1;
        s.nonlinear_bit = (s.nonlinear_bit + 1) % (uint64_t(c->table_size) * 8);
        mask ^= p->table_logic->nonlinear(bit);
    } else
        mask ^= p->logic->nonlinear(s.cipher_history, s.plain_history);
    s.mask = mask;
    return plain;
}
uint16_t ns10_crypto_unscramble(const NS10Crypto *c, uint16_t word) {
    if (!c)
        return word;
    word ^= c->state.data_xor;
    uint16_t out = 0;
    for (unsigned i = 0; i < 16; i++)
        out = (out << 1) | ((word >> c->profile->permutation[i]) & 1);
    return out;
}
uint16_t ns10_crypto_memm_bank_word(NS10Crypto *c, uint16_t word, unsigned bank, int direct) {
    if (direct)
        return word;
    if (ns10_crypto_active(c))
        return ns10_crypto_decrypt_word(c, word);
    return bank < 0x10 ? ns10_crypto_unscramble(c, word) : word;
}
void ns10_crypto_set_data_xor(NS10Crypto *c, uint16_t value) {
    if (c)
        c->state.data_xor = value;
}
void ns10_crypto_dataxor_write(NS10Crypto *c, uint16_t value) {
    ns10_crypto_set_data_xor(c, (value << 8) | (value >> 8));
}
void ns10_crypto_get_state(const NS10Crypto *c, NS10CryptoState *s) {
    if (c && s)
        *s = c->state;
}
int ns10_crypto_set_state(NS10Crypto *c, const NS10CryptoState *s) {
    if (!c || !s || s->active > 1 || s->iv > 15)
        return NS10_INVALID_ARGUMENT;
    if (s->active && (!c->profile->type || !(c->profile->known_iv_mask & (1u << s->iv))))
        return NS10_UNKNOWN_IV;
    if (c->profile->type == 3 &&
        ((s->active && !c->table_size) ||
         (c->table_size && s->nonlinear_bit >= uint64_t(c->table_size) * 8)))
        return NS10_MISSING_TABLE;
    c->state = *s;
    return NS10_OK;
}
NS10NAND *ns10_nand_create(const NS10NANDConfig *cfg) {
    if (!cfg || !cfg->bytes || !cfg->pages || uint64_t(cfg->pages) * 528 != cfg->size ||
        (cfg->writable_bytes && (cfg->writable_bytes != cfg->bytes || cfg->pages % 32)))
        return nullptr;
    auto n = new (std::nothrow) NS10NAND{*cfg, {}, {}, 0};
    if (n)
        ns10_nand_reset(n);
    return n;
}
void ns10_nand_destroy(NS10NAND *n) { delete n; }
void ns10_nand_reset(NS10NAND *n) {
    if (n) {
        std::memset(&n->state, 0, sizeof(n->state));
        n->state.status = 0xc0;
    }
}
int ns10_nand_command(NS10NAND *n, uint8_t cmd) {
    if (!n)
        return NS10_INVALID_ARGUMENT;
    auto &s = n->state;
    switch (cmd) {
    case 0xff:
        ns10_nand_reset(n);
        return NS10_OK;
    case 0:
    case 1:
    case 0x50:
        s.mode = 1;
        s.pointer_mode = cmd == 0x50 ? 2 : cmd;
        s.address_cycle = 0;
        return NS10_OK;
    case 0x80:
        if (!n->config.writable_bytes) break;
        s.mode = 4; s.address_cycle = 0; n->program_bytes = 0;
        std::memset(n->page_buffer, 0xff, sizeof(n->page_buffer));
        return NS10_OK;
    case 0x10:
    case 0x15:
        if (!n->config.writable_bytes) break;
        if (s.mode != 4 || s.address_cycle < 3 || s.page >= n->config.pages)
            return NS10_INVALID_ARGUMENT;
        for (unsigned i = 0; i < 528; i++)
            n->config.writable_bytes[size_t(s.page) * 528 + i] &= n->page_buffer[i];
        s.status = 0xc0; s.mode = 0;
        return NS10_OK;
    case 0x60:
        if (!n->config.writable_bytes) break;
        s.mode = 5; s.page = 0; s.address_cycle = 0;
        return NS10_OK;
    case 0xd0:
        if (!n->config.writable_bytes) break;
        if (s.mode != 5 || s.address_cycle < 2 || s.page >= n->config.pages)
            return NS10_INVALID_ARGUMENT;
        std::memset(n->config.writable_bytes + size_t(s.page & ~31u) * 528, 0xff, 32 * 528);
        s.status = 0xc0; s.mode = 0;
        if (s.pointer_mode == 1) s.pointer_mode = 0;
        return NS10_OK;
    case 0x70:
        s.mode = 2;
        return NS10_OK;
    case 0x90:
        s.mode = 3;
        s.address_cycle = 0;
        return NS10_OK;
    default: break;
    }
    s.mode = 0;
    s.unsupported_commands++;
    return NS10_UNSUPPORTED_COMMAND;
}
void ns10_nand_address(NS10NAND *n, uint8_t v) {
    if (!n)
        return;
    auto &s = n->state;
    if (s.mode == 3) {
        if (!s.address_cycle)
            s.column = v;
        s.address_cycle++;
        return;
    }
    if (s.mode == 5) {
        if (s.address_cycle < 2) {
            unsigned shift = s.address_cycle * 8;
            s.page = (s.page & ~(0xffu << shift)) | (uint32_t(v) << shift);
        }
        if (s.address_cycle < 255) s.address_cycle++;
        return;
    }
    if (s.mode != 1 && s.mode != 4) return;
    if (!s.address_cycle) {
        s.page = 0;
        s.column = s.pointer_mode == 2 ? 512 + (v & 15) : s.pointer_mode == 1 ? 256 + v : v;
        if (s.pointer_mode == 1)
            s.pointer_mode = 0;
    } else if (s.address_cycle < 3) {
        unsigned shift = (s.address_cycle - 1) * 8;
        s.page = (s.page & ~(0xffu << shift)) | (uint32_t(v) << shift);
    }
    if (s.address_cycle < 255)
        s.address_cycle++;
}
int ns10_nand_write_byte(NS10NAND *n, uint8_t value) {
    if (!n) return NS10_INVALID_ARGUMENT;
    if (!n->config.writable_bytes) return NS10_UNSUPPORTED_COMMAND;
    auto &s = n->state;
    if (s.mode != 4 || s.address_cycle < 3 || s.column >= 528)
        return NS10_INVALID_ARGUMENT;
    if (n->program_bytes++ < 528) n->page_buffer[s.column] = value;
    if (++s.column == 528) s.column = s.pointer_mode == 2 ? 512 : 0;
    return NS10_OK;
}
uint8_t ns10_nand_read_byte(NS10NAND *n) {
    if (!n)
        return 0xff;
    auto &s = n->state;
    if (s.mode == 2)
        return s.status & 0xc1;
    if (s.mode == 3) {
        unsigned pos = s.column++;
        return pos == 0 ? n->config.manufacturer_id : pos == 1 ? n->config.device_id : 0;
    }
    if (s.mode != 1)
        return 0;
    uint8_t result = (s.page < n->config.pages && s.column < 528)
                         ? n->config.bytes[size_t(s.page) * 528 + s.column]
                         : 0xff;
    s.column++;
    if (s.column == 528 && n->config.sequential_row_read) {
        s.column = s.pointer_mode == 2 ? 512 : 0;
        s.page = (s.page + 1) % n->config.pages;
    }
    return result;
}
uint16_t ns10_nand_read_word(NS10NAND *n) {
    uint16_t hi = ns10_nand_read_byte(n);
    return (hi << 8) | ns10_nand_read_byte(n);
}
void ns10_nand_get_state(const NS10NAND *n, NS10NANDState *s) {
    if (n && s)
        *s = n->state;
}
uint16_t ns10_crypto_nand_read_word(NS10Crypto *c, NS10NAND *n, unsigned chip, uint32_t address) {
    if (!n)
        return 0xffff;
    uint16_t word = ns10_nand_read_word(n);
    if (ns10_crypto_active(c))
        return ns10_crypto_decrypt_word(c, word);
    if (!chip && (((address >> 8) & 0xffff) >> 5) < 2)
        return word;
    return ns10_crypto_unscramble(c, word);
}
}
