/*
 * Namco System 12 base-board profiles for the interpreter harness.
 * Board mapping/banking/DMA derived from MAME namcos12.cpp by smf,
 * BSD-3-Clause. See THIRD_PARTY_NOTICES.md for the retained license.
 * New integration code: GPL-2.0-or-later, as the surrounding PCSX core.
 *
 * RTC/settings serial wiring and JAMMA conversion are adapted from MAME;
 * shared RAM exchanges are produced by the actual H8 ROM in the interpreter.
 */
#include "system12.h"
#include "system12_profile.h"
#include "system12_audio_clock.h"
#include "system10.h"
#include "system11.h"
#include "systemtaito.h"
#include "systemzn.h"
#include "psxmem.h"
#include "psxhw.h"
#include "psxdma.h"
#include "psxinterpreter.h"
#include "psxcounters.h"
#include "plugins.h"
#include <inttypes.h>
#include "arcade/h8.h"
#include "arcade/c352.h"
#include "gpu_vram.h"
#include "arcade/serial_nvram.h"
#include "arcade/at28c16.h"
#include "arcade/system12_jvs.h"
#include "arcade/system12_cdxa.h"
#include "psxevents.h"
#include "misc.h"
#include <errno.h>
#ifdef SYSTEM12_BOARD_THREAD
#include <stdatomic.h>
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#endif
#include <retro_timers.h>
#include "../frontend/pcsxr-threads.h"
#endif

#ifndef DRC_DISABLE
#include "new_dynarec/new_dynarec.h"
#endif
#if DISABLE_MEM_LUTS
#error System12 requires memory LUTs for banked instruction fetch
#endif

enum { DEFAULT_MAIN_SIZE = 0x400000, DEFAULT_BANK_SIZE = 0x2000000,
       DEFAULT_SUB_SIZE = 0x80000, DEFAULT_WAVE_SIZE = 0x1000000,
       DEFAULT_RAM_SIZE = 0x400000, WINDOW_SIZE = 0x200000 };
static struct {
    u8 *main, *banked, *sub, *wave;
    size_t main_size, banked_size, sub_size, wave_size;
    u32 ram_size;
    u8 shared[0x10000];
    AT28C16 *eeprom;
    u8 default_eeprom[2080];
    size_t default_eeprom_size;
    H8CPU *h8;
    H8CPU *link;
    S12Cdxa *cdxa;
    char *disk_path;
    u8 link_shared[0x10000], link_ram[0x10000], link_regs[0x2000];
    u8 link_io[0x200], link_small[32], link_internal[0xf0];
    int link_reset;
    C352 *pcm;
    rtc4543_serial_t rtc;
    namco_settings_serial_t settings;
    u64 rtc_fraction, eeprom_time_us, eeprom_cycle_fraction;
    int eeprom_clock_error;
    u32 last_cycle;
    u64 h8_fraction, h8_requested, pcm_fraction, pcm_due, pcm_frames;
    u64 pcm_epoch_frames, sub_reads, sub_writes;
    System12AudioClock pcm_h8_clock;
    unsigned pcm_peak;
    FILE *audio_file;
    system12_audio_cb audio_cb;
    void *audio_opaque;
    u64 audio_bytes;
    int audio_error;
    u32 jvs_player[2], jvs_system;
    u8 jvs_coin[2];
    u16 gun[4], analog[8], gun_outputs;
    S12Jvs jvs;
    S12Jvs jvs_printer;
    int jvs_enabled;
    u8 port8, porta, portb;
    u32 dma_offset;
    u32 bank;
    u32 alt_bank_offset, tektagt_dma_offset;
    const char *profile, *machine, *state, *unsupported;
    int boot_hack, alt_bank, tektagt_dma, has_tektagt_dma;
    /* Tekken Tag protection (MAME tektagt_state): two 32-bit values written
     * through 0x1fb00000 after a 0x1fb80000 write select a reply. */
    int ttt_cnt;
    u32 ttt_val[2];
    int profile_owned;
    u64 shared_reads, shared_writes, bank_writes, dma_transfers, dma_bytes;
    u64 unknown_reads, unknown_writes, dma_truncations;
    u32 last_shared_address, last_shared_pc, last_shared_value;
    u32 last_unknown_address, last_unknown_pc;
    int active, booted;
    /* MAME timeslice order for the source instruction step profile. */
    psx_screen_time slice_base, slice_target;
    int slice_open, slice_ran;
} board;
static void pcm_sync_h8(void);
/* Until this stream frame no C352 voice flags can change: an H8 read of the
 * flags at or before it needs no generating (0: unknown). Not in `board`,
 * whose size the savestates check. */
static u64 pcm_flags_until;
/* The stream frame board SYNC commands asked for: generated in blocks
 * (PCM_BATCH), at vblank, and before anything that needs it. */
static u64 pcm_wanted;
#define PCM_BATCH 256
static void pcm_flush_wanted(void);
/* PCSX_PCM_BATCH=1 (Mac harness): one frame per command, as before, to
 * compare. */
static unsigned pcm_batch(void)
{
#if !defined(__vita__)
    static int unbatched = -1;
    if (unbatched < 0) {
        const char *v = getenv("PCSX_PCM_BATCH");
        unbatched = v && atoi(v) <= 1;
    }
    if (unbatched) return 1;
#endif
    return PCM_BATCH;
}
static void pcm_flags_horizon(void);
static int pcm_access_needs_sync(unsigned reg, int write, u16 value, u16 mask);
/* The board's time split, H8 and C352, in microseconds (the Vita
 * frontend's progress lines, the harness's H8 perf line). */
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
static inline u64 board_now_us(void) { return sceKernelGetProcessTimeWide(); }
#elif defined(__APPLE__)
/* mach_absolute_time reads a counter; clock_gettime went through
 * gettimeofday and cost as much as the board's work it timed. */
#include <mach/mach_time.h>
static inline u64 board_now_us(void)
{
    static mach_timebase_info_data_t base;
    if (!base.denom) mach_timebase_info(&base);
    return mach_absolute_time() * base.numer / base.denom / 1000u;
}
#else
#include <time.h>
static inline u64 board_now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000000u + (u64)t.tv_nsec / 1000u;
}
#endif
unsigned long long system12_h8_us, system12_pcm_us;
/* The timings above cost a clock read per board command (over 100000 a
 * second): measured only when the frontend asks (diagnostic builds, the
 * harness's perf line). */
int system12_perf_timing;
#define PERF_NOW() (system12_perf_timing ? board_now_us() : 0)
typedef struct board_cmd board_cmd;
static void board_post(const board_cmd *c);
static void board_drain(void);
static void board_audio_out(const int16_t *stereo, unsigned frames);
static system12_bus_trace_cb bus_trace_cb;
static void *bus_trace_opaque;
static system12_edge_trace_cb edge_trace_cb;
static void *edge_trace_opaque;
static int edge_trace_shared;

void system12_set_edge_trace(system12_edge_trace_cb callback, void *opaque)
{
    edge_trace_cb = callback;
    edge_trace_opaque = opaque;
}
void system12_set_edge_trace_shared(int flags)
{
    edge_trace_shared = flags & (SYSTEM12_TRACE_SHARED_STORES | SYSTEM12_TRACE_SHARED_LOADS);
}

static void trace_edge(unsigned kind, unsigned value)
{
    if (edge_trace_cb) {
        system12_edge_event event = {kind, value, psxRegs.cycle,
            board.last_cycle, h8_current_cycle(board.h8), board.h8_requested, 0, 0};
        edge_trace_cb(edge_trace_opaque, &event);
    }
}
static void trace_shared(unsigned kind, u32 address, unsigned width, u32 value)
{
    unsigned flag = kind == SYSTEM12_EDGE_H8_LOAD || kind == SYSTEM12_EDGE_MAIN_SHARED_LOAD
        ? SYSTEM12_TRACE_SHARED_LOADS : SYSTEM12_TRACE_SHARED_STORES;
    if (edge_trace_cb && (edge_trace_shared & flag)) {
        system12_edge_event event = {kind, value,
            psxRegs.cycle, board.last_cycle, h8_current_cycle(board.h8),
            board.h8_requested, address, width};
        edge_trace_cb(edge_trace_opaque, &event);
    }
}

void system12_set_bus_trace(system12_bus_trace_cb callback, void *opaque)
{
    bus_trace_cb = callback;
    bus_trace_opaque = opaque;
}

static void trace_bus(u32 address, unsigned width, int is_write, u32 value)
{
    if (bus_trace_cb)
        bus_trace_cb(bus_trace_opaque, address, width, is_write, value,
            psxRegs.pc, psxRegs.cycle);
}

static const struct { const char *name; unsigned player, mask; } player_inputs[] = {
    {"p1start", 0, 0x00000080}, {"p1up", 0, 0x00000020},
    {"p1down", 0, 0x00000010}, {"p1left", 0, 0x00000008},
    {"p1right", 0, 0x00000004}, {"p1button1", 0, 0x00000002},
    {"p1button2", 0, 0x00000001}, {"p1button3", 0, 0x00008000},
    {"p1button4", 0, 0x00004000}, {"p1button5", 0, 0x00002000},
    {"p1button6", 0, 0x00001000}, {"p1button7", 0, 0x00000800},
    {"p1button8", 0, 0x00000400},
    {"p2start", 1, 0x00000080}, {"p2up", 1, 0x00000020},
    {"p2down", 1, 0x00000010}, {"p2left", 1, 0x00000008},
    {"p2right", 1, 0x00000004}, {"p2button1", 1, 0x00000002},
    {"p2button2", 1, 0x00000001}, {"p2button3", 1, 0x00008000},
    {"p2button4", 1, 0x00004000}, {"p2button5", 1, 0x00002000},
    {"p2button6", 1, 0x00001000}, {"p2button7", 1, 0x00000800},
    {"p2button8", 1, 0x00000400}
};

static void put_le16(u8 *p, u16 value)
{
    p[0] = value; p[1] = value >> 8;
}

static void put_le32(u8 *p, u32 value)
{
    p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}

static int audio_write_header(FILE *f, u32 data_bytes)
{
    u8 h[44] = {0};
    memcpy(h, "RIFF", 4); put_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8); put_le32(h + 16, 16);
    put_le16(h + 20, 1); put_le16(h + 22, 2);
    put_le32(h + 24, 88200); put_le32(h + 28, 88200 * 4);
    put_le16(h + 32, 4); put_le16(h + 34, 16);
    memcpy(h + 36, "data", 4); put_le32(h + 40, data_bytes);
    return fseek(f, 0, SEEK_SET) || fwrite(h, 1, sizeof(h), f) != sizeof(h) ? -1 : 0;
}

static int audio_write_samples(const int16_t *samples, unsigned frames)
{
    u8 bytes[256 * 4];
    unsigned i;
    if (!board.audio_file || board.audio_error) return board.audio_error ? -1 : 0;
    if (board.audio_bytes + (u64)frames * 4 > 0xffffffc3u) {
        board.audio_error = 1; return -1;
    }
    for (i = 0; i < frames; i++) {
        put_le16(bytes + i * 4, (u16)samples[i * 4]);
        put_le16(bytes + i * 4 + 2, (u16)samples[i * 4 + 1]);
    }
    if (fwrite(bytes, 4, frames, board.audio_file) != frames) {
        board.audio_error = 1; return -1;
    }
    board.audio_bytes += (u64)frames * 4;
    return 0;
}

int system12_open_audio(const char *path)
{
    FILE *f;
    board_drain();
    if (!board.active || !path || board.audio_file) return -1;
    f = fopen(path, "wb+");
    if (!f) return -1;
    board.audio_file = f; board.audio_bytes = 0; board.audio_error = 0;
    if (audio_write_header(f, 0)) {
        fclose(f); board.audio_file = NULL; board.audio_error = 1;
        return -1;
    }
    if (fseek(f, 44, SEEK_SET)) {
        fclose(f); board.audio_file = NULL; board.audio_error = 1;
        return -1;
    }
    return 0;
}

int system12_close_audio(void)
{
    FILE *f;
    int failed;
    board_drain();
    pcm_flush_wanted();
    f = board.audio_file;
    if (!f) return board.audio_error ? -1 : 0;
    failed = board.audio_error || board.audio_bytes > 0xffffffc3u;
    if (!failed && audio_write_header(f, (u32)board.audio_bytes)) failed = 1;
    if (fclose(f)) failed = 1;
    board.audio_file = NULL;
    board.audio_error = failed;
    return failed ? -1 : 0;
}

/* Not board state: savestates keep the listener's volume. */
static volatile int audio_gain_q8 = 256;

void system12_set_volume(unsigned percent)
{
    audio_gain_q8 = (int)(percent > 1000 ? 1000 : percent) * 256 / 100;
}

