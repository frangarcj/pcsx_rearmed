#include "gnet_zoom.h"
#include "nor_flash.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
void emit(std::vector<uint8_t> &rom, size_t &p,
          std::initializer_list<uint8_t> bytes)
{
    for (uint8_t b : bytes) rom[p++] = b;
}

void emit_store8(std::vector<uint8_t> &rom, size_t &p,
                 uint8_t value, uint32_t address)
{
    emit(rom, p, {0xf8, value, 0x00, 0xf4, 0x44,
                  uint8_t(address), uint8_t(address >> 8), uint8_t(address >> 16)});
}

struct Media {
    std::vector<uint8_t> firm = std::vector<uint8_t>(GNET_FIRM_FLASH_BYTES, 0xff);
    std::vector<uint8_t> program = std::vector<uint8_t>(GNET_ZOOM_PROGRAM_FLASH_BYTES, 0xff);
    std::array<std::vector<uint8_t>, 3> samples{
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0xff),
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0xff),
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0xff)};
    std::vector<uint8_t> eprom = std::vector<uint8_t>(GNET_EPROM_BYTES, 0xff);

    gnet_zoom_config_t config(unsigned jp1 = 0)
    {
        gnet_zoom_config_t c{};
        c.firm_flash = firm.data(); c.firm_flash_size = firm.size();
        c.zoom_program_flash = program.data(); c.zoom_program_flash_size = program.size();
        for (unsigned i = 0; i < 3; ++i) {
            c.sample_flash[i] = samples[i].data();
            c.sample_flash_size[i] = samples[i].size();
        }
        c.eprom = eprom.data(); c.eprom_size = eprom.size(); c.jp1 = jp1;
        return c;
    }
};

struct PcCardProbe {
    uint32_t read_memory_offset = 0;
    uint16_t read_memory_mask = 0;
    uint32_t write_memory_offset = 0;
    uint16_t write_memory_data = 0;
    uint16_t write_memory_mask = 0;
    uint32_t read_reg_offset = 0;
    uint32_t previous_read_reg_offset = 0;
    uint16_t read_reg_mask = 0;
    unsigned read_reg_count = 0;
    uint32_t write_reg_offset = 0;
    uint32_t previous_write_reg_offset = 0;
    uint16_t write_reg_data = 0;
    uint16_t write_reg_mask = 0;
    unsigned write_reg_count = 0;
    unsigned reset_count = 0;
    gnet_zoom_board *board = nullptr;
    bool destroy_from_read = false;
    bool destroy_from_reset = false;
    int read_destroy_result = GNET_ZOOM_OK;
    int reset_destroy_result = GNET_ZOOM_OK;
};

uint16_t pc_read_memory(void *opaque, uint32_t offset, uint16_t mask)
{
    auto *probe = static_cast<PcCardProbe *>(opaque);
    probe->read_memory_offset = offset;
    probe->read_memory_mask = mask;
    if (probe->destroy_from_read)
        probe->read_destroy_result = gnet_zoom_try_destroy(probe->board);
    return uint16_t(0x1200 | (offset & 0xff));
}

void pc_write_memory(void *opaque, uint32_t offset, uint16_t data,
                     uint16_t mask)
{
    auto *probe = static_cast<PcCardProbe *>(opaque);
    probe->write_memory_offset = offset;
    probe->write_memory_data = data;
    probe->write_memory_mask = mask;
}

uint16_t pc_read_reg(void *opaque, uint32_t offset, uint16_t mask)
{
    auto *probe = static_cast<PcCardProbe *>(opaque);
    probe->previous_read_reg_offset = probe->read_reg_offset;
    probe->read_reg_offset = offset;
    probe->read_reg_mask = mask;
    ++probe->read_reg_count;
    return uint16_t(0xa000 | (offset & 0x0fff));
}

void pc_write_reg(void *opaque, uint32_t offset, uint16_t data,
                  uint16_t mask)
{
    auto *probe = static_cast<PcCardProbe *>(opaque);
    probe->previous_write_reg_offset = probe->write_reg_offset;
    probe->write_reg_offset = offset;
    probe->write_reg_data = data;
    probe->write_reg_mask = mask;
    ++probe->write_reg_count;
}

void pc_reset(void *opaque)
{
    auto *probe = static_cast<PcCardProbe *>(opaque);
    ++probe->reset_count;
    if (probe->destroy_from_reset)
        probe->reset_destroy_result = gnet_zoom_try_destroy(probe->board);
}

