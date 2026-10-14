/*
 * (C) Gražvydas "notaz" Ignotas, 2011-2012
 *
 * This work is licensed under the terms of any of these licenses
 * (at your option):
 *  - GNU GPL, version 2 or later.
 *  - GNU LGPL, version 2.1 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#ifdef PCSX_SYSTEM12
#include "../../libpcsxcore/psxmem.h"
#define GPU_DMA_RAM_MASK PSX_RAM_MASK
#else
#define GPU_DMA_RAM_MASK 0x1fffffu
#endif
#include "gpu_timing.h"
#include "gpu_async.h"
#include "../../libpcsxcore/gpu.h" // meh
#include "../../frontend/plugin_lib.h"
#include "../../include/compiler_features.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

//#define log_io gpu_log
#define log_io(...)

struct psx_gpu gpu;
unsigned gpu_vram_height = GPU_VRAM_ROWS_CONSOLE;
#ifdef PCSX_SYSTEM12
int gpu_model = 2;
#else
static const int gpu_model = 2;
#endif
#ifdef GPULIB_TRACE
long GPUfreeze(uint32_t type, GPUFreeze_t *freeze, uint16_t **vram_ptr);
#endif

int gpu_set_model(int model)
{
  if ((model != 1 && model != 2) || gpu.vram != NULL)
    return -1;
#ifndef PCSX_SYSTEM12
  if (model != 2)
    return -1;
#else
  if (model == 1)
    gpu_vram_height = GPU_VRAM_ROWS_SYSTEM12;
  gpu_model = model;
#endif
  return 0;
}

void gpu_set_lightgun_position(uint16_t x,uint16_t y)
{
  gpu.lightgun_x=x;gpu.lightgun_y=y;
}


int gpu_set_vram_height(unsigned rows)
{
#ifndef PCSX_SYSTEM12
  if (rows != GPU_VRAM_ROWS_CONSOLE) return -1;
#endif
  if ((rows != GPU_VRAM_ROWS_CONSOLE && rows != GPU_VRAM_ROWS_SYSTEM12)
      || gpu.vram != NULL)
    return -1;
  gpu_vram_height = rows;
  return 0;
}

unsigned gpu_get_vram_height(void)
{
  return gpu_vram_height;
}

static noinline int do_cmd_buffer(struct psx_gpu *gpu, uint32_t *data, int count,
    int *cycles_sum, int *cycles_last);
static noinline void finish_vram_transfer(struct psx_gpu *gpu, int is_read, int is_async);

static void sync_renderer(struct psx_gpu *gpu)
{
  if (gpu_async_enabled(gpu))
    gpu_async_sync(gpu);
  else {
#ifdef GPU_VITA_CTR
    renderer_barrier();
#else
    renderer_flush_queues();
#endif
  }
}

#ifdef GPU_VITA_CTR
/* For a frontend that draws on the renderer between frames (menus): returns
 * with no GPU work in flight. */
void gpu_sync_renderer(void)
{
  sync_renderer(&gpu);
}
#endif

static void sync_renderer_ecmds(struct psx_gpu *gpu)
{
  gput_sync(&gpu->timing, gpu->ex_regs);
  if (gpu_async_enabled(gpu))
    gpu_async_sync_ecmds(gpu);
  else
    renderer_sync_ecmds(gpu->ex_regs);
}

static void renderer_update_caches_wrapped(int x, int y, int w, int h,
    int state_changed)
{
  int first = (int)gpu_vram_height - y;
  if (first > h)
    first = h;
  if (first > 0)
    renderer_update_caches(x, y, w, first, state_changed);
  if (first < h)
    renderer_update_caches(x, 0, w, h - first, state_changed);
}

static void sync_ecmds_status_bits(struct psx_gpu *gpu)
{
#ifdef PCSX_SYSTEM12
  if (gpu_get_model() == 1) {
    gpu->status &= ~0x1fff;
    gpu->status |= gpu->type1_status & 0x1fff;
  }
  else {
    gpu->status &= ~0x9fff;
    gpu->status |= (gpu->ex_regs[1] & 0x7ff)
                | ((gpu->ex_regs[6] & 3) << 11)
                | ((gpu->ex_regs[1] & 0x800) << 4);
  }
#else
  gpu->status &= ~0x1fff;
  gpu->status |= gpu->ex_regs[1] & 0x7ff;
  gpu->status |= (gpu->ex_regs[6] & 3) << 11;
#endif
}

static noinline void do_cmd_reset(struct psx_gpu *gpu)
{
  int dummy = 0;

  if (unlikely(gpu->cmd_len > 0)) {
    do_cmd_buffer(gpu, gpu->cmd_buffer, gpu->cmd_len, &dummy, &dummy);
    sync_ecmds_status_bits(gpu);
  }
  gpu->cmd_len = 0;

  if (unlikely(gpu->dma.h > 0))
    finish_vram_transfer(gpu, gpu->dma_start.is_read, 0);
  gpu->dma.h = 0;
}

static noinline void do_reset(struct psx_gpu *gpu)
{
  unsigned int i;

  do_cmd_reset(gpu);
  sync_renderer(gpu);

  memset(gpu->regs, 0, sizeof(gpu->regs));
  for (i = 0; i < sizeof(gpu->ex_regs) / sizeof(gpu->ex_regs[0]); i++)
    gpu->ex_regs[i] = (0xe0 + i) << 24;
  gpu->status = 0x14802000;
  gpu->type1_status = 0;
  gpu->gp0 = 0;
  gpu->regs[3] = 1;
  gpu->screen.src_x = gpu->screen.src_y = 0;
  gpu->screen.hres = gpu->screen.w = 256;
  gpu->screen.vres = gpu->screen.h = 240;
  gpu->screen.x = gpu->screen.y = 0;
  gpu->screen.x1 = 0x200, gpu->screen.x2 = 0x200+256*10;
  gpu->screen.y1 = 0x010, gpu->screen.y2 = 0x010+240;
  gput_sync(&gpu->timing, gpu->ex_regs);
  renderer_sync_ecmds(gpu->ex_regs);
  renderer_notify_screen_change(&gpu->screen);
  gpu_async_sync_ecmds(gpu);
}

static noinline void update_width(struct psx_gpu *gpu)
{
  static const short hres_all[8] = { 256, 368, 320, 368, 512, 368, 640, 368 };
  static const uint8_t hdivs[8] = { 10, 7, 8, 7, 5, 7, 4, 7 };
  uint8_t hdiv = hdivs[(gpu->status >> 16) & 7];
  int hres = hres_all[(gpu->status >> 16) & 7];
  int pal = gpu->status & PSX_GPU_STATUS_PAL;
  int sw = gpu->screen.x2 - gpu->screen.x1;
  int type = gpu->state.screen_centering_type;
  int x = 0, x_auto;
  if (type == C_AUTO)
    type = gpu->state.screen_centering_type_default;
  if (sw <= 0)
    /* nothing displayed? */;
  else {
    int s = pal ? 656 : 608; // or 600? pal is just a guess
    x = (gpu->screen.x1 - s) / hdiv;
    x = (x + 1) & ~1;   // blitter limitation
    sw /= hdiv;
    sw = (sw + 2) & ~3; // according to nocash

    if (gpu->state.show_overscan == 2) // widescreen hack
      sw = (sw + 63) & ~63;
    if (gpu->state.show_overscan && sw >= hres)
      x = 0, hres = sw;
    switch (type) {
    case C_INGAME:
      break;
    case C_MANUAL:
      x = gpu->state.screen_centering_x;
      break;
    default:
      // correct if slightly miscentered
      x_auto = (hres - sw) / 2 & ~3;
      if ((uint32_t)x_auto <= 8u && abs(x) < 24)
        x = x_auto;
    }
    if (x + sw > hres)
      sw = hres - x;
    // .x range check is done in vout_update()
  }
  // reduce the unpleasant right border that a few games have
  if (gpu->state.screen_centering_type == 0
      && x <= 4 && hres - (x + sw) >= 4)
    hres -= 4;
  gpu->screen.x = x;
  gpu->screen.w = sw;
  gpu->screen.hres = hres;
  gpu->state.dims_changed = 1;
  //printf("xx %d %d (%d) -> %2d, %d / %d\n", gpu->screen.x1,
  //  gpu->screen.x2, gpu->screen.x2 - gpu->screen.x1, x, sw, hres);
}

