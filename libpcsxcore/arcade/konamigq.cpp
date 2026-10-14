/* Standalone Konami GQ common hardware adapter.
 * MAME source revision deb2f54fe7300e7a192690318beb860159bfd52a.
 * SPDX-License-Identifier: BSD-3-Clause; see konamigq_LICENSE.txt.
 */
#include "konamigq.h"

#include "eeprom93c46.h"
#include "k054539.h"
#include "k056800.h"
#include "m68000.h"
#include "m68000/m68000_compat.h"
#include "mb89371.h"
#include "nscsi_bus.h"
#include "nscsi_target.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace {
constexpr uint32_t kNcrHz = 16000000;
constexpr uint32_t kSoundHz = 8000000;
constexpr uint32_t kDspHz = 24000000;
constexpr uint32_t kDuartHz = 4000000;
constexpr uint32_t kK054Hz = 18432000;
constexpr uint64_t kDspSyncCycles = 500;
constexpr uint32_t kPcmMapStart = 0x1f300000;
constexpr uint32_t kPcmMapBytes = 0x300000;
constexpr uint32_t kNcrAddress = 0x1f000000;
constexpr uint32_t kMiracAddress = 0x1f100000;
constexpr uint32_t kEepromAddress = 0x1f180000;
constexpr uint32_t kDuartAddress = 0x1f680000;
constexpr uint32_t kSoundRamStart = 0x100000;
constexpr uint32_t kK054Address = 0x200000;
constexpr uint32_t kDspDataAddress = 0x300001;
constexpr uint32_t kMiracSoundAddress = 0x400000;
constexpr uint32_t kDspStatusAddress = 0x500000;
constexpr uint32_t kNresAddress = 0x580000;
constexpr uint32_t kSoundAddrMask = 0x00ffffff;

const uint16_t kDefaultEeprom[64] = {
    0x292b, 0x5256, 0x2094, 0x4155, 0x0041, 0x1414, 0x0003, 0x0101,
    0x0103, 0x0000, 0x0707, 0x0001, 0xaa00, 0xaaaa, 0xaaaa, 0xaaaa,
    0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa,
    0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa,
    0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa,
    0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa,
    0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa,
    0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa};

uint64_t muldiv_floor(uint64_t value, uint64_t multiplier, uint64_t divisor,
                      bool *overflow = nullptr) {
  const uint64_t quotient = value / divisor;
  const uint64_t remainder = value % divisor;
  if (quotient && multiplier > UINT64_MAX / quotient) {
    if (overflow)
      *overflow = true;
    return UINT64_MAX;
  }
  const uint64_t whole = quotient * multiplier;
  // Board clock ratios keep this remainder product below UINT64_MAX.
  const uint64_t fraction = remainder * multiplier / divisor;
  if (fraction > UINT64_MAX - whole) {
    if (overflow)
      *overflow = true;
    return UINT64_MAX;
  }
  return whole + fraction;
}
uint64_t muldiv_ceil(uint64_t value, uint64_t multiplier, uint64_t divisor,
                     bool *overflow = nullptr) {
  const uint64_t quotient = value / divisor;
  const uint64_t remainder = value % divisor;
  if (quotient && multiplier > UINT64_MAX / quotient) {
    if (overflow)
      *overflow = true;
    return UINT64_MAX;
  }
  const uint64_t whole = quotient * multiplier;
  // Board clock ratios keep this remainder product below UINT64_MAX.
  const uint64_t product = remainder * multiplier;
  const uint64_t fraction = product / divisor + (product % divisor != 0);
  if (fraction > UINT64_MAX - whole) {
    if (overflow)
      *overflow = true;
    return UINT64_MAX;
  }
  return whole + fraction;
}
} // namespace

struct konamigq {
  konamigq_config_t config{};
  NscsiBus *bus = nullptr;
  nscsi_hd_t *hd = nullptr;
  Ncr53cf96 *ncr = nullptr;
  mb89371_t *duart = nullptr;
  EEPROM93C46 *eeprom = nullptr;
  tms57002_t *dsp = nullptr;
  K056800 mirac{};
  K054539 *pcm[2]{};
  std::unique_ptr<M68000> sound_cpu;
  std::vector<uint8_t> sound_ram =
      std::vector<uint8_t>(KONAMIGQ_SOUNDRAM_BYTES);
  std::vector<uint8_t> pcm_ram = std::vector<uint8_t>(KONAMIGQ_PCMRAM_BYTES);
  std::vector<uint8_t> dsp_ram = std::vector<uint8_t>(KONAMIGQ_TMSRAM_BYTES);
  std::array<uint8_t, KONAMIGQ_EEPROM_BYTES> default_eeprom{};
  uint64_t now_ns = 0;
  uint64_t sound_epoch_ns = 0;
  uint64_t sound_epoch_cycle = 0;
  uint64_t dsp_epoch_ns = 0;
  uint64_t dsp_epoch_cycle = 0;
  uint64_t dsp_sync_cycle = kDspSyncCycles;
  uint64_t dma_address = 0;
  uint64_t dma_remaining = 0;
  uint32_t own_ctrl = 0, own_data = 0, wait_ctrl = 0;
  uint8_t sound_ctrl = 0;
  uint8_t sound_intck = 0;
  bool sound_cpu_reset = true;
  bool dsp_reset = true;
  bool drq = false;
  bool dma_active = false;
  bool dma_write = false;
  bool busy = false;
  uint32_t faults = 0;

  uint64_t sound_cycle_access() {
    uint64_t cycle = 0;
    if (sound_cpu && sound_cpu->current_cycle(cycle))
      return cycle;
    faults |= KONAMIGQ_FAULT_TIME_OVERFLOW;
    return sound_cpu ? sound_cpu->total_cycles() : 0;
  }
  uint64_t sound_access_time_ns() {
    const uint64_t cycle = sound_cycle_access();
    if (cycle < sound_epoch_cycle)
      return sound_epoch_ns;
    bool overflow = false;
    const uint64_t delta =
        muldiv_ceil(cycle - sound_epoch_cycle, 1000000000, kSoundHz, &overflow);
    if (overflow || delta > UINT64_MAX - sound_epoch_ns) {
      faults |= KONAMIGQ_FAULT_TIME_OVERFLOW;
      return UINT64_MAX;
    }
    return sound_epoch_ns + delta;
  }
};

