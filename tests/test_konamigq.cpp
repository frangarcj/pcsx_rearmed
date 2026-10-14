#include "chd_hd.h"
#include "eeprom93c46.h"
#include "konamigq.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t kNcr = 0x1f000000;
constexpr uint32_t kMirac = 0x1f100000;
constexpr uint32_t kEeprom = 0x1f180000;
constexpr uint32_t kGunX1 = 0x1f200000;
constexpr uint32_t kDsw = 0x1f238000;
constexpr uint32_t kDuart = 0x1f680000;

struct Context {
  konamigq_t *board = nullptr;
  bool reenter_media = false;
  bool rejected_media_reentry = false;
  arcade_chd_hd_t *disk = nullptr;
  std::vector<uint8_t> psx_ram =
      std::vector<uint8_t>(KONAMIGQ_PSRAM_BYTES, 0xcd);
  std::vector<uint8_t> sound_rom =
      std::vector<uint8_t>(KONAMIGQ_SOUND_ROM_BYTES, 0xff);
  std::vector<uint8_t> pcm_rom =
      std::vector<uint8_t>(KONAMIGQ_K054539_ROM_BYTES, 0);
  std::array<uint32_t, 9> inputs{};
  std::vector<std::pair<unsigned, int>> sound_irqs;
  std::vector<std::tuple<unsigned, uint64_t, int32_t, int32_t>> samples;
  std::vector<std::pair<unsigned, int>> uart_txd;
  unsigned media_reads = 0;
  unsigned media_writes = 0;
};

void put16(std::vector<uint8_t> &mem, size_t address, uint16_t value) {
  mem[address] = uint8_t(value >> 8);
  mem[address + 1] = uint8_t(value);
}
void put32(std::vector<uint8_t> &mem, size_t address, uint32_t value) {
  put16(mem, address, uint16_t(value >> 16));
  put16(mem, address + 2, uint16_t(value));
}
void emit_move_byte(std::vector<uint8_t> &rom, size_t &pc, uint8_t data,
                    uint32_t address) {
  put16(rom, pc, 0x13fc);
  pc += 2;
  put16(rom, pc, data);
  pc += 2;
  put32(rom, pc, address);
  pc += 4;
}
void emit_move_word(std::vector<uint8_t> &rom, size_t &pc, uint16_t data,
                    uint32_t address) {
  put16(rom, pc, 0x33fc);
  pc += 2;
  put16(rom, pc, data);
  pc += 2;
  put32(rom, pc, address);
  pc += 4;
}
void make_guest_sound_rom(Context &ctx) {
  put32(ctx.sound_rom, 0, 0x0010fff0);
  put32(ctx.sound_rom, 4, 0x00000100);
  size_t pc = 0x100;
  put16(ctx.sound_rom, pc, 0x46fc);
  pc += 2; // MOVE #$2000,SR
  put16(ctx.sound_rom, pc, 0x2000);
  pc += 2;
  emit_move_byte(ctx.sound_rom, pc, 1, 0x00400009); // MIRAC IRQ enable
  emit_move_word(ctx.sound_rom, pc, 0x0035,
                 0x00500000); // DSP PLD/CLOAD, run, sync
  emit_move_byte(ctx.sound_rom, pc, 0x12, 0x00300001); // DASP host data
  emit_move_byte(ctx.sound_rom, pc, 0xff, 0x0020044e); // K054539 timer
  // Voice 0: 8-bit PCM, one-sample pitch, source address zero, key on.
  emit_move_byte(ctx.sound_rom, pc, 0x00, 0x00200000); // pitch low
  emit_move_byte(ctx.sound_rom, pc, 0x00, 0x00200002);
  emit_move_byte(ctx.sound_rom, pc, 0x01, 0x00200004); // pitch high
  emit_move_byte(ctx.sound_rom, pc, 0x00, 0x00200006); // volume
  emit_move_byte(ctx.sound_rom, pc, 0x18, 0x0020000a); // pan
  emit_move_byte(ctx.sound_rom, pc, 0x00, 0x00200010); // start low
  emit_move_byte(ctx.sound_rom, pc, 0x00, 0x00200012);
  emit_move_byte(ctx.sound_rom, pc, 0x00, 0x00200014);
  emit_move_byte(ctx.sound_rom, pc, 0x00, 0x00200400); // PCM8
  emit_move_byte(ctx.sound_rom, pc, 0x00, 0x00200402); // no loop
  emit_move_byte(ctx.sound_rom, pc, 0x21,
                 0x0020045e); // key-on latch + timer output
  emit_move_byte(ctx.sound_rom, pc, 0x01, 0x00200428); // key on voice 0
  put16(ctx.sound_rom, pc, 0x60fe);                    // BRA.S *
  ctx.pcm_rom[0] = 0x7f;
  ctx.pcm_rom[1] = 0x40;
  ctx.pcm_rom[2] = 0x20;
  ctx.pcm_rom[3] = 0x80; // MAME PCM8 end marker
}

