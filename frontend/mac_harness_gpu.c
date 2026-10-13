/* GPULIB access kept apart from the core's PSEmu typedef names. */
#include <stdio.h>
#include <stdlib.h>
#include "../plugins/gpulib/gpu.h"
#ifdef GPU_VITA_CTR
#include "../plugins/gpu_vita_ctr/include/platform/mac_vita_compat.h"
static int capture_gl_requested;
static int capture_gl_done;
static unsigned int capture_gl_target_frame;
#endif

static unsigned char *presented_rgb;
static int presented_w, presented_h, presented_valid;
static int presented_src_x, presented_src_y, presented_rgb24;

void harness_capture_mode(int w, int h)
{
  unsigned char *rgb;
  if (w <= 0 || w > 1024 || h <= 0 || h > (int)gpu_vram_height) return;
  if (w == presented_w && h == presented_h && presented_rgb) return;
  rgb = calloc((size_t)w * h, 3);
  free(presented_rgb);
  presented_rgb = rgb;
  presented_w = w; presented_h = h; presented_valid = 0;
}

void harness_capture_flip(const void *vram, int offset, int rgb24,
                          int x, int y, int w, int h)
{
  const unsigned char *bytes = vram;
  int row, col;
  /* Blank callbacks have no framebuffer; retain the last rendered frame. */
  if (!bytes || !presented_rgb || w <= 0 || h <= 0) return;
  memset(presented_rgb, 0, (size_t)presented_w * presented_h * 3);
  for (row = 0; row < h; row++) {
    int dst_y = y + row;
    int vram_mask = 1024 * (int)gpu_vram_height * 2 - 1;
    int line_offset = (offset + row * 2048) & vram_mask;
    if (dst_y < 0 || dst_y >= presented_h) continue;
    for (col = 0; col < w; col++) {
      int dst_x = x + col;
      unsigned char *rgb;
      if (dst_x < 0 || dst_x >= presented_w) continue;
      rgb = presented_rgb + ((size_t)dst_y * presented_w + dst_x) * 3;
      if (rgb24) {
        int pos = (line_offset + col * 3) & vram_mask;
        rgb[0] = bytes[pos];
        rgb[1] = bytes[(pos + 1) & vram_mask];
        rgb[2] = bytes[(pos + 2) & vram_mask];
      }
      else {
        int pos = (line_offset + col * 2) & vram_mask;
        unsigned short p = bytes[pos] | (bytes[pos + 1] << 8);
        rgb[0] = ((p & 31) * 255 + 15) / 31;
        rgb[1] = (((p >> 5) & 31) * 255 + 15) / 31;
        rgb[2] = (((p >> 10) & 31) * 255 + 15) / 31;
      }
    }
  }
  presented_valid = 1;
  presented_src_x = (offset % 2048) / 2;
  presented_src_y = offset / 2048;
  presented_rgb24 = rgb24;
}

unsigned short harness_vram_pixel(int x, int y)
{
  return gpu.vram[y * 1024 + x];
}

static void harness_gp0(uint32_t data)
{
  GPUwriteData(data);
}

static void harness_upload_pixel(int x, int y, uint16_t pixel)
{
  harness_gp0(0xa0000000);
  harness_gp0((uint32_t)x | ((uint32_t)y << 16));
  harness_gp0(1 | (1u << 16));
  harness_gp0(pixel | ((uint32_t)pixel << 16));
}

/* Exercise type-2 textures and uploads with the board's actual VRAM size.
 * A 2 MiB board additionally covers TPAGE bit 11 and 10-bit CLUT rows. */