static int16_t audio_gain(int16_t sample, int gain_q8)
{
    const int32_t v = (int32_t)sample * gain_q8 >> 8;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void system12_set_audio_output(system12_audio_cb callback, void *opaque)
{
    board_drain();
    system12_audio_pump();
    board.audio_cb = callback;
    board.audio_opaque = opaque;
}

/* Deterministic nonzero waveform fixture used only by harness validation. */
void system12_audio_test_fixture(void)
{
    unsigned i;
    if (!board.active || !board.pcm) return;
    board_drain();
    memset(board.wave, 0, 64);
    for (i = 0; i < 64; i++) board.wave[i] = 0x40;
    c352_reset(board.pcm);
    c352_write(board.pcm, 0, 0x8080, 0xffff); /* front L/R volume */
    c352_write(board.pcm, 2, 0x8000, 0xffff); /* half sample per frame */
    c352_write(board.pcm, 3, 0x4006, 0xffff); /* key on, filtered loop */
    c352_write(board.pcm, 6, 63, 0xffff);     /* inclusive wave end */
    c352_write(board.pcm, 0x202, 0, 0xffff);  /* execute-key */
}

/* Board work posted by the main CPU, run in posting order either at once or
 * on the board thread. The main CPU computes every target as before and only
 * touches state that board work uses (shared RAM, reports, reset) after
 * board_drain(). The H8 observes the main CPU only through shared RAM, the
 * vblank line and inputs, all ordered here, so the thread changes when the
 * work runs, never what it computes. */
enum { BOARD_CMD_SYNC, BOARD_CMD_VBLANK, BOARD_CMD_INPUT };
struct board_cmd {
    u8 kind, run_h8, run_until, level;
    u32 h8_cycles, rtc_seconds;
    u64 h8_target, pcm_target;
    u32 jvs_player[2], jvs_system;
    u8 jvs_coin[2];
    u16 gun[4], analog[8];
};
/* Inputs as set by the frontend; the board's copy follows in posting order. */
static struct { u32 player[2], system; u8 coin[2]; u16 gun[4], analog[8]; } inputs;

static void post_inputs(void)
{
    board_cmd c = {BOARD_CMD_INPUT};
    c.jvs_player[0] = inputs.player[0]; c.jvs_player[1] = inputs.player[1];
    c.jvs_system = inputs.system;
    c.jvs_coin[0] = inputs.coin[0]; c.jvs_coin[1] = inputs.coin[1];
    memcpy(c.gun, inputs.gun, sizeof(c.gun));
    memcpy(c.analog, inputs.analog, sizeof(c.analog));
    board_post(&c);
}

static int analog_index(const char *control, int *gun)
{
    static const char *const names[] = {"p1gunx", "p1guny", "p2gunx", "p2guny"};
    unsigned i;
    if (!control) return -1;
    for (i = 0; i < 4; i++) if (!strcmp(control, names[i])) { *gun = 1; return i; }
    if (!strncmp(control, "adc", 3) && control[3] >= '0' && control[3] <= '7' && !control[4]) {
        *gun = 0; return control[3] - '0';
    }
    if (!strncmp(control, "jvsanalog", 9) && control[9] >= '0' && control[9] <= '7' && !control[10]) {
        *gun = 2; return control[9] - '0';
    }
    return -1;
}

unsigned system12_analog_max(const char *control)
{
    int gun, index = analog_index(control, &gun);
    if (!board.active || index < 0) return 0;
    if (gun == 1 && strcmp(board.state, "ptblank2_state") && strcmp(board.state, "golgo13_state")) return 0;
    if (gun == 2 && !board.jvs_enabled) return 0;
    return gun ? 65535 : 1023;
}

int system12_set_analog(const char *control, unsigned value)
{
    int gun, index = analog_index(control, &gun);
    unsigned max = system12_analog_max(control);
    if (!max || value > max) return -1;
    if (gun == 1) inputs.gun[index] = value; else inputs.analog[index] = value;
    post_inputs();
    return 0;
}

int system12_set_gun_position(unsigned player, unsigned x, unsigned y)
{
    unsigned xmin, xmax, ymin, ymax;
    if (!board.active || player > 1 || x > 65535 || y > 65535 || !system12_analog_max("p1gunx")) return -1;
    if (!strcmp(board.state, "golgo13_state")) {
        xmin = ymin = 0; xmax = ymax = 511; y = 65535 - y;
    } else if ((!strncmp(board.profile, "ptblank2", 8) || !strcmp(board.profile, "gunbarl"))) {
        xmin = 0xdc; xmax = 0x387; ymin = 0x28; ymax = 0x11b;
    } else {
        xmin = 0xbe; xmax = 0x35f; ymin = 0x1a; ymax = 0x109;
    }
    inputs.gun[player * 2] = xmin + x * (xmax - xmin) / 65535;
    inputs.gun[player * 2 + 1] = ymin + y * (ymax - ymin) / 65535;
    post_inputs(); return 0;
}

int system12_set_input(const char *control, int pressed)
{
    unsigned i;
    if (!board.active || !control || (pressed != 0 && pressed != 1)) return -1;
    if (!strcmp(board.machine, "aplarail")) {
        if (!strcmp(control, "p1up")) return system12_set_analog("adc0", pressed ? 0x08b : 0x216);
        if (!strcmp(control, "p1down")) return system12_set_analog("adc0", pressed ? 0x397 : 0x216);
    }
    if (!strcmp(board.machine, "technodr")) {
        if (!strcmp(control, "p1left")) return system12_set_analog("jvsanalog0", pressed ? 0xa600 : 0x5e00);
        if (!strcmp(control, "p1right")) return system12_set_analog("jvsanalog0", pressed ? 0x1600 : 0x5e00);
        if (!strcmp(control, "p1up")) return system12_set_analog("jvsanalog2", pressed ? 0x4680 : 0xed80);
        if (!strcmp(control, "p1down")) return system12_set_analog("jvsanalog1", pressed ? 0x3e00 : 0x7e00);
    }
    if (!strcmp(board.machine, "kartduel")) {
        if (!strcmp(control, "p1left")) system12_set_analog("adc2", pressed ? 0xe0 : 0x200);
        if (!strcmp(control, "p1right")) system12_set_analog("adc2", pressed ? 0x320 : 0x200);
        if (!strcmp(control, "p1up")) system12_set_analog("adc1", pressed ? 0x15a : 0xad);
        if (!strcmp(control, "p1down")) system12_set_analog("adc0", pressed ? 0x15a : 0xad);
    }
    if (!strcmp(board.machine, "truckk")) {
        if (!strcmp(control, "p1left")) return system12_set_analog("adc0", pressed ? 0 : 0x200);
        if (!strcmp(control, "p1right")) return system12_set_analog("adc0", pressed ? 0x3ff : 0x200);
        if (!strcmp(control, "p1up")) system12_set_analog("adc2", pressed ? 0 : 0x200);
        if (!strcmp(control, "p1down")) system12_set_analog("adc1", pressed ? 0 : 0x200);
    }
    if (!strcmp(control, "coin1")) inputs.coin[0] = pressed;
    else if (!strcmp(control, "coin2")) inputs.coin[1] = pressed;
    else if (!strcmp(control, "test")) inputs.system = (inputs.system & ~0x80u) | ((u32)pressed << 7);
    else if (!strcmp(control, "service")) inputs.player[0] = (inputs.player[0] & ~0x40u) | ((u32)pressed << 6);
    else {
        for (i = 0; i < sizeof(player_inputs) / sizeof(player_inputs[0]); i++)
            if (!strcmp(control, player_inputs[i].name)) {
                u32 *p = &inputs.player[player_inputs[i].player];
                unsigned mask = player_inputs[i].mask;
                if (!strcmp(board.machine, "aplarail")) {
                    if (!strcmp(control, "p1start")) mask = 1;
                    if (!strcmp(control, "p1button2")) mask = 0x8000;
                }
                if (!strcmp(board.machine, "technodr") && !strcmp(control, "p1button3")) mask = 0x2000;
                if (!strcmp(board.machine, "truckk")) {
                    if (!strcmp(control, "p1start")) mask = 0x8000;
                    if (!strcmp(control, "p1button4")) mask = 0x80;
                }
                if (!strcmp(board.profile, "ujlnow")) {
                    if (!strcmp(control, "p1button4")) mask = 0x800;
                    if (!strcmp(control, "p1button5")) mask = 4;
                    if (!strcmp(control, "p1button6")) mask = 8;
                }
                if (!strcmp(board.state, "golgo13_state")) {
                    if (!strcmp(control, "p1button1")) mask = 1;
                    if (!strcmp(control, "p1button2")) mask = 0x800;
                    if (!strcmp(control, "p1button3")) mask = 2;
                }
                *p = (*p & ~mask) | (pressed ? mask : 0);
                break;
            }
        if (i == sizeof(player_inputs) / sizeof(player_inputs[0])) return -1;
    }
    post_inputs();
    return 0;
}

int system12_eeprom_export(u8 *image, size_t size)
{
    if (!board.active || !board.eeprom) return -1;
    return at28c16_get_image(board.eeprom, image, size);
}

int system12_eeprom_import(const u8 *image, size_t size)
{
    if (!board.active || !board.eeprom) return -1;
    return at28c16_import_image(board.eeprom, image, size);
}

unsigned char system12_jamma_read(unsigned offset)
{
    if (board.jvs_enabled) return 0xff;
    u32 p1 = board.jvs_player[0], p2 = board.jvs_player[1];
    u32 value;
    switch (offset & 3) {
    case 0: value = ((p2 >> 7 & 1) << 7) | ((p2 >> 15 & 1) << 6) |
            ((p2 >> 0 & 1) << 5) | ((p2 >> 1 & 1) << 4) |
            ((p2 >> 5 & 1) << 3) | ((p2 >> 4 & 1) << 2) |
            ((p2 >> 3 & 1) << 1) | ((p2 >> 2 & 1) << 0); break;
    case 1: value = ((p1 >> 7 & 1) << 7) | ((p1 >> 15 & 1) << 6) |
            ((p1 >> 0 & 1) << 5) | ((p1 >> 1 & 1) << 4) |
            ((p1 >> 5 & 1) << 3) | ((p1 >> 4 & 1) << 2) |
            ((p1 >> 3 & 1) << 1) | ((p1 >> 2 & 1) << 0); break;
    case 2: value = ((p1 >> 6 & 1) << 7) | ((board.jvs_system >> 7 & 1) << 6) |
            ((board.jvs_coin[0] & 1) << 5) | ((board.jvs_coin[1] & 1) << 4) |
            ((p1 >> 11 & 1) << 3) | ((p2 >> 11 & 1) << 2) |
            ((p1 >> 10 & 1) << 1) | ((p2 >> 10 & 1) << 0); break;
    default: value = ((p2 >> 12 & 1) << 7) | ((p2 >> 13 & 1) << 6) |
            ((p2 >> 14 & 1) << 5) | ((p1 >> 12 & 1) << 3) |
            ((p1 >> 13 & 1) << 2) | ((p1 >> 14 & 1) << 1); break;
    }
    return (u8)~value;
}

int system12_active(void) { return board.active; }

static u32 physical(u32 address)
{
    return address >= 0x80000000 && address < 0xc0000000
        ? address & 0x1fffffff : address;
}

#ifndef DRC_DISABLE
static int system12_code_span(unsigned int pc, const unsigned int **words,
    unsigned int *limit)
{
    const u32 address = physical(pc);
    if (!board.active || address < 0x1fa00000 || address >= 0x1fc00000 ||
        (pc & 3) || board.bank >= board.banked_size / WINDOW_SIZE)
        return 0;
    *words = (const unsigned int *)(board.banked + board.bank * WINDOW_SIZE +
        address - 0x1fa00000);
    *limit = pc + (0x1fc00000 - address);
    return 1;
}
#endif

/* The H8 sees big-endian 16-bit words, while the main CPU sees the same
 * shared backing as little-endian halfwords. MAME bus oracle verifies this. */
static u8 sub_read8(void *opaque, u32 address)
{
    (void)opaque;
    if (address < board.sub_size) return board.sub[address];
    if (address >= 0x080000 && address < 0x090000) {
        board.sub_reads++;
        u8 value = board.shared[(address - 0x080000) ^ 1];
        trace_shared(SYSTEM12_EDGE_H8_LOAD, address, 8, value);
        return value;
    }
    if (address >= 0x280000 && address < 0x288000) {
        const unsigned reg = (address - 0x280000) / 2;
        if (pcm_access_needs_sync(reg, 0, 0, 0))
            pcm_sync_h8();
        u16 word = c352_read(board.pcm, reg);
        return address & 1 ? word : word >> 8;
    }
    /* Default JVS reads are active-low with all controls released. */
    if (address >= 0x300000 && address < 0x300004)
        return system12_jamma_read(address - 0x300000);
    return 0xff;
}

static void sub_write8(void *opaque, u32 address, u8 value)
{
    (void)opaque;
    if (address >= 0x080000 && address < 0x090000) {
        board.shared[(address - 0x080000) ^ 1] = value;
        board.sub_writes++;
        trace_shared(SYSTEM12_EDGE_H8_STORE, address, 8, value);
    }
    else if (address >= 0x280000 && address < 0x288000) {
        unsigned shift = address & 1 ? 0 : 8;
        const unsigned reg = (address - 0x280000) / 2;
        if (pcm_access_needs_sync(reg, 1, (u16)value << shift, 0xffu << shift)) {
            pcm_sync_h8();
            c352_write(board.pcm, reg, (u16)value << shift, 0xffu << shift);
            pcm_flags_horizon();
        }
    }
}

/* Side-effect-free H8 reads for the exact idle-loop skip: code ROM, and shared
 * RAM, which the main CPU cannot change while the H8 runs. Tracing H8 loads
 * needs every access, so it disables the skip. */
static int sub_peek8(void *opaque, u32 address, u8 *value)
{
    (void)opaque;
    if (address < board.sub_size) {
        *value = board.sub[address];
        return 1;
    }
    if (address >= 0x080000 && address < 0x090000 &&
        !(edge_trace_cb && (edge_trace_shared & SYSTEM12_TRACE_SHARED_LOADS))) {
        *value = board.shared[(address - 0x080000) ^ 1];
        return 1;
    }
    return 0;
}
static void sub_account_reads(void *opaque, u32 address, u64 count)
{
    (void)opaque;
    if (address >= 0x080000 && address < 0x090000)
        board.sub_reads += count;
}
static u16 sub_read16(void *opaque, u32 address)
{
    if (address >= 0x080000 && address < 0x08ffff) {
        u16 value = (u16)board.shared[(address - 0x080000) ^ 1] << 8 |
            board.shared[((address + 1) - 0x080000) ^ 1];
        board.sub_reads += 2;
        trace_shared(SYSTEM12_EDGE_H8_LOAD, address, 16, value);
        return value;
    }
    return ((u16)sub_read8(opaque, address) << 8) | sub_read8(opaque, address + 1);
}

static void sub_write16(void *opaque, u32 address, u16 value)
{
    if (address >= 0x280000 && address < 0x288000) {
        const unsigned reg = (address - 0x280000) / 2;
        if (pcm_access_needs_sync(reg, 1, value, 0xffff)) {
            pcm_sync_h8();
            c352_write(board.pcm, reg, value, 0xffff);
            pcm_flags_horizon();
        }
    } else if (address >= 0x080000 && address < 0x08ffff) {
        board.shared[(address - 0x080000) ^ 1] = value >> 8;
        board.shared[((address + 1) - 0x080000) ^ 1] = value;
        board.sub_writes += 2;
        trace_shared(SYSTEM12_EDGE_H8_STORE, address, 16, value);
    } else {
        sub_write8(opaque, address, value >> 8);
        sub_write8(opaque, address + 1, value);
    }
}

static u8 sub_port_read(void *opaque, unsigned port)
{
    (void)opaque;
    if (port == 6 && board.jvs_enabled)
        return board.jvs.address == 0xff || (!strcmp(board.machine, "technodr") && board.jvs_printer.address == 0xff) ? 0xff : 0xfd;
    if (port == 8 && board.jvs_enabled) return 0xff;
    if (port == 8) return 0xef; /* sense None */
    if (port == 11) {
        u8 value = board.portb | 0x7f;
        trace_edge(SYSTEM12_EDGE_PORTB_INPUT, value);
        return value;
    }
    return 0xff;
}

static u16 sub_adc_read(void *opaque, unsigned channel)
{
    (void)opaque;
    if (!strcmp(board.state, "golgo13_state")) {
        if (channel == 1) return ((u32)board.gun[0] * 0x240) / 0x1ff + 0x7a;
        if (channel == 2) return ((u32)board.gun[1] * 0x1bf) / 0x1ff + 0x1f;
    }
    return channel < 8 ? board.analog[channel] : 0;
}

static void sub_port_write(void *opaque, unsigned port, u8 value, u8 ddr)
{
    (void)opaque; (void)ddr;
    if (port == 8) board.port8 = value;
    if (port == 10) board.porta = value;
    if (port == 11) board.portb = (board.portb & 0x80) | (value & 0x7f);
    rtc4543_ce_w(&board.rtc, (board.portb & 0x20) && (board.porta & 1));
    namco_settings_ce_w(&board.settings, (board.portb & 0x20) && !(board.porta & 1));
}

/* MAME wiring order: RTC sees the inverted clock before settings sees CLK. */
static void sub_serial_tx_pin(void *opaque, unsigned channel, int level)
{
    (void)opaque;
    if (channel != 1) return; /* No JVS peripheral connected. */
    namco_settings_data_w(&board.settings, level);
}
static void sub_serial_clk_pin(void *opaque, unsigned channel, int level)
{
    (void)opaque;
    if (channel != 1) return;
    rtc4543_clk_w(&board.rtc, !level);
    namco_settings_clk_w(&board.settings, level);
}
static void rtc_serial_data(void *opaque, int level)
{
    (void)opaque;
    h8_serial_rx_pin(board.h8, 1, level);
}

static void sub_serial_observer(void *opaque, unsigned channel, u8 byte)
{
    (void)opaque;
    if (channel == 0 && board.jvs_enabled) {
        int techno = !strcmp(board.machine, "technodr");
        int printer_initialized = board.jvs_printer.address != 0xff;
        board.jvs.allow_address = !techno || printer_initialized;
        board.jvs_printer.allow_address = !printer_initialized;
        s12_jvs_receive(&board.jvs, byte, board.jvs_player, board.jvs_system,
            board.analog, techno, h8_current_cycle(board.h8));
        if (techno) s12_jvs_receive(&board.jvs_printer, byte, board.jvs_player, board.jvs_system,
            board.analog, 2, h8_current_cycle(board.h8));
    }
}

static int board_run_h8_until(u64 target)
{
    if (!board.jvs_enabled) return h8_run_until(board.h8, target);
    while (h8_current_cycle(board.h8) < target) {
        S12Jvs *j = &board.jvs;
        S12Jvs *printer = &board.jvs_printer;
        if (!strcmp(board.machine, "technodr") && printer->tx_index < printer->tx_size &&
            (j->tx_index >= j->tx_size || printer->next_bit < j->next_bit)) j = printer;
        u64 next = target;
        if (j->tx_index < j->tx_size && !(board.portb & 1)) {
            u64 now = h8_current_cycle(board.h8);
            if (j->next_bit <= now) {
                j->next_bit = now;
                h8_serial_rx_pin(board.h8, 0, s12_jvs_tx_bit(j));
                continue;
            }
            if (j->next_bit < next) next = j->next_bit;
        }
        if (h8_run_until(board.h8, next)) return -1;
    }
    return 0;
}

static u8 link_read8(void *opaque, u32 a)
{
    if (a >= 0xffff10 && a <= 0xffffff) return board.link_internal[a - 0xffff10];
    (void)opaque;
    if (a < 0x10000) return board.link_shared[a ^ 1];
    if (a < 0x20000) return board.link_ram[a - 0x10000];
    if (a >= 0x200000 && a < 0x200200) return board.link_io[a - 0x200000];
    if (a >= 0x400000 && a < 0x400008) return board.link_small[a - 0x400000];
    if (a >= 0x410000 && a < 0x410010) return board.link_small[8 + a - 0x410000];
    if (a >= 0x418002 && a < 0x418008) return board.link_small[24 + a - 0x418002];
    return 0xff;
}

static void link_write8(void *opaque, u32 a, u8 value)
{
    if (a >= 0xffff10 && a <= 0xffffff) { board.link_internal[a - 0xffff10] = value; return; }
    (void)opaque;
    if (a < 0x10000) board.link_shared[a ^ 1] = value;
    else if (a < 0x20000) board.link_ram[a - 0x10000] = value;
    else if (a >= 0x200000 && a < 0x200200) board.link_io[a - 0x200000] = value;
    else if (a >= 0x400000 && a < 0x400008) board.link_small[a - 0x400000] = value;
    else if (a >= 0x410000 && a < 0x410010) board.link_small[8 + a - 0x410000] = value;
    else if (a >= 0x418002 && a < 0x418008) board.link_small[24 + a - 0x418002] = value;
}

static u64 link_clock(u64 h8_clock)
{
    return (h8_clock / 16934400) * 14745600 + (h8_clock % 16934400) * 14745600 / 16934400;
}

static void sync_link(void)
{
    if (board.link && !board.link_reset)
        h8_run_until(board.link, link_clock(h8_current_cycle(board.h8)));
#ifdef PCSX_SYSTEM12_CDXA
    if (board.cdxa) s12_cdxa_run_until(board.cdxa, link_clock(h8_current_cycle(board.h8)));
#endif
}

#ifdef PCSX_SYSTEM12_CDXA
static void cdxa_irq(void *opaque, int asserted)
{
    (void)opaque;
    if (asserted) set_event(PSXINT_IRQ10, 1);
}
#endif

/* Preserve sub-microsecond time across scheduler calls. Reset re-anchors the
 * PSX cycle counter, while the EEPROM's absolute device clock keeps advancing. */
static int eeprom_advance_cycles(u32 elapsed)
{
    u64 numerator = board.eeprom_cycle_fraction + (u64)elapsed * 1000000;
    u64 delta = numerator / PSXCLK;
    if (board.eeprom_time_us > UINT64_MAX - delta)
        return -1;
    board.eeprom_time_us += delta;
    board.eeprom_cycle_fraction = numerator % PSXCLK;
    return at28c16_advance_to_us(board.eeprom, board.eeprom_time_us);
}

unsigned long long system12_c352_access_us, system12_c352_horizon_us, system12_c352_accesses;
static void pcm_flags_horizon_body(void);
static void pcm_flags_horizon(void)
{
    const u64 t0 = PERF_NOW();
    pcm_flags_horizon_body();
    if (system12_perf_timing)
        system12_c352_horizon_us += board_now_us() - t0;
}
static void pcm_flags_horizon_body(void)
{
    const size_t stable = c352_flags_stable_frames(board.pcm);
    pcm_flags_until = stable > UINT64_MAX - board.pcm_frames
        ? UINT64_MAX : board.pcm_frames + stable;
}

static void pcm_generate_to(u64 target);
static void pcm_flush_wanted(void)
{
    if (pcm_wanted > board.pcm_frames)
        pcm_generate_to(pcm_wanted);
}

/* One monotonic stream cursor for register catch-up and board catch-up. */
static void pcm_generate_to(u64 target)
{
    while (board.pcm_frames < target) {
        int16_t samples[256 * 4];
        int16_t stereo[256 * 2];
        u64 remaining = target - board.pcm_frames;
        unsigned frames = remaining > 256 ? 256 : (unsigned)remaining, i;
        uint64_t pcm_started = pcsx_profile_begin(PCSX_PROF_PCM, 1);
        c352_generate(board.pcm, samples, frames);
        pcsx_profile_end(PCSX_PROF_PCM, pcm_started);
#ifdef PCSX_SYSTEM12_CDXA
        if (board.cdxa) for (i = 0; i < frames; i++) {
            int16_t cd[2]; unsigned channel;
            s12_cdxa_sample(board.cdxa, board.pcm_frames + i, cd);
            for (channel = 0; channel < 2; channel++) {
                int value = samples[i * 4 + channel] + cd[channel];
                samples[i * 4 + channel] = value > 32767 ? 32767 : value < -32768 ? -32768 : value;
            }
        }
#endif
        const int gain_q8 = audio_gain_q8;
        for (i = 0; i < frames; i++) {
            stereo[i * 2] = audio_gain(samples[i * 4], gain_q8);
            stereo[i * 2 + 1] = audio_gain(samples[i * 4 + 1], gain_q8);
        }
        uint64_t audio_started = pcsx_profile_begin(PCSX_PROF_BOARD_AUDIO, 1);
        board_audio_out(stereo, frames);
        pcsx_profile_end(PCSX_PROF_BOARD_AUDIO, audio_started);
        for (i = 0; i < frames * 4; i++) {
            unsigned amplitude = samples[i] < 0 ? -(int)samples[i] : samples[i];
            if (amplitude > board.pcm_peak) board.pcm_peak = amplitude;
        }
        if (audio_write_samples(samples, frames))
            SysPrintf("System12: WAV audio write failed\n");
        board.pcm_frames += frames;
    }
    pcm_flags_horizon();
}


#if !defined(__vita__)
unsigned long system12_pcm_origin_h8, system12_pcm_origin_sync;
#define PCM_ORIGIN(i) ((i) ? system12_pcm_origin_sync++ : system12_pcm_origin_h8++)
#else
#define PCM_ORIGIN(i)
#endif
static void pcm_sync_h8(void)
{
    u64 target;
    PCM_ORIGIN(0);
    if (system12_audio_clock_advance(&board.pcm_h8_clock,
            h8_current_cycle(board.h8), &target) == 0)
        pcm_generate_to(target);
}

/* Whether the stream must reach the H8's time before it accesses C352
 * register reg (writing value under mask, or reading when !write). The
 * sound program reads voice flags and rewrites unchanged values about
 * once per sample: generating one frame at a time cost Soul Calibur up to
 * 9 ms a frame. Generating changes no register but the voices' flags, and
 * those only at a wave end (c352_flags_stable_frames); the stream is the
 * same whenever it is made, as long as it is made before a register
 * changes. */
static int pcm_access_needs_sync_body(unsigned reg, int write, u16 value, u16 mask);
static int pcm_access_needs_sync(unsigned reg, int write, u16 value, u16 mask)
{
    const u64 t0 = PERF_NOW();
    const int result = pcm_access_needs_sync_body(reg, write, value, mask);
    system12_c352_accesses++;
    if (system12_perf_timing)
        system12_c352_access_us += board_now_us() - t0;
    return result;
}
static int pcm_access_needs_sync_body(unsigned reg, int write, u16 value, u16 mask)
{
    const int flags = reg < 0x100 && reg % 8 == 3;
    u64 target;
    if (write) {
        const u16 old = c352_read(board.pcm, reg);
        if (reg == 0x202 || ((old & ~mask) | (value & mask)) != old)
            return 1;
    }
    if (!flags)
        return 0;
    if (system12_audio_clock_advance(&board.pcm_h8_clock, h8_current_cycle(board.h8), &target))
        return 0;
    return !pcm_flags_until || target > pcm_flags_until;
}

/* H8 instructions run and its shared RAM accesses, totals; vblanks seen. */
unsigned long long system12_h8_instructions, system12_h8_shared, system12_h8_frames;
/* At the first counted vblank: the H8 perf line's starting point. */
static unsigned long long perf_base_us, perf_base_instructions, perf_base_board_us;

static void board_exec_kind(const board_cmd *c);
/* All of the board's work per command (H8, C352, peripherals): the
 * harness's perf line, the Vita's b_cpu counterpart. */
unsigned long long system12_board_us;
static void board_exec(const board_cmd *c)
{
    const u64 t0 = PERF_NOW();
    board_exec_kind(c);
    if (system12_perf_timing)
        system12_board_us += board_now_us() - t0;
}

static void board_exec_kind(const board_cmd *c)
{
    uint64_t started;
    u64 t0;
    switch (c->kind) {
    case BOARD_CMD_SYNC:
        if (c->rtc_seconds) rtc4543_advance_seconds(&board.rtc, c->rtc_seconds);
        started = pcsx_profile_begin(PCSX_PROF_H8, 1);
        t0 = PERF_NOW();
        if (!c->run_h8) ;
        else if (c->run_until) board_run_h8_until(c->h8_target);
        else h8_run_cycles(board.h8, c->h8_cycles);
        sync_link();
        pcsx_profile_end(PCSX_PROF_H8, started);
        if (system12_perf_timing) {
            system12_h8_us += board_now_us() - t0;
            t0 = board_now_us();
        }
        PCM_ORIGIN(1);
        /* The stream is the same whenever it is made, as long as it is made
         * before a register changes (H8 accesses that change one generate
         * first): one frame per command took the C352's slow path. */
        if (c->pcm_target > pcm_wanted)
            pcm_wanted = c->pcm_target;
        if (pcm_wanted >= board.pcm_frames + pcm_batch())
            pcm_generate_to(pcm_wanted);
        if (system12_perf_timing)
            system12_pcm_us += board_now_us() - t0;
        break;
    case BOARD_CMD_VBLANK:
        pcm_flush_wanted();
        if (c->run_h8) {
            started = pcsx_profile_begin(PCSX_PROF_H8, 1);
            t0 = PERF_NOW();
            if (board_run_h8_until(c->h8_target))
                SysPrintf("System12: invalid H8 screen deadline\n");
            pcsx_profile_end(PCSX_PROF_H8, started);
            if (system12_perf_timing)
                system12_h8_us += board_now_us() - t0;
            h8_set_irq(board.h8, 1, c->level);
            h8_adc_trigger(board.h8, c->level);
            board.portb = (board.portb & 0x7f) | (c->level ? 0x80 : 0);
        } else {
            board.portb = (board.portb & 0x7f) | (c->level ? 0x80 : 0);
            h8_set_irq(board.h8, 1, c->level);
            h8_adc_trigger(board.h8, c->level);
        }
        sync_link();
        if (c->level) {
            H8Status status;
            h8_get_status(board.h8, &status);
            system12_h8_instructions = status.instructions - status.skipped;
            system12_h8_shared = board.shared_reads + board.shared_writes;
            if (!system12_h8_frames++) {
                perf_base_us = system12_h8_us;
                perf_base_instructions = system12_h8_instructions;
                perf_base_board_us = system12_board_us;
            }
        }
        trace_edge(SYSTEM12_EDGE_VBLANK, c->level);
        break;
    case BOARD_CMD_INPUT:
        board.jvs_player[0] = c->jvs_player[0]; board.jvs_player[1] = c->jvs_player[1];
        board.jvs_system = c->jvs_system;
        board.jvs_coin[0] = c->jvs_coin[0]; board.jvs_coin[1] = c->jvs_coin[1];
        memcpy(board.gun, c->gun, sizeof(board.gun));
        memcpy(board.analog, c->analog, sizeof(board.analog));
        if (board.jvs_enabled) {
            s12_jvs_coin(&board.jvs, 0, c->jvs_coin[0] & 1);
            s12_jvs_coin(&board.jvs, 1, c->jvs_coin[1] & 1);
        }
        break;
    }
}

#ifdef SYSTEM12_BOARD_THREAD
/* The board thread lags the main CPU by at most BOARD_QUEUE commands (about
 * a dozen frames). Its audio goes through a ring that the main thread
 * delivers, because libretro audio must be sent from the retro_run thread. */
#define BOARD_QUEUE 64
#define BOARD_AUDIO_FRAMES 65536
static struct {
    sthread_t *thread;
    slock_t *lock;
    scond_t *work, *done_cond;
    board_cmd queue[BOARD_QUEUE];
    unsigned posted;  /* main thread; updated under lock */
    atomic_uint done; /* board thread */
    int quit, active;
    atomic_uint audio_head, audio_tail; /* frames: board writes head, main tail */
    int16_t audio[BOARD_AUDIO_FRAMES * 2];
} bt;

#ifdef SYSTEM12_BOARD_THREAD_JITTER
/* Test builds: random delays on both sides must not change any output. */
#include <unistd.h>
static unsigned jitter_main_seed, jitter_board_seed;
static void board_jitter(unsigned *seed)
{
    const char *env = getenv("S12_BOARD_JITTER");
    unsigned r;
    if (!env) return;
    if (!jitter_main_seed) {
        jitter_main_seed = 2 * (unsigned)atoi(env) + 1;
        jitter_board_seed = 2 * (unsigned)atoi(env) + 2;
    }
    r = rand_r(seed);
    if ((r & 3) == 0) usleep(r % 3000);
}
#define BOARD_JITTER(seed) board_jitter(&(seed))
#else
#define BOARD_JITTER(seed)
#endif

static int board_audio_pending(void)
{
    return atomic_load_explicit(&bt.audio_head, memory_order_acquire) !=
        atomic_load_explicit(&bt.audio_tail, memory_order_relaxed);
}

static void board_audio_pump(void)
{
    for (;;) {
        unsigned head = atomic_load_explicit(&bt.audio_head, memory_order_acquire);
        unsigned tail = atomic_load_explicit(&bt.audio_tail, memory_order_relaxed);
        unsigned n = head - tail, at = tail % BOARD_AUDIO_FRAMES;
        if (!n) break;
        if (n > BOARD_AUDIO_FRAMES - at) n = BOARD_AUDIO_FRAMES - at;
        if (board.audio_cb)
            board.audio_cb(board.audio_opaque, bt.audio + at * 2, n);
        atomic_store_explicit(&bt.audio_tail, tail + n, memory_order_release);
    }
}

static void board_audio_push(const int16_t *stereo, unsigned frames)
{
    while (frames) {
        unsigned head = atomic_load_explicit(&bt.audio_head, memory_order_relaxed);
        unsigned tail = atomic_load_explicit(&bt.audio_tail, memory_order_acquire);
        unsigned space = BOARD_AUDIO_FRAMES - (head - tail);
        unsigned at = head % BOARD_AUDIO_FRAMES, n = frames;
        if (!space) {
            /* Wake a waiting main thread so that it delivers audio. */
            slock_lock(bt.lock);
            scond_signal(bt.done_cond);
            slock_unlock(bt.lock);
            retro_sleep(1);
            continue;
        }
        if (n > space) n = space;
        if (n > BOARD_AUDIO_FRAMES - at) n = BOARD_AUDIO_FRAMES - at;
        memcpy(bt.audio + at * 2, stereo, (size_t)n * 4);
        atomic_store_explicit(&bt.audio_head, head + n, memory_order_release);
        stereo += n * 2;
        frames -= n;
    }
}

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
/* The board thread's own time (H8 and C352), for the progress lines, and
 * the CPU time the kernel gave it (runClocks): busy minus CPU is the time
 * other threads held its core. */
unsigned long long system12_board_busy_us, system12_board_cpu_us;
#endif

static STRHEAD_RET_TYPE board_thread(void *unused)
{
    (void)unused;
    slock_lock(bt.lock);
    for (;;) {
        unsigned next = atomic_load_explicit(&bt.done, memory_order_relaxed);
        if (next == bt.posted) {
            if (bt.quit) break;
            scond_wait(bt.work, bt.lock);
            continue;
        }
        slock_unlock(bt.lock);
        BOARD_JITTER(jitter_board_seed);
#ifdef __vita__
        const SceUInt64 busy_start = system12_perf_timing ? sceKernelGetProcessTimeWide() : 0;
#endif
        board_exec(&bt.queue[next % BOARD_QUEUE]);
#ifdef __vita__
        if (system12_perf_timing)
            system12_board_busy_us += sceKernelGetProcessTimeWide() - busy_start;
        {
            /* The kernel's count of this thread's CPU time: cheap enough to
             * read every 1024 commands in any build. */
            static unsigned commands;
            if (!(++commands & 1023)) {
                SceKernelThreadInfo info;
                memset(&info, 0, sizeof(info));
                info.size = sizeof(info);
                if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0)
                    system12_board_cpu_us = info.runClocks;
            }
        }
#endif
        slock_lock(bt.lock);
        atomic_store_explicit(&bt.done, next + 1, memory_order_release);
        scond_signal(bt.done_cond);
    }
    slock_unlock(bt.lock);
    STRHEAD_RETURN();
}

