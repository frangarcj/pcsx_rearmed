// license:BSD-3-Clause
// copyright-holders:R. Belmont, superctr
/* Standalone Namco C352 PCM engine, adapted from MAME's c352 device. */
#ifndef LIBPCSXCORE_ARCADE_C352_H
#define LIBPCSXCORE_ARCADE_C352_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct C352 C352;

/* ROM is borrowed and must remain valid until c352_destroy(). */
C352 *c352_create(const uint8_t *rom, size_t rom_size,
                  uint32_t clock_hz, unsigned divider);
void c352_destroy(C352 *c352);
void c352_reset(C352 *c352);

/* Offsets are 16-bit word offsets, matching MAME's register bus map. */
uint16_t c352_read(C352 *c352, uint32_t word_offset);
/* mem_mask uses MAME's COMBINE_DATA convention; execute-key at 0x202
 * accepts only mem_mask == 0xffff, matching the original device. */
void c352_write(C352 *c352, uint32_t word_offset,
                uint16_t data, uint16_t mem_mask);

unsigned c352_sample_rate(const C352 *c352);
/* Writes interleaved FL, FR, RL, RR signed 16-bit samples. */
void c352_generate(C352 *c352, int16_t *out_interleaved_4ch, size_t frames);

/* Savestates: the voices and registers as an opaque block of
 * c352_state_size() bytes for this build. A restore keeps the sample ROM
 * and rate of the chip it is applied to. */
size_t c352_state_size(void);
void c352_get_state(const C352 *c352, void *state);
void c352_set_state(C352 *c352, const void *state);

#ifdef __cplusplus
}
#endif

#endif
