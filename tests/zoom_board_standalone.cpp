#include "zoom_board.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

static void emit(std::vector<uint8_t> &rom, size_t &p,
                 std::initializer_list<uint8_t> bytes)
{
    for (uint8_t b : bytes) rom[p++] = b;
}
static void emit_store8(std::vector<uint8_t> &rom, size_t &p,
                        uint8_t value, uint32_t address)
{
    emit(rom, p, {0xf8, value, 0x00, 0xf4, 0x44,
                  uint8_t(address), uint8_t(address >> 8), uint8_t(address >> 16)});
}

struct FlashSpy { unsigned writes = 0; unsigned chips[4]{}; };
static int spy_flash_write(void *opaque, unsigned chip, uint32_t word,
                           uint16_t data, uint16_t mask)
{
    auto *spy = static_cast<FlashSpy *>(opaque);
    assert(word == 0 && data == 0x00ff && mask == 0xffff);
    assert(chip < 4);
    ++spy->writes;
    ++spy->chips[chip];
    return 0;
}
struct AudioSpy { unsigned frames = 0; uint64_t last_cycle = 0; float last_l = 0, last_r = 0; };
static void audio_frame(void *opaque, float left, float right, uint64_t cycle)
{
    auto *spy = static_cast<AudioSpy *>(opaque);
    ++spy->frames;
    spy->last_cycle = cycle;
    spy->last_l = left;
    spy->last_r = right;
}

