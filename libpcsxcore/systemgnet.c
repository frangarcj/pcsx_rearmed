/* GPL-2.0-or-later. Native host wiring for the MAME G-Net / Sony ZN board.
 * Source: MAME deb2f54fe7300e7a192690318beb860159bfd52a, sony/taitogn.cpp.
 * Device implementations retain their upstream BSD-3-Clause notices.
 */
#include "systemgnet.h"
#include "system10.h"
#include "system11.h"
#include "system12.h"
#include "systemtaito.h"
#include "systemzn.h"
#include "psxmem.h"
#include "r3000a.h"
#include "psxhw.h"
#include "arcade/gnet_zoom.h"
#include "arcade/gnet_storage.h"
#include "psxcommon.h"
#include "arcade/zn_board.h"
#include "arcade/zn_mcu.h"
#include "arcade/psx_sio.h"
#include "arcade/pcm_wav.h"
#include "gpu_vram.h"
#if defined(VITA) || defined(__vita__)
#include <openssl/sha.h>
#else
#include <CommonCrypto/CommonDigest.h>
#endif
#ifndef DRC_DISABLE
#include "new_dynarec/new_dynarec.h"
#endif
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GNET_SOURCE_COMMIT "deb2f54fe7300e7a192690318beb860159bfd52a"
#define PSX_CLOCK_HZ PSXCLK
#define MN_CLOCK_HZ 6250000u
#define BIOS_BYTES 0x80000u
#define FIRM_BYTES 0x200000u
#define EPROM_BYTES 0x100000u
#define KEY_BYTES 8u
#define EEPROM_BYTES 2080u
#define PATH_BYTES 4096
enum { GNET_SHA1_BYTES = 20 };

static int gnet_sha1(const void *data, size_t size,
                     unsigned char digest[GNET_SHA1_BYTES])
{
#if defined(VITA) || defined(__vita__)
    return SHA1((const unsigned char *)data, size, digest) ? 0 : -1;
#else
    if (size > UINT32_MAX) return -1;
    return CC_SHA1(data, (CC_LONG)size, digest) ? 0 : -1;
#endif
}

typedef struct GnetBiosRoot {
    const char *set, *model, *macro, *cpu_file, *cpu_crc, *cpu_sha1;
    const char *firm_file, *firm_crc, *firm_sha1;
    const char *key0_file, *key0_crc, *key0_sha1;
    const char *key1_file, *key1_crc, *key1_sha1;
} GnetBiosRoot;
typedef struct GnetBiosOption {
    const char *root, *name, *file, *crc, *sha1;
} GnetBiosOption;
typedef struct GnetProfile {
    const char *set;
    int year;
    const char *description, *parent, *bios_root, *bios_model;
    const char *bios_default, *machine, *input, *state, *init, *rotation;
    const char *manufacturer, *flags, *card, *card_kind, *chd, *chd_sha1;
    const char *metadata_policy;
} GnetProfile;

static const GnetBiosRoot bios_roots[] = {
#define GNET_BIOS_ROOT(...) { __VA_ARGS__ },
#define GNET_BIOS_OPTION(...)
#define GNET_METADATA_TAGS(...)
#define GNET_PROFILE(...)
#include "arcade/gnet_profiles.inc"
#undef GNET_BIOS_ROOT
#undef GNET_BIOS_OPTION
#undef GNET_METADATA_TAGS
#undef GNET_PROFILE
};
static const GnetBiosOption bios_options[] = {
#define GNET_BIOS_ROOT(...)
#define GNET_BIOS_OPTION(...) { __VA_ARGS__ },
#define GNET_METADATA_TAGS(...)
#define GNET_PROFILE(...)
#include "arcade/gnet_profiles.inc"
#undef GNET_BIOS_ROOT
#undef GNET_BIOS_OPTION
#undef GNET_METADATA_TAGS
#undef GNET_PROFILE
};
static const GnetProfile profiles[] = {
#define GNET_BIOS_ROOT(...)
#define GNET_BIOS_OPTION(...)
#define GNET_METADATA_TAGS(...)
#define MACHINE_IS_BIOS_ROOT "MACHINE_IS_BIOS_ROOT"
#define MACHINE_SUPPORTS_SAVE "MACHINE_SUPPORTS_SAVE"
#define GNET_PROFILE(...) { __VA_ARGS__ },
#include "arcade/gnet_profiles.inc"
#undef GNET_BIOS_ROOT
#undef GNET_BIOS_OPTION
#undef GNET_METADATA_TAGS
#undef MACHINE_IS_BIOS_ROOT
#undef MACHINE_SUPPORTS_SAVE
#undef GNET_PROFILE
};

typedef struct GnetBoard {
    const GnetProfile *profile;
    const GnetBiosRoot *bios_root;
    const GnetBiosOption *bios_option;
    uint8_t *bios, *key0, *key1, *firm, *eprom;
    uint8_t *zoomprog, *waves[3];
    gnet_zoom_board *zoom;
    gnet_storage_t *storage;
    int test_storage_fixture;
    char flash_state_path[PATH_BYTES];
    zn_board_t zn;
    ZNMCU *mcu;
    psx_sio_t *sio[2];
    uint8_t inputs[6], dsw;
    uint8_t analog[2], handles[2];
    uint16_t trackball[2], lamps;
    uint8_t weights[2];
    int cat_out[2], mcu_out, active, fault, zn_initialized;
    int memory_initialized, reset_pending;
    uint32_t old_ram_size, old_rom_size, last_cycle;
    uint64_t total_cycles, mn_cycles, mn_fraction;
    uint64_t reads, writes, unsupported;
    PcmWav wav;
    uint64_t audio_frames, audio_nonzero;
    unsigned audio_peak;
} GnetBoard;
static GnetBoard board;
static const char *test_storage_chd_path;
static systemgnet_audio_frame_fn audio_output;
static void *audio_output_opaque;

static void request_soft_reset(void *opaque)
{
    (void)opaque;
    board.reset_pending = 1;
}

static void zoom_audio_frame(void *opaque, float left, float right,
                             uint64_t zsg_cycle)
{
    (void)opaque;
    int16_t channels[4] = {0};
    float samples[2] = {left, right};
    for (unsigned i = 0; i < 2; i++) {
        if (!isfinite(samples[i])) { board.fault = 1; continue; }
        float clipped = fmaxf(-1.0f, fminf(1.0f, samples[i]));
        int value = (int)lrintf(clipped * 32768.0f);
        if (value > 32767) value = 32767;
        channels[i] = (int16_t)value;
        unsigned magnitude = value < 0 ? (unsigned)-value : (unsigned)value;
        if (magnitude > board.audio_peak) board.audio_peak = magnitude;
        if (value) board.audio_nonzero++;
    }
    board.audio_frames++;
    if (pcm_wav_append4(&board.wav, channels, 1)) board.fault = 1;
    if (audio_output) audio_output(audio_output_opaque, left, right, zsg_cycle);
}

static int open_test_storage(const char *chd_path)
{
    gnet_storage_config_t config;
    char overlay_path[PATH_BYTES], error[256];
    int length;
    memset(&config, 0, sizeof(config));
    length = snprintf(overlay_path, sizeof(overlay_path), "%s.overlay", chd_path);
    if (length < 0 || length >= (int)sizeof(overlay_path)) return -1;
    config.chd_path = chd_path;
    config.overlay_path = overlay_path;
    config.card_variant = GNET_PCCARD_TAITO_TYPE1;
    if (gnet_storage_open(&config, &board.storage, error, sizeof(error)) !=
        GNET_STORAGE_OK) {
        fprintf(stderr, "G-Net synthetic fixture CHD failed: %s\n", error);
        return -1;
    }
    board.test_storage_fixture = 1;
    return 0;
}

