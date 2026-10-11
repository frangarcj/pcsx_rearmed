// license:BSD-3-Clause
// copyright-holders:R. Belmont, superctr
/*
 * Namco C352 custom PCM chip emulation.
 * Ported from MAME's c352.cpp (R. Belmont, superctr; additional code by
 * cync and the hoot development team). This standalone API takes the final
 * clock divisor; the common System 12 configuration is 25.4016 MHz / 288.
 *
 * BSD 3-Clause License
 * Copyright (c) R. Belmont and superctr
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain this copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce this copyright notice,
 *    this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "c352.h"

#include <stdlib.h>
#include <string.h>
#if defined(__ARM_NEON) && !defined(C352_SCALAR)
#include <arm_neon.h>
#endif

enum {
    C352_FLG_BUSY       = 0x8000,
    C352_FLG_KEYON      = 0x4000,
    C352_FLG_KEYOFF     = 0x2000,
    C352_FLG_LOOPTRG    = 0x1000,
    C352_FLG_LOOPHIST   = 0x0800,
    C352_FLG_FM          = 0x0400,
    C352_FLG_PHASERL    = 0x0200,
    C352_FLG_PHASEFL    = 0x0100,
    C352_FLG_PHASEFR    = 0x0080,
    C352_FLG_LDIR        = 0x0040,
    C352_FLG_LINK        = 0x0020,
    C352_FLG_NOISE       = 0x0010,
    C352_FLG_MULAW       = 0x0008,
    C352_FLG_FILTER      = 0x0004,
    C352_FLG_REVLOOP     = 0x0003,
    C352_FLG_LOOP        = 0x0002,
    C352_FLG_REVERSE     = 0x0001
};

typedef struct {
    uint32_t pos;
    uint32_t counter;
    int16_t sample;
    int16_t last_sample;
    uint16_t vol_f, vol_r;
    uint8_t curr_vol[4];
    uint16_t freq, flags;
    uint16_t wave_bank, wave_start, wave_end, wave_loop;
} C352Voice;

#ifdef C352_TRACE
/* Call trace for tools/c352-replay: C352_TRACE_FILE=path:frames records the
 * chip's calls until that many output frames, then the output's FNV-1a. */
#include <stdio.h>
enum { C352T_WRITE = 1, C352T_READ, C352T_GENERATE, C352T_RESET, C352T_END };
static FILE *c352t_file;
static unsigned long long c352t_left, c352t_hash = 0xcbf29ce484222325ull;
static void c352t_rec(uint32_t tag, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t r[4] = { tag, a, b, c };
    if (c352t_file) fwrite(r, 4, 4, c352t_file);
}
static void c352t_open(const uint8_t *rom, size_t rom_size, uint32_t clock_hz, unsigned divider)
{
    const char *env = getenv("C352_TRACE_FILE");
    char path[512];
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;
    if (!env || sscanf(env, "%511[^:]:%llu", path, &c352t_left) != 2 ||
        !(c352t_file = fopen(path, "wb")))
        return;
    for (i = 0; i < rom_size; i++)
        h = (h ^ rom[i]) * 0x100000001b3ull;
    c352t_rec(0x54323533, clock_hz, divider, (uint32_t)rom_size);
    c352t_rec((uint32_t)h, (uint32_t)(h >> 32), 0, 0);
}
static void c352t_output(const int16_t *out, size_t frames)
{
    const uint8_t *b = (const uint8_t *)out;
    size_t i;
    if (!c352t_file) return;
    for (i = 0; i < frames * 8; i++)
        c352t_hash = (c352t_hash ^ b[i]) * 0x100000001b3ull;
    if (frames >= c352t_left) {
        c352t_rec(C352T_END, (uint32_t)c352t_hash, (uint32_t)(c352t_hash >> 32), 0);
        fclose(c352t_file);
        c352t_file = NULL;
    } else
        c352t_left -= frames;
}
#define C352T(x) x
#else
#define C352T(x)
#endif

struct C352 {
    const uint8_t *rom;
    size_t rom_size;
    unsigned sample_rate;
    C352Voice voice[32];
    int16_t mulaw[256];
    uint16_t random;
    uint16_t control;
};

