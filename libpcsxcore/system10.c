/* GPL-2.0-or-later. Native host wiring for MAME-derived System 10 boards. */
#include "system10.h"

#include "system11.h"
#include "system12.h"
#include "systemtaito.h"
#include "systemzn.h"
#include "psxhw.h"
#include "psxmem.h"
#include "arcade/namcos10_board.h"
#include "arcade/namcos10_io.h"
#include "gpu_vram.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#if !defined(PCSX_SYSTEM12)
#error System10 native board layouts require the PCSX_SYSTEM12 runtime-size build.
#endif

enum { SYSTEM10_ROM_SIZE = 0x400000, SYSTEM10_RAM_SIZE = 0x400000,
       MAX_REGIONS = 16, MAX_NAME = 64, MAX_FILE = 96, MAX_SPEC = 768 };

typedef struct Profile {
    const char *set, *parent, *board_type, *crypto, *machine, *input;
    const char *state, *init, *flags, *extensions, *no_dump, *unsupported;
    const char *runtime_candidate, *region_spec, *nand_type, *nand_count;
} Profile;
static const Profile profiles[] = {
#include "arcade/system10_profiles.inc"
};

typedef struct Region {
    char tag[MAX_NAME];
    char filename[MAX_FILE];
    size_t size;
    uint8_t *bytes;
    unsigned filename_seen, size_seen;
} Region;

typedef struct ProfileRegion {
    char tag[MAX_NAME];
    char filename[MAX_FILE];
    size_t size;
} ProfileRegion;

static struct {
    NS10Board *device;
    NS10IO *io;
    const Profile *profile;
    Region regions[MAX_REGIONS];
    size_t region_count;
    uint8_t *program_rom;
    uint8_t *flash_image;
    uint16_t system_inputs;
    uint32_t player_inputs;
    uint32_t last_cycle;
    uint64_t cycle_fraction;
    uint64_t io_elapsed_us;
    uint64_t dma_transfers, dma_bytes, read_errors, write_errors, fetch_errors;
    uint64_t unsupported_accesses, unmapped_accesses;
    uint32_t last_address, last_value;
    unsigned last_width;
    int fault;
} board;

struct InputBinding { const char *name; uint32_t mask; unsigned system; };
static const struct InputBinding input_bindings[] = {
    {"p1right", 1u << 0, 0}, {"p1left", 1u << 1, 0},
    {"p1down", 1u << 2, 0}, {"p1up", 1u << 3, 0},
    {"p1button1", 1u << 4, 0}, {"p1button2", 1u << 5, 0},
    {"p1button3", 1u << 6, 0}, {"p1start", 1u << 7, 0},
    {"p2right", 1u << 8, 0}, {"p2left", 1u << 9, 0},
    {"p2down", 1u << 10, 0}, {"p2up", 1u << 11, 0},
    {"p2button1", 1u << 12, 0}, {"p2button2", 1u << 13, 0},
    {"p2button3", 1u << 14, 0}, {"p2start", 1u << 15, 0},
    {"p1button4", 1u << 17, 0}, {"p1button5", 1u << 18, 0},
    {"p1button6", 1u << 19, 0}, {"p2button4", 1u << 21, 0},
    {"p2button5", 1u << 22, 0}, {"p2button6", 1u << 23, 0},
    {"coin2", 1u << 28, 0}, {"coin1", 1u << 29, 0},
    {"service", 1u << 30, 0}, {"test", 1u << 30, 0},
    {"service1", 1u << 31, 0},
    {"dip1", 1u << 0, 1}, {"dip2", 1u << 1, 1},
    {"dip3", 1u << 2, 1}, {"dip4", 1u << 3, 1},
    {"dip5", 1u << 4, 1}, {"dip6", 1u << 5, 1},
    {"dip7", 1u << 6, 1}, {"dip8", 1u << 7, 1},
    {"jvs_sense", 1u << 10, 1}
};