static void install_storage_guest_program(void)
{
    static const uint8_t key[5] = {0x10, 0x21, 0x32, 0x43, 0x54};
    uint32_t program[128];
    unsigned count = 0, i;
#define EMIT(x) do { program[count++] = (x); } while (0)
#define I(op, rs, rt, imm) (((uint32_t)(op) << 26) | ((uint32_t)(rs) << 21) | \
                            ((uint32_t)(rt) << 16) | ((uint16_t)(imm)))
#define LUI(rt, imm) EMIT(I(0x0f, 0, (rt), (imm)))
#define ORI(rt, rs, imm) EMIT(I(0x0d, (rs), (rt), (imm)))
#define ADDIU(rt, rs, imm) EMIT(I(0x09, (rs), (rt), (imm)))
#define SB(rt, off, rs) EMIT(I(0x28, (rs), (rt), (off)))
#define SH(rt, off, rs) EMIT(I(0x29, (rs), (rt), (off)))
#define LH(rt, off, rs) EMIT(I(0x21, (rs), (rt), (off)))
#define LBU(rt, off, rs) EMIT(I(0x24, (rs), (rt), (off)))
#define SW(rt, off, rs) EMIT(I(0x2b, (rs), (rt), (off)))
#define ANDI(rt, rs, imm) EMIT(I(0x0c, (rs), (rt), (imm)))
#define BEQ(rs, rt, imm) EMIT(I(0x04, (rs), (rt), (imm)))
#define BNE(rs, rt, imm) EMIT(I(0x05, (rs), (rt), (imm)))
#define NOP() EMIT(0)
#define LI(rt, value) do { LUI((rt), ((uint32_t)(value) >> 16)); \
                           ORI((rt), (rt), (uint32_t)(value)); } while (0)
    /* Type-1 card key through the PSX's PC Card memory window. */
    LUI(8, 0x1f20);
    for (i = 0; i < 9; ++i) {
        ADDIU(9, 0, i < 5 ? key[i] : 0);
        SH(9, 0x0500 + (int)i * 2, 8);
    }
    LI(8, 0x1f200000);
    LI(11, 0x80000100);
    EMIT(I(0x25, 8, 10, 0x0402)); /* lhu locked-register */
    NOP(); /* R3000 load delay */
    SW(10, 0, 11);

    /* ATA IDENTIFY PIO, then retain raw word 0 and the final word. */
    LI(8, 0x1fb00000);
    ADDIU(9, 0, 0xec); SB(9, 7, 8);
    LBU(10, 7, 8); NOP(); ANDI(10, 10, 8); BEQ(10, 0, -4); NOP();
    LI(11, 0x80000200); ADDIU(12, 0, 256);
    LH(13, 0, 8); NOP(); SH(13, 0, 11); ADDIU(11, 11, 2); ADDIU(12, 12, -1);
    BNE(12, 0, -6); NOP();
    LI(11, 0x80000200); LH(13, 0, 11); LI(14, 0x80000104); SW(13, 0, 14);
    ADDIU(11, 11, 510); LH(13, 0, 11); LI(14, 0x80000108); SW(13, 0, 14);

    /* ATA READ SECTOR 3 into a separate buffer and capture its first word. */
    ADDIU(9, 0, 0x0301); SH(9, 2, 8);
    ADDIU(9, 0, 0); SH(9, 4, 8);
    ADDIU(9, 0, 0x0040); SH(9, 6, 8);
    ADDIU(9, 0, 0x20); SB(9, 7, 8);
    /* Poll DRQ with a real guest loop. */
    LBU(10, 7, 8); NOP(); ANDI(10, 10, 8); BEQ(10, 0, -4); NOP();
    LI(11, 0x80000400); ADDIU(12, 0, 256);
    /* read_loop: LH, SH, pointer++, count--, BNE(-5), delay slot */
    LH(13, 0, 8); NOP(); SH(13, 0, 11); ADDIU(11, 11, 2); ADDIU(12, 12, -1);
    BNE(12, 0, -6); NOP();
    LI(11, 0x80000400); LH(13, 0, 11); LI(14, 0x80000110); SW(13, 0, 14);

    /* ATA WRITE the same sector with a recognizable pattern for overlay
     * reopen. The host advances board time and flushes on owner teardown. */
    ADDIU(9, 0, 0x0301); SH(9, 2, 8);
    ADDIU(9, 0, 0); SH(9, 4, 8);
    ADDIU(9, 0, 0x0040); SH(9, 6, 8);
    ADDIU(9, 0, 0x30); SB(9, 7, 8);
    LBU(10, 7, 8); NOP(); ANDI(10, 10, 8); BEQ(10, 0, -4); NOP();
    LI(13, 0x5aa5); ADDIU(12, 0, 256);
    /* write_loop: SH, count--, BNE(-3), delay slot */
    SH(13, 0, 8); ADDIU(12, 12, -1); BNE(12, 0, -3); NOP();
    /* Publish success sentinel and stop. */
    LI(11, 0x8000010c); ADDIU(10, 0, 0x4750); SW(10, 0, 11);
    /* MIPS J uses the high region of PC+4; encode the BFC00000 ROM alias. */
    program[count] = 0x0bf00000u | (count & 0x03ffffffu);
    count++;
    NOP();
    for (i = 0; i < count; ++i) {
        board.bios[i * 4 + 0] = (uint8_t)program[i];
        board.bios[i * 4 + 1] = (uint8_t)(program[i] >> 8);
        board.bios[i * 4 + 2] = (uint8_t)(program[i] >> 16);
        board.bios[i * 4 + 3] = (uint8_t)(program[i] >> 24);
    }
#undef LI
#undef NOP
#undef BNE
#undef BEQ
#undef ANDI
#undef SW
#undef LBU
#undef LH
#undef SH
#undef SB
#undef ADDIU
#undef ORI
#undef LUI
#undef I
#undef EMIT
}

#ifndef DRC_DISABLE
static int gnet_code_span(unsigned int pc, const unsigned int **words,
                          unsigned int *limit)
{
    (void)pc; (void)words; (void)limit;
    return 0;
}
#endif

static uint32_t physical_address(uint32_t address)
{
    return address >= 0x80000000u && address < 0xc0000000u ?
        address & 0x1fffffffu : address;
}

static const GnetProfile *find_profile(const char *set)
{
    size_t i;
    for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i)
        if (!strcmp(set, profiles[i].set)) return &profiles[i];
    return NULL;
}

static const GnetBiosRoot *find_bios_root(const char *set)
{
    size_t i;
    for (i = 0; i < sizeof(bios_roots) / sizeof(bios_roots[0]); ++i)
        if (!strcmp(set, bios_roots[i].set)) return &bios_roots[i];
    return NULL;
}

static const GnetBiosOption *find_bios_option(const char *root,
                                              const char *name)
{
    size_t i;
    for (i = 0; i < sizeof(bios_options) / sizeof(bios_options[0]); ++i)
        if (!strcmp(root, bios_options[i].root) &&
            !strcmp(name, bios_options[i].name)) return &bios_options[i];
    return NULL;
}

