/* GPL-2.0-or-later. Native host wiring for source-derived Sony ZN daughterboards. */
#include "systemzn.h"
#include "plugins.h"
#include "system10.h"
#include "system11.h"
#include "system12.h"
#include "systemtaito.h"
#include "psxmem.h"
#include "psxhw.h"
#include "arcade/capcom_zn.h"
#include "arcade/zn_board.h"
#include "arcade/zn_mcu.h"
#include "arcade/psx_sio.h"
#include "arcade/qsound_hle.h"
#include "arcade/pcm_wav.h"
#include "arcade/raizing_sound.h"
#include "arcade/atlus_sound.h"
#include "arcade/cbaj_sound.h"
#include "arcade/tecmo_link.h"
#include "arcade/primal_zn_bus.h"
#include "arcade/nbajam_zn_bus.h"
#include "arcade/jdredd_zn_bus.h"
#include "arcade/metro_zn_bus.h"
#ifdef HAVE_CHD
#include "libchdr/chd.h"
#endif
#include "arcade/rax_sound.h"
#include "arcade/chd_hd.h"
#include <math.h>
#include "gpu_vram.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef DRC_DISABLE
#include "new_dynarec/new_dynarec.h"
#endif
#ifdef SYSTEM12_BOARD_THREAD
#include <stdatomic.h>
#include <retro_timers.h>
#include "../frontend/pcsxr-threads.h"
#endif

#define SYSTEMZN_PSX_CLOCK 33868800u
#define SYSTEMZN_QSOUND_RATE 24038u
#define SYSTEMZN_ROM_PAGE (1u << PSXM_SHIFT)

typedef struct ZnProfile {
    const char *set, *parent, *bios_parent, *bios, *model, *family, *input;
    uint32_t cpu_clock, gpu_clock, ram_size, vram_size;
    size_t bios_size, banked_size, country_size, audio_size, qsound_size;
} ZnProfile;
typedef struct TpsMediaCheck {
    unsigned region; uint32_t offset, length; unsigned group, stride, swap; uint32_t crc;
} TpsMediaCheck;
typedef struct ZnFillCheck { unsigned region; uint32_t offset, length; uint8_t value; } ZnFillCheck;
#include "arcade/tecmo_tps_profiles.inc"
#include "arcade/raizing_zn_profiles.inc"
#include "arcade/visco_zn_profiles.inc"
#include "arcade/atlus_zn_profiles.inc"
#include "arcade/cbaj_tps_profiles.inc"
#include "arcade/primal_zn_profiles.inc"
#include "arcade/nbajam_zn_profiles.inc"
#include "arcade/jdredd_zn_profiles.inc"
#include "arcade/metro_zn_profiles.inc"

static const ZnProfile profiles[] = {
#include "arcade/capcom_zn_profiles.inc"
};

typedef struct SystemZn {
    CapcomZn *capcom;
    RaizingSound *raizing_sound;
    int raizing;
    int visco, atlus, cbaj;
    CbajSound *cbaj_sound;
    TecmoLink *tecmo_link;
    int primal, nbajam, jdredd, memory_initialized;
    int metro;uint8_t metro_dsw;
    MetroZnBus metro_bus;
#ifdef HAVE_CHD
    chd_file *metro_media;
#endif
    JdreddZnBus judge_bus;
    uint64_t judge_irq_edges;uint8_t judge_ata_irq;
    NbajamZnBus nba_bus;
    RaxSound *rax;
    void (*rax_callback)(void *,const float *,size_t,unsigned,const uint64_t *);
    void *rax_opaque;
    PrimalZnBus *primal_bus;
    ata_storage_t *primal_ata;
    arcade_chd_hd_t *primal_disk;
    arcade_chd_hd_info_t primal_disk_info;
    uint64_t primal_ns, primal_fraction, disk_reads, disk_writes, disk_errors;
    AtlusSound *atlus_sound;
    uint64_t coin_count[4];
    uint8_t coin_lockout;
    void (*ymf_callback)(void *, float, float, uint64_t);
    void *ymf_opaque;
    QSoundHLE *qsound;
    ZNMCU *mcu;
    zn_board_t common;
    psx_sio_t *sio[2];
    const ZnProfile *profile;
    uint8_t *regions[8]; /* BIOS, banked, country, audio, samples, CAT0, CAT1, DSP */
    size_t sizes[8];
    uint16_t *qsound_words;
    uint8_t inputs[6];
    uint32_t kicks[2];
    int cat_dataout[2];
    int mcu_dataout;
    int sio_irq[2];
    uint32_t last_psx_cycle;
    uint64_t sio_cycles, sio_fraction;
    uint64_t peripheral_cycles;
    uint64_t qsound_fraction;
    uint64_t sound_cycles;
    uint64_t frames;
    unsigned peak;
#ifdef SYSTEM12_BOARD_THREAD
    atomic_uint faults;
#else
    uint32_t faults;
#endif
    size_t mapped_bank_offset;
    PcmWav wav;
    void (*audio_callback)(void *, const int16_t *, unsigned);
    void *audio_opaque;
    int16_t audio_buffer[512];
    unsigned audio_buffer_frames;
    int synthetic;
    int tps;
    unsigned tps_bank;
    uint64_t tps_bank_writes;
    int active;
} SystemZn;
static SystemZn board;

static uint32_t canonical_address(uint32_t address)
{
    if (address >= 0x80000000u && address < 0xc0000000u)
        return address & 0x1fffffffu;
    return address;
}

/* Only Z80/QSound advances run on the core-2 board worker. */
#ifdef SYSTEM12_BOARD_THREAD
#define ZN_QUEUE 64
#define ZN_SLICES 64
/* Retain every original nominal deadline. Only queue publication is batched;
 * merging cycle budgets could move QSound accesses across sample edges. */
struct zn_sound_work { unsigned count; u32 cycles[ZN_SLICES]; };
#define ZN_AUDIO_FRAMES 65536
static struct {
    sthread_t *thread;
    slock_t *lock;
    scond_t *work, *done_cond;
    struct zn_sound_work queue[ZN_QUEUE], pending;
    unsigned posted;
    atomic_uint done, audio_head, audio_tail;
    int quit, active;
    int16_t audio[ZN_AUDIO_FRAMES * 2];
    u64 commands, worker_cycles, worker_slices;
} bt;

static void zn_audio_pump(void)
{
    for (;;) {
        unsigned head = atomic_load_explicit(&bt.audio_head, memory_order_acquire);
        unsigned tail = atomic_load_explicit(&bt.audio_tail, memory_order_relaxed);
        unsigned count = head - tail, at = tail % ZN_AUDIO_FRAMES;
        if (!count) break;
        if (count > ZN_AUDIO_FRAMES - at) count = ZN_AUDIO_FRAMES - at;
        if (board.audio_callback)
            board.audio_callback(board.audio_opaque, bt.audio + at * 2, count);
        atomic_store_explicit(&bt.audio_tail, tail + count, memory_order_release);
    }
}

static void zn_audio_push(const int16_t *samples, unsigned frames)
{
    while (frames) {
        unsigned head = atomic_load_explicit(&bt.audio_head, memory_order_relaxed);
        unsigned tail = atomic_load_explicit(&bt.audio_tail, memory_order_acquire);
        unsigned count = ZN_AUDIO_FRAMES - (head - tail), at = head % ZN_AUDIO_FRAMES;
        if (!count) {
            slock_lock(bt.lock);
            scond_signal(bt.done_cond);
            slock_unlock(bt.lock);
            retro_sleep(1);
            continue;
        }
        if (count > frames) count = frames;
        if (count > ZN_AUDIO_FRAMES - at) count = ZN_AUDIO_FRAMES - at;
        memcpy(bt.audio + at * 2, samples, (size_t)count * 4);
        atomic_store_explicit(&bt.audio_head, head + count, memory_order_release);
        samples += count * 2; frames -= count;
    }
}

static STRHEAD_RET_TYPE zn_thread(void *unused)
{
    (void)unused;
    slock_lock(bt.lock);
    for (;;) {
        unsigned next = atomic_load_explicit(&bt.done, memory_order_relaxed);
        QSoundHLEStatus status;
        const struct zn_sound_work *work;
        if (next == bt.posted) {
            if (bt.quit) break;
            scond_wait(bt.work, bt.lock);
            continue;
        }
        work = &bt.queue[next % ZN_QUEUE];
        slock_unlock(bt.lock);
        for (unsigned i = 0; i < work->count; i++) {
            if (capcom_zn_run_sound_cycles(board.capcom, work->cycles[i]) < 0) board.faults |= 8u;
            bt.worker_cycles += work->cycles[i];
        }
        bt.commands++; bt.worker_slices += work->count;
        if (qsound_hle_get_status(board.qsound, &status)) board.faults |= 2u;
        slock_lock(bt.lock);
        atomic_store_explicit(&bt.done, next + 1, memory_order_release);
        scond_signal(bt.done_cond);
    }
    slock_unlock(bt.lock);
    STRHEAD_RETURN();
}

static void zn_wait(unsigned target)
{
    if ((int)(atomic_load_explicit(&bt.done, memory_order_acquire) - target) >= 0)
        return;
    slock_lock(bt.lock);
    while ((int)(atomic_load_explicit(&bt.done, memory_order_acquire) - target) < 0) {
        if (atomic_load_explicit(&bt.audio_head, memory_order_acquire) !=
            atomic_load_explicit(&bt.audio_tail, memory_order_relaxed)) {
            slock_unlock(bt.lock);
            zn_audio_pump();
            slock_lock(bt.lock);
        } else scond_wait(bt.done_cond, bt.lock);
    }
    slock_unlock(bt.lock);
}

static void zn_post_pending(void)
{
    if (!bt.pending.count) return;
    zn_wait(bt.posted - ZN_QUEUE + 1);
    bt.queue[bt.posted % ZN_QUEUE] = bt.pending;
    bt.pending.count = 0;
    slock_lock(bt.lock);
    bt.posted++;
    scond_signal(bt.work);
    slock_unlock(bt.lock);
}
#endif

static void zn_drain(void)
{
#ifdef SYSTEM12_BOARD_THREAD
    if (bt.active) {
        zn_post_pending();
        zn_wait(bt.posted);
        zn_audio_pump();
    }
#endif
}

static void zn_flush_audio(void)
{
    zn_drain();
    if (board.audio_callback && board.audio_buffer_frames)
        board.audio_callback(board.audio_opaque, board.audio_buffer, board.audio_buffer_frames);
    board.audio_buffer_frames = 0;
}

static void zn_run_sound(u32 cycles)
{
#ifdef SYSTEM12_BOARD_THREAD
    if (bt.active) {
        bt.pending.cycles[bt.pending.count++] = cycles;
        if (bt.pending.count == ZN_SLICES) zn_post_pending();
        return;
    }
#endif
    if (capcom_zn_run_sound_cycles(board.capcom, cycles) < 0) board.faults |= 8u;
}

int systemzn_board_thread_active(void)
{
#ifdef SYSTEM12_BOARD_THREAD
    return bt.active;
#else
    return 0;
#endif
}

int systemzn_board_thread_start(void)
{
#ifdef SYSTEM12_BOARD_THREAD
    if (bt.active) return 0;
    if (!systemzn_capcom_active()) return -1;
    bt.lock = slock_new(); bt.work = scond_new(); bt.done_cond = scond_new();
    if (bt.lock && bt.work && bt.done_cond) {
        bt.posted = 0; bt.quit = 0; bt.commands = bt.worker_cycles = bt.worker_slices = 0; bt.pending.count = 0;
        atomic_store(&bt.done, 0);
        atomic_store(&bt.audio_head, 0); atomic_store(&bt.audio_tail, 0);
        bt.active = 1;
        bt.thread = pcsxr_sthread_create(zn_thread, PCSXRT_BOARD);
        if (bt.thread) {
            SysPrintf("SystemZN: board thread started (Z80 + QSound)\n");
            return 0;
        }
        bt.active = 0;
    }
    if (bt.done_cond) scond_free(bt.done_cond);
    if (bt.work) scond_free(bt.work);
    if (bt.lock) slock_free(bt.lock);
    bt.done_cond = bt.work = NULL; bt.lock = NULL;
#endif
    return -1;
}

void systemzn_board_thread_stop(void)
{
#ifdef SYSTEM12_BOARD_THREAD
    if (!bt.active) return;
    zn_flush_audio();
    slock_lock(bt.lock);
    bt.quit = 1; scond_signal(bt.work);
    slock_unlock(bt.lock);
    sthread_join(bt.thread); bt.thread = NULL; bt.active = 0;
    scond_free(bt.done_cond); scond_free(bt.work); slock_free(bt.lock);
    bt.done_cond = bt.work = NULL; bt.lock = NULL;
    SysPrintf("SystemZN: board thread stopped\n");
#endif
}

void systemzn_audio_pump(void)
{
#ifdef SYSTEM12_BOARD_THREAD
    if (bt.active) { zn_post_pending(); zn_audio_pump(); }
#endif
}

int systemzn_capcom_active(void) { return board.active && board.capcom != NULL; }

#ifndef DRC_DISABLE
static int systemzn_code_span(unsigned pc, const unsigned **words, unsigned *limit)
{
    u32 address = canonical_address(pc), end;
    const uint8_t *region;
    size_t offset, size;
    if (!systemzn_capcom_active() || (pc & 3)) return 0;
    if (address >= 0x1f000000u && address < 0x1f800000u) {
        region = board.regions[1]; size = board.sizes[1];
        if (address < 0x1f400000u) {
            offset = address - 0x1f000000u; end = 0x1f400000u;
        } else {
            offset = board.mapped_bank_offset + address - 0x1f400000u; end = 0x1f800000u;
        }
    } else if (address >= 0x1fb80000u && address < 0x1fc00000u) {
        region = board.regions[2]; size = board.sizes[2];
        offset = address - 0x1fb80000u; end = 0x1fc00000u;
    } else return 0;
    if (address == end - 4 || offset > size || size - offset < 4) return -1;
    *words = (const unsigned *)(region + offset);
    *limit = pc + end - address;
    return 1;
}
#endif

