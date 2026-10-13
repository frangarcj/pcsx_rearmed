/* GPL-2.0-or-later. Native host wiring for MAME-derived Taito FX-1S devices.
 * Source: MAME deb2f54fe7300e7a192690318beb860159bfd52a, sony/zn.cpp.
 * Device implementations retain their upstream BSD-3-Clause notices/licenses.
 */
#include "systemtaito.h"
#include "system10.h"
#include "system11.h"
#include "system12.h"
#include "psxmem.h"
#include "r3000a.h"
#include "arcade/taito_fx1.h"
#include "arcade/zn_board.h"
#include "arcade/psx_sio.h"
#include "arcade/pcm_wav.h"
#include "../include/gpu_vram.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* The sibling native ZN adapter is integrated by the host. */
extern int systemzn_active(void);
#define CONFIG_COUNT 25
#define REGION_COUNT 6

typedef struct MediaCheck {
    unsigned region, offset, length, group, stride, swap;
    uint32_t crc;
} MediaCheck;
typedef struct Profile {
    const char *expected[CONFIG_COUNT];
    int eligible;
    size_t sizes[REGION_COUNT];
    unsigned erase[REGION_COUNT];
    const MediaCheck *media;
    size_t media_count;
} Profile;
#include "arcade/taito_fx1_profiles.inc"
static const char *const region_files[REGION_COUNT] = {
    "main.bin", "banked.bin", "audiocpu.bin", "adpcma.bin", "cat702_key0.bin", "cat702_key1.bin"
};
static struct {
    TaitoFx1 *device;
    ZNMCU *mcu;
    zn_board_t zn;
    psx_sio_t *sio[2];
    const Profile *profile;
    uint8_t *regions[REGION_COUNT];
    uint8_t inputs[6], dsw;
    int mcu_rx, cat_rx[2], irq[2], fault, reset_pending, sound_barrier;
    uint32_t last_cycle;
    uint64_t cycles, sound_fraction, reset_requests, pin_callbacks;
    unsigned peak;
    PcmWav wav;
} board;