namespace {

bool enter(konamigq *b) {
  if (!b || b->busy) {
    if (b)
      b->faults |= KONAMIGQ_FAULT_REENTRANT;
    return false;
  }
  b->busy = true;
  return true;
}
void leave(konamigq *b) { b->busy = false; }

int media_read(void *opaque, uint32_t lba, uint8_t *sector, size_t bytes) {
  auto *b = static_cast<konamigq *>(opaque);
  if (!enter(b))
    return 0;
  const auto &media = b->config.hard_disk;
  const int result = media.read_sector(media.media_opaque, lba, sector, bytes);
  leave(b);
  return result;
}
int media_write(void *opaque, uint32_t lba, const uint8_t *sector, size_t bytes) {
  auto *b = static_cast<konamigq *>(opaque);
  if (!enter(b))
    return 0;
  const auto &media = b->config.hard_disk;
  const int result = media.write_sector(media.media_opaque, lba, sector, bytes);
  leave(b);
  return result;
}

void irq10(void *opaque, int asserted) {
  auto *b = static_cast<konamigq *>(opaque);
  if (b->config.psx_irq10 && enter(b)) {
    b->config.psx_irq10(b->config.opaque, asserted);
    leave(b);
  }
}
void irq_sound(konamigq *b, unsigned level, int asserted) {
  if (b->config.sound_irq && enter(b)) {
    b->config.sound_irq(b->config.opaque, level, asserted);
    leave(b);
  }
}
void mirac_irq(void *opaque, int asserted) {
  auto *b = static_cast<konamigq *>(opaque);
  // konamigq.cpp connects K056800 INT directly to the sound CPU's IRQ1.
  // The observer is optional and must not replace the actual CPU input line.
  b->sound_cpu->set_irq(1, asserted != 0);
  irq_sound(b, 1, asserted);
}
void pcm_timer(void *opaque, uint64_t cycle, int state) {
  auto *b = static_cast<konamigq *>(opaque);
  if ((b->sound_ctrl & 1) && !b->sound_intck && state)
    b->sound_cpu->set_irq(2, true);
  b->sound_intck = static_cast<uint8_t>(state != 0);
  (void)cycle;
}
void pcm_sample(void *opaque, uint64_t cycle, int32_t left, int32_t right) {
  auto *b = static_cast<konamigq *>(opaque);
  if (b->config.k054539_audio && enter(b)) {
    b->config.k054539_audio(b->config.opaque, 0, cycle, left, right);
    leave(b);
  }
}
void pcm_sample1(void *opaque, uint64_t cycle, int32_t left, int32_t right) {
  auto *b = static_cast<konamigq *>(opaque);
  if (b->config.k054539_audio && enter(b)) {
    b->config.k054539_audio(b->config.opaque, 1, cycle, left, right);
    leave(b);
  }
}
uint8_t sample_read(void *opaque, uint32_t address) {
  auto *b = static_cast<konamigq *>(opaque);
  if (address < KONAMIGQ_K054539_ROM_BYTES)
    return b->config.k054539_rom[address];
  const uint32_t offset = address - KONAMIGQ_K054539_ROM_BYTES;
  if (offset < b->pcm_ram.size())
    return b->pcm_ram[offset];
  b->faults |= KONAMIGQ_FAULT_PCMRAM_RANGE;
  return 0xff;
}
uint8_t dsp_read(void *opaque, uint32_t address) {
  auto *b = static_cast<konamigq *>(opaque);
  return address < b->dsp_ram.size() ? b->dsp_ram[address] : 0xff;
}
void dsp_write(void *opaque, uint32_t address, uint8_t value) {
  auto *b = static_cast<konamigq *>(opaque);
  if (address < b->dsp_ram.size())
    b->dsp_ram[address] = value;
}
void dsp_line_pc0(void *, int) {}
void dsp_line_empty(void *, int) {}
#define KONAMIGQ_DUART_CB(Name, Member, Channel)                               \
  void Name(void *opaque, int level) {                                         \
    auto *b = static_cast<konamigq *>(opaque);                                 \
    const auto callback = b->config.Member[Channel];                           \
    if (callback && enter(b)) {                                                \
      callback(b->config.opaque, Channel, level);                              \
      leave(b);                                                                \
    }                                                                          \
  }
KONAMIGQ_DUART_CB(duart_txd0, uart_txd, 0)
KONAMIGQ_DUART_CB(duart_txd1, uart_txd, 1)
KONAMIGQ_DUART_CB(duart_dtr0, uart_dtr, 0)
KONAMIGQ_DUART_CB(duart_dtr1, uart_dtr, 1)
KONAMIGQ_DUART_CB(duart_rts0, uart_rts, 0)
KONAMIGQ_DUART_CB(duart_rts1, uart_rts, 1)
KONAMIGQ_DUART_CB(duart_rxready0, uart_rxready, 0)
KONAMIGQ_DUART_CB(duart_rxready1, uart_rxready, 1)
KONAMIGQ_DUART_CB(duart_txready0, uart_txready, 0)
KONAMIGQ_DUART_CB(duart_txready1, uart_txready, 1)
KONAMIGQ_DUART_CB(duart_txempty0, uart_txempty, 0)
KONAMIGQ_DUART_CB(duart_txempty1, uart_txempty, 1)
KONAMIGQ_DUART_CB(duart_syndet0, uart_syndet, 0)
KONAMIGQ_DUART_CB(duart_syndet1, uart_syndet, 1)
#undef KONAMIGQ_DUART_CB
void dsp_line_dready(void *, int) {}

void bus_changed(void *opaque, uint32_t ctrl, uint32_t data) {
  auto *b = static_cast<konamigq *>(opaque);
  b->own_ctrl = ctrl;
  b->own_data = data;
  ncr53cf96_bus_changed(b->ncr, ctrl, data);
}
void ncr_drive_ctrl(void *opaque, uint32_t value, uint32_t mask) {
  auto *b = static_cast<konamigq *>(opaque);
  b->own_ctrl = (b->own_ctrl & ~mask) | (value & mask);
  nscsi_bus_drive_ctrl(b->bus, 7, value, mask);
}
void ncr_drive_data(void *opaque, uint32_t value) {
  auto *b = static_cast<konamigq *>(opaque);
  b->own_data = value;
  nscsi_bus_drive_data(b->bus, 7, value);
}
void ncr_wait_ctrl(void *opaque, uint32_t value, uint32_t mask) {
  auto *b = static_cast<konamigq *>(opaque);
  b->wait_ctrl = (b->wait_ctrl & ~mask) | (value & mask);
  nscsi_bus_wait_ctrl(b->bus, 7, value, mask);
}
void ncr_irq(void *opaque, int asserted) { irq10(opaque, asserted); }
void ncr_drq(void *opaque, int state) {
  auto *b = static_cast<konamigq *>(opaque);
  const bool next = state != 0;
  if (!b->drq && next && b->dma_active)
    b->drq = true;
  else
    b->drq = next;
}

int flush_bus(konamigq *b) {
  if (!nscsi_bus_flush(b->bus)) {
    b->faults |= KONAMIGQ_FAULT_BUS;
    return 0;
  }
  return 1;
}

uint64_t sound_cycles_at_ns(uint64_t ns, bool *overflow = nullptr) {
  return muldiv_floor(ns, kSoundHz, 1000000000, overflow);
}
uint64_t dsp_cycles_at_ns(uint64_t ns, bool *overflow = nullptr) {
  return muldiv_floor(ns, kDspHz, 1000000000, overflow);
}
uint64_t pcm_cycles_at_ns(uint64_t ns, bool *overflow = nullptr) {
  return muldiv_floor(ns, kK054Hz, 1000000000, overflow);
}
uint64_t pcm_cycle_to_ns(uint64_t cycle, bool *overflow = nullptr) {
  return muldiv_ceil(cycle, 1000000000, kK054Hz, overflow);
}
uint64_t sound_target_cycle(konamigq *b, uint64_t ns) {
  return b->sound_cpu_reset || ns <= b->sound_epoch_ns
             ? b->sound_epoch_cycle
             : b->sound_epoch_cycle +
                   sound_cycles_at_ns(ns - b->sound_epoch_ns);
}
uint64_t dsp_target_cycle(konamigq *b, uint64_t ns) {
  return b->dsp_reset || ns <= b->dsp_epoch_ns
             ? b->dsp_epoch_cycle
             : b->dsp_epoch_cycle + dsp_cycles_at_ns(ns - b->dsp_epoch_ns);
}
void sync_pcm_cycle(konamigq *b, uint64_t cycle) {
  for (unsigned i = 0; i < 2; ++i) {
    const uint64_t current = k054539_cycles(b->pcm[i]);
    if (cycle > current &&
        k054539_advance_cycles(b->pcm[i], cycle - current,
                               i == 0 ? pcm_sample : pcm_sample1, b) != 0)
      b->faults |= KONAMIGQ_FAULT_TIME_OVERFLOW;
  }
}
void sync_pcm_for_sound_access(konamigq *b) {
  const uint64_t access_ns = b->sound_access_time_ns();
  if (b->faults & KONAMIGQ_FAULT_TIME_OVERFLOW)
    return;
  sync_pcm_cycle(b, pcm_cycles_at_ns(access_ns));
}

void tms_sync(konamigq *b, uint64_t dsp_cycle) {
  if (b->sound_ctrl & 0x20)
    tms57002_sync_w(b->dsp, 1);
  (void)dsp_cycle;
}

void dma_pump(konamigq *b) {
  if (!b->dma_active || !b->drq || !b->dma_remaining)
    return;
  const uint64_t budget = b->dma_remaining;
  uint64_t transferred = 0;
  while (b->dma_active && b->drq && b->dma_remaining && transferred < budget) {
    uint8_t byte;
    if (b->dma_write) {
      if (!enter(b))
        return;
      byte = b->config.read_psx_ram8(b->config.opaque,
                                     static_cast<uint32_t>(b->dma_address));
      leave(b);
      ncr53cf96_dma_write(b->ncr, byte);
    } else {
      byte = ncr53cf96_dma_read(b->ncr);
      if (!enter(b))
        return;
      b->config.write_psx_ram8(b->config.opaque,
                               static_cast<uint32_t>(b->dma_address), byte);
      leave(b);
    }
    ++b->dma_address;
    --b->dma_remaining;
    ++transferred;
    if (!flush_bus(b))
      return;
    ncr53cf96_status_t state{};
    ncr53cf96_get_status(b->ncr, &state);
    b->drq = state.drq != 0;
    if (!b->dma_remaining)
      b->dma_active = false;
  }
}

void pump_sound_cpu_to_cycle(konamigq *b, uint64_t target_cycle) {
  if (b->sound_cpu_reset || target_cycle <= b->sound_cpu->total_cycles())
    return;
  uint64_t remaining = target_cycle - b->sound_cpu->total_cycles();
  while (remaining) {
    const uint32_t chunk =
        static_cast<uint32_t>(std::min<uint64_t>(remaining, 0x7fffffff));
    if (b->sound_cpu->run(chunk) < 0) {
      b->faults |= KONAMIGQ_FAULT_SOUND_CPU;
      return;
    }
    remaining -= chunk;
  }
}

uint16_t status_word(konamigq *b) {
  return static_cast<uint16_t>((tms57002_dready_r(b->dsp) ? 4 : 0) |
                               (tms57002_pc0_r(b->dsp) ? 2 : 0) |
                               (tms57002_empty_r(b->dsp) ? 1 : 0));
}
void control_word_write(konamigq *b, uint16_t data, uint16_t mem_mask) {
  if (!(mem_mask & 0x00ff))
    return;
  if (!(data & 1))
    b->sound_cpu->set_irq(2, false);
  tms57002_pload_w(b->dsp, data & 4);
  tms57002_cload_w(b->dsp, data & 8);
  const bool next_reset = !(data & 0x10);
  if (next_reset && !b->dsp_reset)
    tms57002_reset(b->dsp);
  if (!next_reset && b->dsp_reset) {
    b->dsp_epoch_ns = b->sound_access_time_ns();
    if (b->faults & KONAMIGQ_FAULT_TIME_OVERFLOW)
      return;
    b->dsp_epoch_cycle = tms57002_total_cycles(b->dsp);
    b->dsp_sync_cycle = b->dsp_epoch_cycle + kDspSyncCycles;
  }
  b->dsp_reset = next_reset;
  b->sound_ctrl = static_cast<uint8_t>(data);
}

uint8_t sound_byte_read(konamigq *b, uint32_t address, M68kSpace space) {
  address &= kSoundAddrMask;
  if (address < KONAMIGQ_SOUND_ROM_BYTES)
    return b->config.sound_rom[address];
  if (address >= kSoundRamStart &&
      address < kSoundRamStart + b->sound_ram.size())
    return b->sound_ram[address - kSoundRamStart];
  if (address >= kK054Address && address < kK054Address + 0x500) {
    sync_pcm_for_sound_access(b);
    const uint32_t offset = (address - kK054Address) >> 1;
    const unsigned chip = address & 1 ? 1 : 0;
    return k054539_read(b->pcm[chip], offset, 0);
  }
  if (address == kDspDataAddress)
    return tms57002_data_r(b->dsp);
  if (address >= kMiracSoundAddress && address < kMiracSoundAddress + 0x20 &&
      (address & 1))
    return k056800_sound_read(&b->mirac, (address - kMiracSoundAddress) >> 1);
  if (address == kDspStatusAddress + 1)
    return static_cast<uint8_t>(status_word(b));
  (void)space;
  return 0;
}
void sound_byte_write(konamigq *b, uint32_t address, uint8_t value) {
  address &= kSoundAddrMask;
  if (address >= kSoundRamStart &&
      address < kSoundRamStart + b->sound_ram.size()) {
    b->sound_ram[address - kSoundRamStart] = value;
    return;
  }
  if (address >= kK054Address && address < kK054Address + 0x500) {
    sync_pcm_for_sound_access(b);
    const uint32_t offset = (address - kK054Address) >> 1;
    const unsigned chip = address & 1 ? 1 : 0;
    if (k054539_write(b->pcm[chip], offset, value) != 0)
      b->faults |= KONAMIGQ_FAULT_TIME_OVERFLOW;
    return;
  }
  if (address == kDspDataAddress) {
    tms57002_data_w(b->dsp, value);
    return;
  }
  if (address >= kMiracSoundAddress && address < kMiracSoundAddress + 0x20 &&
      (address & 1)) {
    if (k056800_sound_write(&b->mirac, (address - kMiracSoundAddress) >> 1,
                            value) != 0)
      b->faults |= KONAMIGQ_FAULT_REENTRANT;
    return;
  }
  if (address == kDspStatusAddress + 1) {
    control_word_write(b, value, 0x00ff);
    return;
  }
  (void)kNresAddress;
}

bool m68k_read(void *opaque, M68kSpace space, uint32_t address,
               uint16_t mem_mask, uint16_t *value) {
  auto *b = static_cast<konamigq *>(opaque);
  const uint32_t aligned = address & ~1u;
  if (aligned == kDspStatusAddress) {
    *value = status_word(b);
    return true;
  }
  uint16_t out = 0xffff;
  if (mem_mask & 0xff00)
    out = static_cast<uint16_t>(
        (out & 0x00ff) | (uint16_t(sound_byte_read(b, aligned, space)) << 8));
  if (mem_mask & 0x00ff)
    out = static_cast<uint16_t>((out & 0xff00) |
                                sound_byte_read(b, aligned + 1, space));
  *value = out;
  return true;
}
bool m68k_write(void *opaque, M68kSpace, uint32_t address, uint16_t data,
                uint16_t mem_mask) {
  auto *b = static_cast<konamigq *>(opaque);
  const uint32_t aligned = address & ~1u;
  if (aligned == kDspStatusAddress) {
    control_word_write(b, data, mem_mask);
    return true;
  }
  if (aligned == kNresAddress)
    return true;
  if (mem_mask & 0xff00)
    sound_byte_write(b, aligned, static_cast<uint8_t>(data >> 8));
  if (mem_mask & 0x00ff)
    sound_byte_write(b, aligned + 1, static_cast<uint8_t>(data));
  return true;
}

uint32_t read_input(konamigq *b, unsigned port) {
  if (b->config.read_input && enter(b)) {
    const uint32_t value = b->config.read_input(b->config.opaque, port);
    leave(b);
    return value;
  }
  return UINT32_MAX;
}
/* Both pinned CPU program maps retain MAME default-zero unmapped reads. */
uint8_t open_bus() { return 0; }
bool selected(uint32_t mask, unsigned lane) {
  return (mask & (0xffu << (lane * 8))) != 0;
}
uint32_t mapped_input(konamigq *b, uint32_t address) {
  switch (address & ~3u) {
  case 0x1f200000:
    return read_input(b, KONAMIGQ_PORT_GUNX1);
  case 0x1f208000:
    return read_input(b, KONAMIGQ_PORT_GUNY1);
  case 0x1f210000:
    return read_input(b, KONAMIGQ_PORT_GUNX2);
  case 0x1f218000:
    return read_input(b, KONAMIGQ_PORT_GUNY2);
  case 0x1f220000:
    return read_input(b, KONAMIGQ_PORT_GUNX3);
  case 0x1f228000:
    return read_input(b, KONAMIGQ_PORT_GUNY3);
  case 0x1f230000:
    return read_input(b, KONAMIGQ_PORT_P1_P2);
  case 0x1f230004:
    return read_input(b, KONAMIGQ_PORT_P3_SERVICE);
  case 0x1f238000: {
    const uint32_t switches = read_input(b, KONAMIGQ_PORT_DSW);
    const uint32_t eeprom_do = eeprom93c46_do(b->eeprom) ? 0x00010000 : 0;
    return (switches & ~0x00010000u) | eeprom_do;
  }
  default:
    return 0;
  }
}

uint64_t ncr_cycles_to_ns(uint64_t cycle, bool *overflow) {
  return muldiv_ceil(cycle, 1000000000, kNcrHz, overflow);
}
uint64_t dsp_cycle_to_ns(konamigq *b, uint64_t cycle, bool *overflow) {
  if (cycle < b->dsp_epoch_cycle)
    return b->dsp_epoch_ns;
  const uint64_t delta =
      muldiv_ceil(cycle - b->dsp_epoch_cycle, 1000000000, kDspHz, overflow);
  if (delta > UINT64_MAX - b->dsp_epoch_ns) {
    *overflow = true;
    return UINT64_MAX;
  }
  return b->dsp_epoch_ns + delta;
}
uint64_t next_deadline_ns(konamigq *b, uint64_t horizon) {
  uint64_t due = horizon;
  ncr53cf96_status_t ncr{};
  ncr53cf96_get_status(b->ncr, &ncr);
  if (ncr.timer_deadline != UINT64_MAX) {
    bool overflow = false;
    const uint64_t candidate = ncr_cycles_to_ns(ncr.timer_deadline, &overflow);
    if (!overflow && candidate < due)
      due = candidate;
  }
  nscsi_target_status_t target{};
  nscsi_target_get_status(nscsi_hd_target(b->hd), &target);
  if (target.timer_deadline_ns != UINT64_MAX && target.timer_deadline_ns < due)
    due = target.timer_deadline_ns;
  uint64_t duart_due = UINT64_MAX;
  if (mb89371_next_event_ns(b->duart, &duart_due) && duart_due < due)
    due = duart_due;
  if (!b->dsp_reset) {
    bool overflow = false;
    const uint64_t candidate = dsp_cycle_to_ns(b, b->dsp_sync_cycle, &overflow);
    if (!overflow && candidate < due)
      due = candidate;
  }
  const uint64_t pcm_cycle = k054539_cycles(b->pcm[0]);
  const uint64_t next_sample =
      (pcm_cycle / K054539_CLOCK_DIVIDER + 1) * K054539_CLOCK_DIVIDER;
  bool overflow = false;
  const uint64_t sample_due =
      muldiv_ceil(next_sample, 1000000000, kK054Hz, &overflow);
  if (!overflow && sample_due < due)
    due = sample_due;
  for (unsigned i = 0; i < 2; ++i) {
    uint64_t timer_cycle = UINT64_MAX;
    if (k054539_next_timer_cycle(b->pcm[i], &timer_cycle)) {
      overflow = false;
      const uint64_t timer_due = pcm_cycle_to_ns(timer_cycle, &overflow);
      if (!overflow && timer_due < due)
        due = timer_due;
    }
  }
  return due;
}
void advance_dsp_to(konamigq *b, uint64_t now_ns) {
  if (b->dsp_reset)
    return;
  const uint64_t target = dsp_target_cycle(b, now_ns);
  while (b->dsp_sync_cycle <= target) {
    const uint64_t current = tms57002_total_cycles(b->dsp);
    if (b->dsp_sync_cycle > current &&
        tms57002_run(b->dsp, b->dsp_sync_cycle - current) < 0)
      b->faults |= KONAMIGQ_FAULT_DSP;
    tms_sync(b, b->dsp_sync_cycle);
    if (b->dsp_sync_cycle > UINT64_MAX - kDspSyncCycles) {
      b->faults |= KONAMIGQ_FAULT_TIME_OVERFLOW;
      return;
    }
    b->dsp_sync_cycle += kDspSyncCycles;
  }
  const uint64_t current = tms57002_total_cycles(b->dsp);
  if (target > current && tms57002_run(b->dsp, target - current) < 0)
    b->faults |= KONAMIGQ_FAULT_DSP;
}
void advance_pcm_to(konamigq *b, uint64_t now_ns) {
  bool overflow = false;
  const uint64_t target = pcm_cycles_at_ns(now_ns, &overflow);
  if (overflow) {
    b->faults |= KONAMIGQ_FAULT_TIME_OVERFLOW;
    return;
  }
  for (unsigned i = 0; i < 2; ++i) {
    const uint64_t current = k054539_cycles(b->pcm[i]);
    if (target > current &&
        k054539_advance_cycles(b->pcm[i], target - current,
                               i == 0 ? pcm_sample : pcm_sample1, b) != 0)
      b->faults |= KONAMIGQ_FAULT_TIME_OVERFLOW;
  }
}
void advance_to_event(konamigq *b, uint64_t at_ns) {
  pump_sound_cpu_to_cycle(b, sound_target_cycle(b, at_ns));
  advance_dsp_to(b, at_ns);
  advance_pcm_to(b, at_ns);
  nscsi_hd_advance_to(b->hd, at_ns);
  (void)flush_bus(b);
  ncr53cf96_advance_to(b->ncr, muldiv_floor(at_ns, kNcrHz, 1000000000));
  mb89371_advance(b->duart, at_ns);
  (void)eeprom93c46_advance_to_ns(b->eeprom, at_ns);
  (void)flush_bus(b);
  dma_pump(b);
  b->now_ns = at_ns;
}
void update_to_ns(konamigq *b, uint64_t horizon) {
  while (b->now_ns < horizon) {
    uint64_t due = next_deadline_ns(b, horizon);
    if (due < b->now_ns)
      due = b->now_ns;
    if (due == b->now_ns) {
      /* A due event may advance a cycle without moving on the 1ns clock. */
      advance_to_event(b, due);
      if (due == horizon)
        break;
      const uint64_t stepped = due == UINT64_MAX ? due : due + 1;
      if (stepped == due)
        break;
      advance_to_event(b, stepped);
    } else {
      advance_to_event(b, due);
    }
  }
  if (b->now_ns < horizon)
    advance_to_event(b, horizon);
}
} // namespace