static const ZnProfile *find_profile(const char *set)
{
    size_t i;
    const ZnProfile *list = board.metro ? metro_profiles : board.jdredd ? jdredd_profiles : board.nbajam ? nbajam_profiles : board.primal ? primal_profiles : board.cbaj ? cbaj_profiles : board.atlus ? atlus_profiles : board.visco ? visco_profiles : board.raizing ? raizing_profiles : board.tps ? tps_profiles : profiles;
    size_t count = board.metro ? sizeof(metro_profiles)/sizeof(metro_profiles[0]) : board.jdredd ? sizeof(jdredd_profiles)/sizeof(jdredd_profiles[0]) : board.nbajam ? sizeof(nbajam_profiles)/sizeof(nbajam_profiles[0]) : board.primal ? sizeof(primal_profiles) / sizeof(primal_profiles[0]) : board.cbaj ? sizeof(cbaj_profiles) / sizeof(cbaj_profiles[0]) : board.atlus ? sizeof(atlus_profiles) / sizeof(atlus_profiles[0]) : board.visco ? sizeof(visco_profiles) / sizeof(visco_profiles[0]) : board.raizing ? sizeof(raizing_profiles) / sizeof(raizing_profiles[0]) : board.tps ? sizeof(tps_profiles) / sizeof(tps_profiles[0]) : sizeof(profiles) / sizeof(profiles[0]);
    for (i = 0; i < count; i++)
        if (!strcmp(list[i].set, set)) return &list[i];
    return NULL;
}

static int load_config(const char *directory, const ZnProfile **selected)
{
    static const char *keys[] = {
        "format", "mame_set", "mame_parent", "bios_parent", "bios", "board_model",
        "board_family", "cpu_device", "cpu_clock_hz", "gpu_device", "gpu_clock_hz",
        "main_ram_size", "vram_size", "board_execution_verified"
    };
    char path[4096], line[512], values[14][128] = {{0}};
    unsigned seen = 0, i;
    FILE *file;
    const ZnProfile *p;
    if (snprintf(path, sizeof(path), "%s/board.ini", directory) >= (int)sizeof(path)) return -1;
    file = fopen(path, "r");
    if (!file) { perror(path); return -1; }
    while (fgets(line, sizeof(line), file)) {
        char *equal, *end;
        if (!strchr(line, '\n') && !feof(file)) goto bad;
        end = line + strcspn(line, "\r\n"); *end = 0;
        if (!*line) continue;
        equal = strchr(line, '=');
        if (!equal || strchr(equal + 1, '=')) goto bad;
        *equal++ = 0;
        for (i = 0; i < 14 && strcmp(line, keys[i]); i++) { }
        if (i == 14 || (seen & (1u << i)) || !*equal || strlen(equal) >= sizeof(values[i])) goto bad;
        strcpy(values[i], equal); seen |= 1u << i;
    }
    if (ferror(file) || seen != 0x3fff || strcmp(values[0], board.metro ? "pcsx-aow-metro-zn-v1" : board.jdredd ? "pcsx-aow-jdredd-zn-v1" : board.nbajam ? "pcsx-aow-nbajam-zn-v1" : board.primal ? "pcsx-aow-primal-zn-v1" : board.cbaj ? "pcsx-aow-cbaj-tps-v1" : board.atlus ? "pcsx-aow-atlus-zn-v1" : board.visco ? "pcsx-aow-visco-zn-v1" : board.raizing ? "pcsx-aow-raizing-zn-v1" : board.tps ? "pcsx-aow-tecmo-tps-v1" : "pcsx-aow-capcom-zn-v1") ||
        strcmp(values[13], "false")) goto bad;
    p = find_profile(values[1]);
    if (!p || strcmp(values[2], p->parent) || strcmp(values[3], p->bios_parent) ||
        strcmp(values[5], p->model) || strcmp(values[6], p->family) ||
        strcmp(values[7], p->cpu_clock == 67737600 ? "CXD8530CQ" : "CXD8661R") ||
        strcmp(values[8], p->cpu_clock == 100000000u ? "100000000" : "67737600") ||
        strcmp(values[9], p->cpu_clock == 67737600 ? "CXD8561Q" : "CXD8654Q") ||
        strcmp(values[10], "53693175") || strcmp(values[11], p->ram_size == 0x800000 ? "8388608" : "4194304") ||
        strcmp(values[12], !strcmp(p->model, "COH1000C") ? "1048576" : "2097152")) goto bad;
    if (!strcmp(p->bios, "fixed")) {
        if (strcmp(values[4], "fixed")) goto bad;
    } else if (strcmp(values[4], "j") && strcmp(values[4], "e")) goto bad;
    *selected = p;
    fclose(file);
    return 0;
bad:
    fprintf(stderr, "%s: invalid or unsupported Sony ZN profile (exact 14-key board.ini required)\n", path);
    fclose(file);
    return -1;
}

static int load_region(const char *directory, const char *name, unsigned index, size_t expected)
{
    char path[4096];
    FILE *file;
    long length;
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path)) return -1;
    file = fopen(path, "rb");
    if (!file) { perror(path); return -1; }
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        (size_t)length != expected || fseek(file, 0, SEEK_SET)) {
        fprintf(stderr, "%s: expected exactly %zu bytes\n", path, expected);
        fclose(file); return -1;
    }
    board.regions[index] = (uint8_t *)malloc(expected ? expected : 1);
    if (!board.regions[index]) { fclose(file); return -1; }
    board.sizes[index] = expected;
    if (fread(board.regions[index], 1, expected, file) != expected) { fclose(file); return -1; }
    return fclose(file) ? -1 : 0;
}

static uint8_t input_read(void *opaque, zn_board_input_t input)
{
    (void)opaque;
    if(board.metro && (input==ZN_INPUT_P1 || input==ZN_INPUT_P2))return board.inputs[input]|((board.metro_dsw&1)?0x40:0);
    if(board.jdredd && input==ZN_INPUT_SERVICE)return (board.inputs[input]&~8u)|(board.judge_bus.gun_mux?0:8);
    return input >= ZN_INPUT_P1 && input <= ZN_INPUT_P4 ? board.inputs[input] : 0xff;
}
static uint32_t kick_read(void *opaque, unsigned player)
{
    (void)opaque;
    return player < 2 ? board.kicks[player] : UINT32_MAX;
}
static uint8_t mcu_analog(void *opaque) { (void)opaque; return 0xff; }
static uint16_t mcu_trackball(void *opaque) { (void)opaque; return 0; }
static uint8_t mcu_dsw(void *opaque) { (void)opaque; return board.metro?board.metro_dsw:0xff; }

static uint64_t cycles_to_us(uint64_t cycles)
{
    uint64_t whole = cycles / SYSTEMZN_PSX_CLOCK;
    uint64_t part = cycles % SYSTEMZN_PSX_CLOCK;
    if (whole > (UINT64_MAX / 1000000u)) return UINT64_MAX;
    return whole * 1000000u + part * 1000000u / SYSTEMZN_PSX_CLOCK;
}

static void update_sio0_rxd(void)
{
    if (board.sio[0])
        psx_sio_write_rxd(board.sio[0], board.cat_dataout[0] &&
            board.cat_dataout[1] && board.mcu_dataout);
}

static int sync_peripherals_to_cycle(uint64_t cycle)
{
    int result;
    uint64_t now_us;
    if (!board.active) return 0;
    if (cycle < board.peripheral_cycles) {
        board.faults |= 1u;
        return -1;
    }
    board.peripheral_cycles = cycle;
    now_us = cycles_to_us(cycle);
    if (now_us == UINT64_MAX || (result = zn_board_advance_to_us(&board.common, now_us)) != ZN_MCU_OK) {
        board.faults |= 1u;
        return -1;
    }
    return 0;
}

static void sio0_sck(void *opaque, int state)
{
    uint64_t cycle;
    (void)opaque;
    if (!board.active || !board.sio[0]) return;
    cycle = psx_sio_elapsed_cycles(board.sio[0]);
    if (sync_peripherals_to_cycle(cycle) || zn_board_sio_clock(&board.common, state))
        board.faults |= 1u;
}
static void sio0_txd(void *opaque, int state)
{
    uint64_t cycle;
    (void)opaque;
    if (!board.active || !board.sio[0]) return;
    cycle = psx_sio_elapsed_cycles(board.sio[0]);
    if (sync_peripherals_to_cycle(cycle)) { board.faults |= 1u; return; }
    zn_board_sio_tx(&board.common, state);
}
static void sio0_irq(void *opaque, int state)
{
    (void)opaque;
    board.sio_irq[0] = !!state;
    if (state && psxRegs.ptrs.psxH) psxHu32ref(0x1070) |= SWAPu32(0x80u);
}
static void sio1_irq(void *opaque, int state)
{
    (void)opaque;
    board.sio_irq[1] = !!state;
    if (state && psxRegs.ptrs.psxH) psxHu32ref(0x1070) |= SWAPu32(0x100u);
}
static void mcu_dsr(void *opaque, int state)
{
    (void)opaque;
    if (board.sio[0]) psx_sio_write_dsr(board.sio[0], state);
}
static void mcu_txd(void *opaque, int state)
{
    (void)opaque;
    board.mcu_dataout = !!state;
    update_sio0_rxd();
}
static void cat_dataout(void *opaque, unsigned chip, int state)
{
    (void)opaque;
    if (chip < 2) board.cat_dataout[chip] = !!state;
    update_sio0_rxd();
}

static void qsound_pcm(void *opaque, int16_t left, int16_t right, uint64_t frame)
{
    int16_t sample[4] = {left, right, 0, 0};
    unsigned l = left < 0 ? (unsigned)-(int)left : (unsigned)left;
    unsigned r = right < 0 ? (unsigned)-(int)right : (unsigned)right;
    (void)opaque;
    board.frames = frame + 1;
    if (l > board.peak) board.peak = l;
    if (r > board.peak) board.peak = r;
    if (pcm_wav_append4(&board.wav, sample, 1)) board.faults |= 2u;
    if (board.audio_callback) {
        unsigned index = board.audio_buffer_frames++ * 2;
        board.audio_buffer[index] = left; board.audio_buffer[index + 1] = right;
        if (board.audio_buffer_frames == 256) {
#ifdef SYSTEM12_BOARD_THREAD
            if (bt.active) zn_audio_push(board.audio_buffer, 256);
            else
#endif
                board.audio_callback(board.audio_opaque, board.audio_buffer, 256);
            board.audio_buffer_frames = 0;
        }
    }
}

static void raizing_pcm(void *opaque, const float outputs[4], uint64_t cycle)
{
    int16_t sample[4] = {0};
    (void)opaque;
    board.frames++;
    /* Source only routes chip outputs 0/1 to the stereo speaker. Keep float
     * values intact for the source mixer; standalone WAV uses final PCM scale. */
    for (unsigned ch = 0; ch < 2; ++ch) {
        float v = outputs[ch];
        if (!isfinite(v)) { board.faults |= 2; return; }
        if (v > 1) v = 1;
        if (v < -1) v = -1;
        sample[ch] = (int16_t)lrintf(v * 32767);
        unsigned peak = sample[ch] < 0 ? -(int)sample[ch] : sample[ch];
        if (peak > board.peak) board.peak = peak;
    }
    if (pcm_wav_append4(&board.wav, sample, 1)) board.faults |= 2;
    if (board.ymf_callback) board.ymf_callback(board.ymf_opaque, outputs[0], outputs[1], cycle);
}

static void atlus_pcm(void *opaque, float left, float right, uint64_t cycle)
{
    float outputs[4] = {left, right, 0, 0};
    raizing_pcm(opaque, outputs, cycle);
}

static void sio_bus_advance(psx_sio_t *sio, uint64_t cycle)
{
    if (sio && psx_sio_advance_to_cycle(sio, cycle) != PSX_SIO_OK)
        board.faults |= 4u;
}

static uint32_t tps_crc_byte(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    return crc;
}

static int validate_tps_media(void)
{
    size_t profile = (size_t)(board.profile - (board.metro ? metro_profiles : board.jdredd ? jdredd_profiles : board.nbajam ? nbajam_profiles : board.primal ? primal_profiles : board.cbaj ? cbaj_profiles : board.atlus ? atlus_profiles : board.visco ? visco_profiles : board.raizing ? raizing_profiles : tps_profiles));
    const TpsMediaCheck *checks = board.metro ? metro_media[profile] : board.jdredd ? jdredd_media[profile] : board.nbajam ? nbajam_media[profile] : board.primal ? primal_media[profile] : board.cbaj ? cbaj_media[profile] : board.atlus ? atlus_media[profile] : board.visco ? visco_media[profile] : board.raizing ? raizing_media[profile] : tps_media[profile];
    size_t count = board.metro ? metro_media_count[profile] : board.jdredd ? jdredd_media_count[profile] : board.nbajam ? nbajam_media_count[profile] : board.primal ? primal_media_count[profile] : board.cbaj ? cbaj_media_count[profile] : board.atlus ? atlus_media_count[profile] : board.visco ? visco_media_count[profile] : board.raizing ? raizing_media_count[profile] : tps_media_count[profile];
    static const unsigned regions[] = {0, 1, 5, 6, 2, 3, 4, 7};
    for (unsigned r = 0; r < 8; ++r) {
        unsigned region = regions[r];
        if (!board.sizes[region]) continue;
        uint8_t *marked = calloc(board.sizes[region], 1);
        if (!marked) return -1;
        if (board.atlus) for (size_t i = 0; i < sizeof(atlus_fills)/sizeof(atlus_fills[0]); ++i) {
            const ZnFillCheck *f = &atlus_fills[i];
            if (f->region != region) continue;
            if ((size_t)f->offset + f->length > board.sizes[region]) { free(marked); return -1; }
            for (size_t j = f->offset; j < (size_t)f->offset + f->length; ++j) {
                if (marked[j] || board.regions[region][j] != f->value) {
                    fprintf(stderr, "Sony ZN: source ROM_FILL mismatch region=%u offset=%zx\n", region, j);
                    free(marked); return -1;
                }
                marked[j] = 1;
            }
        }
        for (size_t i = 0; i < count; ++i) {
            const TpsMediaCheck *c = &checks[i];
            uint32_t crc = UINT32_MAX;
            if (c->region != region) continue;
            for (size_t j = 0; j < c->length; ++j) {
                size_t lane = j % c->group;
                size_t dst = c->offset + j / c->group * c->stride +
                    (c->swap ? c->group - 1 - lane : lane);
                if (dst >= board.sizes[region] || marked[dst]) { free(marked); return -1; }
                marked[dst] = 1;
                crc = tps_crc_byte(crc, board.regions[region][dst]);
            }
            if ((crc ^ UINT32_MAX) != c->crc) {
                fprintf(stderr, "Sony ZN: source ROM CRC mismatch region=%u offset=%x\n", region, c->offset);
                free(marked); return -1;
            }
        }
        for (size_t j = 0; j < board.sizes[region]; ++j)
            if (!marked[j] && board.regions[region][j]) {
                fprintf(stderr, "Sony ZN: source ROM padding mismatch at %zx\n", j);
                free(marked); return -1;
            }
        free(marked);
    }
    return 0;
}