static void hardware_for_root(const char *root, const char **model,
                              const char **cpu, const char **cpu_hz,
                              const char **gpu)
{
    if (!strcmp(root, "coh1002t")) {
        *model = "ZN1"; *cpu = "CXD8530CQ"; *cpu_hz = "67737600";
        *gpu = "CXD8561Q";
    } else {
        *model = "ZN2"; *cpu = "CXD8661R"; *cpu_hz = "100000000";
        *gpu = "CXD8654Q";
    }
}

static const char *const config_keys[] = {
    "format", "mame_set", "mame_source_commit", "board_model",
    "cpu_device", "cpu_clock_hz", "gpu_device", "gpu_clock_hz",
    "main_ram_size", "vram_size", "game_execution_verified",
    "bios_option", "chd_file", "overlay_file"
};

static int safe_media_filename(const char *name)
{
    return name && *name && !strchr(name, '/') && !strchr(name, '\\') &&
           strcmp(name, ".") && strcmp(name, "..") && !strstr(name, "..");
}

static int read_config(const char *directory, const GnetProfile **selected,
                       const GnetBiosRoot **selected_root,
                       const GnetBiosOption **selected_option,
                       char overlay_file[PATH_BYTES])
{
    char path[PATH_BYTES], line[512], values[14][PATH_BYTES] = {{0}};
    uint32_t seen = 0;
    size_t i;
    FILE *file;
    const GnetProfile *p;
    const GnetBiosRoot *root;
    const GnetBiosOption *option = NULL;
    const char *model, *cpu, *cpu_hz, *gpu;
    const char *bios_name;
    {
        int length = snprintf(path, sizeof(path), "%s/board.ini", directory);
        if (length < 0 || length >= (int)sizeof(path)) return -1;
    }
    file = fopen(path, "r");
    if (!file) { perror(path); return -1; }
    while (fgets(line, sizeof(line), file)) {
        char *equal;
        if (!strchr(line, '\n') && !feof(file)) goto bad;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        equal = strchr(line, '=');
        if (!equal || strchr(equal + 1, '=')) goto bad;
        *equal++ = 0;
        for (i = 0; i < sizeof(config_keys) / sizeof(config_keys[0]); ++i)
            if (!strcmp(line, config_keys[i])) break;
        if (i == sizeof(config_keys) / sizeof(config_keys[0]) ||
            (seen & (1u << i)) || !*equal || strlen(equal) >= sizeof(values[i])) goto bad;
        strcpy(values[i], equal);
        seen |= 1u << i;
    }
    if (ferror(file) || (seen & 0x7ffu) != 0x7ffu) goto bad;
    p = find_profile(values[1]);
    root = p ? find_bios_root(p->bios_root) : NULL;
    if (!p || !root || strcmp(values[0], "pcsx-aow-gnet-v1") ||
        strcmp(values[2], GNET_SOURCE_COMMIT)) goto bad;
    hardware_for_root(root->set, &model, &cpu, &cpu_hz, &gpu);
    if (strcmp(values[3], model) || strcmp(values[4], cpu) ||
        strcmp(values[5], cpu_hz) || strcmp(values[6], gpu) ||
        strcmp(values[7], "53693175") ||
        strcmp(values[8], "4194304") || strcmp(values[9], "2097152") ||
        strcmp(values[10], "false")) goto bad;
    bios_name = (seen & (1u << 11)) ? values[11] : p->bios_default;
    if (!strcmp(root->set, "coh1002t")) {
        if (strcmp(bios_name, "fixed")) goto bad;
    } else {
        option = find_bios_option(root->set, bios_name);
        if (!option) goto bad;
    }
    if (p->card_kind && strcmp(p->card_kind, "NONE")) {
        char expected_chd[PATH_BYTES];
        if (!p->chd || !p->chd_sha1) goto bad;
        {
            int length = snprintf(expected_chd, sizeof(expected_chd),
                                  "%s.chd", p->chd);
            if (length < 0 || length >= (int)sizeof(expected_chd)) goto bad;
        }
        if ((seen & (1u << 12)) &&
            (!safe_media_filename(values[12]) ||
             strcmp(values[12], expected_chd))) goto bad;
        if ((seen & (1u << 13)) && !safe_media_filename(values[13])) goto bad;
        if (seen & (1u << 13))
            strcpy(overlay_file, values[13]);
        else {
            int length = snprintf(overlay_file, PATH_BYTES, "%s.overlay", p->set);
            if (length < 0 || length >= PATH_BYTES) goto bad;
        }
    } else if ((seen & ((1u << 12) | (1u << 13))) != 0) {
        goto bad;
    } else {
        overlay_file[0] = 0;
    }
    *selected = p;
    *selected_root = root;
    *selected_option = option;
    fclose(file);
    return 0;
bad:
    fprintf(stderr, "%s: invalid or unsupported G-Net board.ini\n", path);
    fclose(file);
    return -1;
}

static int read_region(const char *directory, const char *filename, size_t size,
                       const char *sha1, uint8_t **result)
{
    char path[PATH_BYTES];
    unsigned char digest[GNET_SHA1_BYTES];
    char hex[GNET_SHA1_BYTES * 2 + 1];
    static const char digits[] = "0123456789abcdef";
    FILE *file;
    long length;
    size_t i;
    if (snprintf(path, sizeof(path), "%s/%s", directory, filename) >= (int)sizeof(path)) return -1;
    file = fopen(path, "rb");
    if (!file) { perror(path); return -1; }
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        (size_t)length != size || fseek(file, 0, SEEK_SET)) {
        fprintf(stderr, "%s: expected exactly %zu bytes\n", path, size);
        fclose(file); return -1;
    }
    *result = (uint8_t *)malloc(size);
    if (!*result) { fclose(file); return -1; }
    {
        int read_failed = fread(*result, 1, size, file) != size;
        int close_failed = fclose(file) != 0;
        if (read_failed || close_failed) return -1;
    }
    if (gnet_sha1(*result, size, digest)) return -1;
    for (i = 0; i < sizeof(digest); ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[sizeof(hex) - 1] = 0;
    if (strcmp(hex, sha1)) {
        fprintf(stderr, "%s: SHA1 does not match pinned MAME region\n", path);
        return -1;
    }
    return 0;
}

static uint8_t input_read(void *opaque, zn_board_input_t input)
{
    (void)opaque;
    return input >= ZN_INPUT_P1 && input <= ZN_INPUT_P4 ? board.inputs[input] : board.inputs[2 + (input == ZN_INPUT_SERVICE ? 0 : 1)];
}
static uint8_t mcu_dsw(void *opaque) { (void)opaque; return board.dsw; }
static uint8_t mcu_analog0(void *opaque) { (void)opaque; return board.analog[0]; }
static uint8_t mcu_analog1(void *opaque) { (void)opaque; return board.analog[1]; }
static uint8_t mcu_analog(void *opaque) { (void)opaque; return 0xff; }
static uint16_t mcu_trackball(void *opaque) { (void)opaque; return 0; }
static uint16_t mcu_handle0(void *opaque)
{ (void)opaque; board.trackball[0] += (int)board.handles[0] - 128; return board.trackball[0]; }
static uint16_t mcu_handle1(void *opaque)
{ (void)opaque; board.trackball[1] += (((int)board.handles[1] - 128) * -193) / 256; return board.trackball[1]; }
static void update_rxd(void)
{
    if (board.sio[0]) psx_sio_write_rxd(board.sio[0], board.cat_out[0] && board.cat_out[1] && board.mcu_out);
}
static void cat_dataout(void *opaque, unsigned chip, int state)
{
    (void)opaque;
    if (chip < 2) board.cat_out[chip] = !!state;
    update_rxd();
}
static void mcu_txd(void *opaque, int state)
{
    (void)opaque; board.mcu_out = !!state; update_rxd();
}
static void mcu_dsr(void *opaque, int state)
{
    (void)opaque;
    if (board.sio[0]) psx_sio_write_dsr(board.sio[0], state);
}
static int advance_zn_at(uint64_t cycles)
{
    uint64_t us = cycles / PSX_CLOCK_HZ * 1000000u +
                  cycles % PSX_CLOCK_HZ * 1000000u / PSX_CLOCK_HZ;
    return zn_board_advance_to_us(&board.zn, us);
}
static void sio_sck(void *opaque, int state)
{
    (void)opaque;
    if (advance_zn_at(psx_sio_elapsed_cycles(board.sio[0])) ||
        zn_board_sio_clock(&board.zn, state)) board.fault = 1;
}
static void sio_txd(void *opaque, int state)
{
    (void)opaque;
    if (advance_zn_at(psx_sio_elapsed_cycles(board.sio[0]))) { board.fault = 1; return; }
    zn_board_sio_tx(&board.zn, state);
}
static void sio_irq(void *opaque, int state)
{
    unsigned port = (unsigned)(uintptr_t)opaque;
    if (state && psxRegs.ptrs.psxH) psxHu32ref(0x1070) |= SWAPu32(1u << (7 + port));
}

