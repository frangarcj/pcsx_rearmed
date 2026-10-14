/* Taito Zoom / G-Net sound-board wiring from pinned MAME source.
 * See zoom_board_LICENSE.txt for device-source attribution and terms. */
#include "zoom_board.h"

#include "mn10200.h"
#include "tms57002.h"
#include "zsg2.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

struct zoom_board {
    zoom_board_config config{};
    mn10200_cpu *cpu = nullptr;
    ZSG2 *zsg = nullptr;
    tms57002_t *dsp = nullptr;
    uint8_t sound_ram[ZOOM_SOUND_RAM_BYTES]{};
    uint8_t dsp_ram[ZOOM_DSP_DATA_BYTES]{};
    uint8_t shared[ZOOM_SHARED_BYTES]{};
    uint8_t tms_ctrl = 0;
    uint8_t reg_address = 0;
    uint64_t synced_main_cycle = 0;
    uint64_t cpu_cycle_origin = 0;
    uint64_t zsg_cycle = 0;
    uint64_t audio_frames = 0;
    float last_audio[2]{};
    float output_gain[2] = {1.0f, 1.0f};
    uint32_t fault = ZOOM_FAULT_NONE;
    bool sound_reset = true;
    bool callback_active = false;
    bool synchronizing = false;
    bool cpu_running = false;
};