#ifdef HAVE_CHD
static int open_metro_media(const char *directory)
{
    int cd=!strcmp(board.profile->set,"bam2a");
    const char *name=cd?"bam2cd.chd":"bam2.chd";
    const char *expected=cd?"cdedc8f9eb67bed267c899d679f986bf1748bd00":"634d9a745a82c567fc4d7ce48e3570d88326c5f9";
    char path[4096],digest[41];static const char hex[]="0123456789abcdef";
    if(snprintf(path,sizeof(path),"%s/%s",directory,name)>=(int)sizeof(path))return -1;
    if(chd_open(path,CHD_OPEN_READ,NULL,&board.metro_media)!=CHDERR_NONE)return -1;
    const chd_header *h=chd_get_header(board.metro_media);if(!h)return -1;
    for(unsigned i=0;i<20;i++){digest[2*i]=hex[h->sha1[i]>>4];digest[2*i+1]=hex[h->sha1[i]&15];}digest[40]=0;
    if(strcmp(digest,expected))return -1;
    fprintf(stderr,"Metro media: %s source SHA1 verified; H8 firmware NO_DUMP, ATA transport unimplemented\n",name);
    return 0;
}
static int primal_disk_read(void *opaque, uint32_t lba, void *data)
{
    (void)opaque; board.disk_reads++;
    int result = arcade_chd_hd_read(board.primal_disk, lba, data, 512);
    if (result) board.disk_errors++;
    return !result;
}
static int primal_disk_write(void *opaque, uint32_t lba, const void *data)
{
    (void)opaque; board.disk_writes++;
    int result = arcade_chd_hd_write(board.primal_disk, lba, data, 512);
    if (result) board.disk_errors++;
    return !result;
}
static void primal_irq10(void *opaque, int state)
{
    (void)opaque;
    if (state && psxRegs.ptrs.psxH) psxHu32ref(0x1070) |= SWAPu32(1u << 10);
}
static void primal_ata_irq(void *opaque, int state)
{
    (void)opaque;
    if(board.jdredd){
        if(state && !board.judge_ata_irq)board.judge_irq_edges++;
        board.judge_ata_irq=state!=0;primal_irq10(NULL,state);
    }else primal_zn_bus_ata_irq(board.primal_bus, state);
}
static void judge_lightgun(void *opaque,uint16_t x,uint16_t y){(void)opaque;gpu_set_lightgun_position(x,y);}
static int open_primal_disk(const char *directory)
{
    char path[4096], overlay[4096], error[256];
    if (snprintf(path,sizeof(path),board.jdredd?"%s/jdreddc.chd":"%s/primrag2.chd",directory)>=(int)sizeof(path) ||
        snprintf(overlay,sizeof(overlay),board.jdredd?"%s/jdreddc.overlay":"%s/primrag2.overlay",directory)>=(int)sizeof(overlay)) return -1;
    board.primal_disk = arcade_chd_hd_open(path,overlay,error,sizeof(error));
    if (!board.primal_disk) { fprintf(stderr,"ZN HDD: %s\n",error);return -1; }
    arcade_chd_hd_get_info(board.primal_disk,&board.primal_disk_info);
    static const uint8_t sha[20] = {0xbc,0x61,0x50,0x68,0xdd,0xf4,0xfd,0x96,0x7f,0x77,0x0e,0xe0,0x1c,0x02,0xf2,0x85,0xc0,0x52,0xc4,0xc5};
    static const uint8_t judge_sha[20]={0xee,0xe2,0x05,0xf8,0x3e,0x5f,0x59,0x0f,0x8b,0xaf,0x36,0x45,0x2c,0x87,0x3d,0x70,0x63,0x15,0x6b,0xd0};
    arcade_chd_hd_info_t *i = &board.primal_disk_info;
    if ((!board.synthetic && memcmp(i->sha1,board.jdredd?judge_sha:sha,20)) || i->sector_bytes!=512 || !i->cylinders || i->cylinders>65535 ||
        !i->heads || i->heads>255 || !i->sectors_per_track || i->sectors_per_track>255) {
        fprintf(stderr,"ZN HDD: source SHA1 or geometry mismatch\n");return -1;
    }
    uint8_t identify[512];size_t n=0;
    int meta=arcade_chd_hd_read_metadata(board.primal_disk,ARCADE_CHD_HD_METADATA_ATA_IDENTIFY,identify,sizeof(identify),&n);
    if (meta!=ARCADE_CHD_HD_METADATA_OK && meta!=ARCADE_CHD_HD_METADATA_MISSING) return -1;
    if (meta==ARCADE_CHD_HD_METADATA_OK && n!=512) return -1;
    ata_storage_config_t cfg={0};cfg.device_kind=ATA_STORAGE_DEVICE_IDE_HDD;
    cfg.sector_count=i->sector_count;cfg.cylinders=i->cylinders;cfg.heads=i->heads;cfg.sectors_per_track=i->sectors_per_track;
    cfg.read_sector=primal_disk_read;cfg.write_sector=primal_disk_write;cfg.irq=primal_ata_irq;
    if (meta==ARCADE_CHD_HD_METADATA_OK) {cfg.identify_data=identify;cfg.identify_size=512;}
    if (ata_storage_create(&cfg,&board.primal_ata)) return -1;
    if(board.jdredd)return jdredd_zn_bus_init(&board.judge_bus,board.primal_ata,board.regions[1],board.sizes[1],&board,judge_lightgun);
    PrimalZnBusConfig bus={board.primal_ata,&board,primal_irq10};
    return primal_zn_bus_create(&bus,&board.primal_bus);
}
#endif
static int nba_command(void *opaque,uint16_t word) { (void)opaque;return rax_sound_host_write(board.rax,word); }
static void nba_pcm(void *opaque,const float *v,size_t frames,unsigned rate,const uint64_t *ticks) {
    (void)opaque;if(board.rax_callback)board.rax_callback(board.rax_opaque,v,frames,rate,ticks);
}
static int load_board(const char *directory, int family, int synthetic)
{
    QSoundHLEConfig qcfg;
    QSoundHLEStatus qstatus;
    capcom_zn_callbacks_t cap_callbacks;
    capcom_zn_config_t cap_config;
    ZNMCUConfig mcu_config;
    zn_board_config_t common_config;
    zn_board_callbacks_t common_callbacks;
    psx_sio_callbacks_t sio_callbacks, sio1_callbacks;
    uint8_t keys[2][8];
    unsigned i;
    static const char *files[] = {"main.bin", "banked.bin", "country.bin", "audiocpu.bin",
        "qsound_samples.bin", "cat702_key0.bin", "cat702_key1.bin"};
    if (board.active || systemtaito_active() || system10_active() || system11_active() || system12_active() ||
        psxRegs.ptrs.psxM || !directory) return -1;
    board.tps = family == 1;
    board.raizing = family == 2;
    board.visco = family == 3;
    board.atlus = family == 4;
    board.cbaj = family == 5;
    board.primal = family == 6;
    board.nbajam = family == 7;
    board.jdredd = family == 8;
    board.metro = family == 9;board.metro_dsw=0xfe;
    board.synthetic = synthetic;
    if (load_config(directory, &board.profile)) { memset(&board, 0, sizeof(board)); return -1; }
    if (load_region(directory, files[0], 0, board.profile->bios_size) ||
        load_region(directory, board.primal || board.jdredd ? "fixed.bin" : files[1], 1, board.profile->banked_size) ||
        load_region(directory, files[5], 5, 8) || load_region(directory, files[6], 6, 8)) goto bad;
    if(board.metro){
        if(!synthetic && validate_tps_media())goto bad;
        if(metro_zn_bus_init(&board.metro_bus,board.regions[1],board.sizes[1]))goto bad;
#ifdef HAVE_CHD
        if(!synthetic && open_metro_media(directory))goto bad;
#else
        fprintf(stderr,"Metro ZN requires HAVE_CHD=1\n");goto bad;
#endif
    }else if(board.jdredd){
        if(!synthetic && validate_tps_media())goto bad;
#ifdef HAVE_CHD
        if(open_primal_disk(directory))goto bad;
#else
        fprintf(stderr,"Judge Dredd requires HAVE_CHD=1\n");goto bad;
#endif
    }else if (board.nbajam) {
        if(load_region(directory,"rax.bin",3,board.profile->audio_size) || (!synthetic && validate_tps_media()))goto bad;
        RaxSoundConfig cfg={board.regions[3],board.sizes[3],&board,nba_pcm};
        if(rax_sound_create(&cfg,&board.rax) || nbajam_zn_bus_init(&board.nba_bus,board.regions[1],board.sizes[1],&board,nba_command))goto bad;
    } else if (board.primal) {
        if (load_region(directory,"eeprom_default.bin",2,board.profile->country_size) ||
            (!synthetic && validate_tps_media())) goto bad;
#ifdef HAVE_CHD
        if (open_primal_disk(directory)) goto bad;
#else
        fprintf(stderr,"Primal Rage II requires HAVE_CHD=1\n");goto bad;
#endif
    } else if (board.cbaj) {
        if (load_region(directory, "audiocpu.bin", 3, board.profile->audio_size) ||
            load_region(directory, "ymz_samples.bin", 4, board.profile->qsound_size) ||
            load_region(directory, "misc.bin", 7, 0xc00)) goto bad;
        if (!synthetic && validate_tps_media()) goto bad;
        CbajSoundConfig cfg = {board.regions[3], board.regions[4], board.sizes[3], board.sizes[4], &board, atlus_pcm};
        if (cbaj_sound_create(&cfg, &board.cbaj_sound)) goto bad;
    } else if (board.atlus) {
        if (load_region(directory, "audiocpu.bin", 3, board.profile->audio_size) ||
            load_region(directory, "ymz_samples.bin", 4, board.profile->qsound_size)) goto bad;
        if (!synthetic && validate_tps_media()) goto bad;
        AtlusSoundConfig cfg = {board.regions[3], board.regions[4], board.sizes[3], board.sizes[4], &board, atlus_pcm};
        if (atlus_sound_create(&cfg, &board.atlus_sound)) goto bad;
    } else if (board.visco) {
        if (load_region(directory, "fixed.bin", 2, board.profile->country_size)) goto bad;
        if (!synthetic && validate_tps_media()) goto bad;
    } else if (board.raizing) {
        if (load_region(directory, "eeprom_default.bin", 2, board.profile->country_size) ||
            load_region(directory, "audiocpu.bin", 3, board.profile->audio_size) ||
            load_region(directory, "ymf_samples.bin", 4, board.profile->qsound_size)) goto bad;
        if (!synthetic && validate_tps_media()) goto bad;
        RaizingSoundConfig cfg = {board.regions[3], board.regions[4], board.sizes[3],
            board.sizes[4], &board, raizing_pcm};
        if (raizing_sound_create(&cfg, &board.raizing_sound)) goto bad;
    } else if (board.tps) {
        if (board.profile->audio_size &&
            load_region(directory, "linkcpu.bin", 3, board.profile->audio_size)) goto bad;
        if (!synthetic && validate_tps_media()) goto bad;
        if (board.profile->audio_size &&
            tecmo_link_create(board.regions[3], board.sizes[3], &board.tecmo_link)) goto bad;
    } else {
        if (load_region(directory, files[2], 2, board.profile->country_size) ||
            load_region(directory, files[3], 3, board.profile->audio_size) ||
            load_region(directory, files[4], 4, board.profile->qsound_size)) goto bad;
        if (load_region(directory, "qsound_dsp.bin", 7, 0x2000)) goto bad;
        board.qsound_words = (uint16_t *)malloc(0x1000 * sizeof(uint16_t));
        if (!board.qsound_words) goto bad;
        for (i = 0; i < 0x1000; i++)
            board.qsound_words[i] = (uint16_t)(board.regions[7][i * 2] |
                ((uint16_t)board.regions[7][i * 2 + 1] << 8));
        memset(&qcfg, 0, sizeof(qcfg));
        qcfg.firmware = board.qsound_words;
        qcfg.firmware_words = 0x1000;
        qcfg.samples = board.regions[4];
        qcfg.sample_bytes = (uint32_t)board.sizes[4];
        qcfg.clock_hz = 60000000;
        qcfg.pcm = qsound_pcm;
        qcfg.opaque = &board;
        board.qsound = qsound_hle_create(&qcfg);
        if (!board.qsound || qsound_hle_get_status(board.qsound, &qstatus) ||
            qstatus.sample_rate != SYSTEMZN_QSOUND_RATE) goto bad;

        memset(&cap_callbacks, 0, sizeof(cap_callbacks));
        cap_callbacks.opaque = &board;
        cap_callbacks.read_kick = kick_read;
        memset(&cap_config, 0, sizeof(cap_config));
        cap_config.main_rom = board.regions[1]; cap_config.main_rom_size = board.sizes[1];
        cap_config.country_rom = board.regions[2]; cap_config.country_rom_size = board.sizes[2];
        cap_config.sound_rom = board.regions[3]; cap_config.sound_rom_size = board.sizes[3];
        cap_config.qsound_hle = board.qsound;
        board.capcom = capcom_zn_create(&cap_config, &cap_callbacks);
        if (!board.capcom) goto bad;
        capcom_zn_reset(board.capcom);

    }

    memset(&sio_callbacks, 0, sizeof(sio_callbacks));
    sio_callbacks.opaque = &board;
    sio_callbacks.sck = sio0_sck;
    sio_callbacks.txd = sio0_txd;
    sio_callbacks.irq = sio0_irq;
    memset(&sio1_callbacks, 0, sizeof(sio1_callbacks));
    sio1_callbacks.opaque = &board; sio1_callbacks.irq = sio1_irq;
    if (psx_sio_create(PSX_SIO0, &sio_callbacks, &board.sio[0]) != PSX_SIO_OK ||
        psx_sio_create(PSX_SIO1, &sio1_callbacks, &board.sio[1]) != PSX_SIO_OK) goto bad;
    psx_sio_write_rxd(board.sio[0], 1);

    memset(&mcu_config, 0, sizeof(mcu_config));
    for (i = 0; i < 8; i++) mcu_config.analog[i] = mcu_analog;
    for (i = 0; i < 4; i++) mcu_config.trackball[i] = mcu_trackball;
    mcu_config.dsw = mcu_dsw;
    mcu_config.dsr = mcu_dsr;
    mcu_config.txd = mcu_txd;
    mcu_config.opaque = &board;
    if (zn_mcu_create(&mcu_config, &board.mcu) != ZN_MCU_OK) goto bad;

    memset(&common_config, 0, sizeof(common_config));
    common_config.model = board.profile->cpu_clock == 67737600 ? ZN_BOARD_ZN1 : ZN_BOARD_ZN2;
    common_config.ram_bytes = board.profile->ram_size;
    common_config.vram_bytes = board.profile->vram_size;
    common_config.cat702_count = 2;
    memcpy(keys[0], board.regions[5], 8); memcpy(keys[1], board.regions[6], 8);
    common_config.cat702_keys = (const uint8_t (*)[8])keys;
    common_config.mcu = board.mcu;
    if (board.raizing || board.primal) {
        common_config.eeprom = board.regions[2]; common_config.eeprom_size = board.sizes[2];
    }
    memset(&common_callbacks, 0, sizeof(common_callbacks));
    common_callbacks.opaque = &board;
    common_callbacks.read_input = input_read;
    common_callbacks.cat702_dataout = cat_dataout;
    if (!zn_board_init(&board.common, &common_config, &common_callbacks)) goto bad;
    board.cat_dataout[0] = board.cat_dataout[1] = board.mcu_dataout = 1;
    board.active = 1;
    update_sio0_rxd();
    memset(board.inputs, 0xff, sizeof(board.inputs));
    board.kicks[0] = board.kicks[1] = UINT32_MAX;
    if (gpu_set_model(2) || gpu_set_vram_height(board.profile->vram_size == 0x100000 ? 512 : 1024)) goto bad;
    psxRamSize = board.profile->ram_size;
    psxRomSize = board.profile->bios_size;
    Config.HLE = FALSE;
    board.last_psx_cycle = psxRegs.cycle;
    if (systemzn_sync(psxRegs.cycle) < 0) goto bad;
#ifndef DRC_DISABLE
    if (board.capcom) new_dynarec_set_code_span_provider(systemzn_code_span);
#endif
    return 0;
bad:
    fprintf(stderr, "SystemZN: media/profile initialization failed\n");
    systemzn_unload();
    return -1;
}