static uint16_t input_system_read(void *opaque)
{
    (void)opaque;
    return board.system_inputs;
}

static uint32_t input_in1_read(void *opaque)
{
    (void)opaque;
    return board.player_inputs;
}

static void irq10(void *opaque, int asserted)
{
    (void)opaque;
    if (asserted && psxRegs.ptrs.psxH != NULL)
        psxHu32ref(0x1070) |= SWAPu32(1u << 10);
}

static int parse_profile_regions(const Profile *profile,
                                 ProfileRegion regions[MAX_REGIONS], size_t *count)
{
    char copy[MAX_SPEC];
    char *entry, *save = NULL;
    size_t n = 0;
    if (strlen(profile->region_spec) >= sizeof(copy)) return -1;
    strcpy(copy, profile->region_spec);
    for (entry = strtok_r(copy, ";", &save); entry != NULL;
         entry = strtok_r(NULL, ";", &save)) {
        char *tag = entry, *size_text = strchr(tag, '|'), *filename;
        char *end;
        unsigned long size;
        if (n == MAX_REGIONS || size_text == NULL) return -1;
        *size_text++ = 0;
        filename = strchr(size_text, '|');
        if (filename == NULL) return -1;
        *filename++ = 0;
        if (!*tag || !*size_text || !*filename || strlen(tag) >= MAX_NAME ||
            strlen(filename) >= MAX_FILE) return -1;
        errno = 0;
        size = strtoul(size_text, &end, 10);
        if (errno || *end || size == 0 || size > SIZE_MAX) return -1;
        strcpy(regions[n].tag, tag);
        strcpy(regions[n].filename, filename);
        regions[n].size = (size_t)size;
        n++;
    }
    if (n == 0) return -1;
    *count = n;
    return 0;
}

static int parse_size_decimal(const char *text, size_t *value)
{
    const unsigned char *p = (const unsigned char *)text;
    unsigned long long parsed;
    char *end;
    if (p == NULL || *p == 0 || (*p == '0' && p[1] != 0)) return -1;
    for (; *p; p++) if (*p < '0' || *p > '9') return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno || *end || parsed == 0 || parsed > SIZE_MAX) return -1;
    *value = (size_t)parsed;
    return 0;
}

static const Profile *find_profile(const char *set_name)
{
    size_t i;
    for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++)
        if (!strcmp(set_name, profiles[i].set)) return &profiles[i];
    return NULL;
}

static int profile_supported(const Profile *profile)
{
    if (strcmp(profile->runtime_candidate, "true") ||
        strcmp(profile->extensions, "") || strcmp(profile->no_dump, "") ||
        strcmp(profile->unsupported, "") || strcmp(profile->crypto, "none") == 0 ||
        strstr(profile->flags, "MACHINE_NOT_WORKING") != NULL ||
        strstr(profile->flags, "MACHINE_UNEMULATED_PROTECTION") != NULL)
        return 0;
    return !strcmp(profile->board_type, "MEM(M)") ||
           !strcmp(profile->board_type, "MEM(N)");
}