int harness_gpu_vram_selftest(void)
{
  const int upper = gpu_vram_height == GPU_VRAM_ROWS_SYSTEM12;
  const unsigned tex_y = upper ? 512 : 256, clut_y = upper ? 800 : 400, dst_y = upper ? 600 : 300;
  const unsigned tpage_row = upper ? 0x800 : 0x10;
  const unsigned tex_x = 0, clut_x = 1, dst_x = 100;
  uint16_t old_texel, old_odd_texel, old_clut, old_dst[4], got[4];
  unsigned i;
  if ((gpu_vram_height != 512 && !upper) || gpu_get_model() != 2 || gpu.vram == NULL)
    return 1;
  old_texel = harness_vram_pixel(tex_x, tex_y);
  old_clut = harness_vram_pixel(clut_x, clut_y);
  old_odd_texel = harness_vram_pixel(64, tex_y);
  for (i = 0; i < 4; i++)
    old_dst[i] = harness_vram_pixel(dst_x + i, dst_y);

  harness_upload_pixel(tex_x, tex_y, 0x0001); /* 4bpp texel index 1 */
  harness_upload_pixel(clut_x, clut_y, 0x7c00); /* blue CLUT entry 1 */
  harness_gp0(0xe3000000); /* draw area top left */
  harness_gp0(0xe4000000 | 1023u | ((gpu_vram_height - 1u) << 10));
  for (i = 0; i < 2; i++) {
    /* Bit 7 selects 8bpp on the second pass. */
    harness_gp0(0xe1000000 | tpage_row | (i ? 0x80 : 0));
    harness_gp0(0x64000000 | 0x808080); /* textured 1x1 sprite */
    harness_gp0((dst_x + i) | (dst_y << 16));
    harness_gp0((clut_y * 64u << 16)); /* CLUT at x=0 */
    harness_gp0(1 | (1u << 16));
  }
  /* Switching 8bpp parity banks must preserve high dirty-mask bits. Page 33
   * is independently cached; a 32-bit mask temporary skips its first decode. */
  harness_upload_pixel(64, tex_y, 0x0001);
  harness_gp0(0xe1000081 | tpage_row); /* odd 8bpp page, x=64 */
  harness_gp0(0x64808080);
  harness_gp0((dst_x + 3) | (dst_y << 16));
  harness_gp0((clut_y * 64u << 16));
  harness_gp0(1 | (1u << 16));
  /* In 16bpp mode the texture word is a direct color, with no CLUT lookup. */
  harness_upload_pixel(tex_x, tex_y, 0x03e0);
  harness_gp0(0xe1000100 | tpage_row); /* type-2 row, 16bpp */
  harness_gp0(0x64000000 | 0x808080);
  harness_gp0((dst_x + 2) | (dst_y << 16));
  harness_gp0(0);
  harness_gp0(1 | (1u << 16));
  (void)GPUreadStatus();
  renderer_flush_queues();
  for (i = 0; i < 4; i++)
    got[i] = harness_vram_pixel(dst_x + i, dst_y);

  if (harness_vram_pixel(tex_x, tex_y) != 0x03e0 ||
      harness_vram_pixel(clut_x, clut_y) != 0x7c00 ||
      got[0] != 0x7c00 || got[1] != 0x7c00 || got[2] != 0x03e0 || got[3] != 0x7c00)
    fprintf(stderr, "GPU VRAM selftest values: source=%04x clut=%04x dst=%04x/%04x/%04x/%04x\n",
      harness_vram_pixel(tex_x, tex_y),
      harness_vram_pixel(clut_x, clut_y), got[0], got[1], got[2], got[3]);

  gpu.vram[tex_y * 1024 + tex_x] = old_texel;
  gpu.vram[clut_y * 1024 + clut_x] = old_clut;
  gpu.vram[tex_y * 1024 + 64] = old_odd_texel;
  for (i = 0; i < 4; i++)
    gpu.vram[dst_y * 1024 + dst_x + i] = old_dst[i];
  renderer_update_caches(0, tex_y, 80, 1, 0);
  renderer_update_caches(0, clut_y, 16, 1, 0);
  renderer_update_caches(dst_x, dst_y, 4, 1, 0);
  (void)GPUreadStatus();
  GPUwriteStatus(0x00000000); /* restore normal draw-area/status state */
  int pass = got[0] == 0x7c00 && got[1] == 0x7c00 && got[2] == 0x03e0 && got[3] == 0x7c00;
  fprintf(stderr, "GPU %s VRAM 4bpp/8bpp/16bpp + odd-cache selftest: %s\n",
    upper ? "upper" : "base", pass ? "PASS" : "FAIL");
  return pass ? 0 : 2;
}