int systemzn_load_metro(const char *directory){return load_board(directory,9,0);}
int systemzn_test_load_metro(const char *directory)
{
    static const uint32_t guest[]={
        0x3c0a8000u, 0x3c08bf00u, 0x8d090000u, 0x00000000u, 0xad490100u, 0x3c08bf40u,
        0x8d090000u, 0x00000000u, 0xad490104u, 0x3c08bfb0u, 0x2409000au, 0xa5090000u,
        0x3c08bf40u, 0x8d090000u, 0x00000000u, 0xad490108u, 0x3c08bfb0u, 0x2409007fu,
        0xa5090002u, 0x95090004u, 0x00000000u, 0xad49010cu, 0x2409001cu, 0xa5090002u,
        0x95090004u, 0x00000000u, 0xad490110u, 0x24090000u, 0xa5090002u, 0x95090004u,
        0x00000000u, 0xad490114u, 0x3c08bfa0u, 0x91090000u, 0x00000000u, 0xad490118u,
        0x91090100u, 0x00000000u, 0xad49011cu, 0x91090300u, 0x00000000u, 0xad490120u,
        0x3c094d45u, 0x35295452u, 0xad490124u, 0x1000ffffu, 0x00000000u,
    };
    if(load_board(directory,9,1))return -1;
    memset(board.regions[0],0,board.sizes[0]);
    for(unsigned i=0;i<sizeof(guest)/sizeof(guest[0]);i++){
        uint32_t word=SWAPu32(guest[i]);memcpy(board.regions[0]+i*4,&word,4);
    }
    return 0;
}
int systemzn_metro_active(void){return board.active && board.metro;}
int systemzn_metro_generic(void){return systemzn_metro_active() && (board.metro_dsw&1);}
int systemzn_load(const char *directory) { return load_board(directory, 0, 0); }
int systemzn_load_tps(const char *directory) { return load_board(directory, 1, 0); }
int systemzn_test_load_tps(const char *directory) { return load_board(directory, 1, 1); }
int systemzn_load_raizing(const char *directory) { return load_board(directory, 2, 0); }
int systemzn_test_load_raizing(const char *directory) { return load_board(directory, 2, 1); }
int systemzn_raizing_active(void) { return board.active && board.raizing; }
int systemzn_load_visco(const char *directory) { return load_board(directory, 3, 0); }
int systemzn_test_load_visco(const char *directory) { return load_board(directory, 3, 1); }
int systemzn_load_atlus(const char *directory) { return load_board(directory, 4, 0); }
int systemzn_test_load_atlus(const char *directory) { return load_board(directory, 4, 1); }
int systemzn_atlus_active(void) { return board.active && board.atlus; }
int systemzn_load_cbaj(const char *directory) { return load_board(directory, 5, 0); }
int systemzn_jdredd_active(void){return board.active && board.jdredd;}
int systemzn_load_jdredd(const char *directory){return load_board(directory,8,0);}
int systemzn_test_load_jdredd(const char *directory){return load_board(directory,8,1);}
void systemzn_vblank(int state){if(systemzn_jdredd_active())jdredd_zn_bus_vblank(&board.judge_bus,state);}
int systemzn_set_gun_position(unsigned p,unsigned x,unsigned y){
    if(!systemzn_jdredd_active() || p>1 || x>65535 || y>65535)return -1;
    return jdredd_zn_bus_gun(&board.judge_bus,p,x,y);
}
int systemzn_set_gun_axis(const char *name,unsigned value){
    if(!systemzn_jdredd_active() || !name || strlen(name)!=6 || name[0]!='p' || (name[1]!='1' && name[1]!='2') || strncmp(name+2,"gun",3) || (name[5]!='x' && name[5]!='y') || value>65535)return -1;
    unsigned p=name[1]-'1';JdreddZnBus *b=&board.judge_bus;
    return jdredd_zn_bus_gun(b,p,name[5]=='x'?value:b->gun_x[p],name[5]=='y'?value:b->gun_y[p]);
}
int systemzn_nbajam_active(void){return board.active && board.nbajam;}
int systemzn_load_nbajam(const char *directory){return load_board(directory,7,0);}
int systemzn_test_load_nbajam(const char *directory){return load_board(directory,7,1);}
int systemzn_load_primal(const char *directory) { return load_board(directory, 6, 0); }
int systemzn_test_load_primal(const char *directory) { return load_board(directory, 6, 1); }
int systemzn_primal_active(void) { return board.active && board.primal; }
int systemzn_test_load_cbaj(const char *directory) { return load_board(directory, 5, 1); }
int systemzn_cbaj_active(void) { return board.active && board.cbaj; }
int systemzn_visco_active(void) { return board.active && board.visco; }
unsigned systemzn_rotation(void) { return board.active && (board.visco || (board.raizing && !strcmp(board.profile->set, "brvblade"))) ? 270 : 0; }
int systemzn_tps_active(void) { return board.active && board.tps; }

void systemzn_unload(void)
{
    unsigned i;
    systemzn_board_thread_stop();
#ifndef DRC_DISABLE
    if (board.capcom) new_dynarec_set_code_span_provider(NULL);
#endif
    /* Executable LUT pages borrow the region buffers until unload. */
    static const uint32_t bases[] = {0x1f000000u, 0x9f000000u, 0xbf000000u};
    if (board.active && psxRegs.ptrs.memRLUT && psxRegs.ptrs.memWLUT) {
        unsigned segment;
        for (segment = 0; segment < 3; segment++) {
            uint32_t page;
            for (page = 0; page < 0x800000u; page += SYSTEMZN_ROM_PAGE) {
                unsigned index = (bases[segment] + page) >> PSXM_SHIFT;
                psxRegs.ptrs.memRLUT[index] = INVALID_PTR_VAL;
                psxRegs.ptrs.memWLUT[index] = INVALID_PTR_VAL;
            }
            for (page = board.visco ? 0xb00000u : 0xb80000u; page < 0xc00000u; page += SYSTEMZN_ROM_PAGE) {
                unsigned index = (bases[segment] + page) >> PSXM_SHIFT;
                psxRegs.ptrs.memRLUT[index] = INVALID_PTR_VAL;
                psxRegs.ptrs.memWLUT[index] = INVALID_PTR_VAL;
            }
        }
    }
    pcm_wav_close(&board.wav);
    primal_zn_bus_destroy(board.primal_bus);
    board.primal_bus=NULL;
    ata_storage_destroy(board.primal_ata);
#ifdef HAVE_CHD
    if(board.metro_media)chd_close(board.metro_media);
    if (board.primal_disk && arcade_chd_hd_flush(board.primal_disk)) fprintf(stderr,"Primal HDD overlay flush failed\n");
    arcade_chd_hd_close(board.primal_disk);
#endif
    tecmo_link_destroy(board.tecmo_link);
    rax_sound_destroy(board.rax);
    cbaj_sound_destroy(board.cbaj_sound);
    atlus_sound_destroy(board.atlus_sound);
    raizing_sound_destroy(board.raizing_sound);
    capcom_zn_destroy(board.capcom);
    qsound_hle_destroy(board.qsound);
    zn_board_destroy(&board.common);
    zn_mcu_destroy(board.mcu);
    psx_sio_destroy(board.sio[0]); psx_sio_destroy(board.sio[1]);
    for (i = 0; i < 8; i++) free(board.regions[i]);
    free(board.qsound_words);
    memset(&board, 0, sizeof(board));
    if (!psxRegs.ptrs.psxM && !systemtaito_active() && !system10_active() && !system11_active() && !system12_active()) {
        psxRamSize = 0x200000; psxRomSize = 0x80000;
        gpu_set_model(2); gpu_set_vram_height(512);
    }
}

int systemzn_active(void) { return board.active; }
unsigned int systemzn_main_clock_hz(void)
{
    return board.active && board.profile ? board.profile->cpu_clock : 0;
}

static void map_window(uint32_t base, const uint8_t *data, size_t size, int writable)
{
    size_t i;
    if (!psxRegs.ptrs.memRLUT || !psxRegs.ptrs.memWLUT) return;
    for (i = 0; i < size; i += SYSTEMZN_ROM_PAGE) {
        uint32_t address = base + (uint32_t)i;
        uintptr_t offset = (uintptr_t)data - (uintptr_t)base;
        psxRegs.ptrs.memRLUT[address >> PSXM_SHIFT] = offset;
        psxRegs.ptrs.memWLUT[address >> PSXM_SHIFT] = writable ? offset : INVALID_PTR_VAL;
    }
}
static void map_bankedroms(void)
{
    static const uint32_t bank_aliases[] = {0x1f400000u, 0x9f400000u, 0xbf400000u};
    static const uint32_t fixed_aliases[] = {0x1f000000u, 0x9f000000u, 0xbf000000u};
    capcom_zn_status_t status;
    unsigned a;
    if (!board.active || !psxRegs.ptrs.memRLUT || !psxRegs.ptrs.memWLUT) return;
    if(board.metro){
        for(a=0;a<3;a++)for(size_t off=0;off<0x800000;off+=SYSTEMZN_ROM_PAGE){
            uint32_t addr=fixed_aliases[a]+off;unsigned idx=addr>>PSXM_SHIFT;
            const uint8_t *rom=metro_zn_bus_rom_span(&board.metro_bus,0x1f000000+off,SYSTEMZN_ROM_PAGE);
            psxRegs.ptrs.memRLUT[idx]=rom?(uintptr_t)rom-(uintptr_t)addr:INVALID_PTR_VAL;
            psxRegs.ptrs.memWLUT[idx]=INVALID_PTR_VAL;
        }
        if(psxCpu && psxCpu->Clear)for(a=0;a<3;a++)psxCpu->Clear(bank_aliases[a],0x400000/4);
        return;
    }
    if(board.nbajam){
        for(a=0;a<3;a++)for(size_t off=0;off<0x800000;off+=SYSTEMZN_ROM_PAGE){
            uint32_t addr=fixed_aliases[a]+off;unsigned idx=addr>>PSXM_SHIFT;
            const uint8_t *rom=nbajam_zn_bus_rom_span(&board.nba_bus,0x1f000000+off,SYSTEMZN_ROM_PAGE);
            psxRegs.ptrs.memRLUT[idx]=rom?(uintptr_t)rom-(uintptr_t)addr:INVALID_PTR_VAL;
            psxRegs.ptrs.memWLUT[idx]=INVALID_PTR_VAL;
        }
        if(psxCpu && psxCpu->Clear)for(a=0;a<3;a++)psxCpu->Clear(fixed_aliases[a],0x800000/4);
        return;
    }
    if (board.primal || board.jdredd) {
        for (a=0;a<3;a++) map_window(fixed_aliases[a],board.regions[1],board.sizes[1],0);
        return;
    }
    if (board.visco) {
        size_t offset = (size_t)board.tps_bank * 0x100000;
        for (a = 0; a < 3; ++a) {
            map_window(fixed_aliases[a], board.regions[2], board.sizes[2], 0);
            uint32_t base = fixed_aliases[a] + 0xb00000;
            if (board.tps_bank < 24)
                map_window(base, board.regions[1] + offset, 0x100000, 0);
            else for (size_t i = 0; i < 0x100000; i += SYSTEMZN_ROM_PAGE) {
                unsigned index = (base + i) >> PSXM_SHIFT;
                psxRegs.ptrs.memRLUT[index] = psxRegs.ptrs.memWLUT[index] = INVALID_PTR_VAL;
            }
            if (psxCpu && psxCpu->Clear) psxCpu->Clear(base, 0x100000 / 4);
        }
        return;
    }
    if (board.tps || board.raizing || board.atlus || board.cbaj) {
        size_t offset = (size_t)board.tps_bank * 0x800000;
        for (a = 0; a < 3; ++a) {
            if (board.tps_bank < 9 && offset <= board.sizes[1] && 0x800000 <= board.sizes[1] - offset)
                map_window(fixed_aliases[a], board.regions[1] + offset, 0x800000, 0);
            else for (size_t i = 0; i < 0x800000; i += SYSTEMZN_ROM_PAGE) {
                unsigned index = (fixed_aliases[a] + i) >> PSXM_SHIFT;
                psxRegs.ptrs.memRLUT[index] = psxRegs.ptrs.memWLUT[index] = INVALID_PTR_VAL;
            }
            if (psxCpu && psxCpu->Clear) psxCpu->Clear(fixed_aliases[a], 0x800000 / 4);
        }
        return;
    }
    zn_drain();
    capcom_zn_get_status(board.capcom, &status);
#ifndef DRC_DISABLE
    if (psxCpu == &psxRec && board.mapped_bank_offset != status.main_bank_offset)
        new_dynarec_invalidate_range_hard(0x1f400000u, 0x1f800000u);
#endif
    board.mapped_bank_offset = status.main_bank_offset;
    for (a = 0; a < 3; a++)
        map_window(fixed_aliases[a], board.regions[1], CAPCOM_ZN_MAIN_FIXED_SIZE, 0);
    for (a = 0; a < 3; a++) {
        uint32_t address = bank_aliases[a];
        size_t i;
        size_t offset = status.main_bank_offset;
        if (offset > board.sizes[1] || CAPCOM_ZN_MAIN_BANK_SIZE > board.sizes[1] - offset) {
            for (i = 0; i < CAPCOM_ZN_MAIN_BANK_SIZE; i += SYSTEMZN_ROM_PAGE) {
                size_t idx = (address + (uint32_t)i) >> PSXM_SHIFT;
                psxRegs.ptrs.memRLUT[idx] = INVALID_PTR_VAL;
                psxRegs.ptrs.memWLUT[idx] = INVALID_PTR_VAL;
            }
        } else {
            map_window(address, board.regions[1] + offset, CAPCOM_ZN_MAIN_BANK_SIZE, 0);
        }
    }
}

