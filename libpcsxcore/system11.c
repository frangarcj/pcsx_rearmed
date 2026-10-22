/* GPL-2.0-or-later. Native host wiring for MAME-derived System 11 devices. */
#include "plugins.h"
#include "system11.h"
#include "system12.h"
#include "systemtaito.h"
#include "systemzn.h"
#include "system10.h"
#include "psxmem.h"
#include "r3000a.h"
#include "psxinterpreter.h"
#include "arcade/arcade_media.h"
#include "arcade/namcos11_board.h"
#include "arcade/pcm_wav.h"
#include "../include/gpu_vram.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#ifdef SYSTEM12_BOARD_THREAD
#include <stdatomic.h>
#include <retro_timers.h>
#include "../frontend/pcsxr-threads.h"
#endif
#ifndef DRC_DISABLE
#include "new_dynarec/new_dynarec.h"
#endif

typedef struct Profile {
    const char *set, *parent, *model, *banking, *keycus, *lightgun, *machine, *input, *flags;
} Profile;
static const Profile profiles[] = {
#include "arcade/system11_profiles.inc"
};
struct System11InputBinding {
    const char *profile, *control;
    unsigned analog, index, mask;
};
struct System11InputDefault {
    const char *profile;
    unsigned analog, index, value;
};
#include "arcade/system11_inputs.inc"
static struct {
    Namcos11Board *device;
    Namcos11Config config;
    const Profile *profile;
    uint8_t *regions[5];
    uint8_t inputs[4];
    uint16_t analog[12];
    unsigned leds, recoil, peak;
    uint32_t random, last_cycle;
    uint64_t fraction;
    PcmWav wav;
    void (*audio_callback)(void *, const int16_t *, unsigned);
    void *audio_opaque;
#ifdef SYSTEM12_BOARD_THREAD
    atomic_int fault;
#else
    int fault;
#endif
    unsigned mapped_banks[8];
} board;


/* Same worker option and Vita affinity as System 12: C76 + C352 on core 2.
 * Only cycle advances are queued. Main-CPU device accesses fence the queue,
 * so shared RAM, banking, KEYCUS and EEPROM keep their original ordering. */
#ifdef SYSTEM12_BOARD_THREAD
#define S11_QUEUE 64
#define S11_AUDIO_FRAMES 65536
static struct {
    sthread_t *thread;
    slock_t *lock;
    scond_t *work, *done_cond;
    u32 cycles[S11_QUEUE];
    unsigned posted;
    atomic_uint done, audio_head, audio_tail, device_fault;
    int quit, active;
    int16_t audio[S11_AUDIO_FRAMES * 2];
    u64 commands, worker_cycles;
} bt;

static void board_audio_pump(void)
{
    for (;;) {
        unsigned head = atomic_load_explicit(&bt.audio_head, memory_order_acquire);
        unsigned tail = atomic_load_explicit(&bt.audio_tail, memory_order_relaxed);
        unsigned count = head - tail, at = tail % S11_AUDIO_FRAMES;
        if (!count) break;
        if (count > S11_AUDIO_FRAMES - at) count = S11_AUDIO_FRAMES - at;
        if (board.audio_callback)
            board.audio_callback(board.audio_opaque, bt.audio + at * 2, count);
        atomic_store_explicit(&bt.audio_tail, tail + count, memory_order_release);
    }
}

static void board_audio_push(const int16_t *samples, unsigned frames)
{
    while (frames) {
        unsigned head = atomic_load_explicit(&bt.audio_head, memory_order_relaxed);
        unsigned tail = atomic_load_explicit(&bt.audio_tail, memory_order_acquire);
        unsigned count = S11_AUDIO_FRAMES - (head - tail), at = head % S11_AUDIO_FRAMES;
        if (!count) {
            slock_lock(bt.lock);
            scond_signal(bt.done_cond);
            slock_unlock(bt.lock);
            retro_sleep(1);
            continue;
        }
        if (count > frames) count = frames;
        if (count > S11_AUDIO_FRAMES - at) count = S11_AUDIO_FRAMES - at;
        memcpy(bt.audio + at * 2, samples, (size_t)count * 4);
        atomic_store_explicit(&bt.audio_head, head + count, memory_order_release);
        samples += count * 2; frames -= count;
    }
}

