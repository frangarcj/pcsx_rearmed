#include "ata_storage.h"
#include "ata_mame/atastorage_port.h"
#include "gnet_pccard.h"
#include "rf5c296.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr uint8_t kBsy = 0x80;
constexpr uint8_t kDrq = 0x08;
constexpr uint8_t kDrdy = 0x40;
constexpr uint8_t kErr = 0x01;
void finish_power_on_reset(ata_storage_t *ata, bool expect_ready = true);
void write_register(ata_storage_t *ata, uint32_t reg, uint16_t value);
void set_lba(ata_storage_t *ata, uint16_t count, uint8_t lba);
void read_sector_words(ata_storage_t *ata, uint8_t *out);
void write_sector_words(ata_storage_t *ata, const uint8_t *in);

class HddStatusProbe final : public ide_hdd_device_base {
public:
  HddStatusProbe() {
    m_can_identify_device = 1;
    m_num_cylinders = 1;
    m_num_heads = 1;
    m_num_sectors = 16;
    device_start();
    device_reset();
  }
  uint8_t taskfile_status() { return static_cast<uint8_t>(command_r(7)); }
  uint8_t alternate_status() { return static_cast<uint8_t>(control_r(6)); }
  void reset() { device_reset(); }
  void run_until(uint64_t ns) { assert(device_t::run_until(ns)); }

protected:
  int read_sector(uint32_t, void *) override { return 1; }
  int write_sector(uint32_t, const void *) override { return 1; }
  void set_irq_out(int) override {}
  void set_dmarq_out(int) override {}
  void set_dasp_out(int) override {}
  void set_pdiag_out(int) override {}
};

struct Disk {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(16 * 512);
  bool fail_read = false;
  bool fail_write = false;
  ata_storage_t *ata = nullptr;
  int destroy_result = 0;
  int reentry_result = 0;
};

int read_sector(void *opaque, uint32_t lba, void *out) {
  auto &disk = *static_cast<Disk *>(opaque);
  if (disk.ata) {
    disk.destroy_result = ata_storage_try_destroy(disk.ata);
    disk.reentry_result = ata_storage_run_until(
        disk.ata, ata_storage_now(disk.ata));
  }
  if (disk.fail_read || lba >= 16)
    return 0;
  std::memcpy(out, disk.bytes.data() + lba * 512, 512);
  return 1; // MAME read_sector convention: nonzero means success.
}

int write_sector(void *opaque, uint32_t lba, const void *in) {
  auto &disk = *static_cast<Disk *>(opaque);
  if (disk.ata) {
    disk.destroy_result = ata_storage_try_destroy(disk.ata);
    disk.reentry_result = ata_storage_run_until(
        disk.ata, ata_storage_now(disk.ata));
  }
  if (disk.fail_write || lba >= 16)
    return 0;
  std::memcpy(disk.bytes.data() + lba * 512, in, 512);
  return 1;
}

struct Lines {
  int irq = 0;
  int dmarq = 0;
  int dasp = 0;
  ata_storage_t *ata = nullptr;
  bool probe_run_until_on_irq = false;
  int irq_reentry_result = 0;
};
void irq_line(void *opaque, int state) {
  auto &lines = *static_cast<Lines *>(opaque);
  lines.irq = state;
  if (state && lines.probe_run_until_on_irq) {
    lines.probe_run_until_on_irq = false;
    lines.irq_reentry_result =
        ata_storage_run_until(lines.ata, ata_storage_now(lines.ata));
  }
}
void dmarq_line(void *opaque, int state) { static_cast<Lines *>(opaque)->dmarq = state; }
void dasp_line(void *opaque, int state) { static_cast<Lines *>(opaque)->dasp = state; }