/* Wait until the board has run the commands before `target` (modulo 2^32),
 * delivering board audio meanwhile. */
#ifdef __vita__
/* Microseconds and count of the main thread's waits for the board thread,
 * for the Vita frontend's progress lines. */
unsigned long long system12_board_wait_us;
unsigned system12_board_wait_n;
#endif

static void board_wait(unsigned target)
{
    if ((int)(atomic_load_explicit(&bt.done, memory_order_acquire) - target) >= 0)
        return;
#ifdef __vita__
    const SceUInt64 wait_start = sceKernelGetProcessTimeWide();
#endif
    slock_lock(bt.lock);
    while ((int)(atomic_load_explicit(&bt.done, memory_order_acquire) - target) < 0) {
        if (board_audio_pending()) {
            slock_unlock(bt.lock);
            board_audio_pump();
            slock_lock(bt.lock);
            continue;
        }
        scond_wait(bt.done_cond, bt.lock);
    }
    slock_unlock(bt.lock);
#ifdef __vita__
    system12_board_wait_us += sceKernelGetProcessTimeWide() - wait_start;
    system12_board_wait_n++;
#endif
}
#endif

static void board_post(const board_cmd *c)
{
#ifdef SYSTEM12_BOARD_THREAD
    if (bt.active) {
        BOARD_JITTER(jitter_main_seed);
        board_wait(bt.posted - BOARD_QUEUE + 1);
        bt.queue[bt.posted % BOARD_QUEUE] = *c;
        slock_lock(bt.lock);
        bt.posted++;
        scond_signal(bt.work);
        slock_unlock(bt.lock);
        return;
    }
#endif
    board_exec(c);
}