uint8_t ram_read(void *opaque, uint32_t address) {
  auto *ctx = static_cast<Context *>(opaque);
  assert(address < ctx->psx_ram.size());
  return ctx->psx_ram[address];
}
void ram_write(void *opaque, uint32_t address, uint8_t value) {
  auto *ctx = static_cast<Context *>(opaque);
  assert(address < ctx->psx_ram.size());
  ctx->psx_ram[address] = value;
}
uint32_t input_read(void *opaque, unsigned port) {
  auto *ctx = static_cast<Context *>(opaque);
  assert(port < ctx->inputs.size());
  return ctx->inputs[port];
}
void psx_irq(void *, int) {}
void sound_irq(void *opaque, unsigned level, int asserted) {
  auto *ctx = static_cast<Context *>(opaque);
  ctx->sound_irqs.emplace_back(level, !!asserted);
}
void audio_sample(void *opaque, unsigned chip, uint64_t cycle, int32_t left,
                  int32_t right) {
  auto *ctx = static_cast<Context *>(opaque);
  ctx->samples.emplace_back(chip, cycle, left, right);
}
void uart_txd(void *opaque, unsigned channel, int level) {
  auto *ctx = static_cast<Context *>(opaque);
  ctx->uart_txd.emplace_back(channel, !!level);
}
int disk_read(void *opaque, uint32_t lba, uint8_t *data, size_t bytes) {
  auto *ctx = static_cast<Context *>(opaque);
  if (ctx->reenter_media) {
    ctx->reenter_media = false;
    konamigq_status_t status{};
    konamigq_get_status(ctx->board, &status);
    ctx->rejected_media_reentry = !konamigq_advance_to_ns(ctx->board, status.now_ns);
  }
  ++ctx->media_reads;
  return arcade_chd_hd_read(ctx->disk, lba, data, bytes) == 0;
}
int disk_write(void *opaque, uint32_t lba, const uint8_t *data, size_t bytes) {
  auto *ctx = static_cast<Context *>(opaque);
  ++ctx->media_writes;
  return arcade_chd_hd_write(ctx->disk, lba, data, bytes) == 0;
}