ata_storage_t *make_ata(Disk &disk, Lines &lines,
                        const uint8_t *identify = nullptr,
                        size_t identify_size = 0,
                        int device_kind = ATA_STORAGE_DEVICE_COMPACTFLASH) {
  ata_storage_config_t config{};
  config.sector_count = 16;
  config.cylinders = 1;
  config.heads = 1;
  config.sectors_per_track = 16;
  config.model = "Port fidelity disk";
  config.identify_data = identify;
  config.identify_size = identify_size;
  config.device_kind = device_kind;
  config.media_opaque = &disk;
  config.read_sector = read_sector;
  config.write_sector = write_sector;
  config.line_opaque = &lines;
  config.irq = irq_line;
  config.dmarq = dmarq_line;
  config.dasp = dasp_line;
  ata_storage_t *ata = nullptr;
  assert(ata_storage_create(&config, &ata) == ATA_STORAGE_OK);
  disk.ata = ata;
  lines.ata = ata;
  return ata;
}

void test_geometry_signed_capacity_limit() {
  Disk disk;
  ata_storage_config_t config{};
  config.sector_count = 16;
  config.cylinders = 65535;
  config.heads = 255;
  config.sectors_per_track = 255;
  config.read_sector = read_sector;
  config.write_sector = write_sector;
  ata_storage_t *ata = nullptr;
  assert(ata_storage_create(&config, &ata) == ATA_STORAGE_ARGUMENT);
  assert(ata == nullptr);

  // The largest signed-int-compatible CHS capacity is accepted, and its
  // source-generated IDENTIFY words retain the geometry-derived capacity.
  config.cylinders = 33025;
  const uint64_t capacity =
      uint64_t(config.cylinders) * config.heads * config.sectors_per_track;
  assert(capacity <= uint64_t(INT32_MAX));
  assert(ata_storage_create(&config, &ata) == ATA_STORAGE_OK);
  const uint16_t *words = ata_storage_identify(ata);
  assert(words[7] == (capacity >> 16));
  assert(words[8] == (capacity & 0xffff));
  assert(words[57] == (capacity & 0xffff));
  assert(words[58] == (capacity >> 16));
  assert(words[60] == (capacity & 0xffff));
  assert(words[61] == (capacity >> 16));
  ata_storage_destroy(ata);
}

void test_hdd_rotational_index_status() {
  constexpr uint64_t rotation_ns = 1'000'000'000 / 90;
  HddStatusProbe hdd;
  assert((hdd.alternate_status() & 0x02) == 0);
  hdd.run_until(rotation_ns);
  assert((hdd.taskfile_status() & 0x02) == 0); // Source uses strict >.
  hdd.run_until(rotation_ns + 1);
  assert((hdd.alternate_status() & 0x02) != 0);

  // Resetting the ATA device does not restart the HDD status timer. The next
  // status read still reports IDX after another full elapsed rotation.
  hdd.run_until(2 * rotation_ns + 2);
  hdd.reset();
  assert((hdd.taskfile_status() & 0x02) != 0);
  assert((hdd.alternate_status() & 0x02) == 0);
  hdd.run_until(3 * rotation_ns + 3);
  assert((hdd.alternate_status() & 0x02) != 0);
}

void test_cf_keeps_source_status_override() {
  Disk disk;
  Lines lines;
  ata_storage_t *ata = make_ata(disk, lines);
  assert(ata_storage_identify(ata)[0] == 0x848a);
  finish_power_on_reset(ata, true);
  constexpr uint64_t rotation_ns = 1'000'000'000 / 90;
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + rotation_ns + 1) ==
         ATA_STORAGE_OK);
  assert((ata_storage_control_r(ata, 6, 0xffff) & 0x02) == 0);
  assert((ata_storage_command_r(ata, 7, 0xffff, 0) & 0x02) == 0);
  ata_storage_reset(ata);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + rotation_ns + 1) ==
         ATA_STORAGE_OK);
  assert((ata_storage_control_r(ata, 6, 0xffff) & 0x02) == 0);
  ata_storage_destroy(ata);
}

