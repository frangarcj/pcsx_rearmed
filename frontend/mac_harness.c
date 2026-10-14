/* Headless native macOS harness, GPL-2.0-or-later. */
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <unistd.h>
#ifdef PCSX_NATIVE_VITA_PATH
#include <SDL3/SDL.h>
#include "mac_pause.h"
#include "gun_pointer.h"
#include "arcade_audio_mixer.h"
#endif
#include "../libpcsxcore/r3000a.h"
#include "../libpcsxcore/psxinterpreter.h"
#include "../libpcsxcore/gte.h"
#include "../libpcsxcore/misc.h"
#include "../libpcsxcore/psxcounters.h"
#include "../libpcsxcore/spu.h"
#include "../libpcsxcore/gpu.h"
#include "../plugins/dfsound/spu_config.h"
#include "plugin.h"
#include "plugin_lib.h"
#include "gpu_vram.h"
#include "../libpcsxcore/psxmem.h"
#ifdef PCSX_SYSTEM12
#include "../libpcsxcore/system12.h"
#include "../libpcsxcore/arcade/system12_romset.h"
#include "../libpcsxcore/system11.h"
#include "../libpcsxcore/system10.h"
#include "../libpcsxcore/arcade/pcm_wav.h"
#include "../plugins/dfsound/out.h"
#include "../libpcsxcore/systemzn.h"
#include "../libpcsxcore/systemtaito.h"
#ifdef PCSX_SYSTEM_GV
#include "../libpcsxcore/systemgv.h"
#endif
#ifdef PCSX_SYSTEM_GNET
#include "../libpcsxcore/systemgnet.h"
typedef struct HarnessGnetAudio {
  uint64_t frames, last_cycle;
  int finite, monotonic, nonzero;
} HarnessGnetAudio;
static HarnessGnetAudio gnet_audio;
static void harness_gnet_audio_frame(void *opaque, float left, float right,
    uint64_t zsg_cycle)
{
  HarnessGnetAudio *audio = opaque;
  if (!isfinite(left) || !isfinite(right)) audio->finite = 0;
  if (audio->frames && zsg_cycle <= audio->last_cycle) audio->monotonic = 0;
  if (fabsf(left) > 0.00001f || fabsf(right) > 0.00001f) audio->nonzero = 1;
  audio->frames++;
  audio->last_cycle = zsg_cycle;
}
#endif

enum { BOARD_BUS_TRACE_BIN_CYCLES = 8192, BOARD_BUS_TRACE_MAX_BINS = 100000 };
typedef struct harness_bus_trace_bin {
  uint64_t hash;
  uint32_t count, first_cycle, first_address, first_value, first_pc;
  uint8_t first_width, first_write;
} harness_bus_trace_bin;
static void harness_system12_bus_event(void *opaque, u32 address, unsigned width,
    int is_write, u32 value, u32 pc, u32 cycle)
{
  harness_bus_trace_bin *bins = opaque;
  uint32_t index = cycle / BOARD_BUS_TRACE_BIN_CYCLES;
  harness_bus_trace_bin *bin;
  uint64_t fields[2];
  unsigned i, j;
  if (!bins || index >= BOARD_BUS_TRACE_MAX_BINS) return;
  /* Ignore writes to MAME's declared no-op windows. They dominate the
   * early BIOS wait loop but cannot distinguish the board's real buses. */
  if (is_write && ((address >= 0x1f008000 && address < 0x1f008004) ||
      (address >= 0x1f010000 && address < 0x1f010004) ||
      (address >= 0x1f018000 && address < 0x1f018004) ||
      (address >= 0x1f1bff08 && address < 0x1f1bff10) ||
      (address >= 0x1f000002 && address < 0x1f000004))) return;
  bin = &bins[index];
  if (!bin->count) {
    bin->hash = UINT64_C(14695981039346656037);
    bin->first_cycle = cycle;
    bin->first_address = address;
    bin->first_value = value;
    bin->first_pc = pc;
    bin->first_width = width > 255 ? 255 : width;
    bin->first_write = is_write != 0;
  }
  if (bin->count != UINT32_MAX) bin->count++;
  fields[0] = address | ((uint64_t)(width & 0xff) << 32) |
    ((uint64_t)(is_write != 0) << 40);
  fields[1] = value;
  for (i = 0; i < 2; i++)
    for (j = 0; j < sizeof(fields[i]); j++) {
      bin->hash ^= (fields[i] >> (j * 8)) & 0xff;
      bin->hash *= UINT64_C(1099511628211);
    }
}

static int write_system12_bus_trace(FILE *file,
    const harness_bus_trace_bin *bins)
{
  unsigned i;
  if (!file || !bins) return 0;
  fputs("bin,cycle,events,fnv64,address,width,write,value,reported_pc\n", file);
  for (i = 0; i < BOARD_BUS_TRACE_MAX_BINS; i++) {
    const harness_bus_trace_bin *b = &bins[i];
    if (b->count)
      fprintf(file, "%u,%u,%u,%016" PRIx64 ",%08x,%u,%u,%08x,%08x\n",
        i, b->first_cycle, b->count, b->hash, b->first_address,
        b->first_width, b->first_write, b->first_value, b->first_pc);
  }
  return ferror(file) ? -1 : 0;
}

#ifdef PCSX_SYSTEM12
typedef struct {
  FILE *file;
  system12_edge_event last_read[3];
  uint64_t sequence, last_read_sequence[3], printed_sequence;
  unsigned rows;
  int have_read[3], overflow;
} HarnessEdgeTrace;

static void write_edge_event(HarnessEdgeTrace *trace,
    const system12_edge_event *event, uint64_t sequence)
{
  if (sequence <= trace->printed_sequence) return;
  if (trace->rows >= 20000) { trace->overflow = 1; return; }
  fprintf(trace->file, "%" PRIu64 ",%s,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%u,%u\n",
    sequence, event->kind == SYSTEM12_EDGE_VBLANK ? "vblank" :
      event->kind == SYSTEM12_EDGE_H8_STORE ? "h8_store" :
      event->kind == SYSTEM12_EDGE_H8_LOAD ? "h8_load" :
      event->kind == SYSTEM12_EDGE_MAIN_SHARED_LOAD ? "main_shared_load" :
      event->kind == SYSTEM12_EDGE_MAIN_SHARED_STORE ? "main_shared_store" : "portb_input",
    event->value, event->main_cycle, event->synced_main_cycle,
    (uint64_t)event->h8_cycle, (uint64_t)event->h8_budget, event->address, event->width);
  trace->printed_sequence = sequence;
  trace->rows++;
}

static void flush_edge_reads(HarnessEdgeTrace *trace)
{
  /* Last reads from independent CPUs may have interleaved sequence numbers.
   * Emit the pending observations in order before a write or value change. */
  unsigned count;
  for (count = 0; count < 3; count++) {
    unsigned i, next = 3;
    for (i = 0; i < 3; i++)
      if (trace->have_read[i] && trace->last_read_sequence[i] > trace->printed_sequence &&
          (next == 3 || trace->last_read_sequence[i] < trace->last_read_sequence[next]))
        next = i;
    if (next == 3) break;
    write_edge_event(trace, &trace->last_read[next], trace->last_read_sequence[next]);
  }
}

static void harness_system12_edge_event(void *opaque,
    const system12_edge_event *event)
{
  HarnessEdgeTrace *trace = opaque;
  uint64_t sequence = ++trace->sequence;
  int slot = event->kind == SYSTEM12_EDGE_PORTB_INPUT ? 0 :
    event->kind == SYSTEM12_EDGE_H8_LOAD ? 1 :
    event->kind == SYSTEM12_EDGE_MAIN_SHARED_LOAD ? 2 : -1;
  if (slot < 0 || !trace->have_read[slot] ||
      (event->kind == SYSTEM12_EDGE_PORTB_INPUT
       ? ((trace->last_read[slot].value ^ event->value) & 0x80)
       : trace->last_read[slot].value != event->value ||
         trace->last_read[slot].address != event->address ||
         trace->last_read[slot].width != event->width)) {
    flush_edge_reads(trace);
    write_edge_event(trace, event, sequence);
  }
  if (slot >= 0) {
    trace->last_read[slot] = *event;
    trace->last_read_sequence[slot] = sequence;
    trace->have_read[slot] = 1;
  }
}
#endif

typedef struct HarnessBoard {
  int family;
  const char *name;
  int (*load)(const char *);
  void (*unload)(void);
  int (*sync)(u32);
  int (*set_input)(const char *, int);
  int (*selftest)(void);
  void (*report)(FILE *);
  int (*open_audio)(const char *);
  int (*close_audio)(void);
  int (*import_nvram)(const unsigned char *, size_t);
  int (*export_nvram)(unsigned char *, size_t);
} HarnessBoard;
static const HarnessBoard *selected_board;
static int sync_system12(u32 cycle) { system12_sync(cycle); return 0; }
#ifdef PCSX_SYSTEM_GV
static int sync_systemgv(u32 cycle) { return systemgv_sync(cycle); }
#endif
#ifdef PCSX_SYSTEM_GNET
static int sync_systemgnet(u32 cycle) { return systemgnet_sync(cycle); }
#endif
#include "../tests/gnet_spu_dma_fixture.h"
/* --system12 takes a prepared region directory or a MAME zip (<set>.zip). */
static int load_system12(const char *path)
{
  /* The report's H8 perf line: time the board's work. */
  extern int system12_perf_timing;
  system12_perf_timing = 1;
  return system12_romset_is_zip(path) ? system12_romset_load(path) : system12_load(path);
}
/* System 10 uses the PSX SPU. Capture its native PCM without replacing the
 * emulated chip or resampling its 768-main-cycle sample cadence. */
static PcmWav spu_wav;
static void (*spu_original_feed)(void *, int);
static unsigned spu_audio_peak;
static int source_spu_speaker_board(void)
{ return selected_board && (selected_board->family == 104 || selected_board->family == 107 || selected_board->family == 110 || selected_board->family == 111 || selected_board->family == 112); }
static void capture_spu_pcm(void *data, int bytes)
{
  const int16_t *samples = data;
  if (source_spu_speaker_board() && spu_original_feed)
    spu_original_feed(data, bytes);
  int16_t four[256 * 4] = {0};
  if (bytes < 0 || (bytes & 3)) { spu_wav.error = 1; return; }
  size_t frames = (size_t)bytes / 4;
  while (frames) {
    size_t n = frames < 256 ? frames : 256;
    for (size_t i = 0; i < n; i++) {
      for (unsigned ch = 0; ch < 2; ch++) {
        int value = samples[i * 2 + ch];
        unsigned magnitude = value < 0 ? (unsigned)-value : (unsigned)value;
        if (magnitude > spu_audio_peak) spu_audio_peak = magnitude;
        four[i * 4 + ch] = value;
      }
    }
    if (pcm_wav_append4(&spu_wav, four, n)) return;
    samples += n * 2; frames -= n;
  }
}
static int open_spu_audio(const char *path)
{
  if (!out_current || pcm_wav_open(&spu_wav, path, PSXCLK / 768)) return -1;
  spu_audio_peak = 0;
  spu_original_feed = out_current->feed;
  out_current->feed = capture_spu_pcm;
  return 0;
}
static int close_spu_audio(void)
{
  if (!spu_original_feed) return pcm_wav_close(&spu_wav);
  if (SPU_async) SPU_async(psxRegs.cycle, 1);
  if (out_current) out_current->feed = spu_original_feed;
  spu_original_feed = NULL;
  fprintf(stderr, "%s SPU: rate=%u frames=%" PRIu64 " peak=%u WAV_bytes=%" PRIu64 "\n",
    selected_board && selected_board->family == 102 ? "KonamiGV" :
    selected_board && selected_board->family == 104 ? "TecmoTPS" :
    selected_board && selected_board->family == 112 ? "JudgeZN" : selected_board && selected_board->family == 111 ? "NBAJamZN" : selected_board && selected_board->family == 110 ? "PrimalZN" : selected_board && selected_board->family == 107 ? "ViscoZN" : "System10",
    spu_wav.rate, spu_wav.bytes / 4, spu_audio_peak, spu_wav.bytes);
  return pcm_wav_close(&spu_wav);
}

static const HarnessBoard boards[] = {
  {112,"JudgeZN",systemzn_load_jdredd,systemzn_unload,systemzn_sync,
    systemzn_set_input,systemzn_selftest,systemzn_report,
    open_spu_audio,close_spu_audio,systemzn_eeprom_import,systemzn_eeprom_export},
  {111, "NBAJamZN", systemzn_load_nbajam, systemzn_unload, systemzn_sync,
    systemzn_set_input, systemzn_selftest, systemzn_report,
    open_spu_audio, close_spu_audio, systemzn_eeprom_import, systemzn_eeprom_export},
  {110, "PrimalZN", systemzn_load_primal, systemzn_unload, systemzn_sync,
    systemzn_set_input, systemzn_selftest, systemzn_report,
    open_spu_audio, close_spu_audio, systemzn_eeprom_import, systemzn_eeprom_export},
  {101, "TaitoFX1S", systemtaito_load, systemtaito_unload, systemtaito_sync,
    systemtaito_set_input, systemtaito_selftest, systemtaito_report,
    systemtaito_open_audio, systemtaito_close_audio, systemtaito_eeprom_import, systemtaito_eeprom_export},
#ifdef PCSX_SYSTEM_GV
  {102, "KonamiGV", systemgv_load, systemgv_unload, sync_systemgv,
    systemgv_set_input, systemgv_selftest, systemgv_report,
    open_spu_audio, close_spu_audio, systemgv_eeprom_import, systemgv_eeprom_export},
#endif
#ifdef PCSX_SYSTEM_GNET
  {103, "GNet", systemgnet_load, systemgnet_unload, sync_systemgnet,
    systemgnet_set_input, systemgnet_selftest, systemgnet_report,
    systemgnet_open_audio, systemgnet_close_audio, systemgnet_eeprom_import, systemgnet_eeprom_export},
#endif
  {105, "TaitoFX1Z", systemtaito_load_fx1z, systemtaito_unload, systemtaito_sync,
    systemtaito_set_input, systemtaito_selftest, systemtaito_report,
    systemtaito_open_audio, systemtaito_close_audio, systemtaito_eeprom_import, systemtaito_eeprom_export},
  {106, "RaizingZN", systemzn_load_raizing, systemzn_unload, systemzn_sync,
    systemzn_set_input, systemzn_selftest, systemzn_report,
    systemzn_open_audio, systemzn_close_audio, systemzn_eeprom_import, systemzn_eeprom_export},
  {109, "CbajTPS", systemzn_load_cbaj, systemzn_unload, systemzn_sync,
    systemzn_set_input, systemzn_selftest, systemzn_report,
    systemzn_open_audio, systemzn_close_audio, systemzn_eeprom_import, systemzn_eeprom_export},
  {108, "AtlusZN", systemzn_load_atlus, systemzn_unload, systemzn_sync,
    systemzn_set_input, systemzn_selftest, systemzn_report,
    systemzn_open_audio, systemzn_close_audio, systemzn_eeprom_import, systemzn_eeprom_export},
  {107, "ViscoZN", systemzn_load_visco, systemzn_unload, systemzn_sync,
    systemzn_set_input, systemzn_selftest, systemzn_report,
    open_spu_audio, close_spu_audio, systemzn_eeprom_import, systemzn_eeprom_export},
  {104, "TecmoTPS", systemzn_load_tps, systemzn_unload, systemzn_sync,
    systemzn_set_input, systemzn_selftest, systemzn_report,
    open_spu_audio, close_spu_audio, systemzn_eeprom_import, systemzn_eeprom_export},
  {100, "CapcomZN", systemzn_load, systemzn_unload, systemzn_sync,
    systemzn_set_input, systemzn_selftest, systemzn_report,
    systemzn_open_audio, systemzn_close_audio, systemzn_eeprom_import, systemzn_eeprom_export},
  {10, "System10", system10_load, system10_unload, system10_sync,
    system10_set_input, system10_selftest, system10_report, open_spu_audio, close_spu_audio, NULL, NULL},
  {11, "System11", system11_load, system11_unload, system11_sync,
    system11_set_input, system11_selftest, system11_report,
    system11_open_audio, system11_close_audio, system11_eeprom_import, system11_eeprom_export},
  {12, "System12", load_system12, system12_unload, sync_system12,
    system12_set_input, system12_selftest, system12_report,
    system12_open_audio, system12_close_audio, system12_eeprom_import, system12_eeprom_export},
};
#endif