static STRHEAD_RET_TYPE board_thread(void *unused)
{
    (void)unused;
    slock_lock(bt.lock);
    for (;;) {
        unsigned next = atomic_load_explicit(&bt.done, memory_order_relaxed);
        Namcos11Status status;
        u32 cycles;
        if (next == bt.posted) {
            if (bt.quit) break;
            scond_wait(bt.work, bt.lock);
            continue;
        }
        cycles = bt.cycles[next % S11_QUEUE];
        slock_unlock(bt.lock);
        namcos11_run_cycles(board.device, cycles);
        bt.commands++; bt.worker_cycles += cycles;
        namcos11_get_status(board.device, &status);
        atomic_store_explicit(&bt.device_fault, status.fault, memory_order_relaxed);
        slock_lock(bt.lock);
        atomic_store_explicit(&bt.done, next + 1, memory_order_release);
        scond_signal(bt.done_cond);
    }
    slock_unlock(bt.lock);
    STRHEAD_RETURN();
}

static void board_wait(unsigned target)
{
    if ((int)(atomic_load_explicit(&bt.done, memory_order_acquire) - target) >= 0)
        return;
    slock_lock(bt.lock);
    while ((int)(atomic_load_explicit(&bt.done, memory_order_acquire) - target) < 0) {
        if (atomic_load_explicit(&bt.audio_head, memory_order_acquire) !=
            atomic_load_explicit(&bt.audio_tail, memory_order_relaxed)) {
            slock_unlock(bt.lock);
            board_audio_pump();
            slock_lock(bt.lock);
        } else scond_wait(bt.done_cond, bt.lock);
    }
    slock_unlock(bt.lock);
}
#endif

static void board_drain(void)
{
#ifdef SYSTEM12_BOARD_THREAD
    if (bt.active) {
        board_wait(bt.posted);
        board_audio_pump();
    }
#endif
}

static void board_run(u32 cycles)
{
#ifdef SYSTEM12_BOARD_THREAD
    if (bt.active) {
        board_wait(bt.posted - S11_QUEUE + 1);
        bt.cycles[bt.posted % S11_QUEUE] = cycles;
        slock_lock(bt.lock);
        bt.posted++;
        scond_signal(bt.work);
        slock_unlock(bt.lock);
        return;
    }
#endif
    namcos11_run_cycles(board.device, cycles);
}

int system11_board_thread_active(void)
{
#ifdef SYSTEM12_BOARD_THREAD
    return bt.active;
#else
    return 0;
#endif
}