void test_ide_hdd_source_profile_and_pio_callbacks() {
  Disk disk;
  Lines lines;
  for (size_t i = 0; i < disk.bytes.size(); ++i)
    disk.bytes[i] = static_cast<uint8_t>(i * 17 + 3);
  ata_storage_t *ata = make_ata(disk, lines, nullptr, 0,
                                ATA_STORAGE_DEVICE_IDE_HDD);
  const uint16_t *words = ata_storage_identify(ata);
  assert(words[0] == 0x045a); // Pinned MAME IDE HDD, not CompactFlash.
  assert(words[1] == 1 && words[3] == 1 && words[6] == 16);
  assert(words[47] == 0x8010);
  assert(words[49] == 0x0f03);
  assert(words[54] == 1 && words[55] == 1 && words[56] == 16);
  assert(words[57] == 16 && words[58] == 0);
  assert(words[60] == 16 && words[61] == 0);
  assert(words[62] == 0x0007 && words[63] == 0x0407 && words[64] == 3);
  assert(words[27] == 0x506f); // Configured model string: "Port fidelity disk".

  finish_power_on_reset(ata);
  write_register(ata, 7, 0xec);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + 10'000) ==
         ATA_STORAGE_OK);
  assert(ata_storage_status(ata) & kDrq);
  assert(ata_storage_command_r(ata, 0, 0xffff, 0) == 0x045a);
  for (unsigned word = 1; word < 256; ++word)
    (void)ata_storage_command_r(ata, 0, 0xffff, 0);

  set_lba(ata, 1, 3);
  write_register(ata, 7, 0x20);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + 400) ==
         ATA_STORAGE_OK);
  assert(ata_storage_status(ata) & kDrq);
  std::array<uint8_t, 512> readback{};
  read_sector_words(ata, readback.data());
  assert(readback[0] == disk.bytes[3 * 512]);
  assert(readback[511] == disk.bytes[4 * 512 - 1]);

  disk.fail_write = true;
  set_lba(ata, 1, 5);
  write_register(ata, 7, 0x30);
  std::array<uint8_t, 512> payload{};
  payload.fill(0x6d);
  write_sector_words(ata, payload.data());
  assert(ata_storage_status(ata) & kBsy);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + 3'000'000) ==
         ATA_STORAGE_OK);
  assert((ata_storage_status(ata) & (kErr | kDrq)) == kErr);
  assert(ata_storage_error(ata) == 0x80); // Source BAD_SECTOR write failure.
  disk.fail_write = false;
  ata_storage_destroy(ata);
}

void test_ide_hdd_rotation_index_via_public_api() {
  Disk disk;
  Lines lines;
  ata_storage_t *ata = make_ata(disk, lines, nullptr, 0,
                                ATA_STORAGE_DEVICE_IDE_HDD);
  finish_power_on_reset(ata);
  constexpr uint64_t rotation_ns = 1'000'000'000 / 90;
  assert(ata_storage_run_until(ata, rotation_ns + 1) == ATA_STORAGE_OK);
  assert(ata_storage_control_r(ata, 6, 0xffff) & 0x02);
  assert(!(ata_storage_command_r(ata, 7, 0xffff, 0) & 0x02));
  ata_storage_reset(ata);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + rotation_ns + 1) ==
         ATA_STORAGE_OK);
  assert(ata_storage_control_r(ata, 6, 0xffff) & 0x02);
  ata_storage_destroy(ata);
}

void finish_power_on_reset(ata_storage_t *ata, bool expect_ready) {
  assert(ata_storage_status(ata) & kBsy);
  assert(ata_storage_next_event_ns(nullptr) == UINT64_MAX);
  assert(ata_storage_next_event_ns(ata) == 2'000'000);
  assert(ata_storage_run_until(ata, 2'000'000) == ATA_STORAGE_OK);
  assert(ata_storage_status(ata) & kBsy); // device detection starts diagnostics.
  assert(ata_storage_next_event_ns(ata) == 4'000'000);
  assert(ata_storage_run_until(ata, 4'000'000) == ATA_STORAGE_OK);
  assert(ata_storage_next_event_ns(ata) == UINT64_MAX);
  assert(!(ata_storage_status(ata) & kBsy));
  if (expect_ready)
    assert(ata_storage_status(ata) & kDrdy);
}