static int parse_config(const char *directory, const Profile **profile_out,
                        ProfileRegion expected[MAX_REGIONS], size_t *expected_count)
{
    static const char *keys[] = {"format", "mame_set", "mame_parent", "board_type",
        "crypto_profile", "mame_machine", "mame_input", "mame_state", "mame_init",
        "mame_flags", "board_execution_verified"};
    char path[4096], line[512], values[11][256] = {{0}};
    Region supplied[MAX_REGIONS] = {0};
    size_t supplied_count = 0, i, j;
    unsigned seen = 0;
    FILE *file;
    const Profile *profile;
    if (snprintf(path, sizeof(path), "%s/board.ini", directory) >= (int)sizeof(path)) return -1;
    file = fopen(path, "r");
    if (file == NULL) { perror(path); return -1; }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *end = line + strcspn(line, "\r\n");
        char *equal, *key, *value;
        if (!strchr(line, '\n') && !feof(file)) goto bad;
        *end = 0;
        if (!*line) continue;
        equal = strchr(line, '=');
        if (equal == NULL || strchr(equal + 1, '=')) goto bad;
        *equal++ = 0; key = line; value = equal;
        if (!strncmp(key, "region.", 7)) {
            char *tag = key + 7, *suffix = strstr(tag, ".size");
            Region *r = NULL;
            if (suffix != NULL && suffix[5] != 0) goto bad;
            if (suffix != NULL) *suffix = 0;
            if (!*tag || strlen(tag) >= MAX_NAME || !*value) goto bad;
            for (i = 0; i < supplied_count; i++) if (!strcmp(supplied[i].tag, tag)) { r = &supplied[i]; break; }
            if (r == NULL) {
                if (supplied_count == MAX_REGIONS) goto bad;
                r = &supplied[supplied_count++];
                strcpy(r->tag, tag);
            }
            if (suffix != NULL) {
                size_t size;
                if (r->size_seen) goto bad;
                if (parse_size_decimal(value, &size)) goto bad;
                r->size = size; r->size_seen = 1;
            } else {
                if (r->filename_seen || strlen(value) >= sizeof(r->filename)) goto bad;
                strcpy(r->filename, value); r->filename_seen = 1;
            }
            continue;
        }
        for (i = 0; i < 11 && strcmp(key, keys[i]); i++) { }
        if (i == 11 || (seen & (1u << i)) || !*value || strlen(value) >= sizeof(values[i])) goto bad;
        strcpy(values[i], value); seen |= 1u << i;
    }
    if (ferror(file) || seen != 0x7ff || strcmp(values[0], "pcsx-aow-system10-v1") ||
        strcmp(values[10], "false")) goto bad;
    profile = find_profile(values[1]);
    if (profile == NULL || !profile_supported(profile) ||
        strcmp(values[2], profile->parent) || strcmp(values[3], profile->board_type) ||
        strcmp(values[4], profile->crypto) || strcmp(values[5], profile->machine) ||
        strcmp(values[6], profile->input) || strcmp(values[7], profile->state) ||
        strcmp(values[8], profile->init) || strcmp(values[9], profile->flags) ||
        parse_profile_regions(profile, expected, expected_count)) goto bad;
    if (supplied_count != *expected_count) goto bad;
    for (i = 0; i < *expected_count; i++) {
        Region *r = NULL;
        for (j = 0; j < supplied_count; j++) if (!strcmp(supplied[j].tag, expected[i].tag)) { r = &supplied[j]; break; }
        if (r == NULL || !r->filename_seen || !r->size_seen ||
            strcmp(r->filename, expected[i].filename) || r->size != expected[i].size)
            goto bad;
    }
    *profile_out = profile;
    fclose(file);
    return 0;
bad:
    fprintf(stderr, "%s: invalid, unsupported, or non-source-matching System10 profile\n", path);
    fclose(file);
    return -1;
}

static int load_region(const char *directory, const ProfileRegion *expected,
                       Region *region)
{
    char path[4096];
    FILE *file;
    long length;
    if (snprintf(path, sizeof(path), "%s/%s", directory, expected->filename) >= (int)sizeof(path)) return -1;
    file = fopen(path, "rb");
    if (file == NULL) { perror(path); return -1; }
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        (size_t)length != expected->size || fseek(file, 0, SEEK_SET)) {
        fprintf(stderr, "%s: region size mismatch (expected %zu bytes)\n", path, expected->size);
        fclose(file); return -1;
    }
    region->bytes = (uint8_t *)malloc(expected->size);
    if (region->bytes == NULL) { fclose(file); return -1; }
    if (fread(region->bytes, 1, expected->size, file) != expected->size) {
        fclose(file); return -1;
    }
    if (fclose(file)) return -1;
    strcpy(region->tag, expected->tag);
    strcpy(region->filename, expected->filename);
    region->size = expected->size;
    return 0;
}