void systemzn_mem_reset(void)
{
    zn_drain();
    if (!board.active || !psxRegs.ptrs.psxM) return;
    memcpy(psxRegs.ptrs.psxR, board.regions[0], board.sizes[0]);
    if ((!board.primal && !board.nbajam && !board.jdredd && !board.metro) || !board.memory_initialized) memset(psxRegs.ptrs.psxM, 0, psxRamSize);
    board.memory_initialized=1;
    memset(psxRegs.ptrs.psxP, 0xff, 0x10000);
    map_bankedroms();
    if (board.raizing || board.visco || board.atlus || board.cbaj || board.primal || board.nbajam || board.jdredd || board.metro) return;
    map_window(0x1fb80000u, board.regions[2], board.sizes[2], 0);
    map_window(0x9fb80000u, board.regions[2], board.sizes[2], 0);
    map_window(0xbfb80000u, board.regions[2], board.sizes[2], 0);
}

void systemzn_reset(void)
{
    if (!board.active) return;
    zn_flush_audio();
    /* MAME has no device_reset override for the shared ZN MCU, CAT702, AT28C16
     * or PSX SIO devices; preserve their pin/timer/storage state on soft reset. */
    board.faults = 0;
    if (board.tps || board.raizing || board.visco || board.atlus || board.cbaj || board.primal || board.nbajam || board.jdredd || board.metro) board.tps_bank = 0;
    else capcom_zn_reset(board.capcom);
    if(board.nbajam){
        nbajam_zn_bus_reset(&board.nba_bus);
        if(rax_sound_reset(board.rax))board.faults|=8;
        RaxSoundStatus status;rax_sound_status(board.rax,&status);board.sound_cycles=status.cpu.cycles;
    }
    if (board.raizing) {
        if (raizing_sound_reset(board.raizing_sound)) board.faults |= 8;
        RaizingSoundStatus status; raizing_sound_status(board.raizing_sound, &status);
        board.sound_cycles = status.cpu_cycles;
    }
    if (board.atlus) {
        if (atlus_sound_reset(board.atlus_sound)) board.faults |= 8;
        AtlusSoundStatus status; atlus_sound_status(board.atlus_sound, &status); board.sound_cycles = status.cpu_cycles;
    }
    if (board.cbaj) {
        if (cbaj_sound_reset(board.cbaj_sound)) board.faults |= 8;
        CbajSoundStatus status; cbaj_sound_status(board.cbaj_sound, &status); board.sound_cycles = status.cpu_cycles;
    }
    if (board.tecmo_link) {
        if (tecmo_link_reset(board.tecmo_link)) board.faults |= 8;
        TecmoLinkStatus status; tecmo_link_status(board.tecmo_link, &status); board.sound_cycles = status.cycles;
    }
    board.qsound_fraction = 0; if (!board.raizing && !board.atlus && !board.cbaj && !board.nbajam && !board.jdredd && !board.metro && !board.tecmo_link) board.sound_cycles = 0;
    board.frames = 0;
    board.last_psx_cycle = psxRegs.cycle;
    if (board.primal) primal_zn_bus_reset(board.primal_bus);
    if(board.jdredd)jdredd_zn_bus_reset(&board.judge_bus);
    if(board.metro)metro_zn_bus_reset(&board.metro_bus);
    map_bankedroms();
}

int systemzn_sync(u32 psx_cycle)
{
    uint32_t elapsed;
    uint64_t sound_cycles;
    QSoundHLEStatus status;
    if (!board.active) return 0;
    elapsed = psx_cycle - board.last_psx_cycle;
    board.last_psx_cycle = psx_cycle;
    if (elapsed) {
        uint64_t numerator;
        if (board.primal || board.jdredd) {
            numerator=(uint64_t)elapsed*1000000000u+board.primal_fraction;
            board.primal_ns+=numerator/PSXCLK;board.primal_fraction=numerator%PSXCLK;
            if(board.jdredd){if(ata_storage_run_until(board.primal_ata,board.primal_ns))board.faults|=8;}
            else if (primal_zn_bus_sync(board.primal_bus,board.primal_ns)<0)board.faults|=8;
        }
        sound_cycles = 0;
        if ((!board.tps && !board.visco && !board.primal && !board.jdredd && !board.metro) || board.tecmo_link) {
            numerator = (uint64_t)elapsed * (board.nbajam ? RAX_SOUND_CLOCK : board.tecmo_link ? TECMO_LINK_CLOCK : board.cbaj ? CBAJ_SOUND_CLOCK : board.atlus ? ATLUS_SOUND_CLOCK : board.raizing ? RAIZING_SOUND_CLOCK : CAPCOM_ZN_SOUND_CLOCK_HZ) + board.qsound_fraction;
            sound_cycles = numerator / PSXCLK;
            board.qsound_fraction = numerator % PSXCLK;
        }
        /* SIO bit timers use the source's fixed 33.8688 MHz clock even on
         * ZN-2. MCU/EEPROM callbacks share that absolute time domain. */
        numerator = (uint64_t)elapsed * SYSTEMZN_PSX_CLOCK + board.sio_fraction;
        board.sio_cycles += numerator / PSXCLK;
        board.sio_fraction = numerator % PSXCLK;
        board.sound_cycles += sound_cycles;
        if(board.nbajam && rax_sound_run(board.rax,sound_cycles))board.faults|=8;
        if (board.tecmo_link && tecmo_link_sync(board.tecmo_link, board.sound_cycles)) board.faults |= 8;
        if (board.cbaj && cbaj_sound_sync(board.cbaj_sound, board.sound_cycles)) board.faults |= 8;
        if (board.atlus && atlus_sound_sync(board.atlus_sound, board.sound_cycles)) board.faults |= 8;
        if (board.raizing && raizing_sound_sync(board.raizing_sound, board.sound_cycles)) board.faults |= 8u;
        if (!board.tps && !board.raizing && !board.visco && !board.atlus && !board.cbaj && !board.primal && !board.nbajam && !board.jdredd && !board.metro && sound_cycles)
            zn_run_sound(sound_cycles);
    }
    sio_bus_advance(board.sio[0], board.sio_cycles);
    sio_bus_advance(board.sio[1], board.sio_cycles);
    if (sync_peripherals_to_cycle(board.sio_cycles)) board.faults |= 1u;
    if (!board.tps && !board.raizing && !board.visco && !board.atlus && !board.cbaj && !board.primal && !board.nbajam && !board.jdredd && !board.metro && !systemzn_board_thread_active() && qsound_hle_get_status(board.qsound, &status)) board.faults |= 2u;
    if (!board.tps && !board.raizing && !board.visco && !board.atlus && !board.cbaj && !board.primal && !board.nbajam && !board.jdredd && !board.metro && !systemzn_board_thread_active()) {
        capcom_zn_status_t cap_status;
        capcom_zn_get_status(board.capcom, &cap_status);
        if (cap_status.faults) board.faults |= 8u;
    }
    PrimalZnBusStatus primal={0};
    if (board.primal) primal_zn_bus_status(board.primal_bus,&primal);
    return board.faults ? -1 : primal.reset_pending ? 1 : 0;
}

int systemzn_primal_dma5(uint32_t madr,uint32_t bcr,uint32_t chcr)
{
    if(!systemzn_primal_active() || systemzn_sync(psxRegs.cycle)<0)return -1;
    uint64_t words=bcr;
    if(chcr&0x200){uint64_t blocks=bcr>>16;if(!blocks)blocks=65536;words=blocks*(bcr&65535);}
    int read=chcr==0x01000000 || chcr==0x01000200 || (chcr&0xffbffeff)==0x11000000;
    int write=chcr==0x01000201 || chcr==0x11050100 || chcr==0x11150100;
    if((chcr&0xffbffeff)==0x11000000){uint64_t blocks=bcr>>16;if(!blocks)blocks=1;words=blocks*(bcr&65535);}
    if(!read && !write)return 1;
    if(words>INT32_MAX || (read && primal_zn_bus_dma_read(board.primal_bus,psxRegs.ptrs.psxM,PSX_RAM_SIZE,madr&PSX_RAM_MASK,words))){board.faults|=8;return -1;}
    if(write)primal_zn_bus_dma_write(board.primal_bus);
    return 0;
}

static int handle_sio_read(uint32_t physical, unsigned width, uint32_t *value)
{
    uint32_t base, data, offset, lane, mask;
    psx_sio_t *sio;
    if (physical >= 0x1f801040 && physical < 0x1f801050) base = 0x1f801040;
    else if (physical >= 0x1f801050 && physical < 0x1f801060) base = 0x1f801050;
    else return 0;
    if ((width != 1 && width != 2 && width != 4) || (physical & (width - 1)) ||
        (uint64_t)physical + width > (uint64_t)base + 0x10) return -1;
    if (systemzn_sync(psxRegs.cycle) < 0) return -1;
    sio = board.sio[(base == 0x1f801040) ? 0 : 1];
    offset = (physical - base) >> 2;
    lane = (physical & 3u) * 8;
    mask = width == 4 ? UINT32_MAX : (1u << (width * 8)) - 1;
    if (psx_sio_read(sio, offset, UINT32_MAX, &data) != PSX_SIO_OK) return -1;
    *value = (data >> lane) & mask;
    return 1;
}

static int handle_sio_write(uint32_t physical, unsigned width, uint32_t value)
{
    uint32_t base, offset, lane, mask;
    psx_sio_t *sio;
    if (physical >= 0x1f801040 && physical < 0x1f801050) base = 0x1f801040;
    else if (physical >= 0x1f801050 && physical < 0x1f801060) base = 0x1f801050;
    else return 0;
    if ((width != 1 && width != 2 && width != 4) || (physical & (width - 1)) ||
        (uint64_t)physical + width > (uint64_t)base + 0x10) return -1;
    if (systemzn_sync(psxRegs.cycle) < 0) return -1;
    sio = board.sio[(base == 0x1f801040) ? 0 : 1];
    offset = (physical - base) >> 2;
    lane = (physical & 3u) * 8;
    mask = width == 4 ? UINT32_MAX : ((1u << (width * 8)) - 1) << lane;
    if (psx_sio_write(sio, offset, value << lane, mask) != PSX_SIO_OK) return -1;
    return 1;
}

int systemzn_read(u32 address, unsigned width, u32 *value)
{
    uint32_t physical = canonical_address(address);
    int result;
    if (!board.active || !value) return 0;
    if(board.metro){
        result=metro_zn_bus_read(&board.metro_bus,physical,width,value);
        if(result){if(result<0){board.faults|=8;*value=UINT32_MAX;}return 1;}
    }
    if(board.jdredd){
        if(systemzn_sync(psxRegs.cycle)<0)board.faults|=8;
        result=jdredd_zn_bus_read(&board.judge_bus,physical,width,value);
        if(result){if(result<0){board.faults|=8;*value=UINT32_MAX;}return 1;}
    }
    if(board.nbajam){
        result=nbajam_zn_bus_read(&board.nba_bus,physical,width,value);
        if(result){if(result<0){board.faults|=8;*value=UINT32_MAX;}return 1;}
    }
    if (board.primal) {
        if (systemzn_sync(psxRegs.cycle)<0)board.faults|=8;
        result=primal_zn_bus_read(board.primal_bus,physical,width,value);
        if(result){if(result<0){board.faults|=8;*value=UINT32_MAX;}return 1;}
        if(physical>=0x1f000000 && physical<0x1f200000){
            if((width!=1 && width!=2 && width!=4) || (physical&(width-1)) || (uint64_t)physical+width>0x1f200000)return 0;
            *value=0;for(unsigned i=0;i<width;i++)*value|=(uint32_t)board.regions[1][physical-0x1f000000+i]<<(i*8);
            return 1;
        }
    }
    result = handle_sio_read(physical, width, value);
    if (result) { if (result < 0) { board.faults |= 4u; *value = UINT32_MAX; return 1; } return 1; }
    if (board.visco && ((physical >= 0x1f000000 && physical < 0x1f280000) ||
        (physical >= 0x1fb00000 && physical < 0x1fc00000))) {
        int banked = physical >= 0x1fb00000;
        uint32_t base = banked ? 0x1fb00000 : 0x1f000000;
        size_t size = banked ? 0x100000 : board.sizes[2];
        unsigned region = banked ? 1 : 2;
        size_t offset = (banked ? (size_t)board.tps_bank * 0x100000 : 0) + physical - base;
        *value = UINT32_MAX;
        if ((width != 1 && width != 2 && width != 4) || (physical & (width - 1)) ||
            (uint64_t)physical + width > (uint64_t)base + size || offset > board.sizes[region] ||
            width > board.sizes[region] - offset) board.faults |= 8;
        else {
            *value = 0;
            for (unsigned i = 0; i < width; ++i) *value |= (uint32_t)board.regions[region][offset + i] << (i * 8);
        }
        return 1;
    }
    if (board.cbaj && physical >= 0x1fb00000 && physical <= 0x1fb00003) {
        if ((width != 1 && width != 2 && width != 4) || (physical & (width - 1)) ||
            (uint64_t)physical + width > 0x1fb00004) return 0;
        if (systemzn_sync(psxRegs.cycle) < 0) board.faults |= 8;
        *value = 0;
        for (unsigned i = 0; i < width; ++i) {
            uint8_t data = 0xff;
            if (physical + i == 0x1fb00000) {
                if (cbaj_sound_receive(board.cbaj_sound, &data)) board.faults |= 8;
            } else if (physical + i == 0x1fb00003) {
                if (cbaj_sound_main_status(board.cbaj_sound, &data)) board.faults |= 8;
            }
            *value |= (uint32_t)data << (i * 8);
        }
        return 1;
    }
    if ((board.tps || board.raizing || board.atlus || board.cbaj) && physical >= 0x1f000000 && physical < 0x1f800000) {
        size_t offset = (size_t)board.tps_bank * 0x800000 + physical - 0x1f000000;
        *value = UINT32_MAX;
        if ((width != 1 && width != 2 && width != 4) || (physical & (width - 1)) ||
            physical + width > 0x1f800000 || offset > board.sizes[1] || width > board.sizes[1] - offset)
            board.faults |= 8u;
        else {
            *value = 0;
            for (unsigned i = 0; i < width; ++i) *value |= (uint32_t)board.regions[1][offset + i] << (i * 8);
        }
        return 1;
    }
    if (board.capcom && ((physical >= 0x1f000000u && physical < 0x1f800000u) ||
        (physical >= 0x1fb00000u && physical < 0x1fc00000u))) zn_drain();
    result = board.tps || board.raizing || board.visco || board.atlus || board.cbaj || board.primal || board.nbajam || board.jdredd || board.metro ? 0 : capcom_zn_main_read(board.capcom, physical, width, value);
    if (result == CAPCOM_ZN_HANDLED) return 1;
    if (result < 0) { board.faults |= 8u; *value = UINT32_MAX; return 1; }
    if ((physical >= 0x1fa00000 && physical < 0x1fa40000) ||
        (physical >= 0x1fa10000 && physical < 0x1fa10400) ||
        (physical >= 0x1faf0000 && physical < 0x1faf0800))
        if (systemzn_sync(psxRegs.cycle) < 0) board.faults |= 1u;
    result = zn_board_read(&board.common, physical, width, value);
    return result == ZN_BOARD_HANDLED;
}

