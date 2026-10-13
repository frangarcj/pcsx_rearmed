// license:BSD-3-Clause
// copyright-holders:smf
/* MAME namcos11.cpp/ns11prot.cpp, deb2f54fe7300e7a192690318beb860159bfd52a.
 * See THIRD_PARTY_NOTICES.md for the BSD terms. */
#include "namcos11_board.h"
#include "c352.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define BANK_BYTES 0x100000u
#define IRQ_PERIOD 141120u
#define SAMPLE_PERIOD 96u
struct Namcos11Board {
    Namcos11Config cfg;
    Namcos11Status status;
    M37710 *cpu;
    C352 *sound;
    AT28C16 *eeprom;
    uint8_t shared[0x8000];
    uint16_t key_p[3];
    unsigned irq_left, sample_left;
    uint64_t eeprom_us, eeprom_fraction;
};

static uint16_t keycus_unknown(Namcos11Board *b, unsigned offset)
{
    (void)offset;
    b->status.unknown_keycus_reads++;
    if (b->cfg.random16) return b->cfg.random16(b->cfg.opaque);
    b->status.fault = 1;
    return 0xffff;
}
#include "namcos11_keycus.inc"
static uint16_t key_read(Namcos11Board *b, unsigned offset)
{
    switch (b->cfg.keycus) {
#define KEY_READ(n) case n: return keycus_##n##_read(b, offset)
    KEY_READ(406); KEY_READ(409); KEY_READ(410); KEY_READ(411);
    KEY_READ(430); KEY_READ(431); KEY_READ(432); KEY_READ(442); KEY_READ(443);
#undef KEY_READ
    }
    return keycus_unknown(b, offset);
}
static void key_write(Namcos11Board *b, unsigned offset, uint16_t data)
{
    switch (b->cfg.keycus) {
#define KEY_WRITE(n) case n: keycus_##n##_write(b, offset, data); break
    KEY_WRITE(406); KEY_WRITE(409); KEY_WRITE(410); KEY_WRITE(411);
    KEY_WRITE(430); KEY_WRITE(431); KEY_WRITE(432); KEY_WRITE(442); KEY_WRITE(443);
#undef KEY_WRITE
    }
}
static uint16_t get_input(Namcos11Board *b, unsigned input)
{
    return b->cfg.input ? b->cfg.input(b->cfg.opaque, input) : 0xff;
}
static uint8_t c76_read8(void *opaque, uint32_t a)
{
    Namcos11Board *b = opaque;
    uint16_t word;
    if (a >= 0x1000 && a < 0x1008) {
        word = get_input(b, (a - 0x1000) / 2);
        return (uint8_t)(word >> ((a & 1) * 8));
    }
    if (a >= 0x2000 && a < 0x3000) {
        word = c352_read(b->sound, (a - 0x2000) / 2);
        return (uint8_t)(word >> ((a & 1) * 8));
    }
    if (a >= 0x4000 && a < 0xc000) return b->shared[a - 0x4000];
    if ((a >= 0x080000 && a < 0x100000) || (a >= 0x200000 && a < 0x300000))
        return b->cfg.c76_data[a & 0x7ffff];
    /* Explicitly preserves MAME's documented Family Bowl unknown-ROM hack. */
    if (a >= 0x510000 && a < 0x520000) return 0x80;
    b->status.unmapped_c76_reads++;
    return 0xff;
}
static uint16_t c76_read16(void *opaque, uint32_t a)
{
    Namcos11Board *b = opaque;
    if (!(a & 1) && a >= 0x2000 && a < 0x3000)
        return c352_read(b->sound, (a - 0x2000) / 2);
    return (uint16_t)(c76_read8(b, a) | ((uint16_t)c76_read8(b, (a + 1) & 0xffffff) << 8));
}
static void c76_write8(void *opaque, uint32_t a, uint8_t value)
{
    Namcos11Board *b = opaque;
    if (a >= 0x2000 && a < 0x3000) {
        unsigned shift = (a & 1) * 8;
        c352_write(b->sound, (a - 0x2000) / 2, (uint16_t)value << shift, 0xffu << shift);
    }
    else if (a >= 0x4000 && a < 0xc000) b->shared[a - 0x4000] = value;
    else if ((a >= 0x300000 && a < 0x300002) || (a >= 0x301000 && a < 0x301002)) { }
    else b->status.unmapped_c76_writes++;
}
static void c76_write16(void *opaque, uint32_t a, uint16_t value)
{
    Namcos11Board *b = opaque;
    if (!(a & 1) && a >= 0x2000 && a < 0x3000)
        c352_write(b->sound, (a - 0x2000) / 2, value, 0xffff);
    else { c76_write8(b, a, value); c76_write8(b, (a + 1) & 0xffffff, value >> 8); }
}
static uint16_t c76_analog(void *opaque, unsigned channel)
{
    Namcos11Board *b = opaque;
    return b->cfg.analog ? b->cfg.analog(b->cfg.opaque, channel) : 0xff;
}
static void c76_irq_ack(void *opaque, unsigned line)
{
    Namcos11Board *b = opaque;
    if (line == M37710_LINE_IRQ0 || line == M37710_LINE_IRQ2)
        m37710_irq(b->cpu, line, 0); /* MAME HOLD_LINE is cleared on acknowledge. */
}
static void c76_diagnostic(void *opaque, unsigned kind, uint32_t pc, uint32_t detail)
{
    Namcos11Board *b = opaque;
    fprintf(stderr, "System11 C76 diagnostic: kind=%u pc=%06x detail=%06x\n", kind, pc, detail);
    b->status.fault = 1;
}
static int valid_key(unsigned key)
{
    switch (key) {
    case 406: case 409: case 410: case 411: case 430: case 431:
    case 432: case 442: case 443: return 1;
    default: return 0;
    }
}
Namcos11Board *namcos11_create(const Namcos11Config *cfg)
{
    Namcos11Board *b;
    M37710Config cpu;
    if (!cfg || cfg->model > NAMCOS11_COH110 || cfg->banking > NAMCOS11_BANK_ROM8_64
        || !valid_key(cfg->keycus) || !cfg->main_rom || cfg->main_size != 0x400000
        || !cfg->c76_data || cfg->c76_size != 0x80000
        || !cfg->c76_bios || cfg->c76_bios_size != 0x4000
        || !cfg->wave_rom || !cfg->wave_size || cfg->wave_size > 0x1000000
        || (cfg->banking != NAMCOS11_BANK_NONE && (!cfg->bank_rom || !cfg->bank_size
            || cfg->bank_size % BANK_BYTES || cfg->bank_size > 0x4000000))
        || (cfg->lightgun && (cfg->banking != NAMCOS11_BANK_ROM8_64 || !cfg->gun)))
        return NULL;
    b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->cfg = *cfg;
    b->eeprom = at28c16_create(NULL, 0);
    if (!b->eeprom) { free(b); return NULL; }
    b->sound = c352_create(cfg->wave_rom, cfg->wave_size, 25401600, 288);
    if (!b->sound) { at28c16_destroy(b->eeprom); free(b); return NULL; }
    memset(&cpu, 0, sizeof(cpu));
    cpu.model = M37710_MODEL_M37702M2; cpu.opaque = b;
    cpu.read8 = c76_read8; cpu.read16 = c76_read16;
    cpu.write8 = c76_write8; cpu.write16 = c76_write16;
    cpu.analog_read = c76_analog; cpu.irq_ack = c76_irq_ack;
    cpu.diagnostic = c76_diagnostic;
    cpu.internal_rom = cfg->c76_bios; cpu.internal_rom_size = cfg->c76_bios_size;
    b->cpu = m37710_create(&cpu);
    if (!b->cpu) { c352_destroy(b->sound); at28c16_destroy(b->eeprom); free(b); return NULL; }
    namcos11_reset(b);
    return b;
}
void namcos11_destroy(Namcos11Board *b)
{
    if (!b) return;
    m37710_destroy(b->cpu); c352_destroy(b->sound); at28c16_destroy(b->eeprom); free(b);
}
void namcos11_reset(Namcos11Board *b)
{
    if (!b) return;
    memset(&b->status, 0, sizeof(b->status));
    memset(b->key_p, 0, sizeof(b->key_p));
    b->irq_left = 0; b->sample_left = SAMPLE_PERIOD;
    c352_reset(b->sound); m37710_reset(b->cpu);
}
static int valid_access(Namcos11Board *b, uint32_t a, unsigned width)
{
    return b && (width == 1 || width == 2 || width == 4) && !(a & (width - 1));
}
int namcos11_read(Namcos11Board *b, uint32_t a, unsigned width, uint32_t *value)
{
    unsigned i;
    uint32_t v = 0;
    if (!value || !valid_access(b, a, width)) return -1;
    if (a >= 0x1fc00000 && a < 0x20000000) {
        for (i = 0; i < width; i++) v |= (uint32_t)b->cfg.main_rom[a - 0x1fc00000 + i] << (i * 8);
    }
    else if (b->cfg.banking && a >= 0x1f000000 && a < 0x1f800000
             && !(b->cfg.lightgun && a >= 0x1f780000 && a < 0x1f780010)) {
        size_t offset = (size_t)b->status.banks[(a - 0x1f000000) / BANK_BYTES] * BANK_BYTES;
        offset += a & (BANK_BYTES - 1);
        if (offset + width > b->cfg.bank_size) { b->status.fault = 1; return -1; }
        for (i = 0; i < width; i++) v |= (uint32_t)b->cfg.bank_rom[offset + i] << (i * 8);
    }
    else if (a >= 0x1fa04000 && a < 0x1fa10000) {
        /* Upstream declares a 48 KiB PSX aperture over a 32 KiB share.
         * Do not reproduce an out-of-bounds host access in its final 16 KiB. */
        if (a - 0x1fa04000 + width > sizeof(b->shared)) { b->status.fault = 1; return -1; }
        for (i = 0; i < width; i++) v |= (uint32_t)b->shared[a - 0x1fa04000 + i] << (i * 8);
    }
    else if (a >= 0x1fa20000 && a < 0x1fa20020) {
        if (width == 4) v = key_read(b, (a - 0x1fa20000) / 2) |
                          ((uint32_t)key_read(b, (a - 0x1fa20000) / 2 + 1) << 16);
        else v = key_read(b, (a - 0x1fa20000) / 2) >> ((a & 1) * 8);
    }
    else if (a >= 0x1fa30000 && a < 0x1fa31000) {
        for (i = 0; i < width; i++) v |= (uint32_t)((a + i) & 1 ? 0xff :
            at28c16_read(b->eeprom, (a + i - 0x1fa30000) / 2)) << (i * 8);
    }
    else if (b->cfg.banking == NAMCOS11_BANK_ROM8_64 && a >= 0x1fa10020 && a < 0x1fa10030)
        v = UINT32_MAX; /* MAME nopr; trimmed to access width below. */
    else if (b->cfg.lightgun && a >= 0x1f780000 && a < 0x1f780010) {
        unsigned word = (a - 0x1f780000) / 2, lane;
        for (lane = 0; lane < (width == 4 ? 2u : 1u); lane++, word++) {
            uint16_t gun = 0;
            switch (word) {
            case 0: gun = b->cfg.gun(b->cfg.opaque, 0); break;
            case 2: case 3: gun = b->cfg.gun(b->cfg.opaque, 1) + (word == 3); break;
            case 4: gun = b->cfg.gun(b->cfg.opaque, 2); break;
            case 6: case 7: gun = b->cfg.gun(b->cfg.opaque, 3) + (word == 7); break;
            }
            v |= (uint32_t)gun << (lane * 16);
        }
        if (width == 1) v >>= (a & 1) * 8;
    }
    else return 0;
    *value = width == 1 ? v & 0xff : width == 2 ? v & 0xffff : v;
    return b->status.fault ? -1 : 1;
}
int namcos11_write(Namcos11Board *b, uint32_t a, unsigned width, uint32_t value)
{
    unsigned i;
    if (!valid_access(b, a, width)) return -1;
    if (a >= 0x1fa04000 && a < 0x1fa10000) {
        if (a - 0x1fa04000 + width > sizeof(b->shared)) { b->status.fault = 1; return -1; }
        for (i = 0; i < width; i++) b->shared[a - 0x1fa04000 + i] = value >> (i * 8);
    }
    else if (a >= 0x1fa20000 && a < 0x1fa20020) {
        if (width == 4) { key_write(b, (a - 0x1fa20000) / 2, value);
            key_write(b, (a - 0x1fa20000) / 2 + 1, value >> 16); }
        else key_write(b, (a - 0x1fa20000) / 2, value << ((a & 1) * 8));
    }
    else if (a >= 0x1fa30000 && a < 0x1fa31000) {
        for (i = 0; i < width; i++) if (!((a + i) & 1))
            at28c16_write(b->eeprom, (a + i - 0x1fa30000) / 2, value >> (i * 8));
    }
    else if (b->cfg.banking && a >= 0x1fa10020 && a < 0x1fa10030) {
        for (i = 0; i < (width == 4 ? 2u : 1u); i++) {
            uint16_t data = value >> (i * 16);
            unsigned offset = (a - 0x1fa10020) / 2 + i, entry;
            if (width == 1) data <<= (a & 1) * 8;
            entry = b->cfg.banking == NAMCOS11_BANK_ROM8 ?
                ((data & 0xc0) >> 4) + (data & 3) :
                (((data & 0xc0) >> 3) + (data & 7)) ^ b->status.bank_upper;
            if ((size_t)entry * BANK_BYTES >= b->cfg.bank_size) b->status.fault = 1;
            else b->status.banks[offset] = entry;
        }
    }
    else if (b->cfg.banking == NAMCOS11_BANK_ROM8_64 && a >= 0x1f080000 && a < 0x1f080004)
        b->status.bank_upper = ((a - 0x1f080000) / 2 + (width == 4)) * 16;
    else if (b->cfg.lightgun && a >= 0x1f788000 && a < 0x1f788004) {
        if (a < 0x1f788002 && b->cfg.gun_outputs) {
            uint16_t data = value << ((a & 1) * 8);
            uint32_t inverse = ~(uint32_t)data;
            b->cfg.gun_outputs(b->cfg.opaque, ((inverse >> 3) & 1) | ((inverse >> 1) & 2),
                ((inverse >> 1) & 1) | ((inverse << 1) & 2));
        }
    }
    else if ((a >= 0x1fb00000 && a < 0x1fb00004) || (a >= 0x1fbf6000 && a < 0x1fbf6004)) { }
    else return 0;
    return b->status.fault ? -1 : 1;
}
void namcos11_run_cycles(Namcos11Board *b, uint64_t cycles)
{
    if (!b) return;
    if (!b->irq_left && !b->status.fault) {
        /* Both MAME timer_device instances start immediately after reset. */
        m37710_irq(b->cpu, M37710_LINE_IRQ0, 1);
        m37710_irq(b->cpu, M37710_LINE_IRQ2, 1);
        b->status.irq0_ticks++; b->status.irq2_ticks++;
        b->irq_left = IRQ_PERIOD;
    }
    while (cycles && !b->status.fault) {
        unsigned step = b->sample_left < b->irq_left ? b->sample_left : b->irq_left;
        if (cycles < step) step = (unsigned)cycles;
        m37710_run_cycles(b->cpu, step);
        b->status.cycles += step; cycles -= step;
        b->eeprom_fraction += (uint64_t)step * 1000000;
        b->eeprom_us += b->eeprom_fraction / 8467200;
        b->eeprom_fraction %= 8467200;
        at28c16_advance_to_us(b->eeprom, b->eeprom_us);
        b->sample_left -= step; b->irq_left -= step;
        if (!b->sample_left) {
            int16_t samples[4];
            c352_generate(b->sound, samples, 1);
            if (b->cfg.audio) b->cfg.audio(b->cfg.opaque, samples, 1);
            b->status.samples++; b->sample_left = SAMPLE_PERIOD;
        }
        if (!b->irq_left) {
            m37710_irq(b->cpu, M37710_LINE_IRQ0, 1);
            m37710_irq(b->cpu, M37710_LINE_IRQ2, 1);
            b->status.irq0_ticks++; b->status.irq2_ticks++;
            b->irq_left = IRQ_PERIOD;
        }
    }
}
void namcos11_get_status(const Namcos11Board *b, Namcos11Status *s)
{ if (b && s) *s = b->status; }
M37710 *namcos11_c76(Namcos11Board *b) { return b ? b->cpu : NULL; }
unsigned namcos11_gpu_model(const Namcos11Board *b)
{ return b && b->cfg.model == NAMCOS11_COH100 ? 1 : 2; }
int namcos11_eeprom_import(Namcos11Board *b, const uint8_t *data, size_t size)
{ return b ? at28c16_import_image(b->eeprom, data, size) : -1; }
int namcos11_eeprom_export(const Namcos11Board *b, uint8_t *data, size_t size)
{ return b ? at28c16_get_image(b->eeprom, data, size) : -1; }