static Region *region_by_tag(const char *tag)
{
    size_t i;
    for (i = 0; i < board.region_count; i++)
        if (!strcmp(board.regions[i].tag, tag)) return &board.regions[i];
    return NULL;
}

static int create_device(void)
{
    NS10BoardConfig *cfg;
    NS10NANDConfig nand_configs[16] = {{0}};
    Region *main_region, *flash_region, *data_region;
    size_t i, nand_count = 0;
    int result;
    NS10IOConfig io_config = {input_system_read, input_in1_read, irq10, NULL, 0xff};
    NS10BoardConfig config;

    if (ns10_io_create(&io_config, &board.io) != NS10_IO_OK) return -1;
    main_region = region_by_tag("maincpu:rom");
    if (main_region == NULL || main_region->size != SYSTEM10_ROM_SIZE) return -1;
    board.program_rom = (uint8_t *)malloc(SYSTEM10_ROM_SIZE);
    if (board.program_rom == NULL) return -1;
    memcpy(board.program_rom, main_region->bytes, SYSTEM10_ROM_SIZE);
    memset(&config, 0, sizeof(config));
    cfg = &config;
    cfg->game_profile = board.profile->crypto;
    cfg->program_rom = board.program_rom;
    cfg->program_rom_size = SYSTEM10_ROM_SIZE;
    cfg->io = board.io;

    if (!strcmp(board.profile->board_type, "MEM(M)")) {
        flash_region = region_by_tag("nand");
        data_region = region_by_tag("data");
        if (flash_region == NULL || flash_region->size != 0x800000 ||
            data_region == NULL || data_region->size != 0x2000000) return -1;
        board.flash_image = (uint8_t *)malloc(flash_region->size);
        if (board.flash_image == NULL) return -1;
        memcpy(board.flash_image, flash_region->bytes, flash_region->size);
        cfg->kind = NS10_BOARD_MEMM;
        cfg->memm_flash_image = board.flash_image;
        cfg->memm_flash_size = flash_region->size;
        cfg->memm_nand_region = flash_region->bytes;
        cfg->memm_nand_region_size = flash_region->size;
        cfg->memm_data_region = data_region->bytes;
        cfg->memm_data_region_size = data_region->size;
    } else {
        const int is_2808 = !strcmp(board.profile->nand_type, "K9F2808U0B");
        const int is_5608 = !strcmp(board.profile->nand_type, "K9F5608U0D");
        if (!is_2808 && !is_5608) return -1;
        for (i = 0; i < board.region_count; i++) {
            Region *r = &board.regions[i];
            if (strncmp(r->tag, "nand", 4) || r->tag[4] == 0) continue;
            if (r->size % 528 || nand_count >= 16) return -1;
            nand_configs[nand_count].bytes = r->bytes;
            nand_configs[nand_count].writable_bytes = r->bytes;
            nand_configs[nand_count].size = r->size;
            nand_configs[nand_count].pages = (uint32_t)(r->size / 528);
            nand_configs[nand_count].manufacturer_id = 0xec;
            nand_configs[nand_count].device_id = is_2808 ? 0x73 : 0x75;
            nand_configs[nand_count].sequential_row_read = is_5608 ? 1 : 0;
            nand_count++;
        }
        if (nand_count != (size_t)strtoul(board.profile->nand_count, NULL, 10)) return -1;
        cfg->kind = NS10_BOARD_MEMN;
        cfg->nand_chips = nand_configs;
        cfg->nand_chip_count = nand_count;
    }
    result = ns10_board_create(cfg, &board.device);
    return result == NS10_BOARD_OK ? 0 : -1;
}

static uint32_t physical_address(uint32_t address)
{
    return address >= 0x80000000u && address < 0xc0000000u ?
        address & 0x1fffffffu : address;
}

