/* SPDX-License-Identifier: BSD-3-Clause. MAME timekpr.cpp, Aaron Giles/smf. */
#ifndef PCSX_M48T58_H
#define PCSX_M48T58_H
#include <stdint.h>
#include <time.h>
typedef struct M48T58 {
    uint8_t *ram;
    uint8_t counter[8];
    uint64_t fraction_us, ticks;
} M48T58;
/* Borrowed original 8 KiB SRAM. Clock starts at the supplied local time,
 * as MAME device_rtc_interface does; cold/warm NVRAM cannot replace it. */
int m48t58_init(M48T58 *rtc, uint8_t *ram, const struct tm *local);
void m48t58_advance_us(M48T58 *rtc, uint64_t elapsed);
uint8_t m48t58_read(const M48T58 *rtc, unsigned address);
void m48t58_write(M48T58 *rtc, unsigned address, uint8_t value);
void m48t58_nvram_import(M48T58 *rtc, const uint8_t *image);
#endif