static void board_drain(void)
{
#ifdef SYSTEM12_BOARD_THREAD
    if (bt.active) {
        BOARD_JITTER(jitter_main_seed);
        board_wait(bt.posted);
    }
#endif
}

static void board_audio_out(const int16_t *stereo, unsigned frames)
{
#ifdef SYSTEM12_BOARD_THREAD
    if (bt.active) {
        if (board.audio_cb) board_audio_push(stereo, frames);
        return;
    }
#endif
    if (board.audio_cb)
        board.audio_cb(board.audio_opaque, stereo, frames);
}

/* ---- savestates ---- */

#define S12_STATE_MAGIC 0x53323153u /* "S12S" */
#define S12_STATE_VERSION 3u

size_t system12_freeze_size(void)
{
    return 5 * sizeof(u32) + sizeof(board) + sizeof(AT28C16State) +
        h8_state_size() * (board.link ? 2 : 1) + c352_state_size()
#ifdef PCSX_SYSTEM12_CDXA
        + (board.cdxa ? s12_cdxa_state_size() : 0)
#endif
        ;
}

/* The board struct is saved whole; a load keeps what belongs to this run
 * rather than to the game's state: ROMs, devices, callbacks, outputs and
 * the loaded set's configuration. */
static void restore_live_fields(const __typeof__(board) *live)
{
    board.main = live->main; board.banked = live->banked; board.sub = live->sub; board.wave = live->wave;
    board.main_size = live->main_size; board.banked_size = live->banked_size;
    board.sub_size = live->sub_size; board.wave_size = live->wave_size; board.ram_size = live->ram_size;
    board.eeprom = live->eeprom; board.h8 = live->h8; board.pcm = live->pcm;
    board.link = live->link;
    board.cdxa = live->cdxa; board.disk_path = live->disk_path;
    board.rtc.data_cb = live->rtc.data_cb; board.rtc.data_opaque = live->rtc.data_opaque;
    board.settings.frame_cb = live->settings.frame_cb; board.settings.frame_opaque = live->settings.frame_opaque;
    board.audio_file = live->audio_file; board.audio_cb = live->audio_cb; board.audio_opaque = live->audio_opaque;
    board.audio_bytes = live->audio_bytes; board.audio_error = live->audio_error;
    board.profile = live->profile; board.machine = live->machine; board.state = live->state;
    board.unsupported = live->unsupported; board.boot_hack = live->boot_hack; board.alt_bank = live->alt_bank;
    board.tektagt_dma = live->tektagt_dma; board.has_tektagt_dma = live->has_tektagt_dma;
    board.profile_owned = live->profile_owned; board.active = live->active;
}

int system12_freeze(void *f, int mode)
{
    u32 header[5] = { S12_STATE_MAGIC, S12_STATE_VERSION, (u32)sizeof(board), (u32)h8_state_size(),
        (u32)c352_state_size() };
    AT28C16State eeprom;
    void *h8 = malloc(h8_state_size()), *pcm = malloc(c352_state_size());
    void *link = board.link ? malloc(h8_state_size()) : NULL;
#ifdef PCSX_SYSTEM12_CDXA
    void *cdxa = board.cdxa ? malloc(s12_cdxa_state_size()) : NULL;
    if (board.cdxa && !cdxa) { free(h8); free(pcm); free(link); return -1; }
#endif
    int result = -1;

    if (!board.active || !h8 || !pcm || (board.link && !link))
        goto done;
    /* The board thread finishes what it was given: its devices are idle. */
    board_drain();
    pcm_flush_wanted();
    if (mode) {
        if (at28c16_get_state(board.eeprom, &eeprom))
            goto done;
        h8_get_state(board.h8, h8);
        c352_get_state(board.pcm, pcm);
        SaveFuncs.write(f, header, sizeof(header));
        SaveFuncs.write(f, &board, sizeof(board));
        SaveFuncs.write(f, &eeprom, sizeof(eeprom));
        SaveFuncs.write(f, h8, h8_state_size());
        SaveFuncs.write(f, pcm, c352_state_size());
        if (board.link) {
            h8_get_state(board.link, link);
            SaveFuncs.write(f, link, h8_state_size());
        }
#ifdef PCSX_SYSTEM12_CDXA
        if (board.cdxa) { s12_cdxa_get_state(board.cdxa, cdxa); SaveFuncs.write(f, cdxa, s12_cdxa_state_size()); }
#endif
        result = 0;
    } else {
        u32 saved[5];
        __typeof__(board) live = board;
        if (SaveFuncs.read(f, saved, sizeof(saved)) != (int)sizeof(saved) || memcmp(saved, header, sizeof(header))) {
            SysPrintf("System12: the savestate is from another build or not a System 12 one\n");
            goto done;
        }
        if (SaveFuncs.read(f, &board, sizeof(board)) != (int)sizeof(board) ||
            SaveFuncs.read(f, &eeprom, sizeof(eeprom)) != (int)sizeof(eeprom) ||
            SaveFuncs.read(f, h8, h8_state_size()) != (int)h8_state_size() ||
            SaveFuncs.read(f, pcm, c352_state_size()) != (int)c352_state_size() ||
            (live.link && SaveFuncs.read(f, link, h8_state_size()) != (int)h8_state_size())
#ifdef PCSX_SYSTEM12_CDXA
            || (live.cdxa && SaveFuncs.read(f, cdxa, s12_cdxa_state_size()) != (int)s12_cdxa_state_size())
#endif
            ) {
            board = live;
            SysPrintf("System12: the savestate is truncated\n");
            goto done;
        }
        restore_live_fields(&live);
        at28c16_set_state(board.eeprom, &eeprom);
        h8_set_state(board.h8, h8);
        c352_set_state(board.pcm, pcm);
        if (board.link) h8_set_state(board.link, link);
#ifdef PCSX_SYSTEM12_CDXA
        if (board.cdxa) s12_cdxa_set_state(board.cdxa, cdxa);
#endif
        pcm_flags_until = 0;
        pcm_wanted = board.pcm_frames;
#ifdef SYSTEM12_BOARD_THREAD
        /* Sound the board made before the load is not this game's any more. */
        if (bt.active)
            atomic_store(&bt.audio_tail, atomic_load(&bt.audio_head));
#endif
        result = 0;
    }
done:
    free(h8);
    free(pcm);
    free(link);
#ifdef PCSX_SYSTEM12_CDXA
    free(cdxa);
#endif
    return result;
}

void system12_audio_pump(void)
{
#ifdef SYSTEM12_BOARD_THREAD
    if (bt.active) board_audio_pump();
#endif
}

int system12_board_thread_active(void)
{
#ifdef SYSTEM12_BOARD_THREAD
    return bt.active;
#else
    return 0;
#endif
}

int system12_board_thread_start(void)
{
#ifdef PCSX_SYSTEM12_CDXA
    /* CDXA IRQ10 delivery touches the live main CPU event queue. */
    if (board.cdxa) return -1;
#endif
#ifdef SYSTEM12_BOARD_THREAD
    if (bt.active) return 0;
    /* Traces and MAME timeslices read board state from the main thread. */
    if (!board.active || edge_trace_cb || bus_trace_cb || board.slice_open)
        return -1;
    bt.lock = slock_new();
    bt.work = scond_new();
    bt.done_cond = scond_new();
    if (bt.lock && bt.work && bt.done_cond) {
        bt.posted = 0;
        atomic_store(&bt.done, 0);
        atomic_store(&bt.audio_head, 0);
        atomic_store(&bt.audio_tail, 0);
        bt.quit = 0;
        bt.active = 1;
        bt.thread = pcsxr_sthread_create(board_thread, PCSXRT_BOARD);
        if (bt.thread) {
            SysPrintf("System12: board thread started\n");
            return 0;
        }
        bt.active = 0;
    }
    if (bt.done_cond) scond_free(bt.done_cond);
    if (bt.work) scond_free(bt.work);
    if (bt.lock) slock_free(bt.lock);
    bt.done_cond = bt.work = NULL;
    bt.lock = NULL;
#endif
    return -1;
}

void system12_board_thread_stop(void)
{
#ifdef SYSTEM12_BOARD_THREAD
    if (!bt.active) return;
    board_drain();
    slock_lock(bt.lock);
    bt.quit = 1;
    scond_signal(bt.work);
    slock_unlock(bt.lock);
    sthread_join(bt.thread);
    bt.thread = NULL;
    board_audio_pump();
    bt.active = 0;
    scond_free(bt.done_cond);
    scond_free(bt.work);
    slock_free(bt.lock);
    bt.done_cond = bt.work = NULL;
    bt.lock = NULL;
    SysPrintf("System12: board thread stopped\n");
#endif
}

static void sync_devices(u32 psx_cycle, int force, int run_h8)
{
    u32 elapsed;
    if (!board.active || !board.h8) return;
    elapsed = psx_cycle - board.last_cycle;
    if (!elapsed || (!force && elapsed < 256)) return;
    board.last_cycle = psx_cycle;
    if (eeprom_advance_cycles(elapsed) && !board.eeprom_clock_error) {
        board.eeprom_clock_error = 1;
        SysPrintf("System12: EEPROM clock overflow or rewind\n");
    }
    board_cmd c = {BOARD_CMD_SYNC};
    board.rtc_fraction += elapsed;
    if (board.rtc_fraction >= PSXCLK) {
        c.rtc_seconds = (u32)(board.rtc_fraction / PSXCLK);
        board.rtc_fraction %= PSXCLK;
    }
    board.h8_fraction += (u64)elapsed * 16934400;
    u32 requested = board.h8_fraction / PSXCLK;
    board.h8_requested += requested;
    c.run_h8 = run_h8 != 0;
    c.run_until = psxArcadeSourceTimingActive() != 0;
    c.h8_target = board.h8_requested;
    c.h8_cycles = requested;
    board.h8_fraction %= PSXCLK;
    board.pcm_fraction += (u64)elapsed * c352_sample_rate(board.pcm);
    board.pcm_due += board.pcm_fraction / PSXCLK;
    board.pcm_fraction %= PSXCLK;
    /* MAME's stream includes the sample at the current nonzero time. */
    c.pcm_target = board.pcm_due + 1;
    board_post(&c);
}

static void sync_to(u32 psx_cycle, int force)
{
    sync_devices(psx_cycle, force, 1);
}

void system12_sync(u32 psx_cycle)
{
    /* Deliver any source screen deadlines before the H8 passes them, also on
     * frontend block boundaries that did not process an RCNT event yet. */
    psxRcntSourceScreenUpdate();
    sync_to(psx_cycle, 0);
}

/* Before the main CPU counter is cleared, retain even a sub-quantum interval.
 * This runs at the same quiescent CPU boundary as psxReset, never from a sink. */
void system12_pre_reset(void)
{
    if (board.booted) {
        psxRcntSourceScreenUpdate();
        sync_to(psxRegs.cycle, 1);
    }
}

#ifdef PCSX_SYSTEM12_PROFILE
void system12_profile_counters(uint64_t *cycles, uint64_t *samples)
{
    H8Status state;
#ifdef SYSTEM12_BOARD_THREAD
    if (bt.active) {
        /* Requested totals: reading the board would serialize every frame. */
        *cycles = board.h8_requested;
        *samples = board.pcm_due - board.pcm_epoch_frames;
        return;
    }
#endif
    h8_get_status(board.h8, &state);
    *cycles = state.cycles;
    *samples = board.pcm_frames - board.pcm_epoch_frames;
}
#endif

void system12_vblank(int level)
{
    board_cmd c = {BOARD_CMD_VBLANK};
    if (!board.active || !board.h8) return;
    system12_sync(psxRegs.cycle);
    c.level = level != 0;
    board_post(&c);
}