int system10_load(const char *directory)
{
    ProfileRegion expected[MAX_REGIONS] = {0};
    size_t expected_count = 0, i;
    const Profile *profile;
    if (board.device || board.io || systemtaito_active() || systemzn_active() || system11_active() || system12_active() ||
        psxRegs.ptrs.psxM != NULL || directory == NULL) return -1;
    if (parse_config(directory, &profile, expected, &expected_count)) return -1;
    board.profile = profile;
    for (i = 0; i < expected_count; i++) {
        board.region_count++;
        if (load_region(directory, &expected[i], &board.regions[i])) goto bad;
    }
    board.system_inputs = 0xffff;
    board.player_inputs = 0xffffffffu;
    if (create_device()) goto bad;
    if (gpu_set_model(2) || gpu_set_vram_height(1024)) goto bad;
    psxRamSize = SYSTEM10_RAM_SIZE;
    psxRomSize = SYSTEM10_ROM_SIZE;
    system10_reset();
    return 0;
bad:
    fprintf(stderr, "System10: board initialization failed\n");
    system10_unload();
    return -1;
}

void system10_unload(void)
{
    size_t i;
    ns10_board_destroy(board.device);
    ns10_io_destroy(board.io);
    free(board.program_rom);
    free(board.flash_image);
    for (i = 0; i < board.region_count; i++) free(board.regions[i].bytes);
    memset(&board, 0, sizeof(board));
    if (!psxRegs.ptrs.psxM && !systemtaito_active() && !systemzn_active() && !system11_active() && !system12_active()) {
        psxRamSize = 0x200000;
        psxRomSize = 0x80000;
        gpu_set_model(2);
        gpu_set_vram_height(512);
    }
}

int system10_active(void) { return board.device != NULL; }

void system10_mem_reset(void)
{
    if (!board.device || psxRegs.ptrs.psxR == NULL || psxRegs.ptrs.psxM == NULL) return;
    memcpy(psxRegs.ptrs.psxR, board.program_rom, SYSTEM10_ROM_SIZE);
    memset(psxRegs.ptrs.psxM, 0, PSX_RAM_SIZE);
    if (psxRegs.ptrs.psxP != NULL) memset(psxRegs.ptrs.psxP, 0xff, 0x10000);
}

void system10_reset(void)
{
    if (!board.device) return;
    psxRegs.biosBranchCheck = ~0u;
    psxRegs.biosFuncsHooked = 0;
    ns10_board_reset(board.device);
    board.last_cycle = psxRegs.cycle;
    board.cycle_fraction = 0;
    board.io_elapsed_us = 0;
    board.dma_transfers = board.dma_bytes = 0;
    board.read_errors = board.write_errors = board.fetch_errors = 0;
    board.unsupported_accesses = board.unmapped_accesses = 0;
    board.last_address = board.last_value = 0;
    board.last_width = 0;
    board.fault = 0;
}

int system10_sync(uint32_t psx_cycle)
{
    uint32_t elapsed;
    uint64_t numerator, microseconds;
    if (!board.device) return 0;
    elapsed = psx_cycle - board.last_cycle;
    board.last_cycle = psx_cycle;
    numerator = board.cycle_fraction + (uint64_t)elapsed * 1000000u;
    microseconds = numerator / PSXCLK;
    board.cycle_fraction = numerator % PSXCLK;
    board.io_elapsed_us += microseconds;
    ns10_board_advance_us(board.device, microseconds);
    return board.fault ? -1 : 0;
}

static int access_result(int result, uint32_t address, uint32_t value,
                         unsigned width, int is_write, int is_fetch)
{
    if (result == NS10_BOARD_UNMAPPED) {
        board.unmapped_accesses++;
        return 0;
    }
    if (result != NS10_BOARD_OK) {
        board.fault = 1;
        board.unsupported_accesses++;
        board.last_address = address;
        board.last_value = value;
        board.last_width = width;
        if (is_write) board.write_errors++;
        else if (is_fetch) board.fetch_errors++;
        else board.read_errors++;
    }
    return 1;
}