int systemzn_write(u32 address, unsigned width, u32 value)
{
    uint32_t physical = canonical_address(address);
    int result;
    if (!board.active) return 0;
    if(board.metro){
        result=metro_zn_bus_write(&board.metro_bus,physical,width,value);
        if(result){if(result<0)board.faults|=8;if(physical<0x1fb00002)map_bankedroms();return 1;}
    }
    if(board.jdredd){
        if(systemzn_sync(psxRegs.cycle)<0)board.faults|=8;
        result=jdredd_zn_bus_write(&board.judge_bus,physical,width,value);
        if(result){if(result<0)board.faults|=8;return 1;}
    }
    if(board.nbajam){
        if(physical>=0x1fbfff80 && physical<0x1fbfff84 && systemzn_sync(psxRegs.cycle)<0)board.faults|=8;
        result=nbajam_zn_bus_write(&board.nba_bus,physical,width,value);
        if(result){if(result<0)board.faults|=8;if(physical>=0x1fbfff00 && physical<0x1fbfff04)map_bankedroms();return 1;}
    }
    if (board.primal) {
        if(systemzn_sync(psxRegs.cycle)<0)board.faults|=8;
        result=primal_zn_bus_write(board.primal_bus,physical,width,value);
        if(result){if(result<0)board.faults|=8;return 1;}
        if(physical>=0x1f000000 && physical<0x1f200000)return 1;
    }
    result = handle_sio_write(physical, width, value);
    if (result) { if (result < 0) board.faults |= 4u; return 1; }
    if (physical == 0x1fb60000 && systemzn_sync(psxRegs.cycle)<0) board.faults |= 8u;
    if (board.visco && ((physical >= 0x1f000000 && physical < 0x1f280000) ||
        (physical >= 0x1fb00000 && physical < 0x1fc00000))) {
        /* Only the bank window's low first byte is a source write handler.
         * All remaining fixed/banked ROM bytes stay read-only. */
        if (physical == 0x1fb00000 && (width == 1 || width == 2 || width == 4)) {
            board.tps_bank = value & 0xff; board.tps_bank_writes++;
            if (board.tps_bank >= 24) board.faults |= 8;
            map_bankedroms();
        }
        return 1;
    }
    if (board.cbaj && physical >= 0x1fb00000 && physical <= 0x1fb00003) {
        if ((width != 1 && width != 2 && width != 4) || (physical & (width - 1)) ||
            (uint64_t)physical + width > 0x1fb00004) return 0;
        if (systemzn_sync(psxRegs.cycle) < 0) board.faults |= 8;
        if (physical == 0x1fb00000 &&
            cbaj_sound_send(board.cbaj_sound, value & 0xff)) board.faults |= 8;
        return 1;
    }
    if (board.atlus && physical >= 0x1fb00000 && physical <= 0x1fb00003) {
        if ((width != 1 && width != 2 && width != 4) || (physical & (width - 1))) return 0;
        if (systemzn_sync(psxRegs.cycle) < 0) board.faults |= 8;
        if (physical < 0x1fb00002) {
            unsigned lane = (physical - 0x1fb00000) * 8;
            /* Source generic_latch_16 has no mem_mask/COMBINE_DATA. */
            if (atlus_sound_command(board.atlus_sound, (value << lane) & 0xffff)) board.faults |= 8;
        }
        if (physical <= 0x1fb00002 && (uint64_t)physical + width > 0x1fb00002) {
            board.tps_bank = (value >> ((0x1fb00002 - physical) * 8)) & 3;
            board.tps_bank_writes++; map_bankedroms();
        }
        return 1;
    }
    if ((board.tps || board.cbaj) && physical <= 0x1fb00006 && (uint64_t)physical + width > 0x1fb00006) {
        board.tps_bank = (value >> ((0x1fb00006 - physical) * 8)) & 0xff;
        board.tps_bank_writes++;
        if (board.tps_bank >= 9 || (size_t)(board.tps_bank + 1) * 0x800000 > board.sizes[1])
            board.faults |= 8u;
        map_bankedroms();
        return 1;
    }
    if (board.raizing && ((physical <= 0x1fb00000 && (uint64_t)physical + width > 0x1fb00000) ||
        (physical <= 0x1fb00004 && (uint64_t)physical + width > 0x1fb00004))) {
        if (systemzn_sync(psxRegs.cycle) < 0) board.faults |= 8u;
        if (physical <= 0x1fb00000 && (uint64_t)physical + width > 0x1fb00000) {
            if (raizing_sound_command(board.raizing_sound, value >> ((0x1fb00000 - physical) * 8))) board.faults |= 8;
        } else if (raizing_sound_irq(board.raizing_sound)) board.faults |= 8;
        return 1;
    }
    if (board.capcom && physical >= 0x1fb00000u && physical < 0x1fc00000u) zn_drain();
    result = board.tps || board.raizing || board.visco || board.atlus || board.cbaj || board.primal || board.nbajam || board.jdredd || board.metro ? 0 : capcom_zn_main_write(board.capcom, physical, width, value);
    if (result == CAPCOM_ZN_HANDLED) { if (physical == 0x1fb00000) map_bankedroms(); return 1; }
    if (result < 0) { board.faults |= 8u; return 1; }
    if ((physical >= 0x1fa00000 && physical < 0x1fa40000) ||
        (physical >= 0x1fa10000 && physical < 0x1fa10400) ||
        (physical >= 0x1faf0000 && physical < 0x1faf0800))
        if (systemzn_sync(psxRegs.cycle) < 0) board.faults |= 1u;
    uint8_t old_coin = board.common.coin;
    result = zn_board_write(&board.common, physical, width, value);
    if (board.primal && result==ZN_BOARD_HANDLED && physical==0x1fa20000 &&
        !(old_coin&1) && (board.common.coin&1)) board.coin_count[0]++;
    if ((board.visco || board.atlus || board.cbaj || board.tps || board.nbajam || board.jdredd || board.metro) && result == ZN_BOARD_HANDLED && physical == 0x1fa20000) {
        /* Visco/Tecmo omit lockouts; Atlus connects four. Preserve each
         * source counter's rising-edge input and player ordering. */
        for (unsigned i = 0; i < (board.visco ? 2u : 4u); ++i) {
            static const unsigned players[] = {0, 2, 1, 3};
            unsigned mask = 1u << (i * (board.visco ? 4 : 2));
            unsigned player = board.visco ? i : players[i];
            if (!(old_coin & mask) && (board.common.coin & mask)) board.coin_count[player]++;
            if (board.atlus) {
                if (board.common.coin & (mask << 1)) board.coin_lockout |= 1u << player;
                else board.coin_lockout &= ~(1u << player);
            }
        }
    }
    if (board.raizing && result == ZN_BOARD_HANDLED && physical == 0x1fa10300) {
        board.tps_bank = board.common.znsecsel & 3; board.tps_bank_writes++;
        if ((size_t)(board.tps_bank + 1) * 0x800000 > board.sizes[1]) board.faults |= 8;
        map_bankedroms();
    }
    return result == ZN_BOARD_HANDLED;
}

static int set_raw_bit(unsigned port, unsigned mask, int pressed)
{
    if (port >= 6) return -1;
    if (pressed) board.inputs[port] &= (uint8_t)~mask;
    else board.inputs[port] |= (uint8_t)mask;
    return 0;
}
static int set_kick(unsigned player, unsigned bit, int pressed)
{
    if (player >= 2 || bit >= 32) return -1;
    if (pressed) board.kicks[player] &= ~(1u << bit);
    else board.kicks[player] |= 1u << bit;
    return 0;
}
static int input_supported(const char *name, int pressed)
{
    const char *profile = board.profile->input;
    if(board.metro){
        int generic=board.metro_dsw&1;
        if(!strcmp(name,"cabinet_generic")){board.metro_dsw=(board.metro_dsw&~1u)|pressed;return 0;}
        if(!strncmp(name,"p1",2) || !strncmp(name,"p2",2)){
            unsigned p=name[1]-'1';const char *c=name+2;
            const char *directions[]={"up","down","left","right"};
            for(unsigned i=0;i<4;i++)if(!strcmp(c,directions[i]))return pressed && i==1 && !generic?-1:set_raw_bit(p,1u<<i,pressed);
            if(!strcmp(c,"button1") || !strcmp(c,"rhythm"))return set_raw_bit(p,0x10,pressed);
            if(!strcmp(c,"button2") || !strcmp(c,"jammar"))return set_raw_bit(p,0x20,pressed);
            if(!strcmp(c,"pedal"))return pressed && generic?-1:set_raw_bit(p,0x02,pressed);
            if(!strcmp(c,"start"))return generic?set_raw_bit(3,1u<<p,pressed):set_raw_bit(p,0x20,pressed);
            return -1;
        }
        if((!generic || !pressed) && !strcmp(name,"p3up"))return set_raw_bit(0,0x40,pressed);
        if((!generic || !pressed) && !strcmp(name,"p3down"))return set_raw_bit(1,0x40,pressed);
        if((!generic || !pressed) && !strcmp(name,"p3enter"))return set_raw_bit(3,1,pressed);
        if((!generic || !pressed) && !strcmp(name,"service2"))return set_raw_bit(3,2,pressed);
        if(!strcmp(name,"coin1") || !strcmp(name,"coin2"))return set_raw_bit(3,0x10u<< (name[4]-'1'),pressed);
        if(!strcmp(name,"test"))return set_raw_bit(2,1,pressed);
        if(!strcmp(name,"service1"))return set_raw_bit(2,2,pressed);
        if(!strcmp(name,"tilt"))return set_raw_bit(2,4,pressed);
        return -1;
    }
    if (board.tps && (!strcmp(name, "coin2") || strstr(name, "button3"))) return -1;
    if (board.visco && strstr(name, "button3")) return -1;
    if(board.jdredd){
        if(name[0]=='p' && (name[1]=='1' || name[1]=='2')){
            unsigned player=name[1]-'1';const char *c=name+2;
            if(!strcmp(c,"button1"))return set_raw_bit(2,0x40u<<player,pressed);
            if(!strcmp(c,"button2"))return set_raw_bit(player,0x10,pressed);
            if(!strcmp(c,"start"))return set_raw_bit(3,1u<<player,pressed);
            return -1;
        }
        if(strlen(name)==5 && !strncmp(name,"coin",4) && name[4]>='1' && name[4]<='4')return set_raw_bit(3,0x10u<<(name[4]-'1'),pressed);
    }
    if(board.nbajam){
        if(name[0]=='p' && name[1]>='1' && name[1]<='4'){
            unsigned player=name[1]-'1',port=player<2?player:player+2;const char *c=name+2;
            if(!strcmp(c,"start"))return set_raw_bit(3,1u<<player,pressed);
            const char *directions[]={"up","down","left","right"};
            for(unsigned i=0;i<4;i++)if(!strcmp(c,directions[i]))return set_raw_bit(port,1u<<i,pressed);
            if(strlen(c)==7 && !strncmp(c,"button",6) && c[6]>='1' && c[6]<='4')return set_raw_bit(port,0x10u<<(c[6]-'1'),pressed);
            return -1;
        }
        if(strlen(name)==5 && !strncmp(name,"coin",4) && name[4]>='1' && name[4]<='4')return set_raw_bit(3,0x10u<<(name[4]-'1'),pressed);
    }
    if (!strncmp(name, "p1", 2) || !strncmp(name, "p2", 2)) {
        unsigned player = name[1] == '1' ? 0 : 1;
        const char *control = name + 2;
        if (player && (!strcmp(profile, "strider2") || !strcmp(profile, "zn1p"))) return -1;
        if (!strcmp(control, "up")) return set_raw_bit(player, 0x01, pressed);
        if (!strcmp(control, "down")) return set_raw_bit(player, 0x02, pressed);
        if (!strcmp(control, "left")) return set_raw_bit(player, 0x04, pressed);
        if (!strcmp(control, "right")) return set_raw_bit(player, 0x08, pressed);
        if (strlen(control) == 7 && !strncmp(control, "button", 6) && control[6] >= '1' && control[6] <= '6') {
            unsigned button = (unsigned)(control[6] - '1');
            unsigned port = player;
            unsigned mask = 0x10u << button;
            if (button < 2)
                return set_raw_bit(port, mask, pressed);
            if (button == 2) {
                if (!strcmp(profile, "capcom4b"))
                    return set_raw_bit(player ? 5 : 4, 0x10, pressed) | set_kick(player, 0, pressed);
                if (player && (!strcmp(profile, "strider2") || !strcmp(profile, "zn1p"))) return -1;
                return set_raw_bit(port, 0x40, pressed);
            }
            if (!strcmp(profile,"primrag2")) return set_raw_bit(5,1u<<((button-3)+(player?4:0)),pressed);
            if (!strcmp(profile, "bldyror2") && button == 3)
                return set_raw_bit(4, player ? 0x20 : 0x10, pressed);
            if (!strcmp(profile, "capcom6b")) {
                return set_raw_bit(player ? 5 : 4, 0x10u << (button - 3), pressed) |
                    set_kick(player, button - 3, pressed);
            }
            if (!strcmp(profile, "capcom4b")) {
                if (button == 3)
                    return set_raw_bit(player ? 5 : 4, 0x20, pressed) | set_kick(player, 1, pressed);
                return -1;
            }
            if (!strcmp(profile, "plsmaswd")) {
                if (button == 3)
                    return set_raw_bit(player ? 5 : 4, 0x10, pressed) | set_kick(player, 0, pressed);
                return -1;
            }
            return -1; /* Strider 2 leaves P3/P4 and kick harness buttons unused. */
        }
        if (!strcmp(control, "start")) {
            if (player && (!strcmp(profile, "strider2") || !strcmp(profile, "zn1p"))) return -1;
            return set_raw_bit(3, 1u << player, pressed);
        }
        if (strlen(control) == 5 && !strncmp(control, "kick", 4) && control[4] >= '3' && control[4] <= '6') {
            unsigned b = (unsigned)(control[4] - '3');
            unsigned kick_bit;
            if (!strcmp(profile, "capcom6b")) {
                if (!b) return -1;
                kick_bit = b - 1;
            }
            else if (!strcmp(profile, "capcom4b")) {
                if (b > 1) return -1;
                kick_bit = b;
            } else if (!strcmp(profile, "plsmaswd")) {
                if (b != 1) return -1;
                kick_bit = 0;
            } else return -1;
            return set_kick(player, kick_bit, pressed);
        }
    }
    if (!strcmp(name, "coin1")) return set_raw_bit(3, 0x10, pressed);
    if (!strcmp(name, "coin2")) return set_raw_bit(3, 0x20, pressed);
    if (!strcmp(name, "test")) return set_raw_bit(2, 0x01, pressed);
    if (!strcmp(name, "service1")) return set_raw_bit(2, 0x02, pressed);
    if (!strcmp(name, "tilt")) return set_raw_bit(2, 0x04, pressed);
    return -1;
}
int systemzn_set_input(const char *control, int pressed)
{
    if (!board.active || !control || (pressed != 0 && pressed != 1)) return -1;
    return input_supported(control, pressed);
}

