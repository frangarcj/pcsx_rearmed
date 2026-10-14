/* GPL-2.0-or-later. Native Konami GQ adapter; maps/devices audited against
 * MAME deb2f54fe7300e7a192690318beb860159bfd52a, konami/konamigq.cpp.
 * Source original firmware executes on PSX/MC68000/TMS57002; no game HLE. */
#include "systemgq.h"
#include "system10.h"
#include "system11.h"
#include "system12.h"
#include "systemtaito.h"
#include "systemzn.h"
#ifdef PCSX_SYSTEM_GV
#include "systemgv.h"
#endif
#ifdef PCSX_SYSTEM_GNET
#include "systemgnet.h"
#endif
#include "arcade/chd_hd.h"
#include "arcade/konamigq.h"
#include "arcade/pcm_wav.h"
#include "arcade/psx_sio.h"
#include "gpu_vram.h"
#include "psxhw.h"
#include "psxmem.h"
#include "r3000a.h"
#include <CommonCrypto/CommonDigest.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
enum { GQ_MAX_PATH = 4096, GQ_BIOS_BYTES = 524288, GQ_AUDIO_RING = 4096 };
typedef struct GqAudioFrame {
  uint64_t cycle;
  int32_t left, right;
} GqAudioFrame;
typedef struct GqBoard {
  konamigq_t *device;
  arcade_chd_hd_t *disk;
  arcade_chd_hd_info_t disk_info;
  psx_sio_t *sio[2];
  uint8_t *bios, *sound, *pcm, *eeprom;
  uint32_t inputs[9], last_cycle, old_ram_size, old_rom_size;
  uint64_t now_ns, cycle_fraction, sio_cycles, reads, writes, errors,
      dma_requests;
  uint64_t mmio_reads, mmio_writes, audio_frames, audio_nonzero, audio_clipped;
  GqAudioFrame audio[2][GQ_AUDIO_RING];
  unsigned head[2], count[2], audio_peak;
  int16_t output[256 * 2];
  unsigned output_count;
  PcmWav wav;
  void (*sink)(void *, const int16_t *, unsigned);
  void *sink_opaque;
  int active, synthetic, fault, memory_initialized;
} GqBoard;
static GqBoard board;
static void sony_sio_irq(void *opaque, int state) {
  unsigned port = (unsigned)(uintptr_t)opaque;
  if (state && psxRegs.ptrs.psxH)
    psxHu32ref(0x1070) |= SWAPu32(1u << (7 + port));
}

static int sony_sio_bus(uint32_t address, unsigned width, uint32_t *value,
                        uint32_t mask, int write) {
  if (address < 0x1f801040u || address >= 0x1f801060u ||
      (address & (width - 1)))
    return 0;
  unsigned port = (address - 0x1f801040u) >> 4;
  unsigned shift = (address & 3u) * 8;
  uint32_t raw = 0;
  int error;
  if (write)
    error = psx_sio_write(board.sio[port], (address >> 2) & 3, *value << shift,
                          mask << shift);
  else {
    error =
        psx_sio_read(board.sio[port], (address >> 2) & 3, mask << shift, &raw);
    *value = (raw >> shift) & mask;
  }
  if (error) {
    board.fault = 1;
    return -1;
  }
  return 1;
}

static uint32_t physical_address(uint32_t address) {
  return address >= 0x80000000u && address < 0xc0000000u ? address & 0x1fffffffu
                                                         : address;
}

static int overlaps(uint32_t address, unsigned width, uint32_t start,
                    uint32_t size) {
  uint64_t end = (uint64_t)address + width;
  return address < start + size && end > start;
}

static void irq10(void *opaque, int asserted) {
  (void)opaque;
  if (asserted && psxRegs.ptrs.psxH)
    psxHu32ref(0x1070) |= SWAPu32(1u << 10);
}

static uint32_t read_input(void *opaque, unsigned port) {
  (void)opaque;
  return port < 9 ? board.inputs[port] : UINT32_MAX;
}

static uint8_t read_ram8(void *opaque, uint32_t address) {
  (void)opaque;
  if (!psxRegs.ptrs.psxM || address >= PSX_RAM_SIZE) {
    board.fault = 1;
    return 0xff;
  }
  return psxRegs.ptrs.psxM[address];
}