#if !defined(__APPLE__) || !defined(__aarch64__)
#error This harness targets native macOS ARM64.
#endif
#if defined(GPU_NEON) && !defined(__ARM_NEON) && !defined(__ARM_NEON__)
#error GPU_NEON needs ARM Advanced SIMD.
#endif

int in_type[8] = { PSE_PAD_TYPE_STANDARD, PSE_PAD_TYPE_STANDARD };
int multitap1, multitap2, in_analog_left[8][2], in_analog_right[8][2];
unsigned short in_keystate[8];
int in_mouse[8][2];
struct rearmed_cbs pl_rearmed_cbs;
void GPUrearmedCallbacks(const struct rearmed_cbs *cbs);
int harness_write_ppm(const char *path, int w, int h);
int harness_write_vram(const char *path);
int harness_configure_gl_capture(int enabled, unsigned int target_frame);
unsigned short harness_vram_pixel(int x, int y);
void harness_capture_mode(int w, int h);
void harness_capture_flip(const void *vram, int offset, int rgb24,
                          int x, int y, int w, int h);
static volatile sig_atomic_t interrupted;
static unsigned int flips;
static void on_signal(int sig) { interrupted = sig; }
static int video_open(void) { return 0; }
static void video_close(void) {}
static void video_mode(int w, int h, int rw, int rh, int bpp)
{
  harness_capture_mode(w, h);
}
static void video_flip(const void *vram, int offset, int bgr24,
                       int x, int y, int w, int h, int changed)
{
  harness_capture_flip(vram, offset, bgr24, x, y, w, h);
  flips++;
}
static void *gpu_alloc(unsigned int size)
{
  void *ptr = NULL;
  if (posix_memalign(&ptr, 4096, size)) return NULL;
  memset(ptr, 0, size);
  return ptr;
}
static void gpu_free(void *ptr, unsigned int size) { free(ptr); }
static int frontend_loop;
/* As in libretro: the core's per-frame call makes psxCpu->Execute return. */
void pl_frame_limit(void) { if (frontend_loop) psxRegs.stop++; }
void pl_timing_prepare(int is_pal) {}
void plat_trigger_vibrate(int pad, int low, int high) {}
void pl_gun_byte2(int gun, unsigned char byte) {}
void SysPrintf(const char *fmt, ...)
{
  va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
}
void SysMessage(const char *fmt, ...)
{
  va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
  fputc('\n', stderr);
}
void *SysLoadLibrary(const char *path)
{
  return (void *)(uintptr_t)(PLUGIN_DL_BASE + (strstr(path, "spu") ? PLUGIN_SPU : PLUGIN_GPU));
}
void *SysLoadSym(void *lib, const char *sym)
{
  return plugin_link((uintptr_t)lib - PLUGIN_DL_BASE, sym);
}
const char *SysLibError(void) { return NULL; }
void SysCloseLibrary(void *lib) {}
void SysRunGui(void) { interrupted = 1; }

static void usage(const char *name)
{
  fprintf(stderr, "usage: %s (--exe PS-X-EXE | --system12 REGION-DIR | --system11 REGION-DIR | --system10 REGION-DIR | --capcom-zn REGION-DIR | --tecmo-tps REGION-DIR | --raizing-zn REGION-DIR | --visco-zn REGION-DIR | --atlus-zn REGION-DIR | --jdredd-zn REGION-DIR | --nbajam-zn REGION-DIR | --primal-zn REGION-DIR | --cbaj-tps REGION-DIR | --taito-fx1 REGION-DIR | --taito-fx1z REGION-DIR | --system-gv REGION-DIR | --system-gnet REGION-DIR | --system-gnet-fixture) "
    "[--frames N] [--output FRAME.ppm] [--capture-gl] [--dump-vram FILE] [--dump-ram FILE] [--board-test] [--verify-smoke] "
    "[--inputs FILE.csv] [--audio FILE.wav] [--mixed-audio FILE.wav] [--nvram FILE] [--gnet-jp1 on|off] "
    "[--audio-test-fixture] "
    "[--reset-at-cycle N] "
    "[--trace FILE --trace-blocks N --trace-start CYCLES] [--cycle-snapshots FILE "
    "--snapshot-start CYCLES --snapshot-interval N] "
    "[--board-bus-trace FILE] [--system12-edge-trace FILE] [--system12-trace-shared-writes | --system12-trace-shared-accesses] "
    "[--max-blocks N] [--icache] [--widescreen] [--borderless] [--gpu-thread] [--save-state-at FRAME FILE] [--load-state FILE] [--drc] [--interactive [--art-dir DIR] [--pause-at N [--pause-script STEPS] [--pause-shot FILE.ppm]]...] [--system12-vblank-sync-only] [--system12-source-step] [--frontend-loop]\n"
    "       %s (--gpu-type1-test | --gpu-type1-test-1m)\n", name, name);
}

#if defined(PCSX_NATIVE_VITA_PATH) && defined(PCSX_SYSTEM12)
static const struct { SDL_Scancode key; const char *control; } interactive_keys[] = {
  {SDL_SCANCODE_5, "coin1"}, {SDL_SCANCODE_1, "p1start"},
  {SDL_SCANCODE_UP, "p1up"}, {SDL_SCANCODE_DOWN, "p1down"},
  {SDL_SCANCODE_LEFT, "p1left"}, {SDL_SCANCODE_RIGHT, "p1right"},
  {SDL_SCANCODE_A, "p1button1"}, {SDL_SCANCODE_S, "p1button2"},
  {SDL_SCANCODE_D, "p1button3"}, {SDL_SCANCODE_SPACE, "p1button4"},
  {SDL_SCANCODE_Q, "p1button5"}, {SDL_SCANCODE_E, "p1button6"},
  {SDL_SCANCODE_F, "p1button7"}, {SDL_SCANCODE_T, "test"},
  {SDL_SCANCODE_9, "service"},
  {SDL_SCANCODE_6, "coin2"}, {SDL_SCANCODE_2, "p2start"},
  {SDL_SCANCODE_I, "p2up"}, {SDL_SCANCODE_K, "p2down"},
  {SDL_SCANCODE_J, "p2left"}, {SDL_SCANCODE_L, "p2right"},
  {SDL_SCANCODE_Z, "p2button1"}, {SDL_SCANCODE_X, "p2button2"}, {SDL_SCANCODE_C, "p2button3"},
  {SDL_SCANCODE_V, "p2button4"},
  {SDL_SCANCODE_7,"coin3"},{SDL_SCANCODE_8,"coin4"},
  {SDL_SCANCODE_3,"p3start"},{SDL_SCANCODE_4,"p4start"},
  {SDL_SCANCODE_KP_8,"p3up"},{SDL_SCANCODE_KP_2,"p3down"},{SDL_SCANCODE_KP_4,"p3left"},{SDL_SCANCODE_KP_6,"p3right"},
  {SDL_SCANCODE_KP_1,"p3button1"},{SDL_SCANCODE_KP_3,"p3button2"},{SDL_SCANCODE_KP_7,"p3button3"},{SDL_SCANCODE_KP_9,"p3button4"},
  {SDL_SCANCODE_F1,"p4up"},{SDL_SCANCODE_F2,"p4down"},{SDL_SCANCODE_F3,"p4left"},{SDL_SCANCODE_F4,"p4right"},
  {SDL_SCANCODE_F5,"p4button1"},{SDL_SCANCODE_F6,"p4button2"},{SDL_SCANCODE_F7,"p4button3"},{SDL_SCANCODE_F8,"p4button4"},
  {SDL_SCANCODE_F, "p1button5"}, {SDL_SCANCODE_H, "p1button6"},
  {SDL_SCANCODE_B, "p2button5"}, {SDL_SCANCODE_N, "p2button6"}
};
static volatile int pause_requested;
static unsigned char interactive_key_down[sizeof(interactive_keys) / sizeof(interactive_keys[0])];
static int interactive_mouse_trigger, interactive_mouse_sensor;
static int interactive_judge_mouse_aim;
static int interactive_replay, interactive_focused = 1;
static int interactive_mouse_wheel = 128, interactive_mouse_handle = 128;
static unsigned interactive_analog_frame;
static SDL_Gamepad *interactive_pads[4];
static unsigned interactive_volume = 100;

static void interactive_board_input(const char *control, int pressed)
{
  if (!selected_board) return;
  if ((selected_board->family == 103 || selected_board->family == 105) && control[0] == 'p' &&
      (control[1] == '1' || control[1] == '2')) {
    char name[32];
    snprintf(name, sizeof(name), "%.2s_%s", control, control + 2);
    selected_board->set_input(name, pressed);
  } else if ((selected_board->family == 104 || selected_board->family == 106 || selected_board->family == 107 || selected_board->family == 108 || selected_board->family == 109 || selected_board->family == 110 || selected_board->family == 111 || selected_board->family == 112) && !strcmp(control, "service"))
    selected_board->set_input("service1", pressed);
  else selected_board->set_input(control, pressed);
}
void mac_interactive_set_test(int enabled) { interactive_board_input("test", enabled); }
void mac_interactive_set_volume(unsigned percent)
{
  interactive_volume = percent > 400 ? 400 : percent;
  if (selected_board && selected_board->family == 12) system12_set_volume(interactive_volume);
}
static int interactive_key(SDL_Scancode code)
{
  for (unsigned i = 0; i < sizeof(interactive_keys) / sizeof(interactive_keys[0]); ++i)
    if (interactive_keys[i].key == code) return interactive_key_down[i];
  return 0;
}
static int raw_axis(int value)
{
  int raw = (value + 32768) / 256;
  return raw < 0 ? 0 : raw > 255 ? 255 : raw;
}
static void refresh_interactive_gamepads(void)
{
  const unsigned ports=selected_board && selected_board->family==111?4:2;
  for (unsigned i = 0; i < 4; ++i)
    if (interactive_pads[i] && !SDL_GamepadConnected(interactive_pads[i])) {
      SDL_CloseGamepad(interactive_pads[i]); interactive_pads[i] = NULL;
    }
  int count = 0;
  SDL_JoystickID *ids = SDL_GetGamepads(&count);
  for (int n = 0; ids && n < count; ++n) {
    int already=0;
    for(unsigned i=0;i<ports;i++)if(interactive_pads[i] && SDL_GetGamepadID(interactive_pads[i])==ids[n])already=1;
    if(already)continue;
    for (unsigned i = 0; i < ports; ++i) if (!interactive_pads[i]) {
      interactive_pads[i] = SDL_OpenGamepad(ids[n]); break;
    }
  }
  SDL_free(ids);
}
static void poll_interactive_analog(void)
{
  if(selected_board && selected_board->family==112 && !interactive_replay && interactive_focused){
    for(unsigned p=0;p<2;p++)if(interactive_pads[p] && (p || !interactive_judge_mouse_aim)){
      unsigned x=SDL_GetGamepadAxis(interactive_pads[p],SDL_GAMEPAD_AXIS_LEFTX)+32768;
      unsigned y=SDL_GetGamepadAxis(interactive_pads[p],SDL_GAMEPAD_AXIS_LEFTY)+32768;
      systemzn_set_gun_position(p,0x394+(uint64_t)x*(0xcb1-0x394)/65535,0x02e + (uint64_t)y*(0x216-0x02e)/65535);
    }
    return;
  }

#ifdef PCSX_SYSTEM_GNET
  if (!selected_board || selected_board->family != 103 || interactive_replay) return;
  int wheel = interactive_mouse_wheel, trigger = 128, handle[2] = {interactive_mouse_handle,128};
  if (interactive_focused) {
    for (unsigned i = 0; i < 2; ++i) if (interactive_pads[i]) {
      int x = SDL_GetGamepadAxis(interactive_pads[i], SDL_GAMEPAD_AXIS_LEFTX);
      if (abs(x) > 4096) { handle[i] = raw_axis(x); if (!i) wheel = raw_axis(x); }
      if (!i) {
        int t = SDL_GetGamepadAxis(interactive_pads[i], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) -
                SDL_GetGamepadAxis(interactive_pads[i], SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        if (abs(t) > 1024) trigger = raw_axis(t);
      }
    }
    int p1 = interactive_key(SDL_SCANCODE_RIGHT) - interactive_key(SDL_SCANCODE_LEFT);
    int p2 = interactive_key(SDL_SCANCODE_L) - interactive_key(SDL_SCANCODE_J);
    if (p1) wheel = handle[0] = p1 > 0 ? 255 : 0;
    if (p2) handle[1] = p2 > 0 ? 255 : 1;
    if (interactive_mouse_trigger || interactive_key(SDL_SCANCODE_A)) trigger = 255;
    if (interactive_mouse_sensor || interactive_key(SDL_SCANCODE_S)) trigger = 0;
  } else wheel = trigger = handle[0] = handle[1] = 128;
  systemgnet_set_analog("p1_wheel", wheel);
  systemgnet_set_analog("p1_trigger", trigger);
  systemgnet_set_analog("p1_handle", handle[0] < 1 ? 1 : handle[0]);
  systemgnet_set_analog("p2_handle", handle[1]);
#endif
}
/* gpulib_if.c: the next present keeps the frame for the menu. */
extern volatile int pcsx_menu_backdrop_request;

int mac_pause_interrupted(void)
{
  return interrupted != 0;
}

static void release_interactive_input(void)
{
  if (interactive_replay) return; /* preserve replay state across pause/focus */
  memset(interactive_key_down, 0, sizeof(interactive_key_down));
  interactive_mouse_trigger = interactive_mouse_sensor = 0;interactive_judge_mouse_aim=0;
  for (unsigned i = 0; i < sizeof(interactive_keys) / sizeof(interactive_keys[0]); i++)
    interactive_board_input(interactive_keys[i].control, 0);
  interactive_mouse_wheel = interactive_mouse_handle = 128;
#ifdef PCSX_SYSTEM_GNET
  if (selected_board && selected_board->family == 103) {
    systemgnet_set_analog("p1_wheel",128); systemgnet_set_analog("p1_trigger",128);
    systemgnet_set_analog("p1_handle",128); systemgnet_set_analog("p2_handle",128);
  }
#endif
}

static int interactive_pad_button(const char *control)
{
  if (!selected_board || (selected_board->family != 103 && selected_board->family != 104 && selected_board->family != 105 && selected_board->family != 106 && selected_board->family != 107 && selected_board->family != 108 && selected_board->family != 109 && selected_board->family != 110 && selected_board->family != 111 && selected_board->family != 112) || !interactive_focused) return 0;
  unsigned port=control[0]=='p' && control[1]>='1' && control[1]<='4'?control[1]-'1':0;
  const char *name = control[0] == 'p' ? control + 2 : control;
  SDL_Gamepad *pad = interactive_pads[port];
  if (!strncmp(control, "coin", 4)) {
    unsigned player=control[4]>='1' && control[4]<='4'?control[4]-'1':0;
    pad=interactive_pads[player];
    return pad && (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) ||
      SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK));
  }
  static const struct { const char *name; SDL_GamepadButton button; } map[] = {
    {"up",SDL_GAMEPAD_BUTTON_DPAD_UP},{"down",SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"left",SDL_GAMEPAD_BUTTON_DPAD_LEFT},{"right",SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"button1",SDL_GAMEPAD_BUTTON_SOUTH},{"button2",SDL_GAMEPAD_BUTTON_EAST},
    {"button3",SDL_GAMEPAD_BUTTON_WEST},{"button4",SDL_GAMEPAD_BUTTON_NORTH},
    {"button5",SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},{"button6",SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},{"start",SDL_GAMEPAD_BUTTON_START}
  };
  for (unsigned i = 0; pad && i < sizeof(map) / sizeof(map[0]); ++i)
    if (!strcmp(name,map[i].name)) return SDL_GetGamepadButton(pad,map[i].button);
  return 0;
}