static noinline void update_height(struct psx_gpu *gpu)
{
  int pal = gpu->status & PSX_GPU_STATUS_PAL;
  int dheight = gpu->status & PSX_GPU_STATUS_DHEIGHT;
  int y = gpu->screen.y1 - (pal ? 39 : 16); // 39 for spyro
  int sh = gpu->screen.y2 - gpu->screen.y1;
  int center_tol = 16;
  int vres = 240;

  if (pal && (sh > 240 || gpu->screen.vres == 256))
    vres = 256;
  if (dheight)
    y *= 2, sh *= 2, vres *= 2, center_tol *= 2;
  if (sh <= 0)
    /* nothing displayed? */;
  else {
    switch (gpu->state.screen_centering_type) {
    case C_INGAME:
      break;
    case C_BORDERLESS:
      y = 0;
      break;
    case C_MANUAL:
      y = gpu->state.screen_centering_y;
      vres += gpu->state.screen_centering_h_adj;
      break;
    default:
      // correct if slightly miscentered
      if ((uint32_t)(vres - sh) <= 1 && abs(y) <= center_tol)
        y = 0;
    }
    if (y + sh > vres)
      sh = vres - y;
  }
  gpu->screen.y = y;
  gpu->screen.h = sh;
  gpu->screen.vres = vres;
  gpu->state.dims_changed = 1;
  //printf("yy %d %d -> %d, %d / %d\n",
  //  gpu->screen.y1, gpu->screen.y2, y, sh, vres);
}

static noinline void frameskip_prepare_noskip(struct psx_gpu *gpu)
{
  if (gpu->frameskip.ecmds_dirty_renderer) {
    gpu->frameskip.ecmds_dirty_renderer = 0;
    sync_renderer_ecmds(gpu);
  }
  if (gpu->frameskip.pending_fill[0] != 0) {
    int dummy = 0;
    if (gpu_async_enabled(gpu))
      (void)gpu_async_do_cmd_list(gpu, gpu->frameskip.pending_fill, 3,
          &dummy, &dummy, &dummy, &dummy);
    else
      renderer_do_cmd_list(gpu->frameskip.pending_fill, 3, gpu->ex_regs,
          &dummy, &dummy, &dummy);
    gpu->frameskip.pending_fill[0] = 0;
  }
}

#define FRAMESKIP_MAX_FLIP_DELAY 5

static noinline void decide_frameskip(struct psx_gpu *gpu, uint32_t flip_delay)
{
  if (flip_delay > FRAMESKIP_MAX_FLIP_DELAY) {
    // don't skip if no updates for a while
    gpu->frameskip.active = 0;
    gpu->frameskip.cnt = 0;
    gpu->frameskip.frame_ready = 1;
    return;
  }

  *gpu->frameskip.dirty = 1;

  if (gpu->frameskip.active)
    gpu->frameskip.cnt++;
  else {
    gpu->frameskip.cnt = 0;
    gpu->frameskip.frame_ready = 1;
  }

  // note: frontend/libretro only uses 'force'
  if (*gpu->frameskip.force)
    gpu->frameskip.active = 1;
  else if (!gpu->frameskip.active && *gpu->frameskip.advice)
    gpu->frameskip.active = 1;
  else if (gpu->frameskip.set > 0 && gpu->frameskip.cnt < gpu->frameskip.set)
    gpu->frameskip.active = 1;
  else
    gpu->frameskip.active = 0;
}

static int check_screen_intersect(struct psx_gpu *gpu, int x, int y, int w, int h)
{
  int32_t screen_r = gpu->screen.src_x + gpu->screen.w;
  int32_t screen_b = gpu->screen.src_y + gpu->screen.h;
  int32_t dst_r = x + w, dst_b = y + h;
  int32_t no_intersect;
  no_intersect  = screen_r - x - 1;
  no_intersect |= screen_b - y - 1;
  no_intersect |= dst_r - gpu->screen.src_x - 1;
  no_intersect |= dst_b - gpu->screen.src_y - 1;
  no_intersect >>= 31;
  return !no_intersect;
}

static noinline void check_draw_to_display(struct psx_gpu *gpu)
{
  uint32_t cmd_e3 = gpu->ex_regs[3];
  uint32_t x1 = cmd_e3 & 0x3ff;
  uint32_t y1 = (cmd_e3 >> (gpu_get_model() == 1 ? 12 : 10)) & 0x3ff;
  int intersect = check_screen_intersect(gpu, x1, y1, gpu->screen.w, gpu->screen.h);
  gpu->state.draw_display_intersect = intersect;
  // no frameskip if it decides to draw to display area,
  // but not for interlace since it'll most likely always do that
  gpu->frameskip.allow = !intersect || (gpu->status & PSX_GPU_STATUS_INTERLACE);
}

static void flush_cmd_buffer(struct psx_gpu *gpu);

static noinline void get_gpu_info(struct psx_gpu *gpu, uint32_t data)
{
  if (unlikely(gpu->cmd_len > 0))
    flush_cmd_buffer(gpu);
  switch (data & 0x0f) {
    case 0x02:
    case 0x03:
    case 0x04:
      gpu->gp0 = gpu->ex_regs[data & 7]
        & (gpu_get_model() == 1 ? 0x3fffff : 0xfffff);
      break;
    case 0x05:
      gpu->gp0 = gpu->ex_regs[5]
        & (gpu_get_model() == 1 ? 0x7fffff : 0x3fffff);
      break;
    case 0x07:
      gpu->gp0 = gpu_get_model();
      break;
    case 0x08:
      gpu->gp0=(data&0xff)==8?gpu->lightgun_x|((uint32_t)gpu->lightgun_y<<16):0;
      break;
    default:
      // gpu->gp0 unchanged
      break;
  }
}

#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

//  Minimum 16-byte VRAM alignment needed by gpu_unai's pixel-skipping
//  renderer/downscaler it uses in high res modes:
#ifdef GCW_ZERO
	// On GCW platform (MIPS), align to 8192 bytes (1 TLB entry) to reduce # of
	// fills. (Will change this value if it ever gets large page support)
	#define VRAM_ALIGN 8192
#else
	#define VRAM_ALIGN 64
#endif

// double, for overdraw/overscan guard + at least 1 page before
#define VRAM_SIZE ((1024 * gpu_vram_height * 2 * 2) + max(VRAM_ALIGN, 4096))

// vram ptr received from mmap/malloc/alloc (will deallocate using this)
static uint16_t *vram_ptr_orig = NULL;

#ifndef GPULIB_USE_MMAP
# if defined(__linux__) || defined(_3DS) || defined(HAVE_LIBNX) || defined(VITA)
#  define GPULIB_USE_MMAP 1
# else
#  define GPULIB_USE_MMAP 0
# endif
#endif
static int map_vram(void)
{
#if GPULIB_USE_MMAP
  gpu.vram = vram_ptr_orig = gpu.mmap(VRAM_SIZE);
#else
  gpu.vram = vram_ptr_orig = calloc(VRAM_SIZE, 1);
#endif
  if (gpu.vram != NULL && gpu.vram != (void *)(intptr_t)-1) {
    // 4kb guard in front
    gpu.vram += (4096 / 2);
    // Align
    gpu.vram = (uint16_t*)(((uintptr_t)gpu.vram + (VRAM_ALIGN-1)) & ~(VRAM_ALIGN-1));
    return 0;
  }
  else {
    SysPrintf("could not map vram, expect crashes\n");
    gpu.vram = NULL;
    return -1;
  }
}


#ifdef GPULIB_TRACE
/* GPU command trace for tools/gpu-replay: GPULIB_TRACE_FILE=path:from:to
 * records every GPU entry call made in emulated frames [from, to), after a
 * snapshot of VRAM and registers, so the renderer's work can be replayed and
 * profiled without the rest of the emulator. Format in tools/gpu-replay. */
#include <stdio.h>
#include <stdlib.h>

enum {
  GT_STATUS = 1, GT_DATA_MEM, GT_DATA, GT_CHAIN, GT_CHAIN_PKT, GT_CHAIN_END,
  GT_READ_MEM, GT_READ, GT_READ_STATUS, GT_VBLANK, GT_END
};

static struct {
  FILE *f;
  unsigned from, to;
  int state;   /* 0 unparsed, 1 waiting, 2 recording, 3 off */
  const struct rearmed_cbs *cbs;
} gtrace;

static void gt_put(const void *p, size_t words)
{
  fwrite(p, 4, words, gtrace.f);
}

static void gt_rec(uint32_t tag, uint32_t a, uint32_t b)
{
  uint32_t r[3] = { tag, a, b };
  gt_put(r, 3);
}

