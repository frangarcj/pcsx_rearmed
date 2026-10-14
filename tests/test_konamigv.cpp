#include "eeprom93c46.h"
#include "konamigv.h"
#include "nscsi_bus.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
constexpr uint32_t kNcr = 0x1f000000;
constexpr uint32_t kP1 = 0x1f100000;
constexpr uint32_t kP2 = 0x1f100004;
constexpr uint32_t kP34 = 0x1f100008;
constexpr uint32_t kEeprom = 0x1f180000;
constexpr uint32_t kUart = 0x1f680000;
constexpr uint64_t kEepromWriteNs = 1750000;

struct TestContext {
  KonamiGV *board = nullptr;
  std::vector<uint8_t> ram = std::vector<uint8_t>(KONAMIGV_RAM_BYTES, 0xcc);
  std::array<uint32_t, 3> ports{0xa1b2c3d4, 0x12345678, 0x90abcdef};
  unsigned input_reads[3]{};
  std::vector<int> irq_edges;
  std::vector<std::pair<uint64_t, int>> irq_times;
  std::vector<std::pair<uint64_t, int>> txd_edges;
  uint64_t now = 0;
  uint64_t sequence = 1;
  uint64_t media_ns=UINT64_MAX;
  unsigned audio_calls=0,clock_calls=0;
  unsigned sector_reads = 0;
  bool reenter_on_irq = false;
};

static uint32_t input_read(void *opaque, unsigned port) {
  auto *ctx = static_cast<TestContext *>(opaque);
  assert(port < 3);
  ++ctx->input_reads[port];
  return ctx->ports[port];
}
static uint8_t ram_read(void *opaque, uint32_t address) {
  auto *ctx = static_cast<TestContext *>(opaque);
  assert(address < ctx->ram.size());
  return ctx->ram[address];
}
static void ram_write(void *opaque, uint32_t address, uint8_t value) {
  auto *ctx = static_cast<TestContext *>(opaque);
  assert(address < ctx->ram.size());
  ctx->ram[address] = value;
}
static void irq10(void *opaque, int asserted) {
  auto *ctx = static_cast<TestContext *>(opaque);
  ctx->irq_edges.push_back(!!asserted);
  uint64_t now = ctx->now;
  if (ctx->board) {
    konamigv_status_t status{};
    konamigv_get_status(ctx->board, &status);
    now = status.now_ns;
  }
  ctx->irq_times.emplace_back(now, !!asserted);
  if (ctx->reenter_on_irq) {
    uint32_t value = 0;
    (void)konamigv_read(ctx->board, kNcr, 1, &value);
  }
}
static void uart_txd(void *opaque, unsigned channel, int level) {
  auto *ctx = static_cast<TestContext *>(opaque);
  assert(channel < 2);
  uint64_t now = ctx->now;
  if (ctx->board) {
    konamigv_status_t status{};
    konamigv_get_status(ctx->board, &status);
    now = status.now_ns;
  }
  ctx->txd_edges.emplace_back(now, !!level);
}

static int media_present(void *) { return 1; }
static uint64_t media_sequence(void *opaque) {
  return static_cast<TestContext *>(opaque)->sequence;
}
static int media_read(void *opaque, uint32_t lba,
                      uint8_t out[NSCSI_CD_SECTOR_BYTES]) {
  auto *ctx = static_cast<TestContext *>(opaque);
  ++ctx->sector_reads;
  for (size_t i = 0; i < NSCSI_CD_SECTOR_BYTES; ++i)
    out[i] = uint8_t((lba * 7 + i * 13 + 3) & 0xff);
  return 1;
}
static uint32_t track_start(void *, unsigned track) {
  if (track == 0)
    return 0;
  if (track == 1 || track == 0xaa)
    return 100;
  return 0;
}
static unsigned last_track(void *) { return 1; }
static unsigned track_for_lba(void *, uint32_t) { return 0; }
static uint8_t track_adr(void *, unsigned) { return 0x14; }
static int track_audio(void *, unsigned) { return 0; }
static int media_clock(void *opaque,uint64_t ns){auto*c=static_cast<TestContext*>(opaque);assert(c->media_ns==UINT64_MAX || ns>=c->media_ns);c->media_ns=ns;c->clock_calls++;return 1;}
static void cdda_start(void *opaque, uint32_t, uint32_t){auto*c=static_cast<TestContext*>(opaque);assert(c->media_ns!=UINT64_MAX);c->audio_calls++;}
static void cdda_pause(void *, int) {}
static void cdda_status(void *opaque, nscsi_cd_audio_status_t *status) {
  auto*c=static_cast<TestContext*>(opaque);assert(c->media_ns!=UINT64_MAX);c->audio_calls++;
  *status = {};
}
static void cdda_gain(void *, unsigned, float) {}

