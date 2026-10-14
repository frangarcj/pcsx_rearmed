/* GPL-2.0-or-later. Native host integration for source-derived Konami GV. */
#include "systemgv.h"

#include "system10.h"
#include "system11.h"
#include "system12.h"
#include "systemtaito.h"
#include "systemzn.h"
#include "psxhw.h"
#include "psxmem.h"
#include "r3000a.h"
#ifndef DRC_DISABLE
#include "new_dynarec/new_dynarec.h"
#endif
#include "arcade/chd_cd.h"
#include "arcade/konamigv.h"
#include "arcade/simpbowl_io.h"
#include "arcade/tokimeki_io.h"
#include "gpu_vram.h"

#include <CommonCrypto/CommonDigest.h>
#include "libchdr/chd.h"
#include "arcade/psx_sio.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !defined(PCSX_SYSTEM12)
#error Konami GV host integration requires runtime PSX memory sizing.
#endif

enum { GV_BIOS_BYTES = 0x80000, GV_EEPROM_BYTES = 0x80, GV_MAX_INI = 32,
       GV_MAX_VALUE = 256, GV_MAX_PATH = 4096 };
#define GV_MAME_COMMIT "deb2f54fe7300e7a192690318beb860159bfd52a"
#define GV_BIOS_SHA1 "02a82a2fe1fba0404517c3602324bfa64e23e478"

typedef struct GvProfile {
    const char *set, *parent, *init, *flags, *disk, *disk_sha1, *disk_status;
    const char *eeprom_sha1;
} GvProfile;

/* Exact supported profiles from scripts/konamigv_boards.json. */
static const GvProfile gv_profiles[] = {
    {"tmoshs", "konamigv", "tmoshs_init", "MACHINE_IMPERFECT_SOUND | MACHINE_IMPERFECT_GRAPHICS",
     "755jaa01", "fc742a0b763ba38350ba7eb5d775948632aafd9d", "GOOD",
     "f18a0974a6be69dc179706643aab837ff61c2738"},
    {"tmoshsp", "konamigv", "tmoshsp_init", "MACHINE_IMPERFECT_SOUND | MACHINE_IMPERFECT_GRAPHICS",
     "756jab01", "b2c59b9801debccbbd986728152f314535c67e53", "GOOD",
     "97041e287e4c80066043967450779b81b62b2b8e"},
    {"simpbowl", "konamigv", "empty_init", "MACHINE_IMPERFECT_SOUND",
     "829uaa02", "2ec4cc608d5582e478ee047b60ccee67b52f060c", "GOOD",
     "16ae7f81cbe841c429c5c7326cf83e87db1782bf"},
    {"hyperath", "konamigv", "empty_init", "MACHINE_IMPERFECT_SOUND",
     "gv021-j1", "579442444025b18da658cd6455c51459fbc3de0e", "GOOD",
     "a0f203a999757fba68b391c525ac4b9684a57ba9"},
    {"lacrazyc", "konamigv", "empty_init", "MACHINE_IMPERFECT_SOUND",
     "gv027-a1", "840d0d4876cf1b814c9d8db975aa6c92e1fe4039", "BAD_DUMP",
     "066b49236c658a4ef2930f7bacc4b2354dd7f240"},
    {"nagano98", "konamigv", "empty_init", "MACHINE_IMPERFECT_SOUND",
     "nagano98", "1be7bd4531f249ff2233dd40a206c8d60054a8c6", "BAD_DUMP",
     "a77a37e0cc580934d1e7e05d523bae0acd2c1480"},
    {"naganoj", "nagano98", "empty_init", "MACHINE_IMPERFECT_SOUND",
     "720jaa01", "437160996551ef4dfca43899d1d14beca62eb4c9", "GOOD",
     "768225b04a293bdbc114a092d14dee28d52044e9"},
    {"powyak96", "konamigv", "empty_init", "MACHINE_IMPERFECT_SOUND",
     "powyak96", "ebd0ea18ff9ce300ea1e30d66a739a96acfb0621", "BAD_DUMP",
     "e2d978f49748ba3c4a425188abcd3d272ec23907"},
    {"susume", "lacrazyc", "empty_init", "MACHINE_IMPERFECT_SOUND",
     "gv027j1", "e7e6749ac65de7771eb8fed7d5eefaec3f902255", "BAD_DUMP",
     "b8ad7787b0692713439d7d9bebfa0c801c806006"},
    {"weddingr", "konamigv", "empty_init", "MACHINE_IMPERFECT_SOUND",
     "weddingr", "4e7122b191747ab7220fe4ce1b4483d62ab579af", "BAD_DUMP",
     "41510a0ceded81dcb26a70eba97636d38d3742c3"},
};

typedef struct GvBoard {
    KonamiGV *device;
    psx_sio_t *sio[2];
    ChdCd *cd;
    SimpBowlIO *simp;
    TokimekiIO *tok;
    systemgv_printer_capture_cb capture;
    void *capture_opaque;
    char printer_dir[GV_MAX_PATH];
    uint8_t *flash;
    uint16_t track[2];
    char flash_path[GV_MAX_PATH];
    uint8_t flash_key[20];
    int flash_warm;
    const GvProfile *profile;
    uint8_t *bios, *eeprom;
    uint8_t device_eeprom[GV_EEPROM_BYTES];
    uint32_t inputs[3];
    uint32_t last_cycle;
    uint64_t now_ns, cycle_fraction, sio_cycles;
    uint64_t dma_requests, mmio_reads, mmio_writes;
    uint32_t cd_dma_words;
    uint32_t old_ram_size, old_rom_size;
    int active, fault, memory_initialized;
} GvBoard;
static GvBoard board;

static void sony_sio_irq(void *opaque, int state)
{
    unsigned port = (unsigned)(uintptr_t)opaque;
    if (state && psxRegs.ptrs.psxH)
        psxHu32ref(0x1070) |= SWAPu32(1u << (7 + port));
}

static int sony_sio_bus(uint32_t address, unsigned width, uint32_t *value,
                        uint32_t mask, int write)
{
    if (address < 0x1f801040u || address >= 0x1f801060u ||
        (address & (width - 1))) return 0;
    unsigned port = (address - 0x1f801040u) >> 4;
    unsigned shift = (address & 3u) * 8;
    uint32_t raw = 0;
    int error;
    if (write)
        error = psx_sio_write(board.sio[port], (address >> 2) & 3,
                              *value << shift, mask << shift);
    else {
        error = psx_sio_read(board.sio[port], (address >> 2) & 3,
                             mask << shift, &raw);
        *value = (raw >> shift) & mask;
    }
    if (error) { board.fault = 1; return -1; }
    return 1;
}

#ifndef DRC_DISABLE
/* GV's fixed BIOS and RAM use the normal direct mappings. Register a code
 * provider nevertheless so the DRC installs the arcade MMIO handler tables
 * for the board's NCR aperture and DMA extensions. */
static int systemgv_code_span(unsigned int pc, const unsigned int **words,
                              unsigned int *limit)
{
    (void)pc;
    (void)words;
    (void)limit;
    return 0;
}
#endif

typedef struct IniEntry { const char *key; char value[GV_MAX_VALUE]; } IniEntry;
static const char *const ini_keys[GV_MAX_INI] = {
    "format", "mame_set", "mame_parent", "bios_parent", "mame_machine",
    "mame_input", "mame_state", "mame_init", "mame_flags", "cpu_device",
    "cpu_clock_hz", "gpu_device", "gpu_clock_hz", "main_ram_size",
    "vram_size", "scsi_device", "scsi_clock_hz", "cdrom_device",
    "eeprom_device", "duart_device", "duart_clock_hz", "spu_clock_hz",
    "game_execution_verified", "region.maincpu:rom", "region.maincpu:rom.size",
    "region.eeprom", "region.eeprom.size", "disk.cdrom", "disk.cdrom.sha1",
    "disk.cdrom.status", "unsupported_game_io", ""
};

static uint32_t physical_address(uint32_t address)
{
    return address >= 0x80000000u && address < 0xc0000000u ?
        address & 0x1fffffffu : address;
}

static int overlaps(uint32_t address, unsigned width, uint32_t start, uint32_t size)
{
    uint64_t end = (uint64_t)address + width;
    return address < start + size && end > start;
}

static const GvProfile *find_profile(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof(gv_profiles) / sizeof(gv_profiles[0]); i++)
        if (!strcmp(name, gv_profiles[i].set)) return &gv_profiles[i];
    return NULL;
}

