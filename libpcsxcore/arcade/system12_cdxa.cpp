/* Namco CDXA/SH7014 adapter. Mapping and peripheral semantics follow MAME
 * deb2f54f, namcos12_cdxa.cpp and sh7014*.cpp (windyfairy, BSD-3-Clause).
 * Standalone integration GPL-2.0-or-later. */
#include "system12_cdxa.h"
#include "sh2.h"
#include "chd_cd.h"
#include <algorithm>
#include <cstring>
#include <new>
#include <cinttypes>
#include <cstdlib>

namespace {
constexpr uint64_t CPU_HZ = 14745600;
struct AudioSample { uint64_t frame; int16_t left, right; };
struct CdxaState {
    uint8_t cram[0x40000]{}, sram[0x10000]{}, peripheral[0x1000]{}, cache[0x1000]{};
    uint8_t packet[12]{}, data[65536]{}, sector[2352]{}, ata[8]{};
    unsigned packet_size{}, data_size{}, data_pos{}, phase{}, sector_pos{};
    uint32_t read_lba{}, read_blocks{}, sector_bytes{}, remaining{};
    uint8_t read_fields{};
    uint8_t sense{}, ide_sh{}, ide_ps{}, sram_enabled{}, irq_busy{}, running{};
    uint64_t cycles{}, commands{}, sectors{}, faults{}, sci_next{}, sci_fraction{};
    uint64_t dma_next[2]{}, timer_base[3]{};
    uint8_t dma_active[2]{};
    uint8_t sci_busy{}, sci_shift{}, audio_phase{}, audio_high{};
    int16_t pcm[2]{}, output[2]{};
    AudioSample audio[4096]{};
    unsigned audio_head{}, audio_tail{};
    uint64_t audio_frames{}, dma_transfers{};
    uint32_t last_address{}, last_value{};
    Sh2State cpu{};
};
static uint16_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
}
struct S12Cdxa {
    CdxaState s;
    Sh2CPU *cpu = nullptr;
    ChdCd *disc = nullptr;
    nscsi_cd_media_ops_t media{};
    s12_cdxa_irq_cb irq = nullptr;
    void *opaque = nullptr;
    bool in_dma = false;
    ~S12Cdxa() { sh2_destroy(cpu); chd_cd_close(disc); }
    uint16_t ata_read(unsigned reg);
    void ata_write(unsigned reg, uint16_t value);
    void command(uint8_t value);
    void packet_command();
    void response(const uint8_t *data, unsigned size);
    void fill_read();
    bool read_sector(uint32_t lba);
    uint32_t read(uint32_t a, unsigned width);
    void write(uint32_t a, unsigned width, uint32_t value);
    void tick(uint64_t cycle);
    void irq_sh(unsigned vector);
    void dma(unsigned resource);
    void sci_start();
    void dma_transfer(unsigned channel);
    void timers(uint64_t cycle);
};
static uint32_t cpu_read(void *opaque, uint32_t a, unsigned width) { return static_cast<S12Cdxa *>(opaque)->read(a, width); }
static void cpu_write(void *opaque, uint32_t a, unsigned width, uint32_t v) { static_cast<S12Cdxa *>(opaque)->write(a, width, v); }