static int harness_type1_textured_pixel(unsigned dst_x, unsigned dst_y,
                                        unsigned uv, uint16_t expected)
{
  harness_gp0(0x64000000 | 0x808080);
  harness_gp0(dst_x | (dst_y << 16));
  harness_gp0((400u * 64u << 16) | uv);
  harness_gp0(1u | (1u << 16));
  (void)GPUreadStatus();
  renderer_flush_queues();
  if (harness_vram_pixel(dst_x, dst_y) != expected) {
    fprintf(stderr, "GPU type-1 pixel (%u,%u) uv=%04x got=%04x expected=%04x\n",
      dst_x, dst_y, uv, harness_vram_pixel(dst_x, dst_y), expected);
    return 0;
  }
  return 1;
}

/* Exercise type-1 field packing and its MAME-verified indexed texture
 * interleave equations. This is GPU-model coverage, not board/game support. */
int harness_gpu_type1_selftest(void)
{
  const int upper = gpu_vram_height == GPU_VRAM_ROWS_SYSTEM12;
  const unsigned dst_x = 100, dst_y = upper ? 600 : 300;
  const unsigned area_top = upper ? 513 : 257, area_bottom = upper ? 700 : 450;
  const unsigned row256 = 256u * 1024u;
  const uint16_t color4 = 0x7c00, color8 = 0x03e0, color16 = 0x001f;
  const unsigned touched[] = {
    400u * 1024u + 3, 400u * 1024u + 6,
    400u * 1024u + 5, 400u * 1024u + 7,
    row256 + 4, row256 + 1024u, row256 + 64u + 8,
    row256 + 1024u + 64, row256 + 1024u + 2,
    dst_y * 1024u + dst_x, dst_y * 1024u + dst_x + 1,
    dst_y * 1024u + dst_x + 2, dst_y * 1024u + dst_x + 3,
    dst_y * 1024u + dst_x + 4
  };
  uint16_t saved[sizeof(touched) / sizeof(touched[0])];
  unsigned i;
  uint32_t got;
  int pass = 1;
  if (gpu_get_model() != 1 || (gpu_vram_height != 512 && !upper)
      || gpu.vram == NULL)
    return 1;
  renderer_flush_queues();
  for (i = 0; i < sizeof(touched) / sizeof(touched[0]); i++)
    saved[i] = gpu.vram[touched[i]];

  /* GP0 E3/E4 encode Y at bit 12; E5's signed Y offset also starts there. */
  harness_gp0(0xe3000000u | 17u | (area_top << 12));
  harness_gp0(0xe4000000u | 900u | (area_bottom << 12));
  harness_gp0(0xe5000000u | 0x7fdu | (0x7fbu << 12));
  (void)GPUreadStatus();
  GPUwriteStatus(0x10000003u); got = GPUreadData();
  pass &= got == (17u | (area_top << 12));
  GPUwriteStatus(0x10000004u); got = GPUreadData();
  pass &= got == (900u | (area_bottom << 12));
  GPUwriteStatus(0x10000005u); got = GPUreadData();
  pass &= got == (0x7fdu | (0x7fbu << 12));
  GPUwriteStatus(0x10000007u); pass &= GPUreadData() == 1;
  GPUwriteStatus(0x05000000u | 5u | (37u << 12));
  pass &= gpu.screen.src_y == 37;
  harness_gp0(0xe6000003u);
  pass &= (GPUreadStatus() & 0x1800u) == 0x1800u;

  /* Restore a full drawable viewport and zero offset for pixel checks. */
  harness_gp0(0xe3000000u);
  harness_gp0(0xe4ffffffu);
  harness_gp0(0xe5000000u);
  harness_gp0(0xe6000000u);
  gpu.vram[400u * 1024u + 3] = color4;
  gpu.vram[400u * 1024u + 6] = color8;
  gpu.vram[400u * 1024u + 5] = color8;
  gpu.vram[400u * 1024u + 7] = 0x7c1f;
  gpu.vram[row256 + 4] = 3;       /* MAME 4bpp map: U=0,V=1 -> word X=4 */
  gpu.vram[row256 + 1024u] = 6;   /* U=16,V=0 -> word X=0, row 1 */
  gpu.vram[row256 + 64u + 8] = 5; /* MAME 8bpp map: page X=64, U=0,V=1 -> X=72 */
  gpu.vram[row256 + 1024u + 64] = 7u << 8; /* U=33,V=0 -> page X=64, row 1 */
  gpu.vram[row256 + 1024u + 2] = color16; /* 15bpp path remains linear */
  renderer_update_caches(0, 256, 1024, 2, 0);

  /* type 1: bit 5 selects Y=256; bit 13 enables indexed interleave. */
  harness_gp0(0xe1000000u | 0x2020u);
  (void)GPUreadStatus();
  pass &= (GPUreadStatus() & 0x1fffu) == 0x0020u;
  pass &= harness_type1_textured_pixel(dst_x, dst_y, 1u << 8, color4);
  pass &= harness_type1_textured_pixel(dst_x + 3, dst_y, 16u, color8);

  /* Type-1 bits 9/10 select 8bpp/15bpp (rather than type-2 bits 7/8). */
  harness_gp0(0xe1000000u | 0x2221u); /* odd X page exercises paired cache */
  (void)GPUreadStatus();
  pass &= harness_type1_textured_pixel(dst_x + 1, dst_y, 1u << 8, color8);
  pass &= harness_type1_textured_pixel(dst_x + 4, dst_y, 33u, 0x7c1f);
  harness_gp0(0xe1000000u | 0x2420u);
  (void)GPUreadStatus();
  pass &= harness_type1_textured_pixel(dst_x + 2, dst_y, 2u | (1u << 8), color16);

  for (i = 0; i < sizeof(touched) / sizeof(touched[0]); i++)
    gpu.vram[touched[i]] = saved[i];
  renderer_update_caches(0, 256, 1024, 2, 0);
  renderer_update_caches(0, 400, 16, 1, 0);
  renderer_update_caches(dst_x, dst_y, 5, 1, 0);
  GPUwriteStatus(0x00000000);
  fprintf(stderr, "GPU type-1 rows=%u command fields + 4/8/16bpp interleave selftest: %s\n",
    gpu_vram_height, pass ? "PASS" : "FAIL");
  return pass ? 0 : 2;
}