void write_intel_word(gnet_zoom_board *b, uint32_t address, uint16_t data)
{
    assert(gnet_zoom_main_write16(b, address, data, 0xffff) == GNET_ZOOM_OK);
}
uint16_t read_word(gnet_zoom_board *b, uint32_t address)
{
    uint16_t result = 0;
    int status = gnet_zoom_main_read16(b, address, &result);
    if (status != GNET_ZOOM_OK) std::fprintf(stderr, "read %08x status %d bank %u\n", address, status, gnet_zoom_selected_bank(b));
    assert(status == GNET_ZOOM_OK);
    return result;
}

struct DestroyProbe {
    gnet_zoom_board *board = nullptr;
    unsigned callbacks = 0;
    int result = 0;
};

void probe_destroy_from_audio(void *opaque, float, float, uint64_t)
{
    auto *probe = static_cast<DestroyProbe *>(opaque);
    ++probe->callbacks;
    probe->result = gnet_zoom_try_destroy(probe->board);
}

static gnet_zoom_board *create_loop_board(Media &media,
                                          void *audio_opaque = nullptr,
                                          zoom_audio_frame_fn audio_frame = nullptr)
{
    media.program[0] = 0xea; // BRA -2: keep fetching/executing from NOR
    media.program[1] = 0xfe;
    auto config = media.config();
    if (audio_frame) {
        config.audio_opaque = audio_opaque;
        config.audio_frame = audio_frame;
    }
    gnet_zoom_board *board = nullptr;
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK && board);
    assert(gnet_zoom_control_write(board, 0) == GNET_ZOOM_OK);
    return board;
}

struct ReadyProbe {
    gnet_zoom_board *board = nullptr;
    bool ready_seen = false;
    uint64_t first_ready_zsg_cycle = 0;
    unsigned frames = 0;
};

struct SoftResetProbe {
    gnet_zoom_board *board = nullptr;
    unsigned requests = 0;
    int destroy_result = GNET_ZOOM_OK;
};

void probe_soft_reset(void *opaque)
{
    auto *probe = static_cast<SoftResetProbe *>(opaque);
    ++probe->requests;
    probe->destroy_result = gnet_zoom_try_destroy(probe->board);
}

void probe_flash_ready_from_audio(void *opaque, float, float, uint64_t cycle)
{
    auto *probe = static_cast<ReadyProbe *>(opaque);
    ++probe->frames;
    if (!probe->ready_seen && gnet_zoom_flash_ready(probe->board, 0)) {
        probe->ready_seen = true;
        probe->first_ready_zsg_cycle = cycle;
    }
}

struct AudioSample {
    float left;
    float right;
    uint64_t cycle;
};

struct AudioCapture {
    std::vector<AudioSample> samples;
    bool finite = true;
    bool monotonic = true;
    bool nonzero = false;
};

void capture_audio(void *opaque, float left, float right, uint64_t cycle)
{
    auto *capture = static_cast<AudioCapture *>(opaque);
    if (!std::isfinite(left) || !std::isfinite(right)) capture->finite = false;
    if (!capture->samples.empty() && cycle <= capture->samples.back().cycle)
        capture->monotonic = false;
    if (std::abs(left) > 0.00001f || std::abs(right) > 0.00001f)
        capture->nonzero = true;
    capture->samples.push_back({left, right, cycle});
}

void install_tms_tone_program(Media &media)
{
    size_t p = 0;
    /* MN10200 uploads a short TMS57002 tone program over Port 1 and writes
     * its coefficient RAM through the same pin protocol used by the board. */
    emit_store8(media.program, p, 0xff, 0xffe1); // P1 direction = output
    emit_store8(media.program, p, 3, 0xfe64);    // deassert both load pins
    emit_store8(media.program, p, 2, 0xfe64);    // assert PLOAD
    const uint8_t program_bytes[] = {
        0,0,0, 0,0,0, 0xcc,0x05,0x10, 0x01,0x10,0x00, 0xfc,0x40,0x00
    };
    for (uint8_t byte : program_bytes) emit_store8(media.program, p, byte, 0xc00000);
    emit_store8(media.program, p, 3, 0xfe64); // release PLOAD
    emit_store8(media.program, p, 1, 0xfe64); // assert CLOAD
    const uint8_t cload_bytes[] = {0x10,0x12,0x34,0x56,0x78};
    for (uint8_t byte : cload_bytes) emit_store8(media.program, p, byte, 0xc00000);
    emit_store8(media.program, p, 3, 0xfe64); // complete CLOAD
    emit(media.program, p, {0xea, 0xfe}); // BRA -2 after setup
    for (auto &sample : media.samples) std::fill(sample.begin(), sample.end(), 0);
}