int system11_board_thread_start(void)
{
#ifdef SYSTEM12_BOARD_THREAD
    Namcos11Status status;
    if (bt.active) return 0;
    if (!board.device) return -1;
    bt.lock = slock_new(); bt.work = scond_new(); bt.done_cond = scond_new();
    if (bt.lock && bt.work && bt.done_cond) {
        bt.posted = 0; bt.quit = 0; bt.commands = bt.worker_cycles = 0;
        atomic_store(&bt.done, 0);
        atomic_store(&bt.audio_head, 0); atomic_store(&bt.audio_tail, 0);
        namcos11_get_status(board.device, &status);
        atomic_store(&bt.device_fault, status.fault);
        bt.active = 1;
        bt.thread = pcsxr_sthread_create(board_thread, PCSXRT_BOARD);
        if (bt.thread) {
            SysPrintf("System11: board thread started (C76 + C352)\n");
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

void system11_board_thread_stop(void)
{
#ifdef SYSTEM12_BOARD_THREAD
    if (!bt.active) return;
    board_drain();
    slock_lock(bt.lock);
    bt.quit = 1; scond_signal(bt.work);
    slock_unlock(bt.lock);
    sthread_join(bt.thread); bt.thread = NULL; bt.active = 0;
    scond_free(bt.done_cond); scond_free(bt.work); slock_free(bt.lock);
    bt.done_cond = bt.work = NULL; bt.lock = NULL;
    SysPrintf("System11: board thread stopped\n");
#endif
}

void system11_audio_pump(void)
{
#ifdef SYSTEM12_BOARD_THREAD
    if (bt.active) board_audio_pump();
#endif
}

#ifndef DRC_DISABLE
static int system11_code_span(unsigned int pc, const unsigned int **words,
                              unsigned int *limit)
{
    u32 address = pc >= 0x80000000u && pc < 0xc0000000u ? pc & 0x1fffffffu : pc;
    unsigned window;
    size_t offset;
    if (!board.device || !board.config.banking || (pc & 3) ||
        address < 0x1f000000u || address >= 0x1f800000u)
        return 0;
    window = (address - 0x1f000000u) / 0x100000;
    /* A delay slot can belong to another independently selected window. */
    if ((address & 0xfffff) == 0xffffc) return -1;
    offset = (size_t)board.mapped_banks[window] * 0x100000 + (address & 0xfffff);
    if (offset + 4 > board.config.bank_size) return 0;
    *words = (const unsigned int *)(board.regions[1] + offset);
    *limit = pc + 0x100000 - (address & 0xfffff);
    return 1;
}
#endif

static uint16_t input_read(void *opaque, unsigned input)
{ (void)opaque; return input < 4 ? board.inputs[input] : 0xff; }
static uint16_t analog_read(void *opaque, unsigned channel)
{ (void)opaque; return channel < 8 ? board.analog[channel] : 0xff; }
static uint16_t gun_read(void *opaque, unsigned channel)
{ (void)opaque; return channel < 4 ? board.analog[8 + channel] : 0xffff; }
static void gun_outputs(void *opaque, unsigned leds, unsigned recoil)
{ (void)opaque; board.leds = leds; board.recoil = recoil; }
static uint16_t random_read(void *opaque)
{
    (void)opaque;
    /* running_machine::rand(), pinned MAME src/emu/machine.cpp. */
    board.random = board.random * 1664525u + 1013904223u;
    return board.random >> 16;
}
static void audio_output(void *opaque, const int16_t *samples, size_t frames)
{
    size_t i; (void)opaque;
    for (i = 0; i < frames * 4; i++) {
        unsigned amplitude = samples[i] < 0 ? -(int)samples[i] : samples[i];
        if (amplitude > board.peak) board.peak = amplitude;
    }
    if (pcm_wav_append4(&board.wav, samples, frames)) board.fault = 1;
    if (board.audio_callback) {
        int16_t stereo[512];
        while (frames) {
            unsigned count = frames > 256 ? 256 : (unsigned)frames;
            for (i = 0; i < count; i++) {
                stereo[i * 2] = samples[i * 4];
                stereo[i * 2 + 1] = samples[i * 4 + 1];
            }
#ifdef SYSTEM12_BOARD_THREAD
            if (bt.active) board_audio_push(stereo, count);
            else
#endif
                board.audio_callback(board.audio_opaque, stereo, count);
            samples += count * 4; frames -= count;
        }
    }
}
static int read_config(const char *directory, const Profile **profile)
{
    static const char *keys[] = {"format", "mame_set", "mame_parent", "board_model", "banking",
        "keycus", "lightgun", "mame_machine", "mame_input", "mame_flags", "board_execution_verified"};
    char path[4096], line[512], values[11][128] = {{0}};
    unsigned seen = 0, i, j; FILE *file;
    if (snprintf(path, sizeof(path), "%s/board.ini", directory) >= (int)sizeof(path)) return -1;
    file = arcade_media_open_text(path);
    if (!file) { fprintf(stderr, "%s: cannot open board.ini\n", path); return -1; }
    while (fgets(line, sizeof(line), file)) {
        char *equal, *end;
        if (!strchr(line, '\n') && !feof(file)) goto bad;
        end = line + strcspn(line, "\r\n"); *end = 0;
        if (!*line) continue;
        equal = strchr(line, '='); if (!equal || strchr(equal + 1, '=')) goto bad;
        *equal++ = 0;
        for (i = 0; i < 11 && strcmp(line, keys[i]); i++) { }
        if (i == 11 || (seen & (1u << i)) || !*equal || strlen(equal) >= sizeof(values[i])) goto bad;
        strcpy(values[i], equal); seen |= 1u << i;
    }
    if (ferror(file) || seen != 0x7ff || strcmp(values[0], "pcsx-aow-system11-v1") ||
        strcmp(values[10], "false")) goto bad;
    for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) if (!strcmp(values[1], profiles[i].set)) {
        const char *expected[] = {profiles[i].set, profiles[i].parent, profiles[i].model,
            profiles[i].banking, profiles[i].keycus, profiles[i].lightgun,
            profiles[i].machine, profiles[i].input, profiles[i].flags};
        for (j = 0; j < 9; j++) if (strcmp(values[j + 1], expected[j])) goto bad;
        if (strstr(profiles[i].flags, "MACHINE_NOT_WORKING")) goto bad;
        *profile = &profiles[i]; fclose(file); return 0;
    }
bad:
    fprintf(stderr, "%s: invalid or unsupported System11 board profile\n", path);
    fclose(file); return -1;
}
static int load_region(const char *directory, const char *name, unsigned index,
    size_t minimum, size_t maximum, size_t *size)
{
    char path[4096]; size_t length;
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path)) return -1;
    if (arcade_media_size(path, &length)) { fprintf(stderr, "%s: cannot open region\n", path); return -1; }
    if (length < minimum || length > maximum) {
        fprintf(stderr, "%s: invalid System11 region size (expected %zu..%zu bytes)\n", path, minimum, maximum);
        return -1;
    }
    *size = length;
    board.regions[index] = malloc(*size ? *size : 1);
    if (!board.regions[index]) return -1;
    return arcade_media_read(path, board.regions[index], *size) ? -1 : 0;
}
int system11_load(const char *directory)
{
    Namcos11Config *cfg = &board.config;
    unsigned i;
    if (board.device || systemtaito_active() || systemzn_active() || system10_active() || system12_active() || psxRegs.ptrs.psxM || !directory) return -1;
    if (read_config(directory, &board.profile)) return -1;
    cfg->model = !strcmp(board.profile->model, "COH100") ? NAMCOS11_COH100 : NAMCOS11_COH110;
    cfg->banking = !strcmp(board.profile->banking, "NONE") ? NAMCOS11_BANK_NONE :
        !strcmp(board.profile->banking, "ROM8") ? NAMCOS11_BANK_ROM8 : NAMCOS11_BANK_ROM8_64;
    cfg->keycus = (unsigned)atoi(board.profile->keycus + 1);
    cfg->lightgun = !strcmp(board.profile->lightgun, "true");
    if (load_region(directory, "main.bin", 0, 0x400000, 0x400000, &cfg->main_size) ||
        load_region(directory, "banked.bin", 1, cfg->banking ? 0x100000 : 0, cfg->banking ? 0x4000000 : 0, &cfg->bank_size) ||
        load_region(directory, "c76.bin", 2, 0x80000, 0x80000, &cfg->c76_size) ||
        load_region(directory, "c76bios.bin", 3, 0x4000, 0x4000, &cfg->c76_bios_size) ||
        load_region(directory, "wave.bin", 4, 1, 0x1000000, &cfg->wave_size)) goto bad;
    cfg->main_rom = board.regions[0]; cfg->bank_rom = board.regions[1];
    cfg->c76_data = board.regions[2]; cfg->c76_bios = board.regions[3]; cfg->wave_rom = board.regions[4];
    cfg->input = input_read; cfg->analog = analog_read;
    cfg->gun = gun_read; cfg->gun_outputs = gun_outputs;
    cfg->random16 = random_read; cfg->audio = audio_output;
    memset(board.inputs, 0xff, sizeof(board.inputs));
    for (i = 0; i < 12; i++) board.analog[i] = 0xff;
    for (i = 0; i < sizeof(input_defaults) / sizeof(input_defaults[0]); i++) {
        const struct System11InputDefault *d = &input_defaults[i];
        if (!strcmp(d->profile, board.profile->input)) {
            if (d->analog) board.analog[d->index] = d->value;
            else board.inputs[d->index] = d->value;
        }
    }
    board.random = 0x9d14abd7;
    board.device = namcos11_create(cfg);
    if (!board.device || gpu_set_model(namcos11_gpu_model(board.device)) || gpu_set_vram_height(1024)) goto bad;
    psxRamSize = 0x400000; psxRomSize = 0x400000;
    /* System 11 has no PlayStation SPU: the C352 is its sound. */
    psxSetSpuPresent(0);
#ifndef DRC_DISABLE
    new_dynarec_set_code_span_provider(system11_code_span);
#endif
    return 0;
bad:
    fprintf(stderr, "System11: board initialization failed\n"); system11_unload(); return -1;
}
void system11_unload(void)
{
    unsigned i;
    system11_board_thread_stop();
#ifndef DRC_DISABLE
    new_dynarec_set_code_span_provider(NULL);
#endif
    psxSetSpuPresent(1);
    pcm_wav_close(&board.wav); namcos11_destroy(board.device);
    for (i = 0; i < 5; i++) free(board.regions[i]);
    memset(&board, 0, sizeof(board));
    if (!psxRegs.ptrs.psxM && !systemtaito_active() && !systemzn_active() && !system10_active() && !system12_active()) {
        psxRamSize = 0x200000; psxRomSize = 0x80000;
        gpu_set_model(2); gpu_set_vram_height(512);
    }
}
int system11_active(void) { return board.device != NULL; }
static void map_banks(void)
{
    Namcos11Status status;
    unsigned bank, segment, page;
    const u32 bases[] = {0x1f000000, 0x9f000000, 0xbf000000};
    if (!board.device || !board.config.banking || DISABLE_MEM_LUTS) return;
    namcos11_get_status(board.device, &status);
    for (bank = 0; bank < 8; bank++) {
#ifndef DRC_DISABLE
        if (psxCpu == &psxRec && board.mapped_banks[bank] != status.banks[bank])
            new_dynarec_invalidate_range_hard(0x1f000000u + bank * 0x100000,
                                            0x1f100000u + bank * 0x100000);
#endif
        board.mapped_banks[bank] = status.banks[bank];
        for (segment = 0; segment < 3; segment++)
        for (page = 0; page < 0x100000; page += 1u << PSXM_SHIFT) {
            u32 address = bases[segment] + bank * 0x100000 + page;
            psxRegs.ptrs.memRLUT[address >> PSXM_SHIFT] =
                (uintptr_t)(board.regions[1] + (size_t)status.banks[bank] * 0x100000) -
                (bases[segment] + bank * 0x100000);
            psxRegs.ptrs.memWLUT[address >> PSXM_SHIFT] = INVALID_PTR_VAL;
        }
    }
}
void system11_mem_reset(void)
{
    board_drain();
    memcpy(psxRegs.ptrs.psxR, board.regions[0], board.config.main_size);
    memset(psxRegs.ptrs.psxM, 0, PSX_RAM_SIZE);
    memset(psxRegs.ptrs.psxP, 0xff, 0x10000);
}
void system11_reset(void)
{
    board_drain();
#ifdef SYSTEM12_BOARD_THREAD
    atomic_store(&bt.device_fault, 0);
#endif
    namcos11_reset(board.device); map_banks();
    board.last_cycle = psxRegs.cycle; board.fraction = 0; board.peak = 0; board.fault = 0;
}
int system11_sync(u32 cycle)
{
    Namcos11Status status; u32 elapsed;
    if (!board.device) return 0;
    elapsed = cycle - board.last_cycle;
    if (elapsed >= 256) {
        board.last_cycle = cycle; board.fraction += (u64)elapsed * 8467200;
        board_run(board.fraction / PSXCLK);
        board.fraction %= PSXCLK;
    }
#ifdef SYSTEM12_BOARD_THREAD
    if (bt.active) return board.fault || atomic_load(&bt.device_fault) ? -1 : 0;
#endif
    namcos11_get_status(board.device, &status);
    return board.fault || status.fault ? -1 : 0;
}
static u32 physical_address(u32 address)
{
    return address >= 0x80000000u && address < 0xc0000000u ?
        address & 0x1fffffffu : address;
}
int system11_read(u32 address, unsigned width, u32 *value)
{
    int result;
    if (!board.device) return 0;
    if (system11_sync(psxRegs.cycle)) board.fault = 1;
    /* Core RAM/GPU/SIO accesses need no board fence. */
    u32 physical = physical_address(address);
    if (!((physical >= 0x1f000000 && physical < 0x1f800000) ||
          (physical >= 0x1fa04000 && physical < 0x1fa31000) ||
          (physical >= 0x1fc00000 && physical < 0x20000000))) return 0;
    board_drain();
    result = namcos11_read(board.device, physical, width, value);
    if (result < 0) { board.fault = 1; *value = UINT32_MAX; }
    return result != 0;
}
int system11_write(u32 address, unsigned width, u32 value)
{
    int result; u32 physical = physical_address(address);
    if (!board.device) return 0;
    if (system11_sync(psxRegs.cycle)) board.fault = 1;
    if (!((physical >= 0x1f000000 && physical < 0x1f800000) ||
          (physical >= 0x1fa04000 && physical < 0x1fa31000) ||
          (physical >= 0x1fb00000 && physical < 0x1fb00004) ||
          (physical >= 0x1fbf6000 && physical < 0x1fbf6004))) return 0;
    board_drain();
    result = namcos11_write(board.device, physical, width, value);
    if (result < 0) board.fault = 1;
    if (result > 0 && ((physical >= 0x1fa10020 && physical < 0x1fa10030) ||
        (physical >= 0x1f080000 && physical < 0x1f080004))) map_banks();
    return result != 0;
}
int system11_set_input(const char *name, int pressed)
{
    unsigned i;
    if (!board.device || !name || (pressed != 0 && pressed != 1)) return -1;
    for (i = 0; i < sizeof(input_bindings) / sizeof(input_bindings[0]); i++) {
        const struct System11InputBinding *b = &input_bindings[i];
        if (!strcmp(b->profile, board.profile->input) && !strcmp(b->control, name)) {
            board_drain();
            unsigned old = b->analog ? board.analog[b->index] : board.inputs[b->index];
            unsigned value = pressed ? old & ~b->mask : old | b->mask;
            if (b->analog) board.analog[b->index] = value;
            else board.inputs[b->index] = value;
            return 0;
        }
    }
    return -1;
}
void system11_set_audio_output(void (*callback)(void *, const int16_t *, unsigned), void *opaque)
{ board_drain(); board.audio_callback = callback; board.audio_opaque = opaque; }
int system11_open_audio(const char *path) { board_drain(); return pcm_wav_open(&board.wav, path, 88200); }
int system11_close_audio(void) { board_drain(); return pcm_wav_close(&board.wav); }
int system11_eeprom_import(const unsigned char *image, size_t size)
{ board_drain(); return namcos11_eeprom_import(board.device, image, size); }
int system11_eeprom_export(unsigned char *image, size_t size)
{ board_drain(); return namcos11_eeprom_export(board.device, image, size); }
int system11_selftest(void)
{
    unsigned bank, segment, i, failures = 0;
    int threaded = system11_board_thread_active();
    u32 original, value; const u32 bases[] = {0x1f000000, 0x9f000000, 0xbf000000};
    if (!board.device) return -1;
    system11_board_thread_stop();
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"System11 selftest failed line %u: %s\n",__LINE__,#x); failures++; } } while(0)
    CHECK(PSX_RAM_SIZE == 0x400000 && !Config.HLE);
    CHECK(system11_read(0xfffe0130, 4, &value) == 0);
    CHECK(system11_write(0xfffe0130, 4, 0) == 0);
    CHECK(psxMemRead32(&psxRegs, 0xbfc00000) == SWAPu32(*(u32 *)board.regions[0]));
    psxMemWrite32(&psxRegs, 0x80300000, 0x11335577);
    CHECK(psxMemRead32(&psxRegs, 0xa0300000) == 0x11335577);
    CHECK(psxMemRead32(&psxRegs, 0x80100000) != 0x11335577);
    original = psxMemRead32(&psxRegs, 0x1fa04000);
    psxMemWrite32(&psxRegs, 0x1fa04000, 0x1234abcd);
    CHECK(m37710_read16(namcos11_c76(board.device), 0x4000) == 0xabcd);
    CHECK(m37710_read16(namcos11_c76(board.device), 0x4002) == 0x1234);
    m37710_write16(namcos11_c76(board.device), 0x4002, 0x5678);
    CHECK(psxMemRead32(&psxRegs, 0xbfa04000) == 0x5678abcd);
    psxMemWrite32(&psxRegs, 0x1fa04000, original);
    if (board.config.banking) for (bank = 0; bank < 8; bank++) {
        unsigned entry = bank % (board.config.bank_size / 0x100000);
        unsigned data = board.config.banking == NAMCOS11_BANK_ROM8 ?
            (entry & 3) | ((entry & 12) << 4) : (entry & 7) | ((entry & 24) << 3);
        psxMemWrite16(&psxRegs, 0x1fa10020 + bank * 2, data);
        value = SWAPu32(*(u32 *)(board.regions[1] + (size_t)entry * 0x100000));
        for (segment = 0; segment < 3; segment++) {
            u32 address = bases[segment] + bank * 0x100000;
            CHECK(psxMemRead32(&psxRegs, address) == value);
            CHECK(intFakeFetch(address) == value);
        }
    }
    /* Restore all main CPU RAM touched above; device sharing was restored. */
    for (i = 0; i < 4; i++) psxRegs.ptrs.psxM[0x300000 + i] = 0;
    fprintf(stderr, "System11 selftest: %s\n", failures ? "FAIL" : "PASS");
    psxReset();
    if (threaded && system11_board_thread_start()) failures++;
    return failures ? -1 : 0;
#undef CHECK
}
void system11_report(FILE *out)
{
    Namcos11Status status; M37710Status cpu;
    if (!board.device) return;
    board_drain();
    namcos11_get_status(board.device, &status); m37710_get_status(namcos11_c76(board.device), &cpu);
    fprintf(out, "System11: profile=%s model=%s GPU=%u RAM=%u banking=%u KEYCUS=%u\n",
        board.profile->set, board.profile->model, namcos11_gpu_model(board.device), PSX_RAM_SIZE, board.config.banking, board.config.keycus);
#ifdef SYSTEM12_BOARD_THREAD
    fprintf(out, "System11: board_thread=%u worker_commands=%" PRIu64 " worker_cycles=%" PRIu64 "\n",
        bt.active, bt.commands, bt.worker_cycles);
#endif
    fprintf(out, "System11: inputs=%02x/%02x/%02x/%02x\n",board.inputs[0],board.inputs[1],board.inputs[2],board.inputs[3]);
    fprintf(out, "System11: C76 pc=%06x cycles=%" PRIu64 " instructions=%" PRIu64 " illegal=%" PRIu64 " unsupported=%" PRIu64 " fault=%u\n",
        cpu.pc,cpu.cycles,cpu.instructions,cpu.illegal_opcodes,cpu.unsupported_accesses,status.fault || board.fault || cpu.fault);
    fprintf(out, "System11: C352 rate=88200 frames=%" PRIu64 " peak=%u WAV_bytes=%" PRIu64 "\n",status.samples,board.peak,board.wav.bytes);
    fprintf(out, "System11: IRQ0=%" PRIu64 " IRQ2=%" PRIu64 " KEYCUS unknown=%" PRIu64 "/%" PRIu64 " C76 unmapped=%" PRIu64 "/%" PRIu64 "\n",
        status.irq0_ticks,status.irq2_ticks,status.unknown_keycus_reads,status.unknown_keycus_writes,status.unmapped_c76_reads,status.unmapped_c76_writes);
}