void write_register(ata_storage_t *ata, uint32_t reg, uint16_t value) {
  ata_storage_command_w(ata, reg, value, 0xffff, 0);
}
void set_lba(ata_storage_t *ata, uint16_t count, uint8_t lba) {
  write_register(ata, 2, count);
  write_register(ata, 3, lba);
  write_register(ata, 4, 0);
  write_register(ata, 5, 0);
  write_register(ata, 6, 0x40);
}
void read_sector_words(ata_storage_t *ata, uint8_t *out) {
  for (unsigned i = 0; i < 256; ++i) {
    const uint16_t word = ata_storage_command_r(ata, 0, 0xffff, 0);
    out[i * 2] = static_cast<uint8_t>(word);
    out[i * 2 + 1] = static_cast<uint8_t>(word >> 8);
  }
}
void write_sector_words(ata_storage_t *ata, const uint8_t *in) {
  for (unsigned i = 0; i < 256; ++i) {
    const uint16_t word = uint16_t(in[i * 2]) | (uint16_t(in[i * 2 + 1]) << 8);
    ata_storage_command_w(ata, 0, word, 0xffff, 0);
  }
}

void test_pio_read_write_and_multiple() {
  Disk disk;
  Lines lines;
  for (size_t i = 0; i < disk.bytes.size(); ++i)
    disk.bytes[i] = static_cast<uint8_t>(i * 13 + 7);
  ata_storage_t *ata = make_ata(disk, lines);
  finish_power_on_reset(ata);

  set_lba(ata, 2, 0);
  write_register(ata, 7, 0x20);
  assert(ata_storage_status(ata) & kBsy);
  assert(ata_storage_run_until(ata, ata_storage_now(ata)) == ATA_STORAGE_OK);
  assert(ata_storage_status(ata) & kDrq);
  assert(lines.irq == 1);
  std::array<uint8_t, 512> first{};
  read_sector_words(ata, first.data());
  assert(ata_storage_status(ata) & kBsy);
  assert(ata_storage_sector_count(ata) == 1);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + 400) == ATA_STORAGE_OK);
  assert(ata_storage_status(ata) & kDrq);
  std::array<uint8_t, 512> second{};
  read_sector_words(ata, second.data());
  assert(first[0] == disk.bytes[0] && first[511] == disk.bytes[511]);
  assert(second[0] == disk.bytes[512] && second[511] == disk.bytes[1023]);
  assert(ata_storage_sector_count(ata) == 0);

  std::array<uint8_t, 512> out_a{}, out_b{};
  for (unsigned i = 0; i < 512; ++i) {
    out_a[i] = static_cast<uint8_t>(i ^ 0x39);
    out_b[i] = static_cast<uint8_t>(i ^ 0xa6);
  }
  set_lba(ata, 2, 4);
  write_register(ata, 7, 0x30);
  assert(ata_storage_status(ata) & kDrq);
  write_sector_words(ata, out_a.data());
  assert(ata_storage_status(ata) & kBsy);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + 100'000) ==
         ATA_STORAGE_OK);
  assert(ata_storage_status(ata) & kDrq);
  write_sector_words(ata, out_b.data());
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + 100'000) ==
         ATA_STORAGE_OK);
  assert(std::memcmp(disk.bytes.data() + 4 * 512, out_a.data(), 512) == 0);
  assert(std::memcmp(disk.bytes.data() + 5 * 512, out_b.data(), 512) == 0);
  assert(ata_storage_sector_count(ata) == 0);
  assert(disk.destroy_result == ATA_STORAGE_BUSY);
  assert(disk.reentry_result == ATA_STORAGE_BUSY);
  assert(ata_storage_try_destroy(ata) == ATA_STORAGE_OK);
}