std::vector<AudioSample> run_tms_tone(bool split_sync, AudioCapture &capture)
{
    Media media;
    install_tms_tone_program(media);
    auto config = media.config();
    config.audio_opaque = &capture;
    config.audio_frame = capture_audio;
    gnet_zoom_board *board = nullptr;
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK && board);
    assert(gnet_zoom_control_write(board, 0) == GNET_ZOOM_OK);
    if (split_sync) {
        for (uint64_t cycle = 250; cycle < 5000; cycle += 250)
            assert(gnet_zoom_sync_to_cycle(board, cycle) == GNET_ZOOM_OK);
        assert(gnet_zoom_sync_to_cycle(board, 5000) == GNET_ZOOM_OK);
    } else {
        assert(gnet_zoom_sync_to_cycle(board, 5000) == GNET_ZOOM_OK);
    }
    assert(gnet_zoom_fault(board) == 0);
    assert(gnet_zoom_try_destroy(board) == GNET_ZOOM_OK);
    return capture.samples;
}

void test_mn_tms_program_reaches_real_zoom_audio_callback()
{
    AudioCapture one_shot;
    const std::vector<AudioSample> one_shot_samples = run_tms_tone(false, one_shot);
    AudioCapture split;
    const std::vector<AudioSample> split_samples = run_tms_tone(true, split);

    for (const AudioCapture *capture : {&one_shot, &split}) {
        assert(capture->samples.size() > 4);
        assert(capture->finite);
        assert(capture->monotonic);
        assert(capture->nonzero);
    }
    assert(one_shot_samples.size() == split_samples.size());
    for (size_t i = 0; i < one_shot_samples.size(); ++i) {
        assert(one_shot_samples[i].cycle == split_samples[i].cycle);
        assert(one_shot_samples[i].left == split_samples[i].left);
        assert(one_shot_samples[i].right == split_samples[i].right);
    }
}

void test_mn_executes_real_zoom_flash_and_bus_is_little_endian()
{
    Media media;
    size_t p = 0;
    /* The MN10200 resets at 0x80000. This fixture runs from the actual
     * borrowed E28F400B image through the Zoom flash-read callback. */
    emit(media.program, p, {0xf8, 0x34, 0x12}); // D0 = 0x1234
    emit(media.program, p, {0xf4, 0x40, 0x10, 0x00, 0x40}); // sound RAM store
    emit(media.program, p, {0xf4, 0x44, 0x00, 0x00, 0x08}); // ignored ROM write
    emit(media.program, p, {0xf8, 0x78, 0x56}); // proves execution continued
    media.firm[0] = 0x34; media.firm[1] = 0x12;
    media.program[0x200] = 0xcd; media.program[0x201] = 0xab;

    gnet_zoom_config_t config = media.config();
    gnet_zoom_board *board = nullptr;
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK && board);
    assert(gnet_zoom_control_read(board) == 0x10);
    assert(gnet_zoom_sound_pc(board) == 0x80000);
    assert(gnet_zoom_control_write(board, 0) == GNET_ZOOM_OK);
    assert(gnet_zoom_run(board, 4000) >= 4000);
    assert(gnet_zoom_sound_pc(board) > 0x80000);
    assert(gnet_zoom_sound_d(board, 0) == 0x5678);
    assert(gnet_zoom_sound_ram_read(board, 0x10) == 0x34);
    assert(gnet_zoom_sound_ram_read(board, 0x11) == 0x12);
    assert(media.program[0] == 0xf8); // MN map is read-only in taito_zm.cpp
    assert(gnet_zoom_fault(board) == 0);

    /* Bank 0 maps the main CPU to firmware at offset 0 and Zoom program at
     * 0x300000. Raw images are little-endian despite the x16 NOR engine. */
    assert(read_word(board, 0x1f000000) == 0x1234);
    assert(read_word(board, 0x1f300200) == 0xabcd);

    /* Intel x16 program sequence through the PSX banked window updates the
     * original little-endian ROM/NVRAM buffer. */
    write_intel_word(board, 0x1f300200, 0x0040);
    write_intel_word(board, 0x1f300200, 0x1234);
    write_intel_word(board, 0x1f300200, 0x00ff); // return to array-read mode
    assert(media.program[0x200] == 0x34 && media.program[0x201] == 0x12);
    assert(read_word(board, 0x1f300200) == 0x1234);
    assert(gnet_zoom_flash_ready(board, 1) == 1);
    gnet_zoom_destroy(board);
}