konamigq_t *make_board(Context &ctx, bool irq_guest = false) {
  make_guest_sound_rom(ctx);
  if (irq_guest) {
    std::fill(ctx.sound_rom.begin(), ctx.sound_rom.end(), 0);
    put32(ctx.sound_rom, 0, 0x0010fff0);
    put32(ctx.sound_rom, 4, 0x100);
    put32(ctx.sound_rom, (24 + 1) * 4, 0x200); // IRQ1 autovector
    size_t pc = 0x100;
    put16(ctx.sound_rom, pc, 0x46fc); pc += 2;
    put16(ctx.sound_rom, pc, 0x2000); pc += 2;
    emit_move_byte(ctx.sound_rom, pc, 1, 0x400009); // enable MIRAC IRQ
    put16(ctx.sound_rom, pc, 0x60fe);
    pc = 0x200;
    emit_move_byte(ctx.sound_rom, pc, 0x5a, 0x400001);
    emit_move_byte(ctx.sound_rom, pc, 0, 0x400009); // acknowledge at source
    put16(ctx.sound_rom, pc, 0x4e73); // RTE
  }
  arcade_chd_hd_info_t info{};
  arcade_chd_hd_get_info(ctx.disk, &info);
  konamigq_config_t config{};
  config.opaque = &ctx;
  config.read_psx_ram8 = ram_read;
  config.write_psx_ram8 = ram_write;
  config.read_input = input_read;
  config.psx_irq10 = psx_irq;
  config.sound_irq = sound_irq;
  config.k054539_audio = audio_sample;
  config.uart_txd[0] = config.uart_txd[1] = uart_txd;
  config.sound_rom = ctx.sound_rom.data();
  config.sound_rom_bytes = ctx.sound_rom.size();
  config.k054539_rom = ctx.pcm_rom.data();
  config.k054539_rom_bytes = ctx.pcm_rom.size();
  config.hard_disk.media_present = 1;
  config.hard_disk.cylinders = info.cylinders;
  config.hard_disk.heads = info.heads;
  config.hard_disk.sectors_per_track = info.sectors_per_track;
  config.hard_disk.sector_bytes = info.sector_bytes;
  config.hard_disk.media_opaque = &ctx;
  config.hard_disk.read_sector = disk_read;
  config.hard_disk.write_sector = disk_write;
  config.hard_disk.inquiry_data = info.inquiry;
  config.hard_disk.inquiry_data_bytes = info.inquiry_bytes;
  auto *board = konamigq_create(&config);
  assert(board);
  ctx.board = board;
  return board;
}

void write8(konamigq_t *b, uint32_t address, uint8_t data) {
  assert(konamigq_write(b, address, 1, data, 0xff));
}
uint8_t read8(konamigq_t *b, uint32_t address) {
  uint32_t value = 0;
  assert(konamigq_read(b, address, 1, &value));
  return uint8_t(value);
}
void ncr_write(konamigq_t *b, unsigned reg, uint8_t data) {
  write8(b, kNcr + reg * 2, data);
}
uint8_t ncr_read(konamigq_t *b, unsigned reg) {
  return read8(b, kNcr + reg * 2);
}
void advance(konamigq_t *b, uint64_t &now, uint64_t target) {
  assert(target >= now);
  assert(konamigq_advance_to_ns(b, target));
  now = target;
}