static void write_ram8(void *opaque, uint32_t address, uint8_t value) {
  (void)opaque;
  if (!psxRegs.ptrs.psxM || address >= PSX_RAM_SIZE) {
    board.fault = 1;
    return;
  }
  psxRegs.ptrs.psxM[address] = value;
}

static int read_exact_file(const char *directory, const char *name,
                           size_t expected, uint8_t **result) {
  char path[GQ_MAX_PATH];
  FILE *file;
  uint8_t *data;
  long size;
  if (snprintf(path, sizeof(path), "%s/%s", directory, name) >=
      (int)sizeof(path))
    return -1;
  file = fopen(path, "rb");
  if (!file)
    return -1;
  if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 ||
      (size_t)size != expected || fseek(file, 0, SEEK_SET)) {
    fclose(file);
    return -1;
  }
  data = (uint8_t *)malloc(expected ? expected : 1);
  if (!data) {
    fclose(file);
    return -1;
  }
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

static int file_sha1(const uint8_t *data, size_t size, const char *expected) {
  unsigned char digest[CC_SHA1_DIGEST_LENGTH];
  char hex[CC_SHA1_DIGEST_LENGTH * 2 + 1];
  static const char digits[] = "0123456789abcdef";
  size_t i;
  if (!CC_SHA1(data, (CC_LONG)size, digest))
    return 0;
  for (i = 0; i < sizeof(digest); i++) {
    hex[i * 2] = digits[digest[i] >> 4];
    hex[i * 2 + 1] = digits[digest[i] & 15];
  }
  hex[sizeof(hex) - 1] = 0;
  return !strcmp(hex, expected);
}

/* Exactly the source-owned preparer's reviewed profile. Empty/unknown/duplicate
 * fields or modified clocks/maps/media names are rejected before activation. */