void test_bank_control_mapping_eprom_alias_and_wave_flash()
{
    Media media;
    media.eprom[0] = 0x9a; media.eprom[1] = 0xbc;
    media.samples[0][0] = 0x11; media.samples[0][1] = 0x22;
    media.samples[1][0] = 0x33; media.samples[1][1] = 0x44;
    media.samples[2][0] = 0x55; media.samples[2][1] = 0x66;
    auto config = media.config(1); // JP1 selects initial bank 2
    gnet_zoom_board *board = nullptr;
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK);
    assert(gnet_zoom_selected_bank(board) == 2);
    assert(read_word(board, 0x1f000000) == 0xbc9a); // bank2 EPROM base
    assert(gnet_zoom_main_write16(board, 0x1f000000, 0, 0xffff) == GNET_ZOOM_READ_ONLY);
    uint16_t unmapped = 0;
    assert(gnet_zoom_main_read16(board, 0x1f800000, &unmapped) == GNET_ZOOM_UNMAPPED);
    /* Bank 2 exposes firmware at 0x10200000; confirm the alternate mapping
     * reaches the same TE28F160 command engine and raw image. */
    write_intel_word(board, 0x1f200200, 0x0040);
    write_intel_word(board, 0x1f200200, 0x5678);
    write_intel_word(board, 0x1f200200, 0x00ff);
    assert(media.firm[0x200] == 0x78 && media.firm[0x201] == 0x56);
    assert(read_word(board, 0x1f200200) == 0x5678);

    /* Control bit 2 selects the other bank bit; JP1 remains the high bank bit. */
    assert(gnet_zoom_control_write(board, 0x14) == GNET_ZOOM_OK);
    assert(gnet_zoom_selected_bank(board) == 3);
    assert(read_word(board, 0x1f000000) == 0x2211);
    assert(read_word(board, 0x1f200000) == 0x4433);
    assert(read_word(board, 0x1f400000) == 0x6655);
    /* MAME maps bank 3 through mirror(0x10000000) to the same wave devices. */
    uint16_t wave_hole = 0;
    assert(gnet_zoom_main_read16(board, 0x1f600000, &wave_hole) == GNET_ZOOM_UNMAPPED);
    assert(gnet_zoom_main_write16(board, 0x1f000000, 0x0040, 0xffff) == GNET_ZOOM_OK);
    assert(gnet_zoom_main_write16(board, 0x1f000000, 0xbeef, 0xffff) == GNET_ZOOM_OK);
    assert(media.samples[0][0] == 0xef && media.samples[0][1] == 0xbe);

    /* Reset release invokes the Zoom device reset, including NOR 0xff reset
     * commands, without erasing program data. */
    assert(gnet_zoom_control_write(board, 0x04) == GNET_ZOOM_OK);
    assert(read_word(board, 0x1f000000) == 0xbeef);
    assert(gnet_zoom_fault(board) == 0);
    gnet_zoom_destroy(board);
}