static KonamiGV *make_board(TestContext &ctx, const uint8_t *eeprom = nullptr,
                            size_t eeprom_bytes = 0) {
  konamigv_config_t config{};
  config.opaque = &ctx;
  config.read_input = input_read;
  config.read_ram8 = ram_read;
  config.write_ram8 = ram_write;
  config.irq10 = irq10;
  config.uart_txd[0] = uart_txd;
  config.uart_txd[1] = uart_txd;
  config.cd_media = {&ctx,        media_present, media_sequence, media_read,
                     track_start, last_track,    track_for_lba,  track_adr,
                     track_audio, cdda_start,    cdda_pause,     cdda_status,
                     cdda_gain};
  config.advance_media=media_clock;
  config.eeprom_image = eeprom;
  config.eeprom_image_bytes = eeprom_bytes;
  ctx.board = konamigv_create(&config);
  assert(ctx.board);
  return ctx.board;
}

static void advance(KonamiGV *b, TestContext &ctx, uint64_t now) {
  assert(now >= ctx.now);
  ctx.now = now;
  assert(konamigv_advance_to(b, now));
}
static int read_bus(KonamiGV *b, uint32_t address, unsigned width,
                    uint32_t *value) {
  return konamigv_read(b, address, width, value);
}
static int read8(KonamiGV *b, uint32_t address) {
  uint32_t value = 0;
  assert(read_bus(b, address, 1, &value));
  return int(value & 0xff);
}
static void write_bus(KonamiGV *b, uint32_t address, unsigned width,
                      uint32_t value, uint32_t mask) {
  assert(konamigv_write(b, address, width, value, mask));
}
static void write8(KonamiGV *b, uint32_t address, uint8_t value) {
  write_bus(b, address, 1, value, 0xff);
}
static void ncr_write(KonamiGV *b, unsigned reg, uint8_t value) {
  write8(b, kNcr + reg * 2, value);
}
static int ncr_read(KonamiGV *b, unsigned reg) {
  return read8(b, kNcr + reg * 2);
}

static void mmio_width_mask_tests() {
  TestContext ctx;
  auto *b = make_board(ctx);
  uint32_t value = 0;
  assert(read_bus(b, kP1, 4, &value));
  assert(ctx.input_reads[KONAMIGV_PORT_P1] == 1);
  const uint32_t expected_p1 = (ctx.ports[0] & ~(1u << 13)) | (1u << 13);
  assert(value == expected_p1); // P1's EEPROM DO replaces raw input bit 13.
  assert(read_bus(b, kP2, 2, &value) && value == 0x5678);
  assert(read_bus(b, kP34, 1, &value) && value == 0xef);

  ncr_write(b, 8, 7);
  ncr_write(b, 9, 0x35);
  assert(read_bus(b, kNcr + 16, 1, &value) && value == 7);
  assert(read_bus(b, kNcr + 16, 2, &value) && value == 0x0007);
  assert(read_bus(b, kNcr + 16, 4, &value));
  assert(value == 0x00000007); // Source default-zero lanes and write-only reg 9.
  assert(read8(b, kNcr + 20) == 0 && read8(b, kNcr + 30) == 0);
  write_bus(b, kNcr + 16, 4, 0x00aa00cc, 0x00ff0000);
  assert(ncr_read(b, 8) == 7); // lane 2 reaches write-only register 9.
  write_bus(b, kNcr + 16, 4, 0x000000f1, 0);
  assert(ncr_read(b, 8) == 7); // inactive byte lanes do not mutate registers

  write8(b, kUart + 2, 0x4e); // 8251 mode write
  write_bus(b, kUart, 4, 0x0000055a, 0x00ff00ff);
  const int uart_status = read8(b, kUart + 2);
  assert(uart_status != 0xff); // lane 2 reaches channel 0 status/control
  write_bus(b, kUart + 4, 4, 0x00f80000, 0x00ff0000);
  assert(read8(b, kUart + 6) == 0xf8); // wrapper mode uses its mapped lane
  write8(b, 0x1f780000, 0xaa);         // source watchdog write no-op
  value = 0x12345678;
  assert(!read_bus(b, 0x1f780000, 4, &value)); // nopw() supplies no read handler
  assert(value == 0x12345678);
  assert(!read_bus(b, 0x1f200000, 4, &value));
  konamigv_destroy(b);
}