int systemzn_eeprom_import(const unsigned char *image, size_t size)
{
    if(!board.active || !image || size!=(board.nbajam?34848u:2080u))return -1;
    if(zn_board_eeprom_import(&board.common,image,2080))return -1;
    if(board.nbajam)memcpy(board.nba_bus.sram,image+2080,32768);return 0;
}
int systemzn_eeprom_export(unsigned char *image, size_t size)
{
    if(!board.active || !image || size!=(board.nbajam?34848u:2080u))return -1;
    if(zn_board_eeprom_export(&board.common,image,2080))return -1;
    if(board.nbajam)memcpy(image+2080,board.nba_bus.sram,32768);return 0;
}
void systemzn_set_audio_output(void (*callback)(void *, const int16_t *, unsigned), void *opaque)
{ zn_flush_audio(); board.audio_callback = callback; board.audio_opaque = opaque; board.audio_buffer_frames = 0; }
void systemzn_set_rax_audio_output(void (*callback)(void *,const float *,size_t,unsigned,const uint64_t *),void *opaque)
{ board.rax_callback=callback;board.rax_opaque=opaque; }
void systemzn_set_ymf_audio_output(void (*callback)(void *, float, float, uint64_t), void *opaque)
{ board.ymf_callback = callback; board.ymf_opaque = opaque; }
void systemzn_set_ymz_audio_output(void (*callback)(void *, float, float, uint64_t), void *opaque)
{ systemzn_set_ymf_audio_output(callback, opaque); }
int systemzn_open_audio(const char *path)
{
    if (!board.active || board.tps || board.primal || board.nbajam || board.jdredd || board.wav.file) return -1;
    zn_flush_audio();
    return pcm_wav_open(&board.wav, path, board.atlus || board.cbaj ? 88200 : board.raizing ? 44100 : SYSTEMZN_QSOUND_RATE);
}
int systemzn_close_audio(void) { zn_flush_audio(); return pcm_wav_close(&board.wav); }

int systemzn_selftest(void)
{
    uint32_t value;
    unsigned before;
    if (!board.active) return -1;
    zn_drain();
    if (systemzn_read(0x1fa10200, 1, &value) != 1 ||
        value != (board.primal ? 0x6a : board.profile->vram_size == 0x200000 ? 0x69 : 0x61)) return -1;
    if (systemzn_read(0x1f000000, 4, &value) != 1 ||
        value != *(uint32_t *)board.regions[board.visco ? 2 : 1]) return -1;
    if (!board.tps && !board.raizing && !board.visco && !board.atlus && !board.cbaj && !board.primal && !board.nbajam && !board.jdredd && !board.metro && (systemzn_read(0x1fb80000, 4, &value) != 1 || value != *(uint32_t *)board.regions[2])) return -1;
    if (systemzn_read(0x1fa00300, 1, &value) != 1) return -1;
    before = value;
    if (systemzn_set_input("coin1", 1) || systemzn_read(0x1fa00300, 1, &value) != 1 ||
        value != (before & ~0x10u)) return -1;
    systemzn_set_input("coin1", 0);
    if (systemzn_set_input("unknown-control", 1) == 0) return -1;
    if(board.metro){
        uint8_t old_dsw=board.metro_dsw;uint16_t old_command=board.metro_bus.command;
        unsigned old_bank=board.metro_bus.bank;
        if(systemzn_set_input("cabinet_generic",0) || systemzn_set_input("p1start",1) ||
           systemzn_read(0x1fa00000,1,&value)!=1 || value!=0xdf)return -1;
        systemzn_set_input("p1start",0);
        if(systemzn_set_input("p1down",1)!=-1 || systemzn_set_input("p1pedal",1) ||
           systemzn_read(0x1fa00000,1,&value)!=1 || value!=0xfd)return -1;
        systemzn_set_input("p1pedal",0);
        if(systemzn_set_input("p3up",1) || systemzn_read(0x1fa00000,1,&value)!=1 || value!=0xbf)return -1;
        systemzn_set_input("p3up",0);
        if(systemzn_set_input("cabinet_generic",1) || systemzn_set_input("p1start",1) ||
           systemzn_read(0x1fa00300,1,&value)!=1 || value!=0xfe)return -1;
        systemzn_set_input("p1start",0);
        if(systemzn_set_input("p1down",1) || systemzn_read(0x1fa00000,1,&value)!=1 || value!=0xfd)return -1;
        systemzn_set_input("p1down",0);
        if(systemzn_set_input("p3enter",1)!=-1 || systemzn_set_input("p1pedal",1)!=-1 ||
           systemzn_set_input("p1button3",1)!=-1)return -1;
        if(systemzn_write(0x1fb00000,2,10)!=1 || systemzn_read(0x1f400000,4,&value)!=1 ||
           value!=*(uint32_t *)(board.regions[1]+0x2800000))return -1;
        if(systemzn_write(0x1fb00002,2,0x7f)!=1 || systemzn_read(0x1fb00004,2,&value)!=1 || value!=1)return -1;
        if(systemzn_write(0x1fb00002,2,0)!=1 || systemzn_read(0x1fb00004,2,&value)!=1 || value!=4)return -1;
        systemzn_write(0x1fb00000,2,old_bank);systemzn_write(0x1fb00002,2,old_command);board.metro_dsw=old_dsw;
    }
    if(board.jdredd){
        if(systemzn_set_input("p1left",1)==0 || systemzn_set_input("p1button3",1)==0 || systemzn_set_input("p3start",1)==0)return -1;
        if(systemzn_set_input("p1button1",1) || systemzn_read(0x1fa00200,1,&value)!=1 || value!=0xbf)return -1;
        systemzn_set_input("p1button1",0);
        if(systemzn_set_input("p2button2",1) || systemzn_read(0x1fa00100,1,&value)!=1 || value!=0xef)return -1;
        systemzn_set_input("p2button2",0);
        unsigned char nv[2080],copy[2080];
        if(systemzn_eeprom_export(nv,sizeof(nv)) || systemzn_eeprom_import(nv,sizeof(nv)) || systemzn_eeprom_export(copy,sizeof(copy)) || memcmp(nv,copy,sizeof(nv)))return -1;
    }
    if(board.nbajam){
        const uint32_t ports[]={0x1fa00000,0x1fa00100,0x1fa10000,0x1fa10100};
        for(unsigned player=0;player<4;player++)for(unsigned button=1;button<=4;button++){
            char name[24];snprintf(name,sizeof(name),"p%ubutton%u",player+1,button);
            if(systemzn_set_input(name,1) || systemzn_read(ports[player],1,&value)!=1 || value!=(0xffu&~(0x10u<<(button-1))))return -1;
            systemzn_set_input(name,0);
        }
        unsigned char nv[34848],copy[34848];
        if(systemzn_eeprom_export(nv,sizeof(nv)) || systemzn_eeprom_import(nv,sizeof(nv)) ||
           systemzn_eeprom_export(copy,sizeof(copy)) || memcmp(nv,copy,sizeof(nv)))return -1;
    }
    if (board.primal) {
        for(unsigned player=0;player<2;player++)for(unsigned button=4;button<=6;button++){
            char name[24];snprintf(name,sizeof(name),"p%ubutton%u",player+1,button);
            unsigned mask=1u<<(button-4+(player?4:0));
            if(systemzn_set_input(name,1) || systemzn_read(0x1fa10100,1,&value)!=1 || value!=(0xffu&~mask))return -1;
            systemzn_set_input(name,0);
        }
        unsigned char nv[2080],roundtrip[2080];
        if(systemzn_eeprom_export(nv,sizeof(nv)) || systemzn_eeprom_import(nv,sizeof(nv)) ||
            systemzn_eeprom_export(roundtrip,sizeof(roundtrip)) || memcmp(nv,roundtrip,sizeof(nv)))return -1;
    }
    if (board.cbaj) {
        if (systemzn_set_input("p2start", 1) == 0 || systemzn_set_input("p2left", 1) == 0 ||
            systemzn_set_input("p2button1", 1) == 0 || systemzn_set_input("p1button4", 1) == 0) return -1;
        if (systemzn_set_input("p1button3", 1) || systemzn_read(0x1fa00000, 1, &value) != 1 ||
            value != 0xbf) return -1;
        systemzn_set_input("p1button3", 0);
        unsigned char nv[2080], roundtrip[2080];
        if (systemzn_eeprom_export(nv, sizeof(nv)) || systemzn_eeprom_import(nv, sizeof(nv)) ||
            systemzn_eeprom_export(roundtrip, sizeof(roundtrip)) || memcmp(nv, roundtrip, sizeof(nv))) return -1;
    }
    if (board.visco) {
        if (systemzn_set_input("p1button3", 1) == 0 || systemzn_set_input("p2button3", 1) == 0 ||
            systemzn_set_input("p1button4", 1) == 0) return -1;
        unsigned char nv[2080], roundtrip[2080];
        if (systemzn_eeprom_export(nv, sizeof(nv)) || systemzn_eeprom_import(nv, sizeof(nv)) ||
            systemzn_eeprom_export(roundtrip, sizeof(roundtrip)) || memcmp(nv, roundtrip, sizeof(nv))) return -1;
    }
    if (board.raizing) {
        unsigned char nv[2080], roundtrip[2080];
        if (systemzn_eeprom_export(nv, sizeof(nv)) || (board.synthetic && memcmp(nv, board.regions[2], 2048)) ||
            systemzn_eeprom_import(nv, sizeof(nv)) || systemzn_eeprom_export(roundtrip, sizeof(roundtrip)) ||
            memcmp(nv, roundtrip, sizeof(nv))) return -1;
        if (!strcmp(board.profile->input, "bldyror2")) {
            if (systemzn_set_input("p1button4", 1) || systemzn_read(0x1fa10000, 1, &value) != 1 || value != 0xef) return -1;
            systemzn_set_input("p1button4", 0);
            if (systemzn_set_input("p2button4", 1) || systemzn_read(0x1fa10000, 1, &value) != 1 || value != 0xdf) return -1;
            systemzn_set_input("p2button4", 0);
        } else if (systemzn_set_input("p1button4", 1) == 0) return -1;
    }
    fprintf(stderr, "SystemZN selftest: PASS\n");
    return 0;
}