void test_source_maincpu_map_routes_registers_and_shared_ram_lanes()
{
    Media media;
    media.firm[0] = 0x12;
    media.firm[1] = 0x34;
    media.firm[2] = 0x56;
    media.firm[3] = 0x78;
    auto config = media.config();
    gnet_zoom_board *board = nullptr;
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK);

    uint32_t value = 0;
    assert(gnet_zoom_bus_read(board, 0x1fa30000, 1, &value) == GNET_ZOOM_OK && value == 0);
    assert(gnet_zoom_bus_write(board, 0x1fa30000, 1, 0xa5) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_read(board, 0x1fa30000, 1, &value) == GNET_ZOOM_OK && value == 0xa5);
    assert(gnet_zoom_bus_read(board, 0x1f000000, 2, &value) == GNET_ZOOM_OK && value == 0x3412);

    assert(gnet_zoom_bus_read(board, 0x1f000000, 1, &value) == GNET_ZOOM_OK && value == 0x12);
    assert(gnet_zoom_bus_read(board, 0x1f000001, 1, &value) == GNET_ZOOM_OK && value == 0x34);
    assert(gnet_zoom_bus_read(board, 0x1f000000, 4, &value) == GNET_ZOOM_OK && value == 0x78563412);

    /* Reset starts asserted with bank 0. The control register is byte-wide;
     * bit 2 selects bank 1 and bit 4 releases/reset-pulses the Zoom side. */
    assert(gnet_zoom_bus_read(board, 0x1fb40000, 1, &value) == GNET_ZOOM_OK && value == 0x10);
    assert(gnet_zoom_bus_write(board, 0x1fb40000, 1, 0x04) == GNET_ZOOM_OK);
    assert(gnet_zoom_selected_bank(board) == 1);
    assert(gnet_zoom_bus_read(board, 0x1fb70000, 2, &value) == GNET_ZOOM_OK && value == 2);
    assert(gnet_zoom_bus_write(board, 0x1fb70000, 2, 0xbeef) == GNET_ZOOM_OK);

    assert(gnet_zoom_bus_read(board, 0x1fb70000, 1, &value) == GNET_ZOOM_OK && value == 2);
    assert(gnet_zoom_bus_read(board, 0x1fb70001, 1, &value) == GNET_ZOOM_OK && value == 0);
    assert(gnet_zoom_bus_read(board, 0x1fb70000, 4, &value) == GNET_ZOOM_OK && value == 0xffff0002);
    assert(gnet_zoom_bus_write(board, 0x1fb70000, 1, 1) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_write(board, 0x1fb40000, 2, 0xff04) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_read(board, 0x1fb40000, 2, &value) == GNET_ZOOM_OK && value == 0xff04);

    /* The 16-bit Zoom data/address latches and sound IRQ strobe are the exact
     * mapped devices used by taitogn_state::maincpu_program_map. */
    assert(gnet_zoom_bus_write(board, 0x1fb80002, 2, 4) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_write(board, 0x1fb80000, 2, 0x0020) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_write(board, 0x1fba0000, 2, 0) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_read(board, 0x1fbc0000, 2, &value) == GNET_ZOOM_OK && value == 0);

    /* MAME's .umask32(0x00ff00ff) exposes bytes at lanes 0 and 2 only.
     * Byte/halfword/word accesses preserve the two open-bus lanes. */
    assert(gnet_zoom_bus_write(board, 0x1fbe0000, 4, 0x00340012) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_read(board, 0x1fbe0000, 4, &value) == GNET_ZOOM_OK &&
           value == 0xff34ff12);
    assert(gnet_zoom_bus_read(board, 0x1fbe0001, 1, &value) == GNET_ZOOM_OK && value == 0xff);
    assert(gnet_zoom_bus_read(board, 0x1fbe0002, 1, &value) == GNET_ZOOM_OK && value == 0x34);
    assert(gnet_zoom_bus_write(board, 0x1fbe0004, 1, 0x56) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_read(board, 0x1fbe0004, 4, &value) == GNET_ZOOM_OK &&
           value == 0xff00ff56);

    assert(gnet_zoom_bus_read(board, 0x1fb80000, 2, &value) == GNET_ZOOM_UNMAPPED);
    assert(gnet_zoom_bus_read(board, 0x1fb00000, 1, &value) == GNET_ZOOM_UNSUPPORTED);
    assert(gnet_zoom_bus_write(board, 0x1fbe0000, 3, 0) == GNET_ZOOM_ARGUMENT);
    assert(gnet_zoom_bus_read(board, 0x1fa30000, 2, &value) == GNET_ZOOM_OK && value == 0xffa5);
    assert(gnet_zoom_bus_read(board, 0x1fb90000, 1, &value) == GNET_ZOOM_UNMAPPED);
    gnet_zoom_destroy(board);
}