int harness_write_vram(const char *path)
{
  unsigned char header[16] = {'P', 'S', 'X', 'V', 'R', 'A', 'M', '1', 0, 4, 0, 0};
  unsigned char row[2048];
  unsigned x, y;
  FILE *file;
  if (gpu.vram == NULL) return -1;
  GPUreadStatus();
#ifdef GPU_VITA_CTR
  renderer_sync_vram_to_cpu(0, 0, 1024, gpu_vram_height);
#else
  renderer_flush_queues();
#endif
  header[12] = gpu_vram_height & 255;
  header[13] = (gpu_vram_height >> 8) & 255;
  file = fopen(path, "wb");
  if (!file) { perror(path); return -1; }
  if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
    fclose(file); return -1;
  }
  for (y = 0; y < gpu_vram_height; y++) {
    for (x = 0; x < 1024; x++) {
      unsigned short p = gpu.vram[y * 1024 + x];
      row[x * 2] = p & 255;
      row[x * 2 + 1] = p >> 8;
    }
    if (fwrite(row, 1, sizeof(row), file) != sizeof(row)) {
      fclose(file); return -1;
    }
  }
  fprintf(stderr, "VRAM dump=%ux%u format=LE16 source=end-of-execution-synchronized\n",
    1024u, gpu_vram_height);
  return fclose(file);
}