static void write_eeprom_lines(KonamiGV *b, TestContext &ctx, int di, int cs,
                               int clk) {
  advance(b, ctx, ++ctx.now);
  const uint32_t data = (di ? 1u : 0u) | (cs ? 2u : 0u) | (clk ? 4u : 0u);
  write_bus(b, kEeprom, 4, data, 0xff);
}
static void eeprom_bit(KonamiGV *b, TestContext &ctx, int bit) {
  write_eeprom_lines(b, ctx, bit, 1, 0);
  write_eeprom_lines(b, ctx, bit, 1, 1);
  write_eeprom_lines(b, ctx, bit, 1, 0);
}
static void eeprom_command(KonamiGV *b, TestContext &ctx, unsigned op,
                           unsigned address) {
  write_eeprom_lines(b, ctx, 0, 1, 0);
  eeprom_bit(b, ctx, 1); // start bit is separate from op/address
  const unsigned word = ((op << 6) | (address & 63u));
  for (int bit = 7; bit >= 0; --bit)
    eeprom_bit(b, ctx, (word >> bit) & 1u);
}
static void eeprom_send_word(KonamiGV *b, TestContext &ctx, uint16_t word) {
  for (int bit = 15; bit >= 0; --bit)
    eeprom_bit(b, ctx, (word >> bit) & 1u);
}
static bool eeprom_do(KonamiGV *b) {
  uint32_t value = 0;
  assert(read_bus(b, kP1, 4, &value));
  return (value & 0x2000) != 0;
}
static void eeprom_mmio_clock_and_busy_tests() {
  std::array<uint8_t, EEPROM93C46_IMAGE_BYTES> initial{};
  initial.fill(0x5a);
  TestContext ctx;
  auto *b = make_board(ctx, initial.data(), initial.size());
  std::array<uint8_t, EEPROM93C46_IMAGE_BYTES> exported{};
  assert(konamigv_eeprom_export(b, exported.data(), exported.size()) == 0);
  assert(exported == initial);

  eeprom_command(b, ctx, 0, 0x30); // EWEN
  write_eeprom_lines(b, ctx, 0, 0, 0);
  eeprom_command(b, ctx, 1, 37);
  eeprom_send_word(b, ctx, 0xa65c);
  write_eeprom_lines(b, ctx, 0, 0, 0);
  write_eeprom_lines(b, ctx, 0, 1, 0);
  konamigv_status_t status{};
  konamigv_get_status(b, &status);
  assert(status.eeprom_busy && !eeprom_do(b));
  assert(konamigv_eeprom_export(b, exported.data(), exported.size()) == 0);
  assert(exported[74] == 0x5c && exported[75] == 0xa6);
  advance(b, ctx, ctx.now + kEepromWriteNs);
  konamigv_get_status(b, &status);
  assert(!status.eeprom_busy && eeprom_do(b));

  /* READ opcode and all clock/data edges pass through the guest MMIO port. */
  eeprom_command(b, ctx, 2, 37);
  uint16_t readback = 0;
  for (unsigned i = 0; i < 16; ++i) {
    eeprom_bit(b, ctx, 0);
    readback = uint16_t((readback << 1) | unsigned(eeprom_do(b)));
  }
  write_eeprom_lines(b, ctx, 0, 0, 0);
  assert(readback == 0xa65c);

  /* A machine reset cancels only the busy countdown; the words persist. */
  eeprom_command(b, ctx, 1, 38);
  eeprom_send_word(b, ctx, 0x1357);
  write_eeprom_lines(b, ctx, 0, 0, 0);
  konamigv_get_status(b, &status);
  assert(status.eeprom_busy);
  assert(konamigv_reset(b));
  konamigv_get_status(b, &status);
  assert(!status.eeprom_busy);
  assert(konamigv_eeprom_export(b, exported.data(), exported.size()) == 0);
  assert(exported[76] == 0x57 && exported[77] == 0x13);
  konamigv_destroy(b);
}