void test_rf5c296_controller_and_optional_pc_card_windows()
{
    Media media;
    PcCardProbe probe;
    rf5c296_card_ops_t card{};
    card.opaque = &probe;
    card.read_memory = pc_read_memory;
    card.write_memory = pc_write_memory;
    card.read_reg = pc_read_reg;
    card.write_reg = pc_write_reg;
    card.reset = pc_reset;
    auto config = media.config();
    config.pc_card = &card;
    gnet_zoom_board *board = nullptr;
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK && board);
    probe.board = board;

    uint32_t value = 0;
    /* Bank-zero offset 0x200000 is PC Card attribute/configuration memory;
     * the PSX x16 aperture translates byte offsets to 16-bit card units. */
    assert(gnet_zoom_bus_read(board, 0x1f200000, 2, &value) == GNET_ZOOM_OK &&
           value == 0xa000);
    assert(probe.read_reg_offset == 0 && probe.read_reg_mask == 0xffff);
    assert(gnet_zoom_bus_read(board, 0x1f200200, 2, &value) == GNET_ZOOM_OK &&
           value == 0xa100);
    assert(probe.read_reg_offset == 0x100);
    assert(gnet_zoom_bus_write(board, 0x1f200004, 2, 0x7654) == GNET_ZOOM_OK);
    assert(probe.write_reg_offset == 2 && probe.write_reg_data == 0x7654 &&
           probe.write_reg_mask == 0xffff);

    /* The amap16 window preserves byte masks and splits dword accesses into
     * little-endian x16 words. mem_r/mem_w receive word offsets unchanged. */
    assert(gnet_zoom_bus_read(board, 0x1f200040, 1, &value) == GNET_ZOOM_OK &&
           value == 0x20);
    assert(probe.read_reg_offset == 0x20 && probe.read_reg_mask == 0x00ff);
    assert(gnet_zoom_bus_read(board, 0x1f200041, 1, &value) == GNET_ZOOM_OK &&
           value == 0xa0);
    assert(probe.read_reg_offset == 0x20 && probe.read_reg_mask == 0xff00);
    assert(gnet_zoom_bus_write(board, 0x1f200040, 1, 0x5a) == GNET_ZOOM_OK);
    assert(probe.write_reg_offset == 0x20 && probe.write_reg_data == 0x005a &&
           probe.write_reg_mask == 0x00ff);
    assert(gnet_zoom_bus_write(board, 0x1f200041, 1, 0x6b) == GNET_ZOOM_OK);
    assert(probe.write_reg_offset == 0x20 && probe.write_reg_data == 0x6b00 &&
           probe.write_reg_mask == 0xff00);
    unsigned read_calls = probe.read_reg_count;
    assert(gnet_zoom_bus_read(board, 0x1f200080, 4, &value) == GNET_ZOOM_OK &&
           value == 0xa041a040);
    assert(probe.read_reg_count == read_calls + 2 &&
           probe.previous_read_reg_offset == 0x40 &&
           probe.read_reg_offset == 0x41 && probe.read_reg_mask == 0xffff);
    unsigned write_calls = probe.write_reg_count;
    assert(gnet_zoom_bus_write(board, 0x1f200088, 4, 0x55667788) ==
           GNET_ZOOM_OK);
    assert(probe.write_reg_count == write_calls + 2 &&
           probe.previous_write_reg_offset == 0x44 &&
           probe.write_reg_offset == 0x45 &&
           probe.write_reg_data == 0x5566 &&
           probe.write_reg_mask == 0xffff);

    /* The last aligned dword fits exactly. The following flash region and a
     * nonzero bank stay outside the PC Card callback path. */
    read_calls = probe.read_reg_count;
    assert(gnet_zoom_bus_read(board, 0x1f2ffffc, 4, &value) == GNET_ZOOM_OK &&
           value == 0xafffaffe);
    assert(probe.read_reg_count == read_calls + 2 &&
           probe.previous_read_reg_offset == 0x7fffe &&
           probe.read_reg_offset == 0x7ffff);
    read_calls = probe.read_reg_count;
    assert(gnet_zoom_bus_read(board, 0x1f300000, 4, &value) ==
           GNET_ZOOM_OK && value == 0xffffffff);
    assert(probe.read_reg_count == read_calls);
    assert(gnet_zoom_control_write(board, 0x04) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_read(board, 0x1f200000, 4, &value) ==
           GNET_ZOOM_OK && value == 0xffffffff);
    assert(probe.read_reg_count == read_calls);
    assert(gnet_zoom_control_write(board, 0x00) == GNET_ZOOM_OK);

    /* RF5C296 I/O passes the ATA task-file aperture through to the card.
     * Byte high-lane accesses increment the card byte address and shift data. */
    assert(gnet_zoom_bus_read(board, 0x1fb00008, 2, &value) == GNET_ZOOM_OK &&
           value == 0x1208);
    probe.destroy_from_read = true;
    assert(gnet_zoom_bus_read(board, 0x1fb00008, 2, &value) == GNET_ZOOM_OK &&
           value == 0x1208);
    assert(probe.read_destroy_result == GNET_ZOOM_BUSY);
    probe.destroy_from_read = false;
    assert(probe.read_memory_offset == 8 &&
           probe.read_memory_mask == 0xffff);
    assert(gnet_zoom_bus_read(board, 0x1fb00009, 1, &value) == GNET_ZOOM_OK &&
           value == 9);
    assert(probe.read_memory_offset == 9 && probe.read_memory_mask == 0x00ff);
    assert(gnet_zoom_bus_write(board, 0x1fb00009, 1, 0x5a) == GNET_ZOOM_OK);
    assert(probe.write_memory_offset == 9 &&
           probe.write_memory_data == 0x5a &&
           probe.write_memory_mask == 0x00ff);

    /* The RF5C296 low/high byte lanes at 0x3e0/0x3e1 select the indexed
     * register and access its value. Register reads return zero in MAME;
     * register 3 bit 6 low resets the card. */
    assert(gnet_zoom_bus_write(board, 0x1fb003e0, 1, 3) == GNET_ZOOM_OK);
    assert(gnet_zoom_bus_read(board, 0x1fb003e0, 1, &value) == GNET_ZOOM_OK &&
           value == 3);
    assert(gnet_zoom_bus_read(board, 0x1fb003e1, 1, &value) == GNET_ZOOM_OK &&
           value == 0);
    assert(gnet_zoom_bus_write(board, 0x1fb003e1, 1, 0x40) == GNET_ZOOM_OK);
    assert(probe.reset_count == 0);
    probe.destroy_from_reset = true;
    assert(gnet_zoom_bus_write(board, 0x1fb003e1, 1, 0) == GNET_ZOOM_OK);
    assert(probe.reset_count == 1);
    assert(probe.reset_destroy_result == GNET_ZOOM_BUSY);
    probe.destroy_from_reset = false;

    assert(gnet_zoom_try_destroy(board) == GNET_ZOOM_OK);

    /* The established no-card behavior remains explicit for both windows. */
    config = media.config();
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK && board);
    assert(gnet_zoom_bus_read(board, 0x1f200000, 2, &value) ==
           GNET_ZOOM_UNSUPPORTED);
    assert(gnet_zoom_bus_read(board, 0x1f200000, 4, &value) ==
           GNET_ZOOM_UNSUPPORTED);
    assert(gnet_zoom_bus_write(board, 0x1f200000, 2, 0) ==
           GNET_ZOOM_UNSUPPORTED);
    assert(gnet_zoom_bus_read(board, 0x1fb00000, 1, &value) ==
           GNET_ZOOM_UNSUPPORTED);
    assert(gnet_zoom_bus_write(board, 0x1fb00000, 1, 0) ==
           GNET_ZOOM_UNSUPPORTED);
    assert(gnet_zoom_bus_read(board, 0x1f000000, 1, &value) ==
           GNET_ZOOM_OK && value == 0xff);
    assert(gnet_zoom_bus_read(board, 0x1f000000, 4, &value) ==
           GNET_ZOOM_OK && value == 0xffffffff);
    assert(gnet_zoom_try_destroy(board) == GNET_ZOOM_OK);
}