int system10_read(uint32_t address, unsigned width, uint32_t *value)
{
    uint32_t physical = physical_address(address), result_value = UINT32_MAX;
    int result;
    if (!board.device) return 0;
    (void)system10_sync(psxRegs.cycle);
    result = ns10_board_read(board.device, physical, width, &result_value);
    if (result == NS10_BOARD_OK) {
        if (value != NULL) *value = result_value;
        return 1;
    }
    if (access_result(result, physical, result_value, width, 0, 0)) {
        if (value != NULL) *value = UINT32_MAX;
        return 1;
    }
    return 0;
}

int system10_write(uint32_t address, unsigned width, uint32_t value, uint32_t mem_mask)
{
    uint32_t physical = physical_address(address);
    int result;
    if (!board.device) return 0;
    (void)system10_sync(psxRegs.cycle);
    result = ns10_board_write(board.device, physical, width, value, mem_mask);
    return access_result(result, physical, value, width, 1, 0);
}

int system10_fetch(uint32_t address, uint32_t *value)
{
    uint32_t physical = physical_address(address), fetched = UINT32_MAX;
    int result;
    if (!board.device || value == NULL || strcmp(board.profile->board_type, "MEM(M)") ||
        physical < 0x1f400000u || physical > 0x1f5ffffcu) return 0;
    (void)system10_sync(psxRegs.cycle);
    result = ns10_board_read(board.device, physical, 4, &fetched);
    if (result == NS10_BOARD_OK) {
        *value = fetched;
        return 1;
    }
    if (access_result(result, physical, fetched, 4, 0, 1)) {
        *value = UINT32_MAX;
        return 1;
    }
    return 0;
}

int system10_dma_read(uint8_t *destination, size_t bytes)
{
    int result;
    if (!board.device || destination == NULL) return -1;
    (void)system10_sync(psxRegs.cycle);
    result = ns10_board_dma_read(board.device, destination, bytes);
    if (result != NS10_BOARD_OK) {
        board.fault = 1;
        board.unsupported_accesses++;
        return -1;
    }
    board.dma_transfers++;
    board.dma_bytes += bytes;
    return 0;
}

int system10_dma5(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    uint64_t words, bytes;
    uint32_t destination = (madr & PSX_RAM_MASK) & ~3u;
    if (!board.device) return -1;
    words = chcr == 0x01000200u ?
        (uint64_t)((bcr >> 16) ? (bcr >> 16) : 0x10000u) * (bcr & 0xffff) : bcr;
    bytes = words * 4;
    if ((chcr != 0x01000000u && chcr != 0x01000200u) ||
        bytes > PSX_RAM_SIZE - destination) {
        board.fault = 1;
        board.last_address = madr; board.last_value = chcr; board.last_width = 4;
        board.unsupported_accesses++;
        return -1;
    }
    if (system10_dma_read(psxRegs.ptrs.psxM + destination, (size_t)bytes)) return -1;
    psxCpu->Clear(destination, (uint32_t)words);
    return 0;
}

int system10_set_input(const char *name, int pressed)
{
    size_t i;
    uint32_t disabled = 0;
    if (!board.device || name == NULL || (pressed != 0 && pressed != 1)) return -1;
    /* PORT_MODIFY("IN1") unused masks in the eligible MAME input layouts. */
    if (!strcmp(board.profile->input, "gamshara") || !strcmp(board.profile->input, "gjspace"))
        disabled = 0x0fff0000u;
    else if (!strcmp(board.profile->input, "startrgn")) disabled = 0x0fffff6fu;
    else if (!strcmp(board.profile->input, "konotako")) disabled = 0x1fff4040u;
    for (i = 0; i < sizeof(input_bindings) / sizeof(input_bindings[0]); i++) {
        const struct InputBinding *binding = &input_bindings[i];
        if (strcmp(name, binding->name)) continue;
        if (!binding->system && (binding->mask & disabled)) return -1;
        if (binding->system) {
            if (pressed) board.system_inputs &= (uint16_t)~binding->mask;
            else board.system_inputs |= (uint16_t)binding->mask;
        } else {
            if (pressed) board.player_inputs &= ~binding->mask;
            else board.player_inputs |= binding->mask;
        }
        return 0;
    }
    return -1;
}

