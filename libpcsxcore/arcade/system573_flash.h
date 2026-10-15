/* SPDX-License-Identifier: BSD-3-Clause. MAME ksys573.cpp, smf/R. Belmont. */
#ifndef PCSX_SYSTEM573_FLASH_H
#define PCSX_SYSTEM573_FLASH_H
#include "nor_flash.h"
#include "m48t58.h"
#include "adc0834.h"
#include "konami_security.h"
#include "konami_zs01.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { SYSTEM573_CASSETTE_ZI = 0x5a49 };
typedef struct System573Flash {
    nor_flash_t *flash[8];
    KonamiSecureFlash *cassette;
    KonamiZS01 *zs01;
    KonamiDS2401 *serial;
    uint64_t cassette_us;
    M48T58 rtc;
    Adc0834 adc;
    const uint8_t *h8_response;
    uint32_t input[4];
    uint16_t control,security;
    uint8_t h8_index,h8_clk,lamp_color,strobe,lamps[3];
    uint8_t jvs[512];unsigned jvs_r,jvs_w;
    unsigned cassette_type;
    uint8_t analog_output[4];
    struct {unsigned shift,bit;uint8_t data,clk,init;} stage[2];
    uint32_t stage_input,stage_mask;
    uint64_t stage_edges;
    /* Union of x8 dirty spans in the latest CPU flash write, including
     * changes before a later byte clears the chip's last-write diagnostics. */
    size_t dirty_start[2],dirty_length[2];
    uint64_t flash_reads,flash_writes,h8_edges,rtc_reads,rtc_writes,jvs_packets,empty_ata_reads;
    unsigned faults;
} System573Flash;
int system573_flash_init(System573Flash *b,uint8_t *flash[8],uint8_t *rtc,
                         const uint8_t *response,const uint8_t *cassette,const struct tm *local);
int system573_flash_init_cassette(System573Flash *b,uint8_t *flash[8],uint8_t *rtc,
                         const uint8_t *response,const uint8_t *cassette,
                         unsigned cassette_type,const struct tm *local);
/* Cassette ZI uses its own original EEPROM and serial, not X/Y fallback data. */
int system573_flash_init_zi(System573Flash *b,uint8_t *flash[8],uint8_t *rtc,
                         const uint8_t *response,const uint8_t *cassette,size_t cassette_size,
                         const uint8_t *serial,size_t serial_size,const struct tm *local);
void system573_flash_destroy(System573Flash *b);
void system573_flash_reset(System573Flash *b);
void system573_flash_advance_us(System573Flash *b,uint64_t us);
int system573_flash_read(System573Flash *b,uint32_t address,unsigned width,uint32_t *value);
int system573_flash_write(System573Flash *b,uint32_t address,unsigned width,uint32_t value);
/* Instruction-fetch view; data and commands still use actual x8 engines. */
void system573_flash_fetch_view(System573Flash *b,uint8_t *window);
#ifdef __cplusplus
}
#endif
#endif
