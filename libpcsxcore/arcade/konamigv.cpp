/* Standalone common Konami GV board adapter, based on MAME
 * src/mame/konami/konamigv.cpp at deb2f54fe7300e7a192690318beb860159bfd52a.
 * Copyright R. Belmont and smf. SPDX-License-Identifier: BSD-3-Clause.
 */
#include "konamigv.h"

#include "eeprom93c46.h"
#include "mb89371.h"
#include "nscsi_bus.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace {
constexpr uint32_t kNcrBase = 0x1f000000;
constexpr uint32_t kInputBase = 0x1f100000;
constexpr uint32_t kEepromBase = 0x1f180000;
constexpr uint32_t kUartBase = 0x1f680000;
constexpr uint32_t kWatchdogBase = 0x1f780000;
constexpr uint32_t kNcrClockHz = 16000000;
constexpr uint32_t kRamBytes = KONAMIGV_RAM_BYTES;
constexpr uint64_t kNanosPerSecond = 1000000000ULL;
constexpr uint64_t kEventBudget = 1000000;
constexpr uint32_t kNcrId = 7;
constexpr uint32_t kCdId = 4;

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
  // Here remainder < 1e9 and multiplier is the 16 MHz NCR clock, so this
  // product is safely representable in uint64_t.
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
  // Here remainder < 16 MHz and multiplier is 1e9, also safely in uint64_t.
  const uint64_t product = remainder * multiplier;
  const uint64_t fraction = product / divisor + (product % divisor != 0);
  if (fraction > UINT64_MAX - whole) {
    if (overflow)
      *overflow = true;
    return UINT64_MAX;
  }
  return whole + fraction;
}

uint64_t ns_to_cycles_floor(uint64_t ns, bool *overflow = nullptr) {
  return muldiv_floor(ns, kNcrClockHz, kNanosPerSecond, overflow);
}

uint64_t cycles_to_ns_ceil(uint64_t cycles, bool *overflow = nullptr) {
  return muldiv_ceil(cycles, kNanosPerSecond, kNcrClockHz, overflow);
}

uint32_t width_mask(unsigned width) {
  switch (width) {
  case 1:
    return 0xff;
  case 2:
    return 0xffff;
  case 4:
    return UINT32_MAX;
  default:
    return 0;
  }
}

bool valid_media(const nscsi_cd_media_ops_t &m) {
  return m.present && m.sequence_counter && m.read_mode1_sector &&
         m.track_start && m.last_track && m.track_for_lba &&
         m.track_adr_control && m.track_is_audio && m.cdda_start &&
         m.cdda_pause && m.cdda_status && m.cdda_set_gain;
}
} // namespace

struct konamigv {
  konamigv_config_t config{};
  nscsi_cd_media_ops_t media{};
  NscsiBus *bus = nullptr;
  Ncr53cf96 *ncr = nullptr;
  NscsiCdrom *cd = nullptr;
  mb89371_t *duart = nullptr;
  EEPROM93C46 *eeprom = nullptr;
  uint64_t now_ns = 0;
  uint32_t faults = 0;
  uint32_t eeprom_out = 0;
  uint32_t dma_address = 0;
  uint64_t dma_remaining = 0;
  bool dma_active = false;
  bool dma_write_to_scsi = false;
  bool drq = false;
  bool in_callback = false;
  bool scheduling = false;
};

