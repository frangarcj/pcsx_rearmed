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
#endif
#include "../libpcsxcore/r3000a.h"
#include "../libpcsxcore/psxinterpreter.h"
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
static int sync_system12(u32 cycle) { system12_sync(cycle); return 0; }
#ifdef PCSX_SYSTEM_GV
static int sync_systemgv(u32 cycle) { return systemgv_sync(cycle); }
#endif
#ifdef PCSX_SYSTEM_GNET
static int sync_systemgnet(u32 cycle) { return systemgnet_sync(cycle); }
#endif
/* --system12 takes a prepared region directory or a MAME zip (<set>.zip). */
static int load_system12(const char *path)
{
  return system12_romset_is_zip(path) ? system12_romset_load(path) : system12_load(path);
}
static const HarnessBoard boards[] = {
  {101, "TaitoFX1S", systemtaito_load, systemtaito_unload, systemtaito_sync,
    systemtaito_set_input, systemtaito_selftest, systemtaito_report,
    systemtaito_open_audio, systemtaito_close_audio, systemtaito_eeprom_import, systemtaito_eeprom_export},
#ifdef PCSX_SYSTEM_GV
  {102, "KonamiGV", systemgv_load, systemgv_unload, sync_systemgv,
    systemgv_set_input, systemgv_selftest, systemgv_report,
    NULL, NULL, systemgv_eeprom_import, systemgv_eeprom_export},
#endif
#ifdef PCSX_SYSTEM_GNET
  {103, "GNet", systemgnet_load, systemgnet_unload, sync_systemgnet,
    systemgnet_set_input, systemgnet_selftest, systemgnet_report,
    NULL, NULL, systemgnet_eeprom_import, systemgnet_eeprom_export},
#endif
  {100, "CapcomZN", systemzn_load, systemzn_unload, systemzn_sync,
    systemzn_set_input, systemzn_selftest, systemzn_report,
    systemzn_open_audio, systemzn_close_audio, systemzn_eeprom_import, systemzn_eeprom_export},
  {10, "System10", system10_load, system10_unload, system10_sync,
    system10_set_input, system10_selftest, system10_report, NULL, NULL, NULL, NULL},
  {11, "System11", system11_load, system11_unload, system11_sync,
    system11_set_input, system11_selftest, system11_report,
    system11_open_audio, system11_close_audio, system11_eeprom_import, system11_eeprom_export},
  {12, "System12", load_system12, system12_unload, sync_system12,
    system12_set_input, system12_selftest, system12_report,
    system12_open_audio, system12_close_audio, system12_eeprom_import, system12_eeprom_export},
};
static const HarnessBoard *selected_board;
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
  fprintf(stderr, "usage: %s (--exe PS-X-EXE | --system12 REGION-DIR | --system11 REGION-DIR | --system10 REGION-DIR | --capcom-zn REGION-DIR | --taito-fx1 REGION-DIR | --system-gv REGION-DIR | --system-gnet REGION-DIR | --system-gnet-fixture) "
    "[--frames N] [--output FRAME.ppm] [--capture-gl] [--dump-vram FILE] [--board-test] [--verify-smoke] "
    "[--inputs FILE.csv] [--audio FILE.wav] [--nvram FILE] "
    "[--audio-test-fixture] "
    "[--reset-at-cycle N] "
    "[--trace FILE --trace-blocks N] [--cycle-snapshots FILE "
    "--snapshot-start CYCLES --snapshot-interval N] "
    "[--board-bus-trace FILE] [--system12-edge-trace FILE] [--system12-trace-shared-writes | --system12-trace-shared-accesses] "
    "[--max-blocks N] [--icache] [--drc] [--interactive [--pause-at N [--pause-script STEPS] [--pause-shot FILE.ppm]]] [--system12-vblank-sync-only] [--system12-source-step] [--frontend-loop]\n"
    "       %s (--gpu-type1-test | --gpu-type1-test-1m)\n", name, name);
}

#if defined(PCSX_NATIVE_VITA_PATH) && defined(PCSX_SYSTEM12)
static const struct { SDL_Scancode key; const char *control; } interactive_keys[] = {
  {SDL_SCANCODE_5, "coin1"}, {SDL_SCANCODE_1, "p1start"},
  {SDL_SCANCODE_UP, "p1up"}, {SDL_SCANCODE_DOWN, "p1down"},
  {SDL_SCANCODE_LEFT, "p1left"}, {SDL_SCANCODE_RIGHT, "p1right"},
  {SDL_SCANCODE_A, "p1button1"}, {SDL_SCANCODE_S, "p1button2"},
  {SDL_SCANCODE_D, "p1button3"}, {SDL_SCANCODE_SPACE, "p1button4"}
};
static volatile int pause_requested;

static void release_interactive_input(void)
{
  for (unsigned i = 0; i < sizeof(interactive_keys) / sizeof(interactive_keys[0]); i++)
    system12_set_input(interactive_keys[i].control, 0);
}

