/* GPL-2.0-or-later. Native host wiring for source-derived Capcom/Tecmo/Raizing Sony ZN boards. */
#include "systemzn.h"
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
#include <math.h>
#include "gpu_vram.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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
#include "arcade/tecmo_tps_profiles.inc"
#include "arcade/raizing_zn_profiles.inc"

static const ZnProfile profiles[] = {
#include "arcade/capcom_zn_profiles.inc"
};

typedef struct SystemZn {
    CapcomZn *capcom;
    RaizingSound *raizing_sound;
    int raizing;
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
    uint32_t faults;
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

static const ZnProfile *find_profile(const char *set)
{
    size_t i;
    const ZnProfile *list = board.raizing ? raizing_profiles : board.tps ? tps_profiles : profiles;
    size_t count = board.raizing ? sizeof(raizing_profiles) / sizeof(raizing_profiles[0]) : board.tps ? sizeof(tps_profiles) / sizeof(tps_profiles[0]) : sizeof(profiles) / sizeof(profiles[0]);
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
    if (ferror(file) || seen != 0x3fff || strcmp(values[0], board.raizing ? "pcsx-aow-raizing-zn-v1" : board.tps ? "pcsx-aow-tecmo-tps-v1" : "pcsx-aow-capcom-zn-v1") ||
        strcmp(values[13], "false")) goto bad;
    p = find_profile(values[1]);
    if (!p || strcmp(values[2], p->parent) || strcmp(values[3], p->bios_parent) ||
        strcmp(values[5], p->model) || strcmp(values[6], p->family) ||
        strcmp(values[7], p->cpu_clock == 67737600 ? "CXD8530CQ" : "CXD8661R") ||
        strcmp(values[8], p->cpu_clock == 100000000u ? "100000000" : "67737600") ||
        strcmp(values[9], p->cpu_clock == 67737600 ? "CXD8561Q" : "CXD8654Q") ||
        strcmp(values[10], "53693175") || strcmp(values[11], "4194304") ||
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
    return input >= ZN_INPUT_P1 && input <= ZN_INPUT_P4 ? board.inputs[input] : 0xff;
}
static uint32_t kick_read(void *opaque, unsigned player)
{
    (void)opaque;
    return player < 2 ? board.kicks[player] : UINT32_MAX;
}
static uint8_t mcu_analog(void *opaque) { (void)opaque; return 0xff; }
static uint16_t mcu_trackball(void *opaque) { (void)opaque; return 0; }
static uint8_t mcu_dsw(void *opaque) { (void)opaque; return 0xff; }

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
    size_t profile = (size_t)(board.profile - (board.raizing ? raizing_profiles : tps_profiles));
    const TpsMediaCheck *checks = board.raizing ? raizing_media[profile] : tps_media[profile];
    size_t count = board.raizing ? raizing_media_count[profile] : tps_media_count[profile];
    static const unsigned regions[] = {0, 1, 5, 6, 2, 3, 4};
    for (unsigned r = 0; r < (board.raizing ? 7u : 4u); ++r) {
        unsigned region = regions[r];
        uint8_t *marked = calloc(board.sizes[region], 1);
        if (!marked) return -1;
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
    board.synthetic = synthetic;
    if (load_config(directory, &board.profile)) { memset(&board, 0, sizeof(board)); return -1; }
    if (load_region(directory, files[0], 0, board.profile->bios_size) ||
        load_region(directory, files[1], 1, board.profile->banked_size) ||
        load_region(directory, files[5], 5, 8) || load_region(directory, files[6], 6, 8)) goto bad;
    if (board.raizing) {
        if (load_region(directory, "eeprom_default.bin", 2, board.profile->country_size) ||
            load_region(directory, "audiocpu.bin", 3, board.profile->audio_size) ||
            load_region(directory, "ymf_samples.bin", 4, board.profile->qsound_size)) goto bad;
        if (!synthetic && validate_tps_media()) goto bad;
        RaizingSoundConfig cfg = {board.regions[3], board.regions[4], board.sizes[3],
            board.sizes[4], &board, raizing_pcm};
        if (raizing_sound_create(&cfg, &board.raizing_sound)) goto bad;
    } else if (board.tps) {
        if (!synthetic && validate_tps_media()) goto bad;
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
    if (board.raizing) {
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
    if (systemzn_sync(psxRegs.cycle)) goto bad;
    return 0;
bad:
    fprintf(stderr, "SystemZN: media/profile initialization failed\n");
    systemzn_unload();
    return -1;
}

int systemzn_load(const char *directory) { return load_board(directory, 0, 0); }
int systemzn_load_tps(const char *directory) { return load_board(directory, 1, 0); }
int systemzn_test_load_tps(const char *directory) { return load_board(directory, 1, 1); }
int systemzn_load_raizing(const char *directory) { return load_board(directory, 2, 0); }
int systemzn_test_load_raizing(const char *directory) { return load_board(directory, 2, 1); }
int systemzn_raizing_active(void) { return board.active && board.raizing; }
unsigned systemzn_rotation(void) { return board.active && board.raizing && !strcmp(board.profile->set, "brvblade") ? 270 : 0; }
int systemzn_tps_active(void) { return board.active && board.tps; }

void systemzn_unload(void)
{
    unsigned i;
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
            for (page = 0xb80000u; page < 0xc00000u; page += SYSTEMZN_ROM_PAGE) {
                unsigned index = (bases[segment] + page) >> PSXM_SHIFT;
                psxRegs.ptrs.memRLUT[index] = INVALID_PTR_VAL;
                psxRegs.ptrs.memWLUT[index] = INVALID_PTR_VAL;
            }
        }
    }
    pcm_wav_close(&board.wav);
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
        uintptr_t offset = (uintptr_t)data - (uintptr_t)address;
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
    if (board.tps || board.raizing) {
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
    capcom_zn_get_status(board.capcom, &status);
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
    if (!board.active || !psxRegs.ptrs.psxM) return;
    memcpy(psxRegs.ptrs.psxR, board.regions[0], board.sizes[0]);
    memset(psxRegs.ptrs.psxM, 0, psxRamSize);
    memset(psxRegs.ptrs.psxP, 0xff, 0x10000);
    map_bankedroms();
    if (board.raizing) return;
    map_window(0x1fb80000u, board.regions[2], board.sizes[2], 0);
    map_window(0x9fb80000u, board.regions[2], board.sizes[2], 0);
    map_window(0xbfb80000u, board.regions[2], board.sizes[2], 0);
}

void systemzn_reset(void)
{
    if (!board.active) return;
    /* MAME has no device_reset override for the shared ZN MCU, CAT702, AT28C16
     * or PSX SIO devices; preserve their pin/timer/storage state on soft reset. */
    board.faults = 0;
    if (board.tps || board.raizing) board.tps_bank = 0;
    else capcom_zn_reset(board.capcom);
    if (board.raizing) {
        if (raizing_sound_reset(board.raizing_sound)) board.faults |= 8;
        RaizingSoundStatus status; raizing_sound_status(board.raizing_sound, &status);
        board.sound_cycles = status.cpu_cycles;
    }
    board.qsound_fraction = 0; if (!board.raizing) board.sound_cycles = 0;
    board.frames = 0;
    board.last_psx_cycle = psxRegs.cycle;
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
        sound_cycles = 0;
        if (!board.tps) {
            numerator = (uint64_t)elapsed * (board.raizing ? RAIZING_SOUND_CLOCK : CAPCOM_ZN_SOUND_CLOCK_HZ) + board.qsound_fraction;
            sound_cycles = numerator / PSXCLK;
            board.qsound_fraction = numerator % PSXCLK;
        }
        /* SIO bit timers use the source's fixed 33.8688 MHz clock even on
         * ZN-2. MCU/EEPROM callbacks share that absolute time domain. */
        numerator = (uint64_t)elapsed * SYSTEMZN_PSX_CLOCK + board.sio_fraction;
        board.sio_cycles += numerator / PSXCLK;
        board.sio_fraction = numerator % PSXCLK;
        board.sound_cycles += sound_cycles;
        if (board.raizing && raizing_sound_sync(board.raizing_sound, board.sound_cycles)) board.faults |= 8u;
        if (!board.tps && !board.raizing && sound_cycles && capcom_zn_run_sound_cycles(board.capcom, sound_cycles) < 0)
            board.faults |= 8u;
    }
    sio_bus_advance(board.sio[0], board.sio_cycles);
    sio_bus_advance(board.sio[1], board.sio_cycles);
    if (sync_peripherals_to_cycle(board.sio_cycles)) board.faults |= 1u;
    if (!board.tps && !board.raizing && qsound_hle_get_status(board.qsound, &status)) board.faults |= 2u;
    if (!board.tps && !board.raizing) {
        capcom_zn_status_t cap_status;
        capcom_zn_get_status(board.capcom, &cap_status);
        if (cap_status.faults) board.faults |= 8u;
    }
    return board.faults ? -1 : 0;
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
    if (systemzn_sync(psxRegs.cycle)) return -1;
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
    if (systemzn_sync(psxRegs.cycle)) return -1;
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
    result = handle_sio_read(physical, width, value);
    if (result) { if (result < 0) { board.faults |= 4u; *value = UINT32_MAX; return 1; } return 1; }
    if ((board.tps || board.raizing) && physical >= 0x1f000000 && physical < 0x1f800000) {
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
    result = board.tps || board.raizing ? 0 : capcom_zn_main_read(board.capcom, physical, width, value);
    if (result == CAPCOM_ZN_HANDLED) return 1;
    if (result < 0) { board.faults |= 8u; *value = UINT32_MAX; return 1; }
    if ((physical >= 0x1fa00000 && physical < 0x1fa40000) ||
        (physical >= 0x1fa10000 && physical < 0x1fa10400) ||
        (physical >= 0x1faf0000 && physical < 0x1faf0800))
        if (systemzn_sync(psxRegs.cycle)) board.faults |= 1u;
    result = zn_board_read(&board.common, physical, width, value);
    return result == ZN_BOARD_HANDLED;
}

int systemzn_write(u32 address, unsigned width, u32 value)
{
    uint32_t physical = canonical_address(address);
    int result;
    if (!board.active) return 0;
    result = handle_sio_write(physical, width, value);
    if (result) { if (result < 0) board.faults |= 4u; return 1; }
    if (physical == 0x1fb60000 && systemzn_sync(psxRegs.cycle)) board.faults |= 8u;
    if (board.tps && physical <= 0x1fb00006 && (uint64_t)physical + width > 0x1fb00006) {
        board.tps_bank = (value >> ((0x1fb00006 - physical) * 8)) & 0xff;
        board.tps_bank_writes++;
        if (board.tps_bank >= 9 || (size_t)(board.tps_bank + 1) * 0x800000 > board.sizes[1])
            board.faults |= 8u;
        map_bankedroms();
        return 1;
    }
    if (board.raizing && ((physical <= 0x1fb00000 && (uint64_t)physical + width > 0x1fb00000) ||
        (physical <= 0x1fb00004 && (uint64_t)physical + width > 0x1fb00004))) {
        if (systemzn_sync(psxRegs.cycle)) board.faults |= 8u;
        if (physical <= 0x1fb00000 && (uint64_t)physical + width > 0x1fb00000) {
            if (raizing_sound_command(board.raizing_sound, value >> ((0x1fb00000 - physical) * 8))) board.faults |= 8;
        } else if (raizing_sound_irq(board.raizing_sound)) board.faults |= 8;
        return 1;
    }
    result = board.tps || board.raizing ? 0 : capcom_zn_main_write(board.capcom, physical, width, value);
    if (result == CAPCOM_ZN_HANDLED) { if (physical == 0x1fb00000) map_bankedroms(); return 1; }
    if (result < 0) { board.faults |= 8u; return 1; }
    if ((physical >= 0x1fa00000 && physical < 0x1fa40000) ||
        (physical >= 0x1fa10000 && physical < 0x1fa10400) ||
        (physical >= 0x1faf0000 && physical < 0x1faf0800))
        if (systemzn_sync(psxRegs.cycle)) board.faults |= 1u;
    result = zn_board_write(&board.common, physical, width, value);
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
    if (board.tps && (!strcmp(name, "coin2") || strstr(name, "button3"))) return -1;
    if (!strncmp(name, "p1", 2) || !strncmp(name, "p2", 2)) {
        unsigned player = name[1] == '1' ? 0 : 1;
        const char *control = name + 2;
        if (player && !strcmp(profile, "strider2")) return -1;
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
                if (player && !strcmp(profile, "strider2")) return -1;
                return set_raw_bit(port, 0x40, pressed);
            }
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
            if (player && !strcmp(profile, "strider2")) return -1;
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
{ return board.active ? zn_board_eeprom_import(&board.common, image, size) : -1; }
int systemzn_eeprom_export(unsigned char *image, size_t size)
{ return board.active ? zn_board_eeprom_export(&board.common, image, size) : -1; }
void systemzn_set_audio_output(void (*callback)(void *, const int16_t *, unsigned), void *opaque)
{ board.audio_callback = callback; board.audio_opaque = opaque; board.audio_buffer_frames = 0; }
void systemzn_set_ymf_audio_output(void (*callback)(void *, float, float, uint64_t), void *opaque)
{ board.ymf_callback = callback; board.ymf_opaque = opaque; }
int systemzn_open_audio(const char *path)
{
    if (!board.active || board.tps || board.wav.file) return -1;
    return pcm_wav_open(&board.wav, path, board.raizing ? 44100 : SYSTEMZN_QSOUND_RATE);
}
int systemzn_close_audio(void) { return pcm_wav_close(&board.wav); }

int systemzn_selftest(void)
{
    uint32_t value;
    unsigned before;
    if (!board.active) return -1;
    if (systemzn_read(0x1fa10200, 1, &value) != 1 ||
        value != (board.profile->vram_size == 0x200000 ? 0x69 : 0x61)) return -1;
    if (systemzn_read(0x1f000000, 4, &value) != 1 ||
        value != *(uint32_t *)board.regions[1]) return -1;
    if (!board.tps && !board.raizing && (systemzn_read(0x1fb80000, 4, &value) != 1 || value != *(uint32_t *)board.regions[2])) return -1;
    if (systemzn_read(0x1fa00300, 1, &value) != 1) return -1;
    before = value;
    if (systemzn_set_input("coin1", 1) || systemzn_read(0x1fa00300, 1, &value) != 1 ||
        value != (before & ~0x10u)) return -1;
    systemzn_set_input("coin1", 0);
    if (systemzn_set_input("unknown-control", 1) == 0) return -1;
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