namespace {
constexpr uint32_t BOOT_BASE = 0x080000;
constexpr uint32_t BOOT_END = 0x100000;
constexpr uint32_t SOUND_RAM_BASE = 0x400000;
constexpr uint32_t SOUND_RAM_END = SOUND_RAM_BASE + ZOOM_SOUND_RAM_BYTES;
constexpr uint32_t ZSG_BASE = 0x800000;
constexpr uint32_t ZSG_END = 0x800800;
constexpr uint32_t TMS_PORT = 0xc00000;
constexpr uint32_t SHARED_BASE = 0xe00000;
constexpr uint32_t SHARED_END = SHARED_BASE + ZOOM_SHARED_BYTES;
constexpr uint32_t MAIN_SHARED_BASE = 0x1fbe0000;
constexpr uint64_t ZSG_PER_MN = 4;
constexpr uint64_t TMS_PER_MN = 2;

static int call_flash_read(zoom_board *b, unsigned chip, uint32_t word,
                           uint16_t *value)
{
    if (b->config.flash_read16) {
        if (b->callback_active) {
            b->fault = ZOOM_FAULT_FLASH_CALLBACK;
            return -1;
        }
        b->callback_active = true;
        int result = b->config.flash_read16(b->config.flash_opaque, chip, word, value);
        b->callback_active = false;
        if (result != 0) b->fault = ZOOM_FAULT_FLASH_CALLBACK;
        return result;
    }
    const uint8_t *image = chip == 0 ? b->config.boot_flash : b->config.sample_flash[chip - 1];
    size_t size = chip == 0 ? b->config.boot_flash_size : b->config.sample_flash_size[chip - 1];
    size_t byte = size_t(word) * 2;
    if (!image || byte + 1 >= size) {
        *value = 0xffff;
        return 0;
    }
    *value = uint16_t(image[byte]) | (uint16_t(image[byte + 1]) << 8);
    return 0;
}

static int call_flash_write(zoom_board *b, unsigned chip, uint32_t word,
                            uint16_t data, uint16_t mask)
{
    if (!b->config.flash_write16) {
        b->fault = ZOOM_FAULT_FLASH_WRITE_UNSUPPORTED;
        return ZOOM_BOARD_UNSUPPORTED;
    }
    if (b->callback_active) {
        b->fault = ZOOM_FAULT_FLASH_CALLBACK;
        return ZOOM_BOARD_DEVICE;
    }
    b->callback_active = true;
    int result = b->config.flash_write16(b->config.flash_opaque, chip, word, data, mask);
    b->callback_active = false;
    if (result != 0) {
        b->fault = ZOOM_FAULT_FLASH_CALLBACK;
        return ZOOM_BOARD_DEVICE;
    }
    return ZOOM_BOARD_OK;
}

static uint32_t zsg_external_read(void *opaque, uint32_t word_offset)
{
    auto *b = static_cast<zoom_board *>(opaque);
    uint64_t doubled = uint64_t(word_offset) << 1;
    unsigned chip = unsigned((doubled & 0x300000u) >> 20) + 1;
    if (chip > 3) return 0;
    uint32_t word = uint32_t(doubled & 0x0fffffu);
    uint16_t lo = 0xffff, hi = 0xffff;
    if (call_flash_read(b, chip, word, &lo) != 0 ||
        call_flash_read(b, chip, word | 1u, &hi) != 0)
        return 0xffffffffu;
    return uint32_t(lo) | (uint32_t(hi) << 16);
}

static void dsp_empty_changed(void *opaque, int state)
{
    auto *b = static_cast<zoom_board *>(opaque);
    /* taito_zm.cpp wires EMPTY through an inverted MN10200 IRQ1 input. */
    if (b->cpu) (void)mn10200_set_irq(b->cpu, 1, !state);
}

static uint8_t dsp_data_read(void *opaque, uint32_t address)
{
    auto *b = static_cast<zoom_board *>(opaque);
    return address < ZOOM_DSP_DATA_BYTES ? b->dsp_ram[address] : 0xff;
}

static void dsp_data_write(void *opaque, uint32_t address, uint8_t value)
{
    auto *b = static_cast<zoom_board *>(opaque);
    if (address < ZOOM_DSP_DATA_BYTES) b->dsp_ram[address] = value;
}

static int sync_to(zoom_board *b, uint64_t main_cycle);

static int sync_cpu_callback(zoom_board *b)
{
    /* The MN10200 may notify its reset port while mn10200_create is still
       constructing it, before the returned CPU pointer can be stored here. */
    if (!b->cpu) return ZOOM_BOARD_OK;
    uint64_t local = mn10200_get_callback_cycle(b->cpu);
    if (local > std::numeric_limits<uint64_t>::max() - b->cpu_cycle_origin) {
        b->fault = ZOOM_FAULT_TIME;
        return ZOOM_BOARD_TIME;
    }
    return sync_to(b, b->cpu_cycle_origin + local);
}

static uint8_t cpu_port_read(void *opaque, unsigned port, uint8_t)
{
    auto *b = static_cast<zoom_board *>(opaque);
    (void)sync_cpu_callback(b);
    return port == 1 ? b->tms_ctrl : 0xff;
}

static void cpu_port_write(void *opaque, unsigned port, uint8_t value, uint8_t)
{
    auto *b = static_cast<zoom_board *>(opaque);
    (void)sync_cpu_callback(b);
    if (port != 1) return;
    b->tms_ctrl = value;
    tms57002_cload_w(b->dsp, value & 2);
    tms57002_pload_w(b->dsp, value & 1);
}

static uint8_t cpu_read8(void *opaque, uint32_t address);
static void cpu_write8(void *opaque, uint32_t address, uint8_t value);
static uint16_t cpu_read16(void *opaque, uint32_t address);
static void cpu_write16(void *opaque, uint32_t address, uint16_t value);

static void zsg_sample(void *opaque, const int16_t out[ZSG2_OUTPUTS], uint64_t cycle)
{
    auto *b = static_cast<zoom_board *>(opaque);
    if (!b->dsp) {
        b->fault = ZOOM_FAULT_DEVICE;
        return;
    }
    uint64_t dsp_target = cycle / 2;
    uint64_t dsp_now = tms57002_total_cycles(b->dsp);
    if (dsp_target < dsp_now ||
        (dsp_target > dsp_now && tms57002_run(b->dsp, dsp_target - dsp_now) < 0)) {
        b->fault = ZOOM_FAULT_TIME;
        return;
    }
    const float scale = 1.0f / 32768.0f;
    float input[4] = {
        float(out[0]) * (0.5f * scale), float(out[1]) * (0.5f * scale),
        float(out[2]) * scale, float(out[3]) * scale
    };
    float output[4]{};
    if (tms57002_process_sample(b->dsp, input, output) != 0) {
        b->fault = ZOOM_FAULT_DEVICE;
        return;
    }
    b->last_audio[0] = output[2] * b->output_gain[0];
    b->last_audio[1] = output[3] * b->output_gain[1];
    ++b->audio_frames;
    if (b->config.audio_frame) {
        if (b->callback_active) {
            b->fault = ZOOM_FAULT_DEVICE;
            return;
        }
        b->callback_active = true;
        b->config.audio_frame(b->config.audio_opaque, b->last_audio[0],
                              b->last_audio[1], cycle);
        b->callback_active = false;
    }
}

static void collect_device_faults(zoom_board *b)
{
    if (b->fault != ZOOM_FAULT_NONE) return;
    if (b->cpu && !b->cpu_running && mn10200_get_fault(b->cpu) != 0) {
        b->fault = ZOOM_FAULT_MN_CPU;
        return;
    }
    if (b->dsp && tms57002_callback_fault(b->dsp)) {
        b->fault = ZOOM_FAULT_TMS_CALLBACK;
        return;
    }
    if (b->dsp && tms57002_unsupported_instruction(b->dsp)) {
        b->fault = ZOOM_FAULT_TMS_UNSUPPORTED;
        return;
    }
    if (b->zsg) {
        ZSG2Status status{};
        zsg2_get_status(b->zsg, &status);
        /* MAME latches these control registers and logs their unknown
         * purpose. Authentic sound firmware initializes them during boot;
         * this source-defined behavior is not an unsupported transaction. */
        if (status.diagnostics & ~ZSG2_DIAG_UNKNOWN_CONTROL)
            b->fault = ZOOM_FAULT_ZSG_DIAGNOSTIC;
    }
}

static int sync_to(zoom_board *b, uint64_t main_cycle)
{
    if (!b || b->callback_active || b->synchronizing) {
        if (b) b->fault = ZOOM_FAULT_DEVICE;
        return ZOOM_BOARD_DEVICE;
    }
    if (main_cycle < b->synced_main_cycle ||
        main_cycle > std::numeric_limits<uint64_t>::max() / ZSG_PER_MN) {
        b->fault = ZOOM_FAULT_TIME;
        return ZOOM_BOARD_TIME;
    }
    uint64_t target_zsg = main_cycle * ZSG_PER_MN;
    uint64_t target_dsp = main_cycle * TMS_PER_MN;
    if (target_dsp > uint64_t(std::numeric_limits<int64_t>::max())) {
        b->fault = ZOOM_FAULT_TIME;
        return ZOOM_BOARD_TIME;
    }
    b->synchronizing = true;
    if (b->config.time_sync &&
        b->config.time_sync(b->config.time_opaque, main_cycle) != 0) {
        b->synchronizing = false;
        b->fault = ZOOM_FAULT_TIME;
        return ZOOM_BOARD_TIME;
    }
    int zr = 0;
    if (target_zsg > b->zsg_cycle) {
        zr = zsg2_run_cycles(b->zsg, target_zsg - b->zsg_cycle);
    }
    b->zsg_cycle = target_zsg;
    uint64_t dsp_now = tms57002_total_cycles(b->dsp);
    int64_t dr = 0;
    if (target_dsp > dsp_now) dr = tms57002_run(b->dsp, target_dsp - dsp_now);
    b->synced_main_cycle = main_cycle;
    b->synchronizing = false;
    if (zr != ZSG2_OK || dr < 0) {
        collect_device_faults(b);
        if (b->fault == ZOOM_FAULT_NONE) b->fault = ZOOM_FAULT_DEVICE;
        return ZOOM_BOARD_DEVICE;
    }
    collect_device_faults(b);
    return b->fault == ZOOM_FAULT_NONE ? ZOOM_BOARD_OK : ZOOM_BOARD_DEVICE;
}

static uint8_t cpu_read8(void *opaque, uint32_t address)
{
    auto *b = static_cast<zoom_board *>(opaque);
    address &= 0xffffff;
    if (sync_cpu_callback(b) != ZOOM_BOARD_OK) return 0xff;
    if (address >= BOOT_BASE && address < BOOT_END) {
        uint16_t v = 0xffff;
        if (call_flash_read(b, 0, (address - BOOT_BASE) >> 1, &v) != 0) return 0xff;
        return (address & 1) ? uint8_t(v >> 8) : uint8_t(v);
    }
    if (address >= SOUND_RAM_BASE && address < SOUND_RAM_END)
        return b->sound_ram[address - SOUND_RAM_BASE];
    if (address >= ZSG_BASE && address < ZSG_END) {
        /* MAME's ZSG-2 device rejects byte/partial accesses. */
        b->fault = ZOOM_FAULT_BAD_BUS_ACCESS;
        return 0;
    }
    if (address == TMS_PORT) return tms57002_data_r(b->dsp);
    if (address >= SHARED_BASE && address < SHARED_END)
        return b->shared[address - SHARED_BASE];
    return 0xff;
}

static void cpu_write8(void *opaque, uint32_t address, uint8_t value)
{
    auto *b = static_cast<zoom_board *>(opaque);
    address &= 0xffffff;
    if (sync_cpu_callback(b) != ZOOM_BOARD_OK) return;
    if (address >= BOOT_BASE && address < BOOT_END) {
        /* MAME maps this CPU window read-only. */
    } else if (address >= SOUND_RAM_BASE && address < SOUND_RAM_END) {
        b->sound_ram[address - SOUND_RAM_BASE] = value;
    } else if (address >= ZSG_BASE && address < ZSG_END) {
        b->fault = ZOOM_FAULT_BAD_BUS_ACCESS;
        (void)zsg2_write(b->zsg, (address - ZSG_BASE) >> 1, value << ((address & 1) ? 8 : 0),
                         (address & 1) ? 0xff00 : 0x00ff);
    } else if (address == TMS_PORT) {
        tms57002_data_w(b->dsp, value);
    } else if (address >= SHARED_BASE && address < SHARED_END) {
        b->shared[address - SHARED_BASE] = value;
    }
}

static uint16_t cpu_read16(void *opaque, uint32_t address)
{
    auto *b = static_cast<zoom_board *>(opaque);
    address &= 0xfffffe;
    if (sync_cpu_callback(b) != ZOOM_BOARD_OK) return 0xffff;
    if (address >= BOOT_BASE && address < BOOT_END) {
        uint16_t value = 0xffff;
        if (call_flash_read(b, 0, (address - BOOT_BASE) >> 1, &value) != 0) return 0xffff;
        return value;
    }
    if (address >= SOUND_RAM_BASE && address + 1 < SOUND_RAM_END) {
        unsigned i = address - SOUND_RAM_BASE;
        return uint16_t(b->sound_ram[i]) | (uint16_t(b->sound_ram[i + 1]) << 8);
    }
    if (address >= ZSG_BASE && address < ZSG_END) {
        uint16_t value = 0;
        int r = zsg2_read(b->zsg, (address - ZSG_BASE) >> 1, 0xffff, &value);
        if (r != ZSG2_OK) b->fault = ZOOM_FAULT_DEVICE;
        return value;
    }
    if (address == TMS_PORT) return uint16_t(tms57002_data_r(b->dsp)) | 0xff00;
    if (address >= SHARED_BASE && address + 1 < SHARED_END) {
        unsigned i = address - SHARED_BASE;
        return uint16_t(b->shared[i]) | (uint16_t(b->shared[i + 1]) << 8);
    }
    return 0xffff;
}

static void cpu_write16(void *opaque, uint32_t address, uint16_t value)
{
    auto *b = static_cast<zoom_board *>(opaque);
    address &= 0xfffffe;
    if (sync_cpu_callback(b) != ZOOM_BOARD_OK) return;
    if (address >= BOOT_BASE && address + 1 < BOOT_END) {
        /* Read-only MN10200 flash mapping. */
    } else if (address >= SOUND_RAM_BASE && address + 1 < SOUND_RAM_END) {
        unsigned i = address - SOUND_RAM_BASE;
        b->sound_ram[i] = uint8_t(value);
        b->sound_ram[i + 1] = uint8_t(value >> 8);
    } else if (address >= ZSG_BASE && address < ZSG_END) {
        int r = zsg2_write(b->zsg, (address - ZSG_BASE) >> 1, value, 0xffff);
        if (r != ZSG2_OK) b->fault = ZOOM_FAULT_DEVICE;
    } else if (address == TMS_PORT) {
        tms57002_data_w(b->dsp, uint8_t(value));
    } else if (address >= SHARED_BASE && address + 1 < SHARED_END) {
        unsigned i = address - SHARED_BASE;
        b->shared[i] = uint8_t(value);
        b->shared[i + 1] = uint8_t(value >> 8);
    }
}
} // namespace