/* Separate emulated flash NVRAM from the strictly audited source images. */
static int flash_state(int save)
{
    enum { BYTES = FIRM_BYTES + GNET_ZOOM_PROGRAM_FLASH_BYTES + 3 * FIRM_BYTES };
    unsigned char expected[128] = {0}, header[128], digest[20], stored[20];
    unsigned char *data;
    char temporary[PATH_BYTES];
    FILE *file;
    int failed = 0;
    if (!board.flash_state_path[0]) return 0;
    int n = snprintf((char *)expected, sizeof(expected), "PCSXGNETFLASH1 %s %s %s %s\n",
        board.profile->set, board.bios_root->set,
        board.bios_option ? board.bios_option->name : "fixed",
        board.profile->chd_sha1 ? board.profile->chd_sha1 : "none");
    if (n < 0 || n >= (int)sizeof(expected)) return -1;
    if (save) {
        if (snprintf(temporary, sizeof(temporary), "%s.tmp", board.flash_state_path) >= (int)sizeof(temporary)) return -1;
        file = fopen(temporary, "wb");
    } else {
        file = fopen(board.flash_state_path, "rb");
        if (!file && errno == ENOENT) return 0;
    }
    if (!file) return -1;
    data = (unsigned char *)malloc(BYTES);
    if (!data) { fclose(file); return -1; }
    if (save) {
        memcpy(data, board.firm, FIRM_BYTES);
        memcpy(data + FIRM_BYTES, board.zoomprog, GNET_ZOOM_PROGRAM_FLASH_BYTES);
        for (unsigned i = 0; i < 3; i++) memcpy(data + FIRM_BYTES + GNET_ZOOM_PROGRAM_FLASH_BYTES + i * FIRM_BYTES, board.waves[i], FIRM_BYTES);
        failed = gnet_sha1(data, BYTES, digest) ||
            fwrite(expected, 1, sizeof(expected), file) != sizeof(expected) ||
            fwrite(digest, 1, sizeof(digest), file) != sizeof(digest) ||
            fwrite(data, 1, BYTES, file) != BYTES;
    } else {
        failed = fread(header, 1, sizeof(header), file) != sizeof(header) ||
            memcmp(header, expected, sizeof(header)) ||
            fread(stored, 1, sizeof(stored), file) != sizeof(stored) ||
            fread(data, 1, BYTES, file) != BYTES || fgetc(file) != EOF ||
            ferror(file) || gnet_sha1(data, BYTES, digest) || memcmp(stored, digest, sizeof(digest));
        if (!failed) {
            memcpy(board.firm, data, FIRM_BYTES);
            memcpy(board.zoomprog, data + FIRM_BYTES, GNET_ZOOM_PROGRAM_FLASH_BYTES);
            for (unsigned i = 0; i < 3; i++) memcpy(board.waves[i], data + FIRM_BYTES + GNET_ZOOM_PROGRAM_FLASH_BYTES + i * FIRM_BYTES, FIRM_BYTES);
        }
    }
    free(data);
    if (fclose(file)) failed = 1;
    if (save) {
        if (!failed && rename(temporary, board.flash_state_path)) failed = 1;
        if (failed) remove(temporary);
    }
    if (failed) fprintf(stderr, "SystemGNet flash NVRAM %s failed: %s\n", save ? "save" : "load", board.flash_state_path);
    else fprintf(stderr, "SystemGNet flash NVRAM %s PASS: %s\n", save ? "save" : "load", board.flash_state_path);
    return failed ? -1 : 0;
}

static int destroy_devices(void)
{
    unsigned i;
    if (board.zoom) {
        if (gnet_zoom_try_destroy(board.zoom) != GNET_ZOOM_OK) {
            fprintf(stderr, "G-Net: Zoom board is still in a callback; unload deferred\n");
            return -1;
        }
        board.zoom = NULL;
    }
    if (board.active && flash_state(1))
        fprintf(stderr, "G-Net: flash state could not be persisted\n");
    if (board.storage) {
        if (gnet_storage_flush(board.storage) != GNET_STORAGE_OK)
            fprintf(stderr, "G-Net: storage overlay flush failed; writes may be lost\n");
        if (gnet_storage_try_destroy(board.storage) != GNET_STORAGE_OK) {
            fprintf(stderr, "G-Net: storage is still in a callback; unload deferred\n");
            return -1;
        }
        board.storage = NULL;
    }
    if (board.zn_initialized) zn_board_destroy(&board.zn);
    zn_mcu_destroy(board.mcu);
    for (i = 0; i < 2; ++i) psx_sio_destroy(board.sio[i]);
    free(board.bios); free(board.key0); free(board.key1); free(board.firm); free(board.eprom);
    free(board.zoomprog);
    for (i = 0; i < 3; ++i) free(board.waves[i]);
    pcm_wav_close(&board.wav);
    memset(&board, 0, sizeof(board));
    return 0;
}