static void uart_tx_waveform_and_loopback_tests() {
  TestContext tx;
  auto *b = make_board(tx);
  /* Mode bit 1 selects the internal BRG; the source default is external. */
  write8(b, kUart + 4, 4); // 4 MHz / (2 << 4) = 125 kHz clock
  write8(b, kUart + 6, 0xf2);
  write8(b, kUart + 2, 0x4e); // asynchronous 8N1, x16
  write8(b, kUart + 2, 0x05); // enable receiver and transmitter
  tx.txd_edges.clear();
  write8(b, kUart, 0xa5);
  advance(b, tx, 2000000);
  std::vector<int> levels;
  std::vector<uint64_t> times;
  for (const auto &edge : tx.txd_edges) {
    levels.push_back(edge.second);
    times.push_back(edge.first);
  }
  const std::vector<int> expected{0, 1, 0, 1, 0, 0, 1, 0, 1, 1};
  assert(levels.size() >= expected.size());
  assert(std::equal(expected.begin(), expected.end(), levels.begin()));
  for (size_t i = 1; i < times.size(); ++i)
    assert(times[i] - times[i - 1] == 128000);
  konamigv_destroy(b);

  TestContext loop;
  b = make_board(loop);
  write8(b, kUart + 4, 4);
  write8(b, kUart + 6, 0xfa); // BRG plus source loopback control bit
  write8(b, kUart + 2, 0x4e);
  write8(b, kUart + 2, 0x05);
  write8(b, kUart, 0x69);
  advance(b, loop, 3000000);
  assert(read8(b, kUart + 2) & 0x02); // 8251 RXRDY
  assert(read8(b, kUart) == 0x69);
  konamigv_destroy(b);
}

static void irq_ack_and_reset_pending_dma_tests() {
  TestContext ctx;
  auto *b = make_board(ctx);
  ctx.irq_edges.clear();
  ctx.irq_times.clear();
  ctx.reenter_on_irq = true;
  ncr_write(b, 8, 7);    // IRQ enabled, initiator ID 7
  ncr_write(b, 3, 0x03); // SCSI bus reset
  konamigv_status_t status{};
  konamigv_get_status(b, &status);
  assert(status.ncr.timer_deadline == 260); // 130 * source clock-conversion 2
  const uint64_t reset_due_ns = 16250;      // ceil(260 * 1e9 / 16 MHz)
  advance(b, ctx, reset_due_ns - 1);
  konamigv_get_status(b, &status);
  assert(status.ncr.cycles == 259 && !status.ncr.irq);
  assert(ctx.irq_times.empty());
  advance(b, ctx, reset_due_ns);
  konamigv_get_status(b, &status);
  assert(status.ncr.irq && (status.ncr.interrupt_status & 0x80));
  assert(status.faults & KONAMIGV_FAULT_REENTRANT);
  assert(!ctx.irq_edges.empty() && ctx.irq_edges.back() == 1);
  assert(!ctx.irq_times.empty() && ctx.irq_times.back().first == reset_due_ns &&
         ctx.irq_times.back().second == 1);
  assert(ncr_read(b, 5) == 0x80); // acknowledge interrupt status
  konamigv_get_status(b, &status);
  assert(!status.ncr.irq && ctx.irq_edges.back() == 0);
  ctx.reenter_on_irq = false;
  const size_t ack_edges = ctx.irq_edges.size();
  advance(b, ctx, reset_due_ns + 1000);
  assert(ctx.irq_edges.size() == ack_edges); // no auto-relatch after ACK

  assert(konamigv_dma5_read(b, 0x100, 16)); // request waits for DRQ
  konamigv_get_status(b, &status);
  assert(status.dma_active && status.dma_bytes_remaining == 64);
  assert(konamigv_reset(b));
  konamigv_get_status(b, &status);
  assert(!status.dma_active && status.dma_bytes_remaining == 0);
  assert(!konamigv_dma5_read(b, KONAMIGV_RAM_BYTES - 4, 2));
  konamigv_get_status(b, &status);
  assert(status.faults & KONAMIGV_FAULT_DMA_RANGE);
  konamigv_destroy(b);
}