extern "C" konamigq_t *konamigq_create(const konamigq_config_t *config) {
  if (!config || !config->read_psx_ram8 || !config->write_psx_ram8 ||
      !config->sound_rom ||
      config->sound_rom_bytes != KONAMIGQ_SOUND_ROM_BYTES ||
      !config->k054539_rom ||
      config->k054539_rom_bytes != KONAMIGQ_K054539_ROM_BYTES ||
      config->eeprom_image_bytes !=
          (config->eeprom_image ? KONAMIGQ_EEPROM_BYTES : 0) ||
      !config->hard_disk.media_present || !config->hard_disk.read_sector ||
      !config->hard_disk.write_sector || config->hard_disk.sector_bytes != 512)
    return nullptr;
  auto *b = new (std::nothrow) konamigq;
  if (!b)
    return nullptr;
  b->config = *config;
  for (unsigned i = 0; i < 64; ++i) {
    b->default_eeprom[i * 2] = static_cast<uint8_t>(kDefaultEeprom[i]);
    b->default_eeprom[i * 2 + 1] = static_cast<uint8_t>(kDefaultEeprom[i] >> 8);
  }
  b->bus = nscsi_bus_create();
  if (!b->bus)
    goto fail;
  {
    nscsi_bus_ops_t ops{};
    ops.opaque = b;
    ops.changed = bus_changed;
    if (!nscsi_bus_attach(b->bus, 7, &ops))
      goto fail;
  }
  {
    nscsi_hd_config_t hd_config = config->hard_disk;
    hd_config.media_opaque = b;
    hd_config.read_sector = media_read;
    hd_config.write_sector = media_write;
    if (!hd_config.inquiry_data && !hd_config.inquiry_data_bytes)
      hd_config.inquiry_data_bytes = 0;
    b->hd = nscsi_hd_create(b->bus, 0, &hd_config);
    if (!b->hd)
      goto fail;
  }
  {
    ncr53cf96_bus_ops_t ops{};
    ops.opaque = b;
    ops.drive_ctrl = ncr_drive_ctrl;
    ops.drive_data = ncr_drive_data;
    ops.wait_ctrl = ncr_wait_ctrl;
    ops.irq = ncr_irq;
    ops.drq = ncr_drq;
    b->ncr = ncr53cf96_create(kNcrHz, &ops);
    if (!b->ncr)
      goto fail;
  }
  {
    const uint8_t *image = config->eeprom_image;
    size_t image_size = config->eeprom_image_bytes;
    if (!image) {
      image = b->default_eeprom.data();
      image_size = b->default_eeprom.size();
    }
    b->eeprom = eeprom93c46_create(image, image_size);
    if (!b->eeprom)
      goto fail;
  }
  {
    mb89371_callbacks_t callbacks{};
    callbacks.user = b;
    callbacks.txd[0] = duart_txd0;
    callbacks.txd[1] = duart_txd1;
    callbacks.dtr[0] = duart_dtr0;
    callbacks.dtr[1] = duart_dtr1;
    callbacks.rts[0] = duart_rts0;
    callbacks.rts[1] = duart_rts1;
    callbacks.rxready[0] = duart_rxready0;
    callbacks.rxready[1] = duart_rxready1;
    callbacks.txready[0] = duart_txready0;
    callbacks.txready[1] = duart_txready1;
    callbacks.txempty[0] = duart_txempty0;
    callbacks.txempty[1] = duart_txempty1;
    callbacks.syndet[0] = duart_syndet0;
    callbacks.syndet[1] = duart_syndet1;
    b->duart = mb89371_create(kDuartHz, &callbacks);
    if (!b->duart)
      goto fail;
  }
  {
    if (k056800_init(&b->mirac, mirac_irq, b) != 0)
      goto fail;
  }
  {
    tms57002_config_t dsp_config{};
    dsp_config.opaque = b;
    dsp_config.data_read = dsp_read;
    dsp_config.data_write = dsp_write;
    dsp_config.dready_changed = dsp_line_dready;
    dsp_config.pc0_changed = dsp_line_pc0;
    dsp_config.empty_changed = dsp_line_empty;
    b->dsp = tms57002_create(&dsp_config);
    if (!b->dsp)
      goto fail;
  }
  for (unsigned i = 0; i < 2; ++i) {
    K054539Config pcm_config{};
    pcm_config.clock_hz = kK054Hz;
    pcm_config.flags = 0;
    pcm_config.sample_read = sample_read;
    pcm_config.timer_line = i == 0 ? pcm_timer : nullptr;
    pcm_config.opaque = b;
    b->pcm[i] = k054539_create(&pcm_config);
    if (!b->pcm[i])
      goto fail;
  }
  {
    M68000Bus sound_bus{};
    sound_bus.opaque = b;
    sound_bus.read_word = m68k_read;
    sound_bus.write_word = m68k_write;
    sound_bus.use_default_autovectors = true;
    b->sound_cpu = std::make_unique<M68000>(sound_bus);
  }
  if (!konamigq_reset(b))
    goto fail;
  return b;
fail:
  konamigq_destroy(b);
  return nullptr;
}

