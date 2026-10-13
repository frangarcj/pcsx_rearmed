/* SPDX-License-Identifier: BSD-3-Clause
 * Namco System 10 crypto: derived from MAME, Andreas Naive.
 * Read-only NAND behavior: derived from MAME, Raphael Nabet.
 */
#ifndef PCSX_NAMCOS10_CRYPTO_H
#define PCSX_NAMCOS10_CRYPTO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NS10Crypto NS10Crypto;
typedef struct NS10NAND NS10NAND;
enum {
    NS10_OK = 0,
    NS10_UNKNOWN_PROFILE = -1,
    NS10_UNKNOWN_ENCRYPTION = -2,
    NS10_UNKNOWN_IV = -3,
    NS10_MISSING_TABLE = -4,
    NS10_INVALID_ARGUMENT = -5,
    NS10_UNSUPPORTED_COMMAND = -6
};
enum { NS10_PASSIVE_ONLY = 0, NS10_MEMM_TYPE1 = 1, NS10_MEMN_TYPE2 = 2, NS10_MEMN_TYPE2_TABLE = 3 };
typedef struct NS10ProfileInfo {
    const char *name;
    unsigned type;
    uint16_t known_iv_mask, default_data_xor;
} NS10ProfileInfo;
typedef struct NS10CryptoState {
    uint64_t cipher_history, plain_history, nonlinear_bit;
    uint16_t mask, data_xor;
    uint8_t counter, active, iv;
} NS10CryptoState;
/* Accepts profile or MAME game/clone name. Table bytes remain caller-owned. */
NS10Crypto *ns10_crypto_create(const char *profile, const uint8_t *table, size_t table_size);
void ns10_crypto_destroy(NS10Crypto *crypto);
size_t ns10_crypto_profile_count(void);
int ns10_crypto_profile_info(size_t index, NS10ProfileInfo *info);
int ns10_crypto_lookup(const char *name, NS10ProfileInfo *info);
void ns10_crypto_reset(NS10Crypto *crypto);
/* Bit15 activates/reinitializes low-four-bit IV; otherwise deactivates. */
int ns10_crypto_control_write(NS10Crypto *crypto, uint16_t value);
int ns10_crypto_active(const NS10Crypto *crypto);
/* Inactive decrypt_word returns raw input, without changing history. */
uint16_t ns10_crypto_decrypt_word(NS10Crypto *crypto, uint16_t cipherword);
uint16_t ns10_crypto_unscramble(const NS10Crypto *crypto, uint16_t raw_word);
/* MEM(M) bank reads: direct flash mode bypasses transforms; banked reads
 * decrypt when active, otherwise banks<0x10 unscramble and data banks bypass. */
uint16_t ns10_crypto_memm_bank_word(NS10Crypto *crypto, uint16_t raw_word, unsigned bank,
                                    int direct_flash_mode);
void ns10_crypto_set_data_xor(NS10Crypto *crypto, uint16_t xor_value);
/* MEM/IO register writes byte-swap the XOR value, matching MAME. */
void ns10_crypto_dataxor_write(NS10Crypto *crypto, uint16_t register_value);
void ns10_crypto_get_state(const NS10Crypto *crypto, NS10CryptoState *state);
int ns10_crypto_set_state(NS10Crypto *crypto, const NS10CryptoState *state);
/* Small-page NAND read engine. Dumps must include each page's OOB bytes. */
typedef struct NS10NANDConfig {
    const uint8_t *bytes;
    size_t size;
    uint32_t pages;
    uint8_t manufacturer_id, device_id, sequential_row_read;
    /* Optional caller-owned writable backing, identical to bytes. NULL keeps
     * the read-only API. Writes never modify the on-disk source dump. */
    uint8_t *writable_bytes;
} NS10NANDConfig;
typedef struct NS10NANDState {
    uint32_t page, column;
    uint8_t mode, pointer_mode, address_cycle, status;
    uint32_t unsupported_commands;
} NS10NANDState;
NS10NAND *ns10_nand_create(const NS10NANDConfig *config);
void ns10_nand_destroy(NS10NAND *nand);
void ns10_nand_reset(NS10NAND *nand);
/* Read/ID/status/reset; writable backing also enables program80/10/15 and erase60/d0. */
int ns10_nand_command(NS10NAND *nand, uint8_t command);
void ns10_nand_address(NS10NAND *nand, uint8_t value);
int ns10_nand_write_byte(NS10NAND *nand, uint8_t value);
uint8_t ns10_nand_read_byte(NS10NAND *nand);
uint16_t ns10_nand_read_word(NS10NAND *nand);
void ns10_nand_get_state(const NS10NAND *nand, NS10NANDState *state);
/* Actual MEM(N) bus pipeline: two high-byte-first reads, active decrypt or
 * passive permutation/XOR. NAND0 blocks0/1 bypass passive unscrambling.
 * address_latch is the board's 24-bit column/row/page register, not byte offset.
 */
uint16_t ns10_crypto_nand_read_word(NS10Crypto *crypto, NS10NAND *nand, unsigned chip,
                                    uint32_t address_latch);
#ifdef __cplusplus
}
#endif
#endif