void S12Cdxa::response(const uint8_t *bytes, unsigned size) {
    size = std::min(size, unsigned(sizeof(s.data)));
    std::memcpy(s.data, bytes, size); s.data_size = size; s.data_pos = 0;
    s.phase = size ? 2 : 0;
    s.ata[7] = (s.ata[7] & 0x10) | (size ? 0x48 : 0x40);
    s.ata[2] = size ? 2 : 3;
    s.ata[4] = size; s.ata[5] = size >> 8;
}
void S12Cdxa::fill_read() {
    unsigned size = 0;
    unsigned limit = be16(s.peripheral + 0xff0);
    if (!limit) limit = 65536;
    while (s.remaining && size < limit) {
        if (!s.sector_pos) {
            int ok = read_sector(s.read_lba);
            if (!ok) { s.faults++; s.ata[7] = 0x41; s.ata[1] = 0x50; s.phase = 0; return; }
            s.sectors++;
        }
        unsigned n = std::min({s.sector_bytes - s.sector_pos, s.remaining, limit - size});
        std::memcpy(s.data + size, s.sector + s.sector_pos, n);
        size += n; s.remaining -= n; s.sector_pos += n;
        if (s.sector_pos == s.sector_bytes) { s.sector_pos = 0; s.read_lba++; }
    }
    s.data_size = size; s.data_pos = 0; s.phase = size ? 2 : 0;
    s.ata[7] = (s.ata[7] & 0x10) | (size ? 0x48 : 0x40); s.ata[2] = size ? 2 : 3;
    s.ata[4] = size; s.ata[5] = size >> 8;
}
bool S12Cdxa::read_sector(uint32_t lba) {
    if (!s.read_fields) { s.sector_bytes = 2048; return media.read_mode1_sector(media.opaque, lba, s.sector); }
    uint8_t raw[2352];
    if (!chd_cd_read_raw_sector(disc, lba, raw)) return false;
    bool mode2 = raw[15] == 2, form2 = mode2 && (raw[18] & 0x20);
    unsigned used = 0;
    auto field = [&](unsigned offset, unsigned size) { std::memcpy(s.sector + used, raw + offset, size); used += size; };
    if (s.read_fields & 0x80) field(0, 12);
    if (s.read_fields & 0x20) field(12, 4);
    if ((s.read_fields & 0x40) && mode2) field(16, 8);
    if (s.read_fields & 0x10) field(mode2 ? 24 : 16, form2 ? 2328 : 2048);
    if ((s.read_fields & 8) && !form2) field(mode2 ? 2072 : 2064, mode2 ? 280 : 288);
    s.sector_bytes = used;
    return used != 0;
}
void S12Cdxa::packet_command() {
    uint8_t bytes[256]{};
    const uint8_t *p = s.packet;
    s.commands++;
    const char *trace = std::getenv("PCSX_CDXA_TRACE");
    if (trace && s.commands < 100) {
        std::fprintf(stderr, "CDXA packet ");
        for (unsigned i = 0; i < 12; i++) std::fprintf(stderr, "%02x", p[i]);
        std::fprintf(stderr, "\n");
    }
    switch (p[0]) {
    case 0x2b:
        response(bytes, 0); s.ata[7] |= 0x10; break;
    case 0x00: case 0x1b: case 0x1e: case 0x4b: case 0xbb:
        response(bytes, 0); break;
    case 0x03:
        bytes[0] = 0x70; bytes[2] = s.sense; bytes[7] = 10;
        response(bytes, std::min(unsigned(p[4]), 18u)); s.sense = 0; break;
    case 0x12:
        bytes[0] = 5; bytes[1] = 0x80; bytes[2] = 0; bytes[3] = 0x21; bytes[4] = 31;
        std::memcpy(bytes + 8, "TOSHIBA ", 8); std::memcpy(bytes + 16, "CD-ROM XM-6402B  ", 16);
        std::memcpy(bytes + 32, "1008", 4); response(bytes, std::min(unsigned(p[4]), 36u)); break;
    case 0x25:
        put32(bytes, chd_cd_last_lba(disc) - 1); put32(bytes + 4, 2048); response(bytes, 8); break;
    case 0x43: {
        put16(bytes, 18); bytes[2] = bytes[3] = 1;
        bytes[5] = 0x14; bytes[6] = 1; bytes[13] = 0x14; bytes[14] = 0xaa;
        if (p[1] & 2) {
            uint32_t frames = chd_cd_last_lba(disc) + 150;
            bytes[9] = 0; bytes[10] = 2; bytes[11] = 0;
            bytes[17] = frames / 4500; bytes[18] = (frames / 75) % 60; bytes[19] = frames % 75;
        } else put32(bytes + 16, chd_cd_last_lba(disc));
        response(bytes, std::min(unsigned(be16(p + 7)), 20u)); break;
    }
    case 0x5a:
        put16(bytes, 26); bytes[8] = 0x2a; bytes[9] = 18; bytes[10] = 3;
        bytes[12] = 0x71; bytes[13] = 0x60; put16(bytes + 16, 1764); put16(bytes + 22, 1764);
        response(bytes, std::min(unsigned(be16(p + 7)), 28u)); break;
    case 0x28: case 0xa8: case 0xbe:
        s.read_lba = be32(p + 2);
        s.read_blocks = p[0] == 0x28 ? be16(p + 7) : p[0] == 0xa8 ? be32(p + 6) :
            (uint32_t(p[6]) << 16) | (p[7] << 8) | p[8];
        s.read_fields = p[0] == 0xbe ? p[9] & 0xf8 : 0;
        if (s.read_blocks > UINT32_MAX / 2352 || s.read_lba >= chd_cd_last_lba(disc) ||
            s.read_blocks > chd_cd_last_lba(disc) - s.read_lba) {
            s.ata[7] = 0x41; s.ata[1] = 0x50; s.sense = 5; break;
        }
        s.remaining = 0;
        for (unsigned i = 0; i < s.read_blocks; i++) {
            if (!read_sector(s.read_lba + i)) { s.faults++; s.ata[7] = 0x41; s.sense = 5; s.phase = 0; return; }
            s.remaining += s.sector_bytes;
        }
        s.sector_pos = 0; fill_read(); break;
    default:
        s.faults++; s.sense = 5; s.ata[7] = 0x41; s.ata[1] = 0x50; s.phase = 0;
        std::fprintf(stderr, "CDXA unsupported ATAPI packet %02x\n", p[0]); break;
    }
}
void S12Cdxa::command(uint8_t value) {
    s.ata[7] = (s.ata[7] & 0x10) | 0x40; s.ata[1] = 0; s.phase = 0;
    if (value == 0xa0) {
        put16(s.peripheral + 0xff0, uint16_t(s.ata[4] | (s.ata[5] << 8)));
        s.packet_size = 0; s.phase = 1; s.ata[7] = 0x48; s.ata[2] = 1;
    } else if (value == 0xa1) {
        uint8_t bytes[512]{};
        bytes[0] = 0; bytes[1] = 0x85;
        const char *model = "TOSHIBA CD-ROM XM-6402B                  ";
        for (unsigned i = 0; i < 40; i += 2) { bytes[54 + i] = model[i + 1]; bytes[55 + i] = model[i]; }
        bytes[98] = 0; bytes[99] = 2; response(bytes, sizeof(bytes));
    } else if (value == 0xe5) s.ata[2] = 0xff;
    else if (value != 0x08 && value != 0xef && value != 0xe1) { s.faults++; s.ata[7] = 0x41; s.ata[1] = 4; }
}
uint16_t S12Cdxa::ata_read(unsigned reg) {
    reg &= 7;
    if (reg) return s.ata[reg];
    if (s.phase != 2 || s.data_pos >= s.data_size) return 0;
    uint16_t v = s.data[s.data_pos++];
    if (s.data_pos < s.data_size) v |= uint16_t(s.data[s.data_pos++]) << 8;
    if (s.data_pos == s.data_size) {
        if (s.remaining) fill_read(); else {
            s.phase = 0; s.ata[7] = (s.ata[7] & 0x10) | 0x40; s.ata[2] = 3;
            s.ata[4] = s.ata[5] = 0;
        }
    }
    return v;
}
void S12Cdxa::ata_write(unsigned reg, uint16_t value) {
    reg &= 7;
    if (reg == 7) command(value);
    else if (!reg && s.phase == 1 && s.packet_size < 12) {
        s.packet[s.packet_size++] = value;
        s.packet[s.packet_size++] = value >> 8;
        if (s.packet_size == 12) packet_command();
    } else s.ata[reg] = value;
}
uint32_t S12Cdxa::read(uint32_t a, unsigned width) {
    if (a >= 0x00c00000 && a < 0x00c20000) {
        uint16_t value = ata_read((a - 0x00c00000) >> 14);
        if (width == 1) return a & 1 ? value & 0xff : value >> 8;
        if (width == 4) return (uint32_t(value) << 16) | ata_read((a + 2 - 0x00c00000) >> 14);
        return value;
    }
    if (a >= 0x0041c000 && a < 0x0041c004) return s.irq_busy ? 0 : 0x800;
    const uint8_t *bytes = nullptr;
    if (uint64_t(a) + width <= sizeof(s.cram)) bytes = s.cram + a;
    else if (a >= 0x00400000 && uint64_t(a) + width <= 0x00410000) bytes = s.sram + a - 0x00400000;
    else if (a >= 0xffff8000 && uint64_t(a) + width <= 0xffff9000ULL) {
        if (a >= 0xffff8240 && a < 0xffff82c0) timers(sh2_current_cycle(cpu));
        bytes = s.peripheral + a - 0xffff8000;
    }
    else if (a >= 0xfffff000 && uint64_t(a) + width <= 0x100000000ULL) bytes = s.cache + a - 0xfffff000;
    if (!bytes) { s.faults++; s.last_address = a; return UINT32_MAX; }
    uint32_t v = 0; for (unsigned i = 0; i < width; i++) {
        uint8_t value = bytes[i];
        unsigned offset = a + i - 0xffff8000;
        unsigned port = offset & ~1u;
        if (port == 0x382 || port == 0x390 || port == 0x3b0) {
            unsigned direction = port == 0x382 ? 0x386 : port == 0x390 ? 0x394 : 0x3b4;
            value &= s.peripheral[direction + (offset & 1)];
        }
        if (offset >= 0x260 && offset < 0x2b0 && ((offset - 0x260) % 0x20 == 1 || (offset - 0x260) % 0x20 == 5)) value |= 0xc0;
        v = (v << 8) | value;
    } return v;
}
void S12Cdxa::write(uint32_t a, unsigned width, uint32_t value) {
    if (a >= 0x00c00000 && a < 0x00c20000) {
        if (width == 1 && !(a & 1)) value <<= 8;
        if (width == 4) { ata_write((a - 0x00c00000) >> 14, value >> 16); ata_write((a + 2 - 0x00c00000) >> 14, value); }
        else ata_write((a - 0x00c00000) >> 14, value);
        return;
    }
    if (a >= 0x0041601c && a < 0x00416020) { s.irq_busy = 1; if (irq) irq(opaque, 1); return; }
    if (a == 0x0041c000 || a == 0x00417000 || (a >= 0x00c20000 && a < 0x00c40000)) return;
    uint8_t *bytes = nullptr;
    if (uint64_t(a) + width <= sizeof(s.cram)) bytes = s.cram + a;
    else if (a >= 0x00400000 && uint64_t(a) + width <= 0x00410000) bytes = s.sram + a - 0x00400000;
    else if (a >= 0xffff8000 && uint64_t(a) + width <= 0xffff9000ULL) bytes = s.peripheral + a - 0xffff8000;
    else if (a >= 0xfffff000 && uint64_t(a) + width <= 0x100000000ULL) bytes = s.cache + a - 0xfffff000;
    if (!bytes) { s.faults++; s.last_address = a; s.last_value = value; return; }
    if (a >= 0xffff8240 && a < 0xffff82c0) timers(sh2_current_cycle(cpu));
    for (unsigned i = 0; i < width; i++) {
        uint8_t data = value >> ((width - i - 1) * 8);
        unsigned offset = a + i - 0xffff8000;
        if (offset >= 0x260 && offset < 0x2b0 && (offset - 0x260) % 0x20 == 5) {
            unsigned mask = offset < 0x280 ? 0x1f : 0x33;
            bytes[i] = (data & ~mask) | (bytes[i] & data & mask);
        } else bytes[i] = data;
    }
    if (a >= 0xffff8380 && a < 0xffff83c0) {
        const unsigned registers[][2] = {{0x382,0x83ff},{0x386,0x83ff},{0x38c,0x400f},{0x38e,0xfd75},
            {0x390,0x3ff},{0x394,0x3fc},{0x398,0xf},{0x39a,0xfff5},{0x3b8,0xf000},{0x3ba,0x5555}};
        for (const auto &reg : registers) put16(s.peripheral + reg[0], be16(s.peripheral + reg[0]) & reg[1]);
    }
    if (a >= 0xffff81a0 && a < 0xffff81a6) {
        if (a == 0xffff81a3 && in_dma) s.peripheral[0x1a4] &= ~0x84;
        sci_start();
    }
    if (a >= 0xffff86b0 && a < 0xffff86e0) {
        for (unsigned ch = 0; ch < 2; ch++) {
            uint8_t *r = s.peripheral + 0x6c0 + ch * 16;
            uint32_t control = be32(r + 12);
            if ((control & 3) == 1 && (be16(s.peripheral + 0x6b0) & 7) == 1 && !s.dma_active[ch]) {
                if (!be32(r + 8)) put32(r + 8, 0x10000);
                s.dma_active[ch] = 1; s.dma_next[ch] = sh2_current_cycle(cpu) + 2;
            }
            if (!(control & 1)) s.dma_active[ch] = 0;
        }
    }
}
void S12Cdxa::irq_sh(unsigned vector) {
    unsigned level = 0;
    if (vector == 72 || vector == 76) level = (be16(s.peripheral + 0x34c) >> (vector == 72 ? 12 : 8)) & 15;
    if (vector >= 88 && vector <= 92) level = (be16(s.peripheral + 0x34e) >> (vector == 92 ? 8 : 12)) & 15;
    if (vector >= 96 && vector <= 101) level = (be16(s.peripheral + 0x34e) >> (vector >= 100 ? 0 : 4)) & 15;
    if (vector >= 104 && vector <= 109) level = (be16(s.peripheral + 0x350) >> (vector >= 108 ? 8 : 12)) & 15;
    if (vector >= 128 && vector <= 135) level = (be16(s.peripheral + 0x352) >> (vector <= 131 ? 4 : 0)) & 15;
    if (level) sh2_set_irq(cpu, vector, level);
}
void S12Cdxa::dma_transfer(unsigned ch) {
    uint8_t *r = s.peripheral + 0x6c0 + ch * 16;
    uint32_t control = be32(r + 12), count = be32(r + 8);
    unsigned width = 1u << ((control >> 3) & 3), sm = (control >> 12) & 3, dm = (control >> 14) & 3;
    if (!count || width > 4 || sm == 3 || dm == 3) { s.faults++; s.dma_active[ch] = 0; return; }
    uint32_t src = be32(r), dst = be32(r + 4);
    if (sm == 2) src -= width; if (dm == 2) dst -= width;
    in_dma = true; uint32_t data = read(src, width); write(dst, width, data); in_dma = false;
    if (sm == 1) src += width; if (dm == 1) dst += width;
    put32(r, src); put32(r + 4, dst); put32(r + 8, count - 1); s.dma_transfers++;
    if (count == 1) {
        put32(r + 12, control | 2); s.dma_active[ch] = 0;
        if (control & 4) irq_sh(ch ? 76 : 72);
    }
}
void S12Cdxa::dma(unsigned resource) {
    for (unsigned ch = 0; ch < 2; ch++) {
        uint32_t control = be32(s.peripheral + 0x6cc + ch * 16);
        if (s.dma_active[ch] && ((control >> 8) & 15) == resource) dma_transfer(ch);
    }
}
void S12Cdxa::sci_start() {
    uint8_t *r = s.peripheral + 0x1a0;
    if (!(r[2] & 0x20) || (r[4] & 0x80) || s.sci_busy) return;
    s.sci_shift = r[3]; s.sci_busy = 1; r[4] = (r[4] | 0x80) & ~4;
    s.sci_fraction += CPU_HZ;
    s.sci_next = sh2_current_cycle(cpu) + s.sci_fraction / 151200;
    s.sci_fraction %= 151200;
    if (r[2] & 0x80) {
        bool handled = false;
        for (unsigned ch = 0; ch < 2; ch++) if (s.dma_active[ch] && ((be32(s.peripheral + 0x6cc + ch * 16) >> 8) & 15) == 12) handled = true;
        if (handled) dma(12); else irq_sh(130);
    }
}
void S12Cdxa::timers(uint64_t cycle) {
    for (unsigned ch = 0; ch < 3; ch++) {
        uint8_t *r = s.peripheral + 0x260 + ch * 0x20;
        if (!(s.peripheral[0x240] & (1 << ch))) { s.timer_base[ch] = cycle; continue; }
        unsigned scale = r[0] & 7;
        unsigned divider = scale < 4 ? 1u << (scale * 2) : ch == 1 && scale == 6 ? 256 : ch == 2 && scale == 7 ? 1024 : 0;
        if (!divider) continue; /* external clock selections */
        uint64_t ticks = (cycle - s.timer_base[ch]) / divider;
        if (!ticks) continue;
        s.timer_base[ch] += ticks * divider;
        uint32_t old = be16(r + 6), total = old + ticks;
        unsigned clear = (r[0] >> 5) & 3;
        unsigned period = clear == 1 ? be16(r + 8) : clear == 2 ? be16(r + 10) : 65536;
        if (!period) period = 65536;
        unsigned flags = 0;
        for (unsigned match = 0; match < (ch ? 2u : 4u); match++) {
            unsigned value = be16(r + 8 + match * 2);
            if ((r[4] & (1 << match)) && ((old < value && total >= value) || ticks >= period)) flags |= 1 << match;
        }
        if (clear == 0 && total >= 65536) flags |= 0x10;
        unsigned fresh = flags & ~r[5]; r[5] |= flags;
        put16(r + 6, total % period);
        for (unsigned match = 0; match < 5; match++) if ((fresh & r[4]) & (1 << match))
            irq_sh((ch == 0 ? 88 : ch == 1 ? 96 : 104) + match);
    }
}
void S12Cdxa::tick(uint64_t cycle) {
    timers(cycle);
    for (unsigned ch = 0; ch < 2; ch++) {
        unsigned resource = (be32(s.peripheral + 0x6cc + ch * 16) >> 8) & 15;
        while (s.dma_active[ch] && (resource == 0 || resource == 2 || resource == 3 || resource == 4) && s.dma_next[ch] <= cycle) {
            dma_transfer(ch); s.dma_next[ch] += 2;
        }
    }
    while (s.sci_busy && s.sci_next <= cycle) {
        uint64_t when = s.sci_next;
        uint8_t byte = s.sci_shift;
        if (!(s.audio_phase & 1)) s.audio_high = byte;
        else s.pcm[s.audio_phase / 2] = uint16_t(s.audio_high | (uint16_t(byte) << 8));
        if (++s.audio_phase == 4) {
            s.audio_phase = 0; unsigned next = (s.audio_head + 1) % 4096;
            if (next == s.audio_tail) s.faults++;
            else { s.audio[s.audio_head] = {uint64_t((__uint128_t(when) * 88200) / CPU_HZ), s.pcm[0], s.pcm[1]}; s.audio_head = next; }
            s.audio_frames++;
        }
        s.sci_busy = 0; sci_start();
        if (!s.sci_busy) { s.peripheral[0x1a4] |= 4; if (s.peripheral[0x1a2] & 4) irq_sh(131); }
    }
}