void systemzn_report(FILE *out)
{
    capcom_zn_status_t status;
    QSoundHLEStatus qstatus;
    uint16_t qreg78 = 0, qreg1 = 0, qreg2 = 0, qreg4 = 0, qreg5 = 0, qreg6 = 0;
    uint8_t sound_f010, sound_f020;
    if (!board.active || !out) return;
    zn_flush_audio();
#ifdef SYSTEM12_BOARD_THREAD
    if (board.capcom) fprintf(out, "SystemZN: board_thread=%u worker_commands=%" PRIu64 " worker_cycles=%" PRIu64 " worker_slices=%" PRIu64 "\n",
        bt.active, bt.commands, bt.worker_cycles, bt.worker_slices);
#endif
    if(board.metro){
        MetroZnBus *b=&board.metro_bus;
        fprintf(out,"Metro ZN: profile=%s RAM=4194304 VRAM=2097152 bank=%u command=%04x faults=%u media=%s H8=NO_DUMP ATA_transport=UNIMPLEMENTED disk_data_accesses=0 DSW=%02x\n",board.profile->set,b->bank,b->command,board.faults,board.synthetic?"SYNTHETIC_FIXTURE":"SOURCE_VERIFIED",board.metro_dsw);
        fprintf(out,"Metro provisional MCU: bank_writes=%" PRIu64 " status_reads=%" PRIu64 " unpopulated_reads=%" PRIu64 " ports=%" PRIu64 "/%" PRIu64 ",%" PRIu64 "/%" PRIu64 ",%" PRIu64 "/%" PRIu64 ",%" PRIu64 "/%" PRIu64 "\n",b->bank_writes,b->provisional_status_reads,b->unpopulated_reads,b->reads[0],b->writes[0],b->reads[1],b->writes[1],b->reads[2],b->writes[2],b->reads[3],b->writes[3]);return;
    }
    if(board.jdredd){
        JdreddZnBus *b=&board.judge_bus;
        fprintf(out,"Judge ZN: profile=jdredd model=COH1000A RAM=4194304 VRAM=2097152 faults=%u media=%s HDD_sectors=%u reads=%" PRIu64 " writes=%" PRIu64 " errors=%" PRIu64 " IRQ10_edges=%" PRIu64 " ATA_IRQ=%u\n",board.faults|b->faults,board.synthetic?"SYNTHETIC_FIXTURE":"SOURCE_VERIFIED",board.primal_disk_info.sector_count,board.disk_reads,board.disk_writes,board.disk_errors,board.judge_irq_edges,board.judge_ata_irq);
        fprintf(out,"Judge guns: VBlank=%" PRIu64 " mux=%u latch=%" PRIu64 " offscreen=%" PRIu64 " P1=%04x/%04x P2=%04x/%04x ATA_cs0=%" PRIu64 "/%" PRIu64 " cs1=%" PRIu64 "/%" PRIu64 "\n",b->vblanks,b->gun_mux,b->gun_latches,b->offscreen,b->gun_x[0],b->gun_y[0],b->gun_x[1],b->gun_y[1],b->reads[0],b->writes[0],b->reads[1],b->writes[1]);return;
    }
    if(board.nbajam){
        RaxSoundStatus r;rax_sound_status(board.rax,&r);NbajamZnBus *b=&board.nba_bus;
        fprintf(out,"NBA Jam ZN: profile=nbajamex model=COH1000A RAM=4194304 VRAM=2097152 page=%u banks=%u/%u latches=%04x/%04x bank_writes=%" PRIu64 " SRAM_reads=%" PRIu64 " SRAM_writes=%" PRIu64 " faults=%u media=%s\n",b->page,b->bank[0],b->bank[1],b->latch[0],b->latch[1],b->bank_writes,b->sram_reads,b->sram_writes,board.faults|b->faults|r.faults,board.synthetic?"SYNTHETIC_FIXTURE":"SOURCE_VERIFIED");
        fprintf(out,"NBA RAX: cycles=%" PRIu64 " PC=%04x IRQ=%" PRIu64 " commands=%" PRIu64 " reads=%" PRIu64 " replies=%" PRIu64 " output=%04x BDMA=%" PRIu64 " words=%" PRIu64 " buffers=%" PRIu64 " rate=%u nonzero=%" PRIu64 " overrun=%" PRIu64 " faults=%u\n",r.cpu.cycles,r.cpu.pc,r.cpu.interrupts,b->commands,r.host_reads,r.replies,r.output_latch,r.bdma_completions,r.bdma_words,r.autobuffers,r.sample_rate,r.dac_nonzero,r.dac_overruns,r.faults);
        fprintf(out,"NBA coins: %" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n",board.coin_count[0],board.coin_count[1],board.coin_count[2],board.coin_count[3]);return;
    }
    if (board.primal) {
        PrimalZnBusStatus s;primal_zn_bus_status(board.primal_bus,&s);
        fprintf(out,"Primal ZN: profile=primrag2 model=COH1000W CPU=67737600 GPU=2 RAM=8388608 VRAM=2097152 faults=%u media=%s\n",board.faults|s.faults,board.synthetic?"SYNTHETIC_FIXTURE":"SOURCE_VERIFIED");
        fprintf(out,"Primal ZN: HDD sectors=%u reads=%" PRIu64 " writes=%" PRIu64 " errors=%" PRIu64 " status=%02x error=%02x DMA5_reads=%" PRIu64 " words=%" PRIu64 " DMA5_writes=%" PRIu64 "\n",
            board.primal_disk_info.sector_count,board.disk_reads,board.disk_writes,board.disk_errors,ata_storage_status(board.primal_ata),ata_storage_error(board.primal_ata),s.dma_reads,s.dma_read_words,s.dma_writes);
        fprintf(out,"Primal ZN: IRQ_control=%04x ATA_IRQ=%u IRQ10=%u latch=%04x watchdog_kicks=%" PRIu64 " watchdog_resets=%" PRIu64 " pending=%u VIA_diagnostics=%u\n",
            s.irq_control,s.ata_irq,s.irq10,s.data_latch,s.watchdog_kicks,s.watchdog_resets,s.reset_pending,s.via.diagnostics);
        return;
    }
    if (board.cbaj) {
        CbajSoundStatus sound; cbaj_sound_status(board.cbaj_sound, &sound);
        fprintf(out, "CBaj TPS: profile=%s model=COH1002M CPU=%u GPU=2 RAM=%u VRAM=%u bank=%u bank_writes=%" PRIu64 " faults=%u media=%s\n",
            board.profile->set, board.profile->cpu_clock, board.profile->ram_size, board.profile->vram_size,
            board.tps_bank, board.tps_bank_writes, board.faults | sound.faults, board.synthetic ? "SYNTHETIC_FIXTURE" : "SOURCE_VERIFIED");
        fprintf(out, "CBaj TPS: Z80=4000000Hz cycles=%" PRIu64 " actual=%" PRIu64 " PC=%04x SP=%04x IRQ=%" PRIu64 " NMI=%" PRIu64 " unsupported=%" PRIu64 " debt=%" PRId64 "\n",
            sound.cpu_cycles, sound.z80.cycles, sound.z80.pc, sound.z80.sp, sound.z80.interrupts, sound.z80.nmis, sound.z80.unsupported_operations, sound.z80.cycle_debt);
        fprintf(out, "CBaj TPS: YMZ280B=16934400Hz rate=%u frames=%" PRIu64 " writes=%" PRIu64 " peak=%u diagnostics=%u playing=%02x ends=%" PRIu64 "\n",
            sound.ymz.sample_rate, sound.ymz.frames, sound.ymz_writes, board.peak, sound.ymz.diagnostics, sound.ymz.playing, sound.ymz.voice_ends);
        for (unsigned i = 0; i < 2; ++i)
            fprintf(out, "CBaj TPS: FIFO%u depth=%u used=%u EF/FF/HF=%u/%u/%u writes=%" PRIu64 " reads=%" PRIu64 " overflow=%" PRIu64 " underflow=%" PRIu64 "\n",
                i, sound.fifo[i].depth, sound.fifo[i].used, sound.fifo[i].ef, sound.fifo[i].ff, sound.fifo[i].hf,
                sound.fifo[i].writes, sound.fifo[i].reads, sound.fifo[i].overflow, sound.fifo[i].underflow);
        return;
    }
    if (board.atlus) {
        AtlusSoundStatus sound; atlus_sound_status(board.atlus_sound, &sound);
        fprintf(out, "Atlus ZN: profile=%s model=COH1001L CPU=%u GPU=2 RAM=%u VRAM=%u bank=%u bank_writes=%" PRIu64 " faults=%u media=%s\n",
            board.profile->set, board.profile->cpu_clock, board.profile->ram_size, board.profile->vram_size,
            board.tps_bank, board.tps_bank_writes, board.faults | sound.faults, board.synthetic ? "SYNTHETIC_FIXTURE" : "SOURCE_VERIFIED");
        fprintf(out, "Atlus ZN: MC68000=10000000Hz cycles=%" PRIu64 " PC=%06x SR=%04x commands=%" PRIu64 " latch=%04x reads=%" PRIu64 " IRQ3_ack=%" PRIu64 " IRQ2_ack=%" PRIu64 " pending=%u IRQ2=%u\n",
            sound.cpu_cycles, sound.pc, sound.sr, sound.commands, sound.latch, sound.latch_reads, sound.irq3_ack, sound.irq2_ack, sound.pending, sound.irq2);
        fprintf(out, "Atlus ZN: YMZ280B=16934400Hz rate=%u frames=%" PRIu64 " writes=%" PRIu64 " peak=%u status=%02x playing=%02x ends=%" PRIu64 " diagnostics=%u coin_counters=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 " lockout=%x\n",
            sound.ymz.sample_rate, sound.ymz.frames, sound.ymz_writes, board.peak, sound.ymz.status, sound.ymz.playing, sound.ymz.voice_ends,
            sound.ymz.diagnostics, board.coin_count[0], board.coin_count[1], board.coin_count[2], board.coin_count[3], board.coin_lockout);
        return;
    }
    if (board.visco) {
        fprintf(out, "Visco ZN: profile=%s model=COH1002V CPU=%u GPU=2 RAM=%u VRAM=%u rotation=270 bank=%u bank_writes=%" PRIu64 " faults=%u media=%s\n",
            board.profile->set, board.profile->cpu_clock, board.profile->ram_size,
            board.profile->vram_size, board.tps_bank, board.tps_bank_writes, board.faults,
            board.synthetic ? "SYNTHETIC_FIXTURE" : "SOURCE_VERIFIED");
        fprintf(out, "Visco ZN: SPU=33868800Hz SIO0=%" PRIu64 " SIO1=%" PRIu64 " time_us=%" PRIu64 " coin_counters=%" PRIu64 "/%" PRIu64 " inputs=%02x/%02x/%02x/%02x/%02x/%02x\n",
            psx_sio_elapsed_cycles(board.sio[0]), psx_sio_elapsed_cycles(board.sio[1]), cycles_to_us(board.peripheral_cycles),
            board.coin_count[0], board.coin_count[1], board.inputs[0], board.inputs[1], board.inputs[2],
            board.inputs[3], board.inputs[4], board.inputs[5]);
        return;
    }
    if (board.raizing) {
        RaizingSoundStatus sound; raizing_sound_status(board.raizing_sound, &sound);
        fprintf(out, "Raizing ZN: profile=%s model=COH1002E CPU=%u GPU=2 RAM=%u VRAM=%u rotation=%u bank=%u bank_writes=%" PRIu64 " faults=%u media=%s\n",
            board.profile->set, board.profile->cpu_clock, board.profile->ram_size, board.profile->vram_size,
            systemzn_rotation(), board.tps_bank, board.tps_bank_writes, board.faults | sound.faults,
            board.synthetic ? "SYNTHETIC_FIXTURE" : "SOURCE_VERIFIED");
        fprintf(out, "Raizing ZN: MC68000=12000000Hz cycles=%" PRIu64 " PC=%06x SR=%04x commands=%" PRIu64 " IRQ2_requests=%" PRIu64 " IRQ2_ack=%" PRIu64 " pending=%u IRQ2=%u\n",
            sound.cpu_cycles, sound.pc, sound.sr, sound.commands, sound.irq_requests, sound.irq_acknowledgements, sound.pending, sound.irq2);
        fprintf(out, "Raizing ZN: YMF271=16934400Hz rate=%u frames=%" PRIu64 " writes=%" PRIu64 " peak=%u diagnostics=%u faults=%u timer_events=%" PRIu64 "/%" PRIu64 "\n",
            sound.ymf.sample_rate, sound.ymf.frames, sound.ymf_writes, board.peak, sound.ymf.diagnostics,
            sound.ymf.faults, sound.ymf.timer_a_events, sound.ymf.timer_b_events);
        return;
    }
    if (board.tps) {
        fprintf(out, "Tecmo TPS: profile=%s model=COH1002M CPU=%u GPU=2 RAM=%u VRAM=%u bank=%u bank_writes=%" PRIu64 " faults=%u media=%s\n",
            board.profile->set, board.profile->cpu_clock, board.profile->ram_size,
            board.profile->vram_size, board.tps_bank, board.tps_bank_writes, board.faults,
            board.synthetic ? "SYNTHETIC_FIXTURE" : "SOURCE_VERIFIED");
        fprintf(out, "Tecmo TPS: SPU=33868800Hz SIO0=%" PRIu64 " SIO1=%" PRIu64 " time_us=%" PRIu64 "\n",
            psx_sio_elapsed_cycles(board.sio[0]), psx_sio_elapsed_cycles(board.sio[1]), cycles_to_us(board.peripheral_cycles));
        fprintf(out, "Tecmo TPS: coin_counters=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 " lockouts=UNWIRED\n",
            board.coin_count[0],board.coin_count[1],board.coin_count[2],board.coin_count[3]);
        if (board.tecmo_link) {
            TecmoLinkStatus s; tecmo_link_status(board.tecmo_link, &s);
            fprintf(out, "Tecmo TPS Link: Z80=4000000Hz clock_source=uncertain cycles=%" PRIu64 " actual=%" PRIu64 " PC=%04x SP=%04x IRQ=%" PRIu64 " NMI=%" PRIu64 " unsupported=%" PRIu64 " faults=%u debt=%" PRId64 "\n",
                s.cycles,s.cpu.cycles,s.cpu.pc,s.cpu.sp,s.cpu.interrupts,s.cpu.nmis,s.cpu.unsupported_operations,s.faults,s.cpu.cycle_debt);
            fprintf(out, "Tecmo TPS Link: IO_reads=%" PRIu64 " IO_writes=%" PRIu64 " last_ports=%02x/%02x last_write=%02x RAM8000=%02x map=SOURCE_UNMAPPED transport=UNIMPLEMENTED\n",
                s.io_reads,s.io_writes,s.last_read_port,s.last_write_port,s.last_write_data,tecmo_link_peek(board.tecmo_link,0x8000));
        }
        return;
    }
    capcom_zn_get_status(board.capcom, &status);
    qsound_hle_get_status(board.qsound, &qstatus);
    qsound_hle_read_register(board.qsound, 0x78, &qreg78);
    qsound_hle_read_register(board.qsound, 0x01, &qreg1);
    qsound_hle_read_register(board.qsound, 0x02, &qreg2);
    qsound_hle_read_register(board.qsound, 0x04, &qreg4);
    qsound_hle_read_register(board.qsound, 0x05, &qreg5);
    qsound_hle_read_register(board.qsound, 0x06, &qreg6);
    sound_f010 = capcom_zn_sound_read(board.capcom, 0xf010);
    sound_f020 = capcom_zn_sound_read(board.capcom, 0xf020);
    fprintf(out, "SystemZN: profile=%s model=%s family=%s CPU=%u GPU=2 RAM=%u VRAM=%u\n",
        board.profile->set, board.profile->model, board.profile->family,
        board.profile->cpu_clock, board.profile->ram_size, board.profile->vram_size);
    fprintf(out, "SystemZN: inputs=%02x/%02x/%02x/%02x/%02x/%02x KICK=%08x/%08x\n",
        board.inputs[0], board.inputs[1], board.inputs[2], board.inputs[3], board.inputs[4], board.inputs[5],
        board.kicks[0], board.kicks[1]);
    fprintf(out, "SystemZN: Z80 cycles=%" PRIu64 " bank=%u IRQ=%" PRIu64 " latch=%u faults=%u\n",
        status.sound_cycles, status.sound_bank, status.irq_ticks, status.sound_latch_pending, board.faults | status.faults);
    fprintf(out, "SystemZN: Z80 pc=%04x instructions=%" PRIu64 " unsupported=%" PRIu64 " NMIs=%" PRIu64 "\n",
        status.z80_pc, status.z80_instructions, status.z80_unsupported, status.z80_nmis);
    fprintf(out, "SystemZN: QSound rate=%u frames=%" PRIu64 " peak=%u WAV_bytes=%" PRIu64 " firmware=0x1000words samples=%u\n",
        qstatus.sample_rate, qstatus.frames, board.peak, board.wav.bytes, qstatus.sample_region_bank);
    fprintf(out, "SystemZN: QSound commands=%" PRIu64 " state=%04x next=%04x ready=%u output=%d/%d voice0=%d regs78/1/2/4/5/6=%04x/%04x/%04x/%04x/%04x/%04x Z80ram[f010/f020]=%02x/%02x\n",
        qstatus.commands, qstatus.state, qstatus.next_state, qstatus.ready,
        qstatus.output[0], qstatus.output[1], qstatus.voice_output[0],
        qreg78, qreg1, qreg2, qreg4, qreg5, qreg6, sound_f010, sound_f020);
    fprintf(out, "SystemZN: MCU/EEPROM time_us=%" PRIu64 " SIO0=%" PRIu64 " SIO1=%" PRIu64 "\n",
        cycles_to_us(board.peripheral_cycles), psx_sio_elapsed_cycles(board.sio[0]),
        psx_sio_elapsed_cycles(board.sio[1]));
}