void test_ports_masks_eeprom_irq_guest_sound(Context &ctx) {
  auto *b = make_board(ctx);
  ctx.inputs[KONAMIGQ_PORT_GUNX1] = 0x000001a3;
  ctx.inputs[KONAMIGQ_PORT_DSW] = 0x00000035;
  assert(read8(b, kGunX1) == 0xa3);
  uint32_t input = 0;
  assert(konamigq_read(b, kGunX1, 4, &input));
  // Independent MAME CPU-space reads preserve all nine gun X bits and
  // default zero on inactive byte lanes of device delegates.
  assert(input == 0x000001a3);
  assert(read8(b, kGunX1 + 1) == 1);
  assert(read8(b, kGunX1 + 2) == 0 && read8(b, kGunX1 + 3) == 0);
  assert(konamigq_write(b, 0x1f300000, 4, 0x12345678, UINT32_MAX));
  assert(konamigq_read(b, 0x1f300000, 4, &input) && input == 0x00340078);
  assert(read8(b, 0x1f300001) == 0);
  uint16_t source_word;
  assert(konamigq_sound_read16(b, 0x500000, 0xffff, &source_word) && source_word == 5);
  assert(konamigq_sound_read16(b, 0x400000, 0xffff, &source_word) && source_word == 0);
  assert(konamigq_sound_read16(b, 0x580000, 0xffff, &source_word) && source_word == 0);
  assert(read8(b, kDsw + 2) == 1);

  std::array<uint8_t, KONAMIGQ_EEPROM_BYTES> image{};
  assert(konamigq_eeprom_export(b, image.data(), image.size()) == 0);
  assert(image[0] == 0x2b && image[1] == 0x29 && image[2] == 0x56);
  image[0x20] = 0x34;
  image[0x21] = 0x12;
  assert(konamigq_eeprom_import(b, image.data(), image.size()) == 0);
  assert(konamigq_reset(b));
  std::array<uint8_t, KONAMIGQ_EEPROM_BYTES> retained{};
  assert(konamigq_eeprom_export(b, retained.data(), retained.size()) == 0);
  assert(retained == image);

  // Write data and CS/CLK to the EEPROM port, then release the 68000.
  assert(konamigq_write(b, kEeprom, 2, 0x0040, 0xffff));
  konamigq_status_t status{};
  konamigq_get_status(b, &status);
  assert(!status.sound_cpu_reset);
  uint64_t now = 0;
  advance(b, now, 1000000);
  konamigq_get_status(b, &status);
  assert(status.sound_cpu_cycles > 0 && status.dsp_cycles > 0);
  assert(status.sound_ctrl == 0x35);
  assert(status.sound_intck == 1); // exact timer deadline raised its first edge
  assert(!ctx.samples.empty());
  assert(std::any_of(ctx.samples.begin(), ctx.samples.end(), [](const auto &s) {
    return std::get<0>(s) == 0 && (std::get<2>(s) != 0 || std::get<3>(s) != 0);
  }));

  // Sound guest enabled K056800 IRQ1; a host-to-sound write raises it.
  write8(b, kMirac, 0x5a);
  write8(b, kMirac + 14, 0x00);
  assert(!ctx.sound_irqs.empty());
  assert(ctx.sound_irqs.back().first == 1 && ctx.sound_irqs.back().second == 1);
  uint16_t sound_word = 0;
  assert(konamigq_sound_read16(b, 0x400000, 0x00ff, &sound_word));
  assert(uint8_t(sound_word) == 0x5a);
  assert(konamigq_sound_write16(b, 0x400008, 0x0000, 0x00ff));
  assert(ctx.sound_irqs.back().first == 1 && ctx.sound_irqs.back().second == 0);

  // DUART MMIO exposes four byte registers per channel on every other byte
  // lane.
  write8(b, kDuart + 6, 0x4e);
  write8(b, kDuart + 4, 0x00);
  write8(b, kDuart + 2, 0x27);
  write8(b, kDuart, 0xa5);
  assert(konamigq_advance_to_ns(b, now + 100000));
  assert(ctx.uart_txd.size() > 8);
  konamigq_destroy(b);
}

void test_dma_wait_reset(Context &ctx) {
  auto *b = make_board(ctx);
  assert(konamigq_dma5_read(b, 0x100, 4));
  konamigq_status_t status{};
  konamigq_get_status(b, &status);
  assert(status.dma_active && status.dma_bytes_remaining == 16 && !status.drq);
  assert(konamigq_reset(b));
  konamigq_get_status(b, &status);
  assert(!status.dma_active && status.dma_bytes_remaining == 0);
  konamigq_destroy(b);
}

void test_mirac_interrupt_executes_sound_guest(Context &ctx) {
  auto *b = make_board(ctx, true);
  uint64_t now = 0;
  assert(konamigq_write(b, kEeprom, 2, 0x40, 0xffff));
  advance(b, now, 1000000);
  assert(read8(b, kMirac + 16) == 0);
  write8(b, kMirac, 0xf7);
  write8(b, kMirac + 14, 0);
  advance(b, now, 2000000);
  // This reply is written by the actual IRQ1 handler, never by a test callback.
  assert(read8(b, kMirac + 16) == 0x5a);
  assert(ctx.sound_irqs.back() == std::make_pair(1u, 0));
  konamigq_status_t status{};
  konamigq_get_status(b, &status);
  assert(status.faults == 0 && status.sound_a7 == 0x0010fff0);
  assert(status.sound_sr == 0x2000 && !status.sound_bus_error);
  konamigq_destroy(b);
}

