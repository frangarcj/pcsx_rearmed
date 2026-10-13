/* Namco CDXA board: pinned MAME namcos12_cdxa.cpp, windyfairy, BSD-3-Clause.
 * Standalone integration GPL-2.0-or-later. */
#ifndef PCSX_SYSTEM12_CDXA_H
#define PCSX_SYSTEM12_CDXA_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct S12Cdxa S12Cdxa;
typedef void (*s12_cdxa_irq_cb)(void *opaque, int asserted);
S12Cdxa *s12_cdxa_create(const char *path, const char *sha1, s12_cdxa_irq_cb irq,
    void *opaque, char *error, size_t error_size);
void s12_cdxa_destroy(S12Cdxa *cdxa);
void s12_cdxa_reset(S12Cdxa *cdxa, uint64_t cycle);
void s12_cdxa_run_until(S12Cdxa *cdxa, uint64_t cycle);
int s12_cdxa_read(S12Cdxa *cdxa, uint32_t address, unsigned width, uint32_t *value);
int s12_cdxa_write(S12Cdxa *cdxa, uint32_t address, unsigned width, uint32_t value);
void s12_cdxa_sample(S12Cdxa *cdxa, uint64_t frame, int16_t stereo[2]);
void s12_cdxa_report(S12Cdxa *cdxa, FILE *out);
size_t s12_cdxa_state_size(void);
void s12_cdxa_get_state(S12Cdxa *cdxa, void *state);
void s12_cdxa_set_state(S12Cdxa *cdxa, const void *state);
#ifdef __cplusplus
}
#endif
#endif