int harness_write_ppm(const char *path, int w, int h)
{
#ifdef GPU_VITA_CTR
  if (capture_gl_requested) {
    int result = PCSXMacVitaGL_WriteCapturePpm(path);
    if (result <= 0) {
      fprintf(stderr, "OpenGL capture requested but no frame was captured\n");
      return -1;
    }
    return 0;
  }
#endif
  FILE *fp = fopen(path, "wb");
  int x, y, src_x, src_y, rgb24;
  if (!fp) { perror(path); return -1; }
  GPUreadStatus();
  renderer_flush_queues();
  if (presented_valid) {
    size_t size = (size_t)presented_w * presented_h * 3;
    fprintf(stderr, "capture=%dx%d origin=%d,%d format=%s display=%s source=last-rendered-flip\n",
      presented_w, presented_h, presented_src_x, presented_src_y,
      presented_rgb24 ? "RGB24" : "BGR555",
      gpu.status & PSX_GPU_STATUS_BLANKING ? "disabled" : "enabled");
    fprintf(fp, "P6\n%d %d\n255\n", presented_w, presented_h);
    if (fwrite(presented_rgb, 1, size, fp) != size) { fclose(fp); return -1; }
    return fclose(fp);
  }
  if (w <= 0) w = gpu.screen.hres;
  if (h <= 0) h = gpu.screen.vres;
  if (w <= 0 || w > 1024) w = 320;
  if (h <= 0 || h > (int)gpu_vram_height) h = 240;
  src_x = gpu.screen.src_x;
  src_y = gpu.screen.src_y;
  rgb24 = !!(gpu.status & PSX_GPU_STATUS_RGB24);
  fprintf(stderr, "capture=%dx%d origin=%d,%d format=%s display=%s source=instant-vram-fallback\n",
    w, h, src_x, src_y, rgb24 ? "RGB24" : "BGR555",
    gpu.status & PSX_GPU_STATUS_BLANKING ? "disabled" : "enabled");
  fprintf(fp, "P6\n%d %d\n255\n", w, h);
  for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
    unsigned short p = harness_vram_pixel((src_x + x) & 1023,
                                           (src_y + y) & GPU_VRAM_Y_MASK);
    unsigned char rgb[3] = {
      ((p & 31) * 255 + 15) / 31,
      (((p >> 5) & 31) * 255 + 15) / 31,
      (((p >> 10) & 31) * 255 + 15) / 31
    };
    if (rgb24) {
      const unsigned char *line = (const unsigned char *)&gpu.vram[
        ((src_y + y) & GPU_VRAM_Y_MASK) * 1024];
      int offset = src_x * 2 + x * 3;
      rgb[0] = line[offset & 2047];
      rgb[1] = line[(offset + 1) & 2047];
      rgb[2] = line[(offset + 2) & 2047];
    }
    if (fwrite(rgb, 1, 3, fp) != 3) { fclose(fp); return -1; }
  }
  return fclose(fp);
}

int harness_configure_gl_capture(int enabled, unsigned int target_frame)
{
#ifdef GPU_VITA_CTR
  capture_gl_requested = enabled;
  capture_gl_done = 0;
  capture_gl_target_frame = target_frame;
  return 0;
#else
  (void)target_frame;
  return enabled ? -1 : 0;
#endif
}

void harness_capture_gl_frame(unsigned int frame)
{
#ifdef GPU_VITA_CTR
  /* A 30 Hz game may not present on the final requested vsync. Keep the
   * latest actual presentation in the final eight vsyncs (also for 15 Hz). */
  if (capture_gl_requested && (uint64_t)frame + 7 >= capture_gl_target_frame) {
    capture_gl_done = 1;
    if (!PCSXMacVitaGL_CaptureCurrentFrame())
      fprintf(stderr, "OpenGL capture failed at emulated frame %u\n", frame);
  }
#else
  (void)frame;
#endif
}