static uint32_t physical_address(uint32_t address)
{ return address >= 0x80000000u && address < 0xc0000000u ? address & 0x1fffffffu : address; }
static uint64_t cycle_us(uint64_t cycle)
{ return cycle / PSXCLK * 1000000u + cycle % PSXCLK * 1000000u / PSXCLK; }
static void sio_irq(void *opaque, int state)
{
    unsigned port = (unsigned)(uintptr_t)opaque;
    board.irq[port] = !!state;
    /* MAME intin7/8 latches only on a callback carrying state=1. An I_STAT
     * acknowledge remains cleared until the SIO sends another IRQ event. */
    if (state && psxRegs.ptrs.psxH)
        psxHu32ref(0x1070) |= SWAPu32(1u << (7 + port));
}
static void update_rx(void)
{ psx_sio_write_rxd(board.sio[0], board.mcu_rx && board.cat_rx[0] && board.cat_rx[1]); }
static void mcu_txd(void *opaque, int state)
{ (void)opaque; board.mcu_rx = !!state; update_rx(); }
static void mcu_dsr(void *opaque, int state)
{ (void)opaque; psx_sio_write_dsr(board.sio[0], state); }
static void cat_txd(void *opaque, unsigned chip, int state)
{ (void)opaque; if (chip < 2) board.cat_rx[chip] = !!state; update_rx(); }
static uint8_t read_input(void *opaque, zn_board_input_t input)
{ (void)opaque; return input < 6 ? board.inputs[input] : 0xff; }
static uint8_t read_dsw(void *opaque)
{ (void)opaque; return board.dsw; }
static int advance_pins(uint64_t cycle)
{
    if (zn_board_advance_to_us(&board.zn, cycle_us(cycle))) { board.fault = 1; return -1; }
    return 0;
}
static void sio_sck(void *opaque, int state)
{
    (void)opaque;
    /* During catch-up, elapsed_cycles is this bit's deadline, not the final target. */
    board.pin_callbacks++;
    if (!advance_pins(psx_sio_elapsed_cycles(board.sio[0])) && zn_board_sio_clock(&board.zn, state)) board.fault = 1;
}
static void sio_txd(void *opaque, int state)
{
    (void)opaque; board.pin_callbacks++;
    if (!advance_pins(psx_sio_elapsed_cycles(board.sio[0]))) zn_board_sio_tx(&board.zn, state);
}
static void request_reset(void *opaque)
{
    (void)opaque;
    if (getenv("PCSX_TRACE_TAITO_WATCHDOG"))
        fprintf(stderr,"Taito watchdog reset pc=%08x time_us=%" PRIu64 " IRQ=%04x/%04x SR=%08x RA=%08x SP=%08x EPC=%08x\n",
            psxRegs.pc,cycle_us(board.cycles),psxHu16(0x1070),psxHu16(0x1074),psxRegs.CP0.n.SR,
            psxRegs.GPR.n.ra,psxRegs.GPR.n.sp,psxRegs.CP0.n.EPC);
    board.reset_pending = 1; board.reset_requests++;
}
static void request_sound_barrier(void *opaque)
{ (void)opaque; board.sound_barrier = 1; }
static void audio_sample(void *opaque, int16_t left, int16_t right)
{
    int16_t channels[4] = {left, right, 0, 0};
    unsigned l = left < 0 ? -(int)left : left, r = right < 0 ? -(int)right : right;
    (void)opaque;
    if (l > board.peak) board.peak = l;
    if (r > board.peak) board.peak = r;
    if (pcm_wav_append4(&board.wav, channels, 1)) board.fault = 1;
}
static int create_devices(void)
{
    zn_board_config_t zcfg = {0}; zn_board_callbacks_t zcb = {0};
    ZNMCUConfig mcfg = {0}; psx_sio_callbacks_t scb = {0};
    taito_fx1_config_t cfg = {0}; taito_fx1_callbacks_t cb = {0};
    uint8_t keys[2][8]; unsigned i;
    board.mcu_rx = board.cat_rx[0] = board.cat_rx[1] = 1;
    memset(board.inputs, 0xff, sizeof(board.inputs)); board.dsw = 0x0f;
    for (i = 0; i < 2; i++) {
        scb.opaque = (void *)(uintptr_t)i; scb.irq = sio_irq;
        scb.sck = i ? NULL : sio_sck; scb.txd = i ? NULL : sio_txd;
        if (psx_sio_create(i ? PSX_SIO1 : PSX_SIO0, &scb, &board.sio[i])) return -1;
    }
    mcfg.dsw = read_dsw; mcfg.dsr = mcu_dsr; mcfg.txd = mcu_txd;
    if (zn_mcu_create(&mcfg, &board.mcu)) return -1;
    memcpy(keys[0], board.regions[4], 8); memcpy(keys[1], board.regions[5], 8);
    zcfg.model = ZN_BOARD_ZN1; zcfg.ram_bytes = 0x400000; zcfg.vram_bytes = 0x100000;
    zcfg.cat702_count = 2; zcfg.cat702_keys = (const uint8_t (*)[8])keys; zcfg.mcu = board.mcu;
    zcb.read_input = read_input; zcb.cat702_dataout = cat_txd;
    if (!zn_board_init(&board.zn, &zcfg, &zcb)) return -1;
    cfg.banked_rom = board.regions[1]; cfg.banked_rom_size = board.profile->sizes[1];
    cfg.audio_rom = board.regions[2]; cfg.audio_rom_size = board.profile->sizes[2];
    cfg.adpcm_a = board.regions[3]; cfg.adpcm_a_size = board.profile->sizes[3];
    cb.request_sound_synchronization = request_sound_barrier; cb.request_soft_reset = request_reset;
    cb.audio_sample = audio_sample;
    board.device = taito_fx1_create(&cfg, &cb);
    return board.device ? 0 : -1;
}
static int read_config(const char *directory, const Profile **profile)
{
    char path[4096], line[512], values[CONFIG_COUNT][128] = {{0}};
    uint32_t seen = 0; unsigned i, j; FILE *file;
    if (snprintf(path, sizeof(path), "%s/board.ini", directory) >= (int)sizeof(path)) return -1;
    file = fopen(path, "r"); if (!file) { perror(path); return -1; }
    while (fgets(line, sizeof(line), file)) {
        char *equal;
        if (!strchr(line, '\n') && !feof(file)) goto bad;
        line[strcspn(line, "\r\n")] = 0;
        if (!*line) continue;
        equal = strchr(line, '='); if (!equal || strchr(equal + 1, '=')) goto bad;
        *equal++ = 0;
        for (i = 0; i < CONFIG_COUNT && strcmp(line, config_keys[i]); i++) { }
        if (i == CONFIG_COUNT || (seen & (1u << i)) || !*equal || strlen(equal) >= sizeof(values[i])) goto bad;
        strcpy(values[i], equal); seen |= 1u << i;
    }
    if (ferror(file) || seen != (1u << CONFIG_COUNT) - 1) goto bad;
    for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++)
        if (!strcmp(values[1], profiles[i].expected[1])) {
            if (!profiles[i].eligible) goto bad;
            for (j = 0; j < CONFIG_COUNT; j++) if (strcmp(values[j], profiles[i].expected[j])) goto bad;
            *profile = &profiles[i]; fclose(file); return 0;
        }