static void test_executed_sound_cpu_bus_and_lanes()
{
    std::vector<uint8_t> boot(ZOOM_BOOT_FLASH_BYTES, 0xf6);
    std::array<std::vector<uint8_t>, 3> samples = {
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0),
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0),
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0)
    };
    size_t p = 0;
    emit(boot, p, {0xf8, 0x34, 0x12});                    // D0 = 0x1234
    emit(boot, p, {0xf4, 0x40, 0x00, 0x00, 0x80});      // word -> ZSG2[0]
    // Real MN10200 firmware initializes an unknown-purpose control latch.
    // MAME stores it and logs it; this must not stop the sound CPU.
    emit(boot, p, {0xf4, 0x40, 0x18, 0x06, 0x80});      // word -> ZSG2 control 0x0c
    emit(boot, p, {0xf4, 0x40, 0x10, 0x00, 0x40});      // word -> sound RAM
    emit(boot, p, {0xf4, 0x40, 0x20, 0x00, 0xe0});      // word -> shared RAM

    /* Program a small real TMS57002 tone routine through MN port 1 and its
     * byte data port. It is deliberately uploaded over the board pins. */
    emit_store8(boot, p, 0xff, 0xffe1); // P1 direction = output
    emit_store8(boot, p, 3, 0xfe64);    // deassert both active-low load pins
    emit_store8(boot, p, 2, 0xfe64);    // assert PLOAD only
    const uint8_t program_bytes[] = {
        0,0,0, 0,0,0, 0xcc,0x05,0x10, 0x01,0x10,0x00, 0xfc,0x40,0x00
    };
    for (uint8_t byte : program_bytes) emit_store8(boot, p, byte, 0xc00000);
    emit_store8(boot, p, 3, 0xfe64); // release PLOAD
    emit_store8(boot, p, 1, 0xfe64); // assert CLOAD only
    const uint8_t cload_bytes[] = {0x10,0x12,0x34,0x56,0x78};
    for (uint8_t byte : cload_bytes) emit_store8(boot, p, byte, 0xc00000);
    emit_store8(boot, p, 3, 0xfe64); // complete CLOAD transaction
    emit(boot, p, {0xf8, 0x34, 0x12});

    zoom_board_config config{};
    AudioSpy audio;
    config.boot_flash = boot.data();
    config.boot_flash_size = boot.size();
    for (unsigned i = 0; i < 3; ++i) {
        config.sample_flash[i] = samples[i].data();
        config.sample_flash_size[i] = samples[i].size();
    }
    config.audio_opaque = &audio;
    config.audio_frame = audio_frame;
    zoom_board *board = nullptr;
    assert(zoom_board_create(&config, &board) == ZOOM_BOARD_OK && board);
    assert(zoom_board_sound_pc(board) == 0x80000);
    assert(zoom_board_set_sound_reset(board, 0) == ZOOM_BOARD_OK);
    assert(zoom_board_run(board, 4000) >= 4000);
    assert(zoom_board_sound_pc(board) > 0x80000);
    assert(zoom_board_sound_d(board, 0) == 0x1234);
    assert(zoom_board_sound_ram_read(board, 0x10) == 0x34);
    assert(zoom_board_sound_ram_read(board, 0x11) == 0x12);
    assert(zoom_board_shared_read(board, 0x20) == 0x34);
    assert(zoom_board_shared_read(board, 0x21) == 0x12);
    assert(zoom_board_zsg_register(board, 0, 0) == 0x1234);
    assert(zoom_board_tms_program_word(board,0) == 0x00cc0510);
    assert(zoom_board_tms_program_word(board,1) == 0x00011000);
    assert(zoom_board_tms_program_word(board,2) == 0x00fc4000);
    assert(zoom_board_fault(board) == ZOOM_FAULT_NONE);

    assert(zoom_board_main_write32(board, 0x1fbe0000, 0x00bb00aa,
                                   0x00ff00ff) == ZOOM_BOARD_OK);
    assert(zoom_board_shared_read(board, 0) == 0xaa);
    assert(zoom_board_shared_read(board, 1) == 0xbb);
    assert(zoom_board_main_read32(board, 0x1fbe0000, 0xffffffff) == 0xffbbffaa);
    assert(zoom_board_main_write32(board, 0x1fbe0000, 0x0000cc00,
                                   0x0000ff00) == ZOOM_BOARD_OK); // disconnected lane 1
    assert(zoom_board_shared_read(board, 0) == 0xaa);
    assert(zoom_board_main_write32(board, 0x1fbe01fc, 0x00dd00ee,
                                   0x00ff00ff) == ZOOM_BOARD_OK);
    assert(zoom_board_shared_read(board, 254) == 0xee);
    assert(zoom_board_shared_read(board, 255) == 0xdd);

    /* ZSG uses the integer stream rate floor(25 MHz / 768), so the first
     * completed frame lands at 769 chip clocks, within 193 MN cycles. */
    uint64_t before = zoom_board_audio_frames(board);
    assert(zoom_board_sync_to_cycle(board, 5000) == ZOOM_BOARD_OK);
    assert(zoom_board_audio_frames(board) > before + 1);
    assert(audio.frames == zoom_board_audio_frames(board));
    assert(audio.last_cycle > 769);
    assert(std::isfinite(audio.last_l) && std::isfinite(audio.last_r));
    assert(std::abs(audio.last_l) > 0.00001f || std::abs(audio.last_r) > 0.00001f);
    float stereo[2];
    zoom_board_last_audio(board, stereo);
    assert(stereo[0] == stereo[0] && stereo[1] == stereo[1]);
    assert(zoom_board_main_write16(board, 0x1fb80002, 4, 0xffff) == ZOOM_BOARD_OK);
    assert(zoom_board_main_write16(board, 0x1fb80000, 0x3f00, 0xff00) == ZOOM_BOARD_OK);
    assert(zoom_board_sync_to_cycle(board, 5400) == ZOOM_BOARD_OK);
    zoom_board_last_audio(board, stereo);
    assert(stereo[0] == 0.0f);
    assert(zoom_board_main_write16(board, 0x1fb80000, 0x003f, 0xffff) == ZOOM_BOARD_OK);
    assert(zoom_board_sync_to_cycle(board, 6000) == ZOOM_BOARD_OK);
    zoom_board_last_audio(board, stereo);
    assert(std::abs(stereo[0]) > 0.00001f);
    const uint64_t stopped_cycle = zoom_board_main_cycles(board);
    assert(zoom_board_set_sound_reset(board, 1) == ZOOM_BOARD_OK);
    assert(zoom_board_sync_to_cycle(board, stopped_cycle - 1) == ZOOM_BOARD_OK);
    assert(zoom_board_main_cycles(board) == stopped_cycle);
    assert(zoom_board_set_sound_reset(board, 0) == ZOOM_BOARD_OK);
    assert(zoom_board_sync_to_cycle(board, stopped_cycle + 100) == ZOOM_BOARD_OK);
    assert(zoom_board_fault(board) == ZOOM_FAULT_NONE);
    zoom_board_destroy(board);
}