static void irq10(void *opaque, int asserted)
{
    (void)opaque;
    if (asserted && psxRegs.ptrs.psxH)
        psxHu32ref(0x1070) |= SWAPu32(1u << 10);
}

static uint32_t read_input(void *opaque, unsigned port)
{
    (void)opaque;
    if(board.tok){
      if(port==0)return board.inputs[0]|0xffffc3e0u;
      if(port==1)return 0xfffffbffu|(tokimeki_io_device_bit(board.tok)<<10);
      return UINT32_MAX;
    }
    return port < 3 ? board.inputs[port] : UINT32_MAX;
}

static uint8_t read_ram8(void *opaque, uint32_t address)
{
    (void)opaque;
    if (!psxRegs.ptrs.psxM || address >= PSX_RAM_SIZE) {
        board.fault = 1;
        return 0xff;
    }
    return psxRegs.ptrs.psxM[address];
}

static void write_ram8(void *opaque, uint32_t address, uint8_t value)
{
    (void)opaque;
    if (!psxRegs.ptrs.psxM || address >= PSX_RAM_SIZE) {
        board.fault = 1;
        return;
    }
    psxRegs.ptrs.psxM[address] = value;
}

static int read_exact_file(const char *directory, const char *name,
                           size_t expected, uint8_t **result)
{
    char path[GV_MAX_PATH];
    FILE *file;
    uint8_t *data;
    long size;
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path))
        return -1;
    file = fopen(path, "rb");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 ||
        (size_t)size != expected || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return -1;
    }
    data = (uint8_t *)malloc(expected ? expected : 1);
    if (!data) { fclose(file); return -1; }
    {
        int read_failed = fread(data, 1, expected, file) != expected;
        int close_failed = fclose(file) != 0;
        if (read_failed || close_failed) {
            free(data);
            return -1;
        }
    }
    *result = data;
    return 0;
}

static int file_sha1(const uint8_t *data, size_t size, const char *expected)
{
    unsigned char digest[CC_SHA1_DIGEST_LENGTH];
    char hex[CC_SHA1_DIGEST_LENGTH * 2 + 1];
    static const char digits[] = "0123456789abcdef";
    size_t i;
    if (!CC_SHA1(data, (CC_LONG)size, digest)) return 0;
    for (i = 0; i < sizeof(digest); i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[sizeof(hex) - 1] = 0;
    return !strcmp(hex, expected);
}

static int read_ini(const char *directory, IniEntry values[GV_MAX_INI])
{
    char path[GV_MAX_PATH], line[512];
    FILE *file;
    unsigned seen = 0, count = 0, i;
    if (snprintf(path, sizeof(path), "%s/board.ini", directory) >= (int)sizeof(path))
        return -1;
    file = fopen(path, "r");
    if (!file) return -1;
    while (fgets(line, sizeof(line), file)) {
        char *end, *equal;
        if (!strchr(line, '\n') && !feof(file)) goto bad;
        end = line + strcspn(line, "\r\n");
        *end = 0;
        if (!*line) continue;
        equal = strchr(line, '=');
        if (!equal || strchr(equal + 1, '=')) goto bad;
        *equal++ = 0;
        for (i = 0; i < GV_MAX_INI - 1 && strcmp(ini_keys[i], line); i++) {}
        if (i == GV_MAX_INI - 1 || (seen & (1u << i)) ||
            strlen(equal) >= sizeof(values[i].value)) goto bad;
        strcpy(values[i].value, equal);
        seen |= 1u << i;
        count++;
    }
    if (ferror(file) || count != GV_MAX_INI - 1 || seen != ((1u << (GV_MAX_INI - 1)) - 1))
        goto bad;
    fclose(file);
    return 0;
bad:
    fclose(file);
    fprintf(stderr, "%s: malformed or incomplete Konami GV board.ini\n", path);
    return -1;
}

static const char *ini_value(const IniEntry values[GV_MAX_INI], const char *key)
{
    unsigned i;
    for (i = 0; i < GV_MAX_INI - 1; i++)
        if (!strcmp(ini_keys[i], key)) return values[i].value;
    return "";
}

static int validate_manifest(const char *directory, const GvProfile *profile)
{
    char path[GV_MAX_PATH], *text;
    FILE *file;
    long size;
    int ok;
    if (snprintf(path, sizeof(path), "%s/manifest.json", directory) >= (int)sizeof(path))
        return -1;
    file = fopen(path, "rb");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0 || size > 1024 * 1024 ||
        fseek(file, 0, SEEK_SET)) { fclose(file); return -1; }
    text = (char *)malloc((size_t)size + 1);
    if (!text) { fclose(file); return -1; }
    {
        int read_failed = fread(text, 1, (size_t)size, file) != (size_t)size;
        int close_failed = fclose(file) != 0;
        if (read_failed || close_failed) {
            free(text);
            return -1;
        }
    }
    text[size] = 0;
    {
        char set_field[128];
        snprintf(set_field, sizeof(set_field), "\"mame_set\": \"%s\"", profile->set);
        ok = strstr(text, "\"system\": \"Konami System GV\"") &&
         strstr(text, "\"mame_driver\": \"konami/konamigv.cpp\"") &&
         strstr(text, "\"mame_source_commit\": \"" GV_MAME_COMMIT "\"") &&
         strstr(text, set_field) &&
         strstr(text, "\"game_execution_verified\": false");
    }
    free(text);
    return ok ? 0 : -1;
}

static int validate_ini(const char *directory, const GvProfile *profile)
{
    IniEntry values[GV_MAX_INI] = {0};
    char disk_filename[128];
    static const struct { const char *key, *value; } common[] = {
        {"format", "pcsx-aow-system-gv-v1"}, {"bios_parent", "konamigv"},
        {"cpu_device", "CXD8530BQ"},
        {"cpu_clock_hz", "67737600"}, {"gpu_device", "CXD8514Q"},
        {"gpu_clock_hz", "53693175"}, {"main_ram_size", "2097152"},
        {"vram_size", "1048576"}, {"scsi_device", "NCR53CF96"},
        {"scsi_clock_hz", "16000000"}, {"cdrom_device", "XM-5401"},
        {"eeprom_device", "93C46 16-bit"}, {"duart_device", "MB89371"},
        {"duart_clock_hz", "4000000"}, {"spu_clock_hz", "33868800"},
        {"game_execution_verified", "false"}, {"region.maincpu:rom", "bios.bin"},
        {"region.maincpu:rom.size", "524288"}, {"region.eeprom", "eeprom.bin"},
        {"region.eeprom.size", "128"}
    };
    size_t i;
    if (snprintf(disk_filename, sizeof(disk_filename), "%s.chd", profile->disk) >=
        (int)sizeof(disk_filename)) return -1;
    if (read_ini(directory, values)) return -1;
    if (strcmp(ini_value(values, "mame_set"), profile->set) ||
        strcmp(ini_value(values, "mame_parent"), profile->parent) ||
        strcmp(ini_value(values, "mame_init"), profile->init) ||
        strcmp(ini_value(values, "mame_flags"), profile->flags) ||
        strcmp(ini_value(values, "disk.cdrom"), disk_filename) ||
        strcmp(ini_value(values, "disk.cdrom.sha1"), profile->disk_sha1) ||
        strcmp(ini_value(values, "disk.cdrom.status"), profile->disk_status)) return -1;
    int special = !strcmp(profile->set,"simpbowl"),tok=!strncmp(profile->set,"tmosh",5);
    const char *machine=tok?"tmosh":special?"simpbowl":"konamigv";
    const char *state=tok?"tokimeki_state":special?"simpbowl_state":"konamigv_state";
    const char *io=ini_value(values,"unsupported_game_io");
    if(strcmp(ini_value(values,"mame_machine"),machine) || strcmp(ini_value(values,"mame_input"),machine) ||
       strcmp(ini_value(values,"mame_state"),state) || (*io &&
       (tok?strcmp(io,"Tokimeki sensor/printer controller and print media path"):
        special?strcmp(io,"trackball interface; flash command protocol"):1)))return -1;
    for (i = 0; i < sizeof(common) / sizeof(common[0]); i++)
        if (strcmp(ini_value(values, common[i].key), common[i].value)) return -1;
    return 0;
}