static void set_interactive_control(const char *control, int pressed)
{
  if (selected_board && (selected_board->family == 12 || selected_board->family==112)) {
    if (!strcmp(control, "p1button1")) pressed |= interactive_mouse_trigger;
    if (!strcmp(control, selected_board->family==112?"p1button2":"p1button7")) pressed |= interactive_mouse_sensor;
  }
  pressed |= interactive_pad_button(control);
  for (unsigned i = 0; i < sizeof(interactive_keys) / sizeof(interactive_keys[0]); i++)
    if (!strcmp(control, interactive_keys[i].control)) pressed |= interactive_key_down[i];
  interactive_board_input(control, pressed);
}

static int aim_interactive_mouse(SDL_WindowID id, float px, float py)
{
  SDL_Window *window = SDL_GetWindowFromID(id);
  int w, h, pw, ph, rect[4]; unsigned x, y;
  if (!window || !SDL_GetWindowSize(window, &w, &h) ||
      !SDL_GetWindowSizeInPixels(window, &pw, &ph) || w < 1 || h < 1) return 0;
  rect[2] = pw; rect[3] = pw * 3 / 4;
  if (rect[3] > ph) { rect[3] = ph; rect[2] = ph * 4 / 3; }
  rect[0] = (pw - rect[2]) / 2; rect[1] = (ph - rect[3]) / 2;
#ifdef GPU_VITA_CTR
  extern void VitaCtrNativeRenderer_GetFrameViewport(int rect[4]);
  int viewport[4]; VitaCtrNativeRenderer_GetFrameViewport(viewport);
  if (viewport[2] > 1 && viewport[3] > 1) {
    memcpy(rect, viewport, sizeof(rect)); rect[1] = ph - rect[1] - rect[3];
  }
#endif
  int inside = gun_pointer_position(px * pw / w, py * ph / h, rect, &x, &y);
  if(rect[2]>1 && rect[3]>1){
    if(selected_board && selected_board->family==112){
      interactive_judge_mouse_aim=1;
      systemzn_set_gun_position(0,inside?0x394+(uint64_t)x*(0xcb1-0x394)/65535:0x392,inside?0x02e + (uint64_t)y*(0x216-0x02e)/65535:0x02c);
    }else system12_set_gun_position(0,x,y);
  }
  return inside;
}

static void poll_interactive_input(void)
{
  SDL_Event event;
  int update_pads = frame_counter != interactive_analog_frame;
  if (frame_counter != interactive_analog_frame) {
    interactive_mouse_handle = 128;
    interactive_analog_frame = frame_counter;
  }
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
      interrupted = 1;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
      interactive_focused = 0; release_interactive_input();
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) interactive_focused = 1;
    if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED)
      refresh_interactive_gamepads();
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
      update_pads = 1;
    if(selected_board && selected_board->family==112 && event.type==SDL_EVENT_GAMEPAD_AXIS_MOTION && interactive_pads[0] && event.gaxis.which==SDL_GetGamepadID(interactive_pads[0]))interactive_judge_mouse_aim=0;
    if (interactive_replay || !interactive_focused) {
      if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE) {
        pause_requested = 1; pcsx_menu_backdrop_request = 1;
      }
      continue;
    }
#ifdef PCSX_SYSTEM_GNET
    if (selected_board && selected_board->family == 103) {
      if (event.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Window *window = SDL_GetWindowFromID(event.motion.windowID);
        int w = 0;
        if (window) SDL_GetWindowSize(window, &w, NULL);
        if (w > 1) {
          float raw = event.motion.x * 255.0f / (w - 1);
          interactive_mouse_wheel = raw < 0 ? 0 : raw > 255 ? 255 : (int)raw;
        }
        int value = 128 + (int)(event.motion.xrel * 16);
        interactive_mouse_handle = value < 1 ? 1 : value > 255 ? 255 : value;
      }
      if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        int down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (event.button.button == SDL_BUTTON_LEFT) interactive_mouse_trigger = down;
        if (event.button.button == SDL_BUTTON_RIGHT) interactive_mouse_sensor = down;
      }
    }
#endif
    if (selected_board && (selected_board->family==112 || (selected_board->family == 12 && system12_analog_max("p1gunx")))) {
      if (event.type == SDL_EVENT_MOUSE_MOTION) {
        aim_interactive_mouse(event.motion.windowID, event.motion.x, event.motion.y);
      }
      if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        int inside=aim_interactive_mouse(event.button.windowID,event.button.x,event.button.y);
        int pressed=event.type==SDL_EVENT_MOUSE_BUTTON_DOWN && (selected_board->family==112 || inside);
        if (event.button.button == SDL_BUTTON_LEFT) {
          interactive_mouse_trigger = pressed; set_interactive_control("p1button1", 0);
        }
        if (event.button.button == SDL_BUTTON_RIGHT) {
          interactive_mouse_sensor = pressed; set_interactive_control(selected_board->family==112?"p1button2":"p1button7", 0);
        }
      }
    }
    if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP)
      continue;
    if (event.key.repeat) continue;
    /* Escape pauses, as Start + Select on the Vita. */
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE) {
      pause_requested = 1;
      pcsx_menu_backdrop_request = 1;
    }
    for (unsigned i = 0; i < sizeof(interactive_keys) / sizeof(interactive_keys[0]); i++)
      if (event.key.scancode == interactive_keys[i].key) {
        interactive_key_down[i] = event.type == SDL_EVENT_KEY_DOWN;
        set_interactive_control(interactive_keys[i].control, 0);
      }
  }
  if (update_pads && !interactive_replay && interactive_focused && selected_board && (selected_board->family == 103 || selected_board->family == 104 || selected_board->family == 105 || selected_board->family == 106 || selected_board->family == 107 || selected_board->family == 108 || selected_board->family == 109 || selected_board->family == 110 || selected_board->family == 111 || selected_board->family == 112))
    for (unsigned i = 0; i < sizeof(interactive_keys) / sizeof(interactive_keys[0]); ++i)
      set_interactive_control(interactive_keys[i].control, 0);
  poll_interactive_analog();
}

/* The pause menu's header: the set's MAME description, else the path's
 * last part. */
static void interactive_title(const char *board, char *out, size_t size)
{
  const system12_romset_info *info = system12_romset_is_zip(board) ?
      system12_romset_identify(board) : NULL;
  const char *slash = strrchr(board, '/');
  if (info) snprintf(out, size, "%s", info->description);
  else snprintf(out, size, "%s", slash && slash[1] ? slash + 1 : board);
}

/* Interactive sound: the board's 88.2 kHz stereo goes to an SDL stream, and
 * the queued audio paces the loop, as on the Vita. */
static SDL_AudioStream *interactive_audio;
static ArcadeAudioMixer interactive_mix;
static PcmWav interactive_mix_wav;
static const char *interactive_mix_path;
static int interactive_audio_error, interactive_mixing;
static void (*interactive_spu_feed)(void *, int);
static u32 interactive_spu_cycle;
static uint64_t interactive_zn_spu_frames, interactive_zn_spu_nonzero;
static unsigned interactive_zn_spu_peak;
unsigned mac_interactive_get_volume(void) { return interactive_volume; }

static void interactive_audio_cb(void *opaque, const int16_t *stereo,
    unsigned frames)
{
  (void)opaque;
  if (!SDL_PutAudioStreamData(interactive_audio, stereo, (int)(frames * 4)))
    interactive_audio_error = 1;
  if (interactive_mix_wav.file) {
    int16_t four[256 * 4] = {0};
    while (frames) {
      unsigned n = frames > 256 ? 256 : frames;
      for (unsigned i = 0; i < n; ++i) {
        four[i * 4] = stereo[i * 2]; four[i * 4 + 1] = stereo[i * 2 + 1];
      }
      if (pcm_wav_append4(&interactive_mix_wav, four, n)) interactive_audio_error = 1;
      stereo += n * 2; frames -= n;
    }
  }
}

static void interactive_rax_pcm(void *opaque,const float *v,size_t frames,unsigned rate,const uint64_t *ticks)
{
  (void)opaque;(void)rate;
  for(size_t i=0;i<frames;i++)arcade_mix_push_timed(&interactive_mix,1,v[2*i],v[2*i+1],ticks[i]);
}
static void interactive_zoom_pcm(void *opaque, float left, float right, uint64_t cycle)
{
  (void)opaque; (void)cycle;
  const float gain = selected_board->family == 108 || selected_board->family == 109 ? 0.35f : 1;
  arcade_mix_push(&interactive_mix, 1, left * gain, right * gain);
}
static void interactive_spu_pcm(void *data, int bytes)
{
  const int16_t *samples = data;
  if (bytes < 0 || (bytes & 3)) { interactive_audio_error = 1; return; }
  interactive_mix.gain = interactive_volume;
  for (int i = 0; i < bytes / 4; ++i)
    /* Pinned ZN uses SPU gain 0.35; Taito uses 0.3. */
    arcade_mix_push(&interactive_mix, 0, samples[i * 2] * ((selected_board->family == 108 ? 0.175f : selected_board->family == 106 || selected_board->family == 109 || selected_board->family == 111 ? 0.35f : 0.3f) / 32768.0f),
      samples[i * 2 + 1] * ((selected_board->family == 108 ? 0.175f : selected_board->family == 106 || selected_board->family == 109 || selected_board->family == 111 ? 0.35f : 0.3f) / 32768.0f));
}

static void interactive_zn_spu_pcm(void *data, int bytes)
{
  const int16_t *samples = data;
  int16_t stereo[256 * 2];
  if (bytes < 0 || (bytes & 3)) { interactive_audio_error = 1; return; }
  unsigned frames = (unsigned)bytes / 4;
  interactive_zn_spu_frames += frames;
  while (frames) {
    unsigned n = frames > 256 ? 256 : frames;
    for (unsigned i = 0; i < n * 2; ++i) {
      /* Pinned zn.cpp routes both source SPU outputs at 0.35. */
      int value = (int)samples[i] * (int)interactive_volume * 35 / 10000;
      if (value > 32767) value = 32767;
      if (value < -32768) value = -32768;
      stereo[i] = value;
      if (value) interactive_zn_spu_nonzero++;
      unsigned peak = value < 0 ? -value : value;
      if (peak > interactive_zn_spu_peak) interactive_zn_spu_peak = peak;
    }
    interactive_audio_cb(NULL, stereo, n);
    samples += n * 2; frames -= n;
  }
}