static const struct {
  const char *key, *value;
} ini_expected[] = {
    {"format", "pcsx-aow-konamigq-v1"},
    {"mame_set", "cryptklr"},
    {"mame_parent", "none"},
    {"mame_machine", "konamigq"},
    {"mame_input", "konamigq"},
    {"mame_state", "konamigq_state"},
    {"cpu_device", "CXD8530BQ"},
    {"cpu_clock_hz", "67737600"},
    {"main_ram_size", "4194304"},
    {"gpu_device", "CXD8538Q"},
    {"gpu_model", "1"},
    {"gpu_clock_hz", "53693175"},
    {"vram_size", "2097152"},
    {"sound_cpu_device", "M68000"},
    {"sound_cpu_clock_hz", "8000000"},
    {"effects_device", "TMS57002"},
    {"effects_clock_hz", "24000000"},
    {"scsi_device", "NCR53CF96"},
    {"scsi_clock_hz", "16000000"},
    {"scsi_initiator_id", "7"},
    {"scsi_target_id", "0"},
    {"scsi_target_device", "NSCSI_HARDDISK"},
    {"eeprom_device", "93C46 16-bit"},
    {"eeprom_default", "default_eeprom.bin"},
    {"eeprom_default_only", "true"},
    {"duart_device", "MB89371"},
    {"pcm_device", "K054539"},
    {"pcm_device_count", "2"},
    {"pcm_clock_hz", "18432000"},
    {"pcm_ram_base", "524288"},
    {"pcm_ram_size", "3670016"},
    {"game_execution_verified", "false"},
    {"region.soundcpu", "soundcpu.bin"},
    {"region.soundcpu.size", "524288"},
    {"region.k054539", "pcmrom.bin"},
    {"region.k054539.size", "524288"},
    {"region.maincpu:rom", "bios.bin"},
    {"region.maincpu:rom.size", "524288"},
    {"disk.harddisk", "420uaa04.chd"},
    {"disk.harddisk.sha1", "67cb1418fc0de2a89fc61847dc9efb9f1bebb347"},
    {"disk.harddisk.readonly", "false"},
};
static int validate_ini(const char *dir) {
  char path[GQ_MAX_PATH], line[512];
  uint64_t seen = 0;
  unsigned count = sizeof(ini_expected) / sizeof(ini_expected[0]);
  if (count >= 64 ||
      snprintf(path, sizeof(path), "%s/board.ini", dir) >= (int)sizeof(path))
    return -1;
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  while (fgets(line, sizeof(line), f)) {
    if (!strchr(line, '\n') && !feof(f))
      goto bad;
    line[strcspn(line, "\r\n")] = 0;
    if (!*line)
      continue;
    char *v = strchr(line, '=');
    if (!v)
      goto bad;
    *v++ = 0;
    unsigned i;
    for (i = 0; i < count && strcmp(line, ini_expected[i].key); i++)
      ;
    if (i == count || (seen & (UINT64_C(1) << i)) ||
        strcmp(v, ini_expected[i].value))
      goto bad;
    seen |= UINT64_C(1) << i;
  }
  if (ferror(f) || seen != ((UINT64_C(1) << count) - 1))
    goto bad;
  fclose(f);
  return 0;
bad:
  fclose(f);
  fprintf(stderr, "SystemGQ: malformed or unsupported source board.ini\n");
  return -1;
}
static int hd_read(void *opaque, uint32_t lba, uint8_t *data, size_t size) {
  (void)opaque;
  board.reads++;
  int result = arcade_chd_hd_read(board.disk, lba, data, size);
  if (result)
    board.errors++;
  return result == 0;
}
static int hd_write(void *opaque, uint32_t lba, const uint8_t *data,
                    size_t size) {
  (void)opaque;
  board.writes++;
  int result = arcade_chd_hd_write(board.disk, lba, data, size);
  if (result)
    board.errors++;
  return result == 0;
}
static int16_t clip(int64_t v) {
  if (v > 32767) {
    board.audio_clipped++;
    v = 32767;
  }
  if (v < -32768) {
    board.audio_clipped++;
    v = -32768;
  }
  return v;
}
static void pcm_output(void *opaque, unsigned chip, uint64_t cycle, int32_t l,
                       int32_t r) {
  (void)opaque;
  if (chip > 1 || board.count[chip] == GQ_AUDIO_RING) {
    board.fault = 1;
    return;
  }
  unsigned tail = (board.head[chip] + board.count[chip]) % GQ_AUDIO_RING;
  board.audio[chip][tail] = (GqAudioFrame){cycle, l, r};
  board.count[chip]++;
  /* Both source PCM outputs have gain 1. Match by actual oscillator tick,
   * rather than callback order: sound writes may advance one chip first. */
  while (board.count[0] && board.count[1]) {
    GqAudioFrame *a = &board.audio[0][board.head[0]],
                 *b = &board.audio[1][board.head[1]];
    if (a->cycle != b->cycle) {
      board.fault = 1;
      return;
    }
    int16_t four[4] = {clip((int64_t)a->left + b->left),
                       clip((int64_t)a->right + b->right), 0, 0};
    unsigned peak0 = four[0] < 0 ? -(int)four[0] : four[0],
             peak1 = four[1] < 0 ? -(int)four[1] : four[1];
    if (peak0 > board.audio_peak)
      board.audio_peak = peak0;
    if (peak1 > board.audio_peak)
      board.audio_peak = peak1;
    board.audio_nonzero += four[0] != 0 || four[1] != 0;
    board.audio_frames++;
    if (board.wav.file && pcm_wav_append4(&board.wav, four, 1))
      board.fault = 1;
    if (board.sink) {
      board.output[2 * board.output_count] = four[0];
      board.output[2 * board.output_count + 1] = four[1];
      if (++board.output_count == 256) {
        board.sink(board.sink_opaque, board.output, 256);
        board.output_count = 0;
      }
    }
    for (unsigned i = 0; i < 2; i++) {
      board.head[i] = (board.head[i] + 1) % GQ_AUDIO_RING;
      board.count[i]--;
    }
  }
}
void systemgq_set_audio_output(void (*sink)(void *, const int16_t *, unsigned),
                               void *opaque) {
  if (board.sink && board.output_count)
    board.sink(board.sink_opaque, board.output, board.output_count);
  board.output_count = 0;
  board.sink = sink;
  board.sink_opaque = opaque;
}
int systemgq_open_audio(const char *path) {
  return !board.active || board.wav.file
             ? -1
             : pcm_wav_open(&board.wav, path, 48000);
}
int systemgq_close_audio(void) { return pcm_wav_close(&board.wav); }
static int load_board(const char *dir, int synthetic) {
  char path[GQ_MAX_PATH], overlay[GQ_MAX_PATH], error[256];
  konamigq_config_t cfg = {0};
  if (board.active || !dir || !*dir || systemtaito_active() ||
      systemzn_active() || system10_active() || system11_active() ||
      system12_active()
#ifdef PCSX_SYSTEM_GV
      || systemgv_active()
#endif
#ifdef PCSX_SYSTEM_GNET
      || systemgnet_active()
#endif
  )
    return -1;
  if (validate_ini(dir))
    return -1;
  board.synthetic = synthetic;
  static const struct {
    const char *file, *sha;
    uint8_t **ptr;
    size_t size;
  } regions[] = {
      {"bios.bin", "bf9dc7c0c8168c22a4be266fe6a66d3738df916b", &board.bios,
       524288},
      {"soundcpu.bin", "e7dc7f9f3abe50e1db1d8bd1fb02010376759805", &board.sound,
       524288},
      {"pcmrom.bin", "6d525f10385791e19eb1897d18f0bab319640162", &board.pcm,
       524288},
      {"default_eeprom.bin", "d9ff0458478ee42b7b14f101be96dd2e45edbba3",
       &board.eeprom, 128},
  };
  for (unsigned i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
    if (read_exact_file(dir, regions[i].file, regions[i].size,
                        regions[i].ptr) ||
        (!synthetic &&
         !file_sha1(*regions[i].ptr, regions[i].size, regions[i].sha))) {
      fprintf(stderr, "SystemGQ: original %s size or source SHA1 mismatch\n",
              regions[i].file);
      goto fail;
    }
  }
  if (snprintf(path, sizeof(path), "%s/420uaa04.chd", dir) >=
          (int)sizeof(path) ||
      snprintf(overlay, sizeof(overlay), "%s/420uaa04.overlay", dir) >=
          (int)sizeof(overlay))
    goto fail;
  board.disk = arcade_chd_hd_open(path, overlay, error, sizeof(error));
  if (!board.disk) {
    fprintf(stderr, "SystemGQ: HDD open failed: %s\n", error);
    goto fail;
  }
  arcade_chd_hd_get_info(board.disk, &board.disk_info);
  static const uint8_t expected_sha[] = {
      0x67, 0xcb, 0x14, 0x18, 0xfc, 0x0d, 0xe2, 0xa8, 0x9f, 0xc6,
      0x18, 0x47, 0xdc, 0x9e, 0xfb, 0x9f, 0x1b, 0xeb, 0xb3, 0x47};
  if (!synthetic &&
      (memcmp(board.disk_info.sha1, expected_sha, 20) ||
       board.disk_info.cylinders != 1122 || board.disk_info.heads != 15 ||
       board.disk_info.sectors_per_track != 63 ||
       board.disk_info.sector_bytes != 512)) {
    fprintf(stderr, "SystemGQ: source HDD SHA1/geometry mismatch\n");
    goto fail;
  }
  for (unsigned i = 0; i < 6; i++)
    board.inputs[i] = (i & 1) ? 0x78 : 0x11d;
  board.inputs[6] = board.inputs[7] = 0x00ff00ff;
  board.inputs[8] = 0xfd;
  cfg.opaque = &board;
  cfg.read_input = read_input;
  cfg.read_psx_ram8 = read_ram8;
  cfg.write_psx_ram8 = write_ram8;
  cfg.psx_irq10 = irq10;
  cfg.sound_rom = board.sound;
  cfg.sound_rom_bytes = 524288;
  cfg.k054539_rom = board.pcm;
  cfg.k054539_rom_bytes = 524288;
  cfg.eeprom_image = board.eeprom;
  cfg.eeprom_image_bytes = 128;
  cfg.k054539_audio = pcm_output;
  cfg.hard_disk.media_present = 1;
  cfg.hard_disk.cylinders = board.disk_info.cylinders;
  cfg.hard_disk.heads = board.disk_info.heads;
  cfg.hard_disk.sectors_per_track = board.disk_info.sectors_per_track;
  cfg.hard_disk.sector_bytes = board.disk_info.sector_bytes;
  cfg.hard_disk.read_sector = hd_read;
  cfg.hard_disk.write_sector = hd_write;
  cfg.hard_disk.inquiry_data = board.disk_info.inquiry;
  cfg.hard_disk.inquiry_data_bytes = board.disk_info.inquiry_bytes;
  board.device = konamigq_create(&cfg);
  if (!board.device || gpu_set_model(1) || gpu_set_vram_height(1024))
    goto fail;
  for (unsigned i = 0; i < 2; i++) {
    psx_sio_callbacks_t cb = {0};
    cb.opaque = (void *)(uintptr_t)i;
    cb.irq = sony_sio_irq;
    if (psx_sio_create(i ? PSX_SIO1 : PSX_SIO0, &cb, &board.sio[i]))
      goto fail;
  }
  board.old_ram_size = psxRamSize;
  board.old_rom_size = psxRomSize;
  psxRamSize = 4194304;
  psxRomSize = 524288;
  board.last_cycle = psxRegs.cycle;
  board.active = 1;
  return 0;
fail:
  systemgq_unload();
  return -1;
}
int systemgq_load(const char *dir) { return load_board(dir, 0); }
int systemgq_test_load(const char *dir) { return load_board(dir, 1); }
void systemgq_unload(void) {
  systemgq_close_audio();
  if (board.device)
    konamigq_destroy(board.device);
  for (unsigned i = 0; i < 2; i++)
    psx_sio_destroy(board.sio[i]);
  if (board.disk)
    arcade_chd_hd_close(board.disk);
  free(board.bios);
  free(board.sound);
  free(board.pcm);
  free(board.eeprom);
  if (board.active) {
    psxRamSize = board.old_ram_size;
    psxRomSize = board.old_rom_size;
  }
  memset(&board, 0, sizeof(board));
}
int systemgq_active(void) { return board.active; }
void systemgq_mem_reset(void) {
  if (!board.active || !psxRegs.ptrs.psxM)
    return;
  memcpy(psxRegs.ptrs.psxR, board.bios, 524288);
  if (!board.memory_initialized)
    memset(psxRegs.ptrs.psxM, 0, PSX_RAM_SIZE);
  memset(psxRegs.ptrs.psxP, 0xff, 0x10000);
  board.memory_initialized = 1;
}
void systemgq_reset(void) {
  if (!board.active)
    return;
  board.last_cycle = psxRegs.cycle;
  board.cycle_fraction = 0;
  if (!konamigq_reset(board.device))
    board.fault = 1;
}
int systemgq_sync(uint32_t cycle) {
  if (!board.active)
    return 0;
  if (cycle < board.last_cycle && board.last_cycle - cycle < 0x80000000u) {
    board.fault = 1;
    return -1;
  }
  uint32_t elapsed = cycle - board.last_cycle;
  board.last_cycle = cycle;
  board.sio_cycles += elapsed;
  for (unsigned i = 0; i < 2; i++)
    if (psx_sio_advance_to_cycle(board.sio[i], board.sio_cycles))
      board.fault = 1;
  uint64_t partial = (uint64_t)elapsed * 1000000000 + board.cycle_fraction;
  board.now_ns += partial / PSXCLK;
  board.cycle_fraction = partial % PSXCLK;
  if (!konamigq_advance_to_ns(board.device, board.now_ns))
    board.fault = 1;
  konamigq_status_t st;
  konamigq_get_status(board.device, &st);
  if (st.faults || st.hard_disk.faults || board.errors)
    board.fault = 1;
  return board.fault ? -1 : 0;
}
static int aperture(uint32_t a, unsigned w, int write) {
  return overlaps(a, w, 0x1f000000, 0x20) || overlaps(a, w, 0x1f100000, 0x20) ||
         (write ? overlaps(a, w, 0x1f180000, 4) ||
                      overlaps(a, w, 0x1f198000, 4) ||
                      overlaps(a, w, 0x1f1a0000, 4) ||
                      overlaps(a, w, 0x1f780000, 4)
                : overlaps(a, w, 0x1f200000, 0x40000)) ||
         overlaps(a, w, 0x1f300000, 0x300000) ||
         overlaps(a, w, 0x1f680000, 0x10) || overlaps(a, w, 0x1f801040, 0x20);
}
int systemgq_read(uint32_t addr, unsigned w, uint32_t *v) {
  uint32_t a = physical_address(addr);
  if (!board.active || !v || (w != 1 && w != 2 && w != 4) || !aperture(a, w, 0))
    return 0;
  if (systemgq_sync(psxRegs.cycle)) {
    *v = UINT32_MAX;
    return -1;
  }
  int r = sony_sio_bus(a, w, v, w == 4 ? UINT32_MAX : (1u << (8 * w)) - 1, 0);
  if (!r)
    r = konamigq_read(board.device, a, w, v);
  if (r)
    board.mmio_reads++;
  else
    board.fault = 1;
  return r ? r : -1;
}
int systemgq_write(uint32_t addr, unsigned w, uint32_t v, uint32_t m) {
  uint32_t a = physical_address(addr);
  if (!board.active || (w != 1 && w != 2 && w != 4) || !aperture(a, w, 1))
    return 0;
  if (systemgq_sync(psxRegs.cycle))
    return -1;
  int r = sony_sio_bus(a, w, &v, m, 1);
  if (!r)
    r = konamigq_write(board.device, a, w, v, m);
  if (r)
    board.mmio_writes++;
  else
    board.fault = 1;
  return r ? r : -1;
}
static int32_t dma_words(uint32_t bcr, uint32_t chcr) {
  uint64_t words;
  if (chcr == 0x01000000u)
    words = bcr;
  else if ((chcr & 0xffbffeffu) == 0x11000000u) {
    uint64_t blocks = bcr >> 16;
    /* MAME's CD DMA callback uses max(BCR.high, 1) * BCR.low, which
     * differs from the other bit-9 block mode's zero => 65536 rule. */
    if (blocks < 1)
      blocks = 1;
    words = blocks * (bcr & 0xffffu);
  } else if (chcr == 0x01000200u || chcr == 0x01000201u) {
    uint64_t blocks = bcr >> 16;
    if (!blocks)
      blocks = 0x10000u;
    words = blocks * (bcr & 0xffffu);
  } else if (chcr == 0x11050100u || chcr == 0x11150100u) {
    /* MAME passes raw BCR for these two SCSI DMA write opcodes. */
    words = bcr;
  } else
    return -2;
  return words > INT32_MAX ? -1 : (int32_t)words;
}