void test_nor_busy_time_and_bad_config()
{
    Media media;
    media.firm[0x100] = 0x00;
    auto config = media.config();
    gnet_zoom_board *board = nullptr;
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK);
    assert(gnet_zoom_main_write16(board, 0x1f000000, 0x20, 0xffff) == GNET_ZOOM_OK);
    assert(gnet_zoom_main_write16(board, 0x1f000000, 0xd0, 0xffff) == GNET_ZOOM_OK);
    assert(gnet_zoom_flash_ready(board, 0) == 0);
    assert(gnet_zoom_sync_to_cycle(board, 6250000ull) == GNET_ZOOM_OK);
    assert(gnet_zoom_flash_ready(board, 0) == 1);
    assert(media.firm[0] == 0xff && media.firm[1] == 0xff && media.firm[0x100] == 0xff);
    assert(gnet_zoom_sync_to_cycle(board, 1000) == GNET_ZOOM_TIME);
    gnet_zoom_destroy(board);

    config.firm_flash_size--;
    board = reinterpret_cast<gnet_zoom_board *>(uintptr_t(0x1));
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_ARGUMENT);
    assert(board == nullptr);
    board = reinterpret_cast<gnet_zoom_board *>(uintptr_t(0x1));
    assert(gnet_zoom_create(nullptr, &board) == GNET_ZOOM_ARGUMENT);
    assert(board == nullptr);
}

void test_cpu_fetch_advances_nor_at_bus_cycle_independent_of_quantum()
{
    Media one_quantum_media;
    ReadyProbe one_probe;
    gnet_zoom_board *one = create_loop_board(one_quantum_media, &one_probe,
                                              probe_flash_ready_from_audio);
    one_probe.board = one;
    write_intel_word(one, 0x1f000000, 0x0020);
    write_intel_word(one, 0x1f000000, 0x00d0);
    assert(gnet_zoom_flash_ready(one, 0) == 0);
    assert(gnet_zoom_sync_to_cycle(one, 6250000ull - 20) == GNET_ZOOM_OK);
    assert(gnet_zoom_flash_ready(one, 0) == 0);
    assert(read_word(one, 0x1f000000) == 0x0000); // busy status at predeadline
    assert(gnet_zoom_sync_to_cycle(one, 6250000ull + 20) == GNET_ZOOM_OK);
    assert(gnet_zoom_flash_ready(one, 0) == 1);
    assert(read_word(one, 0x1f000000) == 0x0080);
    assert(one_probe.frames > 0 && one_probe.ready_seen);
    assert(one_probe.first_ready_zsg_cycle >= 25000000ull);
    assert(one_probe.first_ready_zsg_cycle <= 25000000ull + 769);
    assert(gnet_zoom_try_destroy(one) == GNET_ZOOM_OK);

    Media split_media;
    ReadyProbe split_probe;
    gnet_zoom_board *split = create_loop_board(split_media, &split_probe,
                                                probe_flash_ready_from_audio);
    split_probe.board = split;
    write_intel_word(split, 0x1f000000, 0x0020);
    write_intel_word(split, 0x1f000000, 0x00d0);
    for (uint64_t cycle = 312500; cycle < 6250000; cycle += 312500) {
        assert(gnet_zoom_sync_to_cycle(split, cycle) == GNET_ZOOM_OK);
        assert(gnet_zoom_flash_ready(split, 0) == 0);
    }
    assert(gnet_zoom_sync_to_cycle(split, 6250000ull + 20) == GNET_ZOOM_OK);
    assert(gnet_zoom_flash_ready(split, 0) == 1);
    assert(read_word(split, 0x1f000000) == 0x0080);
    assert(split_probe.frames > 0 && split_probe.ready_seen);
    assert(split_probe.first_ready_zsg_cycle >= 25000000ull);
    assert(split_probe.first_ready_zsg_cycle <= 25000000ull + 769);
    uint64_t first_cycle_delta = one_probe.first_ready_zsg_cycle > split_probe.first_ready_zsg_cycle
        ? one_probe.first_ready_zsg_cycle - split_probe.first_ready_zsg_cycle
        : split_probe.first_ready_zsg_cycle - one_probe.first_ready_zsg_cycle;
    assert(first_cycle_delta <= 769);
    assert(gnet_zoom_try_destroy(split) == GNET_ZOOM_OK);
}