static int create_devices(void)
{
    zn_board_config_t zcfg = {0};
    zn_board_callbacks_t zcb = {0};
    ZNMCUConfig mcfg = {0};
    psx_sio_callbacks_t sio_cb = {0};
    gnet_zoom_config_t gcfg = {0};
    uint8_t keys[2][8];
    unsigned i;
    memset(board.inputs, 0xff, sizeof(board.inputs));
    memset(board.analog, 0xff, sizeof(board.analog));
    memset(board.handles, 0x80, sizeof(board.handles));
    board.dsw = 0x0f; board.cat_out[0] = board.cat_out[1] = board.mcu_out = 1;
    for (i = 0; i < 2; ++i) {
        sio_cb.opaque = (void *)(uintptr_t)i; sio_cb.irq = sio_irq;
        if (i == 0) { sio_cb.sck = sio_sck; sio_cb.txd = sio_txd; }
        if (psx_sio_create(i ? PSX_SIO1 : PSX_SIO0, &sio_cb, &board.sio[i])) return -1;
    }
    for (i = 0; i < 8; ++i) mcfg.analog[i] = mcu_analog;
    for (i = 0; i < 4; ++i) mcfg.trackball[i] = mcu_trackball;
    if (board.profile && !strcmp(board.profile->input, "gobyrc")) {
        memset(board.analog, 0x80, sizeof(board.analog));
        mcfg.analog[0] = mcu_analog0; mcfg.analog[1] = mcu_analog1;
    }
    if (board.profile && !strcmp(board.profile->input, "mawasunda")) {
        mcfg.trackball[0] = mcu_handle0; mcfg.trackball[1] = mcu_handle1;
    }
    mcfg.dsw = mcu_dsw; mcfg.dsr = mcu_dsr; mcfg.txd = mcu_txd;
    if (zn_mcu_create(&mcfg, &board.mcu) != ZN_MCU_OK) return -1;
    memcpy(keys[0], board.key0, 8); memcpy(keys[1], board.key1, 8);
    zcfg.model = board.bios_root && !strcmp(board.bios_root->set, "coh1002t")
        ? ZN_BOARD_ZN1 : ZN_BOARD_ZN2;
    zcfg.ram_bytes = 0x400000; zcfg.vram_bytes = 0x200000;
    zcfg.cat702_count = 2; zcfg.cat702_keys = (const uint8_t (*)[8])keys; zcfg.mcu = board.mcu;
    zcb.read_input = input_read; zcb.cat702_dataout = cat_dataout;
    if (!zn_board_init(&board.zn, &zcfg, &zcb)) return -1;
    board.zn_initialized = 1;
    gcfg.firm_flash = board.firm; gcfg.firm_flash_size = FIRM_BYTES;
    gcfg.zoom_program_flash = board.zoomprog; gcfg.zoom_program_flash_size = GNET_ZOOM_PROGRAM_FLASH_BYTES;
    for (i = 0; i < 3; ++i) { gcfg.sample_flash[i] = board.waves[i]; gcfg.sample_flash_size[i] = GNET_FIRM_FLASH_BYTES; }
    gcfg.eprom = board.eprom; gcfg.eprom_size = EPROM_BYTES;
    /* These source input configurations default JP1 to the EPROM bank. */
    gcfg.jp1 = board.profile && (!strcmp(board.profile->input, "taitogn_jp1") ||
        !strcmp(board.profile->input, "raystormg") || !strcmp(board.profile->input, "aerofgtsg"));
    gcfg.audio_frame = zoom_audio_frame;
    gcfg.request_soft_reset = request_soft_reset;
    gcfg.pc_card = board.storage ? gnet_storage_card_ops(board.storage) : NULL;
    if (gnet_zoom_create(&gcfg, &board.zoom) != GNET_ZOOM_OK) return -1;
    if (gpu_set_model(2) || gpu_set_vram_height(1024)) return -1;
    board.old_ram_size = psxRamSize; board.old_rom_size = psxRomSize;
    psxRamSize = 0x400000; psxRomSize = BIOS_BYTES;
    board.last_cycle = psxRegs.cycle;
    board.active = 1;
#ifndef DRC_DISABLE
    new_dynarec_set_code_span_provider(gnet_code_span);
#endif
    update_rxd();
    return 0;
}

static int load_region(const char *directory, const char *name, size_t size,
                       const char *hash, uint8_t **data)
{ return read_region(directory, name, size, hash, data); }

static int join_path(char output[PATH_BYTES], const char *directory,
                     const char *filename)
{
    int length = snprintf(output, PATH_BYTES, "%s/%s", directory, filename);
    return length < 0 || length >= PATH_BYTES ? -1 : 0;
}