/* CPU execution before a global timer is bounded by floor, not ceil.
 * Preserve the H8's truncated attosecond clock instead of rounding the
 * edge through a main-CPU instruction/block endpoint. */
static u64 h8_cycle_at(const psx_screen_time *time)
{
    const u64 period = UINT64_C(1000000000000000000) / 16934400;
    const u64 remainder = UINT64_C(1000000000000000000) - period * 16934400;
    u64 seconds = (u64)time->seconds;
    return seconds * 16934400 + (time->attoseconds + seconds * remainder) / period;
}

void system12_vblank_at(int level, const psx_screen_time *time)
{
    u64 target;
    if (!board.active || !board.h8 || !time || time->seconds < 0) return;
    target = h8_cycle_at(time);
    board_cmd c = {BOARD_CMD_VBLANK};
    c.run_h8 = 1;
    c.h8_target = target;
    c.level = level != 0;
    board_post(&c);
}

/* MAME device_scheduler timeslices (source instruction step profile only).
 * Each slice targets min(base + quantum, next global timer). The main CPU runs
 * first; if it stops short, the target moves up to its local time (never before
 * the base), then the H8 and board devices catch up to that target. */
#define SLICE_QUANTUM_AS UINT64_C(16666666666666666) /* attotime::from_hz(60) */
#define SLICE_MAIN_CYCLE_AS UINT64_C(40000000000)    /* 25 MHz MAME PSX cycle */

static int slice_time_cmp(psx_screen_time a, psx_screen_time b)
{
    if (a.seconds != b.seconds) return a.seconds < b.seconds ? -1 : 1;
    return a.attoseconds < b.attoseconds ? -1 : a.attoseconds > b.attoseconds;
}

static void slice_bound(psx_screen_time *target, psx_screen_time when)
{
    if (slice_time_cmp(when, board.slice_base) > 0 && slice_time_cmp(when, *target) < 0)
        *target = when;
}

u32 system12_slice_begin(void)
{
    psx_screen_time now, when, target;
    H8SyncTimer timer;
    unsigned channel;
    u64 delta;
    if (!board.active || !board.h8) return 0;
    now = psxRcntSourceTime();
    if (!board.slice_open) {
        board.slice_base = now;
        board.slice_open = 1;
    }
    target = board.slice_base;
    target.attoseconds += SLICE_QUANTUM_AS;
    if (target.attoseconds >= PSX_SCREEN_ATTOSECONDS_PER_SECOND) {
        target.attoseconds -= PSX_SCREEN_ATTOSECONDS_PER_SECOND;
        target.seconds++;
    }
    if (psxRcntSourceScreenNext(&when)) slice_bound(&target, when);
    if (h8_peek_next_sync_timer(board.h8, &channel, &timer) == 0) {
        when.seconds = (int64_t)timer.expires.seconds;
        when.attoseconds = timer.expires.attoseconds;
        slice_bound(&target, when);
    }
    board.slice_target = target;
    board.slice_ran = 0;
    if (slice_time_cmp(target, now) <= 0) return 0;
    /* Below one second apart: the target is at most one quantum past base. */
    delta = (u64)(target.seconds - now.seconds) * PSX_SCREEN_ATTOSECONDS_PER_SECOND +
        target.attoseconds - now.attoseconds;
    board.slice_ran = delta >= SLICE_MAIN_CYCLE_AS;
    return (u32)(delta / SLICE_MAIN_CYCLE_AS);
}

void system12_slice_end(void)
{
    psx_screen_time now, target, when;
    H8SyncTimer timer;
    unsigned channel;
    u64 h8_target;
    if (!board.active || !board.h8 || !board.slice_open) return;
    now = psxRcntSourceTime();
    target = board.slice_target;
    if (board.slice_ran && slice_time_cmp(now, target) < 0)
        target = slice_time_cmp(now, board.slice_base) > 0 ? now : board.slice_base;
    h8_target = h8_cycle_at(&target);
    if (h8_target > h8_current_cycle(board.h8)) {
        uint64_t started = pcsx_profile_begin(PCSX_PROF_H8, 1);
        board_run_h8_until(h8_target);
        pcsx_profile_end(PCSX_PROF_H8, started);
    }
    sync_devices(psxRegs.cycle, 1, 0);
    board.slice_base = target;
    /* SCI sync timer callbacks are empty; firing only retires the boundary. */
    while (h8_peek_next_sync_timer(board.h8, &channel, &timer) == 0) {
        when.seconds = (int64_t)timer.expires.seconds;
        when.attoseconds = timer.expires.attoseconds;
        if (slice_time_cmp(when, board.slice_base) > 0 ||
            h8_fire_sync_timer(board.h8, channel, timer.generation) <= 0)
            break;
    }
}

static int load_region(const char *directory, const char *name, size_t size, u8 **out)
{
    char path[4096];
    FILE *f;
    u8 *data;
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= sizeof(path))
        return -1;
    f = fopen(path, "rb");
    if (!f) {
        SysPrintf("System12: cannot open %s\n", path);
        return -1;
    }
    data = malloc(size);
    if (!data || fread(data, 1, size, f) != size || fgetc(f) != EOF || ferror(f)) {
        SysPrintf("System12: %s must contain exactly %zu bytes\n", path, size);
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = data;
    return 0;
}

static int parse_decimal(const char *value, size_t *result)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)value;
    if (!*p) return -1;
    while (*p) {
        if (*p < '0' || *p > '9' || n > (SIZE_MAX - (*p - '0')) / 10) return -1;
        n = n * 10 + (*p++ - '0');
    }
    *result = n;
    return 0;
}

/* Checks a board description against the boards this adapter emulates and
 * makes it the active profile (the strings are copied). */
static int apply_desc(const system12_desc *d)
{
    if (d->default_eeprom_size) {
        if (!d->default_eeprom || (d->default_eeprom_size != 2048 && d->default_eeprom_size != 2080)) return -1;
        memcpy(board.default_eeprom, d->default_eeprom, d->default_eeprom_size);
        board.default_eeprom_size = d->default_eeprom_size;
    }
    if ((d->ram_size != 0x400000 && d->ram_size != 0x1000000) || d->main_size != DEFAULT_MAIN_SIZE ||
        d->bank_size < 0x400000 || d->bank_size > 0x4000000 || (d->bank_size % WINDOW_SIZE) ||
        d->sub_size != DEFAULT_SUB_SIZE || d->wave_size == 0 || d->wave_size > 0x1000000)
        return -1;
    if (!d->mame_set || !d->mame_set[0] || !d->machine || !d->machine[0] ||
        !d->state || !d->state[0] || !d->unsupported || !d->unsupported[0])
        return -1;
    board.ram_size = d->ram_size; board.main_size = d->main_size;
    board.banked_size = d->bank_size; board.sub_size = d->sub_size; board.wave_size = d->wave_size;
    board.profile_owned = 1;
    board.profile = strdup(d->mame_set); board.machine = strdup(d->machine);
    board.state = strdup(d->state); board.unsupported = strdup(d->unsupported);
    if (!board.profile || !board.machine || !board.state || !board.unsupported) return -1;
    board.boot_hack = d->boot_hack != 0;
    board.alt_bank = d->alt_bank != 0;
    board.tektagt_dma = d->tektagt_dma != 0;
    if (d->disk_path) { board.disk_path = strdup(d->disk_path); if (!board.disk_path) return -1; }
    if (strcmp(board.unsupported, "none")) {
        SysPrintf("System12: %s requires unimplemented board device(s): %s\n",
            board.profile, board.unsupported);
        return -2;
    }
    if ((strcmp(board.machine, "coh700") && strcmp(board.machine, "coh700b") &&
         strcmp(board.machine, "coh716") && strcmp(board.machine, "golgo13") &&
         strcmp(board.machine, "aplarail") && strcmp(board.machine, "technodr") && strcmp(board.machine, "kartduel") &&
         strcmp(board.machine, "cdxa_pcb") && strcmp(board.machine, "truckk")) ||
        (strcmp(board.state, "namcos12_state") && strcmp(board.state, "namcos12_altbank_state") &&
         strcmp(board.state, "tektagt_state") && strcmp(board.state, "ptblank2_state") &&
         strcmp(board.state, "golgo13_state") && strcmp(board.state, "technodr_state") && strcmp(board.state, "kartduel_state") &&
         strcmp(board.state, "namcos12_cdxa_state") && strcmp(board.state, "truckk_state"))) {
        SysPrintf("System12: profile %s/%s requires an unimplemented game-specific board state\n",
            board.machine, board.state);
        return -2;
    }
    if (board.alt_bank != (!strcmp(board.state, "namcos12_altbank_state") || !strcmp(board.state, "golgo13_state") ||
         !strcmp(board.state, "namcos12_cdxa_state") || !strcmp(board.state, "truckk_state")) ||
        board.tektagt_dma != !strcmp(board.state, "tektagt_state") ||
        board.boot_hack != (!strcmp(board.machine, "coh700b") || !strcmp(board.machine, "coh716") || !strcmp(board.machine, "golgo13") ||
         !strcmp(board.machine, "kartduel") || !strcmp(board.machine, "cdxa_pcb") || !strcmp(board.machine, "truckk")) ||
        board.ram_size != (!strcmp(board.machine, "coh716") ? 0x1000000 : 0x400000))
        return -1;
#ifndef PCSX_SYSTEM12_CDXA
    if (!strcmp(board.machine, "cdxa_pcb") || !strcmp(board.machine, "truckk")) {
        SysPrintf("System12: this build does not include the CDXA board\n"); return -2;
    }
#endif
    return 0;
}

/* Reads <directory>/board.cfg into *d; values[] holds its strings. */
static int load_profile(const char *directory, system12_desc *d, char values[13][256])
{
    static const char *const keys[] = {"schema", "mame_set", "machine", "state",
        "ram_size", "main_size", "bank_size", "sub_size", "wave_size",
        "boot_hack", "alt_bank", "tektagt_dma", "unsupported"};
    char path[4096], line[512], *eq;
    unsigned seen = 0, i;
    FILE *f;
    if (snprintf(path, sizeof(path), "%s/board.cfg", directory) >= (int)sizeof(path)) return -1;
    f = fopen(path, "rb");
    if (!f) {
        if (errno != ENOENT) return -1;
        /* Original Soul Calibur harness regions predate board.cfg. */
        d->mame_set = "coh700"; d->machine = "coh700"; d->state = "namcos12_state";
        d->unsupported = "none";
        d->ram_size = DEFAULT_RAM_SIZE; d->main_size = DEFAULT_MAIN_SIZE;
        d->bank_size = DEFAULT_BANK_SIZE; d->sub_size = DEFAULT_SUB_SIZE;
        d->wave_size = DEFAULT_WAVE_SIZE;
        d->boot_hack = d->alt_bank = d->tektagt_dma = 0;
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        char *key = line, *value;
        size_t length = strlen(line);
        if (length && line[length - 1] == '\n') line[--length] = 0;
        if (!length || line[0] == '#') continue;
        eq = strchr(line, '=');
        if (!eq || eq == line || !eq[1]) { fclose(f); return -1; }
        *eq = 0; value = eq + 1;
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (!strcmp(key, keys[i])) break;
        if (i == sizeof(keys) / sizeof(keys[0]) || (seen & (1u << i)) || strlen(value) >= 256) {
            fclose(f); return -1;
        }
        strcpy(values[i], value); seen |= 1u << i;
    }
    if (ferror(f)) { fclose(f); return -1; }
    fclose(f);
    if (seen != (1u << (sizeof(keys) / sizeof(keys[0]))) - 1 || strcmp(values[0], "1")) return -1;
    {
        size_t sizes[5];
        for (i = 0; i < 5; i++) if (parse_decimal(values[4 + i], &sizes[i])) return -1;
        if (sizes[0] > 0xffffffffu) return -1;
        d->ram_size = (u32)sizes[0]; d->main_size = sizes[1]; d->bank_size = sizes[2];
        d->sub_size = sizes[3]; d->wave_size = sizes[4];
    }
    for (i = 9; i < 12; i++) if (strcmp(values[i], "0") && strcmp(values[i], "1")) return -1;
    d->mame_set = values[1]; d->machine = values[2]; d->state = values[3];
    d->unsupported = values[12];
    d->boot_hack = values[9][0] == '1';
    d->alt_bank = values[10][0] == '1';
    d->tektagt_dma = values[11][0] == '1';
    return 0;
}

/* Creates the board devices once the profile and regions are in place. */
static int start_board(void)
{
    H8Config h8config = {0};
    board.pcm = c352_create(board.wave, board.wave_size, 25401600, 288);
    board.eeprom = at28c16_create(board.default_eeprom_size ? board.default_eeprom : NULL, board.default_eeprom_size);
    h8config.read8 = sub_read8; h8config.write8 = sub_write8;
    h8config.read16 = sub_read16; h8config.write16 = sub_write16;
    h8config.serial_tx_pin = sub_serial_tx_pin;
    h8config.serial_clk_pin = sub_serial_clk_pin;
    h8config.serial_external_period_as[0] = UINT64_C(542534722222);
    h8config.port_read = sub_port_read; h8config.port_write = sub_port_write;
    h8config.adc_read = sub_adc_read;
    h8config.serial_tx_observer = sub_serial_observer;
    h8config.peek8 = sub_peek8; h8config.account_reads = sub_account_reads;
    h8config.rom = board.sub; h8config.rom_size = (uint32_t)board.sub_size;
    board.h8 = h8_create(&h8config);
    if (!board.pcm || !board.h8 || !board.eeprom) return -1;
#ifdef PCSX_SYSTEM12_CDXA
    if (!strcmp(board.machine, "cdxa_pcb") || !strcmp(board.machine, "truckk")) {
        char error[256] = {0};
        const char *sha1 = !strcmp(board.profile, "truckk") ? "6b7c3686b22a508c44f67295b188504b757dd482" :
            "676f2c530f8d422ab9895ea04a43b2e9272fb8f8";
        if (!board.disk_path) { SysPrintf("System12: CDXA requires a local game CHD\n"); return -1; }
        board.cdxa = s12_cdxa_create(board.disk_path, sha1, cdxa_irq, NULL, error, sizeof(error));
        if (!board.cdxa) { SysPrintf("System12 CDXA: %s\n", error); return -1; }
        if (!strcmp(board.profile, "truckk")) {
            u8 temp[20];
            memcpy(temp, board.main + 0x376ec, sizeof(temp));
            memmove(board.main + 0x376e4 + sizeof(temp), board.main + 0x376e4, 8);
            memcpy(board.main + 0x376e4, temp, sizeof(temp));
        }
    }
#endif
    if (!strcmp(board.machine, "kartduel")) {
        H8Config link_config = {0};
        link_config.read8 = link_read8; link_config.write8 = link_write8;
        link_config.external_io_start = 0xffff10;
        board.link = h8_create(&link_config);
        if (!board.link) return -1;
        board.link_reset = 1;
    }
    rtc4543_datetime_t epoch = {2026, 10, 4, 7, 12, 0, 0};
    rtc4543_init(&board.rtc, rtc_serial_data, NULL, &epoch);
    namco_settings_init(&board.settings, NULL, NULL);
    psxRamSize = board.ram_size;
    psxRomSize = board.main_size;
    if (gpu_set_vram_height(1024)) return -1;
    board.active = 1;
    board.jvs_enabled = !strcmp(board.machine, "aplarail") || !strcmp(board.machine, "technodr");
    s12_jvs_reset(&board.jvs);
    s12_jvs_reset(&board.jvs_printer);
#ifndef DRC_DISABLE
    new_dynarec_set_code_span_provider(system12_code_span);
#endif
    return 0;
}