static void gt_start(void)
{
  const struct rearmed_cbs *c = gtrace.cbs;
  GPUFreeze_t *fz = calloc(1, sizeof(*fz));
  uint16_t *vram;
  uint32_t hdr[32] = { 0x52544753, 1, gpu_vram_height, gpu_model, gtrace.from, gtrace.to };
  if (c) {
    hdr[6] = c->frameskip; hdr[7] = c->dithering; hdr[8] = c->scale_hires;
    hdr[9] = c->alt_flip; hdr[10] = c->gpu_hacks;
    hdr[11] = c->gpu_neon.allow_interlace; hdr[12] = c->gpu_neon.enhancement_enable;
    hdr[13] = c->gpu_neon.enhancement_no_main; hdr[14] = c->gpu_neon.enhancement_tex_adj;
    hdr[15] = c->screen_centering_type; hdr[16] = c->screen_centering_type_default;
    hdr[17] = c->screen_centering_x; hdr[18] = c->screen_centering_y;
    hdr[19] = c->screen_centering_h_adj; hdr[20] = c->show_overscan;
    hdr[21] = c->only_16bpp; hdr[22] = c->gpu_caps;
  }
  hdr[23] = *gpu.state.frame_count;
  hdr[24] = GPU_DMA_RAM_MASK + 1;
  GPUfreeze(1, fz, &vram);
  gt_put(hdr, 32);
  gt_put(&fz->ulStatus, 1);
  gt_put(fz->ulControl, 256);
  gt_put(vram, 1024 * gpu_vram_height / 2);
  free(fz);
  gtrace.state = 2;
  SysPrintf("gpulib: trace started at frame %u\n", hdr[23]);
}

/* Whether this call is to be recorded: starts and stops the capture on the
 * emulated frame counter. */
static int gt_on(void)
{
  if (likely(gtrace.state == 3))
    return 0;
  if (gtrace.state == 0) {
    const char *env = getenv("GPULIB_TRACE_FILE");
    char path[512];
    gtrace.state = 3;
    if (env && sscanf(env, "%511[^:]:%u:%u", path, &gtrace.from, &gtrace.to) == 3 &&
        (gtrace.f = fopen(path, "wb")) != NULL)
      gtrace.state = 1;
    else if (env)
      SysPrintf("gpulib: bad GPULIB_TRACE_FILE \"%s\" (path:from:to)\n", env);
  }
  if (gtrace.state == 1 && *gpu.state.frame_count >= gtrace.from)
    gt_start();
  if (gtrace.state == 2 && *gpu.state.frame_count >= gtrace.to) {
    /* The VRAM the replay must reach: FNV-1a of every byte. */
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *v = (const uint8_t *)gpu.vram;
    size_t i, n = (size_t)1024 * gpu_vram_height * 2;
    if (gpu.cmd_len > 0)
      flush_cmd_buffer(&gpu);
    sync_renderer(&gpu);
    for (i = 0; i < n; i++)
      h = (h ^ v[i]) * 0x100000001b3ull;
    gt_rec(GT_END, (uint32_t)h, (uint32_t)(h >> 32));
    if (getenv("GPULIB_TRACE_VRAM")) {
      FILE *vf = fopen(getenv("GPULIB_TRACE_VRAM"), "wb");
      if (vf) {
        fwrite(gpu.vram, 2, (size_t)1024 * gpu_vram_height, vf);
        fclose(vf);
      }
    }
    fclose(gtrace.f);
    gtrace.f = NULL;
    gtrace.state = 3;
    SysPrintf("gpulib: trace done\n");
  }
  return gtrace.state == 2;
}
#define GT(call) do { if (unlikely(gtrace.state != 3) && gt_on()) { call; } } while (0)
#else
#define GT(call) do { } while (0)
#endif

long GPUinit(void)
{
  int ret;
  ret  = vout_init();
  ret |= renderer_init();
  gput_reset(&gpu.timing);

  memset(&gpu.state, 0, sizeof(gpu.state));
  memset(&gpu.frameskip, 0, sizeof(gpu.frameskip));
  gpu.zero = 0;
  gpu.state.frame_count = &gpu.zero;
  gpu.state.hcnt = &gpu.zero;
  gpu.cmd_len = 0;
  gpu_set_lightgun_position(0,0);
  do_reset(&gpu);

  return ret;
}

long GPUshutdown(void)
{
  long ret;

  gpu_async_stop(&gpu);
  renderer_finish();
  ret = vout_finish();

  if (vram_ptr_orig != NULL) {
#if GPULIB_USE_MMAP
    gpu.munmap(vram_ptr_orig, VRAM_SIZE);
#else
    free(vram_ptr_orig);
#endif
  }
  vram_ptr_orig = gpu.vram = NULL;

  return ret;
}

void GPUwriteStatus(uint32_t data)
{
  GT(gt_rec(GT_STATUS, data, 0));
  uint32_t cmd = data >> 24;
  uint32_t fb_dirty = 1, frame;
  int src_x, src_y, changed;

  if (cmd < ARRAY_SIZE(gpu.regs)) {
    if (cmd > 1 && cmd != 5 && cmd != 0x0d && gpu.regs[cmd] == data)
      return;
    gpu.regs[cmd] = data;
  }

  switch (cmd) {
    case 0x0d:
      gpu_set_lightgun_position(0,0);
      fb_dirty=0;
      break;
    case 0x00:
      do_reset(&gpu);
      break;
    case 0x01:
      do_cmd_reset(&gpu);
      fb_dirty = 0;
      break;
    case 0x03:
      if (data & 1) {
        gpu.status |= PSX_GPU_STATUS_BLANKING;
        gpu.state.dims_changed = 1; // for hud clearing
      }
      else
        gpu.status &= ~PSX_GPU_STATUS_BLANKING;
      break;
    case 0x04:
      gpu.status &= ~PSX_GPU_STATUS_DMA_MASK;
      gpu.status |= PSX_GPU_STATUS_DMA(data & 3);
      fb_dirty = 0;
      break;
    case 0x05:
      src_x = data & 0x3ff;
      src_y = (data >> (gpu_get_model() == 1 ? 12 : 10)) & GPU_VRAM_Y_MASK;
      changed = src_x != gpu.screen.src_x || src_y != gpu.screen.src_y;
      frame = *gpu.state.frame_count;
      // last_flip_frame check allows frameskip on dheight games
      // that always set the same display area address
      if (changed || gpu.frameskip.last_flip_frame != frame)
      {
        gpu.state.fb_dirty_display_area |= gpu.state.fb_dirty;
        gpu.state.fb_dirty = 0;
        gpu.screen.src_x = src_x;
        gpu.screen.src_y = src_y;
        check_draw_to_display(&gpu);
        if (gpu.frameskip.set) {
          uint32_t flip_delay = frame - gpu.frameskip.last_flip_frame;
          if (flip_delay)
            decide_frameskip(&gpu, flip_delay);
          if (!gpu.frameskip.active || !gpu.frameskip.allow)
            frameskip_prepare_noskip(&gpu);
        }
        gpu.frameskip.last_flip_frame = *gpu.state.frame_count;
      }
      if (changed) {
        if (gpu_async_enabled(&gpu))
          gpu_async_notify_screen_change(&gpu);
        else
          renderer_notify_screen_change(&gpu.screen);
      }
      fb_dirty = 0;
      break;
    case 0x06:
      gpu.screen.x1 = data & 0xfff;
      gpu.screen.x2 = (data >> 12) & 0xfff;
      update_width(&gpu);
      break;
    case 0x07:
      gpu.screen.y1 = data & 0x3ff;
      gpu.screen.y2 = (data >> 10) & 0x3ff;
      update_height(&gpu);
      break;
    case 0x08:
      gpu.status = (gpu.status & ~0x7f0000) | ((data & 0x3F) << 17) | ((data & 0x40) << 10);
      update_width(&gpu);
      update_height(&gpu);
      if (gpu_async_enabled(&gpu))
        gpu_async_notify_screen_change(&gpu);
      else
        renderer_notify_screen_change(&gpu.screen);
      break;
    default:
      if ((cmd & 0xf0) == 0x10)
        get_gpu_info(&gpu, data);
      fb_dirty = 0;
      break;
  }

  gpu.state.fb_dirty_display_area |= fb_dirty;

#ifdef GPUwriteStatus_ext
  GPUwriteStatus_ext(data);
#endif
}