static int16_t wrap_s16(int32_t value)
{
    uint16_t bits = (uint16_t)(uint32_t)value;
    int32_t signed_value = bits < 0x8000u ? (int32_t)bits : (int32_t)bits - 0x10000;
    return (int16_t)signed_value;
}

static uint8_t read_byte(const C352 *c, uint32_t address)
{
    address &= 0x00ffffffu;
    return (size_t)address < c->rom_size ? c->rom[address] : 0xff;
}

static void fetch_sample(C352 *c, C352Voice *v)
{
    v->last_sample = v->sample;
    if (v->flags & C352_FLG_NOISE) {
        c->random = (uint16_t)((c->random >> 1) ^ ((uint16_t)-(int16_t)(c->random & 1) & 0xfff6));
        v->sample = (int16_t)c->random;
        return;
    }

    {
        uint8_t raw = read_byte(c, v->pos);
        uint16_t pos = (uint16_t)v->pos;
        v->sample = (v->flags & C352_FLG_MULAW)
                  ? c->mulaw[raw]
                  : wrap_s16((int32_t)(int8_t)raw * 256);

        if ((v->flags & C352_FLG_LOOP) && (v->flags & C352_FLG_REVERSE)) {
            if ((v->flags & C352_FLG_LDIR) && pos == v->wave_loop)
                v->flags &= (uint16_t)~C352_FLG_LDIR;
            else if (!(v->flags & C352_FLG_LDIR) && pos == v->wave_end)
                v->flags |= C352_FLG_LDIR;
            v->pos += (v->flags & C352_FLG_LDIR) ? (uint32_t)-1 : 1;
        } else if (pos == v->wave_end) {
            if ((v->flags & C352_FLG_LINK) && (v->flags & C352_FLG_LOOP)) {
                v->pos = ((uint32_t)v->wave_start << 16) | v->wave_loop;
                v->flags |= C352_FLG_LOOPHIST;
            } else if (v->flags & C352_FLG_LOOP) {
                v->pos = (v->pos & 0x00ff0000u) | v->wave_loop;
                v->flags |= C352_FLG_LOOPHIST;
            } else {
                v->flags |= C352_FLG_KEYOFF;
                v->flags &= (uint16_t)~C352_FLG_BUSY;
                v->sample = 0;
            }
        } else {
            v->pos += (v->flags & C352_FLG_REVERSE) ? (uint32_t)-1 : 1;
        }
    }
}

static void ramp_volume(C352Voice *v, unsigned channel, uint8_t target)
{
    int delta = (int)v->curr_vol[channel] - target;
    if (delta)
        v->curr_vol[channel] = (uint8_t)(v->curr_vol[channel] + (delta > 0 ? -1 : 1));
}