extern "C" {
int zoom_board_create(const zoom_board_config *config, zoom_board **out)
{
    if (!out || !config || !config->boot_flash || config->boot_flash_size != ZOOM_BOOT_FLASH_BYTES)
        return ZOOM_BOARD_ARGUMENT;
    *out = nullptr;
    if (config->sample_rom) {
        if ((config->sample_rom_size != 0x400000 && config->sample_rom_size != 0x600000) ||
            config->flash_read16 || config->flash_write16) return ZOOM_BOARD_ARGUMENT;
    } else {
        for (unsigned i = 0; i < 3; ++i)
            if (!config->sample_flash[i] || config->sample_flash_size[i] != ZOOM_SAMPLE_FLASH_BYTES)
                return ZOOM_BOARD_ARGUMENT;
    }
    auto *b = new (std::nothrow) zoom_board;
    if (!b) return ZOOM_BOARD_NOMEM;
    b->config = *config;

    mn10200_config cpu_config{};
    cpu_config.opaque = b;
    cpu_config.read8 = cpu_read8;
    cpu_config.write8 = cpu_write8;
    cpu_config.read16 = cpu_read16;
    cpu_config.write16 = cpu_write16;
    cpu_config.port_read = cpu_port_read;
    cpu_config.port_write = cpu_port_write;
    b->cpu = mn10200_create(&cpu_config);
    if (!b->cpu) { delete b; return ZOOM_BOARD_NOMEM; }

    tms57002_config_t dsp_config{};
    dsp_config.opaque = b;
    dsp_config.data_read = dsp_data_read;
    dsp_config.data_write = dsp_data_write;
    dsp_config.empty_changed = dsp_empty_changed;
    b->dsp = tms57002_create(&dsp_config);
    if (!b->dsp) { mn10200_destroy(b->cpu); delete b; return ZOOM_BOARD_NOMEM; }

    ZSG2Config zsg_config{};
    zsg_config.clock_hz = ZSG2_DEFAULT_CLOCK;
    zsg_config.sample_rom = config->sample_rom;
    zsg_config.sample_rom_size = config->sample_rom ? config->sample_rom_size : 0x600000;
    zsg_config.external_read = config->sample_rom ? nullptr : zsg_external_read;
    zsg_config.rom_opaque = b;
    zsg_config.sample = zsg_sample;
    zsg_config.sample_opaque = b;
    if (zsg2_create(&zsg_config, &b->zsg) != ZSG2_OK) {
        tms57002_destroy(b->dsp); mn10200_destroy(b->cpu); delete b; return ZOOM_BOARD_NOMEM;
    }
    if (config->sample_rom && zoom_board_set_sound_reset(b, 0) != ZOOM_BOARD_OK) {
        zoom_board_destroy(b); return ZOOM_BOARD_DEVICE;
    }
    *out = b;
    return ZOOM_BOARD_OK;
}

void zoom_board_destroy(zoom_board *b)
{
    if (!b || b->callback_active || b->synchronizing) return;
    zsg2_destroy(b->zsg);
    tms57002_destroy(b->dsp);
    mn10200_destroy(b->cpu);
    delete b;
}

int zoom_board_is_busy(const zoom_board *b)
{
    return b && (b->callback_active || b->synchronizing);
}

int zoom_board_set_sound_reset(zoom_board *b, int asserted)
{
    if (!b || b->callback_active || b->synchronizing) return ZOOM_BOARD_DEVICE;
    bool next = asserted != 0;
    if (next != b->sound_reset) {
        if (!next) {
            int r = zoom_board_reset_devices(b);
            if (r != ZOOM_BOARD_OK) return r;
            uint64_t local = mn10200_get_cycles(b->cpu);
            if (local > b->synced_main_cycle) {
                b->fault = ZOOM_FAULT_TIME;
                return ZOOM_BOARD_TIME;
            }
            b->cpu_cycle_origin = b->synced_main_cycle - local;
            (void)mn10200_reset(b->cpu);
            if (b->config.flash_write16) {
                for (unsigned chip = 0; chip < 4; ++chip) {
                    int fr = call_flash_write(b, chip, 0, 0x00ff, 0xffff);
                    if (fr != ZOOM_BOARD_OK) return fr;
                }
            }
        } else {
            (void)mn10200_reset(b->cpu);
        }
        b->sound_reset = next;
    }
    return ZOOM_BOARD_OK;
}

int zoom_board_reset_devices(zoom_board *b)
{
    if (!b || b->callback_active || b->synchronizing) return ZOOM_BOARD_DEVICE;
    if (zsg2_reset(b->zsg) != ZSG2_OK) return ZOOM_BOARD_DEVICE;
    tms57002_reset(b->dsp);
    b->reg_address = 0;
    return ZOOM_BOARD_OK;
}

int64_t zoom_board_run(zoom_board *b, uint64_t cycles)
{
    if (!b || b->callback_active || b->synchronizing) return ZOOM_BOARD_DEVICE;
    if (b->sound_reset || cycles == 0) return 0;
    uint64_t local = mn10200_get_cycles(b->cpu);
    uint64_t max = std::numeric_limits<uint64_t>::max();
    if (b->cpu_cycle_origin > max - 32 ||
        local > max - b->cpu_cycle_origin - 32 ||
        cycles > max - b->cpu_cycle_origin - local - 32) {
        b->fault = ZOOM_FAULT_TIME;
        return ZOOM_BOARD_TIME;
    }
    b->cpu_running = true;
    int64_t used = mn10200_run(b->cpu, cycles);
    b->cpu_running = false;
    if (used < 0) {
        collect_device_faults(b);
        return b->fault == ZOOM_FAULT_TIME ? ZOOM_BOARD_TIME : ZOOM_BOARD_DEVICE;
    }
    collect_device_faults(b);
    uint64_t final_local = mn10200_get_cycles(b->cpu);
    int r = sync_to(b, b->cpu_cycle_origin + final_local);
    return r == ZOOM_BOARD_OK ? used : r;
}

int zoom_board_sync_to_cycle(zoom_board *b, uint64_t cycle)
{
    if (!b || b->callback_active || b->synchronizing) return ZOOM_BOARD_DEVICE;
    if (!b->sound_reset) {
        uint64_t local_target = cycle > b->cpu_cycle_origin ? cycle - b->cpu_cycle_origin : 0;
        while (mn10200_get_cycles(b->cpu) < local_target) {
            uint64_t remain = local_target - mn10200_get_cycles(b->cpu);
            if (remain > uint64_t(INT64_MAX - 32)) remain = uint64_t(INT64_MAX - 32);
            b->cpu_running = true;
            int64_t used = mn10200_run(b->cpu, remain);
            b->cpu_running = false;
            if (used <= 0) return ZOOM_BOARD_DEVICE;
        }
        uint64_t local = mn10200_get_cycles(b->cpu);
        if (local > std::numeric_limits<uint64_t>::max() - b->cpu_cycle_origin) {
            b->fault = ZOOM_FAULT_TIME;
            return ZOOM_BOARD_TIME;
        }
        return sync_to(b, std::max(cycle, b->cpu_cycle_origin + local));
    }
    /* A reset assertion can stop the MN10200 after its last instruction
     * overran the requested boundary. Retain that time while it is held. */
    return sync_to(b, std::max(cycle, b->synced_main_cycle));
}

uint32_t zoom_board_main_read32(zoom_board *b, uint32_t address, uint32_t mem_mask)
{
    (void)mem_mask;
    if (!b) return 0xffffffffu;
    uint32_t aligned = address & ~3u;
    if (aligned == 0x1fbc0000u) return 0xffff0000u;
    if (aligned < MAIN_SHARED_BASE || aligned >= MAIN_SHARED_BASE + 0x200u)
        return 0xffffffffu;
    unsigned dword = (aligned - MAIN_SHARED_BASE) >> 2;
    uint32_t value = 0xffffffffu;
    unsigned i0 = dword * 2;
    value = (value & 0xffffff00u) | b->shared[i0];
    value = (value & 0xff00ffffu) | (uint32_t(b->shared[i0 + 1]) << 16);
    return value;
}

int zoom_board_main_write32(zoom_board *b, uint32_t address, uint32_t data, uint32_t mem_mask)
{
    if (!b || b->callback_active || b->synchronizing) return ZOOM_BOARD_DEVICE;
    uint32_t aligned = address & ~3u;
    if (aligned < MAIN_SHARED_BASE || aligned >= MAIN_SHARED_BASE + 0x200u)
        return ZOOM_BOARD_UNSUPPORTED;
    unsigned dword = (aligned - MAIN_SHARED_BASE) >> 2;
    uint8_t lane_masks[2] = {uint8_t(mem_mask), uint8_t(mem_mask >> 16)};
    uint8_t lane_data[2] = {uint8_t(data), uint8_t(data >> 16)};
    for (unsigned lane = 0; lane < 2; ++lane) {
        unsigned i = dword * 2 + lane;
        uint8_t mask = lane_masks[lane];
        b->shared[i] = uint8_t((b->shared[i] & ~mask) | (lane_data[lane] & mask));
    }
    return ZOOM_BOARD_OK;
}

uint16_t zoom_board_main_read16(zoom_board *b, uint32_t address, uint16_t mem_mask)
{
    (void)mem_mask;
    if (!b) return 0xffff;
    if ((address & ~1u) == 0x1fbc0000u) return 0;
    if (address >= MAIN_SHARED_BASE && address < MAIN_SHARED_BASE + 0x200u) {
        unsigned i = ((address - MAIN_SHARED_BASE) >> 2) * 2 + ((address & 2u) ? 1u : 0u);
        return uint16_t(0xff00u | b->shared[i]);
    }
    return 0xffff;
}

int zoom_board_main_write16(zoom_board *b, uint32_t address, uint16_t data,
                            uint16_t mem_mask)
{
    if (!b || b->callback_active || b->synchronizing) return ZOOM_BOARD_DEVICE;
    uint32_t word_address = address & ~1u;
    if (word_address == 0x1fb80000u || word_address == 0x1fb80002u ||
        word_address == 0x1fba0000u) {
        if (!mem_mask) return ZOOM_BOARD_OK;
        if (word_address == 0x1fb80000u) {
            if (b->reg_address == 4)
                b->output_gain[0] = float(data & 0x3f) / 63.0f;
            else if (b->reg_address == 5)
                b->output_gain[1] = float(data & 0x3f) / 63.0f;
        } else if (word_address == 0x1fb80002u) {
            b->reg_address = uint8_t(data);
        } else {
            (void)mn10200_set_irq(b->cpu, 0, 1);
            (void)mn10200_set_irq(b->cpu, 0, 0);
        }
        return ZOOM_BOARD_OK;
    }
    if (address >= MAIN_SHARED_BASE && address < MAIN_SHARED_BASE + 0x200u) {
        unsigned i = ((address - MAIN_SHARED_BASE) >> 2) * 2 + ((address & 2u) ? 1u : 0u);
        uint8_t mask = uint8_t(mem_mask);
        b->shared[i] = uint8_t((b->shared[i] & ~mask) | (uint8_t(data) & mask));
        return ZOOM_BOARD_OK;
    }
    return ZOOM_BOARD_UNSUPPORTED;
}

uint32_t zoom_board_fault(const zoom_board *b) { return b ? b->fault : ZOOM_FAULT_BAD_BUS_ACCESS; }
void zoom_board_clear_fault(zoom_board *b) { if (b) b->fault = ZOOM_FAULT_NONE; }
uint64_t zoom_board_main_cycles(const zoom_board *b) { return b ? b->synced_main_cycle : 0; }
uint64_t zoom_board_audio_frames(const zoom_board *b) { return b ? b->audio_frames : 0; }
void zoom_board_last_audio(const zoom_board *b, float stereo[2]) { if (stereo) { stereo[0] = b ? b->last_audio[0] : 0; stereo[1] = b ? b->last_audio[1] : 0; } }
uint8_t zoom_board_shared_read(const zoom_board *b, unsigned i) { return b && i < ZOOM_SHARED_BYTES ? b->shared[i] : 0xff; }
uint8_t zoom_board_sound_ram_read(const zoom_board *b, unsigned i) { return b && i < ZOOM_SOUND_RAM_BYTES ? b->sound_ram[i] : 0xff; }
uint8_t zoom_board_dsp_ram_read(const zoom_board *b, unsigned i) { return b && i < ZOOM_DSP_DATA_BYTES ? b->dsp_ram[i] : 0xff; }
uint32_t zoom_board_sound_pc(const zoom_board *b) { return b ? mn10200_get_pc(b->cpu) : 0; }
uint32_t zoom_board_sound_d(const zoom_board *b, unsigned i) { return b ? mn10200_get_d(b->cpu, i) : 0; }
uint8_t zoom_board_tms_control(const zoom_board *b) { return b ? b->tms_ctrl : 0xff; }
uint32_t zoom_board_mn_fault(const zoom_board *b) { return b ? mn10200_get_fault(b->cpu) : 1; }
uint16_t zoom_board_zsg_register(const zoom_board *b, unsigned channel, unsigned reg)
{
    if (!b || channel >= ZSG2_CHANNELS || reg >= 16) return 0xffff;
    ZSG2Channel state{};
    if (zsg2_get_channel(b->zsg, channel, &state) != ZSG2_OK) return 0xffff;
    return state.registers[reg];
}
uint32_t zoom_board_tms_program_word(const zoom_board *b, uint8_t address)
{ return b ? tms57002_program_read(b->dsp, address) : 0xffffffffu; }
uint8_t zoom_board_tms_pc(const zoom_board *b) { return b ? tms57002_pc(b->dsp) : 0xff; }
}