static int board_busy(void)
{
    /* Layout must be selected before the core allocates/maps memory. */
    return psxRegs.ptrs.psxM || board.active || systemtaito_active() || systemzn_active() ||
        system10_active() || system11_active();
}

int system12_load(const char *directory)
{
    system12_desc desc = {0};
    char disk_path[4096];
    u8 default_eeprom[2080];
    char values[13][256] = {{0}};
    int result;
    if (board_busy())
        return -1;
    result = load_profile(directory, &desc, values);
    if (!result) {
        char path[4096];
        if (snprintf(path, sizeof(path), "%s/eeprom.bin", directory) >= (int)sizeof(path)) result = -1;
        else {
            FILE *f = fopen(path, "rb");
            if (f) {
                size_t size = fread(default_eeprom, 1, sizeof(default_eeprom), f);
                if (ferror(f) || (size != 2048 && size != 2080) || fgetc(f) != EOF) result = -1;
                else { desc.default_eeprom = default_eeprom; desc.default_eeprom_size = size; }
                fclose(f);
            } else if (errno != ENOENT) result = -1;
        }
    }
    if (!result && (!strcmp(desc.machine, "cdxa_pcb") || !strcmp(desc.machine, "truckk"))) {
        if (snprintf(disk_path, sizeof(disk_path), "%s/%s.chd", directory,
            !strcmp(desc.mame_set, "truckk") ? "tkk2-a" : "ul1-a") >= (int)sizeof(disk_path)) result = -1;
        else desc.disk_path = disk_path;
    }
    if (!result)
        result = apply_desc(&desc);
    if (result) {
        system12_unload();
        return result;
    }
    if (load_region(directory, "main.bin", board.main_size, &board.main) ||
        load_region(directory, "banked.bin", board.banked_size, &board.banked) ||
        load_region(directory, "sub.bin", board.sub_size, &board.sub) ||
        load_region(directory, "wave.bin", board.wave_size, &board.wave) ||
        start_board()) {
        system12_unload();
        return -1;
    }
    return 0;
}

int system12_load_regions(const system12_desc *desc, u8 *main, u8 *banked, u8 *sub, u8 *wave)
{
    int result;
    if (board_busy()) {
        free(main); free(banked); free(sub); free(wave);
        return -1;
    }
    board.main = main; board.banked = banked; board.sub = sub; board.wave = wave;
    result = apply_desc(desc);
    if (!result && (!main || !banked || !sub || !wave))
        result = -1;
    if (!result && start_board())
        result = -1;
    if (result)
        system12_unload();
    else
        /* System 12 has no PlayStation SPU: the C352 is its sound. */
        psxSetSpuPresent(0);
    return result;
}

void system12_unload(void)
{
    /* Call after psxShutdown(): sizes are needed to unmap the core. */
    system12_board_thread_stop();
    psxSetSpuPresent(1);
    bus_trace_cb = NULL;
    bus_trace_opaque = NULL;
    edge_trace_cb = NULL;
    edge_trace_opaque = NULL;
    edge_trace_shared = 0;
    psxRcntSourceScreenUnload();
#ifndef DRC_DISABLE
    new_dynarec_set_code_span_provider(NULL);
#endif
    if (board.audio_file) system12_close_audio();
    h8_destroy(board.h8);
    h8_destroy(board.link);
#ifdef PCSX_SYSTEM12_CDXA
    s12_cdxa_destroy(board.cdxa);
#endif
    free(board.disk_path);
    c352_destroy(board.pcm);
    at28c16_destroy(board.eeprom);
    free(board.main);
    free(board.banked);
    free(board.sub);
    free(board.wave);
    if (board.profile_owned) {
        free((void *)board.profile); free((void *)board.machine);
        free((void *)board.state); free((void *)board.unsupported);
    }
    memset(&board, 0, sizeof(board));
    if (!psxRegs.ptrs.psxM) {
        psxRamSize = 0x200000, psxRomSize = 0x80000;
        gpu_set_vram_height(512);
    }
}

static void map_bank(unsigned bank)
{
    unsigned segment, page;
    const u32 bases[] = { 0x1fa00000, 0x9fa00000, 0xbfa00000 };
#ifndef DRC_DISABLE
    if (board.active && board.bank != bank && psxCpu == &psxRec)
        new_dynarec_invalidate_range_hard(0x1fa00000, 0x1fc00000);
#endif
    board.bank = bank;
    if (DISABLE_MEM_LUTS)
        return;
    for (segment = 0; segment < 3; segment++)
        for (page = 0; page < WINDOW_SIZE; page += 1u << PSXM_SHIFT) {
            u32 address = bases[segment] + page;
            psxRegs.ptrs.memRLUT[address >> PSXM_SHIFT] = bank < board.banked_size / WINDOW_SIZE
                ? (uintptr_t)(board.banked + bank * WINDOW_SIZE) - bases[segment]
                : INVALID_PTR_VAL;
            psxRegs.ptrs.memWLUT[address >> PSXM_SHIFT] = INVALID_PTR_VAL;
            /* Tekken Tag's protection registers sit in the banked window:
             * reads of their pages go through system12_read. */
            if (board.tektagt_dma && (page == 0x100000 || page == 0x180000))
                psxRegs.ptrs.memRLUT[address >> PSXM_SHIFT] = INVALID_PTR_VAL;
        }
}

void system12_mem_reset(void)
{
    memcpy(psxRegs.ptrs.psxR, board.main, board.main_size);
    /* MAME allocates RAM at machine start; driver_reset only repeats the boot
     * copy on the affected boards. A soft reset does not erase either CPU RAM. */
    if (!board.booted) memset(psxRegs.ptrs.psxM, 0, PSX_RAM_SIZE);
    memset(psxRegs.ptrs.psxP, 0xff, 0x10000);
    if (board.boot_hack && board.main_size >= 0x2028c && board.ram_size >= 0x1000c)
        memcpy(psxRegs.ptrs.psxM + 0x10000, board.main + 0x20280, 12);
    /* Retain MAME's Point Blank 2 DMA-wait workaround for its synchronous DMA5. */
    if (!strcmp(board.state, "ptblank2_state"))
        memset(psxRegs.ptrs.psxR + 0x331c4, 0, 4);
    map_bank(board.booted ? board.bank : 0);
    if (!board.booted) psxRcntSourceScreenUnload();
}

void system12_reset(void)
{
    board_drain();
    pcsx_profile_reset();
    board.slice_open = 0;
    if (!board.booted) {
        board.dma_offset = board.alt_bank_offset = board.tektagt_dma_offset = 0;
        board.h8_fraction = board.h8_requested = 0;
        board.pcm_fraction = board.pcm_due = board.pcm_frames = 0;
        pcm_flags_until = 0;
        pcm_wanted = 0;
        memset(&board.pcm_h8_clock, 0, sizeof(board.pcm_h8_clock));
        board.port8 = board.porta = 0; board.portb = 0x50;
        board.jvs_player[0] = board.jvs_player[1] = board.jvs_system = 0;
        board.jvs_coin[0] = board.jvs_coin[1] = 0;
        memset(&inputs, 0, sizeof(inputs));
        inputs.gun[0] = inputs.gun[2] = !strcmp(board.state, "golgo13_state") ? 0x100 : (!strncmp(board.profile, "ptblank2", 8) || !strcmp(board.profile, "gunbarl")) ? 0x231 : 0x20e;
        inputs.gun[1] = inputs.gun[3] = !strcmp(board.state, "golgo13_state") ? 0x100 : (!strncmp(board.profile, "ptblank2", 8) || !strcmp(board.profile, "gunbarl")) ? 0xa1 : 0x91;
        memcpy(board.gun, inputs.gun, sizeof(board.gun));
        memset(board.analog, 0, sizeof(board.analog));
        if (!strcmp(board.machine, "aplarail")) inputs.analog[0] = 0x216;
        if (!strcmp(board.machine, "kartduel")) {
            inputs.analog[0] = inputs.analog[1] = 0xad; inputs.analog[2] = 0x200;
        }
        if (!strcmp(board.machine, "technodr")) {
            inputs.analog[0] = 0x5e00; inputs.analog[1] = 0x7e00;
            inputs.analog[2] = 0xed80;
        }
        if (!strcmp(board.machine, "truckk")) {
            inputs.player[1] = 2;
            inputs.analog[0] = inputs.analog[1] = inputs.analog[2] = 0x200;
        }
        if (!strcmp(board.machine, "cdxa_pcb")) inputs.player[1] = 0x30;
        board.jvs_player[1] = inputs.player[1];
        memcpy(board.analog, inputs.analog, sizeof(board.analog));
        s12_jvs_reset(&board.jvs);
        s12_jvs_reset(&board.jvs_printer);
        board.gun_outputs = 0;
        board.rtc_fraction = 0;
        memset(board.shared, 0, sizeof(board.shared));
        h8_reset(board.h8);
    } else {
        /* MAME clears instruction overshoot at reset, while retaining its
         * budget-based local time. The port's executed-state counter can be
         * ahead, so restart from the absolute requested clock, not that tail. */
        if (h8_soft_reset(board.h8, board.h8_requested))
            SysPrintf("System12: H8 soft reset outside a quiescent boundary\n");
    }
    if (board.link) board.link_reset = 1;
#ifdef PCSX_SYSTEM12_CDXA
    if (board.cdxa) s12_cdxa_reset(board.cdxa, link_clock(board.h8_requested));
#endif
    board.has_tektagt_dma = 0;
    if (!board.booted) {   /* MAME: machine_start state, not reset */
        board.ttt_cnt = 0;
        board.ttt_val[0] = board.ttt_val[1] = 0;
    }
    board.last_cycle = psxRegs.cycle;
    board.pcm_epoch_frames = board.pcm_frames;
    board.pcm_peak = 0;
    board.sub_reads = board.sub_writes = 0;
    c352_reset(board.pcm);
    rtc4543_reset(&board.rtc);
    namco_settings_reset(&board.settings);
    board.shared_reads = board.shared_writes = board.bank_writes = 0;
    board.dma_transfers = board.dma_bytes = board.dma_truncations = 0;
    board.unknown_reads = board.unknown_writes = 0;
    board.last_shared_address = board.last_shared_pc = board.last_shared_value = 0;
    board.last_unknown_address = board.last_unknown_pc = 0;
    map_bank(board.bank);
    if (psxArcadeSourceTimingActive()) {
        GPU_writeStatus(0); /* MAME resets GPU configuration, not screen time. */
        gpuSyncPluginSR();
        psxRcntSourceScreenReset(!board.booted, SWAP32(HW_GPU_STATUS));
    }
    board.booted = 1;
    psxRegs.pc = 0xbfc00000;
    psxRegs.CP0.n.SR = 0x10600000;
    psxRegs.CP0.n.PRid = 2;
    psxRegs.biosBranchCheck = ~0u;
    psxRegs.biosFuncsHooked = 0;
}

/* Tekken Tag protection, from MAME's tektagt_state (namcos12.cpp): 16-bit
 * registers on the 32-bit bus, offset 0 at bits 0-15, offset 1 at 16-31. */
static void tektagt_protection_1_w(unsigned offset, u16 data)
{
    if (offset == 0) {
        board.tektagt_dma_offset = data;
        if (board.ttt_cnt != 2)
            board.ttt_val[board.ttt_cnt] = data;
    } else {
        board.tektagt_dma_offset |= (u32)data << 16;
        if (board.ttt_cnt != 2)
            board.ttt_val[board.ttt_cnt++] |= (u32)data << 16;
    }
}

static u16 tektagt_protection_1_r(unsigned offset)
{
    return offset == 0 ? 0x8000 : 0;   /* bit 15: ready */
}

static void tektagt_protection_2_w(unsigned offset, u16 data)
{
    (void)data;
    if (offset == 0)
        board.ttt_cnt = 0;   /* start of a new pair of values */
}

static u16 tektagt_protection_2_r(unsigned offset)
{
    if (board.ttt_cnt != 2)
        return 0;
    if (board.ttt_val[0] == 0x806d2c24 && board.ttt_val[1] == 0xd5545715)
        return offset == 0 ? 0x36e2 : 0;
    if (board.ttt_val[0] == 0x804c2c84 && board.ttt_val[1] == 0xd5545615)
        return offset == 0 ? 0x2651 : 0;
    if (board.ttt_val[0] == 0x2aaba8e6 && board.ttt_val[1] == 0x00820040)
        return offset == 1 ? 0x4186 : 0;
    if (board.ttt_val[0] == 0x2aaba592 && board.ttt_val[1] == 0x01780544)
        return offset == 1 ? 0x3c7d : 0;
    if (((board.ttt_val[1] >> 16) & 0xff) == 0xa9)
        return 0x552e;
    return 0;
}

/* Byte/halfword/word accesses split into the 16-bit lanes they touch, low
 * lane first, as MAME's 32-bit bus calls a 16-bit handler. */
static u32 tektagt_protection_read(u32 a, unsigned width, u16 (*r)(unsigned))
{
    u32 v = 0;
    unsigned lane;
    for (lane = (a & 2) >> 1; lane < 2 && (lane * 2) < (a & 3) + width; lane++)
        v |= (u32)r(lane) << (lane * 16);
    return v >> ((a & 3) * 8);
}

static void tektagt_protection_write(u32 a, unsigned width, u32 value,
    void (*w)(unsigned, u16))
{
    u32 bus = value << ((a & 3) * 8);
    unsigned lane;
    for (lane = (a & 2) >> 1; lane < 2 && (lane * 2) < (a & 3) + width; lane++)
        w(lane, (u16)(bus >> (lane * 16)));
}

static int link_register(u32 a, unsigned width)
{
    return (a >= 0x1f796004 && a + width <= 0x1f79600c) ||
        (a >= 0x1f796014 && a + width <= 0x1f796018) ||
        (a >= 0x1f796020 && a + width <= 0x1f796024) ||
        (a >= 0x1f797000 && a + width <= 0x1f797004);
}