static void ncr_reg_write_for_cdb(KonamiGV *b,
                                  const std::vector<uint8_t> &cdb) {
  for (uint8_t byte : cdb)
    ncr_write(b, 2, byte);
  ncr_write(b, 8, 7);
  ncr_write(b, 4, 4);
  ncr_write(b, 3, 0x41);
}

static void cdda_clock_command_test(){
 TestContext ctx;auto*b=make_board(ctx);advance(b,ctx,1234567);
 ncr_reg_write_for_cdb(b,{0x42,0,0x40,1,0,0,0,0,16,0});advance(b,ctx,2000000);
 assert(ctx.audio_calls && ctx.clock_calls && ctx.media_ns>=1234567 && ctx.media_ns<=2000000);
 konamigv_destroy(b);
}

static void cd_dma_read_test(bool sliced) {
  TestContext ctx;
  auto *b = make_board(ctx);
  const std::vector<uint8_t> cdb{0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0};
  ncr_reg_write_for_cdb(b, cdb);
  advance(b, ctx, 180000);
  konamigv_status_t status{};
  konamigv_get_status(b, &status);
  assert(status.cdrom.target.cdb_length == cdb.size());
  assert(status.ncr.interrupt_status & 0x08); // command phase completed
  (void)ncr_read(b, 5);

  ncr_write(b, 11, 0x08);
  ncr_write(b, 0, 0x00);
  ncr_write(b, 1, 0x08); // 2048 bytes
  ncr_write(b, 14, 0);
  assert(konamigv_dma5_read(b, 0, NSCSI_CD_SECTOR_BYTES / 4));
  ncr_write(b, 3, 0x90); // CI_XFER with DMA enabled
  if (sliced) {
    for (uint64_t t = ctx.now + 1000; t <= 4000000; t += 1000) {
      advance(b, ctx, t);
      konamigv_get_status(b, &status);
      if (!status.dma_active)
        break;
    }
  } else {
    advance(b, ctx, 4000000);
  }
  konamigv_get_status(b, &status);
  assert(!status.dma_active && status.dma_bytes_remaining == 0);
  assert(ctx.sector_reads == 1);
  for (size_t i = 0; i < NSCSI_CD_SECTOR_BYTES; ++i)
    assert(ctx.ram[i] == uint8_t((i * 13 + 3) & 0xff));
  konamigv_destroy(b);
}

static void cd_dma_write_test(bool sliced) {
  TestContext ctx;
  auto *b = make_board(ctx);
  const std::vector<uint8_t> cdb{0x15, 0, 0, 0, 12, 0};
  ncr_reg_write_for_cdb(b, cdb);
  advance(b, ctx, 180000);
  konamigv_status_t status{};
  konamigv_get_status(b, &status);
  assert(status.cdrom.target.cdb_length == cdb.size());
  assert(status.ncr.interrupt_status & 0x08);
  (void)ncr_read(b, 5);

  const std::array<uint8_t, 12> parameters{0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 2, 0};
  std::copy(parameters.begin(), parameters.end(), ctx.ram.begin() + 0x1000);
  ncr_write(b, 11, 0x08);
  ncr_write(b, 0, parameters.size());
  ncr_write(b, 1, 0);
  ncr_write(b, 14, 0);
  assert(konamigv_dma5_write(b, 0x1000, 3));
  ncr_write(b, 3, 0x90);
  if (sliced) {
    for (uint64_t t = ctx.now + 1000; t <= 4000000; t += 1000) {
      advance(b, ctx, t);
      konamigv_get_status(b, &status);
      if (!status.dma_active)
        break;
    }
    advance(b, ctx, 4000000);
  } else {
    advance(b, ctx, 4000000);
  }
  konamigv_get_status(b, &status);
  assert(!status.dma_active && status.dma_bytes_remaining == 0);
  assert(status.cdrom.block_size == 512);
  konamigv_destroy(b);
}
} // namespace

int main() {
  mmio_width_mask_tests();
  eeprom_mmio_clock_and_busy_tests();
  uart_tx_waveform_and_loopback_tests();
  irq_ack_and_reset_pending_dma_tests();
  cdda_clock_command_test();
  cd_dma_read_test(false);
  cd_dma_read_test(true);
  cd_dma_write_test(false);
  cd_dma_write_test(true);
  std::puts("Konami GV common board map, EEPROM, UART, IRQ and SCSI DMA PASS");
}