extern "C" void konamigq_destroy(konamigq_t *b) {
  if (!b)
    return;
  if (b->busy) {
    b->faults |= KONAMIGQ_FAULT_REENTRANT;
    return;
  }
  if (b->pcm[1])
    k054539_destroy(b->pcm[1]);
  if (b->pcm[0])
    k054539_destroy(b->pcm[0]);
  if (b->dsp)
    tms57002_destroy(b->dsp);
  if (b->duart)
    mb89371_destroy(b->duart);
  if (b->ncr)
    ncr53cf96_destroy(b->ncr);
  if (b->hd)
    nscsi_hd_destroy(b->hd);
  if (b->bus)
    nscsi_bus_destroy(b->bus);
  if (b->eeprom)
    eeprom93c46_destroy(b->eeprom);
  delete b;
}

extern "C" int konamigq_advance_to_ns(konamigq_t *b, uint64_t now_ns) {
  if (!b || b->busy || now_ns < b->now_ns) {
    if (b)
      b->faults |= now_ns < b->now_ns ? KONAMIGQ_FAULT_TIME_REWIND
                                      : KONAMIGQ_FAULT_REENTRANT;
    return 0;
  }
  bool overflow = false;
  (void)muldiv_floor(now_ns, kNcrHz, 1000000000, &overflow);
  if (overflow) {
    b->faults |= KONAMIGQ_FAULT_TIME_OVERFLOW;
    return 0;
  }
  update_to_ns(b, now_ns);
  return !(b->faults & KONAMIGQ_FAULT_TIME_OVERFLOW);
}