static void poll_interactive_input(void)
{
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
      interrupted = 1;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
      release_interactive_input();
    if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP)
      continue;
    if (event.key.repeat) continue;
    /* Escape pauses, as Start + Select on the Vita. */
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE)
      pause_requested = 1;
    for (unsigned i = 0; i < sizeof(interactive_keys) / sizeof(interactive_keys[0]); i++)
      if (event.key.scancode == interactive_keys[i].key)
        system12_set_input(interactive_keys[i].control, event.type == SDL_EVENT_KEY_DOWN);
  }
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

static void interactive_audio_cb(void *opaque, const int16_t *stereo,
    unsigned frames)
{
  (void)opaque;
  SDL_PutAudioStreamData(interactive_audio, stereo, (int)(frames * 4));
}

static int open_interactive_audio(void)
{
  const SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, 88200 };
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return -1;
  interactive_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
      &spec, NULL, NULL);
  if (!interactive_audio) return -1;
  system12_set_audio_output(interactive_audio_cb, NULL);
  SDL_ResumeAudioStreamDevice(interactive_audio);
  return 0;
}

/* Keep about three frames queued: enough to avoid gaps, little latency. */
static void wait_interactive_audio(void)
{
  const int limit = 88200 * 4 / 60 * 3;
  while (!interrupted && SDL_GetAudioStreamQueued(interactive_audio) > limit) {
    SDL_DelayNS(1000000);
    poll_interactive_input();
  }
}

static void close_interactive_audio(void)
{
  if (!interactive_audio) return;
  system12_set_audio_output(NULL, NULL);
  SDL_DestroyAudioStream(interactive_audio);
  interactive_audio = NULL;
}
#endif

#ifdef PCSX_SYSTEM12
static int load_nvram(const char *path)
{
  unsigned char image[2080];
  FILE *file = fopen(path, "rb");
  int invalid;
  if (!file) {
    if (errno == ENOENT) return 0;
    perror(path); return -1;
  }
  invalid = fread(image, 1, sizeof(image), file) != sizeof(image);
  if (!invalid) invalid = fgetc(file) != EOF || ferror(file);
  if (fclose(file)) invalid = 1;
  if (invalid) {
    fprintf(stderr, "%s: expected exact 2080-byte AT28C16 image\n", path);
    return -1;
  }
  return selected_board->import_nvram(image, sizeof(image));
}