int system12_read(u32 address, unsigned width, u32 *value)
{
    u32 a = physical(address), v = 0;
    unsigned i;
    if (!board.active)
        return 0;
#ifdef PCSX_SYSTEM12_CDXA
    if (board.cdxa && a >= 0x1f7c0000 && a < 0x1f800000) {
        board_drain();
        if (s12_cdxa_read(board.cdxa, a, width, value)) { trace_bus(a, width, 0, *value); return 1; }
    }
#endif
    if (board.link && ((a >= 0x1f780000 && a + width <= 0x1f790000) || link_register(a, width))) {
        board_drain();
        const u8 *ram = a < 0x1f790000 ? board.link_shared + a - 0x1f780000 : board.link_regs + a - 0x1f796000;
        for (i = 0; i < width; i++) v |= (u32)ram[i] << (i * 8);
        *value = v; trace_bus(a, width, 0, v); return 1;
    }
    if (!strcmp(board.state, "ptblank2_state") &&
        (a & ~0x800000u) >= 0x1f780000 && (a & ~0x800000u) + width <= 0x1f780010) {
        u32 gun_address = a & ~0x800000u;
        board_drain();
        for (i = 0; i < width; i++) {
            u32 at = gun_address + i;
            u16 coord = board.gun[(at - 0x1f780000) / 4];
            if ((coord & 3) == 3) coord++;
            v |= (u32)((coord >> ((at & 1) * 8)) & 0xff) << (i * 8);
        }
        *value = v; trace_bus(a, width, 0, v); return 1;
    }
    if (board.tektagt_dma &&
        ((a >= 0x1f700000 && a < 0x1f700004) ||
         (a >= 0x1ff00000 && a < 0x1ff00004))) {
        board.has_tektagt_dma = 1;
        *value = 0;
        trace_bus(a, width, 0, 0);
        return 1;
    }
    if (board.tektagt_dma && (a & ~3u) == 0x1fb00000) {
        *value = tektagt_protection_read(a, width, tektagt_protection_1_r);
        trace_bus(a, width, 0, *value);
        return 1;
    }
    if (board.tektagt_dma && (a & ~3u) == 0x1fb80000) {
        *value = tektagt_protection_read(a, width, tektagt_protection_2_r);
        trace_bus(a, width, 0, *value);
        return 1;
    }
    /* Non-LUT fallback also permits execution from valid banked ROM; the
     * rest of Tekken Tag's protection pages reads the window the same way. */
    if ((DISABLE_MEM_LUTS || board.tektagt_dma) && a >= 0x1fa00000 && a < 0x1fc00000) {
        for (i = 0; i < width; i++)
            v |= (u32)(board.bank < board.banked_size / WINDOW_SIZE
                ? board.banked[board.bank * WINDOW_SIZE + ((a + i) & (WINDOW_SIZE - 1))]
                : 0xff) << (i * 8);
        *value = v;
        trace_bus(a, width, 0, v);
        return 1;
    }
    if (a < 0x1f000000 || a >= 0x1f800000)
        return 0;
    if (a >= 0x1f080000 && a + width <= 0x1f084000) {
        board_drain();
        for (i = 0; i < width; i++)
            v |= (u32)board.shared[a - 0x1f080000 + i] << (i * 8);
        board.shared_reads++;
        board.last_shared_address = a;
        board.last_shared_pc = psxRegs.pc;
        board.last_shared_value = v;
        trace_shared(SYSTEM12_EDGE_MAIN_SHARED_LOAD, a, width * 8, v);
    }
    else if (a >= 0x1f140000 && a + width <= 0x1f141000) {
        /* AT28C16 has an 8-bit data bus on each low byte of a 16-bit lane. */
        for (i = 0; i < width; i++)
            v |= (u32)((a + i) & 1 ? 0xff
                : at28c16_read(board.eeprom, (a + i - 0x1f140000) / 2)) << (i * 8);
    }
    else if (a < 0x1f000004)
        v = 0; /* MAME nopr bank-select register */
    else {
        v = ~0u;
        board.unknown_reads++;
        board.last_unknown_address = a;
        board.last_unknown_pc = psxRegs.pc;
    }
    *value = v;
    trace_bus(a, width, 0, v);
    return 1;
}

int system12_write(u32 address, unsigned width, u32 value)
{
    u32 a = physical(address);
    unsigned i;
    if (!board.active)
        return 0;
#ifdef PCSX_SYSTEM12_CDXA
    if (board.cdxa && a >= 0x1f7c0000 && a < 0x1f800000) {
        board_drain();
        if (s12_cdxa_write(board.cdxa, a, width, value)) { trace_bus(a, width, 1, value); return 1; }
    }
#endif
    if (board.link && ((a >= 0x1f780000 && a + width <= 0x1f790000) || link_register(a, width))) {
        board_drain();
        u8 *ram = a < 0x1f790000 ? board.link_shared + a - 0x1f780000 : board.link_regs + a - 0x1f796000;
        for (i = 0; i < width; i++) ram[i] = value >> (i * 8);
        trace_bus(a, width, 1, value); return 1;
    }
    if (board.link && a >= 0x1f796000 && a + width <= 0x1f796004) {
        board_drain();
        if (a < 0x1f796002) board.link_reset = 1;
        if (a + width > 0x1f796002) {
            if (board.link_reset) h8_soft_reset(board.link, link_clock(h8_current_cycle(board.h8)));
            board.link_reset = 0;
        }
        trace_bus(a, width, 1, value); return 1;
    }
    if (!strcmp(board.state, "ptblank2_state") &&
        (a & ~0x800000u) >= 0x1f788000 && (a & ~0x800000u) + width <= 0x1f788004) {
        if (((a & ~0x800000u) - 0x1f788000) == 0)
            board.gun_outputs = (u16)~value & 0xf;
        trace_bus(a, width, 1, value); return 1;
    }
    if (board.tektagt_dma && (a & ~3u) == 0x1fb00000) {
        tektagt_protection_write(a, width, value, tektagt_protection_1_w);
        trace_bus(a, width, 1, value);
        return 1;
    }
    if (board.tektagt_dma && (a & ~3u) == 0x1fb80000) {
        tektagt_protection_write(a, width, value, tektagt_protection_2_w);
        trace_bus(a, width, 1, value);
        return 1;
    }
    if (a < 0x1f000000 || a >= 0x1f800000)
        return 0;
    if (a < 0x1f000002) {
        u32 mask = width == 1 ? 0xff : 0xffff;
        unsigned shift = (a & 1) * 8;
        u32 written = (board.bank & ~(mask << shift)) | ((value & mask) << shift);
        if (board.alt_bank) {
            u16 data = (u16)written;
            if (data & 8) board.alt_bank_offset = (u16)((data - 8) << 2);
            else board.alt_bank_offset = (board.alt_bank_offset & ~7u) | (data & 7);
            map_bank(board.alt_bank_offset);
        }
        else map_bank(written);
        board.bank_writes++;
    }
    else if (a >= 0x1f080000 && a + width <= 0x1f084000) {
        board_drain();
        for (i = 0; i < width; i++)
            board.shared[a - 0x1f080000 + i] = value >> (i * 8);
        board.shared_writes++;
        trace_shared(SYSTEM12_EDGE_MAIN_SHARED_STORE, a, width * 8, value);
        if (board.shared_writes <= 8)
            SysPrintf("System12 shared w%u %08x=%08x pc=%08x\n",
                width * 8, a, value, psxRegs.pc);
    }
    else if (a >= 0x1f140000 && a + width <= 0x1f141000) {
        for (i = 0; i < width; i++)
            if (!((a + i) & 1))
                if (at28c16_write(board.eeprom,
                        (a + i - 0x1f140000) / 2, value >> (i * 8)) < 0 &&
                    !board.eeprom_clock_error) {
                    board.eeprom_clock_error = 1;
                    SysPrintf("System12: EEPROM write rejected\n");
                }
    }
    else if (a >= 0x1f700000 && a < 0x1f710000) {
        /* The low address bits supply the low half of the DMA source. MAME's
         * dmaoffset_w on every board; Tekken Tag's second offset comes from
         * its protection register instead. */
        board.dma_offset = ((a - 0x1f700000) & ~1u) | ((value & 0xffff) << 16);
    }
    else if ((a >= 0x1f008000 && a < 0x1f008004) ||
             (a >= 0x1f010000 && a < 0x1f010004) ||
             (a >= 0x1f018000 && a < 0x1f018004) ||
             (a >= 0x1f1bff08 && a < 0x1f1bff10) ||
             (a >= 0x1f000002 && a < 0x1f000004)) {
        /* MAME nopw windows */
    }
    else {
        board.unknown_writes++;
        board.last_unknown_address = a;
        board.last_unknown_pc = psxRegs.pc;
    }
    trace_bus(a, width, 1, value);
    return 1;
}

void system12_dma5(u32 madr, u32 bcr, u32 chcr)
{
    u64 requested, bytes;
    int special_dma = board.has_tektagt_dma;
    u64 offset = board.has_tektagt_dma ? board.tektagt_dma_offset : board.dma_offset;
    u32 destination = madr & PSX_RAM_MASK;
    int mainrom = (offset & 0x80000000) || psxHu32(0x1000) == 0x1f300000;
    u8 *source = mainrom ? board.main : board.banked;
    u64 source_size = mainrom ? board.main_size : board.banked_size;
    if (!board.active)
        return;
    board.has_tektagt_dma = 0;
    /* Sync 0 counts words; sync 1 counts blocks of words. Linked lists are
     * unsupported for the ROM channel. Completion is synchronous for now. */
    requested = chcr == 0x01000200
        ? (u64)((bcr >> 16) ? (bcr >> 16) : 0x10000) * (bcr & 0xffff) : bcr;
    requested *= 4;
    offset &= mainrom ? 0xffffff : 0x7fffffff;
    offset &= ~1u;
    if (!mainrom && !special_dma && board.alt_bank)
        offset += board.alt_bank_offset * (u64)WINDOW_SIZE;
    destination &= ~3u;
    bytes = requested;
    if (offset >= source_size ||
        (chcr != 0x01000000 && chcr != 0x01000200))
        bytes = 0;
    else {
        if (bytes > source_size - offset) bytes = source_size - offset;
        if (bytes > PSX_RAM_SIZE - destination) bytes = PSX_RAM_SIZE - destination;
        bytes &= ~3ull;
    }
    if (bytes < requested) board.dma_truncations++;
    if (bytes) {
        memcpy(psxRegs.ptrs.psxM + destination, source + offset, bytes);
        psxCpu->Clear(destination, bytes / 4);
    }
    board.dma_transfers++;
    board.dma_bytes += bytes;
    if (board.dma_transfers <= 8)
        SysPrintf("System12 DMA5 %s %08" PRIx64 " -> %08x bytes=%" PRIu64 "/%" PRIu64 " chcr=%08x pc=%08x\n",
            mainrom ? "main" : "banked", offset, destination, bytes, requested, chcr, psxRegs.pc);
    /* MAME's ROM read callback and DMA completion leave MADR unchanged. */
    psxHu32ref(0x10d8) &= SWAPu32(~0x11000000u);
    DMA_INTERRUPT(5);
}

void system12_report(FILE *out)
{
    board_drain();
    pcm_flush_wanted();
#ifdef PCSX_SYSTEM12_CDXA
    if (board.cdxa) s12_cdxa_report(board.cdxa, out);
#endif
    if (board.link) {
        H8Status link_state;
        h8_get_status(board.link, &link_state);
        fprintf(out, "System12: link reset=%d pc=%06x cycles=%" PRIu64 " illegal=%06x:%04x unsupported=%x\n",
            board.link_reset, link_state.pc, link_state.cycles,
            link_state.illegal_pc, link_state.illegal_opcode, link_state.unsupported_peripherals);
    }
    if (board.jvs_enabled)
        fprintf(out, "System12: JVS address=%u packets=%u errors=%u coins=%u/%u outputs=%08x\n",
            board.jvs.address, board.jvs.packets, board.jvs.errors,
            board.jvs.coins[0], board.jvs.coins[1], board.jvs.outputs);
    fprintf(out, "System12: JAMMA=%02x/%02x/%02x/%02x\n",
        system12_jamma_read(0), system12_jamma_read(1),
        system12_jamma_read(2), system12_jamma_read(3));
    fprintf(out, "System12: profile=%s machine=%s state=%s RAM=%u main=%" PRIu64 " banked=%" PRIu64 " sub=%" PRIu64 " wave=%" PRIu64 " bank=%u alt=%u boot_hack=%u\n",
        board.profile ? board.profile : "inactive", board.machine ? board.machine : "-",
        board.state ? board.state : "-", PSX_RAM_SIZE,
        (uint64_t)board.main_size, (uint64_t)board.banked_size,
        (uint64_t)board.sub_size, (uint64_t)board.wave_size,
        board.bank, board.alt_bank, board.boot_hack);
    fprintf(out, "System12: DMA5=%" PRIu64 " bytes=%" PRIu64 " truncations=%" PRIu64 " bank_writes=%" PRIu64 "\n",
        board.dma_transfers, board.dma_bytes, board.dma_truncations, board.bank_writes);
    fprintf(out, "System12: shared reads=%" PRIu64 " writes=%" PRIu64 " last=%08x value=%08x pc=%08x\n",
        board.shared_reads, board.shared_writes, board.last_shared_address,
        board.last_shared_value, board.last_shared_pc);
    fprintf(out, "System12: unmapped reads=%" PRIu64 " writes=%" PRIu64 " last=%08x pc=%08x\n",
        board.unknown_reads, board.unknown_writes, board.last_unknown_address, board.last_unknown_pc);
    H8Status h8state;
    h8_get_status(board.h8, &h8state);
    if (system12_h8_frames > 1) {
        /* Same quantities as the Vita's progress lines (b_h8, h8_ins). */
        const unsigned long long frames = system12_h8_frames - 1;
        const unsigned long long us = system12_h8_us - perf_base_us;
        const unsigned long long executed = system12_h8_instructions - perf_base_instructions;
        uint64_t counters[4];
        h8_perf_counters(counters);
        fprintf(out, "System12: H8 perf frames=%llu us=%llu executed=%llu us/frame=%.3f executed/frame=%.0f ns/instruction=%.2f pcm_us/frame=%.3f\n",
            frames, us, executed, (double)us / frames, (double)executed / frames,
            executed ? us * 1000.0 / executed : 0.0, (double)system12_pcm_us / system12_h8_frames);
        fprintf(out, "System12: board us/frame=%.3f (all commands)\n",
            (double)(system12_board_us - perf_base_board_us) / frames);
        fprintf(out, "System12: H8 loop calls/frame=%.0f skips/frame=%.0f batch/frame=%.0f single/frame=%.0f (since start)\n",
            (double)counters[0] / system12_h8_frames, (double)counters[1] / system12_h8_frames,
            (double)counters[2] / system12_h8_frames, (double)counters[3] / system12_h8_frames);
    }
    fprintf(out, "System12: H8 pc=%06x er7=%08x ccr=%02x cycles=%" PRIu64 " instructions=%" PRIu64 " halted=%u illegal=%06x:%04x unsupported=%x shared r=%" PRIu64 " w=%" PRIu64 "\n",
        h8state.pc, h8state.er[7], h8state.ccr, h8state.cycles, h8state.instructions,
        h8state.halted, h8state.illegal_pc, h8state.illegal_opcode,
        h8state.unsupported_peripherals, board.sub_reads, board.sub_writes);
    fprintf(out, "System12: H8 SCI0=%02x/%02x/%02x SCI1=%02x/%02x/%02x IRQ=%02x/%02x/%02x portB=%02x\n",
        h8_read_byte(board.h8, 0xffffb0), h8_read_byte(board.h8, 0xffffb2), h8_read_byte(board.h8, 0xffffb4),
        h8_read_byte(board.h8, 0xffffb8), h8_read_byte(board.h8, 0xffffba), h8_read_byte(board.h8, 0xffffbc),
        h8_read_byte(board.h8, 0xfffff4), h8_read_byte(board.h8, 0xfffff5), h8_read_byte(board.h8, 0xfffff6), board.portb);
    fprintf(out, "System12: C352 rate=%u frames=%" PRIu64 " peak=%u WAV_bytes=%" PRIu64 "\n",
        c352_sample_rate(board.pcm), board.pcm_frames, board.pcm_peak, board.audio_bytes);
    fprintf(out, "System12: settings frames=%" PRIu64 " last=%02x:%02x RTC wirebits=%u\n",
        board.settings.frames_received, board.settings.last_address, board.settings.last_value,
        board.rtc.bit_count);
    fprintf(out, "System12: CPU pc=%08x opcode=%08x ra=%08x sr=%08x cause=%08x epc=%08x epc_opcode=%08x; clock=%u cycle_multiplier=%d timing=%s\n",
        psxRegs.pc, intFakeFetch(psxRegs.pc), psxRegs.GPR.n.ra,
        psxRegs.CP0.n.SR, psxRegs.CP0.n.Cause, psxRegs.CP0.n.EPC,
        intFakeFetch(psxRegs.CP0.n.EPC),
        PSXCLK, Config.cycle_multiplier,
#ifdef PCSX_SYSTEM12_SOURCE_TIMING
        "source-experiment"
#else
        "console"
#endif
    );
}