extern "C" int konamigq_reset(konamigq_t *b) {
  if (!b || b->busy)
    return 0;
  b->dma_address = b->dma_remaining = 0;
  b->dma_active = b->dma_write = b->drq = false;
  b->sound_ctrl = b->sound_intck = 0;
  b->sound_cpu_reset = true;
  b->sound_cpu->reset();
  tms57002_reset(b->dsp);
  b->dsp_sync_cycle =
      ((tms57002_total_cycles(b->dsp) / kDspSyncCycles) + 1) * kDspSyncCycles;
  k056800_reset(&b->mirac);
  ncr53cf96_reset(b->ncr);
  nscsi_hd_reset(b->hd);
  mb89371_reset(b->duart);
  eeprom93c46_reset(b->eeprom);
  k054539_reset(b->pcm[0]);
  k054539_reset(b->pcm[1]);
  mb89371_write_cts(b->duart, 0, 0);
  return flush_bus(b);
}

extern "C" int konamigq_read(konamigq_t *b, uint32_t address, unsigned width,
                             uint32_t *value) {
  if (!b || !value || b->busy || (width != 1 && width != 2 && width != 4))
    return 0;
  uint32_t out = 0;
  uint32_t input_base = UINT32_MAX;
  uint32_t input_data = UINT32_MAX;
  for (unsigned lane = 0; lane < width; ++lane) {
    const uint32_t a = address + lane;
    uint8_t byte = open_bus();
    if (a >= kNcrAddress && a < kNcrAddress + 0x20 && !(a & 1)) {
      const unsigned reg = (a - kNcrAddress) >> 1;
      if (reg != 9 && reg != 10 && reg != 15)
        byte = ncr53cf96_read(b->ncr, reg);
    } else if (a >= kMiracAddress && a < kMiracAddress + 0x20 && !(a & 1)) {
      byte = k056800_host_read(&b->mirac, (a - kMiracAddress) >> 1);
    } else if (a >= 0x1f200000 && a < 0x1f240000) {
      const uint32_t base = a & ~3u;
      const unsigned inlane = a & 3;
      if (base != input_base) {
        input_base = base;
        input_data = mapped_input(b, base);
      }
      /* Inputs are full 32-bit ports, unlike the byte device delegates.
       * In particular lightgun X needs bit 8 in its second byte. */
      byte = static_cast<uint8_t>(input_data >> (inlane * 8));
    } else if (a >= kPcmMapStart && a < kPcmMapStart + kPcmMapBytes &&
               (a & 1) == 0) {
      byte = b->pcm_ram[(a - kPcmMapStart) >> 1];
    } else if (a >= kDuartAddress && a < kDuartAddress + 0x10 && !(a & 1)) {
      const unsigned channel = (a - kDuartAddress) >> 3;
      const unsigned reg = ((a - kDuartAddress) >> 1) & 3;
      byte = mb89371_read(b->duart, channel, reg);
    }
    out |= uint32_t(byte) << (lane * 8);
  }
  *value = out;
  return 1;
}