static int open_interactive_audio(void)
{
  const SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, selected_board->family == 103 || selected_board->family == 104 || selected_board->family == 105 || selected_board->family == 106 || selected_board->family == 107 || selected_board->family == 108 || selected_board->family == 109 || selected_board->family == 110 || selected_board->family == 111 || selected_board->family == 112 ? 44100 : 88200 };
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return -1;
  interactive_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
      &spec, NULL, NULL);
  if (!interactive_audio) return -1;
  if (!SDL_ResumeAudioStreamDevice(interactive_audio)) {
    SDL_DestroyAudioStream(interactive_audio); interactive_audio = NULL; return -1;
  }
  if (interactive_mix_path && pcm_wav_open(&interactive_mix_wav, interactive_mix_path, 44100)) {
    SDL_DestroyAudioStream(interactive_audio); interactive_audio = NULL; return -1;
  }
  if (selected_board->family == 103 || selected_board->family == 105 || selected_board->family == 106 || selected_board->family == 108 || selected_board->family == 109 || selected_board->family == 111) {
    if(selected_board->family == 111)
      arcade_mix_init_rates(&interactive_mix,PSXCLK,16670000,1,interactive_audio_cb,NULL);
    else if (selected_board->family == 108 || selected_board->family == 109)
      arcade_mix_init_rates(&interactive_mix, PSXCLK, 16934400, 192, interactive_audio_cb, NULL);
    else if (selected_board->family == 106)
      arcade_mix_init_rates(&interactive_mix, PSXCLK, 16934400, 384, interactive_audio_cb, NULL);
    else arcade_mix_init(&interactive_mix, PSXCLK, interactive_audio_cb, NULL);
    interactive_mixing = 1;
    interactive_spu_feed = out_current->feed;
    interactive_spu_cycle = psxRegs.cycle;
    out_current->feed = interactive_spu_pcm;
    if(selected_board->family == 111)systemzn_set_rax_audio_output(interactive_rax_pcm,NULL);
    else if (selected_board->family == 108 || selected_board->family == 109) systemzn_set_ymz_audio_output(interactive_zoom_pcm, NULL);
    else if (selected_board->family == 106) systemzn_set_ymf_audio_output(interactive_zoom_pcm, NULL);
    else if (selected_board->family == 105) systemtaito_set_zoom_audio_output(interactive_zoom_pcm, NULL);
#ifdef PCSX_SYSTEM_GNET
    else systemgnet_set_audio_output(interactive_zoom_pcm, NULL);
#endif
    if(selected_board->family == 111)fprintf(stderr,"Interactive NBA audio: SPU=44100 Hz gain=0.35 RAX=guest-DAC-rate gain=1 output=44100 Hz\n");
    else if (selected_board->family == 109) fprintf(stderr, "Interactive CBaj audio: SPU=33868800/768 Hz gain=0.35 YMZ280B=16934400/192 Hz gain=0.35 output=44100 Hz\n");
    else if (selected_board->family == 108) fprintf(stderr, "Interactive Atlus audio: SPU=33868800/768 Hz gain=0.175 YMZ280B=16934400/192 Hz gain=0.35 output=44100 Hz\n");
    else if (selected_board->family == 106) fprintf(stderr, "Interactive Raizing audio: SPU=33868800/768 Hz gain=0.35 YMF271=16934400/384 Hz gain=1 output=44100 Hz\n");
    else fprintf(stderr, "Interactive %s audio: SPU=%u/768 Hz gain=0.3 Zoom=25000000/768 Hz gain=1 output=44100 Hz\n", selected_board->family == 105 ? "FX1Z" : "G-Net", PSXCLK);
  } else if (source_spu_speaker_board()) {
    interactive_spu_feed = out_current->feed;
    interactive_spu_cycle = psxRegs.cycle;
    out_current->feed = interactive_zn_spu_pcm;
    fprintf(stderr, "Interactive %s audio: SPU=44100 Hz gain=0.35 output=44100 Hz\n", selected_board->family == 112 ? "Judge ZN" : selected_board->family == 110 ? "Primal ZN" : selected_board->family == 107 ? "Visco ZN" : "Tecmo TPS");
  } else {
    system12_set_audio_output(interactive_audio_cb, NULL);
    mac_interactive_set_volume(400);
  }
  return 0;
}

/* Keep about three frames queued: enough to avoid gaps, little latency. */
static void wait_interactive_audio(void)
{
  const int rate = interactive_mixing || source_spu_speaker_board() ? 44100 : 88200;
  const int limit = rate * 4 / 60 * 3;
  while (!interrupted && !interactive_audio_error) {
    int queued = SDL_GetAudioStreamQueued(interactive_audio);
    if (queued < 0) { interactive_audio_error = 1; break; }
    if (queued <= limit) break;
    SDL_DelayNS(1000000);
    poll_interactive_input();
  }
}

static void close_interactive_audio(void)
{
  for (unsigned i = 0; i < 4; ++i) if (interactive_pads[i]) {
    SDL_CloseGamepad(interactive_pads[i]); interactive_pads[i] = NULL;
  }
  if (!interactive_audio) return;
  if (interactive_mixing) {
    if (SPU_async) SPU_async(psxRegs.cycle, 1);
    if(selected_board->family == 111)systemzn_set_rax_audio_output(NULL,NULL);
    else if (selected_board->family == 108 || selected_board->family == 109) systemzn_set_ymz_audio_output(NULL, NULL);
    else if (selected_board->family == 106) systemzn_set_ymf_audio_output(NULL, NULL);
    else if (selected_board->family == 105) systemtaito_set_zoom_audio_output(NULL, NULL);
#ifdef PCSX_SYSTEM_GNET
    else systemgnet_set_audio_output(NULL, NULL);
#endif
    out_current->feed = interactive_spu_feed;
    interactive_spu_feed = NULL;
    fprintf(stderr, "Interactive %s mix: frames=%" PRIu64 " SPU_inputs=%" PRIu64 " daughter_inputs=%" PRIu64
      " SPU_nonzero=%" PRIu64 " daughter_nonzero=%" PRIu64 " clipped=%" PRIu64 " pending=%u/%u error=%d\n",
      selected_board->family == 111 ? "NBA" : selected_board->family == 109 ? "CBaj" : selected_board->family == 108 ? "Atlus" : selected_board->family == 106 ? "Raizing" : selected_board->family == 105 ? "FX1Z" : "G-Net", interactive_mix.frames, interactive_mix.lane[0].inputs, interactive_mix.lane[1].inputs,
      interactive_mix.lane[0].nonzero, interactive_mix.lane[1].nonzero, interactive_mix.clipped,
      interactive_mix.lane[0].count, interactive_mix.lane[1].count,
      interactive_mix.error || interactive_audio_error);
    if (interactive_mix.error) interactive_audio_error = 1;
  } else if (source_spu_speaker_board()) {
    if (SPU_async) SPU_async(psxRegs.cycle, 1);
    if (spu_original_feed == interactive_zn_spu_pcm) spu_original_feed = interactive_spu_feed;
    if (out_current->feed == interactive_zn_spu_pcm) out_current->feed = interactive_spu_feed;
    interactive_spu_feed = NULL;
    fprintf(stderr, "Interactive %s audio: frames=%" PRIu64 " nonzero=%" PRIu64 " peak=%u error=%d\n",
      selected_board->family == 112 ? "Judge ZN" : selected_board->family == 110 ? "Primal ZN" : selected_board->family == 107 ? "Visco ZN" : "Tecmo TPS", interactive_zn_spu_frames, interactive_zn_spu_nonzero, interactive_zn_spu_peak, interactive_audio_error);
  } else system12_set_audio_output(NULL, NULL);
  if (pcm_wav_close(&interactive_mix_wav)) interactive_audio_error = 1;
  SDL_DestroyAudioStream(interactive_audio);
  interactive_audio = NULL;
}
#endif

#ifdef PCSX_SYSTEM12
static int load_nvram(const char *path)
{
  unsigned char image[34848];
  size_t size = selected_board->family == 111 ? 34848 : selected_board->family == 102 ? 128 : selected_board->family == 105 ? 2592 : 2080;
  FILE *file = fopen(path, "rb");
  int invalid;
  if (!file) {
    if (errno == ENOENT) return 0;
    perror(path); return -1;
  }
  invalid = fread(image, 1, size, file) != size;
  if (!invalid) invalid = fgetc(file) != EOF || ferror(file);
  if (fclose(file)) invalid = 1;
  if (invalid) {
    fprintf(stderr, "%s: expected exact %zu-byte EEPROM image\n", path, size);
    return -1;
  }
  return selected_board->import_nvram(image, size);
}

static int save_nvram(const char *path)
{
  unsigned char image[34848];
  size_t size = selected_board->family == 111 ? 34848 : selected_board->family == 102 ? 128 : selected_board->family == 105 ? 2592 : 2080;
  char *temporary;
  FILE *file;
  int fd, result = -1;
  if (selected_board->export_nvram(image, size)) return -1;
  temporary = malloc(strlen(path) + sizeof(".tmp.XXXXXX"));
  if (!temporary) return -1;
  sprintf(temporary, "%s.tmp.XXXXXX", path);
  fd = mkstemp(temporary);
  if (fd < 0) { perror(path); free(temporary); return -1; }
  file = fdopen(fd, "wb");
  if (file) {
    int invalid = fwrite(image, 1, size, file) != size;
    if (fclose(file)) invalid = 1;
    if (!invalid && !rename(temporary, path)) result = 0;
  }
  else close(fd);
  if (result) {
    fprintf(stderr, "%s: failed to save AT28C16 image\n", path);
    unlink(temporary);
  }
  free(temporary);
  return result;
}

typedef struct harness_input_event {
  uint64_t frame;
  char control[32];
  int pressed;
} harness_input_event;

static unsigned input_analog_max(const char *name)
{
  if (!selected_board) return 0;
  if(selected_board->family==112 && strlen(name)==6 && name[0]=='p' && (name[1]=='1' || name[1]=='2') && !strncmp(name+2,"gun",3) && (name[5]=='x' || name[5]=='y'))return 65535;
  if (selected_board->family == 12) return system12_analog_max(name);
#ifdef PCSX_SYSTEM_GNET
  if (selected_board->family == 103) return systemgnet_analog_max(name);
#endif
  return 0;
}

static int input_control_known(const char *name)
{
#ifdef PCSX_SYSTEM_GNET
  if (selected_board && selected_board->family == 103 && !strcmp(name, "jp1")) return 1;
#endif
  /* Validate against the selected source input profile while every control
   * is still released, before the CPU begins executing CSV events. */
  return strlen(name) < sizeof(((harness_input_event *)0)->control) &&
    selected_board && (input_analog_max(name) ||
      selected_board->set_input(name, 0) == 0);
}

static char *trim_field(char *s)
{
  char *end;
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
         end[-1] == '\r' || end[-1] == '\n')) *--end = 0;
  return s;
}

static int load_input_events(const char *path, harness_input_event **out,
                             size_t *count)
{
  FILE *f = fopen(path, "r");
  char *line = NULL;
  size_t linecap = 0, used = 0, cap = 0;
  harness_input_event *events = NULL;
  unsigned long long last_frame = 0;
  int have_frame = 0, failed = 0;
  ssize_t n;
  if (!f) { perror(path); return -1; }
  while ((n = getline(&line, &linecap, f)) >= 0) {
    char *frame_s, *control, *pressed_s, *end;
    unsigned long long frame;
    unsigned long pressed;
    (void)n;
    frame_s = trim_field(line);
    if (!*frame_s || *frame_s == '#') continue;
    control = strchr(frame_s, ',');
    if (!control) { failed = 1; break; }
    *control++ = 0;
    pressed_s = strchr(control, ',');
    if (!pressed_s) { failed = 1; break; }
    *pressed_s++ = 0;
    if (strchr(pressed_s, ',')) { failed = 1; break; }
    frame_s = trim_field(frame_s); control = trim_field(control);
    pressed_s = trim_field(pressed_s);
    if (!strcmp(frame_s, "frame") && !strcmp(control, "control") &&
        !strcmp(pressed_s, "pressed")) continue;
    errno = 0; frame = strtoull(frame_s, &end, 10);
    if (errno || end == frame_s || *end || frame > 216000 ||
        (have_frame && frame < last_frame) || !input_control_known(control)) {
      failed = 1; break;
    }
    errno = 0; pressed = strtoul(pressed_s, &end, 10);
    unsigned maximum = input_analog_max(control);
    if (errno || end == pressed_s || *end || pressed > (maximum ? maximum : 1)) { failed = 1; break; }
    if (used == cap) {
      size_t newcap = cap ? cap * 2 : 32;
      harness_input_event *next = realloc(events, newcap * sizeof(*events));
      if (!next) { failed = 1; break; }
      events = next; cap = newcap;
    }
    events[used].frame = frame;
    snprintf(events[used].control, sizeof(events[used].control), "%s", control);
    events[used].pressed = (int)pressed;
    used++; last_frame = frame; have_frame = 1;
  }
  if (ferror(f)) failed = 1;
  free(line);
  if (fclose(f)) failed = 1;
  if (failed) {
    fprintf(stderr, "%s: invalid inputs CSV (expected ordered frame,control,pressed rows)\n", path);
    free(events); return -1;
  }
  *out = events; *count = used;
  return 0;
}

static int apply_input_events(const harness_input_event *events, size_t count,
                              size_t *next, uint64_t frame)
{
  while (*next < count && events[*next].frame <= frame) {
    int error = selected_board->family==112 && input_analog_max(events[*next].control)
      ? systemzn_set_gun_axis(events[*next].control,events[*next].pressed)
      : selected_board->family == 12 && input_analog_max(events[*next].control)
      ? system12_set_analog(events[*next].control, events[*next].pressed)
#ifdef PCSX_SYSTEM_GNET
      : selected_board->family == 103 && input_analog_max(events[*next].control)
      ? systemgnet_set_analog(events[*next].control, events[*next].pressed)
#endif
      : selected_board->set_input(events[*next].control, events[*next].pressed);
    if (error) {
      fprintf(stderr, "invalid arcade control: %s\n", events[*next].control);
      return -1;
    }
    (*next)++;
  }
  return 0;
}
#endif