void test_identify_error_feature_and_reset() {
  Disk disk;
  Lines lines;
  std::array<uint8_t, 512> identify{};
  identify[0] = 0x34;
  identify[1] = 0x12;
  identify[120] = 0xef;
  identify[121] = 0xbe;
  ata_storage_t *ata = make_ata(disk, lines, identify.data(), identify.size());
  finish_power_on_reset(ata);
  write_register(ata, 7, 0xec);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + 10'000) ==
         ATA_STORAGE_OK);
  assert(ata_storage_status(ata) & kDrq);
  assert(ata_storage_command_r(ata, 0, 0xffff, 0) == 0x1234);
  for (unsigned i = 1; i < 256; ++i)
    (void)ata_storage_command_r(ata, 0, 0xffff, 0);
  assert(!(ata_storage_status(ata) & kDrq));

  write_register(ata, 1, 0x01);
  write_register(ata, 7, 0xef);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + 10'000) ==
         ATA_STORAGE_OK);
  write_register(ata, 7, 0xec);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + 10'000) ==
         ATA_STORAGE_OK);
  assert(ata_storage_command_r(ata, 0, 0x00ff, 1) == 0x34);
  assert(ata_storage_command_r(ata, 0, 0x00ff, 1) == 0x12);
  for (unsigned i = 2; i < 512; ++i)
    (void)ata_storage_command_r(ata, 0, 0x00ff, 1);

  // NIEN masks the external pin while preserving the internal pending IRQ.
  ata_storage_control_w(ata, 6, 2, 0xffff);
  write_register(ata, 2, 2);
  write_register(ata, 7, 0xc6);
  assert(lines.irq == 0);
  ata_storage_control_w(ata, 6, 0, 0xffff);
  assert(lines.irq == 1);
  (void)ata_storage_command_r(ata, 7, 0xffff, 0);
  assert(lines.irq == 0);

  // SET BLOCK COUNT asserts IRQ synchronously from command_w. A line callback
  // must not advance either timer in the middle of this register operation.
  lines.probe_run_until_on_irq = true;
  write_register(ata, 2, 2);
  write_register(ata, 7, 0xc6);
  assert(lines.irq_reentry_result == ATA_STORAGE_BUSY);
  assert(lines.irq == 1);
  (void)ata_storage_command_r(ata, 7, 0xffff, 0);
  assert(lines.irq == 0);

  disk.fail_read = true;
  set_lba(ata, 1, 3);
  write_register(ata, 7, 0x20);
  assert(ata_storage_run_until(ata, ata_storage_now(ata)) == ATA_STORAGE_OK);
  assert((ata_storage_status(ata) & kErr) != 0);
  assert(ata_storage_error(ata) == 0x80);
  disk.fail_read = false;

  ata_storage_control_w(ata, 6, 4, 0xffff);
  assert(ata_storage_status(ata) & kBsy);
  ata_storage_control_w(ata, 6, 0, 0xffff);
  assert(ata_storage_status(ata) & kBsy);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + 2'000'000) ==
         ATA_STORAGE_OK);
  assert(!(ata_storage_status(ata) & kBsy));
  ata_storage_destroy(ata);
}