extern "C" int konamigq_write(konamigq_t *b, uint32_t address, unsigned width,
                              uint32_t value, uint32_t mem_mask) {
  if (!b || b->busy || (width != 1 && width != 2 && width != 4))
    return 0;
  bool eeprom_write = false;
  bool scsi_access = false;
  for (unsigned lane = 0; lane < width; ++lane) {
    if (!selected(mem_mask, lane))
      continue;
    const uint32_t a = address + lane;
    const uint8_t byte = static_cast<uint8_t>(value >> (lane * 8));
    if (a >= kNcrAddress && a < kNcrAddress + 0x20 && !(a & 1)) {
      ncr53cf96_write(b->ncr, (a - kNcrAddress) >> 1, byte);
      scsi_access = true;
    } else if (a >= kMiracAddress && a < kMiracAddress + 0x20 && !(a & 1)) {
      (void)k056800_host_write(&b->mirac, (a - kMiracAddress) >> 1, byte);
    } else if (a >= kEepromAddress && a < kEepromAddress + 2) {
      eeprom_write = true;
    } else if (a >= 0x1f300000 && a < 0x1f600000 && !(a & 1)) {
      b->pcm_ram[(a - kPcmMapStart) >> 1] = byte;
    } else if (a >= kDuartAddress && a < kDuartAddress + 0x10 && !(a & 1)) {
      const unsigned channel = (a - kDuartAddress) >> 3;
      const unsigned reg = ((a - kDuartAddress) >> 1) & 3;
      mb89371_write(b->duart, channel, reg, byte);
    }
  }
  if (eeprom_write) {
    const uint16_t data = static_cast<uint16_t>(value);
    (void)eeprom93c46_advance_to_ns(b->eeprom, b->now_ns);
    (void)eeprom93c46_set_di(b->eeprom, data & 1);
    (void)eeprom93c46_set_cs(b->eeprom, data & 2);
    (void)eeprom93c46_set_clk(b->eeprom, data & 4);
    const bool next_reset = !(data & 0x40);
    if (next_reset && !b->sound_cpu_reset)
      b->sound_cpu->reset();
    if (!next_reset && b->sound_cpu_reset) {
      b->sound_epoch_ns = b->now_ns;
      b->sound_epoch_cycle = b->sound_cpu->total_cycles();
    }
    b->sound_cpu_reset = next_reset;
  }
  if (scsi_access) {
    if (!flush_bus(b))
      return 0;
    dma_pump(b);
  }
  return 1;
}

