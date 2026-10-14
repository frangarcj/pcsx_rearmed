/* SPDX-License-Identifier: BSD-3-Clause. MAME ksys573.cpp, smf/R. Belmont. */
#ifndef PCSX_SYSTEM573_FLASH_H
#define PCSX_SYSTEM573_FLASH_H
#include "nor_flash.h"
#include "m48t58.h"
#include "adc0834.h"
#include "konami_security.h"
typedef struct System573Flash {
    nor_flash_t *flash[8];
    KonamiSecureFlash *cassette;
    M48T58 rtc;
    Adc0834 adc;
    const uint8_t *h8_response;
    uint32_t input[4];
    uint16_t control,security;
    uint8_t h8_index,h8_clk,lamp_color,strobe,lamps[3];
    uint8_t jvs[512];unsigned jvs_r,jvs_w;
    uint64_t flash_reads,flash_writes,h8_edges,rtc_reads,rtc_writes,jvs_packets,empty_ata_reads;
    unsigned faults;
} System573Flash;
int system573_flash_init(System573Flash *b,uint8_t *flash[8],uint8_t *rtc,
                         const uint8_t *response,const uint8_t *cassette,const struct tm *local);
void system573_flash_destroy(System573Flash *b);
void system573_flash_reset(System573Flash *b);
void system573_flash_advance_us(System573Flash *b,uint64_t us);
int system573_flash_read(System573Flash *b,uint32_t address,unsigned width,uint32_t *value);
int system573_flash_write(System573Flash *b,uint32_t address,unsigned width,uint32_t value);
/* Instruction-fetch view; data and commands still use actual x8 engines. */
void system573_flash_fetch_view(System573Flash *b,uint8_t *window);
#endif