static int validate_chd_hash(const char *path, const char *expected)
{
    chd_file *chd = NULL;
    const chd_header *header;
    char digest[41];
    static const char hex[] = "0123456789abcdef";
    unsigned i;
    if (chd_open(path, CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE) return -1;
    header = chd_get_header(chd);
    if (!header) { chd_close(chd); return -1; }
    for (i = 0; i < 20; i++) {
        digest[i * 2] = hex[header->sha1[i] >> 4];
        digest[i * 2 + 1] = hex[header->sha1[i] & 15];
    }
    digest[40] = 0;
    chd_close(chd);
    return !strcmp(digest, expected) ? 0 : -1;
}

/* A separate mutable image, bound to the exact original BIOS/EEPROM/CD set.
 * Missing state starts erased; invalid existing state is never overwritten. */
static int init_simp(const char *directory, int fixture)
{
    char binding[256];uint8_t header[64]={0},digest[20];
    if(snprintf(board.flash_path,sizeof(board.flash_path),"%s/%s",directory,
        fixture?"simpbowl.fixture.flash.nvram":"flash.nvram")>=(int)sizeof(board.flash_path))return -1;
    snprintf(binding,sizeof(binding),"%s|%s|%s|%s",fixture?"SIMP-FIXTURE":"simpbowl",
        GV_BIOS_SHA1,"16ae7f81cbe841c429c5c7326cf83e87db1782bf","2ec4cc608d5582e478ee047b60ccee67b52f060c");
    CC_SHA1(binding,(CC_LONG)strlen(binding),board.flash_key);
    board.flash=malloc(SIMPBOWL_FLASH_BYTES);if(!board.flash)return -1;
    memset(board.flash,0xff,SIMPBOWL_FLASH_BYTES);
    FILE *f=fopen(board.flash_path,"rb");
    if(f){
        int failed=fread(header,1,64,f)!=64 || memcmp(header,"PCSX-SIMP-FLASH1",16) ||
          memcmp(header+16,board.flash_key,20) || header[56]!=0 || header[57]!=0 || header[58]!=0x80 || header[59]!=0 ||
          header[60]!=1 || header[61]!=0 || header[62]!=0 || header[63]!=0 ||
          fread(board.flash,1,SIMPBOWL_FLASH_BYTES,f)!=SIMPBOWL_FLASH_BYTES || fgetc(f)!=EOF || ferror(f);
        if(fclose(f))failed=1;
        CC_SHA1(board.flash,SIMPBOWL_FLASH_BYTES,digest);if(memcmp(header+36,digest,20))failed=1;
        if(failed){fprintf(stderr,"SystemGV: invalid or wrong-profile flash.nvram\n");return -1;}
        board.flash_warm=1;
    }else if(errno!=ENOENT){fprintf(stderr,"SystemGV: cannot read flash.nvram\n");return -1;}
    board.simp=simpbowl_io_create(board.flash,SIMPBOWL_FLASH_BYTES);
    board.track[0]=0xfff; // source TRACK0_X PORT_REVERSE default
    return board.simp?0:-1;
}
const char *systemgv_flash_path(void){return board.simp?board.flash_path:NULL;}
int systemgv_flush_flash(void)
{
    if(!board.simp)return 0;char temporary[GV_MAX_PATH];uint8_t header[64]={0};
    if(snprintf(temporary,sizeof(temporary),"%s.tmp.%ld",board.flash_path,(long)getpid())>=(int)sizeof(temporary))return -1;
    memcpy(header,"PCSX-SIMP-FLASH1",16);memcpy(header+16,board.flash_key,20);
    CC_SHA1(board.flash,SIMPBOWL_FLASH_BYTES,header+36);header[58]=0x80;header[60]=1;
    FILE *f=fopen(temporary,"wb");if(!f)return -1;
    int failed=fwrite(header,1,64,f)!=64 || fwrite(board.flash,1,SIMPBOWL_FLASH_BYTES,f)!=SIMPBOWL_FLASH_BYTES;
    if(fclose(f))failed=1;
    if(failed || rename(temporary,board.flash_path)){remove(temporary);return -1;}return 0;
}
int systemgv_has_trackball(void){return board.active && board.simp!=NULL;}
unsigned systemgv_analog_max(const char *name){if(systemgv_has_sensors() && name){if(!strcmp(name,"heartbeat"))return 101;if(!strcmp(name,"gsr"))return 128;}return systemgv_has_trackball() && name && (!strcmp(name,"p1trackx") || !strcmp(name,"p1tracky"))?0xfff:0;}
static unsigned tok_heart=31,tok_gsr=0,tok_meta=3;
int systemgv_sensor_values(unsigned *heart,unsigned *gsr){if(!systemgv_has_sensors())return -1;if(heart)*heart=tok_heart;if(gsr)*gsr=tok_gsr;return 0;}
int systemgv_set_analog(const char *name,unsigned value){
 if(board.tok){unsigned max=systemgv_analog_max(name);if(!max || value>max)return -1;if(!strcmp(name,"heartbeat"))tok_heart=value;else tok_gsr=value;tokimeki_io_inputs(board.tok,tok_heart,tok_gsr,tok_meta);return 0;}
 if(!systemgv_analog_max(name) || value>0xfff)return -1;board.track[name[7]=='y']=value;return 0;}
int systemgv_trackball_delta(int x,int y){if(!systemgv_has_trackball())return -1;board.track[0]=(board.track[0]+(uint32_t)x)&0xfff;board.track[1]=(board.track[1]+(uint32_t)y)&0xfff;return 0;}
void systemgv_frame(void){if(board.active && board.simp)simpbowl_io_frame(board.simp,board.track[0],board.track[1]);}

static int capture_printer(void *opaque,const uint8_t **rgb,unsigned *w,unsigned *h,unsigned *stride)
{(void)opaque;return board.capture?board.capture(board.capture_opaque,rgb,w,h,stride):0;}
static void printed_page(void *opaque,const uint8_t *rgb,unsigned w,unsigned h,unsigned stride)
{
 (void)opaque;if(!*board.printer_dir)return;char path[GV_MAX_PATH];TokimekiIOStatus st;tokimeki_io_status(board.tok,&st);
 if(snprintf(path,sizeof(path),"%s/print-%04" PRIu64 ".ppm",board.printer_dir,st.prints)>=(int)sizeof(path)){board.fault=1;return;}
 FILE *f=fopen(path,"wb");if(!f){board.fault=1;return;}int failed=fprintf(f,"P6\n%u %u\n255\n",w,h)<0;
 for(unsigned y=0;y<h && !failed;y++)failed=fwrite(rgb+(size_t)y*stride,1,w*3,f)!=w*3;
 if(fclose(f))failed=1;if(failed)board.fault=1;else fprintf(stderr,"Tokimeki printed page: %s\n",path);
}
int systemgv_has_sensors(void){return board.active && board.tok!=NULL;}
int systemgv_set_printer_video(systemgv_printer_capture_cb cb,void *opaque,const char *dir)
{
 if(!board.tok)return -1;if(dir && snprintf(board.printer_dir,sizeof(board.printer_dir),"%s",dir)>=(int)sizeof(board.printer_dir))return -1;
 board.capture=cb;board.capture_opaque=opaque;return 0;
}
void systemgv_vblank(int state)
{
 if(board.active && board.tok){if(systemgv_sync(psxRegs.cycle))return;tokimeki_io_vblank(board.tok,state);TokimekiIOStatus st;tokimeki_io_status(board.tok,&st);if(st.fault)board.fault=1;}
}
static int tok_bus(uint32_t a,unsigned w,uint32_t *v,uint32_t mask,int write)
{
 if(!board.tok)return 0;
 if(write){
  if(a<0x1f680090 || a>=0x1f680092)return 0;uint16_t data=0;int selected=0;
  for(unsigned i=0;i<w;i++)if(a+i<0x1f680092 && (mask&(0xffu<<(8*i)))){data|=((*v>>(8*i))&255)<<((a+i-0x1f680090)*8);selected=1;}
  if(selected)tokimeki_io_serial_write(board.tok,data);return 1;
 }else{
  if(a<0x1f680080 || a>=0x1f680082)return 0;uint16_t data=tokimeki_io_serial_read(board.tok);*v=(data>>((a-0x1f680080)*8));return 1;
 }
}

static int sync_devices(uint64_t now_ns)
{
    konamigv_status_t after;
    if (!board.device || !konamigv_advance_to(board.device, now_ns)) {
        board.fault = 1;
        return -1;
    }
    konamigv_get_status(board.device, &after);
    if(chd_cd_advance_to(board.cd,now_ns)!=1){board.fault=1;return -1;}
    if (after.faults || after.uart_faults || after.cdrom.target.faults) {
        board.fault = 1;
        return -1;
    }
    if(board.tok && tokimeki_io_advance_to(board.tok,now_ns)){board.fault=1;return -1;}
    if(board.simp && simpbowl_io_advance_to(board.simp,now_ns)){board.fault=1;return -1;}
    board.now_ns = now_ns;
    return 0;
}

static int advance_cd_media(void *opaque,uint64_t ns)
{(void)opaque;return chd_cd_advance_to(board.cd,ns);}
static int activate_device(const nscsi_cd_media_ops_t *media)
{
    konamigv_config_t config;
    size_t i;
    for (i = 0; i < 3; i++) board.inputs[i] = UINT32_MAX;
    memset(&config, 0, sizeof(config));
    config.opaque = &board;
    config.read_input = read_input;
    config.read_ram8 = read_ram8;
    config.write_ram8 = write_ram8;
    config.irq10 = irq10;
    config.cd_media = *media;
    config.advance_media=advance_cd_media;
    /* The audited ROM_REGION16_BE bytes are the on-disk dump. The 93C46
     * port stores words as little-endian bytes, matching MAME's NVRAM export.
     * Normalize in device memory while keeping the original hash-checked
     * region untouched. */
    for (i = 0; i < GV_EEPROM_BYTES; ++i)
        board.device_eeprom[i] = board.eeprom[i ^ 1u];
    config.eeprom_image = board.device_eeprom;
    config.eeprom_image_bytes = GV_EEPROM_BYTES;
    board.device = konamigv_create(&config);
    /* Pinned video/psx.cpp selects type 1 only for CXD8538Q. The GV's
     * CXD8514Q uses type-2 packet fields, despite its early CPU revision. */
    if (!board.device || gpu_set_model(2) || gpu_set_vram_height(512)) {
        fprintf(stderr, "SystemGV: common board/device initialization failed\n");
        return -1;
    }
    /* These are CPU devices in the pinned psx.cpp internal map. GV has no
     * serial peripheral connected to their output pins; the board's MB89371
     * is a separate device. Preserve the source's idle status 0x0005. */
    for (i = 0; i < 2; ++i) {
        psx_sio_callbacks_t cb = {0};
        cb.opaque = (void *)(uintptr_t)i; cb.irq = sony_sio_irq;
        if (psx_sio_create(i ? PSX_SIO1 : PSX_SIO0, &cb, &board.sio[i])) return -1;
    }
    board.old_ram_size = psxRamSize;
    board.old_rom_size = psxRomSize;
    psxRamSize = KONAMIGV_RAM_BYTES;
    psxRomSize = GV_BIOS_BYTES;
    board.last_cycle = psxRegs.cycle;
    board.active = 1;
#ifndef DRC_DISABLE
    new_dynarec_set_code_span_provider(systemgv_code_span);
#endif
    return 0;
}

int systemgv_load(const char *directory)
{
    IniEntry values[GV_MAX_INI] = {0};
    const GvProfile *profile;
    nscsi_cd_media_ops_t media;
    chd_cd_config_t cd_config = {0};
    char path[GV_MAX_PATH], error[256];
    if (board.active || !directory || !*directory || systemtaito_active() ||
        systemzn_active() || system10_active() || system11_active() || system12_active())
        return -1;
    if (read_ini(directory, values)) return -1;
    profile = find_profile(ini_value(values, "mame_set"));
    if (!profile || validate_ini(directory, profile) || validate_manifest(directory, profile)) {
        fprintf(stderr, "SystemGV: profile is not a supported GV configuration\n");
        return -1;
    }
    board.profile = profile;
    if (read_exact_file(directory, "bios.bin", GV_BIOS_BYTES, &board.bios) ||
        !file_sha1(board.bios, GV_BIOS_BYTES, GV_BIOS_SHA1) ||
        read_exact_file(directory, "eeprom.bin", GV_EEPROM_BYTES, &board.eeprom) ||
        !file_sha1(board.eeprom, GV_EEPROM_BYTES, profile->eeprom_sha1)) {
        fprintf(stderr, "SystemGV: BIOS/EEPROM size or source SHA1 mismatch\n");
        goto fail;
    }
    if (snprintf(path, sizeof(path), "%s/%s.chd", directory, profile->disk) >= (int)sizeof(path) ||
        validate_chd_hash(path, profile->disk_sha1)) {
        fprintf(stderr, "SystemGV: CHD header SHA1 does not match the source profile\n");
        goto fail;
    }
    board.cd = chd_cd_open(path, &cd_config, error, sizeof(error));
    if (!board.cd || chd_cd_get_media_ops(board.cd, &media) != 1) {
        fprintf(stderr, "SystemGV: cannot open local CHD: %s\n", board.cd ? "media API error" : error);
        goto fail;
    }
    if (activate_device(&media)) goto fail;
    if(!strcmp(profile->set,"simpbowl") && init_simp(directory,0))goto fail;
    if(!strncmp(profile->set,"tmosh",5)){
      TokimekiIOConfig cfg={0};cfg.device_initial=!strcmp(profile->set,"tmoshs")?0xf073:0xf0ba;
      cfg.manual_layout=!strcmp(profile->set,"tmoshsp");cfg.capture=capture_printer;cfg.printed=printed_page;
      tok_heart=31;tok_gsr=0;tok_meta=3;board.tok=tokimeki_io_create(&cfg);if(!board.tok)goto fail;
    }
    return 0;
fail:
    systemgv_unload();
    return -1;
}

int systemgv_test_load(const char *chd_path)
{
    static const uint32_t guest[] = {
        0x3c081f10, /* lui t0,0x1f10 */
        0x8d090000, /* lw t1,0(t0): P1 inputs */
        0x00000000, /* MIPS-I load delay */
        0x3c0a8000, /* lui t2,0x8000 */
        0xad490100, /* sw t1,0x100(t2): guest signature */
        0x3c08bf80, /* lui t0,0xbf80: PSX I/O */
        0x3c090080, /* lui t1,0x0080 */
        0xad0910f0, /* sw t1,0x10f0(t0): enable DMA5 */
        0x240a0004, /* addiu t2,zero,4: zero blocks means 65536 */
        0xad0a10d4, /* sw t2,0x10d4(t0): BCR */
        0x3c098000, /* lui t1,0x8000: RAM address */
        0xad0910d0, /* sw t1,0x10d0(t0): MADR */
        0x3c090100, /* lui t1,0x0100: start DMA */
        0x35290201, /* ori t1,t1,0x0201: block-mode SCSI write */
        0xad0910d8, /* sw t1,0x10d8(t0): CHCR */
        0x3c0b0003, /* lui t3,3 */
        0x240a0004, /* addiu t2,zero,4 */
        0x014b5025, /* or t2,t2,t3: BCR=3 blocks * 4 words */
        0xad0a10d4, /* sw t2,0x10d4(t0): BCR */
        0x3c091100, /* lui t1,0x1100: CD DMA mode */
        0x35290100, /* ori t1,t1,0x0100: masked bits */
        0xad0910d8, /* sw t1,0x10d8(t0): 12-word CD read */
        0x240a0004, /* addiu t2,zero,4: zero blocks => 65536 */
        0xad0a10d4, /* sw t2,0x10d4(t0): BCR */
        0x3c090100, /* lui t1,0x0100 */
        0x35290200, /* ori t1,t1,0x0200: SCSI read mode */
        0xad0910d8, /* sw t1,0x10d8(t0): channel 5 CHCR */
        0x3c081f00, /* lui t0,0x1f00: NCR aperture */
        0x24090003, /* addiu t1,zero,RESET BUS */
        0xa1090006, /* sb t1,6(t0): NCR command register */
        0x1000ffff, /* beq zero,zero,self */
        0x00000000
    };
    nscsi_cd_media_ops_t media;
    chd_cd_config_t cd_config = {0};
    char error[256];
    size_t i;
    if (board.active || !chd_path || !*chd_path || systemtaito_active() ||
        systemzn_active() || system10_active() || system11_active() || system12_active())
        return -1;
    memset(&board, 0, sizeof(board));
    board.bios = (uint8_t *)malloc(GV_BIOS_BYTES);
    board.eeprom = (uint8_t *)malloc(GV_EEPROM_BYTES);
    if (!board.bios || !board.eeprom) goto fail;
    memset(board.bios, 0, GV_BIOS_BYTES);
    memset(board.eeprom, 0xff, GV_EEPROM_BYTES);
    for (i = 0; i < sizeof(guest) / sizeof(guest[0]); i++)
        memcpy(board.bios + i * 4, &guest[i], 4);
    board.cd = chd_cd_open(chd_path, &cd_config, error, sizeof(error));
    if (!board.cd || chd_cd_get_media_ops(board.cd, &media) != 1) {
        fprintf(stderr, "SystemGV fixture: cannot open real CHD: %s\n",
                board.cd ? "media API error" : error);
        goto fail;
    }
    if (activate_device(&media)) goto fail;
    board.inputs[0] &= ~0x10u;
    return 0;
fail:
    systemgv_unload();
    return -1;
}

/* Explicit synthetic MIPS only; the original profile loader cannot accept it. */
int systemgv_test_load_simpbowl(const char *chd_path)
{
    static const uint32_t guest[]={
        0x3c08bf68u, 0x35080000u, 0x3c0a8000u, 0x354a0000u, 0x3c090000u, 0x3529091au,
        0xa5090082u, 0x3c090000u, 0x35290012u, 0xa5090084u, 0x3c090000u, 0x35290000u,
        0xa5090086u, 0x95090080u, 0x00000000u, 0xad490100u, 0x3c090000u, 0x352902aau,
        0xa5090082u, 0x3c090000u, 0x35290005u, 0xa5090084u, 0x3c090000u, 0x35290000u,
        0xa5090086u, 0x95090088u, 0x00000000u, 0x3c090000u, 0x3529aaaau, 0xa5090080u,
        0x3c090000u, 0x35290155u, 0xa5090082u, 0x3c090000u, 0x35290002u, 0xa5090084u,
        0x3c090000u, 0x35290000u, 0xa5090086u, 0x3c090000u, 0x35295555u, 0xa5090080u,
        0x3c090000u, 0x352902aau, 0xa5090082u, 0x3c090000u, 0x35290005u, 0xa5090084u,
        0x3c090000u, 0x35290000u, 0xa5090086u, 0x95090088u, 0x00000000u, 0x3c090000u,
        0x3529a0a0u, 0xa5090080u, 0x3c090000u, 0x3529091au, 0xa5090082u, 0x3c090000u,
        0x35290012u, 0xa5090084u, 0x3c090000u, 0x35290000u, 0xa5090086u, 0x3c090000u,
        0x35293412u, 0xa5090080u, 0x3c090000u, 0x3529091au, 0xa5090082u, 0x3c090000u,
        0x35290012u, 0xa5090084u, 0x3c090000u, 0x35290000u, 0xa5090086u, 0x95090080u,
        0x00000000u, 0xad490104u, 0x3c090000u, 0x352902aau, 0xa5090082u, 0x3c090000u,
        0x35290005u, 0xa5090084u, 0x3c090000u, 0x35290000u, 0xa5090086u, 0x95090088u,
        0x00000000u, 0x3c090000u, 0x3529aaaau, 0xa5090080u, 0x3c090000u, 0x35290155u,
        0xa5090082u, 0x3c090000u, 0x35290002u, 0xa5090084u, 0x3c090000u, 0x35290000u,
        0xa5090086u, 0x3c090000u, 0x35295555u, 0xa5090080u, 0x3c090000u, 0x352902aau,
        0xa5090082u, 0x3c090000u, 0x35290005u, 0xa5090084u, 0x3c090000u, 0x35290000u,
        0xa5090086u, 0x95090088u, 0x00000000u, 0x3c090000u, 0x35299090u, 0xa5090080u,
        0x3c090000u, 0x35290000u, 0xa5090082u, 0x3c090000u, 0x35290000u, 0xa5090084u,
        0x3c090000u, 0x35290000u, 0xa5090086u, 0x95090080u, 0x00000000u, 0xad490108u,
        0x95090080u, 0x00000000u, 0xad49010cu, 0x3c090000u, 0x35290000u, 0xa5090082u,
        0x3c090000u, 0x35290000u, 0xa5090084u, 0x3c090000u, 0x35290000u, 0xa5090086u,
        0x3c090000u, 0x3529f0f0u, 0xa5090080u, 0x3c090000u, 0x352902aau, 0xa5090082u,
        0x3c090000u, 0x35292005u, 0xa5090084u, 0x3c090000u, 0x35290040u, 0xa5090086u,
        0x95090088u, 0x00000000u, 0x3c090000u, 0x3529aaaau, 0xa5090080u, 0x3c090000u,
        0x35290155u, 0xa5090082u, 0x3c090000u, 0x35292002u, 0xa5090084u, 0x3c090000u,
        0x35290040u, 0xa5090086u, 0x3c090000u, 0x35295555u, 0xa5090080u, 0x3c090000u,
        0x352902aau, 0xa5090082u, 0x3c090000u, 0x35292005u, 0xa5090084u, 0x3c090000u,
        0x35290040u, 0xa5090086u, 0x95090088u, 0x00000000u, 0x3c090000u, 0x3529a0a0u,
        0xa5090080u, 0x3c090000u, 0x3529091au, 0xa5090082u, 0x3c090000u, 0x35292012u,
        0xa5090084u, 0x3c090000u, 0x35290040u, 0xa5090086u, 0x3c090000u, 0x35297856u,
        0xa5090080u, 0x3c090000u, 0x3529091au, 0xa5090082u, 0x3c090000u, 0x35292012u,
        0xa5090084u, 0x3c090000u, 0x35290040u, 0xa5090086u, 0x95090080u, 0x00000000u,
        0xad490124u, 0x3c0c0010u, 0x358c0000u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x8d0900c0u, 0x00000000u, 0xad490110u, 0x8d0900c4u, 0x00000000u, 0xad490114u,
        0x910900c9u, 0x00000000u, 0x8d0900c0u, 0x00000000u, 0xad490118u, 0x8d0900c4u,
        0x00000000u, 0xad49011cu, 0x3c095349u, 0x35294d50u, 0xad490120u, 0x1000ffffu,
        0x00000000u,
    };
    char directory[GV_MAX_PATH];const char *slash=strrchr(chd_path,'/');
    if(slash){size_t n=slash-chd_path;if(n>=sizeof(directory))return -1;memcpy(directory,chd_path,n);directory[n]=0;}
    else strcpy(directory,".");
    if(systemgv_test_load(chd_path))return -1;
    if(init_simp(directory,1)){systemgv_unload();return -1;}
    memset(board.bios,0,GV_BIOS_BYTES);
    for(unsigned i=0;i<sizeof(guest)/sizeof(guest[0]);i++){uint32_t w=SWAPu32(guest[i]);memcpy(board.bios+i*4,&w,4);}
    return 0;
}

/* Explicit generated MIPS fixture; never a substitute for game firmware. */
int systemgv_test_load_tokimeki(const char *chd_path){
 static const uint32_t guest[]={
        0x3c08bf80u, 0x35081000u, 0x3c090000u, 0x35290000u, 0xad090814u, 0x3c090300u,
        0x35290000u, 0xad090814u, 0x3c090500u, 0x35290000u, 0xad090814u, 0x3c0906c6u,
        0x35290260u, 0xad090814u, 0x3c090704u, 0x35292018u, 0xad090814u, 0x3c090800u,
        0x35290001u, 0xad090814u, 0x3c090200u, 0x352900ffu, 0xad090810u, 0x3c090000u,
        0x35290000u, 0xad090810u, 0x3c0900f0u, 0x35290140u, 0xad090810u, 0x3c08bf68u,
        0x35080000u, 0x3c0a8000u, 0x354a0000u, 0x3c0dbf10u, 0x35ad0000u, 0x3c0ebf18u,
        0x35ce0000u, 0x8da90004u, 0x00000000u, 0x31290400u, 0xad490100u, 0x3c090000u,
        0x35290010u, 0xadc90000u, 0x3c090000u, 0x35290000u, 0xadc90000u, 0x8da90004u,
        0x00000000u, 0x31290400u, 0xad490104u, 0x3c090000u, 0x35290010u, 0xadc90000u,
        0x3c090000u, 0x35290000u, 0xadc90000u, 0x8da90004u, 0x00000000u, 0x31290400u,
        0xad490108u, 0x3c090000u, 0x35290010u, 0xadc90000u, 0x3c090000u, 0x35290000u,
        0xadc90000u, 0x8da90004u, 0x00000000u, 0x31290400u, 0xad49010cu, 0x3c090000u,
        0x35290010u, 0xadc90000u, 0x3c090000u, 0x35290000u, 0xadc90000u, 0x8da90004u,
        0x00000000u, 0x31290400u, 0xad490110u, 0x3c090000u, 0x35290010u, 0xadc90000u,
        0x3c090000u, 0x35290000u, 0xadc90000u, 0x8da90004u, 0x00000000u, 0x31290400u,
        0xad490114u, 0x3c090000u, 0x35290010u, 0xadc90000u, 0x3c090000u, 0x35290000u,
        0xadc90000u, 0x8da90004u, 0x00000000u, 0x31290400u, 0xad490118u, 0x3c090000u,
        0x35290010u, 0xadc90000u, 0x3c090000u, 0x35290000u, 0xadc90000u, 0x8da90004u,
        0x00000000u, 0x31290400u, 0xad49011cu, 0x3c090000u, 0x35290010u, 0xadc90000u,
        0x3c090000u, 0x35290000u, 0xadc90000u, 0x8da90004u, 0x00000000u, 0x31290400u,
        0xad490120u, 0x3c090000u, 0x35290010u, 0xadc90000u, 0x3c090000u, 0x35290000u,
        0xadc90000u, 0x8da90004u, 0x00000000u, 0x31290400u, 0xad490124u, 0x3c090000u,
        0x35290010u, 0xadc90000u, 0x3c090000u, 0x35290000u, 0xadc90000u, 0x8da90004u,
        0x00000000u, 0x31290400u, 0xad490128u, 0x3c090000u, 0x35290010u, 0xadc90000u,
        0x3c090000u, 0x35290000u, 0xadc90000u, 0x8da90004u, 0x00000000u, 0x31290400u,
        0xad49012cu, 0x3c090000u, 0x35290010u, 0xadc90000u, 0x3c090000u, 0x35290000u,
        0xadc90000u, 0x8da90004u, 0x00000000u, 0x31290400u, 0xad490130u, 0x3c090000u,
        0x35290010u, 0xadc90000u, 0x3c090000u, 0x35290000u, 0xadc90000u, 0x8da90004u,
        0x00000000u, 0x31290400u, 0xad490134u, 0x3c090000u, 0x35290010u, 0xadc90000u,
        0x3c090000u, 0x35290000u, 0xadc90000u, 0x8da90004u, 0x00000000u, 0x31290400u,
        0xad490138u, 0x3c090000u, 0x35290010u, 0xadc90000u, 0x3c090000u, 0x35290000u,
        0xadc90000u, 0x8da90004u, 0x00000000u, 0x31290400u, 0xad49013cu, 0x3c090000u,
        0x35290010u, 0xadc90000u, 0x3c090000u, 0x35290000u, 0xadc90000u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c3200u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c3200u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c3200u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c3200u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c3200u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c3200u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c0c0004u,
        0x358c0000u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c3200u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c3200u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c3200u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u,
        0xa5090090u, 0x3c0c0000u, 0x358c0c80u, 0x258cffffu, 0x1580fffeu, 0x00000000u,
        0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u,
        0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u,
        0x35290000u, 0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u,
        0x358c1900u, 0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u,
        0xa5090090u, 0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u,
        0x258cffffu, 0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u,
        0x3c090000u, 0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu,
        0x1580fffeu, 0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x3c090000u,
        0x35290001u, 0xa5090090u, 0x3c0c0000u, 0x358c1900u, 0x258cffffu, 0x1580fffeu,
        0x00000000u, 0x3c090000u, 0x35290000u, 0xa5090090u, 0x95090080u, 0x00000000u,
        0x31290040u, 0xad490140u, 0x3c09544fu, 0x35294b50u, 0xad490144u, 0x1000ffffu,
        0x00000000u,
 };
 if(systemgv_test_load(chd_path))return -1;
 TokimekiIOConfig cfg={0};cfg.device_initial=0xf073;cfg.capture=capture_printer;cfg.printed=printed_page;
 board.tok=tokimeki_io_create(&cfg);if(!board.tok){systemgv_unload();return -1;}
 memset(board.bios,0,GV_BIOS_BYTES);
 for(unsigned i=0;i<sizeof(guest)/sizeof(guest[0]);i++){uint32_t w=SWAPu32(guest[i]);memcpy(board.bios+i*4,&w,4);}return 0;
}

void systemgv_unload(void)
{
#ifndef DRC_DISABLE
    if (board.active) new_dynarec_set_code_span_provider(NULL);
#endif
    if(board.tok)tokimeki_io_destroy(board.tok);
    if (board.simp) simpbowl_io_destroy(board.simp);
    free(board.flash);
    if (board.device) konamigv_destroy(board.device);
    for (unsigned i = 0; i < 2; ++i) psx_sio_destroy(board.sio[i]);
    if (board.cd) chd_cd_close(board.cd);
    free(board.bios);
    free(board.eeprom);
    if (board.active) {
        psxRamSize = board.old_ram_size;
        psxRomSize = board.old_rom_size;
    }
    memset(&board, 0, sizeof(board));
}

int systemgv_active(void) { return board.active; }

void systemgv_mem_reset(void)
{
    if (!board.active || !board.bios || !psxRegs.ptrs.psxM) return;
    memcpy(psxRegs.ptrs.psxR, board.bios, GV_BIOS_BYTES);
    if (!board.memory_initialized) memset(psxRegs.ptrs.psxM, 0, PSX_RAM_SIZE);
    memset(psxRegs.ptrs.psxP, 0xff, 0x10000);
    board.memory_initialized = 1;
}

void systemgv_reset(void)
{
    if (!board.active) return;
    board.fault = 0;
    board.last_cycle = psxRegs.cycle;
    board.cycle_fraction = 0;
    if (!konamigv_reset(board.device)) board.fault = 1;
    if(board.tok)tokimeki_io_reset(board.tok);
}

int systemgv_sync(uint32_t psx_cycle)
{
    uint32_t elapsed;
    uint64_t whole, partial, delta_ns;
    const uint32_t clock = PSXCLK;
    if (!board.active) return 0;
    if (psx_cycle < board.last_cycle && board.last_cycle - psx_cycle < 0x80000000u) {
        board.fault = 1;
        return -1;
    }
    elapsed = psx_cycle - board.last_cycle;
    board.last_cycle = psx_cycle;
    if (board.sio_cycles > UINT64_MAX - elapsed) { board.fault = 1; return -1; }
    board.sio_cycles += elapsed;
    for (unsigned i = 0; i < 2; ++i)
        if (psx_sio_advance_to_cycle(board.sio[i], board.sio_cycles)) {
            board.fault = 1; return -1;
        }
    whole = elapsed / clock;
    partial = elapsed % clock;
    if (whole > UINT64_MAX / 1000000000ULL) { board.fault = 1; return -1; }
    delta_ns = whole * 1000000000ULL;
    partial = partial * 1000000000ULL + board.cycle_fraction;
    if (partial / clock > UINT64_MAX - delta_ns) { board.fault = 1; return -1; }
    delta_ns += partial / clock;
    board.cycle_fraction = partial % clock;
    if (delta_ns > UINT64_MAX - board.now_ns) { board.fault = 1; return -1; }
    board.now_ns += delta_ns;
    if (sync_devices(board.now_ns)) return -1;
    return board.fault ? -1 : 0;
}

int systemgv_read(uint32_t address, unsigned width, uint32_t *value)
{
    int result;
    uint32_t physical = physical_address(address);
    if (!board.active || !value) return 0;
    if (width != 1 && width != 2 && width != 4) return 0;
    if (!overlaps(physical, width, 0x1f000000, 0x20) &&
        !overlaps(physical, width, 0x1f100000, 12) &&
        !overlaps(physical, width, 0x1f680000, 16) &&
        !(board.simp && (overlaps(physical,width,0x1f680080,16) || overlaps(physical,width,0x1f6800c0,10))) &&
        !(board.tok && overlaps(physical,width,0x1f680080,2)) &&
        !overlaps(physical, width, 0x1f801040, 32)) return 0;
    if (systemgv_sync(psxRegs.cycle)) { *value = UINT32_MAX; return -1; }
    result = sony_sio_bus(physical, width, value,
                          width == 4 ? UINT32_MAX : (1u << (width * 8)) - 1, 0);
    if (result) { board.mmio_reads++; return result; }
    result=tok_bus(physical,width,value,0,0);if(result){board.mmio_reads++;return result;}
    result = board.simp ? simpbowl_io_read(board.simp,physical,width,value) : 0;
    if(!result) result = konamigv_read(board.device, physical, width, value);
    if (result < 0) { board.fault = 1; *value = UINT32_MAX; return -1; }
    if (result) board.mmio_reads++;
    return result;
}

int systemgv_write(uint32_t address, unsigned width, uint32_t value,
                   uint32_t mem_mask)
{
    int result;
    uint32_t physical = physical_address(address);
    if (!board.active) return 0;
    if (width != 1 && width != 2 && width != 4) return 0;
    if (!overlaps(physical, width, 0x1f000000, 0x20) &&
        !overlaps(physical, width, 0x1f180000, 4) &&
        !overlaps(physical, width, 0x1f680000, 16) &&
        !(board.simp && overlaps(physical,width,0x1f680080,16)) &&
        !(board.tok && overlaps(physical,width,0x1f680090,2)) &&
        !overlaps(physical, width, 0x1f780000, 4) &&
        !overlaps(physical, width, 0x1f801040, 32)) return 0;
    if (systemgv_sync(psxRegs.cycle)) return -1;
    result = sony_sio_bus(physical, width, &value, mem_mask, 1);
    if (result) { board.mmio_writes++; return result; }
    if(board.tok && physical<=0x1f180000 && physical+width>0x1f180000){
      unsigned lane=0x1f180000-physical;if(mem_mask&(0xffu<<(lane*8)))tokimeki_io_device_line(board.tok,((value>>(lane*8))&0x10)!=0);
    }
    result=tok_bus(physical,width,&value,mem_mask,1);if(result){board.mmio_writes++;return result;}
    result = board.simp ? simpbowl_io_write(board.simp,physical,width,value,mem_mask) : 0;
    if(!result) result = konamigv_write(board.device, physical, width, value, mem_mask);
    if (result < 0) { board.fault = 1; return -1; }
    if (result) board.mmio_writes++;
    return result;
}

static int32_t dma_words(uint32_t bcr, uint32_t chcr)
{
    uint64_t words;
    if (chcr == 0x01000000u) words = bcr;
    else if ((chcr & 0xffbffeffu) == 0x11000000u) {
        uint64_t blocks = bcr >> 16;
        /* MAME's CD DMA callback uses max(BCR.high, 1) * BCR.low, which
         * differs from the other bit-9 block mode's zero => 65536 rule. */
        if (blocks < 1) blocks = 1;
        words = blocks * (bcr & 0xffffu);
    }
    else if (chcr == 0x01000200u || chcr == 0x01000201u) {
        uint64_t blocks = bcr >> 16;
        if (!blocks) blocks = 0x10000u;
        words = blocks * (bcr & 0xffffu);
    } else if (chcr == 0x11050100u || chcr == 0x11150100u) {
        /* MAME passes raw BCR for these two SCSI DMA write opcodes. */
        words = bcr;
    } else return -2;
    return words > INT32_MAX ? -1 : (int32_t)words;
}

int systemgv_dma5(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    uint32_t address;
    int32_t words;
    int result;
    if (!board.active || systemgv_sync(psxRegs.cycle)) return -1;
    /* The dispatch branch finishes 01000201 immediately for channel 5;
     * dma_finished then invokes its write delegate using block-mode BCR.
     * Returning before that payload leaves MODE SELECT waiting for data. */
    words = dma_words(bcr, chcr);
    if (words == -2) return 1; /* MAME leaves unrecognized control words alone. */
    if (words < 0) { board.fault = 1; return -1; }
    address = madr & PSX_RAM_MASK;
    if (getenv("PCSX_TRACE_GV_DMA"))
        fprintf(stderr,"GV DMA5 pc=%08x time=%" PRIu64 " madr=%08x bcr=%08x chcr=%08x words=%d\n",
            psxRegs.pc,board.now_ns,madr,bcr,chcr,words);
    if ((uint64_t)(uint32_t)words * 4 > PSX_RAM_SIZE - address) {
        board.fault = 1;
        return -1;
    }
    if (chcr == 0x01000000u || chcr == 0x01000200u ||
        (chcr & 0xffbffeffu) == 0x11000000u) {
        if ((chcr & 0xffbffeffu) == 0x11000000u)
            board.cd_dma_words = (uint32_t)words;
        result = konamigv_dma5_read(board.device, address, words);
    } else
        result = konamigv_dma5_write(board.device, address, words);
    if (!result) { board.fault = 1; return -1; }
    board.dma_requests++;
    return 0;
}

int systemgv_set_input(const char *name, int pressed)
{
    static const struct { const char *name; unsigned port; uint32_t mask; } binds[] = {
        {"p1left",0,0x0001},{"p1right",0,0x0002},{"p1up",0,0x0004},{"p1down",0,0x0008},
        {"p1button1",0,0x0010},{"p1button2",0,0x0020},{"p1button3",0,0x0040},
        {"p1start",0,0x0200},{"coin1",0,0x0400},{"service",0,0x0800},{"test",0,0x1000},
        {"p2left",1,0x0001},{"p2right",1,0x0002},{"p2up",1,0x0004},{"p2down",1,0x0008},
        {"p2button1",1,0x0010},{"p2button2",1,0x0020},{"p2button3",1,0x0040},
        {"p2start",1,0x0200},{"coin2",1,0x0400},
        {"p3left",2,0x0001},{"p3right",2,0x0002},{"p3up",2,0x0004},{"p3down",2,0x0008},
        {"p3button1",2,0x0010},{"p3button2",2,0x0020},{"p3button3",2,0x0040},{"p3start",2,0x0080},
        {"p4left",2,0x0100},{"p4right",2,0x0200},{"p4up",2,0x0400},{"p4down",2,0x0800},
        {"p4button1",2,0x1000},{"p4button2",2,0x2000},{"p4button3",2,0x4000},{"p4start",2,0x8000},
    };
    size_t i;
    if (!board.active || !name || (pressed != 0 && pressed != 1)) return -1;
    if(board.tok){
      if(!strcmp(name,"heartbeat_pulse")){tokimeki_io_pulse(board.tok,pressed);return 0;}
      if(!strcmp(name,"printer_empty") || !strcmp(name,"printer_fault")){
        unsigned bit=!strcmp(name,"printer_empty")?1:2;if(pressed)tok_meta&=~bit;else tok_meta|=bit;tokimeki_io_inputs(board.tok,tok_heart,tok_gsr,tok_meta);return 0;
      }
      if(strcmp(name,"coin1") && strcmp(name,"service") && strcmp(name,"test") &&
         strcmp(name,"p1left") && strcmp(name,"p1right") && strcmp(name,"p1up") && strcmp(name,"p1down") && strcmp(name,"p1button1"))return -1;
    }
    for (i = 0; i < sizeof(binds) / sizeof(binds[0]); i++) if (!strcmp(name, binds[i].name)) {
        if (pressed) board.inputs[binds[i].port] &= ~binds[i].mask;
        else board.inputs[binds[i].port] |= binds[i].mask;
        return 0;
    }
    return -1;
}

int systemgv_eeprom_import(const unsigned char *image, size_t size)
{
    return board.active ? konamigv_eeprom_import(board.device, image, size) : -1;
}
int systemgv_eeprom_export(unsigned char *image, size_t size)
{
    return board.active ? konamigv_eeprom_export(board.device, image, size) : -1;
}

int systemgv_selftest(void)
{
    uint32_t input, old_irq, value;
    uint8_t eeprom[GV_EEPROM_BYTES];
    unsigned failures = 0;
    if (!board.active || !psxRegs.ptrs.psxM) return -1;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "SystemGV selftest failed line %u: %s\n", __LINE__, #x); failures++; } } while (0)
    old_irq = psxHu32(0x1070);
    CHECK(PSX_RAM_SIZE == 0x200000 && PSX_ROM_SIZE == GV_BIOS_BYTES && !Config.HLE);
    CHECK(gpu_get_model() == 2);
    psxMemWrite32(&psxRegs, 0x1f801814, 0x10000007);
    CHECK(psxMemRead32(&psxRegs, 0x1f801810) == 2);
    CHECK(systemgv_eeprom_export(eeprom, sizeof(eeprom)) == 0);
    for (unsigned i = 0; i < sizeof(eeprom); ++i)
        CHECK(eeprom[i] == board.eeprom[i ^ 1u]);
    CHECK(psxMemRead32(&psxRegs, 0xbfc00000) == SWAPu32(*(uint32_t *)board.bios));
    CHECK(psxMemRead16(&psxRegs, 0x1f801054) == 5);
    psxMemWrite16(&psxRegs, 0x1f801058, 0x000d);
    CHECK(psxMemRead8(&psxRegs, 0x1f801058) == 0x0d);
    psxMemWrite16(&psxRegs, 0x1f80105a, 0x0040);
    CHECK(psxMemRead32(&psxRegs, 0x1f801058) == 0x0040000d);
    CHECK(psxMemRead16(&psxRegs, 0x1f801054) == 5);
    CHECK(systemgv_set_input("p1button1", 1) == 0);
    CHECK(systemgv_read(0x1f100000, 4, &input) == 1 && (input & 0x10) == 0);
    CHECK(systemgv_read(0x9f100000, 4, &value) == 1 && (value & 0x10) == 0);
    CHECK(systemgv_set_input("p1button1", 0) == 0);
    CHECK(systemgv_read(0xbf100000, 4, &input) == 1 && (input & 0x10) != 0);
    CHECK(systemgv_read(0x1f10000c, 4, &value) == 0);
    CHECK(dma_words(4, 0x11000000u) == 4);
    CHECK(dma_words(0x00030004u, 0x11000000u) == 12);
    CHECK(dma_words(0x00030004u, 0x11400100u) == 12);
    CHECK(dma_words(0x00000004u, 0x11000000u) == 4);
    CHECK(dma_words(0x00070001u, 0x01000201u) == 7);
    CHECK(dma_words(0x00000001u, 0x01000201u) == 65536);
    psxHu32ref(0x1070) = SWAPu32(old_irq);
    fprintf(stderr, "SystemGV selftest: %s\n", failures ? "FAIL" : "PASS");
#undef CHECK
    return failures ? -1 : 0;
}

/* Run after the synthetic guest checks: this deliberately changes the test
 * target's block size and must never touch an authenticated game's CD state. */
int systemgv_test_dma_write(void)
{
    static const uint8_t cdb[] = {0x15, 0, 0, 0, 12, 0};
    static const uint8_t parameters[] = {0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 2, 0};
    konamigv_status_t status;
    int passed;
    if (!board.active || board.profile || !psxRegs.ptrs.psxM) return -1;
    systemgv_reset();
    for (unsigned i = 0; i < sizeof(cdb); ++i)
        psxMemWrite8(&psxRegs, 0x1f000004, cdb[i]);
    psxMemWrite8(&psxRegs, 0x1f000010, 7); /* initiator */
    psxMemWrite8(&psxRegs, 0x1f000008, 4); /* target */
    psxMemWrite8(&psxRegs, 0x1f000006, 0x41); /* select with CDB */
    psxRegs.cycle += 7000; /* allow selection and command phase */
    if (systemgv_sync(psxRegs.cycle)) return -1;
    (void)psxMemRead8(&psxRegs, 0x1f00000a); /* acknowledge NCR IRQ */
    memcpy(psxRegs.ptrs.psxM + 0x1000, parameters, sizeof(parameters));
    psxMemWrite8(&psxRegs, 0x1f000016, 0x08);
    psxMemWrite8(&psxRegs, 0x1f000000, sizeof(parameters));
    psxMemWrite8(&psxRegs, 0x1f000002, 0);
    psxMemWrite8(&psxRegs, 0x1f00001c, 0);
    psxMemWrite32(&psxRegs, 0x1f8010d0, 0x1000);
    psxMemWrite32(&psxRegs, 0x1f8010d4, 0x00030001); /* three one-word blocks */
    psxMemWrite32(&psxRegs, 0x1f8010d8, 0x01000201);
    psxMemWrite8(&psxRegs, 0x1f000006, 0x90); /* DMA data-out */
    psxRegs.cycle += 140000;
    if (systemgv_sync(psxRegs.cycle)) return -1;
    konamigv_get_status(board.device, &status);
    passed = !board.fault && !status.faults && !status.dma_active &&
        !status.dma_bytes_remaining && status.cdrom.block_size == 512 &&
        !(psxHu32(0x10d8) & 0x01000000u);
    fprintf(stderr, "SystemGV DMA write fixture: %s CHCR=01000201 block_size=%u remaining=%" PRIu64 " faults=%08x/%08x\n",
        passed ? "PASS" : "FAIL", status.cdrom.block_size,
        status.dma_bytes_remaining, board.fault, status.faults);
    return passed ? 0 : -1;
}

uint64_t systemgv_dma_pending_bytes(void)
{
    konamigv_status_t status;
    if (!board.active || !board.device) return 0;
    konamigv_get_status(board.device, &status);
    return status.dma_bytes_remaining;
}

void systemgv_report(FILE *out)
{
    konamigv_status_t status;
    if (!board.active || !out) return;
    konamigv_get_status(board.device, &status);
    if(board.tok){TokimekiIOStatus st;tokimeki_io_status(board.tok,&st);
      fprintf(out,"Tokimeki IO: device=%04x sensor=%02x data=%04x heartbeat_ticks=%" PRIu64 " packets=%" PRIu64 " rejected=%" PRIu64 " captures=%" PRIu64 " prints=%" PRIu64 " unknown=%" PRIu64 " faults=%u\n",st.device,st.sensor_id,st.sensor_data,st.heartbeat_ticks,st.printer_packets,st.rejected_packets,st.captures,st.prints,st.unknown_commands,st.fault);
    }
    if(board.simp){SimpBowlIOStatus st;simpbowl_io_status(board.simp,&st);
      fprintf(out,"Simpsons IO: flash_address=%08x reads=%" PRIu64 " writes=%" PRIu64 " frames=%" PRIu64 " reset_reads=%" PRIu64 " track=%03x/%03x faults=%u warm=%d\n",st.address,st.reads,st.writes,st.frames,st.reset_reads,st.x,st.y,st.fault,board.flash_warm);
    }
    fprintf(out, "SystemGV: profile=%s CPU=CXD8530BQ source_clock=67737600 source_clocks_per_mame_cycle=4 PSXCLK=%u GPU=CXD8514Q VRAM=1048576 RAM=%u\n",
            board.profile ? board.profile->set : "synthetic-fixture", PSXCLK, PSX_RAM_SIZE);
    fprintf(out, "SystemGV: NCR=%" PRIu64 "Hz IRQ10=%u DRQ=%u DMA=%" PRIu64
            " requests remaining=%" PRIu64 " bytes EEPROM_busy=%u faults=%08x/%08x\n",
            UINT64_C(16000000), !!(psxHu32(0x1070) & (1u << 10)), status.drq,
            board.dma_requests, status.dma_bytes_remaining, status.eeprom_busy,
            status.faults, board.fault);
    fprintf(out, "SystemGV: CD_DMA_words=%u\n", board.cd_dma_words);
    fprintf(out,"SystemGV: NCR command=%02x status=%02x int=%02x seq=%02x fifo=%u phase=%u count=%u/%u\n",
        status.ncr.command,status.ncr.status,status.ncr.interrupt_status,status.ncr.sequence,
        status.ncr.fifo_count,status.ncr.phase,status.ncr.transfer_count,status.ncr.transfer_counter);
    fprintf(out, "SystemGV: IRQ=%04x/%04x SR=%08x Cause=%08x DMA2=%08x/%08x\n",
        psxHu16(0x1070), psxHu16(0x1074), psxRegs.CP0.n.SR,
        psxRegs.CP0.n.Cause, psxHu32(0x10a0), psxHu32(0x10a8));
    fprintf(out, "SystemGV: MMIO reads=%" PRIu64 " writes=%" PRIu64
            " cycles_ns=%" PRIu64 " CD tracks=%u last_lba=%u CHD_status=%s\n",
            board.mmio_reads, board.mmio_writes, board.now_ns,
            chd_cd_track_count(board.cd), chd_cd_last_lba(board.cd),
            board.profile ? board.profile->disk_status : "TEST_FIXTURE");
}