namespace {
bool callback_enter(KonamiGV *b) {
  if (b->in_callback) {
    b->faults |= KONAMIGV_FAULT_REENTRANT;
    return false;
  }
  b->in_callback = true;
  return true;
}
void callback_leave(KonamiGV *b) { b->in_callback = false; }
bool api_allowed(KonamiGV *b) {
  if (!b)
    return false;
  if (b->in_callback) {
    b->faults |= KONAMIGV_FAULT_REENTRANT;
    return false;
  }
  return true;
}

void ncr_irq(void *opaque, int asserted) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (b->config.irq10 && callback_enter(b)) {
    b->config.irq10(b->config.opaque, !!asserted);
    callback_leave(b);
  }
}
void ncr_drq(void *opaque, int asserted) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (b->in_callback) {
    b->faults |= KONAMIGV_FAULT_REENTRANT;
    return;
  }
  const bool next = asserted != 0;
  if (!b->drq && next)
    b->dma_active = b->dma_active || b->dma_remaining != 0;
  b->drq = next;
}
void ncr_drive_ctrl(void *opaque, uint32_t lines, uint32_t mask) {
  auto *b = static_cast<KonamiGV *>(opaque);
  nscsi_bus_drive_ctrl(b->bus, kNcrId, lines, mask);
}
void ncr_drive_data(void *opaque, uint32_t data) {
  auto *b = static_cast<KonamiGV *>(opaque);
  nscsi_bus_drive_data(b->bus, kNcrId, data);
}
void ncr_wait_ctrl(void *opaque, uint32_t lines, uint32_t mask) {
  auto *b = static_cast<KonamiGV *>(opaque);
  nscsi_bus_wait_ctrl(b->bus, kNcrId, lines, mask);
}
void bus_changed(void *opaque, uint32_t ctrl, uint32_t data) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (b->ncr)
    ncr53cf96_bus_changed(b->ncr, ctrl, data);
}

void uart_line(KonamiGV *b, konamigv_line_cb callback, unsigned channel,
               int level) {
  if (callback && callback_enter(b)) {
    callback(b->config.opaque, channel, !!level);
    callback_leave(b);
  }
}
#define UART_CB(name, field, channel)                                          \
  void name(void *opaque, int level) {                                         \
    auto *b = static_cast<KonamiGV *>(opaque);                                 \
    uart_line(b, b->config.uart_##field[channel], channel, level);             \
  }
UART_CB(uart_txd0, txd, 0)
UART_CB(uart_txd1, txd, 1)
UART_CB(uart_dtr0, dtr, 0)
UART_CB(uart_dtr1, dtr, 1)
UART_CB(uart_rts0, rts, 0)
UART_CB(uart_rts1, rts, 1)
UART_CB(uart_rxready0, rxready, 0)
UART_CB(uart_rxready1, rxready, 1)
UART_CB(uart_txready0, txready, 0)
UART_CB(uart_txready1, txready, 1)
UART_CB(uart_txempty0, txempty, 0)
UART_CB(uart_txempty1, txempty, 1)
UART_CB(uart_syndet0, syndet, 0)
UART_CB(uart_syndet1, syndet, 1)
#undef UART_CB

int media_present(void *opaque) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (!callback_enter(b))
    return 0;
  const int result = b->media.present(b->media.opaque);
  callback_leave(b);
  return result;
}
uint64_t media_sequence(void *opaque) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (!callback_enter(b))
    return 0;
  const uint64_t result = b->media.sequence_counter(b->media.opaque);
  callback_leave(b);
  return result;
}
int media_read(void *opaque, uint32_t lba, uint8_t out[NSCSI_CD_SECTOR_BYTES]) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (!callback_enter(b))
    return 0;
  const int result = b->media.read_mode1_sector(b->media.opaque, lba, out);
  callback_leave(b);
  return result;
}
uint32_t media_track_start(void *opaque, unsigned track) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (!callback_enter(b))
    return 0;
  const uint32_t result = b->media.track_start(b->media.opaque, track);
  callback_leave(b);
  return result;
}
unsigned media_last_track(void *opaque) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (!callback_enter(b))
    return 0;
  const unsigned result = b->media.last_track(b->media.opaque);
  callback_leave(b);
  return result;
}
unsigned media_track_for_lba(void *opaque, uint32_t lba) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (!callback_enter(b))
    return 0;
  const unsigned result = b->media.track_for_lba(b->media.opaque, lba);
  callback_leave(b);
  return result;
}
uint8_t media_adr_control(void *opaque, unsigned track) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (!callback_enter(b))
    return 0;
  const uint8_t result = b->media.track_adr_control(b->media.opaque, track);
  callback_leave(b);
  return result;
}
int media_track_audio(void *opaque, unsigned track) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (!callback_enter(b))
    return 0;
  const int result = b->media.track_is_audio(b->media.opaque, track);
  callback_leave(b);
  return result;
}
void media_cdda_start(void *opaque, uint32_t lba, uint32_t blocks) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (callback_enter(b)) {
    if (b->config.advance_media && !b->config.advance_media(b->config.opaque,b->now_ns))
      b->faults |= KONAMIGV_FAULT_BUS;
    b->media.cdda_start(b->media.opaque, lba, blocks);
    callback_leave(b);
  }
}
void media_cdda_pause(void *opaque, int pause) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (callback_enter(b)) {
    if (b->config.advance_media && !b->config.advance_media(b->config.opaque,b->now_ns))
      b->faults |= KONAMIGV_FAULT_BUS;
    b->media.cdda_pause(b->media.opaque, pause);
    callback_leave(b);
  }
}
void media_cdda_status(void *opaque, nscsi_cd_audio_status_t *status) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (callback_enter(b)) {
    if (b->config.advance_media && !b->config.advance_media(b->config.opaque,b->now_ns))
      b->faults |= KONAMIGV_FAULT_BUS;
    b->media.cdda_status(b->media.opaque, status);
    callback_leave(b);
  } else {
    *status = {};
  }
}
void media_cdda_gain(void *opaque, unsigned channel, float gain) {
  auto *b = static_cast<KonamiGV *>(opaque);
  if (callback_enter(b)) {
    if (b->config.advance_media && !b->config.advance_media(b->config.opaque,b->now_ns))
      b->faults |= KONAMIGV_FAULT_BUS;
    b->media.cdda_set_gain(b->media.opaque, channel, gain);
    callback_leave(b);
  }
}
nscsi_cd_media_ops_t wrapped_media(KonamiGV *b) {
  nscsi_cd_media_ops_t m{};
  m.opaque = b;
  m.present = media_present;
  m.sequence_counter = media_sequence;
  m.read_mode1_sector = media_read;
  m.track_start = media_track_start;
  m.last_track = media_last_track;
  m.track_for_lba = media_track_for_lba;
  m.track_adr_control = media_adr_control;
  m.track_is_audio = media_track_audio;
  m.cdda_start = media_cdda_start;
  m.cdda_pause = media_cdda_pause;
  m.cdda_status = media_cdda_status;
  m.cdda_set_gain = media_cdda_gain;
  return m;
}