C352 *c352_create(const uint8_t *rom, size_t rom_size,
                  uint32_t clock_hz, unsigned divider)
{
    C352 *c;
    int j = 0;
    unsigned i;
    if (!divider)
        return NULL;
    c = (C352 *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->rom = rom;
    c->rom_size = rom_size;
    c->sample_rate = (unsigned)(clock_hz / divider);
    for (i = 0; i < 128; ++i) {
        c->mulaw[i] = (int16_t)(j << 5);
        if (i < 16) j += 1;
        else if (i < 24) j += 2;
        else if (i < 48) j += 4;
        else if (i < 100) j += 8;
        else j += 16;
    }
    for (i = 0; i < 128; ++i)
        c->mulaw[i + 128] = (int16_t)((~(uint16_t)c->mulaw[i]) & 0xffe0u);
    C352T(c352t_open(rom, rom_size, clock_hz, divider));
    c352_reset(c);
    return c;
}

void c352_destroy(C352 *c) { free(c); }

size_t c352_state_size(void) { return sizeof(C352); }

void c352_get_state(const C352 *c, void *state) { memcpy(state, c, sizeof(*c)); }

void c352_set_state(C352 *c, const void *state)
{
    const uint8_t *rom = c->rom;
    const size_t rom_size = c->rom_size;
    const unsigned sample_rate = c->sample_rate;
    memcpy(c, state, sizeof(*c));
    c->rom = rom;
    c->rom_size = rom_size;
    c->sample_rate = sample_rate;
}

void c352_reset(C352 *c)
{
    if (!c) return;
    C352T(c352t_rec(C352T_RESET, 0, 0, 0));
    memset(c->voice, 0, sizeof(c->voice));
    c->random = 0x1234;
    c->control = 0;
}

static uint16_t *voice_register(C352Voice *v, unsigned reg)
{
    switch (reg) {
    case 0: return &v->vol_f;
    case 1: return &v->vol_r;
    case 2: return &v->freq;
    case 3: return &v->flags;
    case 4: return &v->wave_bank;
    case 5: return &v->wave_start;
    case 6: return &v->wave_end;
    default: return &v->wave_loop;
    }
}

uint16_t c352_read(C352 *c, uint32_t offset)
{
    if (!c) return 0;
    C352T(c352t_rec(C352T_READ, offset, 0, 0));
    if (offset < 0x100)
        return *voice_register(&c->voice[offset / 8], offset % 8);
    if (offset == 0x200) return c->control;
    return 0;
}

static uint16_t combine_data(uint16_t old, uint16_t data, uint16_t mask)
{
    return (uint16_t)((old & (uint16_t)~mask) | (data & mask));
}

void c352_write(C352 *c, uint32_t offset, uint16_t data, uint16_t mem_mask)
{
    unsigned i;
    if (!c) return;
    C352T(c352t_rec(C352T_WRITE, offset, data, mem_mask));
    if (offset < 0x100) {
        uint16_t *reg = voice_register(&c->voice[offset / 8], offset % 8);
        *reg = combine_data(*reg, data, mem_mask);
    } else if (offset == 0x200) {
        c->control = combine_data(c->control, data, mem_mask);
    } else if (offset == 0x202 && mem_mask == 0xffff) {
        for (i = 0; i < 32; ++i) {
            C352Voice *v = &c->voice[i];
            if (v->flags & C352_FLG_KEYON) {
                v->pos = ((uint32_t)v->wave_bank << 16) | v->wave_start;
                v->sample = v->last_sample = 0;
                v->counter = 0xffff;
                v->flags |= C352_FLG_BUSY;
                v->flags &= (uint16_t)~(C352_FLG_KEYON | C352_FLG_LOOPHIST);
                memset(v->curr_vol, 0, sizeof(v->curr_vol));
            }
            if (v->flags & C352_FLG_KEYOFF) {
                v->flags &= (uint16_t)~(C352_FLG_BUSY | C352_FLG_KEYOFF);
                v->counter = 0xffff;
            }
        }
    }
}

unsigned c352_sample_rate(const C352 *c) { return c ? c->sample_rate : 0; }

/* MAME's order: every voice for one output frame, then the next frame. */
static void generate_frame_major(C352 *c, int16_t *out, size_t frames)
{
    size_t i;
    for (i = 0; i < frames; ++i) {
        int32_t mix[4] = { 0, 0, 0, 0 };
        unsigned j;
        for (j = 0; j < 32; ++j) {
            C352Voice *v = &c->voice[j];
            int32_t sample;
            uint32_t next;
            /* An idle voice outputs zero, so it adds nothing to any channel.
             * About 14 of 32 voices are idle in Soul Calibur play; on the
             * in-order Vita CPU this skips 27 instructions per idle voice. */
            if (!(v->flags & C352_FLG_BUSY))
                continue;
            next = v->counter + v->freq;
            if (next & 0x10000u) fetch_sample(c, v);
            if ((next ^ v->counter) & 0x18000u) {
                ramp_volume(v, 0, (uint8_t)(v->vol_f >> 8));
                ramp_volume(v, 1, (uint8_t)v->vol_f);
                ramp_volume(v, 2, (uint8_t)(v->vol_r >> 8));
                ramp_volume(v, 3, (uint8_t)v->vol_r);
            }
            v->counter = next & 0xffffu;
            sample = v->sample;
            /* MAME multiplies in u32 (counter is u32) and stores into s16.
             * A signed product overflows for large steps and the missing
             * 16-bit truncation then shifts the sample by 65536: a click. */
            if (!(v->flags & C352_FLG_FILTER))
                sample = (int16_t)(uint16_t)((uint32_t)v->last_sample +
                    (v->counter * (uint32_t)(v->sample - v->last_sample) >> 16));
            mix[0] += (((v->flags & C352_FLG_PHASEFL) ? -sample : sample) * v->curr_vol[0]) >> 8;
            mix[2] += (((v->flags & C352_FLG_PHASERL) ? -sample : sample) * v->curr_vol[2]) >> 8;
            mix[1] += (((v->flags & C352_FLG_PHASEFR) ? -sample : sample) * v->curr_vol[1]) >> 8;
            mix[3] += (((v->flags & C352_FLG_PHASEFR) ? -sample : sample) * v->curr_vol[3]) >> 8;
        }
        out[i * 4 + 0] = wrap_s16(mix[0] >> 3);
        out[i * 4 + 1] = wrap_s16(mix[1] >> 3);
        out[i * 4 + 2] = wrap_s16(mix[2] >> 3);
        out[i * 4 + 3] = wrap_s16(mix[3] >> 3);
    }
}

#define C352_BLOCK 256

/* The four channel volumes packed in a word (curr_vol[0] lowest), each
 * stepped by one toward its target, as ramp_volume does. */
static uint32_t ramp_packed(uint32_t cur, uint32_t target)
{
    uint32_t out = 0;
    unsigned k;
    for (k = 0; k < 32; k += 8) {
        uint32_t c8 = (cur >> k) & 0xff, t8 = (target >> k) & 0xff;
        if (c8 != t8)
            c8 = (c8 + (c8 > t8 ? 0xff : 1)) & 0xff;
        out |= c8 << k;
    }
    return out;
}

#if defined(__ARM_NEON) && !defined(C352_SCALAR)
/* Each channel's volume times its phase sign: (-s) * v == s * (-v). */
static int32x4_t signed_volumes(uint32_t packed, int32x4_t sign)
{
    uint16x8_t w = vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(packed)));
    return vmulq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(w))), sign);
}
#endif