int main(int argc, char **argv)
{
  const char *exe = NULL, *board = NULL, *output = "frame.ppm";
  const char *inputs_path = NULL, *audio_path = NULL, *nvram_path = NULL;
#ifdef PCSX_SYSTEM12
  harness_input_event *input_events = NULL;
  size_t input_event_count = 0, next_input_event = 0;
#endif
  const char *ram_path = NULL;
  int jd_fixture=0;
  int nba_fixture=0;
  int tps_fixture = 0, fx1z_fixture = 0, raizing_fixture = 0, visco_fixture = 0, atlus_fixture = 0, cbaj_fixture = 0, primal_fixture = 0;
  const char *trace_path = NULL;
  FILE *trace = NULL;
  const char *snapshot_path = NULL;
  FILE *snapshots = NULL;
  const char *bus_trace_path = NULL;
  FILE *bus_trace_file = NULL;
  const char *edge_trace_path = NULL;
  int trace_shared_flags = 0;
#ifdef PCSX_SYSTEM12
  harness_bus_trace_bin *bus_trace_bins = NULL;
  int bus_trace_finished = 0;
  HarnessEdgeTrace edge_trace = {0};
#endif
  uint64_t snapshot_interval = 262144, snapshot_start = 262144, next_snapshot = 262144;
  unsigned int snapshot_count = 0;
  unsigned int frames = 3, start_frame, prev_cycle;
  uint64_t cycles = 0, cycle_limit;
  unsigned long trace_count = 0;
  uint64_t trace_start = 0;
  unsigned long blocks = 0, trace_blocks = 1000, max_blocks = 1000000000;
  int i, board_test = 0, audio_test_fixture = 0, verify = 0, result = 0, gpu_type1_test = 0;
  int gv_fixture = 0;
  int gnet_fixture = 0;
  int gnet_storage_fixture = 0;
  int gnet_storage_reopen = 0;
  const char *gnet_storage_fixture_chd = NULL;
  int board_load_result = 0;
  int board_family = 12;
  int icache = 0;
  int widescreen = 0, borderless = 0, gpu_thread = 0;
  /* Savestate checks: save at a frame and run on; or start from a state. */
  const char *save_state_path = NULL, *load_state_path = NULL;
  unsigned save_state_at = 0;
  int state_saved = 0;
  int use_drc = 0;
  int capture_gl = 0;
  int interactive = 0, gnet_jp1 = -1;
  int vblank_sync_only = 0;
  int source_step = 0;
  uint32_t slice_steps = 0;
  int slice_started = 0;
  int max_blocks_requested = 0, frames_requested = 0;
  uint64_t reset_at_cycle = 0;
  int reset_performed = 0;
#if defined(PCSX_NATIVE_VITA_PATH) && defined(PCSX_SYSTEM12)
  unsigned interactive_frame = 0;
  uint64_t interactive_started_ns = 0;
#if defined(PCSX_NATIVE_VITA_PATH) && defined(PCSX_SYSTEM12)
  char interactive_game[256] = "";
  const char *art_dir = NULL;
  char art_path[1024] = "";
  /* Scripted pauses, in order: each --pause-at starts one. */
  struct { unsigned at; const char *script, *shot; } pauses[8];
  int pause_count = 0, pause_next = 0;
  const char *pause_script = NULL, *pause_shot = NULL;
#endif
#endif
  const char *vram_path = NULL;
  struct timespec begin, end;
  /* These fixture selectors are parsed for a clear BOARD=0 diagnostic too. */
  (void)gnet_fixture;
  (void)board_load_result;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
    else if (!strcmp(argv[i], "--system-gv-fixture") && i + 1 < argc) {
      if (board) { usage(argv[0]); return 2; }
      board = argv[++i]; board_family = 102; gv_fixture = 1; board_test = 1;
    }
    else if (!strcmp(argv[i], "--system-gnet-storage-fixture") && i + 1 < argc) {
      if (board) { usage(argv[0]); return 2; }
      gnet_storage_fixture_chd = argv[++i];
      board = "<synthetic-gnet-storage-fixture>"; board_family = 103;
      gnet_fixture = gnet_storage_fixture = board_test = 1;
    }
    else if (!strcmp(argv[i], "--storage-reopen-check"))
      gnet_storage_reopen = 1;
    else if (!strcmp(argv[i], "--system-gnet-fixture")) {
      if (board) { usage(argv[0]); return 2; }
      board = "<synthetic-gnet-fixture>"; board_family = 103;
      gnet_fixture = 1; board_test = 1;
    }
    else if (!strcmp(argv[i], "--taito-fx1z-fixture") && i + 1 < argc) {
      if (board) { usage(argv[0]); return 2; }
      board = argv[++i]; board_family = 105; board_test = fx1z_fixture = 1;
    }
    else if (!strcmp(argv[i], "--raizing-zn-fixture") && i + 1 < argc) {
      if (board) { usage(argv[0]); return 2; }
      board = argv[++i]; board_family = 106; board_test = raizing_fixture = 1;
    }
    else if (!strcmp(argv[i], "--visco-zn-fixture") && i + 1 < argc) {
      if (board) { usage(argv[0]); return 2; }
      board = argv[++i]; board_family = 107; board_test = visco_fixture = 1;
    }
    else if (!strcmp(argv[i], "--atlus-zn-fixture") && i + 1 < argc) {
      if (board) { usage(argv[0]); return 2; }
      board = argv[++i]; board_family = 108; board_test = atlus_fixture = 1;
    }
    else if (!strcmp(argv[i], "--cbaj-tps-fixture") && i + 1 < argc) {
      if (board) { usage(argv[0]); return 2; }
      board = argv[++i]; board_family = 109; board_test = cbaj_fixture = 1;
    }
    else if (!strcmp(argv[i],"--jdredd-zn-fixture") && i+1<argc){
      if(board){usage(argv[0]);return 2;}board=argv[++i];board_family=112;board_test=jd_fixture=1;
    }
    else if (!strcmp(argv[i], "--nbajam-zn-fixture") && i + 1 < argc) {
      if(board){usage(argv[0]);return 2;}
      board=argv[++i];board_family=111;board_test=nba_fixture=1;
    }
    else if (!strcmp(argv[i], "--primal-zn-fixture") && i + 1 < argc) {
      if (board) { usage(argv[0]); return 2; }
      board = argv[++i]; board_family = 110; board_test = primal_fixture = 1;
    }
    else if (!strcmp(argv[i], "--tecmo-tps-fixture") && i + 1 < argc) {
      if (board) { usage(argv[0]); return 2; }
      board = argv[++i]; board_family = 104; board_test = tps_fixture = 1;
    }
    else if ((!strcmp(argv[i], "--system12") || !strcmp(argv[i], "--system11") ||
        !strcmp(argv[i], "--system10") || !strcmp(argv[i], "--capcom-zn") ||
        !strcmp(argv[i], "--tecmo-tps") || !strcmp(argv[i], "--raizing-zn") || !strcmp(argv[i], "--visco-zn") || !strcmp(argv[i], "--atlus-zn") || !strcmp(argv[i], "--cbaj-tps") || !strcmp(argv[i], "--primal-zn") || !strcmp(argv[i], "--nbajam-zn") || !strcmp(argv[i],"--jdredd-zn") ||
        !strcmp(argv[i], "--taito-fx1") || !strcmp(argv[i], "--taito-fx1z") || !strcmp(argv[i], "--system-gv") ||
        !strcmp(argv[i], "--system-gnet")) && i + 1 < argc) {
      if (board) { usage(argv[0]); return 2; }
      board_family = !strcmp(argv[i], "--system11") ? 11 :
        !strcmp(argv[i], "--system10") ? 10 : !strcmp(argv[i], "--capcom-zn") ? 100 :
        !strcmp(argv[i], "--taito-fx1") ? 101 : !strcmp(argv[i], "--system-gv") ? 102 :
        !strcmp(argv[i], "--system-gnet") ? 103 : !strcmp(argv[i], "--tecmo-tps") ? 104 : !strcmp(argv[i], "--taito-fx1z") ? 105 : !strcmp(argv[i], "--raizing-zn") ? 106 : !strcmp(argv[i], "--visco-zn") ? 107 : !strcmp(argv[i], "--atlus-zn") ? 108 : !strcmp(argv[i], "--cbaj-tps") ? 109 : !strcmp(argv[i], "--primal-zn") ? 110 : !strcmp(argv[i], "--nbajam-zn") ? 111 : !strcmp(argv[i],"--jdredd-zn") ? 112 : 12;
      board = argv[++i];
    }
    else if (!strcmp(argv[i], "--output") && i + 1 < argc) output = argv[++i];
    else if (!strcmp(argv[i], "--capture-gl")) capture_gl = 1;
    else if (!strcmp(argv[i], "--dump-ram") && i + 1 < argc) ram_path = argv[++i];
    else if (!strcmp(argv[i], "--dump-vram") && i + 1 < argc) vram_path = argv[++i];
    else if (!strcmp(argv[i], "--inputs") && i + 1 < argc) inputs_path = argv[++i];
    else if (!strcmp(argv[i], "--audio") && i + 1 < argc) audio_path = argv[++i];
#if defined(PCSX_NATIVE_VITA_PATH) && defined(PCSX_SYSTEM12)
    else if (!strcmp(argv[i], "--mixed-audio") && i + 1 < argc) interactive_mix_path = argv[++i];
#endif
    else if (!strcmp(argv[i], "--nvram") && i + 1 < argc) nvram_path = argv[++i];
    else if (!strcmp(argv[i], "--audio-test-fixture")) audio_test_fixture = 1;
    else if (!strcmp(argv[i], "--reset-at-cycle") && i + 1 < argc) {
      char *tail; unsigned long long n;
      errno = 0; n = strtoull(argv[++i], &tail, 10);
      if (errno || *tail || !n || reset_at_cycle) { usage(argv[0]); return 2; }
      reset_at_cycle = n;
    }
    else if (!strcmp(argv[i], "--drc")) use_drc = 1;
    else if (!strcmp(argv[i], "--interactive")) interactive = 1;
    else if (!strcmp(argv[i], "--gnet-jp1") && i + 1 < argc) {
      const char *value = argv[++i];
      if (strcmp(value,"on") && strcmp(value,"off")) { usage(argv[0]); return 2; }
      gnet_jp1 = !strcmp(value,"on");
    }
#if defined(PCSX_NATIVE_VITA_PATH) && defined(PCSX_SYSTEM12)
    /* Checks of the pause menu: open it at frame N, press the script's
     * buttons, save it to a PPM and exit. */
    /* Where the pause menu saves cabinet art: DIR/snap/<set>.png, the
     * arcade-shelf data folder (build/arcade-ui/data for the preview). */
    else if (!strcmp(argv[i], "--art-dir") && i + 1 < argc) art_dir = argv[++i];
    else if (!strcmp(argv[i], "--pause-at") && i + 1 < argc && pause_count < 8) {
      pauses[pause_count].at = (unsigned)atoi(argv[++i]);
      pauses[pause_count].script = pauses[pause_count].shot = NULL;
      pause_count++;
    }
    else if (!strcmp(argv[i], "--pause-script") && i + 1 < argc && pause_count)
      pauses[pause_count - 1].script = argv[++i];
    else if (!strcmp(argv[i], "--pause-shot") && i + 1 < argc && pause_count)
      pauses[pause_count - 1].shot = argv[++i];
#endif
    else if (!strcmp(argv[i], "--system12-vblank-sync-only")) vblank_sync_only = 1;
    else if (!strcmp(argv[i], "--system12-source-step")) source_step = 1;
    else if (!strcmp(argv[i], "--frontend-loop")) frontend_loop = 1;
    else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_path = argv[++i];
    else if (!strcmp(argv[i], "--cycle-snapshots") && i + 1 < argc) snapshot_path = argv[++i];
    else if (!strcmp(argv[i], "--board-bus-trace") && i + 1 < argc) bus_trace_path = argv[++i];
    else if (!strcmp(argv[i], "--system12-edge-trace") && i + 1 < argc) edge_trace_path = argv[++i];
    else if (!strcmp(argv[i], "--system12-trace-shared-writes")) trace_shared_flags |= 1;
    else if (!strcmp(argv[i], "--system12-trace-shared-accesses")) trace_shared_flags |= 3;
    else if ((!strcmp(argv[i], "--trace-blocks") || !strcmp(argv[i], "--max-blocks")) && i + 1 < argc) {
      int tracing = !strcmp(argv[i], "--trace-blocks");
      char *tail; unsigned long n;
      errno = 0; n = strtoul(argv[++i], &tail, 10);
      if (errno || *tail || n == 0 || n > (tracing ? 1000000UL : 100000000000UL)) {
        usage(argv[0]); return 2;
      }
      if (tracing) trace_blocks = n;
      else {
        max_blocks = n;
        max_blocks_requested = 1;
      }
    }
    else if (!strcmp(argv[i], "--snapshot-interval") && i + 1 < argc) {
      char *tail; unsigned long long n;
      errno = 0; n = strtoull(argv[++i], &tail, 10);
      if (errno || *tail || n == 0 || n > 0x100000000ULL) { usage(argv[0]); return 2; }
      snapshot_interval = n;
    }
    else if ((!strcmp(argv[i], "--snapshot-start") || !strcmp(argv[i], "--trace-start")) && i + 1 < argc) {
      int tracing = !strcmp(argv[i], "--trace-start");
      char *tail; unsigned long long n;
      errno = 0; n = strtoull(argv[++i], &tail, 10);
      if (errno || *tail) { usage(argv[0]); return 2; }
      if (tracing) trace_start = (uint64_t)n;
      else snapshot_start = (uint64_t)n;
    }
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
      char *tail; unsigned long n = strtoul(argv[++i], &tail, 10);
      if (*tail || n == 0 || n > 216000) { usage(argv[0]); return 2; }
      frames = n; frames_requested = 1;
    }
    else if (!strcmp(argv[i], "--gpu-type1-test") || !strcmp(argv[i], "--gpu-type1-test-1m")) {
      if (gpu_type1_test) { usage(argv[0]); return 2; }
      gpu_type1_test = !strcmp(argv[i], "--gpu-type1-test-1m") ? 2 : 1;
    }
    else if (!strcmp(argv[i], "--board-test")) board_test = 1;
    else if (!strcmp(argv[i], "--verify-smoke")) verify = 1;
    else if (!strcmp(argv[i], "--icache")) icache = 1;
    else if (!strcmp(argv[i], "--save-state-at") && i + 2 < argc) {
      save_state_at = (unsigned)atoi(argv[++i]);
      save_state_path = argv[++i];
    }
    else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) load_state_path = argv[++i];
    /* The Vita's 16:9 widescreen hook: the GTE squeezes the scene and the
     * vitagl-like renderer widens frames the GPU drew. */
    else if (!strcmp(argv[i], "--widescreen")) widescreen = 1;
    else if (!strcmp(argv[i], "--borderless")) borderless = 1;
    else if (!strcmp(argv[i], "--gpu-thread")) gpu_thread = 1;
    else { usage(argv[0]); return 2; }
  }
  if (source_step) {
#ifdef PCSX_SYSTEM12
    if (!board || board_family != 12 || board_test || interactive || use_drc || gpu_type1_test) {
      fprintf(stderr, "--system12-source-step requires the System12 interpreter execution profile\n");
      return 2;
    }
#else
    fprintf(stderr, "Rebuild with BOARD=1 for System12 source instruction stepping.\n");
    return 2;
#endif
  }
  if (frontend_loop && (source_step || board_test || interactive || trace_path ||
      snapshot_path || reset_at_cycle || gpu_type1_test)) {
    fprintf(stderr, "--frontend-loop runs whole frames like libretro; it excludes "
      "source step, board tests, interactive, traces, snapshots and resets\n");
    return 2;
  }
  if (vblank_sync_only) {
#ifdef PCSX_SYSTEM12
    if (!board || board_family != 12 || board_test || interactive) {
      fprintf(stderr, "--system12-vblank-sync-only requires a System12 execution profile\n");
      return 2;
    }
#else
    fprintf(stderr, "Rebuild with BOARD=1 for System12 cadence diagnostics.\n");
    return 2;
#endif
  }
  if (interactive) {
#if defined(PCSX_NATIVE_VITA_PATH) && defined(PCSX_SYSTEM12)
    if (!board || (board_family != 12 && board_family != 103 && board_family != 104 && board_family != 105 && board_family != 106 && board_family != 107 && board_family != 108 && board_family != 109 && board_family != 110 && board_family != 111 && board_family != 112) || board_test) {
      fprintf(stderr, "interactive mode requires System12, G-Net, Tecmo TPS, FX1Z, Raizing ZN, Visco ZN, Atlus ZN, CBaj TPS or Primal ZN without board-test options\n");
      return 2;
    }
    if (!frames_requested) frames = UINT32_MAX / 4;
    if (!max_blocks_requested) max_blocks = ~0UL;
#else
    fprintf(stderr, "interactive mode requires the vitagl-like Mac renderer build\n");
    return 2;
#endif
  }
  if (gnet_jp1 >= 0 && (!board || board_family != 103 || board_test)) {
    fprintf(stderr,"--gnet-jp1 requires an authentic G-Net profile\n"); return 2;
  }