int bus_flush(KonamiGV *b) {
  if (!nscsi_bus_flush(b->bus)) {
    b->faults |= KONAMIGV_FAULT_BUS;
    return 0;
  }
  return 1;
}
bool next_event_ns(KonamiGV *b, uint64_t *due, bool *found) {
  *due = UINT64_MAX;
  *found = false;
  ncr53cf96_status_t ncr{};
  ncr53cf96_get_status(b->ncr, &ncr);
  if (ncr.timer_deadline != UINT64_MAX) {
    bool overflow = false;
    *due = cycles_to_ns_ceil(ncr.timer_deadline, &overflow);
    if (overflow) {
      b->faults |= KONAMIGV_FAULT_TIME_OVERFLOW;
      return false;
    }
    *found = true;
  }
  nscsi_cdrom_status_t cd{};
  nscsi_cdrom_get_status(b->cd, &cd);
  if (cd.target.timer_deadline_ns != UINT64_MAX &&
      (!*found || cd.target.timer_deadline_ns < *due)) {
    *due = cd.target.timer_deadline_ns;
    *found = true;
  }
  uint64_t duart_due = UINT64_MAX;
  if (mb89371_next_event_ns(b->duart, &duart_due) &&
      (!*found || duart_due < *due)) {
    *due = duart_due;
    *found = true;
  }
  return true;
}