/* The same sums in another order: one voice over the whole block, then the
 * next voice. Voices interact only through the mix, an integer sum, and
 * through the noise generator, which this path never runs (see below). A
 * voice keeps its state in registers for the block. */
static void generate_voice_major(C352 *c, int16_t *out, size_t frames)
{
    int32_t mix[C352_BLOCK * 4] __attribute__((aligned(16)));
    size_t i;
    unsigned j;
    memset(mix, 0, frames * 4 * sizeof(mix[0]));
    for (j = 0; j < 32; ++j) {
        C352Voice *v = &c->voice[j];
        uint16_t flags = v->flags;
        uint32_t counter, freq, vol, target;
        int32_t smp, last, sign[4];
        if (!(flags & C352_FLG_BUSY))
            continue;
        counter = v->counter;
        freq = v->freq;
        smp = v->sample;
        last = v->last_sample;
        sign[0] = (flags & C352_FLG_PHASEFL) ? -1 : 1;
        sign[1] = (flags & C352_FLG_PHASEFR) ? -1 : 1;
        sign[2] = (flags & C352_FLG_PHASERL) ? -1 : 1;
        sign[3] = sign[1];   /* MAME uses the FR phase for RR too */
        vol = (uint32_t)v->curr_vol[0] | (uint32_t)v->curr_vol[1] << 8 |
              (uint32_t)v->curr_vol[2] << 16 | (uint32_t)v->curr_vol[3] << 24;
        target = (uint32_t)(v->vol_f >> 8) | (uint32_t)(v->vol_f & 0xff) << 8 |
                 (uint32_t)(v->vol_r >> 8) << 16 | (uint32_t)(v->vol_r & 0xff) << 24;
#if defined(__ARM_NEON) && !defined(C352_SCALAR)
        int32x4_t signq = vld1q_s32(sign);
        int32x4_t svq = signed_volumes(vol, signq);
#else
        int32_t sv[4];
        sv[0] = sign[0] * (int32_t)(vol & 0xff);
        sv[1] = sign[1] * (int32_t)(vol >> 8 & 0xff);
        sv[2] = sign[2] * (int32_t)(vol >> 16 & 0xff);
        sv[3] = sign[3] * (int32_t)(vol >> 24);
#endif
        for (i = 0; i < frames; ++i) {
            int32_t sample;
            uint32_t next;
            if (!(flags & C352_FLG_BUSY))
                break;   /* key-off ended it: silent for the rest */
            next = counter + freq;
            if (next & 0x10000u) {
                uint32_t pos = v->pos;
                /* fetch_sample's common case inline: no reverse loop, not
                 * at the wave end. Noise never runs on this path. */
                if ((flags & C352_FLG_REVLOOP) != C352_FLG_REVLOOP &&
                    (uint16_t)pos != v->wave_end) {
                    uint8_t raw = read_byte(c, pos);
                    last = smp;
                    smp = (flags & C352_FLG_MULAW) ? c->mulaw[raw]
                                                   : (int16_t)((int8_t)raw * 256);
                    v->last_sample = (int16_t)last;
                    v->sample = (int16_t)smp;
                    v->pos = pos + ((flags & C352_FLG_REVERSE) ? (uint32_t)-1 : 1);
                }
                else {
                    fetch_sample(c, v);
                    flags = v->flags;
                    smp = v->sample;
                    last = v->last_sample;
                }
            }
            if (((next ^ counter) & 0x18000u) && vol != target) {
                vol = ramp_packed(vol, target);
#if defined(__ARM_NEON) && !defined(C352_SCALAR)
                svq = signed_volumes(vol, signq);
#else
                sv[0] = sign[0] * (int32_t)(vol & 0xff);
                sv[1] = sign[1] * (int32_t)(vol >> 8 & 0xff);
                sv[2] = sign[2] * (int32_t)(vol >> 16 & 0xff);
                sv[3] = sign[3] * (int32_t)(vol >> 24);
#endif
            }
            counter = next & 0xffffu;
            sample = smp;
            /* MAME's interpolation: u32 product, stored into s16. */
            if (!(flags & C352_FLG_FILTER))
                sample = (int16_t)(uint16_t)((uint32_t)last +
                    (counter * (uint32_t)(smp - last) >> 16));
#if defined(__ARM_NEON) && !defined(C352_SCALAR)
            {
                int32_t *m = &mix[i * 4];
                vst1q_s32(m, vaddq_s32(vld1q_s32(m),
                    vshrq_n_s32(vmulq_n_s32(svq, sample), 8)));
            }
#else
            mix[i * 4 + 0] += (sample * sv[0]) >> 8;
            mix[i * 4 + 1] += (sample * sv[1]) >> 8;
            mix[i * 4 + 2] += (sample * sv[2]) >> 8;
            mix[i * 4 + 3] += (sample * sv[3]) >> 8;
#endif
        }
        v->counter = counter;
        v->curr_vol[0] = (uint8_t)vol;
        v->curr_vol[1] = (uint8_t)(vol >> 8);
        v->curr_vol[2] = (uint8_t)(vol >> 16);
        v->curr_vol[3] = (uint8_t)(vol >> 24);
    }
#if defined(__ARM_NEON) && !defined(C352_SCALAR)
    for (i = 0; i < frames; ++i)
        vst1_s16(out + i * 4, vmovn_s32(vshrq_n_s32(vld1q_s32(&mix[i * 4]), 3)));
#else
    for (i = 0; i < frames * 4; ++i)
        out[i] = wrap_s16(mix[i] >> 3);
#endif
}

void c352_generate(C352 *c, int16_t *out, size_t frames)
{
    size_t done;
    unsigned j;
    int noise = 0;
    if (!c || !out) return;
    C352T(c352t_rec(C352T_GENERATE, (uint32_t)frames, 0, 0));
    /* Noise voices share one generator, advanced in voice order within a
     * frame: only MAME's order reproduces its sequence. A voice cannot turn
     * noise on during a call (only register writes change that flag). */
    for (j = 0; j < 32; ++j)
        if ((c->voice[j].flags & (C352_FLG_BUSY | C352_FLG_NOISE)) ==
            (C352_FLG_BUSY | C352_FLG_NOISE))
            noise = 1;
    for (done = 0; done < frames; ) {
        size_t n = frames - done > C352_BLOCK ? C352_BLOCK : frames - done;
        if (noise || n < 2)
            generate_frame_major(c, out + done * 4, n);
        else
            generate_voice_major(c, out + done * 4, n);
        done += n;
    }
    C352T(c352t_output(out, frames));
}