void test_eeprom_serial(Context &ctx) {
  auto *b = make_board(ctx);
  uint64_t now = 0;
  uint8_t pins = 0x40;
  const auto output = [&](uint8_t next) {
    advance(b, now, now + 1);
    pins = next;
    write8(b, kEeprom, pins);
  };
  const auto bit_out = [&](unsigned bit) {
    output(static_cast<uint8_t>(0x40 | 0x02 | (bit ? 1 : 0)));
    output(static_cast<uint8_t>(0x40 | 0x02 | (bit ? 1 : 0) | 0x04));
    output(static_cast<uint8_t>(0x40 | 0x02 | (bit ? 1 : 0)));
  };
  const auto command = [&](unsigned opcode, unsigned address) {
    output(0x40 | 0x02); // CS rises before the first clock edge.
    bit_out(1);          // Start bit is separate from opcode/address.
    const unsigned word = (opcode << 6) | (address & 63u);
    for (int bit = 7; bit >= 0; --bit)
      bit_out((word >> bit) & 1u);
  };

  command(0, 0x30); // EWEN
  output(0x40);
  command(1, 37); // WRITE word 37
  for (int bit = 15; bit >= 0; --bit)
    bit_out((0xa65c >> bit) & 1u);
  output(0x40); // Drop CS; the write cycle is now busy.
  output(0x42); // Raise it again to expose READY on DO.
  assert(read8(b, kDsw + 2) == 0);
  std::array<uint8_t, KONAMIGQ_EEPROM_BYTES> image{};
  assert(konamigq_eeprom_export(b, image.data(), image.size()) == 0);
  assert(image[74] == 0x5c && image[75] == 0xa6);
  advance(b, now, now + EEPROM93C46_WRITE_NS);
  assert(read8(b, kDsw + 2) == 1);
  konamigq_destroy(b);
}

void transfer_ncr10(konamigq_t *b, Context &ctx, uint64_t &now, bool sliced,
                    bool write, bool expect_overlay = false) {
  const std::array<uint8_t, 10> cdb{
      uint8_t(write ? 0x2a : 0x28), 0, 0, 0, 0, 0, 0, 0, 1, 0};
  const uint32_t dma_address = write ? 0x400 : 0x200;
  if (write) {
    for (unsigned i = 0; i < 512; ++i)
      ctx.psx_ram[dma_address + i] = uint8_t(0xa5 ^ i);
  }
  for (uint8_t byte : cdb)
    ncr_write(b, 2, byte);
  ncr_write(b, 8, 7);
  ncr_write(b, 4, 0);
  ncr_write(b, 3, 0x41);
  for (unsigned i = 0; i < 1000; ++i) {
    advance(b, now, now + (sliced ? 500 : 1000));
    konamigq_status_t state{};
    konamigq_get_status(b, &state);
    if (state.hard_disk.commands)
      break;
    assert(i != 999);
  }
  konamigq_status_t status{};
  konamigq_get_status(b, &status);
  assert(status.hard_disk.commands == 1);
  (void)ncr_read(b, 5);
  // Let the target's source-timed command phase settle before programming
  // NCR CI_XFER, just as the machine scheduler does after selection IRQ.
  advance(b, now, now + 10000);
  konamigq_get_status(b, &status);
  assert(status.ncr.phase ==
         (write ? NCR53CF96_PHASE_DATA_OUT : NCR53CF96_PHASE_DATA_IN));
  ncr_write(b, 11, 0x08);
  ncr_write(b, 0, 0x00);
  ncr_write(b, 1, 0x02); // 512 bytes
  ncr_write(b, 14, 0);
  assert(write ? konamigq_dma5_write(b, dma_address, 128)
               : konamigq_dma5_read(b, dma_address, 128));
  ncr_write(b, 3, 0x90);
  if (sliced) {
    for (unsigned i = 0; i < 4000; ++i) {
      advance(b, now, now + 1000);
      konamigq_get_status(b, &status);
      if (!status.dma_active)
        break;
      assert(i != 3999);
    }
  } else {
    advance(b, now, now + 1000000);
  }
  if (write) {
    // DMA5 can finish feeding the NCR FIFO before its final SCSI REQ/ACK byte.
    // Keep the scheduler running until the source target enters STATUS.
    for (unsigned i = 0; i < 4000; ++i) {
      advance(b, now, now + 1000);
      konamigq_get_status(b, &status);
      if (status.ncr.phase == NCR53CF96_PHASE_STATUS)
        break;
      assert(i != 3999);
    }
  }
  konamigq_get_status(b, &status);
  assert(!status.dma_active && status.dma_bytes_remaining == 0);
  if (write) {
    // The source backend writes its current block before accepting payload,
    // then writes the complete DATA OUT block after the final byte arrives.
    assert(ctx.media_writes == 2);
    assert(arcade_chd_hd_modified_sectors(ctx.disk) == 1);
    std::array<uint8_t, 512> sector{};
    assert(arcade_chd_hd_read(ctx.disk, 0, sector.data(), sector.size()) == 0);
    for (unsigned i = 0; i < sector.size(); ++i)
      assert(sector[i] == uint8_t(0xa5 ^ i));
  } else {
    // The source backend preflights the current 512-byte block before READ(10),
    // then fetches the requested sector when its data phase begins.
    assert(ctx.media_reads == 2);
    for (unsigned i = 0; i < 512; ++i) {
      const uint8_t expected = expect_overlay
                                   ? uint8_t(0xa5 ^ i)
                                   : uint8_t((i * 13 + (i >> 4)) & 0xff);
      assert(ctx.psx_ram[dma_address + i] == expected);
    }
  }
}