bad:
    fprintf(stderr, "%s: invalid or unsupported Taito FX1S source profile\n", path);
    fclose(file); return -1;
}
static int load_region(const char *directory, unsigned region)
{
    char path[4096]; FILE *file; long size; size_t expected = board.profile->sizes[region];
    if (snprintf(path, sizeof(path), "%s/%s", directory, region_files[region]) >= (int)sizeof(path)) return -1;
    file = fopen(path, "rb"); if (!file) { perror(path); return -1; }
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 || (size_t)size != expected || fseek(file, 0, SEEK_SET)) {
        fprintf(stderr, "%s: expected exactly %zu bytes\n", path, expected); fclose(file); return -1;
    }
    board.regions[region] = malloc(expected);
    if (!board.regions[region] || fread(board.regions[region], 1, expected, file) != expected) { fclose(file); return -1; }
    return fclose(file) ? -1 : 0;
}
static uint32_t crc_byte(uint32_t crc, uint8_t value)
{
    unsigned i; crc ^= value;
    for (i = 0; i < 8; i++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    return crc;
}
static int validate_media(const Profile *p, uint8_t *const *regions)
{
    unsigned region;
    for (region = 0; region < REGION_COUNT; region++) {
        uint8_t *marked = calloc(p->sizes[region], 1); size_t i, j;
        if (!marked) return -1;
        for (i = 0; i < p->media_count; i++) {
            const MediaCheck *c = &p->media[i]; uint32_t crc = UINT32_MAX;
            if (c->region != region) continue;
            for (j = 0; j < c->length; j++) {
                size_t lane = j % c->group;
                size_t dst = c->offset + j / c->group * c->stride + (c->swap ? c->group - 1 - lane : lane);
                if (dst >= p->sizes[region] || marked[dst]) { free(marked); return -1; }
                marked[dst] = 1; crc = crc_byte(crc, regions[region][dst]);
            }
            if ((crc ^ UINT32_MAX) != c->crc) {
                fprintf(stderr, "Taito FX1S: %s source ROM CRC mismatch (load at 0x%x)\n", region_files[region], c->offset);
                free(marked); return -1;
            }
        }
        for (j = 0; j < p->sizes[region]; j++) if (!marked[j] && regions[region][j] != p->erase[region]) {
            fprintf(stderr, "Taito FX1S: %s source padding mismatch at 0x%zx\n", region_files[region], j);
            free(marked); return -1;
        }
        free(marked);
    }
    return 0;
}
int systemtaito_load(const char *directory)
{
    unsigned i;
    if (board.device || systemzn_active() || system10_active() || system11_active() || system12_active() ||
        psxRegs.ptrs.psxM || !directory) return -1;
    if (read_config(directory, &board.profile)) return -1;
    for (i = 0; i < REGION_COUNT; i++) if (load_region(directory, i)) goto bad;
    /* Source hashes are verified by prepare_taito_fx1_board.py. This host also
     * accepts explicitly synthetic packages for native wiring tests. */
    if (create_devices() || gpu_set_model(2) || gpu_set_vram_height(512)) goto bad;
    psxRamSize = 0x400000; psxRomSize = 0x80000; board.last_cycle = psxRegs.cycle;
    return 0;
bad:
    fprintf(stderr, "Taito FX1S: board initialization failed\n"); systemtaito_unload(); return -1;
}
static void invalidate_bank(void);
static void destroy_devices(void)
{
    unsigned i;
    pcm_wav_close(&board.wav); taito_fx1_destroy(board.device); zn_board_destroy(&board.zn);
    zn_mcu_destroy(board.mcu); for (i = 0; i < 2; i++) psx_sio_destroy(board.sio[i]);
    for (i = 0; i < REGION_COUNT; i++) free(board.regions[i]);
    memset(&board, 0, sizeof(board));
}
void systemtaito_unload(void)
{
    if (board.device) invalidate_bank();
    destroy_devices();
    if (!psxRegs.ptrs.psxM && !systemzn_active() && !system10_active() && !system11_active() && !system12_active()) {
        psxRamSize = 0x200000; psxRomSize = 0x80000; gpu_set_model(2); gpu_set_vram_height(512);
    }
}
int systemtaito_active(void) { return board.device != NULL; }
static void map_bank(void)
{
    taito_fx1_status_t status; unsigned segment, page;
    const uint32_t bases[] = {0x1f000000, 0x9f000000, 0xbf000000};
    if (!board.device || DISABLE_MEM_LUTS || !psxRegs.ptrs.memRLUT || !psxRegs.ptrs.memWLUT) return;
    taito_fx1_get_status(board.device, &status);
    for (segment = 0; segment < 3; segment++) {
        uintptr_t ptr = status.main_bank_offset + 0x800000 <= board.profile->sizes[1] ?
            (uintptr_t)(board.regions[1] + status.main_bank_offset) - bases[segment] : INVALID_PTR_VAL;
        for (page = 0; page < 0x800000; page += 1u << PSXM_SHIFT) {
            uint32_t index = (bases[segment] + page) >> PSXM_SHIFT;
            psxRegs.ptrs.memRLUT[index] = ptr; psxRegs.ptrs.memWLUT[index] = INVALID_PTR_VAL;
        }
        if (psxCpu && psxCpu->Clear) psxCpu->Clear(bases[segment], 0x800000 / 4);
    }
}
static void invalidate_bank(void)
{
    const uint32_t bases[] = {0x1f000000, 0x9f000000, 0xbf000000};
    unsigned segment, page;
    if (DISABLE_MEM_LUTS || !psxRegs.ptrs.memRLUT || !psxRegs.ptrs.memWLUT) return;
    for (segment = 0; segment < 3; segment++) {
        for (page = 0; page < 0x800000; page += 1u << PSXM_SHIFT) {
            uint32_t index = (bases[segment] + page) >> PSXM_SHIFT;
            psxRegs.ptrs.memRLUT[index] = INVALID_PTR_VAL;
            psxRegs.ptrs.memWLUT[index] = INVALID_PTR_VAL;
        }
        if (psxCpu && psxCpu->Clear) psxCpu->Clear(bases[segment], 0x800000 / 4);
    }
}
void systemtaito_mem_reset(void)
{
    if (!board.device) return;
    memcpy(psxRegs.ptrs.psxR, board.regions[0], 0x80000);
    memset(psxRegs.ptrs.psxM, 0, PSX_RAM_SIZE); memset(psxRegs.ptrs.psxP, 0xff, 0x10000);
    map_bank();
}
void systemtaito_reset(void)
{
    if (!board.device) return;
    /* zn_state and CAT702/ZNMCU/AT28C16/SIO have no source reset override.
     * Retain common I/O, in-flight serial transactions and absolute clock. */
    taito_fx1_reset(board.device); map_bank();
    board.last_cycle = psxRegs.cycle; board.sound_fraction = 0;
    board.reset_pending = board.sound_barrier = board.fault = 0; board.peak = 0;
}
int systemtaito_sync(uint32_t cycle)
{
    uint32_t delta; unsigned i; taito_fx1_status_t status;
    if (!board.device) return 0;
    delta = cycle - board.last_cycle; board.last_cycle = cycle;
    if (delta > UINT64_MAX - board.cycles) { board.fault = 1; return -1; }
    board.cycles += delta;
    /* Advance serial edges first, at each deadline, then the remaining timer span. */
    for (i = 0; i < 2; i++) if (psx_sio_advance_to_cycle(board.sio[i], board.cycles)) board.fault = 1;
    if (advance_pins(board.cycles) || taito_fx1_advance_to_us(board.device, cycle_us(board.cycles)) < 0) board.fault = 1;
    board.sound_fraction += (uint64_t)delta * TAITO_FX1_AUDIO_CLOCK_HZ;
    if (taito_fx1_run_sound_cycles(board.device, board.sound_fraction / PSXCLK) < 0) board.fault = 1;
    board.sound_fraction %= PSXCLK;
    if (board.sound_barrier) { taito_fx1_sound_synchronization_completed(board.device); board.sound_barrier = 0; }
    taito_fx1_get_status(board.device, &status);
    return board.fault || status.faults ? -1 : board.reset_pending;
}
int systemtaito_reset_requested(void) { return board.reset_pending; }
static int sio_bus(uint32_t address, unsigned width, uint32_t *value, int write)
{
    uint32_t raw, mask, shift; unsigned port;
    if (address < 0x1f801040 || address >= 0x1f801060) return 0;
    port = (address - 0x1f801040) >> 4; shift = (address & 3) * 8;
    mask = (width == 4 ? UINT32_MAX : (1u << (width * 8)) - 1) << shift;
    if (write) {
        if (psx_sio_write(board.sio[port], (address >> 2) & 3, *value << shift, mask)) board.fault = 1;
    } else {
        if (psx_sio_read(board.sio[port], (address >> 2) & 3, mask, &raw)) { board.fault = 1; raw = UINT32_MAX; }
        *value = (raw & mask) >> shift;
    }
    return 1;
}
int systemtaito_read(uint32_t address, unsigned width, uint32_t *value)
{
    int result; uint32_t physical = physical_address(address);
    if (!board.device || !value || (width != 1 && width != 2 && width != 4) || (physical & (width - 1))) return 0;
    systemtaito_sync(psxRegs.cycle);
    if (sio_bus(physical, width, value, 0)) return 1;
    result = taito_fx1_main_read(board.device, physical, width, value);
    if (!result) result = zn_board_read(&board.zn, physical, width, value);
    if (result < 0) { board.fault = 1; *value = UINT32_MAX; }
    return result != 0;
}
int systemtaito_write(uint32_t address, unsigned width, uint32_t value)
{
    int result; uint32_t physical = physical_address(address);
    if (!board.device || (width != 1 && width != 2 && width != 4) || (physical & (width - 1))) return 0;
    systemtaito_sync(psxRegs.cycle);
    if (sio_bus(physical, width, &value, 1)) return 1;
    result = taito_fx1_main_write(board.device, physical, width, value);
    if (!result) result = zn_board_write(&board.zn, physical, width, value);
    if (result < 0) board.fault = 1;
    if (result > 0 && physical == 0x1fb40000) map_bank();
    return result != 0;
}
static const struct InputBinding { const char *name; unsigned port, mask; } input_bindings[] = {
    {"p1_up",0,1}, {"p1_down",0,2}, {"p1_left",0,4}, {"p1_right",0,8},
    {"p1_button1",0,16}, {"p1_button2",0,32}, {"p1_button3",0,64},
    {"p2_up",1,1}, {"p2_down",1,2}, {"p2_left",1,4}, {"p2_right",1,8},
    {"p2_button1",1,16}, {"p2_button2",1,32}, {"p2_button3",1,64},
    {"test",2,1}, {"service",2,2}, {"tilt",2,4},
    {"p1_start",3,1}, {"p2_start",3,2}, {"coin1",3,16}, {"coin2",3,32},
    {"bios_service_dip",6,2}, {"test_mode_dip",6,8}
};
int systemtaito_set_input(const char *name, int pressed)
{
    unsigned i;
    if (!board.device || !name || (pressed != 0 && pressed != 1)) return -1;
    for (i = 0; i < sizeof(input_bindings) / sizeof(input_bindings[0]); i++) if (!strcmp(name, input_bindings[i].name)) {
        uint8_t *input = input_bindings[i].port == 6 ? &board.dsw : &board.inputs[input_bindings[i].port];
        if (pressed) *input &= ~input_bindings[i].mask; else *input |= input_bindings[i].mask;
        return 0;
    }
    return -1;
}
int systemtaito_eeprom_import(const unsigned char *image, size_t size)
{ return board.device ? zn_board_eeprom_import(&board.zn, image, size) : -1; }
int systemtaito_eeprom_export(unsigned char *image, size_t size)
{ return board.device ? zn_board_eeprom_export(&board.zn, image, size) : -1; }
int systemtaito_open_audio(const char *path)
{ return board.device ? pcm_wav_open(&board.wav, path, TAITO_FX1_YM_CLOCK_HZ / 144) : -1; }
int systemtaito_close_audio(void) { return pcm_wav_close(&board.wav); }
void systemtaito_report(FILE *out)
{
    taito_fx1_status_t s; uint32_t sio_status[2]; unsigned i;
    if (!out || !board.device) return;
    taito_fx1_get_status(board.device, &s);
    for (i = 0; i < 2; i++) psx_sio_read(board.sio[i], 1, UINT32_MAX, &sio_status[i]);
    fprintf(out, "Taito FX1S set=%s model=COH1000T ram=4MiB vram=1MiB gpu=type2 bios=512KiB "
        "source_cpu=CXD8530CQ crystal=67737600 native_clock=%u source_gpu=CXD8561Q crystal=53693175\n",
        board.profile->expected[1], PSXCLK);
    fprintf(out, "Taito FX1S cycles=%" PRIu64 " time_us=%" PRIu64 " mainbank=%u soundbank=%u "
        "z80pc=%04x z80cycles=%" PRIu64 " instructions=%" PRIu64 " audio=%" PRIu64 " peak=%u "
        "sio0=%04x sio1=%04x pin_callbacks=%" PRIu64 " watchdog_requests=%" PRIu64 " reset_pending=%d fault=%x\n",
        board.cycles, cycle_us(board.cycles), s.main_bank, s.sound_bank, s.z80_pc,
        s.sound_cycles, s.z80_instructions, s.audio_samples, board.peak, sio_status[0], sio_status[1],
        board.pin_callbacks, board.reset_requests, board.reset_pending, s.faults | (board.fault ? 0x80000000u : 0));
}

static int selftest_active(void)
{
    uint32_t value; taito_fx1_status_t s;
    unsigned i;
    if (!board.profile->eligible || board.profile->sizes[0] != 0x80000 || board.zn.ram_bytes != 0x400000 ||
        board.zn.vram_bytes != 0x100000 || board.zn.cat702_count != 2) return -1;
    if (!zn_board_read(&board.zn, 0x1fa10200, 4, &value) || value != 0x61) return -1;
    for (i = 0; i < 2; i++) if (psx_sio_read(board.sio[i], 1, UINT32_MAX, &value)) return -1;
    taito_fx1_get_status(board.device, &s);
    if (s.main_bank < 2) {
        if (taito_fx1_main_read(board.device, 0x1f000000, 4, &value) != TAITO_FX1_HANDLED) return -1;
        for (i = 0; i < 4; i++) if (((value >> (i * 8)) & 0xff) != board.regions[1][s.main_bank_offset + i]) return -1;
    }
    /* znt2p disables these bits; unknown controls must not silently become buttons. */
    if (systemtaito_set_input("p1_button4", 1) != -1 || systemtaito_set_input("p3_start", 1) != -1) return -1;
    return 0;
}
int systemtaito_selftest(void)
{
    uint32_t saved_cycle = psxRegs.cycle, value;
    uint64_t epoch; unsigned i; int result = -1;
    uint8_t image[AT28C16_IMAGE_BYTES], expected[AT28C16_IMAGE_BYTES];
    if (board.device) return selftest_active();
    /* Inactive trace must never alter an initialized console's LUTs or IRQ register. */
    if (psxRegs.ptrs.psxM) return -1;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "Taito FX1S selftest failed line %d: %s\n", __LINE__, #x); systemtaito_report(stderr); goto done; } } while (0)
    CHECK(crc_byte(UINT32_MAX, '1') == 0x7c231048u);
    {
        /* Independent standard CRC vector recovered from an interleaved bus layout. */
        Profile p = {{0}, 1, {18,1,1,1,1,1}, {0}, NULL, 0};
        const MediaCheck checks[] = {{0,0,9,1,2,0,0xcbf43926u},
            {1,0,1,1,1,0,0xd202ef8du}, {2,0,1,1,1,0,0xd202ef8du},
            {3,0,1,1,1,0,0xd202ef8du}, {4,0,1,1,1,0,0xd202ef8du}, {5,0,1,1,1,0,0xd202ef8du}};
        uint8_t zero = 0, digits[18] = {0};
        uint8_t *regions[] = {digits, &zero, &zero, &zero, &zero, &zero};
        for (i = 0; i < 9; i++) digits[i * 2] = (uint8_t)('1' + i);
        p.media = checks; p.media_count = sizeof(checks) / sizeof(checks[0]);
        CHECK(validate_media(&p, regions) == 0);
    }
    for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++)
        if (!strcmp(profiles[i].expected[1], "sfchamp")) board.profile = &profiles[i];
    CHECK(board.profile != NULL);
    for (i = 0; i < REGION_COUNT; i++) { board.regions[i] = calloc(board.profile->sizes[i], 1); CHECK(board.regions[i]); }
    /* Explicit synthetic fixture: HALT-only Z80 ROM and synthetic CAT transform bytes. */
    board.regions[2][0] = 0x76;
    board.regions[1][0] = 0x34; board.regions[1][1] = 0x12; board.regions[1][0x800000] = 0x56;
    for (i = 0; i < 8; i++) board.regions[4][i] = board.regions[5][i] = (uint8_t)(i * 17);
    psxRegs.cycle = 0; CHECK(!create_devices()); systemtaito_reset();
    CHECK(!selftest_active());
    CHECK(systemtaito_set_input("p1_button3", 1) == 0);
    CHECK(systemtaito_read(0x1fa00000, 1, &value) && value == 0xbf);
    CHECK(systemtaito_set_input("p1_button3", 0) == 0);
    CHECK(systemtaito_write(0x1fb40000, 1, 1));
    CHECK(systemtaito_read(0x9f000000, 1, &value) && value == 0x56);
    CHECK(systemtaito_write(0x1fb40000, 1, 0));
    CHECK(systemtaito_read(0xbf000000, 2, &value) && value == 0x1234);
    CHECK(!systemtaito_read(0x1fb90000, 1, &value));
    /* EEPROM busy reads are last_write XOR 0x80 at every offset; reset retains busy. */
    memset(image, 0xff, sizeof(image)); image[0x800] = 0x21;
    CHECK(!systemtaito_eeprom_import(image, sizeof(image)));
    CHECK(systemtaito_write(0x1faf0001, 1, 0x5a));
    CHECK(systemtaito_read(0x1faf0002, 1, &value) && value == 0xda);
    CHECK(systemtaito_write(0x1fa20000, 1, 0xa5));
    psxRegs.cycle = (uint32_t)(((uint64_t)199 * PSXCLK + 999999) / 1000000);
    CHECK(!systemtaito_sync(psxRegs.cycle)); epoch = board.cycles;
    psxRegs.cycle = 0; systemtaito_reset();
    CHECK(board.cycles == epoch && systemtaito_read(0x1faf0002, 1, &value) && value == 0xda);
    CHECK(systemtaito_read(0x1fa20000, 1, &value) && value == 0xa5);
    psxRegs.cycle = (PSXCLK + 999999) / 1000000;
    CHECK(!systemtaito_sync(psxRegs.cycle));
    CHECK(systemtaito_read(0x1faf0001, 1, &value) && value == 0x5a);
    CHECK(!systemtaito_eeprom_export(expected, sizeof(expected)) && expected[0x800] == 0x21);
    /* MCU selected, CATs deselected; 50us DSR low, five us later high.
     * One late SIO advance crosses all eight deadlines. Pins must see each deadline. */
    CHECK(systemtaito_write(0x1fa10300, 1, 0x8c));
    psxRegs.cycle += (uint32_t)(((uint64_t)55 * PSXCLK + 999999) / 1000000);
    CHECK(!systemtaito_sync(psxRegs.cycle));
    CHECK(systemtaito_write(0x1f801048, 2, 1)); /* prescaler one */
    CHECK(systemtaito_write(0x1f80104e, 2, 100));
    CHECK(systemtaito_write(0x1f80104a, 2, PSX_SIO_CONTROL_TX_ENA | PSX_SIO_CONTROL_RX_IENA));
    CHECK(systemtaito_write(0x1f801040, 1, 0));
    psxRegs.cycle += 800; CHECK(!systemtaito_sync(psxRegs.cycle));
    CHECK(systemtaito_read(0x1f801044, 2, &value) && (value & PSX_SIO_STATUS_RX_RDY));
    CHECK(systemtaito_read(0x1f801040, 1, &value) && value == 0x1f);
    CHECK(board.pin_callbacks == 24 && board.irq[0]);
    CHECK(systemtaito_write(0x1f80104a, 2, PSX_SIO_CONTROL_IACK));
    CHECK(!board.irq[0]);
    /* SIO1 is separately mapped and has no fabricated RX connection. */
    CHECK(systemtaito_write(0x1f801058, 2, 1));
    CHECK(systemtaito_write(0x1f80105e, 2, 10));
    CHECK(systemtaito_write(0x1f80105a, 2, PSX_SIO_CONTROL_TX_ENA | PSX_SIO_CONTROL_TX_IENA));
    CHECK(systemtaito_write(0x1f801050, 1, 0x12));
    psxRegs.cycle += 80; CHECK(!systemtaito_sync(psxRegs.cycle));
    CHECK(board.irq[1]);
    CHECK(systemtaito_read(0x1f801054, 2, &value) && !(value & PSX_SIO_STATUS_RX_RDY));
    /* Watchdog test uses its isolated virtual clock, without burning 20M sound cycles. */
    CHECK(taito_fx1_main_write(board.device, 0x1fb40000, 1, 1) == TAITO_FX1_HANDLED);
    CHECK(taito_fx1_advance_to_us(board.device, cycle_us(epoch) + 4999999) == TAITO_FX1_HANDLED); CHECK(!systemtaito_reset_requested());
    CHECK(taito_fx1_advance_to_us(board.device, cycle_us(epoch) + 5000000) == TAITO_FX1_HANDLED);
    CHECK(systemtaito_reset_requested() && board.reset_requests == 1);
    CHECK(taito_fx1_main_read(board.device, 0x1f000000, 1, &value) == TAITO_FX1_HANDLED && value == 0x56);
    /* Bank one persists until the host explicitly services the deferred reset. */
    CHECK(board.reset_pending && board.device);
    board.cycles = ((cycle_us(epoch) + 5000000) * PSXCLK + 999999) / 1000000;
    CHECK(systemtaito_sync(psxRegs.cycle) == 1);
    systemtaito_reset(); CHECK(!systemtaito_reset_requested());
    CHECK(!systemtaito_eeprom_export(image, sizeof(image)) && !memcmp(image, expected, sizeof(image)));
    result = 0;
done:
    destroy_devices(); psxRegs.cycle = saved_cycle;
    return result;
#undef CHECK
}