const unsigned char cmd_lengths[256] =
{
	0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	3, 3, 3, 3, 6, 6, 6, 6, 4, 4, 4, 4, 8, 8, 8, 8, // 20
	5, 5, 5, 5, 8, 8, 8, 8, 7, 7, 7, 7, 11, 11, 11, 11,
	2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, // 40
	3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
	2, 2, 2, 2, 3, 3, 3, 3, 1, 1, 1, 1, 0, 0, 0, 0, // 60
	1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, // 80
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // a0
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // c0
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // e0
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// this isn't very useful so should be rare
void cpy_mask(uint16_t *dst, const uint16_t *src, int l, uint32_t r6)
{
  int i;
  if (r6 == 1) {
    for (i = 0; i < l; i++)
      dst[i] = src[i] | 0x8000;
  }
  else {
    uint16_t msb = r6 << 15;
    for (i = 0; i < l; i++) {
      uint16_t mask = (int16_t)dst[i] >> 15;
      dst[i] = (dst[i] & mask) | ((src[i] | msb) & ~mask);
    }
  }
}

static int do_vram_io(struct psx_gpu *gpu, uint32_t *data, int count, int is_read)
{
  int count_initial = count;
  uint32_t r6 = gpu->ex_regs[6] & 3;
  uint16_t *sdata = (uint16_t *)data;
  uint16_t *vram = gpu->vram;
  int x = gpu->dma.x, y = gpu->dma.y;
  int w = gpu->dma.w, h = gpu->dma.h;
  int o = gpu->dma.offset;
  int l, async_queued = 0;

#if defined(GPU_VITA_CTR) && !defined(__vita__)
  if (!is_read && o == 0 && getenv("PCSX_TRACE_WRITES"))
    fprintf(stderr, "vram-write %d,%d %dx%d words=%d\n", x, y, w, h, count);
#endif
  /* A whole transfer in this call goes to the GPU thread with the commands.
   * Command lists carry the next command after the data: take only the
   * transfer's words (Soul Calibur's palette uploads, 16x1 in 9 words,
   * each synced with the GPU thread before). */
  const int words = (w * h + 1) / 2;
  if (gpu_async_enabled(gpu) && !is_read && o == 0 &&
      words <= AGPU_DMA_MAX && count >= words)
    async_queued = gpu_async_try_dma(gpu, data, words);
  if (async_queued) {
    gpu->dma.h = 0;
    finish_vram_transfer(gpu, 0, 1);
    return words;
  }
#ifndef GPU_VITA_CTR
  if (o == 0)
    sync_renderer(gpu);
#else
  // The GPU thread may still read or write the guest VRAM written here.
  if (o == 0 && gpu_async_enabled(gpu)) {
#ifdef __vita__
    extern int gpu_async_sync_reason;
    extern int gpu_async_write_last[4];
    gpu_async_sync_reason = 1;
    gpu_async_write_last[0] = w;
    gpu_async_write_last[1] = h;
    gpu_async_write_last[2] = count;
    gpu_async_write_last[3] = x | (y << 16);
#endif
    gpu_async_sync(gpu);
  }
#endif
  /* GPU_VITA_CTR keeps its own VRAM: these writes reach it only through
   * renderer_update_caches, which orders them after earlier draws, and a
   * read was synced by start_vram_transfer. A barrier here waited for the
   * renderer thread at every upload. */

  count *= 2; // operate in 16bpp pixels
  if (gpu->dma.offset) {
    l = w - gpu->dma.offset;
    if (count < l)
      l = count;

    do_vram_line(vram, x + o, y, sdata, l, is_read, r6);

    if (o + l < w)
      o += l;
    else {
      o = 0;
      y++;
      h--;
    }
    sdata += l;
    count -= l;
  }

  for (; h > 0 && count >= w; sdata += w, count -= w, y++, h--) {
    y &= GPU_VRAM_Y_MASK;
    do_vram_line(vram, x, y, sdata, w, is_read, r6);
  }

  if (h > 0) {
    if (count > 0) {
      y &= GPU_VRAM_Y_MASK;
      do_vram_line(vram, x, y, sdata, count, is_read, r6);
      o = count;
      count = 0;
    }
  }
  else
    finish_vram_transfer(gpu, is_read, 0);
  gpu->dma.y = y;
  gpu->dma.h = h;
  gpu->dma.offset = o;

  return count_initial - count / 2;
}

static noinline void start_vram_transfer(struct psx_gpu *gpu, uint32_t pos_word,
  uint32_t size_word, int is_read)
{
  if (gpu->dma.h)
    log_anomaly(gpu, "start_vram_transfer while old unfinished\n");

  gpu->dma.x = pos_word & 0x3ff;
  gpu->dma.y = (pos_word >> 16) & GPU_VRAM_Y_MASK;
  gpu->dma.w = ((size_word - 1) & 0x3ff) + 1;
  gpu->dma.h = (((size_word >> 16) - 1) & GPU_VRAM_Y_MASK) + 1;
  gpu->dma.offset = 0;
  gpu->dma.is_read = is_read;
  gpu->dma_start = gpu->dma;

  // postponed until the actual transfer
  //sync_renderer(gpu);

  if (is_read) {
#ifdef GPU_VITA_CTR
    // The renderer's frontend belongs to the GPU thread while it runs.
    if (gpu_async_enabled(gpu)) {
#ifdef __vita__
      extern int gpu_async_sync_reason;
      gpu_async_sync_reason = 2;
#endif
      gpu_async_sync(gpu);
    }
    renderer_sync_vram_to_cpu(gpu->dma.x, gpu->dma.y, gpu->dma.w, gpu->dma.h);
#endif
    const uint16_t *mem = VRAM_MEM_XY(gpu->vram, gpu->dma.x, gpu->dma.y);
    gpu->status |= PSX_GPU_STATUS_IMG;
    // XXX: wrong for width 1
    gpu->gp0 = LE16TOH(mem[0]) | ((uint32_t)LE16TOH(mem[1]) << 16);
    gpu->state.last_vram_read_frame = *gpu->state.frame_count;
  }

  if (gpu->dma.x + gpu->dma.w > 1024)
    log_anomaly(gpu, "vram tr xwrap: %c (%d, %d) %dx%d\n", is_read ? 'r' : 'w',
      gpu->dma.x, gpu->dma.y, gpu->dma.w, gpu->dma.h);
  else
    log_io(gpu, "start_vram_transfer %c (%d, %d) %dx%d\n", is_read ? 'r' : 'w',
      gpu->dma.x, gpu->dma.y, gpu->dma.w, gpu->dma.h);
  if (gpu->gpu_state_change)
    gpu->gpu_state_change(PGS_VRAM_TRANSFER_START, 0);
}

static void finish_vram_transfer(struct psx_gpu *gpu, int is_read, int is_async)
{
  if (is_read)
    gpu->status &= ~PSX_GPU_STATUS_IMG;
  else {
    int intersect = check_screen_intersect(gpu, gpu->dma_start.x, gpu->dma_start.y,
                      gpu->dma_start.w, gpu->dma_start.h);
    log_io(gpu, "dma %3d,%3d %dx%d scr %3d,%3d %3dx%3d -> dirty %d\n",
      gpu->dma_start.x, gpu->dma_start.y, gpu->dma_start.w, gpu->dma_start.h,
      gpu->screen.src_x, gpu->screen.src_y, gpu->screen.hres, gpu->screen.vres, intersect);
    gpu->state.fb_dirty_display_area |= intersect;
    gpu->state.fb_dirty = 1;
    if (!is_async)
      renderer_update_caches_wrapped(gpu->dma_start.x, gpu->dma_start.y,
                                     gpu->dma_start.w, gpu->dma_start.h, 0);
  }
  if (gpu->gpu_state_change)
    gpu->gpu_state_change(PGS_VRAM_TRANSFER_END, 0);
}

int do_vram_copy_pre(struct psx_gpu *gpu, const uint32_t *params, int *cpu_cycles)
{
  const uint32_t sx =  LE32TOH(params[1]) & 0x3FF;
  const uint32_t sy = (LE32TOH(params[1]) >> 16) & GPU_VRAM_Y_MASK;
  const uint32_t dx =  LE32TOH(params[2]) & 0x3FF;
  const uint32_t dy = (LE32TOH(params[2]) >> 16) & GPU_VRAM_Y_MASK;
  uint32_t w =  ((LE32TOH(params[3]) - 1) & 0x3FF) + 1;
  uint32_t h = (((LE32TOH(params[3]) >> 16) - 1) & GPU_VRAM_Y_MASK) + 1;
  int intersect;

  *cpu_cycles = gput_copy(w, h);
  if (sx == dx && sy == dy && !(gpu->ex_regs[6] & 0x8000))
    return 0;

  intersect = check_screen_intersect(gpu, dx, dy, w, h);
  gpu->state.fb_dirty_display_area |= intersect;
  gpu->state.fb_dirty = 1;
  return 1;
}

int do_vram_copy(uint16_t *vram, const uint32_t *ex_regs, const uint32_t *params)
{
  const uint32_t sx =  LE32TOH(params[1]) & 0x3FF;
  const uint32_t sy = (LE32TOH(params[1]) >> 16) & GPU_VRAM_Y_MASK;
  const uint32_t dx =  LE32TOH(params[2]) & 0x3FF;
  const uint32_t dy = (LE32TOH(params[2]) >> 16) & GPU_VRAM_Y_MASK;
  uint32_t w =  ((LE32TOH(params[3]) - 1) & 0x3FF) + 1;
  uint32_t h = (((LE32TOH(params[3]) >> 16) - 1) & GPU_VRAM_Y_MASK) + 1;
  uint16_t msb = ex_regs[6] << 15;
  uint16_t lbuf[128];
  uint32_t x, y;

#ifdef GPU_VITA_CTR
  /* Only the source is read: the copy overwrites the whole destination (no
   * mask test here), and a destination overlapping the source is in the
   * source's read. Reading the destination back cost Soul Calibur's throws
   * a renderer sync and readback every other frame (37-40 fps on the Vita). */
  renderer_sync_vram_to_cpu(sx, sy, w, h);
#else
  renderer_flush_queues();
#endif

  if (unlikely((sx < dx && dx < sx + w) || sx + w > 1024 || dx + w > 1024 || msb))
  {
    for (y = 0; y < h; y++)
    {
      const uint16_t *src = VRAM_MEM_XY(vram, 0, (sy + y) & GPU_VRAM_Y_MASK);
      uint16_t *dst = VRAM_MEM_XY(vram, 0, (dy + y) & GPU_VRAM_Y_MASK);
      for (x = 0; x < w; x += ARRAY_SIZE(lbuf))
      {
        uint32_t x1, w1 = w - x;
        if (w1 > ARRAY_SIZE(lbuf))
          w1 = ARRAY_SIZE(lbuf);
        for (x1 = 0; x1 < w1; x1++)
          lbuf[x1] = src[(sx + x + x1) & 0x3ff];
        for (x1 = 0; x1 < w1; x1++)
          dst[(dx + x + x1) & 0x3ff] = lbuf[x1] | msb;
      }
    }
  }
  else
  {
    uint32_t sy1 = sy, dy1 = dy;
    for (y = 0; y < h; y++, sy1++, dy1++) {
      memcpy(VRAM_MEM_XY(vram, dx, dy1 & GPU_VRAM_Y_MASK),
             VRAM_MEM_XY(vram, sx, sy1 & GPU_VRAM_Y_MASK), w * 2);
    }
  }

  renderer_update_caches_wrapped(dx, dy, w, h, 0);
  return 4;
}

#define gput_se11(v) (((int32_t)((uint32_t)(v) << 21)) >> 21)

/* gpu_neon texture_region_mask(): 64x256 pages, 16 columns, 4 rows. */
static uint64_t gput_region_mask(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
  int32_t x_first = x1 >> 6, x_last = x2 >> 6;
  int32_t y_first = y1 >> 8, y_last = y2 >> 8;
  uint64_t x_mask, mask = 0;
  int32_t row;
  if (x_first < 0) x_first = 0;
  if (x_first > 15) x_first = 15;
  if (x_last < 0) x_last = 0;
  if (x_last > 15) x_last = 15;
  if (y_first < 0) y_first = 0;
  if (y_first > 3) y_first = 3;
  if (y_last < 0) y_last = 0;
  if (y_last > 3) y_last = 3;
  if (x_last < x_first)
    x_mask = (0xffffULL << x_first) | ((1ULL << (x_last + 1)) - 1);
  else
    x_mask = ((1ULL << (x_last + 1)) - 1) & (0xffffULL << x_first);
  if (y_last < y_first) {
    for (row = y_first; row < 4; ++row)
      mask |= x_mask << (row * 16);
    for (row = 0; row <= y_last; ++row)
      mask |= x_mask << (row * 16);
  } else {
    for (row = y_first; row <= y_last; ++row)
      mask |= x_mask << (row * 16);
  }
  return mask;
}

/* gpu_neon's initial state: area (0,0)-(0,0) without a page mask. */
void gput_reset(struct gput_state *t)
{
  memset(t, 0, sizeof(*t));
}

/* E3/E4 update the area (and its page mask) only when the decoded corner
 * changes, E5 always sets the offset: gpu_neon gpu_parse() rules. */
void gput_ecmd(struct gput_state *t, uint32_t word)
{
  int yshift = gpu_get_model() == 1 ? 12 : 10;
  int16_t x = word & 0x3ff, y = (word >> yshift) & GPU_VRAM_Y_MASK;
  switch (word >> 24) {
    case 0xe3:
      if (x == t->x0 && y == t->y0)
        return;
      t->x0 = x; t->y0 = y;
      t->area_mask = gput_region_mask(t->x0, t->y0, t->x1, t->y1);
      break;
    case 0xe4:
      if (x == t->x1 && y == t->y1)
        return;
      t->x1 = x; t->y1 = y;
      t->area_mask = gput_region_mask(t->x0, t->y0, t->x1, t->y1);
      break;
    case 0xe5:
      t->offset_x = gput_se11(word);
      t->offset_y = gput_se11(word >> (gpu_get_model() == 1 ? 12 : 11));
      break;
  }
}

/* renderer_sync_ecmds(): E1..E6 replayed in order. */
void gput_sync(struct gput_state *t, const uint32_t *ex_regs)
{
  gput_ecmd(t, ex_regs[3]);
  gput_ecmd(t, ex_regs[4]);
  gput_ecmd(t, ex_regs[5]);
}

/* gpu_neon render_sprite() size after its clip to the drawing area. */
static int gput_clipped_sprite(const struct gput_state *t, int32_t x, int32_t y,
    int32_t w, int32_t h)
{
  int32_t x_right = x + w - 1, y_bottom = y + h - 1;
  if (!(gput_region_mask(x, y, x_right, y_bottom) & t->area_mask))
    return gput_sprite(0, 0);
  if (x < t->x0) w -= t->x0 - x;
  if (y < t->y0) h -= t->y0 - y;
  if (x_right > t->x1) w -= x_right - t->x1;
  if (y_bottom > t->y1) h -= y_bottom - t->y1;
  if (w <= 0 || h <= 0)
    return gput_sprite(0, 0);
  return gput_sprite(w, h);
}

void gput_cmd(const struct gput_state *t, const uint32_t *list, int cmd,
    int *cyc_sum, int *cyc)
{
  const int16_t *slist = (const void *)list;
  int32_t x, y, w, h;
  int c;

  switch (cmd) {
    case 0x02:
      c = gput_fill(LE16TOH(slist[4]) & 0x3ff, LE16TOH(slist[5]) & 0x1ff);
      break;
    case 0x20 ... 0x23: c = gput_poly_base();    break;
    case 0x24 ... 0x27: c = gput_poly_base_t();  break;
    case 0x28 ... 0x2b: c = gput_quad_base();    break;
    case 0x2c ... 0x2f: c = gput_quad_base_t();  break;
    case 0x30 ... 0x33: c = gput_poly_base_g();  break;
    case 0x34 ... 0x37: c = gput_poly_base_gt(); break;
    case 0x38 ... 0x3b: c = gput_quad_base_g();  break;
    case 0x3c ... 0x3f: c = gput_quad_base_gt(); break;
    case 0x40 ... 0x47:
    case 0x50 ... 0x57: c = gput_line(0);        break;
    case 0x60 ... 0x63:
    case 0x70 ... 0x73:
    case 0x78 ... 0x7b:
      x = gput_se11((int16_t)LE16TOH(slist[2]) + t->offset_x);
      y = gput_se11((int16_t)LE16TOH(slist[3]) + t->offset_y);
      w = cmd < 0x70 ? (LE16TOH(slist[4]) & 0x3ff) : cmd < 0x78 ? 8 : 16;
      h = cmd < 0x70 ? (LE16TOH(slist[5]) & 0x1ff) : cmd < 0x78 ? 8 : 16;
      c = gput_clipped_sprite(t, x, y, w, h);
      break;
    case 0x64 ... 0x67:
    case 0x74 ... 0x77:
    case 0x7c ... 0x7f:
      x = gput_se11(LE32TOH(list[1]) + t->offset_x);
      y = gput_se11((LE32TOH(list[1]) >> 16) + t->offset_y);
      w = cmd < 0x70 ? (LE32TOH(list[3]) & 0x3ff) : cmd < 0x78 ? 8 : 16;
      h = cmd < 0x70 ? ((LE32TOH(list[3]) >> 16) & GPU_VRAM_Y_MASK) : cmd < 0x78 ? 8 : 16;
      c = gput_clipped_sprite(t, x, y, w, h);
      break;
    case 0x68 ... 0x6b: c = gput_sprite(1, 1);   break;
    default:
      return;
  }
  gput_sum(*cyc_sum, *cyc, c);
}

void gput_scan(struct gput_state *t, const uint32_t *list, int count,
    int *cyc_sum, int *cyc)
{
  int pos, len, n;
  for (pos = 0; pos < count; pos += len) {
    int cmd = LE32TOH(list[pos]) >> 24;
    len = 1 + cmd_lengths[cmd];
    switch (cmd) {
      case 0x48 ... 0x4f:  /* polyline: one segment per vertex pair */
        for (n = 2; ; n++) {
          gput_sum(*cyc_sum, *cyc, gput_line(0));
          if (pos + n + 1 >= count ||
              (LE32TOH(list[pos + n + 1]) & 0xf000f000) == 0x50005000)
            break;
        }
        len += n - 2;
        break;
      case 0x58 ... 0x5f:
        for (n = 2; ; n++) {
          gput_sum(*cyc_sum, *cyc, gput_line(0));
          if (pos + n * 2 >= count ||
              (LE32TOH(list[pos + n * 2]) & 0xf000f000) == 0x50005000)
            break;
        }
        len += (n - 2) * 2;
        break;
      case 0xe3 ... 0xe5:
        gput_ecmd(t, LE32TOH(list[pos]));
        break;
      default:
        gput_cmd(t, list + pos, cmd, cyc_sum, cyc);
        break;
    }
  }
}

static noinline int do_cmd_list_skip(struct psx_gpu *gpu, uint32_t *data, int list_len,
    int *cpu_cycles_sum_out, int *cpu_cycles_last, int *last_cmd)
{
  uint32_t cyc_sum = 0, cyc = *cpu_cycles_last;
  int cmd = 0, pos, len;

  gpu->frameskip.pending_fill[0] = 0;

  for (pos = 0; pos < list_len; pos += len)
  {
    uint32_t *list = data + pos;
    const int16_t *slist = (void *)list;
    int num_vertexes, w, h;
    int dummy = 0;

    cmd = LE32TOH(list[0]) >> 24;
    len = 1 + cmd_lengths[cmd];
    if (pos + len > list_len) {
      cmd = -1;
      break; // incomplete cmd
    }

    if ((cmd & 0xf8) != 0x48 && (cmd & 0xf8) != 0x58)
      gput_cmd(&gpu->timing, list, cmd, (int *)&cyc_sum, (int *)&cyc);
    if (cmd >= 0xe3 && cmd <= 0xe5)
      gput_ecmd(&gpu->timing, LE32TOH(list[0]));

    switch (cmd) {
      case 0x02:
        w = ((LE16TOH(slist[4]) & 0x3ff) + 0xf) & ~0xf;
        h =   LE16TOH(slist[5]) & GPU_VRAM_Y_MASK;
        if (w > gpu->screen.w || h > gpu->screen.h)
        {
          // clearing something large, don't skip
          if (gpu_async_enabled(gpu))
            (void)gpu_async_do_cmd_list(gpu, list, 3, &dummy, &dummy, &dummy, &dummy);
          else
            renderer_do_cmd_list(list, 3, gpu->ex_regs, &dummy, &dummy, &dummy);
        }
        else
          memcpy(gpu->frameskip.pending_fill, list, 3 * 4);
        break;
      case 0x1f: // irq?
        goto breakloop;
      case 0x20 ... 0x23:
      case 0x28 ... 0x2b:
      case 0x30 ... 0x33:
      case 0x38 ... 0x3b:
        break;
      case 0x24 ... 0x27:
      case 0x2c ... 0x2f:
      case 0x34 ... 0x37:
      case 0x3c ... 0x3f:
#ifdef PCSX_SYSTEM12
        {
          uint32_t tpage = (LE32TOH(list[4 + ((cmd >> 4) & 1)]) >> 16);
          if (gpu_get_model() == 1) {
            gpu->ex_regs[1] = (gpu->ex_regs[1] & ~0x3fffu) | (tpage & 0x3fffu);
            gpu->type1_status = (gpu->type1_status & ~0x1fffu) | (tpage & 0x1fffu);
          }
          else {
            gpu->ex_regs[1] &= ~0x1fff;
            gpu->ex_regs[1] |= tpage & 0x1fff;
          }
        }
#else
        gpu->ex_regs[1] &= ~0x1ff;
        gpu->ex_regs[1] |= (LE32TOH(list[4 + ((cmd >> 4) & 1)]) >> 16) & 0x1ff;
#endif
        gpu->frameskip.ecmds_dirty_renderer = 1;
        break;
      case 0x48 ... 0x4F:
        for (num_vertexes = 2; ; num_vertexes++)
        {
          if (pos + num_vertexes + 1 >= list_len) {
            cmd = -1;
            goto breakloop;
          }
          if ((list[num_vertexes + 1] & LE32TOH(0xf000f000)) == LE32TOH(0x50005000))
            break;
        }
        // Counted once complete, as the direct path does.
        for (w = 1; w < num_vertexes; w++)
          gput_sum(cyc_sum, cyc, gput_line(0));
        len += (num_vertexes - 2);
        break;
      case 0x58 ... 0x5f:
        for (num_vertexes = 2; ; num_vertexes++)
        {
          if (pos + num_vertexes*2 >= list_len) {
            cmd = -1;
            goto breakloop;
          }
          if ((list[num_vertexes * 2] & LE32TOH(0xf000f000)) == LE32TOH(0x50005000))
            break;
        }
        // Counted once complete, as the direct path does.
        for (w = 1; w < num_vertexes; w++)
          gput_sum(cyc_sum, cyc, gput_line(0));
        len += (num_vertexes - 2) * 2;
        break;
      case 0x80 ... 0x9f: // vid -> vid
        w = ((LE16TOH(slist[6]) - 1) & 0x3ff) + 1;
        h = ((LE16TOH(slist[7]) - 1) & GPU_VRAM_Y_MASK) + 1;
        gput_sum(cyc_sum, cyc, gput_copy(w, h));
        break;
      case 0xa0 ... 0xbf: // sys -> vid
      case 0xc0 ... 0xdf: // vid -> sys
        goto breakloop;
      case 0xe3:
        gpu->ex_regs[cmd & 7] = LE32TOH(list[0]);
        gpu->frameskip.ecmds_dirty_renderer = 1;
        check_draw_to_display(gpu);
        if (!gpu->frameskip.allow) {
          frameskip_prepare_noskip(gpu);
          pos += len;
          goto breakloop;
        }
        break;
      case 0xe0 ... 0xe2:
      case 0xe4 ... 0xe7:
        gpu->ex_regs[cmd & 7] = LE32TOH(list[0]);
        gpu->frameskip.ecmds_dirty_renderer = 1;
        break;
      default:
        break;
    }
  }

breakloop:
  *cpu_cycles_sum_out += cyc_sum;
  *cpu_cycles_last = cyc;
  *last_cmd = cmd;
  return pos;
}

static noinline int do_cmd_buffer(struct psx_gpu *gpu, uint32_t *data, int count,
    int *cycles_sum, int *cycles_last)
{
  int cmd, pos;
  uint32_t old_e3 = gpu->ex_regs[3];
  int vram_dirty = 0;

  // process buffer
  for (pos = 0; pos < count; )
  {
    if (gpu->dma.h && !gpu->dma_start.is_read) { // XXX: need to verify
      // vram_dirty = 1; // handled in finish_vram_transfer()
      pos += do_vram_io(gpu, data + pos, count - pos, 0);
      if (pos == count)
        break;
    }

    cmd = LE32TOH(data[pos]) >> 24;
    switch (cmd & 0xe0) {
    case 0xe0:
      if (cmd < 0xe8) {
        if (gpu->ex_regs[cmd & 7] == LE32TOH(data[pos])) {
          pos++;
          continue;
        }
      }
      break;
    case 0xc0:
    case 0xa0:
      if (unlikely((pos+2) >= count)) {
        // incomplete vram write/read cmd, can't consume yet
        cmd = -1;
        break;
      }

      // consume vram write/read cmd
      start_vram_transfer(gpu, LE32TOH(data[pos + 1]),
        LE32TOH(data[pos + 2]), (cmd & 0xe0) == 0xc0);
      pos += 3;
      continue;
    case 0x80:
      if (unlikely((pos+3) >= count)) {
        cmd = -1; // incomplete cmd, can't consume yet
        break;
      }
      if (gpu_async_enabled(gpu))
        break;
      *cycles_sum += *cycles_last;
      *cycles_last = 0;
      if (do_vram_copy_pre(gpu, data + pos, cycles_last))
        do_vram_copy(gpu->vram, gpu->ex_regs, data + pos);
      pos += 4;
      continue;
    case 0x00:
      if (cmd == 2)
        break;
      if (cmd == 0x1f)
        log_anomaly(gpu, "irq1?\n");
      pos++;
      continue;
    }

    if (gpu->frameskip.active && gpu->frameskip.allow)
      pos += do_cmd_list_skip(gpu, data + pos, count - pos,
               cycles_sum, cycles_last, &cmd);
    else if (gpu_async_enabled(gpu)) {
      pos += gpu_async_do_cmd_list(gpu, data + pos, count - pos,
               cycles_sum, cycles_last, &cmd, &vram_dirty);
      // Mark as the direct path does for any renderer list, so both
      // modes present at the same vblanks.
      vram_dirty = 1;
    }
    else {
      int dummy_sum = 0, dummy_last = 0;
      int done = renderer_do_cmd_list(data + pos, count - pos, gpu->ex_regs,
               &dummy_sum, &dummy_last, &cmd);
      // Timing does not come from the renderer: see struct gput_state.
      gput_scan(&gpu->timing, data + pos, done, cycles_sum, cycles_last);
      pos += done;
      vram_dirty = 1;
    }

    if (cmd == -1)
      // incomplete cmd
      break;
  }

  // this is here because the renderer doesn't tell us if it saw e3
  if (unlikely(old_e3 != gpu->ex_regs[3]))
    check_draw_to_display(gpu);

  gpu->state.fb_dirty_display_area |= vram_dirty & gpu->state.draw_display_intersect;
  gpu->state.fb_dirty |= vram_dirty;

  return count - pos;
}

static noinline void flush_cmd_buffer(struct psx_gpu *gpu)
{
  int cycles_last = 0;
  int dummy = 0, left;
  left = do_cmd_buffer(gpu, gpu->cmd_buffer, gpu->cmd_len, &dummy, &cycles_last);
  if (left > 0)
    memmove(gpu->cmd_buffer, gpu->cmd_buffer + gpu->cmd_len - left, left * 4);
  if (left != gpu->cmd_len) {
    gpu->cmd_len = left;
    if (!gpu->dma.h && gpu->gpu_state_change)
      gpu->gpu_state_change(PGS_PRIMITIVE_START, cycles_last);
  }
}

void GPUwriteDataMem(uint32_t *mem, int count)
{
  int dummy = 0, left;

  log_io(&gpu, "gpu_dma_write %p %d cached %d\n", mem, count, gpu.cmd_len);
  GT((gt_rec(GT_DATA_MEM, count, 0), gt_put(mem, count)));

  if (unlikely(gpu.cmd_len > 0))
    flush_cmd_buffer(&gpu);

  left = do_cmd_buffer(&gpu, mem, count, &dummy, &dummy);
  if (left)
    log_anomaly(&gpu, "GPUwriteDataMem: discarded %d/%d words\n", left, count);

  sync_ecmds_status_bits(&gpu);
}

void GPUwriteData(uint32_t data)
{
  log_io(&gpu, "gpu_write %08x\n", data);
  GT(gt_rec(GT_DATA, data, 0));
  gpu.cmd_buffer[gpu.cmd_len++] = HTOLE32(data);
  if (gpu.cmd_len >= CMD_BUFFER_LEN) {
    flush_cmd_buffer(&gpu);
    sync_ecmds_status_bits(&gpu);
  }
}

long GPUdmaChain(uint32_t *rambase, uint32_t start_addr,
  uint32_t *progress_addr, int32_t *cycles_last_cmd)
{
  uint32_t addr, *list, ld_addr;
  int len, left, count, ld_count = 32;
  int cpu_cycles_sum = 0;
  int cpu_cycles_last = 0;

  preload(rambase + (start_addr & GPU_DMA_RAM_MASK) / 4);
#ifdef GPULIB_TRACE
  int traced = 0;
  GT((gt_rec(GT_CHAIN, start_addr, progress_addr != NULL), traced = 1));
#endif

  if (unlikely(gpu.cmd_len > 0))
    flush_cmd_buffer(&gpu);

  log_io(&gpu, "gpu_dma_chain\n");
  addr = ld_addr = start_addr & 0xffffff;
  for (count = 0; (addr & 0x800000) == 0; count++)
  {
    list = rambase + (addr & GPU_DMA_RAM_MASK) / 4;
    len = LE32TOH(list[0]) >> 24;
    addr = LE32TOH(list[0]) & 0xffffff;
    preload(rambase + (addr & GPU_DMA_RAM_MASK) / 4);
#ifdef GPULIB_TRACE
    if (traced) {
      gt_rec(GT_CHAIN_PKT, (uint32_t)(list - rambase) * 4, len);
      gt_put(list, len + 1);
    }
#endif

    cpu_cycles_sum += 10;
    if (len > 0)
      cpu_cycles_sum += 5 + len;

    log_io(&gpu, ".chain %08lx #%d+%d %u+%u\n",
      (long)(list - rambase) * 4, len, gpu.cmd_len, cpu_cycles_sum, cpu_cycles_last);
    if (unlikely(gpu.cmd_len > 0)) {
      if (gpu.cmd_len + len > ARRAY_SIZE(gpu.cmd_buffer)) {
        log_anomaly(&gpu, "cmd_buffer overflow, likely garbage commands\n");
        gpu.cmd_len = 0;
      }
      memcpy(gpu.cmd_buffer + gpu.cmd_len, list + 1, len * 4);
      gpu.cmd_len += len;
      flush_cmd_buffer(&gpu);
      continue;
    }

    if (len) {
      left = do_cmd_buffer(&gpu, list + 1, len, &cpu_cycles_sum, &cpu_cycles_last);
      if (left) {
        memcpy(gpu.cmd_buffer, list + 1 + len - left, left * 4);
        gpu.cmd_len = left;
        log_anomaly(&gpu, "GPUdmaChain: %d/%d words left\n", left, len);
      }
    }

    if (progress_addr) {
      // hack for bios boot logo race (must be not too fast or too slow)
      if (gpu.status & PSX_GPU_STATUS_DHEIGHT)
        cpu_cycles_sum += 5;
      if (cpu_cycles_sum > 512)
        break;
    }
    if (addr == ld_addr) {
      log_anomaly(&gpu, "GPUdmaChain: loop @ %08x, cnt=%u\n", addr, count);
      break;
    }
    if (count == ld_count) {
      ld_addr = addr;
      ld_count *= 2;
    }
  }

  sync_ecmds_status_bits(&gpu);
#ifdef GPULIB_TRACE
  if (traced)
    gt_rec(GT_CHAIN_END, 0, 0);
#endif

  //printf(" -> %d %d\n", cpu_cycles_sum, cpu_cycles_last);
  gpu.state.last_list.frame = *gpu.state.frame_count;
  gpu.state.last_list.hcnt = *gpu.state.hcnt;
  gpu.state.last_list.cycles = cpu_cycles_sum + cpu_cycles_last;
  gpu.state.last_list.addr = start_addr;

  if (progress_addr)
    *progress_addr = addr;
  *cycles_last_cmd = cpu_cycles_last;
  return cpu_cycles_sum;
}

void GPUreadDataMem(uint32_t *mem, int count)
{
  log_io(&gpu, "gpu_dma_read  %p %d\n", mem, count);
  GT(gt_rec(GT_READ_MEM, count, 0));

  if (unlikely(gpu.cmd_len > 0)) {
    flush_cmd_buffer(&gpu);
    sync_ecmds_status_bits(&gpu);
  }

  if (gpu.dma.h)
    do_vram_io(&gpu, mem, count, 1);
}

uint32_t GPUreadData(void)
{
  uint32_t ret;

  GT(gt_rec(GT_READ, 0, 0));
  if (unlikely(gpu.cmd_len > 0)) {
    flush_cmd_buffer(&gpu);
    sync_ecmds_status_bits(&gpu);
  }

  ret = gpu.gp0;
  if (gpu.dma.h) {
    ret = HTOLE32(ret);
    do_vram_io(&gpu, &ret, 1, 1);
    ret = LE32TOH(ret);
  }

  log_io(&gpu, "gpu_read %08x\n", ret);
  return ret;
}

uint32_t GPUreadStatus(void)
{
  uint32_t ret;

  GT(gt_rec(GT_READ_STATUS, 0, 0));
  if (unlikely(gpu.cmd_len > 0)) {
    flush_cmd_buffer(&gpu);
    sync_ecmds_status_bits(&gpu);
  }

  ret = gpu.status;
  log_io(&gpu, "gpu_read_status %08x\n", ret);
  return ret;
}

long GPUfreeze(uint32_t type, GPUFreeze_t *freeze, uint16_t **vram_ptr)
{
  int i;

  switch (type) {
    case 1: // save
      if (gpu.cmd_len > 0) {
        flush_cmd_buffer(&gpu);
        sync_ecmds_status_bits(&gpu);
      }

      sync_renderer(&gpu);
#ifdef GPU_VITA_CTR
      renderer_sync_vram_to_cpu(0, 0, 1024, gpu_vram_height);
#endif
      memcpy(freeze->ulControl, gpu.regs, sizeof(gpu.regs));
      memcpy(freeze->ulControl + 0xe0, gpu.ex_regs, sizeof(gpu.ex_regs));
      freeze->ulStatus = gpu.status;
      break;
    case 0: // load
      sync_renderer(&gpu);
      //memcpy(gpu.regs, freeze->ulControl, sizeof(gpu.regs));
      memcpy(gpu.ex_regs, freeze->ulControl + 0xe0, sizeof(gpu.ex_regs));
      gpu.status = freeze->ulStatus;
      gpu.cmd_len = 0;
      for (i = 8; i > 1; i--)
        GPUwriteStatus((i << 24) | freeze->ulControl[i]);
      gput_sync(&gpu.timing, gpu.ex_regs);
      renderer_sync_ecmds(gpu.ex_regs);
      /* The Vita renderer copies guest VRAM when this is called: LoadState
       * refreshes it after the saved VRAM has been read in. */
#ifndef GPU_VITA_CTR
      renderer_update_caches(0, 0, 1024, gpu_vram_height, 0);
#endif
      gpu_async_sync_ecmds(&gpu);
      break;
  }

  *vram_ptr = gpu.vram;
  return 1;
}

static void GPUupdateLace(void)
{
  int delay_vout_update = 0;
  int updated = 1;

  if (gpu.frameskip.set) {
    uint32_t flip_delay = *gpu.state.frame_count - gpu.frameskip.last_flip_frame;
    if (gpu_async_enabled(&gpu))
      gpu_async_try_delayed_flip(&gpu, 0);
    if (flip_delay > FRAMESKIP_MAX_FLIP_DELAY) {
      gpu.frameskip.frame_ready = 1;
      if (gpu.frameskip.active) {
        gpu.frameskip.active = 0;
        frameskip_prepare_noskip(&gpu);
      }
    }
  }

  if (gpu.cmd_len > 0) {
    flush_cmd_buffer(&gpu);
    sync_ecmds_status_bits(&gpu);
  }

#ifndef RAW_FB_DISPLAY
  if (gpu.status & PSX_GPU_STATUS_BLANKING) {
    if (!gpu.state.blanked) {
      vout_blank(&gpu);
      gpu.state.blanked = 1;
      gpu.state.fb_dirty_display_area = 1;
      gpu.state.use_alternative_flip = 0;
    }
    return;
  }

  if (!gpu.state.fb_dirty_display_area)
    return;

  if (gpu.frameskip.set) {
    if (!gpu.frameskip.frame_ready)
      return;
    gpu.frameskip.frame_ready = 0;
  }
#endif

#ifdef GPU_VITA_CTR
  // vout_update queues the present behind the frame's commands.
#else
  if (gpu_async_enabled(&gpu))
    delay_vout_update = gpu_async_sync_scanout(&gpu);
  else
    renderer_flush_queues();
#endif

  if (!delay_vout_update)
    updated = vout_update(&gpu, gpu.screen.src_x, gpu.screen.src_y);
  if (gpu.state.enhancement_active && !gpu.state.enhancement_was_active) {
    gpu_async_sync(&gpu);
    renderer_update_caches(0, 0, 1024, gpu_vram_height, 1);
  }
  gpu.state.enhancement_was_active = gpu.state.enhancement_active;
  if (updated) {
    gpu.state.fb_dirty_display_area = 0;
    gpu.state.blanked = 0;
  }
}

void GPUvBlank(int is_vblank, int lcf)
{
  int interlace;
  GT((gt_rec(GT_VBLANK, is_vblank | (lcf << 1), *gpu.state.frame_count), gt_put(gpu.state.hcnt, 1)));
  interlace = gpu.state.allow_interlace
    && (gpu.status & PSX_GPU_STATUS_INTERLACE)
    && (gpu.status & PSX_GPU_STATUS_DHEIGHT);
  // interlace doesn't look nice on progressive displays,
  // so we have this "auto" mode here for games that don't read vram
  if (gpu.state.allow_interlace == 2
      && *gpu.state.frame_count - gpu.state.last_vram_read_frame > 1)
  {
    interlace = 0;
  }
  if (interlace || interlace != gpu.state.old_interlace) {
    gpu.state.old_interlace = interlace;

    if (gpu.cmd_len > 0) {
      flush_cmd_buffer(&gpu);
      sync_ecmds_status_bits(&gpu);
    }
    if (gpu_async_enabled(&gpu))
      gpu_async_set_interlace(&gpu, interlace, !lcf);
    else {
      renderer_flush_queues();
      renderer_set_interlace(interlace, !lcf);
    }
  }
  if ((!is_vblank) == gpu.state.use_alternative_flip)
    GPUupdateLace();
}

void GPUgetScreenInfo(int *y, int *base_hres)
{
  *y = gpu.screen.y;
  *base_hres = gpu.screen.vres;
  if (gpu.status & PSX_GPU_STATUS_DHEIGHT)
    *base_hres >>= 1;
}

void GPUrearmedCallbacks(const struct rearmed_cbs *cbs)
{
#ifdef GPULIB_TRACE
  gtrace.cbs = cbs;
#endif
  gpu.frameskip.set = cbs->frameskip;
  gpu.frameskip.advice = &cbs->fskip_advice;
  gpu.frameskip.force = &cbs->fskip_force;
  gpu.frameskip.dirty = (void *)&cbs->fskip_dirty;
  gpu.frameskip.active = 0;
  gpu.frameskip.frame_ready = 1;
  gpu.frameskip.last_flip_frame = *cbs->gpu_frame_count - FRAMESKIP_MAX_FLIP_DELAY - 1;
  gpu.state.hcnt = (uint32_t *)cbs->gpu_hcnt;
  gpu.state.frame_count = (uint32_t *)cbs->gpu_frame_count;
  gpu.state.allow_interlace = cbs->gpu_neon.allow_interlace;
  if (gpu.state.allow_interlace > 1)
    gpu.state.allow_interlace = (cbs->gpu_hacks & GPU_HACK_NEEDS_INTERLACE) ? 3 : 2;
  gpu.state.enhancement_enable = cbs->gpu_neon.enhancement_enable;
  gpu.state.enhancement_active = 0;
  gpu.state.downscale_enable = cbs->scale_hires;
  gpu.state.use_alternative_flip = cbs->alt_flip;
  gpu.state.screen_centering_type_default = cbs->screen_centering_type_default;
  if (gpu.state.screen_centering_type != cbs->screen_centering_type
      || gpu.state.screen_centering_x != cbs->screen_centering_x
      || gpu.state.screen_centering_y != cbs->screen_centering_y
      || gpu.state.screen_centering_h_adj != cbs->screen_centering_h_adj
      || gpu.state.show_overscan != cbs->show_overscan) {
    gpu.state.screen_centering_type = cbs->screen_centering_type;
    gpu.state.screen_centering_x = cbs->screen_centering_x;
    gpu.state.screen_centering_y = cbs->screen_centering_y;
    gpu.state.screen_centering_h_adj = cbs->screen_centering_h_adj;
    gpu.state.show_overscan = cbs->show_overscan;
    update_width(&gpu);
    update_height(&gpu);
  }

  gpu.mmap = cbs->mmap;
  gpu.munmap = cbs->munmap;
  gpu.gpu_state_change = cbs->gpu_state_change;

  // delayed vram mmap
  if (gpu.vram == NULL)
    map_vram();

  if (cbs->pl_vout_set_raw_vram)
    cbs->pl_vout_set_raw_vram(gpu.vram);
  sync_renderer(&gpu);
  sync_renderer_ecmds(&gpu);
  renderer_set_config(cbs);
  vout_set_config(cbs);

  gpu_async_enable(&gpu, cbs->thread_rendering);
}

// for standalone plugins only
#ifdef GPULIB_PLUGIN
#include <stdarg.h>
void SysPrintf(const char *fmt, ...)
{
   va_list list;
   va_start(list, fmt);
   vfprintf(stderr, fmt, list);
   va_end(list);
}
#endif

// vim:shiftwidth=2:expandtab