static int decode_sha1(const char *hex, uint8_t output[GNET_SHA1_BYTES])
{
    unsigned i;
    if (!hex || strlen(hex) != GNET_SHA1_BYTES * 2) return -1;
    for (i = 0; i < GNET_SHA1_BYTES; ++i) {
        unsigned hi, lo;
        char a = hex[i * 2], b = hex[i * 2 + 1];
        if (a >= '0' && a <= '9') hi = (unsigned)(a - '0');
        else if (a >= 'a' && a <= 'f') hi = (unsigned)(a - 'a' + 10);
        else return -1;
        if (b >= '0' && b <= '9') lo = (unsigned)(b - '0');
        else if (b >= 'a' && b <= 'f') lo = (unsigned)(b - 'a' + 10);
        else return -1;
        output[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static gnet_pccard_variant_t card_variant_for_profile(const GnetProfile *profile)
{
    if (!strcmp(profile->card_kind, "TYPE1")) return GNET_PCCARD_TAITO_TYPE1;
    if (!strcmp(profile->card_kind, "TYPE2")) return GNET_PCCARD_TAITO_TYPE2;
    if (!strcmp(profile->card_kind, "CF")) return GNET_PCCARD_TAITO_COMPACT_FLASH;
    return GNET_PCCARD_ATA_FLASH;
}

static int open_profile_storage(const char *directory,
                                const GnetProfile *profile,
                                const char *overlay_file)
{
    char chd_name[PATH_BYTES], chd_path[PATH_BYTES], overlay_path[PATH_BYTES];
    uint8_t expected_sha1[GNET_SHA1_BYTES];
    gnet_storage_config_t config;
    char error[256];
    int result;
    if (!profile || !profile->card_kind || !strcmp(profile->card_kind, "NONE"))
        return 0;
    if (!profile->chd || !profile->chd_sha1 || !overlay_file ||
        snprintf(chd_name, sizeof(chd_name), "%s.chd", profile->chd) < 0 ||
        strlen(profile->chd) + 4 >= sizeof(chd_name) ||
        join_path(chd_path, directory, chd_name) ||
        join_path(overlay_path, directory, overlay_file) ||
        decode_sha1(profile->chd_sha1, expected_sha1))
        return -1;
    memset(&config, 0, sizeof(config));
    config.chd_path = chd_path;
    config.overlay_path = overlay_path;
    config.card_variant = card_variant_for_profile(profile);
    config.expected_sha1 = expected_sha1;
    config.expected_sha1_size = sizeof(expected_sha1);
    result = gnet_storage_open(&config, &board.storage, error, sizeof(error));
    if (result != GNET_STORAGE_OK) {
        fprintf(stderr, "G-Net: cannot open %s PC Card media %s (%d): %s\n",
                profile->set, chd_path, result, error);
        return -1;
    }
    return 0;
}

int systemgnet_load(const char *directory)
{
    char overlay_file[PATH_BYTES];
    if (!directory || board.active || psxRegs.ptrs.psxM ||
        systemtaito_active() || systemzn_active() || system10_active() ||
        system11_active() || system12_active()) return -1;
    if (read_config(directory, &board.profile, &board.bios_root,
                    &board.bios_option, overlay_file)) goto bad;
    if (load_region(directory, "maincpu.bin", BIOS_BYTES, board.bios_root->cpu_sha1, &board.bios) ||
        load_region(directory, "cat702_0.bin", KEY_BYTES, board.bios_root->key0_sha1, &board.key0) ||
        load_region(directory, "cat702_1.bin", KEY_BYTES, board.bios_root->key1_sha1, &board.key1) ||
        load_region(directory, "firm.bin", FIRM_BYTES, board.bios_root->firm_sha1, &board.firm)) goto bad;
    if (board.bios_option) {
        if (load_region(directory, "eprom.bin", EPROM_BYTES,
                        board.bios_option->sha1, &board.eprom)) goto bad;
    } else {
        board.eprom = (uint8_t *)malloc(EPROM_BYTES);
        if (!board.eprom) goto bad;
        memset(board.eprom, 0xff, EPROM_BYTES);
    }
    if (board.profile->card_kind && strcmp(board.profile->card_kind, "NONE") &&
        open_profile_storage(directory, board.profile, overlay_file)) goto bad;
    board.zoomprog = (uint8_t *)malloc(GNET_ZOOM_PROGRAM_FLASH_BYTES);
    for (unsigned i = 0; i < 3; ++i) board.waves[i] = (uint8_t *)malloc(GNET_FIRM_FLASH_BYTES);
    if (!board.zoomprog || !board.waves[0] || !board.waves[1] || !board.waves[2]) goto bad;
    memset(board.zoomprog, 0xff, GNET_ZOOM_PROGRAM_FLASH_BYTES);
    for (unsigned i = 0; i < 3; ++i) memset(board.waves[i], 0xff, GNET_FIRM_FLASH_BYTES);
    if (join_path(board.flash_state_path, directory, "flash.nvram") || flash_state(0)) goto bad;
    if (create_devices()) goto bad;
    return 0;
bad:
    fprintf(stderr, "G-Net: profile load failed; media must match the selected MAME source profile\n");
    systemgnet_unload();
    return -1;
}

int systemgnet_test_load(void)
{
    if (board.active || psxRegs.ptrs.psxM || systemtaito_active() || systemzn_active() ||
        system10_active() || system11_active() || system12_active()) return -1;
    board.profile = find_profile("coh3002t");
    board.bios_root = find_bios_root("coh3002t");
    board.bios_option = find_bios_option("coh3002t", "v1");
    board.bios = (uint8_t *)malloc(BIOS_BYTES);
    board.key0 = (uint8_t *)malloc(KEY_BYTES); board.key1 = (uint8_t *)malloc(KEY_BYTES);
    board.firm = (uint8_t *)malloc(FIRM_BYTES); board.eprom = (uint8_t *)malloc(EPROM_BYTES);
    board.zoomprog = (uint8_t *)malloc(GNET_ZOOM_PROGRAM_FLASH_BYTES);
    for (unsigned i = 0; i < 3; ++i) board.waves[i] = (uint8_t *)malloc(GNET_FIRM_FLASH_BYTES);
    if (!board.bios || !board.key0 || !board.key1 || !board.firm || !board.eprom ||
        !board.zoomprog || !board.waves[0] || !board.waves[1] || !board.waves[2]) goto bad;
    memset(board.bios, 0xff, BIOS_BYTES);
    /* Real guest code: select G-Net bank 1, read MAME's fixed ID register,
     * and publish the result to PSX RAM. */
    if (test_storage_chd_path) {
        install_storage_guest_program();
    } else {
        const uint32_t program[] = {
            0x3c081fb4, 0x24090004, 0xa1090000, 0x3c0a1f00,
            0x954b0000, 0x3c0c8000, 0xad8b0104, 0x3c0a1fb7,
            0x954b0000, 0x00000000, 0xad8b0100, 0x1000ffff, 0x00000000
        };
        for (unsigned i = 0; i < sizeof(program) / sizeof(program[0]); ++i) {
            board.bios[i * 4 + 0] = (uint8_t)program[i];
            board.bios[i * 4 + 1] = (uint8_t)(program[i] >> 8);
            board.bios[i * 4 + 2] = (uint8_t)(program[i] >> 16);
            board.bios[i * 4 + 3] = (uint8_t)(program[i] >> 24);
        }
    }
    for (unsigned i = 0; i < KEY_BYTES; ++i) { board.key0[i] = (uint8_t)(i * 17 + 3); board.key1[i] = (uint8_t)(i * 29 + 7); }
    memset(board.firm, 0xff, FIRM_BYTES); memset(board.eprom, 0xff, EPROM_BYTES);
    memset(board.zoomprog, 0xff, GNET_ZOOM_PROGRAM_FLASH_BYTES);
    for (unsigned i = 0; i < 3; ++i) memset(board.waves[i], 0, GNET_FIRM_FLASH_BYTES);
    /* Test-only executable MN10200 program which uploads a real TMS57002
     * tone routine through Port 1. Production media loading remains strict
     * and continues to leave the absent program/sample regions erased. */
    {
        size_t p = 0;
        static const uint8_t tms_program[] = {
            0,0,0, 0,0,0, 0xcc,0x05,0x10, 0x01,0x10,0x00, 0xfc,0x40,0x00
        };
        static const uint8_t tms_coefficients[] = {0x10,0x12,0x34,0x56,0x78};
        unsigned i;
#define GNET_TEST_STORE8(v, a) do { \
            const uint8_t instruction[] = {0xf8, (uint8_t)(v), 0, 0xf4, 0x44, \
                (uint8_t)(a), (uint8_t)((a) >> 8), (uint8_t)((a) >> 16)}; \
            memcpy(board.zoomprog + p, instruction, sizeof(instruction)); \
            p += sizeof(instruction); \
        } while (0)
        GNET_TEST_STORE8(0xff, 0xffe1); /* P1 direction = output */
        GNET_TEST_STORE8(3, 0xfe64);    /* release PLOAD and CLOAD */
        GNET_TEST_STORE8(2, 0xfe64);    /* assert PLOAD */
        for (i = 0; i < sizeof(tms_program); ++i) GNET_TEST_STORE8(tms_program[i], 0xc00000);
        GNET_TEST_STORE8(3, 0xfe64);    /* release PLOAD */
        GNET_TEST_STORE8(1, 0xfe64);    /* assert CLOAD */
        for (i = 0; i < sizeof(tms_coefficients); ++i) GNET_TEST_STORE8(tms_coefficients[i], 0xc00000);
        GNET_TEST_STORE8(3, 0xfe64);    /* complete CLOAD */
        board.zoomprog[p++] = 0xea; board.zoomprog[p++] = 0xfe; /* BRA -2 */
#undef GNET_TEST_STORE8
    }
    board.waves[0][0] = 0x34; board.waves[0][1] = 0x12;
    if (test_storage_chd_path && open_test_storage(test_storage_chd_path)) goto bad;
    if (create_devices()) goto bad;
    return 0;
bad:
    systemgnet_unload();
    return -1;
}

int systemgnet_test_load_storage_fixture(const char *chd_path)
{
    int result;
    if (!chd_path || !*chd_path || test_storage_chd_path) return -1;
    test_storage_chd_path = chd_path;
    result = systemgnet_test_load();
    test_storage_chd_path = NULL;
    return result;
}

void systemgnet_unload(void)
{
    const int was_active = board.active;
    const uint32_t old_ram_size = board.old_ram_size;
    const uint32_t old_rom_size = board.old_rom_size;
    if (destroy_devices()) return;
#ifndef DRC_DISABLE
    if (was_active) new_dynarec_set_code_span_provider(NULL);
#endif
    if (was_active) { psxRamSize = old_ram_size; psxRomSize = old_rom_size; }
    if (!psxRegs.ptrs.psxM && !systemtaito_active() && !systemzn_active() &&
        !system10_active() && !system11_active() && !system12_active()) {
        psxRamSize = 0x200000; psxRomSize = 0x80000;
        gpu_set_model(2); gpu_set_vram_height(512);
    }
}
int systemgnet_active(void) { return board.active; }
uint32_t systemgnet_main_clock_hz(void)
{
    if (!board.active || !board.bios_root) return 0;
    if (!strcmp(board.bios_root->set, "coh1002t")) return 67737600u;
    if (!strcmp(board.bios_root->set, "coh3002t")) return 100000000u;
    return 0;
}
void systemgnet_set_audio_output(systemgnet_audio_frame_fn callback, void *opaque)
{
    audio_output = callback;
    audio_output_opaque = callback ? opaque : NULL;
}

void systemgnet_mem_reset(void)
{
    if (!board.active || !psxRegs.ptrs.psxM || !psxRegs.ptrs.psxR) return;
    memcpy(psxRegs.ptrs.psxR, board.bios, BIOS_BYTES);
    if (!board.memory_initialized) memset(psxRegs.ptrs.psxM, 0, PSX_RAM_SIZE);
    memset(psxRegs.ptrs.psxP, 0xff, 0x10000);
    board.memory_initialized = 1;
}
void systemgnet_reset(void)
{
    if (!board.active) return;
    board.last_cycle = psxRegs.cycle; board.fault = 0; board.reset_pending = 0;
    if (gnet_zoom_control_write(board.zoom, 0x10) != GNET_ZOOM_OK)
        board.fault = 1;
    if (board.storage && gnet_storage_reset(board.storage) != GNET_STORAGE_OK)
        board.fault = 1;
}

static int cycles_to_ns(uint64_t cycles, uint64_t *result)
{
    const uint64_t clock = PSXCLK;
    const uint64_t whole = cycles / clock;
    const uint64_t remainder = cycles % clock;
    if (whole > UINT64_MAX / 1000000000u) return -1;
    *result = whole * 1000000000u + remainder * 1000000000u / clock;
    return 0;
}

int systemgnet_sync(uint32_t cycle)
{
    uint32_t delta;
    uint64_t scaled;
    if (!board.active) return 0;
    if (cycle < board.last_cycle && board.last_cycle - cycle < 0x80000000u) {
        board.fault = 1; return -1;
    }
    delta = cycle - board.last_cycle; board.last_cycle = cycle;
    if (delta > UINT64_MAX - board.total_cycles) { board.fault = 1; return -1; }
    board.total_cycles += delta;
    if (board.storage) {
        uint64_t now_ns;
        if (cycles_to_ns(board.total_cycles, &now_ns) ||
            gnet_storage_run_until(board.storage, now_ns) != GNET_STORAGE_OK)
            board.fault = 1;
    }
    for (unsigned i = 0; i < 2; ++i)
        if (psx_sio_advance_to_cycle(board.sio[i], board.total_cycles)) board.fault = 1;
    if (advance_zn_at(board.total_cycles)) board.fault = 1;
    scaled = (uint64_t)delta * MN_CLOCK_HZ + board.mn_fraction;
    if (scaled / PSX_CLOCK_HZ > UINT64_MAX - board.mn_cycles) { board.fault = 1; return -1; }
    board.mn_cycles += scaled / PSX_CLOCK_HZ;
    board.mn_fraction = scaled % PSX_CLOCK_HZ;
    int zoom_result = gnet_zoom_sync_to_cycle(board.zoom, board.mn_cycles);
    if (zoom_result != GNET_ZOOM_OK) {
        if (!board.fault) fprintf(stderr, "SystemGNet Zoom fault main_pc=%08x sound_pc=%06x result=%d kind=%u cycles=%" PRIu64 "\n",
            psxRegs.pc, gnet_zoom_sound_pc(board.zoom), zoom_result,
            gnet_zoom_fault(board.zoom), board.mn_cycles);
        board.fault = 1;
    }
    return board.fault ? -1 : board.reset_pending;
}

int systemgnet_reset_requested(void) { return board.reset_pending; }

int systemgnet_open_audio(const char *path)
{ return board.active ? pcm_wav_open(&board.wav, path, 25000000u / 768u) : -1; }
int systemgnet_close_audio(void) { return pcm_wav_close(&board.wav); }

static int sio_bus(uint32_t address, unsigned width, uint32_t *value, int write)
{
    uint32_t raw, shift, mask; unsigned port;
    if (address < 0x1f801040u || address >= 0x1f801060u) return 0;
    port = (address - 0x1f801040u) >> 4; shift = (address & 3u) * 8;
    mask = width == 4 ? UINT32_MAX : ((1u << (width * 8)) - 1) << shift;
    if (write) {
        if (psx_sio_write(board.sio[port], (address >> 2) & 3, *value << shift, mask)) board.fault = 1;
    } else {
        if (psx_sio_read(board.sio[port], (address >> 2) & 3, mask, &raw)) { board.fault = 1; raw = UINT32_MAX; }
        *value = (raw & mask) >> shift;
    }
    return 1;
}
static int gnet_map(uint32_t address)
{
    return (address >= 0x1f000000u && address < 0x1f800000u) ||
        address == 0x1fa30000u || address == 0x1fb40000u || address == 0x1fb60000u ||
        (address >= 0x1fb70000u && address < 0x1fb70002u) || (address >= 0x1fb00000u && address < 0x1fb10000u) ||
        address == 0x1fb80000u || address == 0x1fb80002u || address == 0x1fba0000u ||
        address == 0x1fbc0000u || (address >= 0x1fbe0000u && address < 0x1fbe0200u);
}
static int zn_common_map(uint32_t address)
{
    return (address >= 0x1fa00000u && address < 0x1fa00400u) ||
        (address >= 0x1fa10000u && address < 0x1fa10400u) ||
        address == 0x1fa20000u || (address >= 0x1fa30000u && address < 0x1fa30004u) ||
        (address >= 0x1fa40000u && address < 0x1fa40004u) ||
        (address >= 0x1fa51c00u && address < 0x1fa51e00u) ||
        (address >= 0x1fa60000u && address < 0x1fa60002u) ||
        (address >= 0x1faf0000u && address < 0x1faf0800u) ||
        (address >= 0x1fb20000u && address < 0x1fb20008u);
}
int systemgnet_read(uint32_t address, unsigned width, uint32_t *value)
{
    int result; uint32_t physical = physical_address(address);
    if (!board.active || !value || (width != 1 && width != 2 && width != 4) || (physical & (width - 1))) return 0;
    if (!gnet_map(physical) && !zn_common_map(physical) &&
        !(physical >= 0x1f801040u && physical < 0x1f801060u)) return 0;
    if (systemgnet_sync(psxRegs.cycle) < 0) { *value = UINT32_MAX; return -1; }
    if (sio_bus(physical, width, value, 0)) return 1;
    result = gnet_zoom_bus_read(board.zoom, physical, width, value);
    if (result == GNET_ZOOM_UNMAPPED) result = zn_board_read(&board.zn, physical, width, value) ? GNET_ZOOM_OK : GNET_ZOOM_UNMAPPED;
    if (result == GNET_ZOOM_UNSUPPORTED) {
        if (!board.fault) fprintf(stderr, "SystemGNet unsupported read pc=%08x address=%08x width=%u bank=%u\n",
            psxRegs.pc, physical, width, gnet_zoom_selected_bank(board.zoom));
        board.unsupported++; board.fault = 1; *value = UINT32_MAX; return 1;
    }
    if (result < 0) { board.fault = 1; *value = UINT32_MAX; return -1; }
    if (result == GNET_ZOOM_OK) board.reads++;
    return result == GNET_ZOOM_OK;
}
int systemgnet_write(uint32_t address, unsigned width, uint32_t value)
{
    int result; uint32_t physical = physical_address(address);
    if (!board.active || (width != 1 && width != 2 && width != 4) || (physical & (width - 1))) return 0;
    if (!gnet_map(physical) && !zn_common_map(physical) &&
        !(physical >= 0x1f801040u && physical < 0x1f801060u)) return 0;
    if (systemgnet_sync(psxRegs.cycle) < 0) return -1;
    if (sio_bus(physical, width, &value, 1)) return 1;
    /* Mawasunda's source flash-bank extension drives handle weights/lamps. */
    if (board.profile && !strcmp(board.profile->machine, "mawasunda") &&
        gnet_zoom_selected_bank(board.zoom) == 0 &&
        (physical == 0x1f600000u || physical == 0x1f600002u) && width == 2) {
        if (physical == 0x1f600000u) {
            board.weights[0] = value & 31; board.weights[1] = (value >> 8) & 31;
        } else board.lamps = (uint16_t)~value;
        board.writes++; return 1;
    }
    result = gnet_zoom_bus_write(board.zoom, physical, width, value);
    if (result == GNET_ZOOM_UNMAPPED) result = zn_board_write(&board.zn, physical, width, value) ? GNET_ZOOM_OK : GNET_ZOOM_UNMAPPED;
    if (result == GNET_ZOOM_UNSUPPORTED) {
        if (!board.fault) fprintf(stderr, "SystemGNet unsupported write pc=%08x address=%08x width=%u value=%08x bank=%u\n",
            psxRegs.pc, physical, width, value, gnet_zoom_selected_bank(board.zoom));
        board.unsupported++; board.fault = 1; return 1;
    }
    if (result < 0) { board.fault = 1; return -1; }
    if (result == GNET_ZOOM_OK) board.writes++;
    return result == GNET_ZOOM_OK;
}

int systemgnet_set_input(const char *name, int pressed)
{
    static const struct { const char *name; unsigned port, mask; } map[] = {
        {"p1_up",0,1},{"p1_down",0,2},{"p1_left",0,4},{"p1_right",0,8},
        {"p1_button1",0,16},{"p1_button2",0,32},{"p1_button3",0,64},
        {"p2_up",1,1},{"p2_down",1,2},{"p2_left",1,4},{"p2_right",1,8},
        {"p2_button1",1,16},{"p2_button2",1,32},{"p2_button3",1,64},
        {"test",2,1},{"service",2,2},{"tilt",2,4},{"p1_start",3,1},
        {"p2_start",3,2},{"coin1",3,16},{"coin2",3,32}
    };
    if (!board.active || !name || (pressed != 0 && pressed != 1)) return -1;
    if (!strcmp(name, "jp1")) {
        if (gnet_zoom_set_jp1(board.zoom, (unsigned)pressed)) return -1;
        return gnet_zoom_control_write(board.zoom, gnet_zoom_control_read(board.zoom));
    }
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i) if (!strcmp(name, map[i].name)) {
        if (pressed) board.inputs[map[i].port] &= (uint8_t)~map[i].mask;
        else board.inputs[map[i].port] |= (uint8_t)map[i].mask;
        return 0;
    }
    return -1;
}

unsigned systemgnet_analog_max(const char *name)
{
    if (!name || !board.profile) return 0;
    if (!strcmp(board.profile->input, "gobyrc") &&
        (!strcmp(name, "p1_wheel") || !strcmp(name, "p1_trigger"))) return 255;
    if (!strcmp(board.profile->input, "mawasunda") &&
        (!strcmp(name, "p1_handle") || !strcmp(name, "p2_handle"))) return 255;
    return 0;
}

unsigned systemgnet_rotation(void)
{
    if (!board.profile) return 0;
    if (!strcmp(board.profile->rotation, "ROT90")) return 90;
    if (!strcmp(board.profile->rotation, "ROT180")) return 180;
    if (!strcmp(board.profile->rotation, "ROT270")) return 270;
    return 0;
}

int systemgnet_set_analog(const char *name, unsigned value)
{
    if (!board.active || !systemgnet_analog_max(name) || value > 255) return -1;
    if (!strcmp(board.profile->input, "gobyrc")) {
        board.analog[!strcmp(name, "p1_trigger")] = (uint8_t)value;
    } else {
        if (!value) return -1;
        board.handles[!strcmp(name, "p2_handle")] = (uint8_t)value;
    }
    return 0;
}
int systemgnet_eeprom_import(const unsigned char *image, size_t size)
{ return board.active ? zn_board_eeprom_import(&board.zn, image, size) : -1; }
int systemgnet_eeprom_export(unsigned char *image, size_t size)
{ return board.active ? zn_board_eeprom_export(&board.zn, image, size) : -1; }
int systemgnet_selftest(void) { return board.active ? 0 : -1; }
void systemgnet_report(FILE *out)
{
    const char *model, *cpu, *cpu_hz, *gpu;
    if (!out || !board.active) return;
    hardware_for_root(board.bios_root ? board.bios_root->set : "coh3002t",
                      &model, &cpu, &cpu_hz, &gpu);
    (void)cpu; (void)cpu_hz; (void)gpu;
    fprintf(out, "SystemGNet set=%s model=%s ram=4194304 vram=2097152 gpu=type2 BIOS=512KiB "
        "main_clock_input_hz=%u reads=%" PRIu64 " writes=%" PRIu64 " unsupported=%" PRIu64 " bank=%u fault=%u execution_verified=false\n",
        board.profile ? board.profile->set : "fixture", model,
        systemgnet_main_clock_hz(),
        board.reads, board.writes, board.unsupported, gnet_zoom_selected_bank(board.zoom), (unsigned)board.fault);
    fprintf(out, "SystemGNet media bios=%s option=%s card=%s chd=%s auth_boot=false%s\n",
        board.bios_root ? board.bios_root->set : "fixture",
        board.bios_option ? board.bios_option->name : "fixed",
        board.test_storage_fixture ? "TYPE1_TEST" :
            (board.profile && board.profile->card_kind ? board.profile->card_kind : "NONE"),
        board.test_storage_fixture ? "synthetic" :
            (board.profile && board.profile->chd ? board.profile->chd : "none"),
        board.test_storage_fixture ? " fixture=true" : "");
    if (board.test_storage_fixture)
        fprintf(out, "SystemGNet storage fixture locked=%d ata_status=%02x overlay=explicit\n",
            gnet_storage_is_locked(board.storage),
            gnet_storage_ata_status(board.storage));
    if (board.profile)
        fprintf(out, "SystemGNet source parent=%s bios_model=%s machine=%s input=%s init=%s rotation=%s flags=%s\n",
            board.profile->parent ? board.profile->parent : "none",
            board.profile->bios_model, board.profile->machine,
            board.profile->input, board.profile->init,
            board.profile->rotation, board.profile->flags);
    fprintf(out, "SystemGNet Zoom pc=%06x fault=%u watchdog_requests=%" PRIu64
        " reset_pending=%d audio_rate=%u audio_frames=%" PRIu64 " peak=%u nonzero=%" PRIu64 " WAV_bytes=%" PRIu64 "\n",
        gnet_zoom_sound_pc(board.zoom), gnet_zoom_fault(board.zoom),
        gnet_zoom_watchdog_reset_requests(board.zoom), board.reset_pending,
        25000000u / 768u, board.audio_frames, board.audio_peak, board.audio_nonzero, board.wav.bytes);
    if (board.storage) fprintf(out, "SystemGNet storage locked=%d ata_status=%02x\n",
        gnet_storage_is_locked(board.storage), gnet_storage_ata_status(board.storage));
    if (psxRegs.ptrs.psxH) fprintf(out, "SystemGNet DMA4 madr=%08x bcr=%08x chcr=%08x dpcr=%08x dicr=%08x irq=%04x/%04x sr=%08x cause=%08x\n",
        psxHu32(0x10c0), psxHu32(0x10c4), psxHu32(0x10c8), psxHu32(0x10f0), psxHu32(0x10f4),
        psxHu16(0x1070), psxHu16(0x1074), psxRegs.CP0.n.SR, psxRegs.CP0.n.Cause);
}