int dma_pump(KonamiGV *b) {
  if (!b->dma_active || !b->drq || !b->dma_remaining)
    return 1;
  uint64_t budget = b->dma_remaining;
  uint64_t transferred = 0;
  while (b->dma_active && b->drq && b->dma_remaining && transferred < budget) {
    uint8_t byte;
    if (b->dma_write_to_scsi) {
      if (!callback_enter(b))
        return 0;
      byte = b->config.read_ram8(b->config.opaque, b->dma_address);
      callback_leave(b);
      ncr53cf96_dma_write(b->ncr, byte);
    } else {
      byte = ncr53cf96_dma_read(b->ncr);
      if (!callback_enter(b))
        return 0;
      b->config.write_ram8(b->config.opaque, b->dma_address, byte);
      callback_leave(b);
    }
    ++b->dma_address;
    --b->dma_remaining;
    ++transferred;
    if (!bus_flush(b))
      return 0;
    if (!b->dma_remaining)
      b->dma_active = false;
  }
  if (transferred == budget && b->dma_active && b->drq && b->dma_remaining) {
    b->faults |= KONAMIGV_FAULT_EVENT_BUDGET;
    return 0;
  }
  return 1;
}

int advance_devices(KonamiGV *b, uint64_t to_ns) {
  bool overflow = false;
  const uint64_t cycles = ns_to_cycles_floor(to_ns, &overflow);
  if (overflow) {
    b->faults |= KONAMIGV_FAULT_TIME_OVERFLOW;
    return 0;
  }
  // Publish the batch timestamp before any device can call the board's host
  // callbacks. Callback observers must see the event's due time, not the
  // preceding scheduler step.
  b->now_ns = to_ns;
  ncr53cf96_advance_to(b->ncr, cycles);
  nscsi_cdrom_advance_to(b->cd, to_ns);
  mb89371_advance(b->duart, to_ns);
  if (eeprom93c46_advance_to_ns(b->eeprom, to_ns) != 0)
    b->faults |= KONAMIGV_FAULT_TIME_REWIND;
  return 1;
}

bool byte_selected(unsigned width, uint32_t mask, unsigned lane) {
  if (lane >= width)
    return false;
  return (mask & (0xffu << (lane * 8))) != 0;
}

uint8_t open_bus_byte() { return 0; }
bool in_range(uint32_t address, unsigned width, uint32_t start,
              uint32_t bytes) {
  const uint64_t end = uint64_t(address) + width;
  return address < start + bytes && end > start;
}

uint32_t input_value(KonamiGV *b, unsigned port) {
  uint32_t value = UINT32_MAX;
  if (b->config.read_input && callback_enter(b)) {
    value = b->config.read_input(b->config.opaque, port);
    callback_leave(b);
  }
  if (port == KONAMIGV_PORT_P1) {
    const uint32_t mask = 1u << 13;
    value = (value & ~mask) | (uint32_t(eeprom93c46_do(b->eeprom) != 0) << 13);
  }
  return value;
}