void test_dma_read_write_and_dma_mode_selection() {
  Disk disk;
  Lines lines;
  std::array<uint8_t, 512> identify{};
  identify[124] = 0x07; // word 62: SWDMA modes 0-2 supported.
  identify[126] = 0x07; // word 63: MWDMA modes 0-2 supported.
  ata_storage_t *ata = make_ata(disk, lines, identify.data(), identify.size());
  finish_power_on_reset(ata);

  write_register(ata, 1, 0x03);
  write_register(ata, 2, 0x22); // Select multiword DMA mode 2.
  write_register(ata, 7, 0xef);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + 10'000) ==
         ATA_STORAGE_OK);
  assert(ata_storage_identify(ata)[63] == 0x0407);

  for (size_t i = 0; i < disk.bytes.size(); ++i)
    disk.bytes[i] = static_cast<uint8_t>(i * 9 + 5);
  set_lba(ata, 2, 1);
  write_register(ata, 7, 0xc8);
  assert(ata_storage_run_until(ata, ata_storage_now(ata)) == ATA_STORAGE_OK);
  assert(lines.dmarq == 1);
  ata_storage_set_dmack(ata, 1);
  std::array<uint8_t, 512> got_a{};
  for (unsigned i = 0; i < 256; ++i) {
    const uint16_t word = ata_storage_dma_r(ata);
    got_a[2 * i] = static_cast<uint8_t>(word);
    got_a[2 * i + 1] = static_cast<uint8_t>(word >> 8);
  }
  ata_storage_set_dmack(ata, 0);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + 400) ==
         ATA_STORAGE_OK);
  assert(lines.dmarq == 1);
  ata_storage_set_dmack(ata, 1);
  std::array<uint8_t, 512> got_b{};
  for (unsigned i = 0; i < 256; ++i) {
    const uint16_t word = ata_storage_dma_r(ata);
    got_b[2 * i] = static_cast<uint8_t>(word);
    got_b[2 * i + 1] = static_cast<uint8_t>(word >> 8);
  }
  ata_storage_set_dmack(ata, 0);
  assert(got_a[0] == disk.bytes[512] && got_a[511] == disk.bytes[1023]);
  assert(got_b[0] == disk.bytes[1024] && got_b[511] == disk.bytes[1535]);
  assert(!(ata_storage_status(ata) & kDrq));

  std::array<uint8_t, 512> write_a{}, write_b{};
  for (unsigned i = 0; i < 512; ++i) {
    write_a[i] = static_cast<uint8_t>(i ^ 0x5a);
    write_b[i] = static_cast<uint8_t>(i ^ 0xc3);
  }
  set_lba(ata, 2, 7);
  write_register(ata, 7, 0xca);
  assert(lines.dmarq == 1);
  ata_storage_set_dmack(ata, 1);
  for (unsigned i = 0; i < 256; ++i)
    ata_storage_dma_w(ata, uint16_t(write_a[2 * i]) |
                               (uint16_t(write_a[2 * i + 1]) << 8));
  ata_storage_set_dmack(ata, 0);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + 100'000) ==
         ATA_STORAGE_OK);
  assert(lines.dmarq == 1);
  ata_storage_set_dmack(ata, 1);
  for (unsigned i = 0; i < 256; ++i)
    ata_storage_dma_w(ata, uint16_t(write_b[2 * i]) |
                               (uint16_t(write_b[2 * i + 1]) << 8));
  ata_storage_set_dmack(ata, 0);
  assert(ata_storage_run_until(ata, ata_storage_now(ata) + 100'000) ==
         ATA_STORAGE_OK);
  assert(std::memcmp(disk.bytes.data() + 7 * 512, write_a.data(), 512) == 0);
  assert(std::memcmp(disk.bytes.data() + 8 * 512, write_b.data(), 512) == 0);
  ata_storage_destroy(ata);
}

void test_security_password_unlock() {
  Disk disk;
  Lines lines;
  std::array<uint8_t, 34> password{};
  password[0] = 1;
  password[1] = 0;
  for (unsigned i = 2; i < password.size(); ++i)
    password[i] = static_cast<uint8_t>(0x30 + i);

  ata_storage_config_t config{};
  config.sector_count = 16;
  config.cylinders = 1;
  config.heads = 1;
  config.sectors_per_track = 16;
  config.user_password = password.data();
  config.user_password_size = password.size();
  config.media_opaque = &disk;
  config.read_sector = read_sector;
  config.write_sector = write_sector;
  config.line_opaque = &lines;
  config.irq = irq_line;
  config.dmarq = dmarq_line;
  config.dasp = dasp_line;
  ata_storage_t *ata = nullptr;
  assert(ata_storage_create(&config, &ata) == ATA_STORAGE_OK);
  disk.ata = ata;
  finish_power_on_reset(ata);

  set_lba(ata, 1, 0);
  write_register(ata, 7, 0x20);
  assert((ata_storage_status(ata) & (kErr | kDrdy)) == kErr);
  assert(ata_storage_error(ata) == 0);

  write_register(ata, 7, 0xf2);
  assert(ata_storage_status(ata) & kDrq);
  std::array<uint8_t, 512> unlock{};
  std::copy(password.begin(), password.end(), unlock.begin());
  write_sector_words(ata, unlock.data());
  assert(!(ata_storage_status(ata) & kErr));

  set_lba(ata, 1, 0);
  write_register(ata, 7, 0x20);
  assert(ata_storage_run_until(ata, ata_storage_now(ata)) == ATA_STORAGE_OK);
  assert(ata_storage_status(ata) & kDrq);
  ata_storage_destroy(ata);
}

