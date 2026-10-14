/* C ABI and media/card adapter for the source-preserved MAME ATA classes.
 * The command engine itself lives in ata_mame/ and is regenerated from the
 * source pin documented in tools/port_mame_ata.py.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "ata_storage.h"

#include "ata_mame/atastorage_port.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string>

struct ata_storage {
  virtual ~ata_storage() = default;
  virtual uint16_t command_r(uint32_t offset, uint16_t mem_mask,
                             int eight_bit) = 0;
  virtual void command_w(uint32_t offset, uint16_t data, uint16_t mem_mask,
                         int eight_bit) = 0;
  virtual uint16_t control_r(uint32_t offset, uint16_t mem_mask) = 0;
  virtual void control_w(uint32_t offset, uint16_t data, uint16_t mem_mask) = 0;
  virtual uint16_t dma_r() = 0;
  virtual void dma_w(uint16_t data) = 0;
  virtual void set_csel(int state) = 0;
  virtual void set_dmack(int state) = 0;
  virtual void set_dasp_input(int state) = 0;
  virtual void set_pdiag_input(int state) = 0;
  virtual int advance(uint64_t ns) = 0;
  virtual uint64_t now() const = 0;
  virtual uint8_t status() const = 0;
  virtual uint8_t error() const = 0;
  virtual uint16_t sector_count() const = 0;
  virtual const uint16_t *identify() const = 0;
  virtual int try_destroy() const = 0;
  virtual int attach_card(gnet_pccard_t *card) = 0;
  virtual void reset_adapter() = 0;
  virtual const gnet_pccard_ata_ops_t *get_card_ops() = 0;
};

template <typename DeviceBase>
struct ata_storage_device final : public DeviceBase, public ata_storage {
public:
  using DeviceBase::ide_build_identify_device;
  using DeviceBase::m_8bit_data_transfers;
  using DeviceBase::m_buffer;
  using DeviceBase::m_buffer_size;
  using DeviceBase::m_can_identify_device;
  using DeviceBase::m_command;
  using DeviceBase::m_cylinder_high;
  using DeviceBase::m_cylinder_low;
  using DeviceBase::m_error;
  using DeviceBase::m_feature;
  using DeviceBase::m_identify_buffer;
  using DeviceBase::m_num_cylinders;
  using DeviceBase::m_num_heads;
  using DeviceBase::m_num_sectors;
  using DeviceBase::m_sector_count;
  using DeviceBase::m_sector_number;
  using DeviceBase::m_status;
  using DeviceBase::set_csel_in;
  using DeviceBase::set_dasp_in;
  using DeviceBase::set_dmack_in;
  using DeviceBase::set_dma_transfer_time;
  using DeviceBase::set_irq;
  using DeviceBase::set_master_password;
  using DeviceBase::set_pdiag_in;
  using DeviceBase::set_user_password;

  explicit ata_storage_device(const ata_storage_config_t &config)
      : config(config) {
    m_num_cylinders = config.cylinders;
    m_num_heads = config.heads;
    m_num_sectors = config.sectors_per_track;
    if (!m_num_cylinders && m_num_heads && m_num_sectors)
      m_num_cylinders = static_cast<uint16_t>(std::min<uint32_t>(
          65535, config.sector_count /
                     (uint32_t(m_num_heads) * m_num_sectors)));
    set_user_password(config.user_password);
    set_master_password(config.master_password);
    set_dma_transfer_time(attotime(static_cast<int64_t>(
        std::min<uint64_t>(config.dma_transfer_time_ns,
                           uint64_t(std::numeric_limits<int64_t>::max())))));

    DeviceBase::device_start();
    m_can_identify_device = 1;
    if (config.identify_size == 512) {
      for (int word = 0; word < 256; ++word)
        m_identify_buffer[word] = get_u16le(config.identify_data + word * 2);
    } else {
      ide_build_identify_device();
      apply_identify_strings();
    }
    DeviceBase::device_reset();
  }

  int read_sector(uint32_t lba, void *buffer) override {
    if (!config.read_sector)
      return 0;
    OperationGuard guard(*this);
    try {
      return config.read_sector(config.media_opaque, lba, buffer);
    } catch (...) {
      return 0;
    }
  }

  int write_sector(uint32_t lba, const void *buffer) override {
    if (!config.write_sector)
      return 0;
    OperationGuard guard(*this);
    try {
      return config.write_sector(config.media_opaque, lba, buffer);
    } catch (...) {
      return 0;
    }
  }

  bool is_ready() override {
    if (card) {
      OperationGuard guard(*this);
      return gnet_pccard_is_ready(card) != 0;
    }
    return ata_mass_storage_device_base::is_ready();
  }

  void process_command() override {
    if (card && offer_command())
      return;
    ata_mass_storage_device_base::process_command();
  }

  void process_buffer() override {
    if (card && offer_buffer())
      return;
    ata_mass_storage_device_base::process_buffer();
  }

  uint16_t command_r(uint32_t offset, uint16_t mem_mask,
                     int eight_bit) override {
    OperationGuard guard(*this);
    if (eight_bit >= 0) m_8bit_data_transfers = eight_bit != 0;
    (void)mem_mask; // MAME's PC Card adapter converts this lane test to the flag.
    return ata_hle_device_base::command_r(offset);
  }
  void command_w(uint32_t offset, uint16_t data, uint16_t mem_mask,
                 int eight_bit) override {
    OperationGuard guard(*this);
    if (eight_bit >= 0) m_8bit_data_transfers = eight_bit != 0;
    (void)mem_mask;
    ata_hle_device_base::command_w(offset, data);
  }
  uint16_t control_r(uint32_t offset, uint16_t mem_mask) override {
    OperationGuard guard(*this);
    (void)mem_mask;
    return ata_hle_device_base::control_r(offset);
  }
  void control_w(uint32_t offset, uint16_t data,
                 uint16_t mem_mask) override {
    OperationGuard guard(*this);
    (void)mem_mask;
    ata_hle_device_base::control_w(offset, data);
  }
  uint16_t dma_r() override {
    OperationGuard guard(*this);
    return ata_hle_device_base::dma_r();
  }
  void dma_w(uint16_t data) override {
    OperationGuard guard(*this);
    ata_hle_device_base::dma_w(data);
  }
  void set_csel(int state) override {
    OperationGuard guard(*this);
    set_csel_in(state);
  }
  void set_dmack(int state) override {
    OperationGuard guard(*this);
    set_dmack_in(state);
  }
  void set_dasp_input(int state) override {
    OperationGuard guard(*this);
    set_dasp_in(state);
  }
  void set_pdiag_input(int state) override {
    OperationGuard guard(*this);
    set_pdiag_in(state);
  }
  int advance(uint64_t ns) override {
    // Advancing from a callback or from another public API call would run
    // timers halfway through the operation that caused that callback. The
    // scheduler itself is the only operation allowed to enter run_until.
    if (running_timers || operation_depth || callback_depth)
      return ATA_STORAGE_BUSY;
    OperationGuard guard(*this);
    running_timers = true;
    const bool valid = device_t::run_until(ns);
    running_timers = false;
    return valid ? ATA_STORAGE_OK : ATA_STORAGE_ARGUMENT;
  }
  uint64_t now() const override { return device_t::now(); }
  uint8_t status() const override { return m_status; }
  uint8_t error() const override { return m_error; }
  uint16_t sector_count() const override { return m_sector_count; }
  const uint16_t *identify() const override { return m_identify_buffer; }
  int try_destroy() const override {
    return callback_depth || operation_depth ? ATA_STORAGE_BUSY : ATA_STORAGE_OK;
  }
  int attach_card(gnet_pccard_t *new_card) override {
    card = new_card;
    return ATA_STORAGE_OK;
  }

  void reset_adapter() override {
    OperationGuard guard(*this);
    DeviceBase::device_reset();
  }
  const gnet_pccard_ata_ops_t *get_card_ops() override {
    card_ops = {static_cast<ata_storage *>(this), bridge_reset,
                bridge_command_r, bridge_command_w,
                bridge_control_r, bridge_control_w};
    return &card_ops;
  }

private:
  struct OperationGuard {
    explicit OperationGuard(ata_storage_device &owner) : owner(owner) {
      ++owner.operation_depth;
    }
    ~OperationGuard() { --owner.operation_depth; }
    ata_storage_device &owner;
  };
  struct CallbackGuard {
    explicit CallbackGuard(ata_storage_device &owner) : owner(owner) {
      ++owner.callback_depth;
    }
    ~CallbackGuard() { --owner.callback_depth; }
    ata_storage_device &owner;
  };

  void apply_identify_strings() {
    auto set_words = [](uint16_t *dst, const char *text, size_t words) {
      for (size_t i = 0; i < words; ++i) {
        const unsigned char hi = text && *text ? *text++ : ' ';
        const unsigned char lo = text && *text ? *text++ : ' ';
        dst[i] = uint16_t((hi << 8) | lo);
      }
    };
    if (config.serial)
      set_words(&m_identify_buffer[10], config.serial, 10);
    if (config.firmware)
      set_words(&m_identify_buffer[23], config.firmware, 4);
    if (config.model)
      set_words(&m_identify_buffer[27], config.model, 20);
  }

  bool offer_command() {
    gnet_pccard_taskfile_t taskfile{};
    taskfile.command = m_command;
    taskfile.feature = m_feature;
    taskfile.sector_count = m_sector_count;
    taskfile.sector_number = m_sector_number;
    taskfile.cylinder_low = m_cylinder_low;
    taskfile.cylinder_high = m_cylinder_high;
    taskfile.status = m_status;
    gnet_pccard_state_delta_t delta{};
    CallbackGuard guard(*this);
    const auto result =
        gnet_pccard_process_command(card, &taskfile, &delta);
    apply_delta(delta);
    return result == GNET_PCCARD_HOOK_HANDLED;
  }
  bool offer_buffer() {
    gnet_pccard_state_delta_t delta{};
    CallbackGuard guard(*this);
    const auto result = gnet_pccard_process_buffer(
        card, m_command, m_buffer.data(), m_buffer_size, &delta);
    apply_delta(delta);
    return result == GNET_PCCARD_HOOK_HANDLED;
  }
  void apply_delta(const gnet_pccard_state_delta_t &delta) {
    m_status |= delta.status_set;
    m_status &= static_cast<uint8_t>(~delta.status_clear);
    if (delta.error_valid)
      m_error = delta.error;
    if (delta.sector_count_valid)
      m_sector_count = delta.sector_count;
    if (delta.transfer_bytes_valid)
      m_buffer_size = std::min<uint16_t>(delta.transfer_bytes,
                                         m_buffer.size());
    if (delta.irq_valid)
      set_irq(delta.irq ? ASSERT_LINE : CLEAR_LINE);
  }

  void set_irq_out(int state) override {
    set_line(config.irq, irq_output, state);
  }
  void set_dmarq_out(int state) override {
    set_line(config.dmarq, dmarq_output, state);
  }
  void set_dasp_out(int state) override {
    set_line(config.dasp, dasp_output, state);
  }
  void set_pdiag_out(int state) override {
    set_line(config.pdiag, pdiag_output, state);
  }
  void set_line(ata_storage_line_fn fn, int &current, int state) {
    if (current == state)
      return;
    current = state;
    if (fn) {
      CallbackGuard guard(*this);
      try {
        fn(config.line_opaque, state);
      } catch (...) {
        // A line observer cannot unwind through the C ABI or the source timer.
      }
    }
  }

  static void bridge_reset(void *opaque) {
    static_cast<ata_storage *>(opaque)->reset_adapter();
  }
  static uint16_t bridge_command_r(void *opaque, uint32_t offset,
                                   uint16_t mem_mask, int eight_bit) {
    return static_cast<ata_storage *>(opaque)->command_r(
        offset, mem_mask, eight_bit != 0);
  }
  static void bridge_command_w(void *opaque, uint32_t offset, uint16_t data,
                               uint16_t mem_mask, int eight_bit) {
    static_cast<ata_storage *>(opaque)->command_w(
        offset, data, mem_mask, eight_bit != 0);
  }
  static uint16_t bridge_control_r(void *opaque, uint32_t offset,
                                   uint16_t mem_mask) {
    return static_cast<ata_storage *>(opaque)->control_r(offset, mem_mask);
  }
  static void bridge_control_w(void *opaque, uint32_t offset, uint16_t data,
                               uint16_t mem_mask) {
    static_cast<ata_storage *>(opaque)->control_w(offset, data, mem_mask);
  }

  ata_storage_config_t config;
  gnet_pccard_t *card = nullptr;
  gnet_pccard_ata_ops_t card_ops{};
  int irq_output = CLEAR_LINE, dmarq_output = CLEAR_LINE,
      dasp_output = CLEAR_LINE, pdiag_output = CLEAR_LINE;
  unsigned callback_depth = 0, operation_depth = 0;
  bool running_timers = false;
};

extern "C" int ata_storage_create(const ata_storage_config_t *config,
                                    ata_storage_t **out) {
  if (out)
    *out = nullptr;
  if (!config || !out || !config->sector_count || !config->heads ||
      !config->sectors_per_track || !config->read_sector ||
      !config->write_sector ||
      (config->user_password && config->user_password_size != 34) ||
      (!config->user_password && config->user_password_size) ||
      (config->master_password && config->master_password_size != 34) ||
      (!config->master_password && config->master_password_size) ||
      (config->identify_size &&
       (!config->identify_data || config->identify_size != 512)) ||
      (config->device_kind != ATA_STORAGE_DEVICE_COMPACTFLASH &&
       config->device_kind != ATA_STORAGE_DEVICE_IDE_HDD) ||
      config->dma_transfer_time_ns >
          uint64_t(std::numeric_limits<int64_t>::max()))
    return ATA_STORAGE_ARGUMENT;

  // The pinned MAME source computes total_sectors in signed int in both the
  // CF IDENTIFY builder and finished_command(). Validate the exact geometry
  // the constructor will install before invoking either source body.
  const uint64_t sectors_per_cylinder =
      uint64_t(config->heads) * config->sectors_per_track;
  const uint64_t cylinders = config->cylinders
                                 ? config->cylinders
                                 : std::min<uint64_t>(
                                       65535, config->sector_count /
                                                  sectors_per_cylinder);
  if (cylinders * sectors_per_cylinder >
      uint64_t(std::numeric_limits<int>::max()))
    return ATA_STORAGE_ARGUMENT;

  ata_storage *ata = nullptr;
  try {
    if (config->device_kind == ATA_STORAGE_DEVICE_IDE_HDD)
      ata = new (std::nothrow)
          ata_storage_device<ide_hdd_device_base>(*config);
    else
      ata = new (std::nothrow) ata_storage_device<cf_device_base>(*config);
  } catch (const std::bad_alloc &) {
    return ATA_STORAGE_NOMEM;
  }
  if (!ata)
    return ATA_STORAGE_NOMEM;
  *out = ata;
  return ATA_STORAGE_OK;
}

extern "C" int ata_storage_try_destroy(ata_storage_t *ata) {
  if (!ata)
    return ATA_STORAGE_ARGUMENT;
  const int result = ata->try_destroy();
  if (result == ATA_STORAGE_OK)
    delete ata;
  return result;
}
extern "C" void ata_storage_destroy(ata_storage_t *ata) {
  if (ata)
    (void)ata_storage_try_destroy(ata);
}
extern "C" void ata_storage_reset(ata_storage_t *ata) {
  if (ata)
    ata->reset_adapter();
}
extern "C" int ata_storage_run_until(ata_storage_t *ata, uint64_t ns) {
  return ata ? ata->advance(ns) : ATA_STORAGE_ARGUMENT;
}
extern "C" uint64_t ata_storage_now(const ata_storage_t *ata) {
  return ata ? ata->now() : 0;
}
extern "C" uint16_t ata_storage_command_r(ata_storage_t *ata, uint32_t offset,
                                            uint16_t mask, int eight_bit) {
  return ata ? ata->command_r(offset, mask, eight_bit) : 0xffff;
}
extern "C" void ata_storage_command_w(ata_storage_t *ata, uint32_t offset,
                                        uint16_t data, uint16_t mask,
                                        int eight_bit) {
  if (ata)
    ata->command_w(offset, data, mask, eight_bit);
}
extern "C" uint16_t ata_storage_control_r(ata_storage_t *ata, uint32_t offset,
                                            uint16_t mask) {
  return ata ? ata->control_r(offset, mask) : 0xffff;
}
extern "C" void ata_storage_control_w(ata_storage_t *ata, uint32_t offset,
                                        uint16_t data, uint16_t mask) {
  if (ata)
    ata->control_w(offset, data, mask);
}
extern "C" uint16_t ata_storage_dma_r(ata_storage_t *ata) {
  return ata ? ata->dma_r() : 0xffff;
}
extern "C" void ata_storage_dma_w(ata_storage_t *ata, uint16_t data) {
  if (ata)
    ata->dma_w(data);
}
extern "C" void ata_storage_set_csel(ata_storage_t *ata, int state) {
  if (ata)
    ata->set_csel(state);
}
extern "C" void ata_storage_set_dmack(ata_storage_t *ata, int state) {
  if (ata)
    ata->set_dmack(state);
}
extern "C" void ata_storage_set_dasp_in(ata_storage_t *ata, int state) {
  if (ata)
    ata->set_dasp_input(state);
}
extern "C" void ata_storage_set_pdiag_in(ata_storage_t *ata, int state) {
  if (ata)
    ata->set_pdiag_input(state);
}
extern "C" uint8_t ata_storage_status(const ata_storage_t *ata) {
  return ata ? ata->status() : 0;
}
extern "C" uint8_t ata_storage_error(const ata_storage_t *ata) {
  return ata ? ata->error() : 0;
}
extern "C" uint16_t ata_storage_sector_count(const ata_storage_t *ata) {
  return ata ? ata->sector_count() : 0;
}
extern "C" const uint16_t *ata_storage_identify(const ata_storage_t *ata) {
  return ata ? ata->identify() : nullptr;
}
extern "C" const gnet_pccard_ata_ops_t *
ata_storage_gnet_pccard_ops(ata_storage_t *ata) {
  return ata ? ata->get_card_ops() : nullptr;
}
extern "C" int ata_storage_set_pccard(ata_storage_t *ata,
                                       gnet_pccard_t *card) {
  return ata ? ata->attach_card(card) : ATA_STORAGE_ARGUMENT;
}