void test_busy_destroy_refusal_and_time_error_propagation()
{
    Media media;
    DestroyProbe probe;
    gnet_zoom_board *board = create_loop_board(media, &probe,
                                                probe_destroy_from_audio);
    probe.board = board;
    assert(gnet_zoom_run(board, 4000) >= 4000);
    assert(probe.callbacks != 0);
    assert(probe.result == GNET_ZOOM_BUSY);
    assert(gnet_zoom_sound_pc(board) == 0x80000); // loop is still alive after refusal
    assert(gnet_zoom_try_destroy(board) == GNET_ZOOM_OK);

    Media time_media;
    board = create_loop_board(time_media);
    assert(gnet_zoom_run(board, UINT64_MAX) == GNET_ZOOM_TIME);
    assert(gnet_zoom_try_destroy(board) == GNET_ZOOM_OK);
}

void test_mame_mb3773_control_bit_and_five_second_timer()
{
    Media media;
    SoftResetProbe probe;
    auto config = media.config();
    config.reset_opaque = &probe;
    config.request_soft_reset = probe_soft_reset;
    gnet_zoom_board *board = nullptr;
    assert(gnet_zoom_create(&config, &board) == GNET_ZOOM_OK && board);
    probe.board = board;

    constexpr uint64_t cycles_per_second = 6250000;
    assert(gnet_zoom_sync_to_cycle(board, 49 * cycles_per_second / 10) == GNET_ZOOM_OK);
    assert(gnet_zoom_control_write(board, 0x30) == GNET_ZOOM_OK); // CK high does not kick.
    assert(gnet_zoom_sync_to_cycle(board, 5 * cycles_per_second) == GNET_ZOOM_OK);
    assert(probe.requests == 1);
    assert(probe.destroy_result == GNET_ZOOM_BUSY);
    assert(gnet_zoom_watchdog_reset_requests(board) == 1);

    assert(gnet_zoom_control_write(board, 0x10) == GNET_ZOOM_OK); // Falling CK rearms at 5 s.
    assert(gnet_zoom_sync_to_cycle(board, 10 * cycles_per_second - 1) == GNET_ZOOM_OK);
    assert(probe.requests == 1);
    assert(gnet_zoom_sync_to_cycle(board, 10 * cycles_per_second) == GNET_ZOOM_OK);
    assert(probe.requests == 2);
    assert(probe.destroy_result == GNET_ZOOM_BUSY);
    assert(gnet_zoom_watchdog_reset_requests(board) == 2);
    assert(gnet_zoom_try_destroy(board) == GNET_ZOOM_OK);
}
} // namespace

int main()
{
    test_mn_executes_real_zoom_flash_and_bus_is_little_endian();
    test_bank_control_mapping_eprom_alias_and_wave_flash();
    test_source_maincpu_map_routes_registers_and_shared_ram_lanes();
    test_rf5c296_controller_and_optional_pc_card_windows();
    test_nor_busy_time_and_bad_config();
    test_mn_tms_program_reaches_real_zoom_audio_callback();
    test_cpu_fetch_advances_nor_at_bus_cycle_independent_of_quantum();
    test_busy_destroy_refusal_and_time_error_propagation();
    test_mame_mb3773_control_bit_and_five_second_timer();
    std::puts("G-Net Zoom/NOR standalone tests passed");
}