void test_chd_dma(Context &ctx, bool sliced) {
  auto *b = make_board(ctx);
  uint64_t now = 0;
  transfer_ncr10(b, ctx, now, sliced, false);
  konamigq_destroy(b);
}

void test_chd_write_dma(Context &ctx) {
  auto *b = make_board(ctx);
  uint64_t now = 0;
  transfer_ncr10(b, ctx, now, true, true);
  konamigq_destroy(b);
}

void test_chd_overlay_readback(Context &ctx) {
  auto *b = make_board(ctx);
  uint64_t now = 0;
  transfer_ncr10(b, ctx, now, false, false, true);
  konamigq_destroy(b);
}
} // namespace

int main(int argc, char **argv) {
  assert(argc == 3);
  char error[256]{};
  auto *disk = arcade_chd_hd_open(argv[1], argv[2], error, sizeof(error));
  if (!disk) {
    std::fprintf(stderr, "CHD open failed: %s\n", error);
    return 2;
  }
  Context static_ctx;
  static_ctx.disk = disk;
  test_ports_masks_eeprom_irq_guest_sound(static_ctx);
  Context irq_ctx;
  irq_ctx.disk = disk;
  test_mirac_interrupt_executes_sound_guest(irq_ctx);
  Context reset_ctx;
  reset_ctx.disk = disk;
  test_dma_wait_reset(reset_ctx);
  Context eeprom_ctx;
  eeprom_ctx.disk = disk;
  test_eeprom_serial(eeprom_ctx);
  Context bulk;
  bulk.disk = disk;
  test_chd_dma(bulk, false);
  Context reentry;
  reentry.disk = disk;
  reentry.reenter_media = true;
  test_chd_dma(reentry, false);
  assert(reentry.rejected_media_reentry);
  Context sliced;
  sliced.disk = disk;
  test_chd_dma(sliced, true);
  Context write_ctx;
  write_ctx.disk = disk;
  test_chd_write_dma(write_ctx);
  assert(arcade_chd_hd_flush(disk) == 0);
  arcade_chd_hd_close(disk);
  disk = arcade_chd_hd_open(argv[1], argv[2], error, sizeof(error));
  assert(disk && !*error);
  Context reopen_ctx;
  reopen_ctx.disk = disk;
  test_chd_overlay_readback(reopen_ctx);
  arcade_chd_hd_close(disk);
  std::puts("Konami GQ guest sound, EEPROM, IRQ, and CHD READ/WRITE DMA PASS");
}