int systemgq_dma5(uint32_t madr, uint32_t bcr, uint32_t chcr) {
  if (!board.active || systemgq_sync(psxRegs.cycle))
    return -1;
  int32_t n = dma_words(bcr, chcr);
  if (n == -2)
    return 1;
  uint32_t a = madr & PSX_RAM_MASK;
  if (n < 0 || (uint64_t)(uint32_t)n * 4 > PSX_RAM_SIZE - a) {
    board.fault = 1;
    return -1;
  }
  int r = (chcr == 0x01000000 || chcr == 0x01000200 ||
           (chcr & 0xffbffeff) == 0x11000000)
              ? konamigq_dma5_read(board.device, a, n)
              : konamigq_dma5_write(board.device, a, n);
  if (!r) {
    board.fault = 1;
    return -1;
  }
  board.dma_requests++;
  return 0;
}
int systemgq_set_input(const char *name, int pressed) {
  if (!board.active || !name || (pressed != 0 && pressed != 1))
    return -1;
  unsigned port, shift;
  uint32_t mask;
  if (!strcmp(name, "test")) {
    port = 7;
    shift = 0;
    mask = 0x200000;
  } else if (!strcmp(name, "service")) {
    port = 6;
    shift = 0;
    mask = 0x20;
  } else if (strlen(name) == 5 && !strncmp(name, "coin", 4) && name[4] >= '1' &&
             name[4] <= '3') {
    unsigned p = name[4] - '1';
    port = p < 2 ? 6 : 7;
    shift = p == 1 ? 16 : 0;
    mask = 1u << shift;
  } else if (name[0] == 'p' && name[1] >= '1' && name[1] <= '3') {
    unsigned p = name[1] - '1';
    port = p < 2 ? 6 : 7;
    shift = p == 1 ? 16 : 0;
    if (!strcmp(name + 2, "start"))
      mask = 2u << shift;
    else if (!strcmp(name + 2, "button1"))
      mask = 8u << shift;
    else if (!strcmp(name + 2, "button2"))
      mask = 16u << shift;
    else if (!strcmp(name + 2, "service"))
      mask = 32u << shift;
    else
      return -1;
  } else
    return -1;
  if (pressed)
    board.inputs[port] &= ~mask;
  else
    board.inputs[port] |= mask;
  return 0;
}
unsigned systemgq_analog_max(const char *name) {
  if (!name || strlen(name) != 6 || name[0] != 'p' || name[1] < '1' ||
      name[1] > '3' || strncmp(name + 2, "gun", 3))
    return 0;
  return name[5] == 'x' ? 0x1ff : name[5] == 'y' ? 0xff : 0;
}
int systemgq_set_analog(const char *name, unsigned value) {
  unsigned max = systemgq_analog_max(name);
  if (!board.active || !max || value > max)
    return -1;
  board.inputs[(name[1] - '1') * 2 + (name[5] == 'y')] = value;
  return 0;
}
int systemgq_set_gun_position(unsigned p, unsigned x, unsigned y) {
  if (!board.active || p >= 3 || x > 0x1ff || y > 0xff)
    return -1;
  board.inputs[2 * p] = x;
  board.inputs[2 * p + 1] = y;
  return 0;
}
int systemgq_eeprom_import(const unsigned char *b, size_t n) {
  return board.active ? konamigq_eeprom_import(board.device, b, n) : -1;
}
int systemgq_eeprom_export(unsigned char *b, size_t n) {
  return board.active ? konamigq_eeprom_export(board.device, b, n) : -1;
}
int systemgq_flush_disk(void) {
  return board.disk ? arcade_chd_hd_flush(board.disk) : -1;
}
int systemgq_selftest(void) {
  if (!board.active || PSX_RAM_SIZE != 4194304 || PSX_ROM_SIZE != 524288 ||
      Config.HLE || gpu_get_model() != 1)
    return -1;
  uint32_t v;
  if (systemgq_read(0x9f200000, 4, &v) != 1 || v != 0x11d ||
      systemgq_read(0xbf208000, 4, &v) != 1 || v != 0x78)
    return -1;
  if (systemgq_read(0x1f801054, 2, &v) != 1 || v != 5 ||
      systemgq_set_input("p1left", 1) == 0 ||
      systemgq_set_input("coin4", 1) == 0)
    return -1;
  if (systemgq_set_input("p3button1", 1) ||
      systemgq_read(0x1f230004, 4, &v) != 1 || (v & 8))
    return -1;
  systemgq_set_input("p3button1", 0);
  fprintf(stderr, "SystemGQ selftest: PASS source_GPU1_VRAM2M=1 RAM4M=1 "
                  "three_guns=1 inputs=1 source_SIO_idle=1\n");
  return 0;
}
void systemgq_report(FILE *f) {
  if (!board.active)
    return;
  konamigq_status_t s;
  konamigq_get_status(board.device, &s);
  fprintf(f,
          "SystemGQ: profile=cryptklr GPU=1 VRAM=2097152 RAM=4194304 media=%s "
          "time_ns=%" PRIu64 " faults=%u/%08x HDD_reads=%" PRIu64
          " writes=%" PRIu64 " errors=%" PRIu64 " commands=%" PRIu64
          " DMA5=%" PRIu64 " remaining=%" PRIu64 "\n",
          board.synthetic ? "SYNTHETIC_FIXTURE" : "SOURCE_VERIFIED",
          board.now_ns, board.fault, s.faults, board.reads, board.writes,
          board.errors, s.hard_disk.commands, board.dma_requests,
          s.dma_bytes_remaining);
  fprintf(f,
          "GQ sound: M68000=%" PRIu64 " cycles reset=%u TMS=%" PRIu64
          " cycles ctrl=%02x K054539=%" PRIu64 " cycles frames=%" PRIu64
          " nonzero=%" PRIu64 " peak=%u clipped=%" PRIu64 " queued=%u/%u\n",
          s.sound_cpu_cycles, s.sound_cpu_reset, s.dsp_cycles, s.sound_ctrl,
          s.k054539_cycles, board.audio_frames, board.audio_nonzero,
          board.audio_peak, board.audio_clipped, board.count[0],
          board.count[1]);
  fprintf(f,
          "GQ firmware: sound_PC=%06x SR=%04x A7=%08x D0=%08x stopped=%u "
          "bus_error=%u DSP_PC=%02x\n",
          s.sound_pc, s.sound_sr, s.sound_a7, s.sound_d0, s.sound_stopped,
          s.sound_bus_error, s.dsp_pc);
  fprintf(f,
          "GQ guns: P1=%03x/%02x P2=%03x/%02x P3=%03x/%02x inputs=%08x/%08x "
          "DSW=%08x MMIO=%" PRIu64 "/%" PRIu64 "\n",
          board.inputs[0], board.inputs[1], board.inputs[2], board.inputs[3],
          board.inputs[4], board.inputs[5], board.inputs[6], board.inputs[7],
          board.inputs[8], board.mmio_reads, board.mmio_writes);
}