static void test_readonly_mn_flash_and_reset_flash_commands()
{
    std::vector<uint8_t> boot(ZOOM_BOOT_FLASH_BYTES, 0xf6);
    std::array<std::vector<uint8_t>, 3> samples = {
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0),
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0),
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0)
    };
    size_t p = 0;
    emit(boot, p, {0xf8, 0x00, 0x00});
    emit(boot, p, {0xf4, 0x40, 0x00, 0x00, 0x08}); // source MN map is read-only
    FlashSpy spy;
    zoom_board_config config{};
    config.boot_flash = boot.data();
    config.boot_flash_size = boot.size();
    for (unsigned i = 0; i < 3; ++i) {
        config.sample_flash[i] = samples[i].data();
        config.sample_flash_size[i] = samples[i].size();
    }
    config.flash_opaque = &spy;
    config.flash_write16 = spy_flash_write;
    zoom_board *board = nullptr;
    assert(zoom_board_create(&config, &board) == ZOOM_BOARD_OK);
    assert(spy.writes == 0);
    assert(zoom_board_set_sound_reset(board, 0) == ZOOM_BOARD_OK);
    assert(zoom_board_run(board, 30) >= 30);
    assert(spy.writes == 4);
    for (unsigned i = 0; i < 4; ++i) assert(spy.chips[i] == 1);
    assert(zoom_board_fault(board) == ZOOM_FAULT_NONE);
    assert(boot[0] == 0xf8); // borrowed image remains unchanged
    zoom_board_destroy(board);
}

static void test_reset_epoch_keeps_devices_monotonic()
{
    std::vector<uint8_t> boot(ZOOM_BOOT_FLASH_BYTES, 0xf6);
    std::array<std::vector<uint8_t>, 3> samples = {
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0),
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0),
        std::vector<uint8_t>(ZOOM_SAMPLE_FLASH_BYTES, 0)
    };
    zoom_board_config config{};
    config.boot_flash = boot.data();
    config.boot_flash_size = boot.size();
    for (unsigned i = 0; i < 3; ++i) {
        config.sample_flash[i] = samples[i].data();
        config.sample_flash_size[i] = samples[i].size();
    }
    zoom_board *board = nullptr;
    assert(zoom_board_create(&config, &board) == ZOOM_BOARD_OK);
    assert(zoom_board_sync_to_cycle(board, 1000) == ZOOM_BOARD_OK); // held reset
    assert(zoom_board_main_cycles(board) == 1000);
    assert(zoom_board_set_sound_reset(board, 0) == ZOOM_BOARD_OK);
    assert(zoom_board_run(board, 32) >= 32);
    uint64_t after_first_run = zoom_board_main_cycles(board);
    assert(after_first_run > 1000);
    assert(zoom_board_set_sound_reset(board, 1) == ZOOM_BOARD_OK);
    assert(zoom_board_sync_to_cycle(board, after_first_run + 500) == ZOOM_BOARD_OK);
    uint64_t held_time = zoom_board_main_cycles(board);
    assert(zoom_board_set_sound_reset(board, 0) == ZOOM_BOARD_OK);
    assert(zoom_board_main_cycles(board) == held_time);
    assert(zoom_board_run(board, 32) >= 32);
    assert(zoom_board_main_cycles(board) >= held_time + 32);
    assert(zoom_board_fault(board) == ZOOM_FAULT_NONE);
    zoom_board_destroy(board);
}

int main()
{
    test_executed_sound_cpu_bus_and_lanes();
    test_readonly_mn_flash_and_reset_flash_commands();
    test_reset_epoch_keeps_devices_monotonic();
    std::puts("Taito Zoom board standalone tests passed");
    return 0;
}