#if defined(PCSX_NATIVE_VITA_PATH) && defined(PCSX_SYSTEM12)
  if (interactive_mix_path && (!interactive || (board_family != 103 && board_family != 104 && board_family != 105 && board_family != 106 && board_family != 107 && board_family != 108 && board_family != 109 && board_family != 110 && board_family != 111 && board_family != 112) ||
      !strcmp(interactive_mix_path, output) ||
      (audio_path && !strcmp(interactive_mix_path, audio_path)) ||
      (nvram_path && !strcmp(interactive_mix_path, nvram_path)) ||
      (inputs_path && !strcmp(interactive_mix_path, inputs_path)))) {
    fprintf(stderr, "--mixed-audio requires interactive G-Net, Tecmo TPS, FX1Z, Raizing ZN, Visco ZN, Atlus ZN, CBaj TPS or Primal ZN and a separate output path\n"); return 2;
  }
#endif
  if (ram_path) {
    const char *paths[] = {exe, output, inputs_path, audio_path, nvram_path,
                          trace_path, snapshot_path, bus_trace_path, edge_trace_path, vram_path};
    for (unsigned path_i = 0; path_i < sizeof(paths) / sizeof(paths[0]); path_i++)
      if (paths[path_i] && !strcmp(ram_path, paths[path_i])) {
        fprintf(stderr, "--dump-ram must use a separate output path\n"); return 2;
      }
  }
  next_snapshot = snapshot_start ? snapshot_start : snapshot_interval;
  if (vram_path) {
    const char *paths[] = {exe, output, inputs_path, audio_path, nvram_path,
                          trace_path, snapshot_path, bus_trace_path, edge_trace_path};
    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
      if (paths[i] && !strcmp(vram_path, paths[i])) {
        fprintf(stderr, "--dump-vram must use a separate output path\n");
        return 2;
      }
    }
  }
#ifdef PCSX_SYSTEM12
  /* Explicit NBA reset vectors run their guest directly, without the unrelated
   * GPU/SPU preflight that --board-test otherwise injects into fixture RAM. */
  if(nba_fixture && reset_at_cycle)board_test=0;
#endif
  if ((!exe && !board && !gpu_type1_test) || (exe && board) ||
      (gpu_type1_test && (exe || board || verify || trace_path)) || (board_test && !board) ||
      ((inputs_path || audio_path || audio_test_fixture || nvram_path) && !board) ||
      (snapshot_path && !board) ||
      (bus_trace_path && (!board || board_family != 12)) ||
      (edge_trace_path && (!board || board_family != 12 || board_test)) ||
      (trace_shared_flags && !edge_trace_path) ||
      (reset_at_cycle && (!board || board_test || interactive)) ||
      (inputs_path && !strcmp(inputs_path, output)) ||
      (trace_path && inputs_path && !strcmp(trace_path, inputs_path)) ||
      (nvram_path && ((!strcmp(nvram_path, output)) ||
        (inputs_path && !strcmp(nvram_path, inputs_path)) ||
        (audio_path && !strcmp(nvram_path, audio_path)) ||
        (trace_path && !strcmp(nvram_path, trace_path)) ||
        (snapshot_path && !strcmp(nvram_path, snapshot_path)) ||
        (bus_trace_path && !strcmp(nvram_path, bus_trace_path)))) ||
      (snapshot_path && ((trace_path && !strcmp(snapshot_path, trace_path)) ||
        (inputs_path && !strcmp(snapshot_path, inputs_path)) ||
        (audio_path && !strcmp(snapshot_path, audio_path)) ||
        !strcmp(snapshot_path, output))) ||
      (bus_trace_path && ((trace_path && !strcmp(bus_trace_path, trace_path)) ||
        (snapshot_path && !strcmp(bus_trace_path, snapshot_path)) ||
        (inputs_path && !strcmp(bus_trace_path, inputs_path)) ||
        (audio_path && !strcmp(bus_trace_path, audio_path)) ||
        !strcmp(bus_trace_path, output))) ||
      (audio_test_fixture && (!board_test || !audio_path || board_family != 12))) {
    usage(argv[0]); return 2;
  }
  if (edge_trace_path) {
    const char *paths[] = {exe, output, inputs_path, audio_path, nvram_path,
                          trace_path, snapshot_path, bus_trace_path};
    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
      if (paths[i] && !strcmp(edge_trace_path, paths[i])) {
        fprintf(stderr, "--system12-edge-trace must use a separate output path\n");
        return 2;
      }
    }
#ifndef PCSX_SYSTEM12
    fprintf(stderr, "Rebuild with BOARD=1 for System12 edge tracing.\n");
    return 2;
#endif
  }
#if !defined(GPU_VITA_CTR)
  if (capture_gl) {
    fprintf(stderr, "--capture-gl requires GPU=vitagl-like\n");
    return 2;
  }
#endif
  if (fx1z_fixture && (exe || inputs_path || audio_path || interactive || use_drc)) { usage(argv[0]); return 2; }
  if (cbaj_fixture && (exe || inputs_path || audio_path || nvram_path || interactive || use_drc)) { usage(argv[0]); return 2; }
  if(jd_fixture && (exe || inputs_path || audio_path || nvram_path || interactive || use_drc)){usage(argv[0]);return 2;}
  if (nba_fixture && (exe || inputs_path || audio_path || nvram_path || interactive || use_drc)) { usage(argv[0]); return 2; }
  if (primal_fixture && (exe || inputs_path || audio_path || nvram_path || interactive || use_drc)) { usage(argv[0]); return 2; }
  if (atlus_fixture && (exe || inputs_path || audio_path || nvram_path || interactive || use_drc)) { usage(argv[0]); return 2; }
  if (visco_fixture && (exe || inputs_path || audio_path || nvram_path || interactive || use_drc)) { usage(argv[0]); return 2; }
  if (raizing_fixture && (exe || inputs_path || audio_path || nvram_path || interactive || use_drc)) { usage(argv[0]); return 2; }
  if (tps_fixture && (exe || inputs_path || audio_path || nvram_path || interactive || use_drc)) {
    usage(argv[0]); return 2;
  }
  if (gv_fixture && (!board || board_family != 102 || !board_test || exe ||
      inputs_path || audio_path || nvram_path || snapshot_path || bus_trace_path ||
      trace_path || verify || gpu_type1_test)) {
    usage(argv[0]); return 2;
  }
#ifndef PCSX_SYSTEM_GV
  if (gv_fixture || (board && board_family == 102)) {
    fprintf(stderr, "Rebuild with BOARD=1 and HAVE_CHD=1 for Konami GV.\n"); return 2;
  }
#endif
  if (trace_path) {
    trace = fopen(trace_path, "w");
    if (!trace) { perror(trace_path); return 1; }
    fputs("block,cycle,pc,opcode", trace);
    for (i = 0; i < 32; i++) fprintf(trace, ",r%d", i);
    fputc('\n', trace);
  }
  if (snapshot_path) {
    snapshots = fopen(snapshot_path, "w");
    if (!snapshots) { perror(snapshot_path); if (trace) fclose(trace); return 1; }
    fputs("cycles,frame,pc,opcode,sr,cause,interrupt,i_stat,i_mask,ram_vector80,"
      "ram_d2190,ram_d2194,ram_d2198,ram_d219c,ram_d21a0,ram_d21a4,"
      "ram_d21a8,ram_d21ac,ram_d21b0,ram_d21b4,ram_d21b8,ram_d21bc,"
      "ram_d21c0,ram_d21c4,ram_d21c8,ram_d21cc,ram_d21d0,ram_d21d4,"
      "ram_d21d8,ram_d21dc", snapshots);
    for (i = 0; i < 32; i++) fprintf(snapshots, ",r%d", i);
    fputc('\n', snapshots);
  }
  if (bus_trace_path) {
#ifdef PCSX_SYSTEM12
    bus_trace_bins = calloc(BOARD_BUS_TRACE_MAX_BINS, sizeof(*bus_trace_bins));
    bus_trace_file = fopen(bus_trace_path, "w");
    if (!bus_trace_bins || !bus_trace_file) {
      perror(bus_trace_path);
      free(bus_trace_bins);
      if (trace) fclose(trace);
      if (snapshots) fclose(snapshots);
      if (bus_trace_file) fclose(bus_trace_file);
      return 1;
    }
    fprintf(stderr, "System12 bus trace: %u-cycle bins, %u bins (%" PRIu64
      "-cycle horizon); reported_pc is block state, not instruction-exact under DRC\n",
      BOARD_BUS_TRACE_BIN_CYCLES, BOARD_BUS_TRACE_MAX_BINS,
      (uint64_t)BOARD_BUS_TRACE_BIN_CYCLES * BOARD_BUS_TRACE_MAX_BINS);
#else
    fprintf(stderr, "Rebuild with BOARD=1 for System12 bus tracing.\n");
    return 2;
#endif
  }
  memset(&Config, 0, sizeof(Config));
  Config.HLE = 1;
  Config.Cpu = use_drc ? CPU_DYNAREC : CPU_INTERPRETER;
#ifdef DRC_DISABLE
  if (use_drc) { fprintf(stderr, "Rebuild with DRC=1 for --drc.\n"); return 2; }
#endif
  if (use_drc && board && board_family != 12 && board_family != 102
#ifdef PCSX_SYSTEM_GNET
      && board_family != 103
#endif
      ) {
    fprintf(stderr, "Arcade DRC validation currently covers System 12 only.\n"); return 2;
  }
  Config.icache_emulation = icache;
  if (widescreen && use_drc) {
    /* Only the interpreter and the 32-bit ARM recompiler (the Vita's) call the hook. */
    fprintf(stderr, "--widescreen needs the interpreter on macOS\n");
    return 2;
  }
  gteSetWidescreenHack(widescreen);
#ifdef GPU_VITA_CTR
  {
    extern void VitaCtrNativeRenderer_SetAspectMode(int mode);
    if (widescreen) VitaCtrNativeRenderer_SetAspectMode(2);
  }
#endif
  Config.cycle_multiplier = CYCLE_MULT_DEFAULT;
  Config.PreciseExceptions = source_step;
  /* The MAME source charges no MULT/DIV/GTE latency. */
  Config.DisableStalls = source_step;
  Config.PsxType = PSX_TYPE_NTSC;
  strcpy(Config.Gpu, "builtin_gpu"); strcpy(Config.Spu, "builtin_spu");
  strcpy(Config.Bios[0], "HLE"); strcpy(Config.Bios[1], "HLE");
  strcpy(Config.Bios[2], "HLE");
  spu_config.iUseThread = 0;
#ifdef PCSX_SYSTEM12
  if (board) {
    for (i = 0; i < (int)(sizeof(boards) / sizeof(boards[0])); i++)
      if (boards[i].family == board_family) selected_board = &boards[i];
    if (!selected_board || (audio_path && !selected_board->open_audio) ||
        (nvram_path && !selected_board->import_nvram)) {
      fprintf(stderr, "Selected board does not provide requested audio/NVRAM output\n");
      if (trace) fclose(trace);
      return 2;
    }
    if (gv_fixture) {
#ifdef PCSX_SYSTEM_GV
      board_load_result = systemgv_test_load(board);
#else
      board_load_result = -1;
#endif
    } else if (gnet_fixture) {
#ifdef PCSX_SYSTEM_GNET
      memset(&gnet_audio, 0, sizeof(gnet_audio));
      gnet_audio.finite = gnet_audio.monotonic = 1;
      /* Register before fixture load and the subsequent psxReset() so this
       * exercises sink persistence through the normal board reset path. */
      systemgnet_set_audio_output(harness_gnet_audio_frame, &gnet_audio);
      board_load_result = gnet_storage_fixture
        ? systemgnet_test_load_storage_fixture(gnet_storage_fixture_chd)
        : systemgnet_test_load();
#else
      board_load_result = -1;
#endif
    } else if (tps_fixture) board_load_result = systemzn_test_load_tps(board);
    else if (cbaj_fixture) board_load_result = systemzn_test_load_cbaj(board);
    else if(jd_fixture)board_load_result=systemzn_test_load_jdredd(board);
    else if (nba_fixture) board_load_result = systemzn_test_load_nbajam(board);
    else if (primal_fixture) board_load_result = systemzn_test_load_primal(board);
    else if (atlus_fixture) board_load_result = systemzn_test_load_atlus(board);
    else if (visco_fixture) board_load_result = systemzn_test_load_visco(board);
    else if (raizing_fixture) board_load_result = systemzn_test_load_raizing(board);
    else if (fx1z_fixture) board_load_result = systemtaito_test_load_fx1z(board);
    else board_load_result = selected_board->load(board);
    if (board_load_result) {
      if (trace) fclose(trace); return 1;
    }
  }
  if (nvram_path && load_nvram(nvram_path)) {
    selected_board->unload();
    if (trace) fclose(trace);
    return 1;
  }
  if (inputs_path && load_input_events(inputs_path, &input_events, &input_event_count)) {
    selected_board->unload(); return 2;
  }