extern "C" S12Cdxa *s12_cdxa_create(const char *path, const char *sha1,
    s12_cdxa_irq_cb irq, void *opaque, char *error, size_t error_size) {
    auto *d = new(std::nothrow) S12Cdxa;
    if (!d) return nullptr;
    d->disc = chd_cd_open(path, nullptr, error, error_size);
    if (!d->disc || !chd_cd_verify_sha1(d->disc, sha1)) {
        if (d->disc && error && error_size) std::snprintf(error, error_size, "CHD SHA1 differs from the pinned game disc");
        delete d; return nullptr;
    }
    chd_cd_get_media_ops(d->disc, &d->media);
    d->irq = irq; d->opaque = opaque;
    Sh2Config config = {d, cpu_read, cpu_write, nullptr};
    d->cpu = sh2_create(&config);
    if (!d->cpu) { delete d; return nullptr; }
    s12_cdxa_reset(d, 0); return d;
}
extern "C" void s12_cdxa_destroy(S12Cdxa *d) { delete d; }
extern "C" void s12_cdxa_reset(S12Cdxa *d, uint64_t cycle) {
    if (!d) return;
    d->s.running = d->s.ide_sh = d->s.ide_ps = d->s.sram_enabled = d->s.irq_busy = 0;
    d->s.cycles = cycle; d->s.phase = d->s.remaining = 0;
    std::memset(d->s.peripheral, 0, sizeof(d->s.peripheral));
    d->s.peripheral[0x1a1] = d->s.peripheral[0x1b1] = 0xff;
    d->s.peripheral[0x1a3] = d->s.peripheral[0x1b3] = 0xff;
    d->s.peripheral[0x1a4] = d->s.peripheral[0x1b4] = 0x84;
    for (unsigned ch = 0; ch < 3; ch++) {
        d->s.timer_base[ch] = cycle;
    }
    d->s.sci_busy = 0; d->s.dma_active[0] = d->s.dma_active[1] = 0;
    d->s.audio_head = d->s.audio_tail = d->s.audio_phase = 0;
    d->s.output[0] = d->s.output[1] = 0;
    d->s.ata[7] = 0x40; d->s.ata[2] = d->s.ata[3] = 1;
    d->s.ata[4] = 0x14; d->s.ata[5] = 0xeb;
    sh2_reset(d->cpu, cycle);
}
extern "C" void s12_cdxa_run_until(S12Cdxa *d, uint64_t target) {
    if (!d || target < d->s.cycles) return;
    if (!d->s.running) { d->s.cycles = target; return; }
    while (sh2_current_cycle(d->cpu) < target) {
        uint64_t next = std::min(target, sh2_current_cycle(d->cpu) + 64);
        if (d->s.sci_busy) next = std::min(next, d->s.sci_next);
        for (unsigned ch = 0; ch < 2; ch++)
            if (d->s.dma_active[ch]) {
                unsigned resource = (be32(d->s.peripheral + 0x6cc + ch * 16) >> 8) & 15;
                if (resource == 0 || resource == 2 || resource == 3 || resource == 4)
                    next = std::min(next, d->s.dma_next[ch]);
            }
        if (next <= sh2_current_cycle(d->cpu)) { d->tick(sh2_current_cycle(d->cpu)); continue; }
        sh2_run_until(d->cpu, next); d->tick(sh2_current_cycle(d->cpu));
    }
    d->s.cycles = target;
}
extern "C" int s12_cdxa_read(S12Cdxa *d, uint32_t a, unsigned width, uint32_t *out) {
    if (!d) return 0;
    uint32_t v = 0;
    if (a >= 0x1f7c0000 && a + width <= 0x1f7d0000) {
        const uint8_t *p = d->s.sram_enabled ? d->s.sram : d->s.cram;
        for (unsigned i = 0; i < width; i++) v |= uint32_t(p[((a + i) - 0x1f7c0000) ^ 1]) << (i * 8);
    } else if (a >= 0x1f7e0000 && a + width <= 0x1f7e0010) {
        for (unsigned i = 0; i < width; i += 2) v |= uint32_t(d->ata_read((a + i - 0x1f7e0000) / 2)) << (i * 8);
    } else if ((a & ~1u) == 0x1f7d800a) v = (a & 1) ? 0 : 0x20;
    else return 0;
    *out = v; return 1;
}
extern "C" int s12_cdxa_write(S12Cdxa *d, uint32_t a, unsigned width, uint32_t value) {
    if (!d) return 0;
    if (a >= 0x1f7c0000 && a + width <= 0x1f7d0000) {
        for (unsigned i = 0; i < width; i++) {
            unsigned offset = ((a + i) - 0x1f7c0000) ^ 1;
            if (!d->s.sram_enabled) d->s.cram[offset] = value >> (i * 8);
            d->s.sram[offset] = value >> (i * 8);
        }
        return 1;
    }
    if (a >= 0x1f7e0000 && a + width <= 0x1f7e0010) {
        for (unsigned i = 0; i < width; i += 2) d->ata_write((a + i - 0x1f7e0000) / 2, value >> (i * 8));
        return 1;
    }
    if (a >= 0x1f7f8000 && a < 0x1f7f8100) return 1;
    if (a >= 0x1f7d6000 && a + width <= 0x1f7d6020) {
        for (unsigned i = 0; i < width; i += 2) {
            unsigned reg = (a + i) & ~1u; uint16_t data = value >> (i * 8);
            if (reg == 0x1f7d6002 && data == 1) { sh2_reset(d->cpu, d->s.cycles); d->s.running = 1; }
            if (reg == 0x1f7d6010) d->s.ide_sh = data != 0;
            if (reg == 0x1f7d6012) d->s.ide_ps = data != 0;
            if (reg == 0x1f7d601a) d->s.sram_enabled = data != 0;
            if (reg == 0x1f7d601e) { d->s.irq_busy = 0; if (d->irq) d->irq(d->opaque, 0); }
        }
        return 1;
    }
    return 0;
}
extern "C" void s12_cdxa_sample(S12Cdxa *d, uint64_t frame, int16_t stereo[2]) {
    stereo[0] = stereo[1] = 0;
    if (!d) return;
    while (d->s.audio_tail != d->s.audio_head && d->s.audio[d->s.audio_tail].frame <= frame) {
        auto &sample = d->s.audio[d->s.audio_tail];
        d->s.output[0] = sample.left; d->s.output[1] = sample.right;
        d->s.audio_tail = (d->s.audio_tail + 1) % 4096;
    }
    if (!(be16(d->s.peripheral + 0x390) & 0x40)) {
        stereo[0] = d->s.output[0] * 3 / 10;
        stereo[1] = d->s.output[1] * 3 / 10;
    }
}
extern "C" void s12_cdxa_report(S12Cdxa *d, FILE *out) {
    if (!d) return;
    Sh2State cpu; sh2_get_state(d->cpu, &cpu);
    const char *dump = std::getenv("PCSX_CDXA_DUMP");
    if (dump) {
        FILE *f = std::fopen(dump, "wb");
        if (f) { std::fwrite(&d->s, 1, sizeof(d->s), f); std::fclose(f); }
    }
    std::fprintf(out, "System12: CDXA running=%u pc=%08x sr=%08x cycles=%" PRIu64
        " instructions=%" PRIu64 " illegal=%08x:%04x ATA_commands=%" PRIu64
        " sectors=%" PRIu64 " faults=%" PRIu64 " last=%08x:%08x audio_frames=%" PRIu64 " irq_busy=%u ATA=%02x phase=%u pending_bytes=%u\n",
        d->s.running, cpu.pc, cpu.sr, cpu.cycles, cpu.instructions, cpu.illegal_pc, cpu.illegal_opcode,
        d->s.commands, d->s.sectors, d->s.faults, d->s.last_address, d->s.last_value, d->s.audio_frames,
        d->s.irq_busy, d->s.ata[7], d->s.phase, d->s.remaining);
    std::fprintf(out, "System12: CDXA DMA0=%08x:%08x:%08x:%08x DMA1=%08x:%08x:%08x:%08x SCI=%02x:%02x:%02x r0=%08x r1=%08x r2=%08x irq=%u/%u\n",
        be32(d->s.peripheral+0x6c0),be32(d->s.peripheral+0x6c4),be32(d->s.peripheral+0x6c8),be32(d->s.peripheral+0x6cc),
        be32(d->s.peripheral+0x6d0),be32(d->s.peripheral+0x6d4),be32(d->s.peripheral+0x6d8),be32(d->s.peripheral+0x6dc),
        d->s.peripheral[0x1a0],d->s.peripheral[0x1a2],d->s.peripheral[0x1a4],cpu.r[0],cpu.r[1],cpu.r[2],cpu.irq_vector,cpu.irq_level);
}
extern "C" size_t s12_cdxa_state_size() { return sizeof(CdxaState); }
extern "C" void s12_cdxa_get_state(S12Cdxa *d, void *out) {
    if (d && out) { sh2_get_state(d->cpu, &d->s.cpu); std::memcpy(out, &d->s, sizeof(d->s)); }
}
extern "C" void s12_cdxa_set_state(S12Cdxa *d, const void *in) {
    if (d && in) { std::memcpy(&d->s, in, sizeof(d->s)); sh2_set_state(d->cpu, &d->s.cpu); }
}