extern "C" int konamigq_dma5_read(konamigq_t *b, uint32_t ram_address,
                                  int32_t word_count) {
  if (!b || b->busy || word_count < 0)
    return 0;
  const uint64_t size = uint64_t(word_count) * 4;
  if (uint64_t(ram_address) + size > KONAMIGQ_PSRAM_BYTES) {
    b->faults |= KONAMIGQ_FAULT_DMA_RANGE;
    return 0;
  }
  b->dma_address = ram_address;
  b->dma_remaining = size;
  b->dma_active = size != 0;
  b->dma_write = false;
  dma_pump(b);
  return 1;
}
extern "C" int konamigq_dma5_write(konamigq_t *b, uint32_t ram_address,
                                   int32_t word_count) {
  if (!b || b->busy || word_count < 0)
    return 0;
  const uint64_t size = uint64_t(word_count) * 4;
  if (uint64_t(ram_address) + size > KONAMIGQ_PSRAM_BYTES) {
    b->faults |= KONAMIGQ_FAULT_DMA_RANGE;
    return 0;
  }
  b->dma_address = ram_address;
  b->dma_remaining = size;
  b->dma_active = size != 0;
  b->dma_write = true;
  dma_pump(b);
  return 1;
}

extern "C" int konamigq_sound_read16(konamigq_t *b, uint32_t address,
                                     uint16_t mem_mask, uint16_t *value) {
  if (!b || !value || b->busy)
    return 0;
  uint16_t result = 0xffff;
  if (mem_mask & 0xff00)
    result = static_cast<uint16_t>(
        (result & 0xff) |
        (uint16_t(sound_byte_read(b, address & ~1u, M68kSpace::Program)) << 8));
  if (mem_mask & 0xff)
    result = static_cast<uint16_t>(
        (result & 0xff00) |
        sound_byte_read(b, (address & ~1u) + 1, M68kSpace::Program));
  *value = result;
  return 1;
}
extern "C" int konamigq_sound_write16(konamigq_t *b, uint32_t address,
                                      uint16_t data, uint16_t mem_mask) {
  if (!b || b->busy)
    return 0;
  if (mem_mask & 0xff00)
    sound_byte_write(b, address & ~1u, uint8_t(data >> 8));
  if (mem_mask & 0xff)
    sound_byte_write(b, (address & ~1u) + 1, uint8_t(data));
  return 1;
}
namespace {
int uart_line_write(konamigq *b, unsigned channel, int pin, int level,
                    uint64_t now_ns) {
  if (!b || b->busy || channel > 1 || now_ns < b->now_ns)
    return 0;
  if (!konamigq_advance_to_ns(b, now_ns))
    return 0;
  switch (pin) {
  case 0:
    mb89371_write_rxd(b->duart, channel, level);
    break;
  case 1:
    mb89371_write_cts(b->duart, channel, level);
    break;
  case 2:
    mb89371_write_dsr(b->duart, channel, level);
    break;
  case 3:
    mb89371_write_txc(b->duart, channel, level);
    break;
  default:
    mb89371_write_rxc(b->duart, channel, level);
    break;
  }
  return 1;
}
} // namespace
extern "C" int konamigq_uart_rxd(konamigq_t *b, unsigned ch, int v,
                                 uint64_t ns) {
  return uart_line_write(b, ch, 0, v, ns);
}
extern "C" int konamigq_uart_cts(konamigq_t *b, unsigned ch, int v,
                                 uint64_t ns) {
  return uart_line_write(b, ch, 1, v, ns);
}
extern "C" int konamigq_uart_dsr(konamigq_t *b, unsigned ch, int v,
                                 uint64_t ns) {
  return uart_line_write(b, ch, 2, v, ns);
}
extern "C" int konamigq_uart_txc(konamigq_t *b, unsigned ch, int v,
                                 uint64_t ns) {
  return uart_line_write(b, ch, 3, v, ns);
}
extern "C" int konamigq_uart_rxc(konamigq_t *b, unsigned ch, int v,
                                 uint64_t ns) {
  return uart_line_write(b, ch, 4, v, ns);
}
extern "C" uint8_t konamigq_tms_data_read(konamigq_t *b) {
  return b ? tms57002_data_r(b->dsp) : 0xff;
}
extern "C" int konamigq_tms_data_write(konamigq_t *b, uint8_t v) {
  if (!b || b->busy)
    return 0;
  tms57002_data_w(b->dsp, v);
  return 1;
}
extern "C" int konamigq_tms_process_sample(konamigq_t *b, const float in[4],
                                           float out[4]) {
  if (!b || b->busy)
    return -1;
  return tms57002_process_sample(b->dsp, in, out);
}
extern "C" int konamigq_eeprom_export(const konamigq_t *b, uint8_t *image,
                                      size_t size) {
  return b ? eeprom93c46_export_image(b->eeprom, image, size) : -1;
}
extern "C" int konamigq_eeprom_import(konamigq_t *b, const uint8_t *image,
                                      size_t size) {
  if (!b || b->busy)
    return -1;
  return eeprom93c46_import_image(b->eeprom, image, size);
}
extern "C" void konamigq_get_status(const konamigq_t *b, konamigq_status_t *s) {
  if (!s)
    return;
  std::memset(s, 0, sizeof(*s));
  if (!b)
    return;
  s->now_ns = b->now_ns;
  s->faults = b->faults;
  s->sound_ctrl = b->sound_ctrl;
  s->sound_intck = b->sound_intck;
  s->sound_cpu_reset = b->sound_cpu_reset;
  s->dma_active = b->dma_active;
  s->dma_write_to_scsi = b->dma_write;
  s->dma_bytes_remaining = b->dma_remaining;
  s->drq = b->drq;
  s->sound_cpu_cycles = b->sound_cpu->total_cycles();
  s->sound_pc = b->sound_cpu->pc(); s->sound_a7 = b->sound_cpu->address_register(7);
  s->sound_d0 = b->sound_cpu->data_register(0); s->sound_sr = b->sound_cpu->sr();
  s->sound_stopped = b->sound_cpu->stopped(); s->sound_bus_error = b->sound_cpu->bus_error();
  s->dsp_pc = tms57002_pc(b->dsp);
  s->dsp_cycles = tms57002_total_cycles(b->dsp);
  s->k054539_cycles = k054539_cycles(b->pcm[0]);
  ncr53cf96_get_status(b->ncr, &s->ncr);
  nscsi_hd_get_status(b->hd, &s->hard_disk);
}