#else
  if (board) { fprintf(stderr, "Rebuild with BOARD=1 for arcade boards.\n"); return 2; }
#endif
  if (gpu_type1_test && (gpu_set_model(1) || gpu_set_vram_height(gpu_type1_test == 2 ? 512 : 1024))) {
    fprintf(stderr, "Rebuild with BOARD=1 for type-1 GPU testing.\n"); return 2;
  }
  if (psxInit() || LoadPlugins()) return 1;
  /* Screen Borders = Borderless, the Vita app's default. */
  pl_rearmed_cbs.screen_centering_type = borderless ? C_BORDERLESS : C_AUTO;
  /* gpulib's GPU thread (GPU_THREAD=1 builds), as on the Vita. */
  pl_rearmed_cbs.thread_rendering = gpu_thread;
  pl_rearmed_cbs.pl_vout_open = video_open;
  pl_rearmed_cbs.pl_vout_close = video_close;
  pl_rearmed_cbs.pl_vout_set_mode = video_mode;
  pl_rearmed_cbs.pl_vout_flip = video_flip;
  pl_rearmed_cbs.mmap = gpu_alloc;
  pl_rearmed_cbs.munmap = gpu_free;
  pl_rearmed_cbs.gpu_frame_count = &frame_counter;
  pl_rearmed_cbs.gpu_hcnt = &hSyncCount;
  pl_rearmed_cbs.gpu_state_change = gpu_state_change;
  GPUrearmedCallbacks(&pl_rearmed_cbs);
  SPU_registerCallback(SPUirq);
  SPU_registerScheduleCb(SPUschedule);
  if (SPU_open() || GPU_open(NULL, NULL, NULL)) return 1;
#ifdef PCSX_SYSTEM_GNET
  if (gnet_jp1 >= 0 && systemgnet_set_input("jp1",gnet_jp1)) return 1;
#endif
#if defined(GPU_VITA_CTR) && defined(PCSX_SYSTEM_GNET)
  if (selected_board && selected_board->family == 103) {
    extern void VitaCtrNativeRenderer_SetPresentationRotation(unsigned degrees);
    VitaCtrNativeRenderer_SetPresentationRotation(systemgnet_rotation());
    fprintf(stderr, "G-Net presentation rotation=%u\n", systemgnet_rotation());
  }
#endif
#if defined(GPU_VITA_CTR) && defined(PCSX_SYSTEM12)
  if (selected_board && (selected_board->family == 106 || selected_board->family == 107)) {
    extern void VitaCtrNativeRenderer_SetPresentationRotation(unsigned degrees);
    VitaCtrNativeRenderer_SetPresentationRotation(systemzn_rotation());
    fprintf(stderr, "%s presentation rotation=%u\n", selected_board->name, systemzn_rotation());
  }
#endif
  psxReset();
#ifdef PCSX_SYSTEM12
  if (gpu_type1_test) {
    extern int harness_gpu_type1_selftest(void);
    result = harness_gpu_type1_selftest();
    GPU_close(); SPU_close(); psxShutdown(); ReleasePlugins();
    return result ? 1 : 0;
  }
  if (board_test) {
    extern int harness_gpu_vram_selftest(void);
    extern int harness_gpu_type1_selftest(void);
    if (selected_board->selftest() || (gpu_get_model() == 1 ? harness_gpu_type1_selftest() :
        harness_gpu_vram_selftest())) result = 1;
    if ((board_family == 101 || board_family == 102 || board_family == 104 || board_family == 105 || board_family == 106 || board_family == 107 || board_family == 108 || board_family == 109 || board_family == 110 || board_family == 111 || board_family == 112) &&
        harness_source_spu_dma_fixture(selected_board->name, 33868800, 24000)) result = 1;
  }
#ifdef PCSX_SYSTEM_GV
  if (gv_fixture) {
    unsigned step;
    systemgv_set_input("p1button1", 1);
    for (step = 0; step < (use_drc ? 1024u : 600u); step++) {
      if (use_drc)
        psxCpu->ExecuteBlock(&psxRegs, EXEC_CALLER_BOOT);
      else {
        psxRegs.branchSeen = 0;
        execI(&psxRegs);
      }
    }
    if (systemgv_sync(psxRegs.cycle) < 0) result = 1;
    if (psxMemRead32(&psxRegs, 0x80000100) != 0xffffffefu ||
        (psxHu32(0x10d8) & 0x01000000u) != 0 ||
        systemgv_dma_pending_bytes() != 1048576 ||
        !(psxHu32(0x1070) & (1u << 10))) {
      fprintf(stderr, "SystemGV MIPS fixture: FAIL guest=%08x CHCR=%08x pending=%" PRIu64 "\n",
        psxMemRead32(&psxRegs, 0x80000100), psxHu32(0x10d8),
        systemgv_dma_pending_bytes());
      result = 1;
    } else fprintf(stderr, "SystemGV MIPS fixture: PASS cpu=%s guest_input=%08x DMA5_pending=1048576 CHCR_completed=1 IRQ10=1\n",
        use_drc ? "drc" : "interpreter",
        psxMemRead32(&psxRegs, 0x80000100));
    systemgv_report(stderr);
    if (systemgv_test_dma_write()) result = 1;
    GPU_close(); SPU_close(); psxShutdown(); ReleasePlugins();
    systemgv_unload();
    return result;
  }
#endif
#ifdef PCSX_SYSTEM_GNET
  if (gnet_fixture) {
    unsigned step;
    uint64_t captured_audio_frames;
    if (gnet_storage_fixture) {
      psxRegs.cycle += 1000000;
      if (systemgnet_sync(psxRegs.cycle)) result = 1;
    }
    for (step = 0; step < (gnet_storage_fixture ?
        (use_drc ? 10000u : 6000u) : (use_drc ? 1024u : 600u)); step++) {
      if (use_drc)
        psxCpu->ExecuteBlock(&psxRegs, EXEC_CALLER_BOOT);
      else {
        psxRegs.branchSeen = 0;
        execI(&psxRegs);
      }
    }
    if (gnet_storage_fixture) {
      uint32_t locked = psxMemRead32(&psxRegs, 0x80000100);
      uint32_t identify_first = psxMemRead32(&psxRegs, 0x80000104);
      uint32_t last = psxMemRead32(&psxRegs, 0x80000108);
      uint32_t sector_first = psxMemRead32(&psxRegs, 0x80000110);
      uint32_t signature = psxMemRead32(&psxRegs, 0x8000010c);
      uint32_t expected_sector = gnet_storage_reopen ? 0x5aa5 : 0x1407;
      psxRegs.cycle += 10000000;
      if (systemgnet_sync(psxRegs.cycle) || locked != 0 ||
          signature != 0x4750 ||
          (identify_first & 0xffff) != 0x848a ||
          (sector_first & 0xffff) != expected_sector) {
        fprintf(stderr, "SystemGNet storage MIPS fixture: FAIL cpu=%s locked=%08x identify_first=%08x identify_last=%08x sector_first=%08x signature=%08x\n",
          use_drc ? "drc" : "interpreter", locked, identify_first, last,
          sector_first, signature);
        result = 1;
      } else fprintf(stderr, "SystemGNet storage MIPS fixture: PASS cpu=%s type1_unlock=1 ata_identify_first=%04x ata_pio_sector_first=%04x identify_last=%04x overlay_write=1 reopen=%d\n",
          use_drc ? "drc" : "interpreter", identify_first & 0xffff,
          sector_first & 0xffff,
          last, gnet_storage_reopen);
      systemgnet_report(stderr);
      systemgnet_reset();
      {
        uint32_t locked_after_reset = 0;
        if (systemgnet_read(0x1f200402, 2, &locked_after_reset) != 1 ||
            locked_after_reset != 1) {
          fprintf(stderr, "SystemGNet storage reset fixture: FAIL locked=%08x\n",
            locked_after_reset);
          result = 1;
        } else fprintf(stderr, "SystemGNet storage reset fixture: PASS type1_relocked=1\n");
      }
      systemgnet_unload();
      GPU_close(); SPU_close(); psxShutdown(); ReleasePlugins();
      return result;
    }
    if (psxMemRead32(&psxRegs, 0x80000100) != 2 ||
        psxMemRead32(&psxRegs, 0x80000104) != 0x1234) {
      fprintf(stderr, "SystemGNet MIPS fixture: FAIL const=%08x bankflash=%08x\n",
        psxMemRead32(&psxRegs, 0x80000100), psxMemRead32(&psxRegs, 0x80000104));
      result = 1;
    } else fprintf(stderr, "SystemGNet MIPS fixture: PASS cpu=%s const=2 bank1_flash=1234\n",
        use_drc ? "drc" : "interpreter");
    /* The guest fixture's memory accesses advance the board as usual. Give
     * the synthetic Zoom firmware a bounded extra interval to emit PCM. */
    for (step = 0; step < 20; step++) {
      psxRegs.cycle += 4096;
      if (systemgnet_sync(psxRegs.cycle)) result = 1;
    }
    if (gnet_audio.frames < 4 || !gnet_audio.finite ||
        !gnet_audio.monotonic || !gnet_audio.nonzero) {
      fprintf(stderr, "SystemGNet PCM fixture: FAIL frames=%" PRIu64
          " finite=%d monotonic=%d nonzero=%d\n", gnet_audio.frames,
          gnet_audio.finite, gnet_audio.monotonic, gnet_audio.nonzero);
      result = 1;
    } else fprintf(stderr, "SystemGNet PCM fixture: PASS frames=%" PRIu64
        " finite=1 monotonic=1 nonzero=1\n", gnet_audio.frames);
    captured_audio_frames = gnet_audio.frames;
    systemgnet_set_audio_output(NULL, NULL);
    psxRegs.cycle += 4096;
    if (systemgnet_sync(psxRegs.cycle) || gnet_audio.frames != captured_audio_frames) {
      fprintf(stderr, "SystemGNet PCM sink disable: FAIL frames=%" PRIu64
          " expected=%" PRIu64 "\n", gnet_audio.frames, captured_audio_frames);
      result = 1;
    } else fprintf(stderr, "SystemGNet PCM sink disable: PASS\n");
    systemgnet_report(stderr);
    if (harness_gnet_spu_dma_fixture()) result = 1;
    /* The MB3773 callback defers the reset; MMIO remains usable until the
     * frontend reaches its safe CPU boundary and acknowledges it. */
    if (systemgnet_write(0x1fb40000, 1, 0x10) != 1) result = 1;
    psxRegs.cycle += 5u * PSXCLK;
    uint32_t reset_control = 0;
    if (systemgnet_sync(psxRegs.cycle) <= 0 || !systemgnet_reset_requested() ||
        systemgnet_read(0x1fb40000, 2, &reset_control) != 1) {
      fprintf(stderr, "SystemGNet watchdog reset: FAIL\n"); result = 1;
    } else {
      systemgnet_reset();
      if (systemgnet_reset_requested() ||
          systemgnet_read(0x1fb40000, 2, &reset_control) != 1 || reset_control != 0xff10) {
        fprintf(stderr, "SystemGNet watchdog reset: FAIL\n"); result = 1;
      } else fprintf(stderr, "SystemGNet watchdog reset: PASS deferred=1 control=10\n");
    }
    GPU_close(); SPU_close(); psxShutdown(); ReleasePlugins();
    systemgnet_unload();
    return result;
  }
#endif
#endif
  if (!result && exe && Load(exe)) result = 1;
  signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
#if defined(PCSX_NATIVE_VITA_PATH) && defined(PCSX_SYSTEM12)
  if (interactive) {
    interactive_replay = inputs_path != NULL;
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    refresh_interactive_gamepads();
    interactive_started_ns = SDL_GetTicksNS();
    interactive_frame = frame_counter;
    if (open_interactive_audio())
      fprintf(stderr, "Interactive: no audio (%s), pacing at 60 Hz\n", SDL_GetError());
    if (interactive_mix_path && !interactive_audio) result = 1;
    interactive_title(board, interactive_game, sizeof(interactive_game));
    if (art_dir) {
      const system12_romset_info *set = system12_romset_is_zip(board) ? system12_romset_identify(board) : NULL;
      if (set) snprintf(art_path, sizeof(art_path), "%s/snap/%s.png", art_dir, set->name);
    }
    if (selected_board->family==110)
      fprintf(stderr,"Primal controls: 5/6 coin, 1/2 Start, P1 arrows + A/S/D/Space/F/H, P2 I/K/J/L + Z/X/C/V/B/N, gamepad face buttons/shoulders, Escape pause\n");
    else if (selected_board->family==112)
      fprintf(stderr,"Judge controls: 5/6 coin, 1/2 Start, mouse aim, left click/A trigger, right click/S second button, shoot outside the viewport to reload, gamepad stick aim, Escape pause\n");
    else fprintf(stderr, "Interactive: 5/6 coin, 1/2 start, arrows/JL move or steer, A/S/D buttons, G-Net mouse/gamepad analog, Escape pause\n");
  }
#endif
  if (!result && load_state_path) {
    if (LoadState(load_state_path)) {
      fprintf(stderr, "cannot load state %s\n", load_state_path);
      result = 1;
    }
    else {
      fprintf(stderr, "Loaded state %s: frame=%u cycle=%u\n", load_state_path, frame_counter, psxRegs.cycle);
      /* --save-state-at 0 with a load: the state again at once, for a round-trip check. */
      if (save_state_path && save_state_at == 0) {
        state_saved = 1;
        if (SaveState(save_state_path)) { fprintf(stderr, "cannot save state %s\n", save_state_path); result = 1; }
        else fprintf(stderr, "Saved state %s right after the load\n", save_state_path);
      }
    }
  }
  start_frame = frame_counter; prev_cycle = psxRegs.cycle;
  if (harness_configure_gl_capture(capture_gl, start_frame + frames - 1)) {
    fprintf(stderr, "failed to configure GL capture\n");
    result = 1;
  }
#ifdef PCSX_SYSTEM12
  if (board && audio_test_fixture) system12_audio_test_fixture();
  if (board && audio_path && selected_board->open_audio(audio_path)) {
    fprintf(stderr, "cannot open WAV output: %s\n", audio_path);
    result = 1;
  }
  if (board && !result && apply_input_events(input_events, input_event_count,
      &next_input_event, 0)) result = 1;