int uart_pin(KonamiGV *b, unsigned channel, int pin, int level,
             uint64_t now_ns) {
  if (!api_allowed(b) || channel > 1)
    return 0;
  if (!konamigv_advance_to(b, now_ns))
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

extern "C" KonamiGV *konamigv_create(const konamigv_config_t *config) {
  if (!config || !config->read_ram8 || !config->write_ram8 ||
      config->eeprom_image_bytes != (config->eeprom_image ? 128u : 0u) ||
      !valid_media(config->cd_media))
    return nullptr;
  auto *b = new (std::nothrow) KonamiGV;
  if (!b)
    return nullptr;
  b->config = *config;
  b->media = config->cd_media;
  b->bus = nscsi_bus_create();
  if (!b->bus)
    goto fail;
  {
    nscsi_bus_ops_t ops{};
    ops.opaque = b;
    ops.changed = bus_changed;
    if (!nscsi_bus_attach(b->bus, kNcrId, &ops))
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
    b->ncr = ncr53cf96_create(kNcrClockHz, &ops);
    if (!b->ncr)
      goto fail;
  }
  {
    nscsi_cdrom_config_t cd{};
    cd.bus = b->bus;
    cd.scsi_id = kCdId;
    cd.block_size = NSCSI_CD_SECTOR_BYTES;
    cd.media = wrapped_media(b);
    b->cd = nscsi_cdrom_create(&cd);
    if (!b->cd)
      goto fail;
  }
  {
    mb89371_callbacks_t cb{};
    cb.user = b;
    for (unsigned ch = 0; ch < 2; ++ch) {
      cb.txd[ch] = ch ? uart_txd1 : uart_txd0;
      cb.dtr[ch] = ch ? uart_dtr1 : uart_dtr0;
      cb.rts[ch] = ch ? uart_rts1 : uart_rts0;
      cb.rxready[ch] = ch ? uart_rxready1 : uart_rxready0;
      cb.txready[ch] = ch ? uart_txready1 : uart_txready0;
      cb.txempty[ch] = ch ? uart_txempty1 : uart_txempty0;
      cb.syndet[ch] = ch ? uart_syndet1 : uart_syndet0;
    }
    b->duart = mb89371_create(4000000, &cb);
    if (!b->duart)
      goto fail;
  }
  b->eeprom =
      eeprom93c46_create(config->eeprom_image, config->eeprom_image_bytes);
  if (!b->eeprom)
    goto fail;
  if (!bus_flush(b))
    goto fail;
  if (!konamigv_reset(b))
    goto fail;
  return b;
fail:
  konamigv_destroy(b);
  return nullptr;
}

extern "C" void konamigv_destroy(KonamiGV *b) {
  if (!b)
    return;
  if (b->in_callback) {
    b->faults |= KONAMIGV_FAULT_REENTRANT;
    return;
  }
  if (b->eeprom)
    eeprom93c46_destroy(b->eeprom);
  if (b->duart)
    mb89371_destroy(b->duart);
  if (b->cd)
    nscsi_cdrom_destroy(b->cd);
  if (b->ncr)
    ncr53cf96_destroy(b->ncr);
  if (b->bus)
    nscsi_bus_destroy(b->bus);
  delete b;
}

extern "C" int konamigv_advance_to(KonamiGV *b, uint64_t now_ns) {
  if (!api_allowed(b))
    return 0;
  if (now_ns < b->now_ns) {
    b->faults |= KONAMIGV_FAULT_TIME_REWIND;
    return 0;
  }
  if (b->scheduling) {
    b->faults |= KONAMIGV_FAULT_REENTRANT;
    return 0;
  }
  b->scheduling = true;
  uint64_t events = 0;
  while (true) {
    bool found = false;
    uint64_t due = UINT64_MAX;
    if (!next_event_ns(b, &due, &found)) {
      b->scheduling = false;
      return 0;
    }
    if (found && due <= now_ns) {
      if (++events > kEventBudget) {
        b->faults |= KONAMIGV_FAULT_EVENT_BUDGET;
        b->scheduling = false;
        return 0;
      }
      if (!advance_devices(b, due)) {
        b->scheduling = false;
        return 0;
      }
      if (!bus_flush(b) || !dma_pump(b)) {
        b->scheduling = false;
        return 0;
      }
      continue;
    }
    if (!advance_devices(b, now_ns)) {
      b->scheduling = false;
      return 0;
    }
    if (!bus_flush(b) || !dma_pump(b)) {
      b->scheduling = false;
      return 0;
    }
    bool immediate = false;
    uint64_t after = UINT64_MAX;
    if (!next_event_ns(b, &after, &immediate)) {
      b->scheduling = false;
      return 0;
    }
    if (!immediate || after > now_ns)
      break;
  }
  b->scheduling = false;
  return 1;
}

extern "C" int konamigv_reset(KonamiGV *b) {
  if (!api_allowed(b))
    return 0;
  b->dma_active = false;
  b->dma_write_to_scsi = false;
  b->dma_address = 0;
  b->dma_remaining = 0;
  ncr53cf96_reset(b->ncr);
  nscsi_cdrom_reset(b->cd);
  mb89371_reset(b->duart);
  eeprom93c46_reset(b->eeprom);
  mb89371_write_cts(b->duart, 0, 0);
  return bus_flush(b);
}

extern "C" int konamigv_read(KonamiGV *b, uint32_t address, unsigned width,
                             uint32_t *value) {
  if (!api_allowed(b) || !value || !width_mask(width))
    return 0;
  if (address > UINT32_MAX - (width - 1))
    return 0;
  uint32_t result = UINT32_MAX;
  bool handled = false;
  uint32_t cached_inputs[3]{};
  bool input_cached[3]{};
  for (unsigned lane = 0; lane < width; ++lane) {
    const uint32_t at = address + lane;
    uint8_t byte = open_bus_byte();
    if (at >= kNcrBase && at < kNcrBase + 0x20) {
      const uint32_t offset = at - kNcrBase;
      if (!(offset & 1)) {
        const unsigned reg = offset >> 1;
        // The source map has no read delegates for clock/test/FIFO-align.
        if (reg != 9 && reg != 10 && reg != 15)
          byte = ncr53cf96_read(b->ncr, reg);
        handled = true;
      }
    } else if (at >= kInputBase && at < kInputBase + 12) {
      const unsigned port = (at - kInputBase) >> 2;
      const unsigned p_lan = (at - kInputBase) & 3;
      if (!input_cached[port]) {
        cached_inputs[port] = input_value(b, port);
        input_cached[port] = true;
      }
      byte = uint8_t(cached_inputs[port] >> (p_lan * 8));
      handled = true;
    } else if (at >= kUartBase && at < kUartBase + 16) {
      const uint32_t offset = at - kUartBase;
      if (!(offset & 1)) {
        const unsigned channel = offset >= 8;
        const unsigned reg = (offset & 7) >> 1;
        byte = mb89371_read(b->duart, channel, reg);
        handled = true;
      }
    }
    const uint32_t lane_mask = 0xffu << (lane * 8);
    result = (result & ~lane_mask) | (uint32_t(byte) << (lane * 8));
  }
  handled = handled || in_range(address, width, kNcrBase, 0x20) ||
            in_range(address, width, kInputBase, 12) ||
            in_range(address, width, kUartBase, 16);
  if (!handled)
    return 0;
  *value = result & width_mask(width);
  if (!bus_flush(b))
    return 0;
  return 1;
}

extern "C" int konamigv_write(KonamiGV *b, uint32_t address, unsigned width,
                              uint32_t value, uint32_t mem_mask) {
  if (!api_allowed(b) || !width_mask(width))
    return 0;
  if (address > UINT32_MAX - (width - 1))
    return 0;
  mem_mask &= width_mask(width);
  bool handled = false;
  for (unsigned lane = 0; lane < width; ++lane) {
    if (!byte_selected(width, mem_mask, lane))
      continue;
    const uint32_t at = address + lane;
    const uint8_t byte = uint8_t(value >> (lane * 8));
    if (at >= kNcrBase && at < kNcrBase + 0x20) {
      const uint32_t offset = at - kNcrBase;
      if (!(offset & 1)) {
        ncr53cf96_write(b->ncr, offset >> 1, byte);
        handled = true;
      }
    } else if (at >= kEepromBase && at < kEepromBase + 4) {
      if (at == kEepromBase) {
        const uint32_t lane_mask = 0xffu << (lane * 8);
        b->eeprom_out =
            (b->eeprom_out & ~lane_mask) | (uint32_t(byte) << (lane * 8));
        eeprom93c46_advance_to_ns(b->eeprom, b->now_ns);
        eeprom93c46_set_di(b->eeprom, (b->eeprom_out & 1) != 0);
        eeprom93c46_set_cs(b->eeprom, (b->eeprom_out & 2) != 0);
        eeprom93c46_set_clk(b->eeprom, (b->eeprom_out & 4) != 0);
      }
      handled = true;
    } else if (at >= kUartBase && at < kUartBase + 16) {
      const uint32_t offset = at - kUartBase;
      if (!(offset & 1)) {
        const unsigned channel = offset >= 8;
        const unsigned reg = (offset & 7) >> 1;
        mb89371_write(b->duart, channel, reg, byte);
        handled = true;
      }
    } else if (at >= kWatchdogBase && at < kWatchdogBase + 4) {
      handled = true;
    }
  }
  handled = handled || in_range(address, width, kNcrBase, 0x20) ||
            in_range(address, width, kEepromBase, 4) ||
            in_range(address, width, kUartBase, 16) ||
            in_range(address, width, kWatchdogBase, 4);
  if (!handled)
    return 0;
  return bus_flush(b) && dma_pump(b);
}

namespace {
int dma_begin(KonamiGV *b, uint32_t ram_address, int32_t word_count,
              bool write_to_scsi) {
  if (!api_allowed(b))
    return 0;
  if (word_count < 0) {
    b->faults |= KONAMIGV_FAULT_DMA_RANGE;
    return 0;
  }
  const uint64_t bytes = uint64_t(uint32_t(word_count)) * 4;
  if (uint64_t(ram_address) + bytes > kRamBytes) {
    b->faults |= KONAMIGV_FAULT_DMA_RANGE;
    return 0;
  }
  b->dma_address = ram_address;
  b->dma_remaining = bytes;
  b->dma_write_to_scsi = write_to_scsi;
  b->dma_active = bytes != 0;
  if (!bus_flush(b))
    return 0;
  return dma_pump(b);
}
} // namespace

extern "C" int konamigv_dma5_read(KonamiGV *b, uint32_t address,
                                  int32_t words) {
  return dma_begin(b, address, words, false);
}
extern "C" int konamigv_dma5_write(KonamiGV *b, uint32_t address,
                                   int32_t words) {
  return dma_begin(b, address, words, true);
}

extern "C" int konamigv_uart_rxd(KonamiGV *b, unsigned channel, int level,
                                 uint64_t now_ns) {
  return uart_pin(b, channel, 0, level, now_ns);
}
extern "C" int konamigv_uart_cts(KonamiGV *b, unsigned channel, int level,
                                 uint64_t now_ns) {
  return uart_pin(b, channel, 1, level, now_ns);
}
extern "C" int konamigv_uart_dsr(KonamiGV *b, unsigned channel, int level,
                                 uint64_t now_ns) {
  return uart_pin(b, channel, 2, level, now_ns);
}
extern "C" int konamigv_uart_txc(KonamiGV *b, unsigned channel, int level,
                                 uint64_t now_ns) {
  return uart_pin(b, channel, 3, level, now_ns);
}
extern "C" int konamigv_uart_rxc(KonamiGV *b, unsigned channel, int level,
                                 uint64_t now_ns) {
  return uart_pin(b, channel, 4, level, now_ns);
}

extern "C" int konamigv_eeprom_export(const KonamiGV *b, uint8_t *image,
                                      size_t bytes) {
  return b ? eeprom93c46_export_image(b->eeprom, image, bytes) : -1;
}
extern "C" int konamigv_eeprom_import(KonamiGV *b, const uint8_t *image,
                                      size_t bytes) {
  if (!api_allowed(b))
    return -1;
  return eeprom93c46_import_image(b->eeprom, image, bytes);
}
extern "C" void konamigv_get_status(const KonamiGV *b,
                                    konamigv_status_t *status) {
  if (!b || !status)
    return;
  *status = {};
  status->now_ns = b->now_ns;
  status->dma_bytes_remaining = b->dma_remaining;
  status->faults = b->faults;
  status->dma_active = b->dma_active;
  status->dma_write_to_scsi = b->dma_write_to_scsi;
  status->drq = b->drq;
  status->eeprom_do = uint8_t(eeprom93c46_do(b->eeprom));
  status->eeprom_busy = uint8_t(eeprom93c46_busy(b->eeprom));
  status->uart_faults = mb89371_faults(b->duart);
  ncr53cf96_get_status(b->ncr, &status->ncr);
  nscsi_cdrom_get_status(b->cd, &status->cdrom);
}