int system10_selftest(void)
{
    uint32_t value;
    uint32_t old_i_stat;
    unsigned failures = 0;
    if (!board.device || psxRegs.ptrs.psxM == NULL) return -1;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "System10 selftest failed at line %u: %s\n", __LINE__, #x); failures++; } } while (0)
    old_i_stat = psxHu32(0x1070);
    CHECK(PSX_RAM_SIZE == SYSTEM10_RAM_SIZE && PSX_ROM_SIZE == SYSTEM10_ROM_SIZE);
    CHECK(system10_set_input("p1button1", 1) == 0);
    CHECK(system10_read(0x1fba0004, 4, &value) == 1 && (value & (1u << 4)) == 0);
    CHECK(system10_set_input("p1button1", 0) == 0);
    CHECK(system10_read(0x1fba0004, 4, &value) == 1 && (value & (1u << 4)) != 0);
    CHECK(system10_read(0xfffe0130, 4, &value) == 0);
    CHECK(system10_write(0xfffe0130, 4, 0, 0xffffffffu) == 0);
    CHECK(psxMemRead32(&psxRegs, 0xbfc00000) == SWAPu32(*(uint32_t *)board.program_rom));
    CHECK(system10_sync(psxRegs.cycle) == 0);
    CHECK(system10_read(0x1fba001a, 2, &value) == 1 && (value & 8) != 0);
    CHECK((psxHu32(0x1070) & (1u << 10)) != 0);
    CHECK(system10_write(0x1fba0018, 2, 0, 0xffff) == 1);
    CHECK(system10_read(0x1fba001a, 2, &value) == 1 && (value & 8) == 0);
    psxHu32ref(0x1070) = SWAPu32(old_i_stat);
    ns10_board_reset(board.device);
    board.last_cycle = psxRegs.cycle;
    board.cycle_fraction = 0;
    board.io_elapsed_us = 0;
    board.fault = 0;
    fprintf(stderr, "System10 selftest: %s\n", failures ? "FAIL" : "PASS");
#undef CHECK
    return failures ? -1 : 0;
}

void system10_report(FILE *out)
{
    if (!board.device || out == NULL) return;
    fprintf(out, "System10: profile=%s board=%s crypto=%s RAM=%u ROM=%u NAND=%s x%s\n",
        board.profile->set, board.profile->board_type, board.profile->crypto,
        PSX_RAM_SIZE, PSX_ROM_SIZE, board.profile->nand_type, board.profile->nand_count);
    fprintf(out, "System10: inputs SYSTEM=%04x IN1=%08x elapsed=%" PRIu64 "us crypto_active=%d XOR=%04x NAND=%u\n",
        board.system_inputs, board.player_inputs, board.io_elapsed_us,
        ns10_board_crypto_active(board.device), ns10_board_data_xor(board.device),
        ns10_board_selected_nand(board.device));
    fprintf(out, "System10: DMA=%" PRIu64 " transfers/%" PRIu64 " bytes read=%" PRIu64 " write=%" PRIu64 " fetch=%" PRIu64 " unsupported=%" PRIu64 " unmapped=%" PRIu64 " fault=%d last=%08x/%u/%08x\n",
        board.dma_transfers, board.dma_bytes, board.read_errors, board.write_errors,
        board.fetch_errors, board.unsupported_accesses, board.unmapped_accesses,
        board.fault, board.last_address, board.last_width, board.last_value);
}