static int save_nvram(const char *path)
{
  unsigned char image[2080];
  char *temporary;
  FILE *file;
  int fd, result = -1;
  if (selected_board->export_nvram(image, sizeof(image))) return -1;
  temporary = malloc(strlen(path) + sizeof(".tmp.XXXXXX"));
  if (!temporary) return -1;
  sprintf(temporary, "%s.tmp.XXXXXX", path);
  fd = mkstemp(temporary);
  if (fd < 0) { perror(path); free(temporary); return -1; }
  file = fdopen(fd, "wb");
  if (file) {
    int invalid = fwrite(image, 1, sizeof(image), file) != sizeof(image);
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

static int input_control_known(const char *name)
{
  /* Validate against the selected source input profile while every control
   * is still released, before the CPU begins executing CSV events. */
  return strlen(name) < sizeof(((harness_input_event *)0)->control) &&
    selected_board && selected_board->set_input(name, 0) == 0;
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
    if (errno || end == pressed_s || *end || pressed > 1) { failed = 1; break; }
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
    if (selected_board->set_input(events[*next].control, events[*next].pressed)) {
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
  int use_drc = 0;
  int capture_gl = 0;
  int interactive = 0;
  int vblank_sync_only = 0;
  int source_step = 0;
  uint32_t slice_steps = 0;
  int slice_started = 0;
  int max_blocks_requested = 0;
  uint64_t reset_at_cycle = 0;
  int reset_performed = 0;
#if defined(PCSX_NATIVE_VITA_PATH) && defined(PCSX_SYSTEM12)
  unsigned interactive_frame = 0;
  uint64_t interactive_started_ns = 0;
#if defined(PCSX_NATIVE_VITA_PATH) && defined(PCSX_SYSTEM12)
  char interactive_game[256] = "";
  unsigned pause_at = 0;
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
    else if ((!strcmp(argv[i], "--system12") || !strcmp(argv[i], "--system11") ||
        !strcmp(argv[i], "--system10") || !strcmp(argv[i], "--capcom-zn") ||
        !strcmp(argv[i], "--taito-fx1") || !strcmp(argv[i], "--system-gv") ||
        !strcmp(argv[i], "--system-gnet")) && i + 1 < argc) {
      if (board) { usage(argv[0]); return 2; }
      board_family = !strcmp(argv[i], "--system11") ? 11 :
        !strcmp(argv[i], "--system10") ? 10 : !strcmp(argv[i], "--capcom-zn") ? 100 :
        !strcmp(argv[i], "--taito-fx1") ? 101 : !strcmp(argv[i], "--system-gv") ? 102 :
        !strcmp(argv[i], "--system-gnet") ? 103 : 12;
      board = argv[++i];
    }
    else if (!strcmp(argv[i], "--output") && i + 1 < argc) output = argv[++i];
    else if (!strcmp(argv[i], "--capture-gl")) capture_gl = 1;
    else if (!strcmp(argv[i], "--dump-vram") && i + 1 < argc) vram_path = argv[++i];
    else if (!strcmp(argv[i], "--inputs") && i + 1 < argc) inputs_path = argv[++i];
    else if (!strcmp(argv[i], "--audio") && i + 1 < argc) audio_path = argv[++i];
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
#if defined(PCSX_NATIVE_VITA_PATH) && defined(PCSX_SYSTEM12)
    /* Checks of the pause menu: open it at frame N, press the script's
     * buttons, save it to a PPM and exit. */
    else if (!strcmp(argv[i], "--pause-at") && i + 1 < argc) pause_at = (unsigned)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--pause-script") && i + 1 < argc) pause_script = argv[++i];
    else if (!strcmp(argv[i], "--pause-shot") && i + 1 < argc) pause_shot = argv[++i];
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
    else if (!strcmp(argv[i], "--snapshot-start") && i + 1 < argc) {
      char *tail; unsigned long long n;
      errno = 0; n = strtoull(argv[++i], &tail, 10);
      if (errno || *tail) { usage(argv[0]); return 2; }
      snapshot_start = (uint64_t)n;
    }
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
      char *tail; unsigned long n = strtoul(argv[++i], &tail, 10);
      if (*tail || n == 0 || n > 216000) { usage(argv[0]); return 2; }
      frames = n;
    }
    else if (!strcmp(argv[i], "--gpu-type1-test") || !strcmp(argv[i], "--gpu-type1-test-1m")) {
      if (gpu_type1_test) { usage(argv[0]); return 2; }
      gpu_type1_test = !strcmp(argv[i], "--gpu-type1-test-1m") ? 2 : 1;
    }
    else if (!strcmp(argv[i], "--board-test")) board_test = 1;
    else if (!strcmp(argv[i], "--verify-smoke")) verify = 1;
    else if (!strcmp(argv[i], "--icache")) icache = 1;
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
    if (!board || board_family != 12 || inputs_path || capture_gl || board_test) {
      fprintf(stderr, "interactive mode requires System12 without replay/capture/board-test options\n");
      return 2;
    }
    frames = UINT32_MAX / 4;
    if (!max_blocks_requested) max_blocks = ~0UL;
#else
    fprintf(stderr, "interactive mode requires the vitagl-like Mac renderer build\n");
    return 2;
#endif
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
  if ((!exe && !board && !gpu_type1_test) || (exe && board) ||
      (gpu_type1_test && (exe || board || verify || trace_path)) || (board_test && !board) ||
      ((inputs_path || audio_path || audio_test_fixture || nvram_path) && !board) ||
      (snapshot_path && (!board || board_family != 12)) ||
      (bus_trace_path && (!board || board_family != 12)) ||
      (edge_trace_path && (!board || board_family != 12 || board_test)) ||
      (trace_shared_flags && !edge_trace_path) ||
      (reset_at_cycle && (!board || board_family != 12 || board_test || interactive)) ||
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
    } else board_load_result = selected_board->load(board);
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
    interactive_started_ns = SDL_GetTicksNS();
    interactive_frame = frame_counter;
    if (open_interactive_audio())
      fprintf(stderr, "Interactive: no audio (%s), pacing at 60 Hz\n", SDL_GetError());
    interactive_title(board, interactive_game, sizeof(interactive_game));
    fprintf(stderr, "Interactive: 5 coin, 1 start, arrows move, A/S/D/Space buttons, Escape pause\n");
  }
#endif
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
      if ((blocks & 255) == 0 || frame_counter != interactive_frame)
        poll_interactive_input();
      if (pause_at && frame_counter - start_frame >= pause_at) {
        pause_at = 0;
        pause_requested = 1;
      }
      if (pause_requested && frame_counter != interactive_frame) {
        /* Between frames: the menu draws over the one just presented. */
        int choice;
        pause_requested = 0;
        release_interactive_input();
        if (interactive_audio) SDL_PauseAudioStreamDevice(interactive_audio);
        choice = mac_pause_run(interactive_game, pause_script, pause_shot);
        if (choice == MAC_PAUSE_CLOSE) interrupted = 1;
        if (choice == MAC_PAUSE_RESTART) {
          unsigned int elapsed_frames = frame_counter - start_frame;
          psxReset();
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
    if (trace && blocks < trace_blocks) {
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
        start_frame = frame_counter - elapsed_frames;
        prev_cycle = psxRegs.cycle;
        slice_steps = 0; slice_started = 0;
      }
    }
#endif
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
  if (board) selected_board->report(stderr);
#endif
  GPU_close(); SPU_close(); psxShutdown(); ReleasePlugins();
#ifdef PCSX_SYSTEM12
  if (board) selected_board->unload();
#endif
  return result || interrupted ? 1 : 0;
}