int system12_selftest(void)
{
    int failures = 0;
    u32 value, first, last;
    unsigned bank;
    AT28C16State saved_eeprom;
    u64 saved_eeprom_time = board.eeprom_time_us;
    u64 saved_eeprom_fraction = board.eeprom_cycle_fraction;
    int saved_eeprom_error = board.eeprom_clock_error;
    int eeprom_state_valid = board.eeprom &&
        at28c16_get_state(board.eeprom, &saved_eeprom) == 0;
    board_drain();
#define CHECK(condition) do { if (!(condition)) { \
    SysPrintf("System12 selftest FAIL line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)
    /* Exercise both CPU endiannesses and byte lanes through the H8 bus. */
    psxMemWrite32(&psxRegs, 0x1f080000, 0x76543210);
    CHECK(h8_read_byte(board.h8, 0x080000) == 0x32);
    CHECK(h8_read_byte(board.h8, 0x080001) == 0x10);
    CHECK(h8_read_byte(board.h8, 0x080002) == 0x76);
    h8_write_byte(board.h8, 0x080002, 0xcd);
    h8_write_byte(board.h8, 0x080003, 0xef);
    CHECK(psxMemRead32(&psxRegs, 0x1f080000) == 0xcdef3210);
    sub_write16(NULL, 0x280000, 0x1234);
    CHECK(sub_read16(NULL, 0x280000) == 0x1234);
    CHECK(board.active && PSX_RAM_SIZE == board.ram_size && PSX_ROM_SIZE == board.main_size);
    if (board.ram_size > DEFAULT_RAM_SIZE) {
        psxMemWrite32(&psxRegs, board.ram_size - 4, 0x5a12c0de);
        CHECK(psxMemRead32(&psxRegs, board.ram_size - 4) == 0x5a12c0de);
    }
    if (board.boot_hack)
        CHECK(!memcmp(psxRegs.ptrs.psxM + 0x10000, board.main + 0x20280, 12));
    CHECK(system12_jamma_read(0) == 0xff && system12_jamma_read(1) == 0xff &&
          system12_jamma_read(2) == 0xff && system12_jamma_read(3) == 0xff);
    CHECK(!system12_set_input("p1start", 1) && system12_jamma_read(1) == 0x7f);
    CHECK(h8_read_byte(board.h8, 0x300001) == 0x7f);
    CHECK(!system12_set_input("service", 1) && system12_jamma_read(2) == 0x7f);
    CHECK(!system12_set_input("test", 1) && system12_jamma_read(2) == 0x3f);
    CHECK(!system12_set_input("coin1", 1) && system12_jamma_read(2) == 0x1f);
    CHECK(!system12_set_input("p1button7", 1) && system12_jamma_read(2) == 0x17);
    CHECK(!system12_set_input("p2button8", 1) && system12_jamma_read(2) == 0x16);
    CHECK(!system12_set_input("p1button6", 1) && system12_jamma_read(3) == 0xf7);
    CHECK(!system12_set_input("p2button4", 1) && system12_jamma_read(3) == 0xd7);
    CHECK(system12_set_input("unknown", 1) < 0);
    if (!strcmp(board.state, "ptblank2_state")) {
        u16 saved_gun[4];
        memcpy(saved_gun, board.gun, sizeof(saved_gun));
        CHECK(!system12_set_analog("p1gunx", 0x35f));
        CHECK(!system12_set_analog("p1guny", 0x103));
        board_drain();
        CHECK(psxMemRead32(&psxRegs, 0x1f780000) == 0x03600360);
        CHECK(psxMemRead16(&psxRegs, 0x1ff80004) == 0x104);
        CHECK(psxMemRead8(&psxRegs, 0x1f780001) == 3);
        psxMemWrite16(&psxRegs, 0x1ff88000, 0xfffa);
        CHECK(board.gun_outputs == 5);
        psxMemWrite8(&psxRegs, 0x1ff88001, 0);
        CHECK(board.gun_outputs == 5);
        CHECK(system12_set_analog("p1gunx", 65536) < 0);
        CHECK(system12_set_gun_position(0, 0, 65535) == 0);
        board_drain();
        CHECK(board.gun[0] == ((!strncmp(board.profile, "ptblank2", 8) || !strcmp(board.profile, "gunbarl")) ? 0xdc : 0xbe));
        CHECK(board.gun[1] == ((!strncmp(board.profile, "ptblank2", 8) || !strcmp(board.profile, "gunbarl")) ? 0x11b : 0x109));
        memcpy(board.gun, saved_gun, sizeof(saved_gun));
        memcpy(inputs.gun, saved_gun, sizeof(saved_gun));
    }
    if (!strcmp(board.state, "golgo13_state")) {
        CHECK(sub_adc_read(NULL, 1) == ((u32)board.gun[0] * 0x240) / 0x1ff + 0x7a);
        CHECK(sub_adc_read(NULL, 2) == ((u32)board.gun[1] * 0x1bf) / 0x1ff + 0x1f);
        CHECK(system12_set_gun_position(0, 65535, 0) == 0);
        board_drain();
        CHECK(board.gun[0] == 511 && board.gun[1] == 511);
        CHECK(sub_adc_read(NULL, 1) == 0x2ba && sub_adc_read(NULL, 2) == 0x1de);
    }
    memset(&inputs, 0, sizeof(inputs));
    post_inputs();
    CHECK(intFakeFetch(0xbfc00000) == SWAPu32(*(u32 *)board.main));
    CHECK(psxMemRead32(&psxRegs, 0x1ffffffc) == SWAPu32(*(u32 *)(board.main + board.main_size - 4)));
    /* Upper 2 MiB must not alias lower RAM; all physical/KSEG mirrors agree. */
    psxMemWrite32(&psxRegs, 0x10000, 0x12345678);
    psxMemWrite32(&psxRegs, 0x210000, 0x87654321);
    CHECK(psxMemRead32(&psxRegs, 0x10000) == 0x12345678);
    CHECK(psxMemRead32(&psxRegs, 0x80210000) == 0x87654321);
    CHECK(psxMemRead32(&psxRegs, 0xa0210000) == 0x87654321);
    CHECK(psxMemRead32(&psxRegs, 0x610000) == (board.ram_size == DEFAULT_RAM_SIZE ? 0x87654321 : 0));
    for (bank = 0; bank < board.banked_size / WINDOW_SIZE; bank++) {
        if (board.alt_bank) {
            psxMemWrite16(&psxRegs, 0xbf000000, 8 + ((bank >> 2) & 7));
            if (bank & 3) psxMemWrite16(&psxRegs, 0xbf000000, bank & 3);
        }
        else psxMemWrite16(&psxRegs, 0xbf000000, bank);
        first = SWAPu32(*(u32 *)(board.banked + bank * WINDOW_SIZE));
        last = SWAPu32(*(u32 *)(board.banked + (bank + 1) * WINDOW_SIZE - 4));
        CHECK(psxMemRead32(&psxRegs, 0x1fa00000) == first);
        CHECK(psxMemRead32(&psxRegs, 0x9fbffffc) == last);
        CHECK(intFakeFetch(0xbfa00000) == first);
    }
    if (!board.alt_bank) {
        psxMemWrite16(&psxRegs, 0x1f000000, board.banked_size / WINDOW_SIZE);
        CHECK(psxMemRead32(&psxRegs, 0x1fa00000) == ~0u);
    }
    else {
        psxMemWrite16(&psxRegs, 0x1f000000, 8); /* bankoffset zero */
    }
    psxMemWrite32(&psxRegs, 0x1f080000, 0x76543210);
    CHECK(psxMemRead16(&psxRegs, 0xbf080002) == 0x7654);
    psxMemWrite8(&psxRegs, 0x9f080001, 0xab);
    CHECK(psxMemRead32(&psxRegs, 0x1f080000) == 0x7654ab10);
    {
        int failures_before_eeprom = failures;
        u64 remaining_199 = 199ull * PSXCLK - board.eeprom_cycle_fraction;
        u64 remaining_200 = 200ull * PSXCLK - board.eeprom_cycle_fraction;
        u32 cycles_to_199us = (u32)((remaining_199 + 999999) / 1000000);
        u32 cycles_to_200us = (u32)((remaining_200 + 999999) / 1000000);
        CHECK(eeprom_state_valid);
        psxMemWrite16(&psxRegs, 0x1f140000, 0x00cd);
        CHECK(psxMemRead16(&psxRegs, 0xbf140000) == 0xff4d); /* D7 polling */
        psxMemWrite16(&psxRegs, 0x1f140002, 0x0012); /* ignored while busy */
        CHECK(eeprom_advance_cycles(cycles_to_199us) == 0);
        CHECK(psxMemRead16(&psxRegs, 0x1f140000) == 0xff4d);
        CHECK(at28c16_busy(board.eeprom));
        CHECK(eeprom_advance_cycles(cycles_to_200us - cycles_to_199us) == 0);
        CHECK(!at28c16_busy(board.eeprom));
        CHECK(psxMemRead16(&psxRegs, 0x1f140000) == 0xffcd);
        psxMemWrite16(&psxRegs, 0x1f140002, 0x0012);
        CHECK(psxMemRead16(&psxRegs, 0x1f140002) == 0xff92);
        remaining_199 = 199ull * PSXCLK - board.eeprom_cycle_fraction;
        remaining_200 = 200ull * PSXCLK - board.eeprom_cycle_fraction;
        cycles_to_199us = (u32)((remaining_199 + 999999) / 1000000);
        cycles_to_200us = (u32)((remaining_200 + 999999) / 1000000);
        CHECK(eeprom_advance_cycles(cycles_to_199us) == 0);
        CHECK(at28c16_busy(board.eeprom));
        CHECK(eeprom_advance_cycles(cycles_to_200us - cycles_to_199us) == 0);
        CHECK(psxMemRead32(&psxRegs, 0xbf140000) == 0xff12ffcd);
        if (failures == failures_before_eeprom)
            SysPrintf("System12 EEPROM timing: PASS\n");
    }
    psxMemWrite32(&psxRegs, 0x1f8010f0, 8u << 20);
    psxMemWrite16(&psxRegs, 0x1f700010, 0x8000);
    psxMemWrite32(&psxRegs, 0x1f8010d0, 0x210000);
    psxMemWrite32(&psxRegs, 0x1f8010d4, 0x10004);
    psxMemWrite32(&psxRegs, 0x1f8010d8, 0x01000200);
    CHECK(!memcmp(psxRegs.ptrs.psxM + 0x210000, board.main + 0x10, 16));
    CHECK(!(psxHu32(0x10d8) & 0x01000000));
    CHECK(psxHu32(0x10d0) == 0x210000);
    /* Starting CHCR before enabling DPCR must defer the transfer. */
    psxMemWrite32(&psxRegs, 0x1f8010f0, 0);
    psxMemWrite16(&psxRegs, 0x1f700000, 0x0010);
    psxMemWrite32(&psxRegs, 0x1f8010d0, 0x210020);
    psxMemWrite32(&psxRegs, 0x1f8010d4, 0x10004);
    psxMemWrite32(&psxRegs, 0x1f8010d8, 0x01000200);
    CHECK(psxHu32(0x10d8) & 0x01000000);
    psxMemWrite32(&psxRegs, 0x1f8010f0, 8u << 20);
    CHECK(!memcmp(psxRegs.ptrs.psxM + 0x210020, board.banked + 0x100000, 16));
    if (board.alt_bank && board.banked_size >= 0x600000) {
        static const u8 alt_dma_pattern[16] = "alt-bank-dma-ok!";
        u8 saved[sizeof(alt_dma_pattern)];
        memcpy(saved, board.banked + 0x500000, sizeof(saved));
        memcpy(board.banked + 0x500000, alt_dma_pattern, sizeof(saved));
        psxMemWrite16(&psxRegs, 0xbf000000, 8);
        psxMemWrite16(&psxRegs, 0xbf000000, 2); /* bankoffset = 2 */
        psxMemWrite16(&psxRegs, 0x1f700000, 0x0010); /* DMA byte offset = 1 MiB */
        psxDma5(0x210040, 0x10004, 0x01000200);
        CHECK(!memcmp(psxRegs.ptrs.psxM + 0x210040, alt_dma_pattern, sizeof(saved)));
        memcpy(board.banked + 0x500000, saved, sizeof(saved));
        psxMemWrite16(&psxRegs, 0xbf000000, 8); /* restore bankoffset = 0 */
    }
    /* Bounds truncation and invalid offsets must not overrun either region. */
    psxMemWrite16(&psxRegs, 0x1f70fffc, 0x803f);
    psxDma5(0x3ffffc, 0x10004, 0x01000200);
    CHECK(!memcmp(psxRegs.ptrs.psxM + 0x3ffffc, board.main + board.main_size - 4, 4));
    psxMemWrite16(&psxRegs, 0x1f700000, 0x7fff);
    psxDma5(0x210000, 0x10004, 0x01000200);
    CHECK(board.dma_truncations == 2);
    CHECK(system12_read(0x1f000000, 4, &value) && value == 0);
    /* DMA masks the base to 4 MiB. A zero block count means 65536 blocks. */
    psxMemWrite16(&psxRegs, 0x1f700000, 0x8000);
    psxDma5(0x610000, 1, 0x01000200);
    CHECK(!memcmp(psxRegs.ptrs.psxM + (board.ram_size == DEFAULT_RAM_SIZE ? 0x210000 : 0x610000),
                  board.main, 0x40000));
    /* DMA IRQ propagation uses the existing PSX interrupt controller. */
    psxMemWrite32(&psxRegs, 0x1f8010f4, (1u << 21) | (1u << 23));
    psxMemWrite16(&psxRegs, 0x1f700000, 0x8000);
    psxDma5(0x210000, 0x10004, 0x01000200);
    CHECK(psxHu32(0x10f4) & (1u << 29));
    CHECK(psxHu32(0x1070) & 8);
    /* A linked-list GPU packet in upper RAM must use the 4 MiB address mask
     * in both the core and the rasterizer, rather than aliasing low RAM. */
    psxMemWrite32(&psxRegs, 0x210100, 0x03ffffff);
    psxMemWrite32(&psxRegs, 0x210104, 0x020000ff); // red fill
    psxMemWrite32(&psxRegs, 0x210108, 0);          // x=0,y=0
    psxMemWrite32(&psxRegs, 0x21010c, 0x00010010);// 16x1 pixels
    psxMemWrite32(&psxRegs, 0x1f8010f0, 8u << 8);
    psxMemWrite32(&psxRegs, 0x1f8010a0, 0x210100);
    psxMemWrite32(&psxRegs, 0x1f8010a8, 0x01000401);
    GPU_readStatus();
    GPU_writeData(0xc0000000); // read back two red pixels from VRAM
    GPU_writeData(0);
    GPU_writeData(0x00010002);
    CHECK(GPU_readData() == 0x001f001f);
    SysPrintf("System12 selftest: %s\n", failures ? "FAIL" : "PASS");
    /* Selftest changes guest state: always restore a fresh board boot. */
    if (eeprom_state_valid)
        (void)at28c16_set_state(board.eeprom, &saved_eeprom);
    board.eeprom_time_us = saved_eeprom_time;
    board.eeprom_cycle_fraction = saved_eeprom_fraction;
    board.eeprom_clock_error = saved_eeprom_error;
    GPU_writeStatus(0);
    /* Validation deliberately dirties RAM/banks: restore a cold boot here,
     * rather than pretending that a normal hardware soft reset erases RAM. */
    board.booted = 0;
    psxReset();
    return failures ? -1 : 0;
#undef CHECK
}