#endif
#ifdef PCSX_SYSTEM12
  if (bus_trace_bins)
    system12_set_bus_trace(harness_system12_bus_event, bus_trace_bins);
  if (edge_trace_path && !result) {
    edge_trace.file = fopen(edge_trace_path, "w");
    if (!edge_trace.file) { perror(edge_trace_path); result = 1; }
    else {
      fputs("sequence,event,value,main_cycle,synced_main_cycle,h8_cycle,h8_budget,address,width\n",
        edge_trace.file);
      system12_set_edge_trace(harness_system12_edge_event, &edge_trace);
      system12_set_edge_trace_shared(trace_shared_flags);
    }
  }
#endif
  cycle_limit = (uint64_t)PSXCLK * frames / 30 + PSXCLK;
  clock_gettime(CLOCK_MONOTONIC, &begin);
  while (!result && !interrupted && frame_counter - start_frame < frames) {
    unsigned int steps = 0;
#if defined(PCSX_NATIVE_VITA_PATH) && defined(PCSX_SYSTEM12)
    if (interactive) {
      if (interactive_audio_error || interactive_mix.error) { result = 1; break; }
      /* At 50 MHz a whole video frame exceeds dfsound's 914-sample work
       * buffer. Service silent SPU intervals as well as its IRQ/MMIO paths,
       * so Zoom and SPU retain the same source-clock epoch. */
      if ((interactive_mixing || source_spu_speaker_board()) && (u32)(psxRegs.cycle - interactive_spu_cycle) >= 256u * 768u) {
        if (SPU_async) SPU_async(psxRegs.cycle, 0);
        interactive_spu_cycle = psxRegs.cycle;
      }
      if ((blocks & 255) == 0 || frame_counter != interactive_frame)
        poll_interactive_input();
      if (pause_next < pause_count && !pause_requested && frame_counter - start_frame >= pauses[pause_next].at) {
        pause_script = pauses[pause_next].script;
        pause_shot = pauses[pause_next].shot;
        pause_next++;
        pause_requested = 1;
        pcsx_menu_backdrop_request = 1;
      }
      /* A request waits for the next present, which keeps the frame for the
       * menu and clears pcsx_menu_backdrop_request. */
      if (pause_requested && !pcsx_menu_backdrop_request) {
        /* Between frames: the menu draws over the one just presented. */
        int choice;
        pause_requested = 0;
        release_interactive_input();
        if (interactive_audio) SDL_PauseAudioStreamDevice(interactive_audio);
        choice = mac_pause_run(interactive_game, pause_script, pause_shot, art_path[0] ? art_path : NULL);
        pause_script = pause_shot = NULL;
        if (choice == MAC_PAUSE_CLOSE) interrupted = 1;
        if (choice == MAC_PAUSE_RESTART) {
          unsigned int elapsed_frames = frame_counter - start_frame;
          psxReset();
          if (interactive_mixing) {
            if(selected_board->family!=111)arcade_mix_reset(&interactive_mix); interactive_spu_cycle = psxRegs.cycle;
          }
          start_frame = frame_counter - elapsed_frames;
          prev_cycle = psxRegs.cycle;
          slice_steps = 0; slice_started = 0;
        }
        if (interactive_audio) {
          SDL_ClearAudioStream(interactive_audio);
          SDL_ResumeAudioStreamDevice(interactive_audio);
        }
        /* Pace from now, not from before the pause. */
        interactive_started_ns = SDL_GetTicksNS() -
            (uint64_t)(frame_counter - start_frame) * 1000000000 / 60;
      }
      if (frame_counter != interactive_frame) {
        /* Pace presentation only; emulated board clocks remain unchanged. */
        if (interactive_audio)
          wait_interactive_audio();
        else {
          uint64_t target = interactive_started_ns +
              (uint64_t)(frame_counter - start_frame) * 1000000000 / 60;
          uint64_t now = SDL_GetTicksNS();
          if (target > now) SDL_DelayNS(target - now);
        }
        interactive_frame = frame_counter;
      }
    }
#endif
#ifdef PCSX_SYSTEM12
    if (board && apply_input_events(input_events, input_event_count,
        &next_input_event, frame_counter - start_frame)) { result = 1; break; }
#endif
    if (trace && cycles >= trace_start && trace_count < trace_blocks) {
      trace_count++;
      fprintf(trace, "%lu,%" PRIu64 ",%08x,%08x", blocks, cycles,
        psxRegs.pc, intFakeFetch(psxRegs.pc));
      for (i = 0; i < 32; i++) fprintf(trace, ",%08x", psxRegs.GPR.r[i]);
      fputc('\n', trace);
    }
    /* Use the public interpreter step to cap otherwise branchless blocks.
     * It preserves the core's branch and pending load-delay semantics. */
    if (frontend_loop) {
      /* libretro retro_run(): one whole frame; the board advances only
       * through the core's screen events, never per block. */
      psxRegs.stop = 0;
      psxCpu->Execute(&psxRegs);
    }
    else if (source_step) {
#ifdef PCSX_SYSTEM12
      /* MAME order: the main CPU runs a whole slice, then the H8 catches up. */
      while (!slice_steps) {
        if (slice_started) system12_slice_end();
        slice_steps = system12_slice_begin();
        slice_started = 1;
      }
      slice_steps--;
#endif
      intExecuteSourceStep(&psxRegs);
    }
    else if (use_drc)
      psxCpu->ExecuteBlock(&psxRegs, EXEC_CALLER_BOOT);
    else {
      psxRegs.branchSeen = 0;
      do {
        execI(&psxRegs);
        steps++;
      } while (!psxRegs.branchSeen && steps < 4096 && !interrupted);
    }
    blocks++;
    cycles += (uint32_t)(psxRegs.cycle - prev_cycle);
    prev_cycle = psxRegs.cycle;
#ifdef PCSX_SYSTEM12
    /* Stop permanently before a later 32-bit CPU cycle wrap could reuse the
     * early bins. The harness's elapsed cycle count is monotonic. */
    if (bus_trace_bins && !bus_trace_finished &&
        cycles >= (uint64_t)BOARD_BUS_TRACE_BIN_CYCLES * BOARD_BUS_TRACE_MAX_BINS) {
      system12_set_bus_trace(NULL, NULL);
      bus_trace_finished = 1;
    }
    if (board) {
      if (reset_at_cycle && !reset_performed && cycles >= reset_at_cycle) {
        unsigned int elapsed_frames = frame_counter - start_frame;
        fprintf(stderr, "System12 harness soft reset: cycle=%" PRIu64 "\n", cycles);
        psxReset();
#ifdef PCSX_NATIVE_VITA_PATH
        if (interactive_mixing) {
          if(selected_board->family!=111)arcade_mix_reset(&interactive_mix); interactive_spu_cycle = psxRegs.cycle;
        }
#endif
        start_frame = frame_counter - elapsed_frames;
        prev_cycle = psxRegs.cycle;
        reset_performed = 1;
        slice_steps = 0; slice_started = 0;
      }
      /* Diagnostic control for frontends whose board catch-up is driven by
       * core vblank events rather than this harness's block loop. */
      int sync_result = vblank_sync_only || source_step || frontend_loop ? 0 :
        selected_board->sync(psxRegs.cycle);
      if (sync_result < 0) {
        fprintf(stderr, "%s: device or bus fault during execution\n", selected_board->name);
        result = 1;
      }
      else if (sync_result > 0) {
        /* A board watchdog requests reset at a safe CPU boundary. Preserve
         * run progress when the core restarts its frame and cycle counters. */
        unsigned int elapsed_frames = frame_counter - start_frame;
        psxReset();
#ifdef PCSX_NATIVE_VITA_PATH
        if (interactive_mixing) {
          if(selected_board->family!=111)arcade_mix_reset(&interactive_mix); interactive_spu_cycle = psxRegs.cycle;
        }
#endif
        start_frame = frame_counter - elapsed_frames;
        prev_cycle = psxRegs.cycle;
        slice_steps = 0; slice_started = 0;
      }
    }
#endif
    /* Between blocks, with the board caught up and no branch in flight. */
    if (save_state_path && !state_saved && frame_counter - start_frame >= save_state_at &&
        !psxRegs.branching && !source_step && !frontend_loop) {
      state_saved = 1;
      if (SaveState(save_state_path)) {
        fprintf(stderr, "cannot save state %s\n", save_state_path);
        result = 1;
      }
      else
        fprintf(stderr, "Saved state %s: frame=%u cycle=%u\n", save_state_path, frame_counter, psxRegs.cycle);
    }
    if (snapshots && snapshot_count < 200000 && cycles >= next_snapshot) {
      unsigned int r;
      fprintf(snapshots, "%" PRIu64 ",%u,%08x,%08x,%08x,%08x,%08x,%04x,%04x,%08x"
        ",%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x"
        ",%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
        cycles, frame_counter - start_frame, psxRegs.pc, psxRegs.code,
        psxRegs.CP0.n.SR, psxRegs.CP0.n.Cause, psxRegs.interrupt,
        psxHu16(0x1070), psxHu16(0x1074), psxMemRead32(&psxRegs, 0x80000080),
        psxMemRead32(&psxRegs, 0x800d2190), psxMemRead32(&psxRegs, 0x800d2194),
        psxMemRead32(&psxRegs, 0x800d2198), psxMemRead32(&psxRegs, 0x800d219c),
        psxMemRead32(&psxRegs, 0x800d21a0), psxMemRead32(&psxRegs, 0x800d21a4),
        psxMemRead32(&psxRegs, 0x800d21a8), psxMemRead32(&psxRegs, 0x800d21ac),
        psxMemRead32(&psxRegs, 0x800d21b0), psxMemRead32(&psxRegs, 0x800d21b4),
        psxMemRead32(&psxRegs, 0x800d21b8), psxMemRead32(&psxRegs, 0x800d21bc),
        psxMemRead32(&psxRegs, 0x800d21c0), psxMemRead32(&psxRegs, 0x800d21c4),
        psxMemRead32(&psxRegs, 0x800d21c8), psxMemRead32(&psxRegs, 0x800d21cc),
        psxMemRead32(&psxRegs, 0x800d21d0), psxMemRead32(&psxRegs, 0x800d21d4),
        psxMemRead32(&psxRegs, 0x800d21d8), psxMemRead32(&psxRegs, 0x800d21dc));
      for (r = 0; r < 32; r++) fprintf(snapshots, ",%08x", psxRegs.GPR.r[r]);
      fputc('\n', snapshots);
      snapshot_count++;
      do {
        if (UINT64_MAX - next_snapshot < snapshot_interval) {
          next_snapshot = UINT64_MAX;
          break;
        }
        next_snapshot += snapshot_interval;
      } while (next_snapshot <= cycles);
    }
    /* Bound a malformed boot that changes timer registers or loses vsync. */
    if (cycles > cycle_limit || blocks >= max_blocks) {
      fprintf(stderr, "execution limit reached before requested vsync count\n");
      result = 1; break;
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &end);
#if defined(PCSX_NATIVE_VITA_PATH) && defined(PCSX_SYSTEM12)
  close_interactive_audio();
  if (interactive_audio_error) result = 1;
#endif
  if (!result && reset_at_cycle && !reset_performed) {
    fprintf(stderr, "requested soft-reset cycle was not reached\n");
    result = 1;
  }
#ifdef PCSX_SYSTEM12
  if (board && audio_path && selected_board->close_audio()) {
    fprintf(stderr, "failed to finalize WAV output: %s\n", audio_path);
    result = 1;
  }
  free(input_events);
#endif
  if (trace && fclose(trace)) result = 1;
  if (snapshots && fclose(snapshots)) result = 1;
#ifdef PCSX_SYSTEM12
  if (edge_trace.file) {
    system12_set_edge_trace(NULL, NULL);
    flush_edge_reads(&edge_trace);
    if (edge_trace.overflow || ferror(edge_trace.file)) {
      fprintf(stderr, "System12 edge trace truncated or write failed\n");
      result = 1;
    }
    if (fclose(edge_trace.file)) result = 1;
  }
  if (bus_trace_bins) {
    system12_set_bus_trace(NULL, NULL);
    if (write_system12_bus_trace(bus_trace_file, bus_trace_bins)) result = 1;
    if (fclose(bus_trace_file)) result = 1;
    free(bus_trace_bins);
  }
#endif
  if (ram_path) {
    FILE *dump = fopen(ram_path, "wb");
    if (!dump) result = 1;
    else {
      if (fwrite(psxRegs.ptrs.psxM, 1, PSX_RAM_SIZE, dump) != PSX_RAM_SIZE) result = 1;
      if (fclose(dump)) result = 1;
    }
  }
  if (harness_write_ppm(output, 0, 0)) result = 1;
  if (vram_path && harness_write_vram(vram_path)) result = 1;
  if (verify) {
    unsigned short center = harness_vram_pixel(160, 100);
    unsigned short corner = harness_vram_pixel(10, 10);
    if (center != 0x03ff || corner != 0x2082 || psxMemRead32(&psxRegs, 0x8000f000) != 0x534d4f4b) {
      fprintf(stderr, "smoke FAILED: triangle=%04x background=%04x sentinel=%08x\n",
        center, corner, psxMemRead32(&psxRegs, 0x8000f000)); result = 1;
    }
    else fprintf(stderr, "smoke PASS: MIPS MMIO -> native GPU triangle + RAM sentinel\n");
  }
#ifdef PCSX_SYSTEM12
  if (!result && !interrupted && nvram_path && save_nvram(nvram_path)) result = 1;
#endif
#if defined(GPU_VITA_CTR)
  fprintf(stderr, "host=macOS-arm64 renderer=Vita-path GL compatibility cpu=%s "
#else
  fprintf(stderr, "host=macOS-arm64 renderer=GPU_NEON SIMD cpu=%s "
#endif
    "frames=%u cycles=%" PRIu64 " blocks=%lu pc=%08x flips=%u elapsed=%.3fs output=%s\n",
#ifndef DRC_DISABLE
    psxCpu == &psxRec ? "ari64" : "interpreter",
#else
    "interpreter",
#endif
    frame_counter - start_frame, cycles, blocks, psxRegs.pc,
    flips, end.tv_sec - begin.tv_sec + (end.tv_nsec - begin.tv_nsec) * 1e-9, output);
#ifdef PCSX_SYSTEM12
  if (inputs_path) fprintf(stderr, "Arcade inputs: consumed=%zu/%zu\n", next_input_event, input_event_count);
  if (board) selected_board->report(stderr);
#endif
  GPU_close(); SPU_close(); psxShutdown(); ReleasePlugins();
#ifdef PCSX_SYSTEM12
  if (board) selected_board->unload();
#endif
  return result || interrupted ? 1 : 0;
}