void test_taito_type2_unlock_through_real_engine() {
  Disk disk;
  Lines lines;
  ata_storage_t *ata = make_ata(disk, lines);
  const uint8_t key[5] = {0x12, 0x34, 0x56, 0x78, 0x9a};
  const auto *ata_ops = ata_storage_gnet_pccard_ops(ata);
  gnet_pccard_config_t card_config{};
  card_config.variant = GNET_PCCARD_TAITO_TYPE2;
  card_config.key = key;
  card_config.key_size = sizeof(key);
  card_config.ata = ata_ops;
  gnet_pccard_t *card = nullptr;
  assert(gnet_pccard_create(&card_config, &card) == GNET_PCCARD_OK);
  assert(ata_storage_set_pccard(ata, card) == ATA_STORAGE_OK);
  assert(gnet_pccard_is_locked(card));
  finish_power_on_reset(ata, false);

  write_register(ata, 7, 0xfe);
  assert(ata_storage_status(ata) & kDrdy);
  assert(ata_storage_sector_count(ata) == 1);
  write_register(ata, 7, 0xfc);
  assert(ata_storage_status(ata) & kDrq);
  for (unsigned byte = 0; byte < 512; byte += 2) {
    uint8_t lo = 0, hi = 0;
    if (byte >= 2 && byte < 7)
      lo = key[byte - 2];
    if (byte + 1 >= 2 && byte + 1 < 7)
      hi = key[byte + 1 - 2];
    ata_storage_command_w(ata, 0, uint16_t(lo) | (uint16_t(hi) << 8),
                          0xffff, 0);
  }
  assert(gnet_pccard_is_ready(card));
  assert(!(ata_storage_status(ata) & kErr));
  assert(gnet_pccard_try_destroy(card) == GNET_PCCARD_OK);
  ata_storage_destroy(ata);
}

void test_get_card_ops_pointer_adjustment_through_rf() {
  for (int kind : {ATA_STORAGE_DEVICE_COMPACTFLASH,
                   ATA_STORAGE_DEVICE_IDE_HDD}) {
    Disk disk;
    Lines lines;
    ata_storage_t *ata = make_ata(disk, lines, nullptr, 0, kind);
    gnet_pccard_config_t card_config{};
    card_config.variant = GNET_PCCARD_ATA_FLASH;
    card_config.ata = ata_storage_gnet_pccard_ops(ata);
    gnet_pccard_t *card = nullptr;
    assert(gnet_pccard_create(&card_config, &card) == GNET_PCCARD_OK);
    assert(ata_storage_set_pccard(ata, card) == ATA_STORAGE_OK);
    rf5c296_device_t *rf = rf5c296_create(gnet_pccard_card_ops(card));
    assert(rf);
    finish_power_on_reset(ata);

    const uint16_t command_mask = 0xff00;
    rf5c296_io_write(rf, 3, uint16_t(0xec) << 8, command_mask);
    assert(ata_storage_run_until(ata, ata_storage_now(ata) + 10'000) ==
           ATA_STORAGE_OK);
    assert(ata_storage_status(ata) & kDrq);
    /* Pull data through the PC Card memory callbacks all the way to the ATA
     * C ABI bridge; both derived device bases exercise its adjusted pointer. */
    for (unsigned word = 0; word < 256; ++word)
      (void)rf5c296_io_read(rf, 0, 0xffff);
    assert(!(ata_storage_status(ata) & kDrq));

    rf5c296_destroy(rf);
    assert(ata_storage_set_pccard(ata, nullptr) == ATA_STORAGE_OK);
    assert(gnet_pccard_try_destroy(card) == GNET_PCCARD_OK);
    assert(ata_storage_try_destroy(ata) == ATA_STORAGE_OK);
  }
}
} // namespace

int main() {
  test_geometry_signed_capacity_limit();
  test_hdd_rotational_index_status();
  test_cf_keeps_source_status_override();
  test_ide_hdd_source_profile_and_pio_callbacks();
  test_ide_hdd_rotation_index_via_public_api();
  test_pio_read_write_and_multiple();
  test_identify_error_feature_and_reset();
  test_dma_read_write_and_dma_mode_selection();
  test_security_password_unlock();
  test_taito_type2_unlock_through_real_engine();
  test_get_card_ops_pointer_adjustment_through_rf();
  std::puts("ATA storage source-port tests passed");
  return 0;
}
