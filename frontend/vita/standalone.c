#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/touch.h>

#include <arm_neon.h>
#include <vita2d.h>
#include <SDL3/SDL.h>
#include <libretro.h>
#include "validation.h"
#include "../../libpcsxcore/system12_profile.h"
#include "input_csv_reader.h"
#include "../gun_pointer.h"
#include "vita_gpuprof.h"
/* Menu mode: the vitaGL executable (selector, pause menu, options) and PCSX
 * ReArcade's NEON one (a game the selector hands it, with the pause menu). */
#if defined(STANDALONE_VITAGL) || defined(REARCADE_NEON)
#define STANDALONE_UI
#include <psp2/appmgr.h>
#include "rom_selector.h"
#include "settings.h"
#include "savestates.h"
#endif
#ifdef STANDALONE_VITAGL
#ifdef ARCADE_SHELF
#include "shelf_selector.h"
#include "shelf_pause.h"
#include "shelf_common.h"
#endif
/* gpulib: drains the GPU thread and the renderer (plugins/gpulib/gpu.c). */
void gpu_sync_renderer(void);
#endif
#ifdef REARCADE_NEON
#include "gl_present.h"
#include "shelf_pause.h"
#include "shelf_common.h"
#endif

#include "app_root.h"
#define BIOS_DIR APP_ROOT "/bios"
#define SAVE_DIR APP_ROOT "/saves"

#define DISPLAY_W 960
#define DISPLAY_H 544
#define DISPLAY_PITCH 960
#define DISPLAY_MEM_SIZE (2 * 1024 * 1024)
#define GAME_W 725
#define GAME_X ((DISPLAY_W - GAME_W) / 2)

#define AUDIO_DEVICE_FRAMES 1024
#define AUDIO_DEVICE_RATE 48000
/* The emulation waits while more than this much audio is queued: three
 * device buffers (64 ms) at any source rate. A fixed 2048 source frames was
 * 23 ms at System 12's 88200 Hz, barely one device buffer: a late frame left
 * the device without data, playback stalled and the game ran at 59.0 fps. */
#define AUDIO_QUEUE_TARGET_DEVICE_BUFFERS 3
#define AUDIO_FRAME_BYTES (sizeof(int16_t) * 2)
#define INPUT_CSV_MAX_BYTES (1024u * 1024u)
#define INPUT_CSV_MAX_EVENTS 4096u
#define INPUT_CSV_LINE_BYTES 256u

static SceUID g_fb_uid[2] = { -1, -1 };
static uint32_t *g_fb[2];
static int g_front;
static int g_map_w = -1;
static int g_map_h = -1;
static uint16_t g_xmap[GAME_W];
static uint16_t g_ymap[DISPLAY_H];
static char g_report_path[1024];
static char g_capture_path[1024];
/* --progress-every N: frames between progress lines (600). */
static unsigned g_progress_every = 600;
static uint16_t *g_capture_pixels;
static size_t g_capture_capacity;
static unsigned g_capture_width;
static unsigned g_capture_height;
static uint8_t *g_capture_rgb;
#ifdef STANDALONE_VITAGL
#include <vitaGL.h>
/* The vitaGL GPU presents by itself, so video_cb never sees its pixels: it
 * reads its next picture back once asked, into capture_gl_frame. */
extern volatile int pcsx_vita_gl_capture_request;
extern volatile int pcsx_menu_backdrop_request;
extern void (*pcsx_vita_gl_capture_store)(const uint8_t *rgba, unsigned y, unsigned rows,
                                          unsigned width, unsigned height);
#endif
static char g_inputs_path[1024];

typedef struct {
    unsigned frame;
    unsigned port;
    unsigned id;
    unsigned pressed;
    char control[32];
} vita_input_event;

static vita_input_event g_input_events[INPUT_CSV_MAX_EVENTS];
static unsigned g_input_event_count;
static unsigned g_next_input_event;
static unsigned g_input_frame;
static uint16_t g_replay_joypad[2];
static bool g_replay_active;

static SceCtrlData g_pad;
static int g_exit_hold;
static bool g_exit_requested;
/* Menu mode (vitaGL builds started without a game): the ROM selector picks
 * the game, Select+Start opens the pause menu, and core options come from
 * settings.cfg. A game named on the command line or in args.txt runs as
 * before, with the fixed options below, so validation runs do not change. */
static bool g_ui_mode;
static bool g_pause_requested;
static bool g_pause_combo_down;

static SDL_AudioStream *g_audio_stream;
static uint32_t g_audio_put_errors;
static int g_audio_queue_target_bytes;
/* --pacing: vblank (default) shows one emulated frame per display refresh
 * (59.94 Hz) and resamples audio dynamically to keep its queue level; audio
 * runs the game at its own rate (59.83 Hz for System 12, as the arcade), the
 * audio queue being the clock, at the cost of a repeated frame every ~9 s. */
enum { PACING_VBLANK, PACING_AUDIO };
static int g_pacing = PACING_VBLANK;
static float g_audio_base_ratio = 1.0f; /* display rate / core frame rate */
static float g_audio_ratio = 1.0f;
static int g_last_vcount = -1;
static unsigned g_audio_peak;
static uint64_t g_audio_nonzero;
static uint64_t g_audio_put_frames; /* source frames handed to SDL */

static SceUID g_vm_block = -1;
extern int _newlib_vm_size_user;
#if defined(STANDALONE_VITAGL) || defined(REARCADE_NEON)
/* Board ROMs live in the newlib heap (77 MB for Tekken Tag) and vitaGL's
 * runtime shader translation allocates there too: the default 128 MB ran out
 * at renderer start. vitaGL keeps the memory left after this. */
int _newlib_heap_size_user = 160 * 1024 * 1024;
#endif

int getVMBlock(void)
{
    int size;

    if (g_vm_block > 0)
        return g_vm_block;

    size = (_newlib_vm_size_user + 0xfffff) & ~0xfffff;
    g_vm_block = sceKernelAllocMemBlockForVM("pcsx-jit", size);
    sceClibPrintf("pcsx-vita: JIT VM block size=0x%x uid=0x%x\n", size, g_vm_block);
    return g_vm_block;
}

/* On hardware there is no attached console: the log also goes to
 * APP_ROOT/log.txt, so a run launched remotely can be read back by FTP. */
static SceUID g_log_fd = -1;

static void log_text(const char *text)
{
    sceClibPrintf("%s", text);
    if (g_log_fd >= 0)
        sceIoWrite(g_log_fd, text, strlen(text));
}

static void vita_logf(const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_text(buf);
}

static void vita_log(enum retro_log_level level, const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    (void)level;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_text(buf);
}
/* Every later message of this file goes through the same log. */
#define sceClibPrintf vita_logf

static bool g_gpu_thread;
static bool g_board_thread;
static bool g_stock_clocks; /* --stock-clocks: keep the system's clocks */
static bool g_gpu2;   /* gpu_neon's second render stage */
static bool g_fb_main; /* framebuffers in main memory instead of CDRAM */
/* Present through GXM (the GPU scales raw VRAM). Not with the vitaGL
 * renderer, which owns GXM and the display itself. */
#ifdef STANDALONE_VITAGL
static bool g_gxm = false;
#else
static bool g_gxm = true;
#endif

static const char *option_value(const char *key)
{
    if (strcmp(key, "pcsx_rearmed_gpu_thread_rendering") == 0)
        return g_gpu_thread ? "enabled" : "disabled";
    if (strcmp(key, "pcsx_rearmed_system12_board_thread") == 0)
        return g_board_thread ? "enabled" : "disabled";
    if (strcmp(key, "pcsx_rearmed_neon_two_stage") == 0)
        return g_gpu2 ? "enabled" : "disabled";
#ifdef STANDALONE_UI
    if (g_ui_mode) {
        const char *value = VitaSettings_GetOption(key);
        if (value)
            return value;
    }
#endif
    static const struct {
        const char *key;
        const char *value;
    } options[] = {
        { "pcsx_rearmed_memcard1", "shared" },
        { "pcsx_rearmed_memcard2", "none" },
        { "pcsx_rearmed_bios", "auto" },
        { "pcsx_rearmed_show_bios_bootlogo", "disabled" },
        { "pcsx_rearmed_rgb32_output", "disabled" },
        { "pcsx_rearmed_frameskip_type", "disabled" },
        { "pcsx_rearmed_region", "auto" },
        { "pcsx_rearmed_display_info", "disabled" },
        { "pcsx_rearmed_vibration", "disabled" },
        { "pcsx_rearmed_dithering", "enabled" },
        { "pcsx_rearmed_neon_interlace_enable_v2", "auto" },
        { "pcsx_rearmed_neon_enhancement_enable", "disabled" },
        { "pcsx_rearmed_scale_hires", "disabled" },
        { "pcsx_rearmed_spu_interpolation", "gaussian" },
        { "pcsx_rearmed_spu_thread", "disabled" },
        { "pcsx_rearmed_cd_readahead", "0" },
        { "pcsx_rearmed_drc", "enabled" },
        { "pcsx_rearmed_psxclock", "auto" },
        { "pcsx_rearmed_fractional_framerate", "auto" },
    };
    size_t i;

    for (i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        if (strcmp(options[i].key, key) == 0)
            return options[i].value;
    }
    return NULL;
}

static bool environment_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = vita_log;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = BIOS_DIR;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = SAVE_DIR;
        return true;
#ifdef RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY
    case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY:
        *(const char **)data = APP_ROOT;
        return true;
#endif
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
#ifdef STANDALONE_UI
        *(bool *)data = g_ui_mode && VitaSettings_ConsumeDirty();
#else
        *(bool *)data = false;
#endif
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = (struct retro_variable *)data;
        var->value = option_value(var->key);
        return var->value != NULL;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
        *(unsigned *)data = 0;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 0;
        return true;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
        return true;
    default:
        return false;
    }
}

static uint32_t rgb565_to_abgr8888(uint16_t p)
{
    uint32_t r5 = (p >> 11) & 31;
    uint32_t g6 = (p >> 5) & 63;
    uint32_t b5 = p & 31;
    uint32_t r = (r5 << 3) | (r5 >> 2);
    uint32_t g = (g6 << 2) | (g6 >> 4);
    uint32_t b = (b5 << 3) | (b5 >> 2);
    return 0xff000000u | (b << 16) | (g << 8) | r;
}

/* Presentation works per source row: each one is converted once (NEON, into
 * cached memory) and scaled horizontally, then copied to the 2-3 display rows
 * that show it. Same pixels as converting every display pixel, which cost
 * 17 ms per frame on hardware. */
static uint32_t g_row_abgr[1024];
static uint32_t g_row_scaled[GAME_W];

static void convert_row(const uint16_t *src, unsigned width)
{
    unsigned x = 0;
    for (; x + 8 <= width; x += 8) {
        uint16x8_t p = vld1q_u16(src + x);
        uint8x8_t r5 = vmovn_u16(vshrq_n_u16(p, 11));
        uint8x8_t g6 = vmovn_u16(vandq_u16(vshrq_n_u16(p, 5), vdupq_n_u16(63)));
        uint8x8_t b5 = vmovn_u16(vandq_u16(p, vdupq_n_u16(31)));
        uint8x8x4_t out;
        out.val[0] = vorr_u8(vshl_n_u8(r5, 3), vshr_n_u8(r5, 2));
        out.val[1] = vorr_u8(vshl_n_u8(g6, 2), vshr_n_u8(g6, 4));
        out.val[2] = vorr_u8(vshl_n_u8(b5, 3), vshr_n_u8(b5, 2));
        out.val[3] = vdup_n_u8(0xff);
        vst4_u8((uint8_t *)(g_row_abgr + x), out);
    }
    for (; x < width; x++)
        g_row_abgr[x] = rgb565_to_abgr8888(src[x]);
}

static void rebuild_scale_map(unsigned width, unsigned height)
{
    int x, y;
    for (x = 0; x < GAME_W; x++)
        g_xmap[x] = (uint16_t)(((uint64_t)x * width) / GAME_W);
    for (y = 0; y < DISPLAY_H; y++)
        g_ymap[y] = (uint16_t)(((uint64_t)y * height) / DISPLAY_H);
    g_map_w = (int)width;
    g_map_h = (int)height;
}

#ifdef VITA_GPUPROF
extern void (*pcsx_vita_swap_cb)(void);
extern void (*pcsx_vita_gpu_label_cb)(int kind, const char *name, unsigned handle);
static void gpuprof_swap(void)
{
    static unsigned presents;
    vita_gpuprof_after_frame(++presents);
}
#endif

/* GXM presentation. The PSX 15-bit pixel (R low, bit 15 = mask) is GXM's
 * X1U5U5U5_1BGR texture format, so the displayed VRAM rows are copied as
 * they are into an uncached texture and the GPU scales them to the screen:
 * no CPU color conversion, and 245 KB written per frame instead of 1.6 MB. */
extern int (*pcsx_raw_frame_cb)(const void *vram, int ofs, int stride, unsigned mask,
    int x, int y, int w, int h, int disp_w, int disp_h);
/* Three textures: the display queue allows two pending swaps, so when a frame
 * writes the texture of the frame three before, that frame's swap has been
 * processed and its rendering is complete. No sceGxmFinish per frame. */
#define GXM_TEX_COUNT 3
static vita2d_texture *g_gxm_tex[GXM_TEX_COUNT];
/* --scale: how the PSX picture reaches the 725x544 game area. Nearest at a
 * non-integer factor (725/512 across) gives columns 1 or 2 pixels wide;
 * sharp first scales by whole factors with nearest, then by the remaining
 * fraction with bilinear, so pixels stay crisp and evenly sized. */
enum { SCALE_NEAREST, SCALE_SHARP, SCALE_BILINEAR };
static int g_scale_mode = SCALE_SHARP;
static vita2d_texture *g_gxm_prescale; /* render target of the sharp pass */
static int g_prescale_w, g_prescale_h;
static int g_gxm_next, g_gxm_ready = -1, g_gxm_w, g_gxm_h;

static int gxm_raw_frame(const void *vram_, int ofs, int stride, unsigned mask,
    int pic_x, int pic_y, int w, int h, int disp_w, int disp_h)
{
    (void)pic_x; (void)pic_y; (void)disp_w; (void)disp_h;
    const uint8_t *vram = vram_;
    vita2d_texture *t = g_gxm_tex[g_gxm_next];
    uint8_t *dst = vita2d_texture_get_datap(t);
    unsigned dst_stride = vita2d_texture_get_stride(t), bytes, r;
    uint64_t copy_started;
    if (w <= 0 || h <= 0) return 0;
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    sceGxmTextureSetFormat(&t->gxm_tex, SCE_GXM_TEXTURE_FORMAT_X1U5U5U5_1BGR);
    bytes = (unsigned)w * 2;
    copy_started = pcsx_profile_begin(PCSX_PROF_PRESENT_COPY, 1);
    for (r = 0; r < (unsigned)h; r++) {
        unsigned row = ((unsigned)ofs + r * (unsigned)stride) & mask;
        unsigned col = row & 2047u;
        uint8_t *out = dst + r * dst_stride;
        if (col + bytes <= 2048)
            memcpy(out, vram + row, bytes);
        else {
            memcpy(out, vram + row, 2048 - col);
            memcpy(out + 2048 - col, vram + (row - col), bytes - (2048 - col));
        }
    }
    pcsx_profile_end(PCSX_PROF_PRESENT_COPY, copy_started);
    g_gxm_w = w;
    g_gxm_h = h;
    g_gxm_ready = g_gxm_next;
    g_gxm_next = (g_gxm_next + 1) % GXM_TEX_COUNT;
    return 1;
}

/* Frames the raw VRAM copy does not take (24-bit display mode: the games'
 * movies) arrive converted to RGB565 by the core; upload those instead. */
static void gxm_rgb565_frame(const void *data, unsigned width, unsigned height, size_t pitch)
{
    vita2d_texture *t = g_gxm_tex[g_gxm_next];
    uint8_t *dst = vita2d_texture_get_datap(t);
    const unsigned dst_stride = vita2d_texture_get_stride(t);
    unsigned w = width > 1024 ? 1024 : width, h = height > 512 ? 512 : height, r;
    uint64_t copy_started = pcsx_profile_begin(PCSX_PROF_PRESENT_COPY, 1);
    sceGxmTextureSetFormat(&t->gxm_tex, SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB);
    for (r = 0; r < h; r++)
        memcpy(dst + r * dst_stride, (const uint8_t *)data + r * pitch, w * 2);
    pcsx_profile_end(PCSX_PROF_PRESENT_COPY, copy_started);
    g_gxm_w = (int)w;
    g_gxm_h = (int)h;
    g_gxm_ready = g_gxm_next;
    g_gxm_next = (g_gxm_next + 1) % GXM_TEX_COUNT;
}

static void gxm_present(void)
{
    if (g_gxm_ready < 0)
        return;
    /* vita2d_start_drawing() resets its vertex pool every frame, while the GPU
     * may still read the last frames' vertices. Use it as a ring instead: a
     * frame takes a few hundred bytes of the 1 MB pool, so it restarts after
     * hundreds of frames, and with at most two swaps pending everything that
     * old is complete. */
    if (vita2d_pool_free_space() < 64 * 1024)
        vita2d_pool_reset();
    vita2d_texture *src = g_gxm_tex[g_gxm_ready];
    int src_w = g_gxm_w, src_h = g_gxm_h;
    if (g_scale_mode == SCALE_SHARP) {
        const int kx = (GAME_W + g_gxm_w - 1) / g_gxm_w, ky = (DISPLAY_H + g_gxm_h - 1) / g_gxm_h;
        const int pw = g_gxm_w * kx, ph = g_gxm_h * ky;
        if (pw != g_prescale_w || ph != g_prescale_h) {
            if (g_gxm_prescale) {
                vita2d_wait_rendering_done();
                vita2d_free_texture(g_gxm_prescale);
            }
            g_gxm_prescale = vita2d_create_empty_texture_rendertarget(pw, ph, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
            g_prescale_w = pw;
            g_prescale_h = ph;
            if (g_gxm_prescale)
                vita2d_texture_set_filters(g_gxm_prescale, SCE_GXM_TEXTURE_FILTER_LINEAR,
                    SCE_GXM_TEXTURE_FILTER_LINEAR);
        }
        if (g_gxm_prescale) {
            /* vita2d keeps the display's 960x544 projection on render
             * targets too: covering it covers the whole pw x ph target, kx by
             * ky target pixels per source texel. */
            vita2d_start_drawing_advanced(g_gxm_prescale, 0);
            vita2d_draw_texture_part_scale(src, 0, 0, 0, 0, g_gxm_w, g_gxm_h,
                (float)DISPLAY_W / g_gxm_w, (float)DISPLAY_H / g_gxm_h);
            vita2d_end_drawing();
            src = g_gxm_prescale;
            src_w = pw;
            src_h = ph;
        }
    }
    vita2d_start_drawing_advanced(NULL, 0);
    vita2d_clear_screen();
    vita2d_draw_texture_part_scale(src, GAME_X, 0, 0, 0,
        src_w, src_h, (float)GAME_W / src_w, (float)DISPLAY_H / src_h);
    vita2d_end_drawing();
    vita2d_swap_buffers();
    g_gxm_ready = -1;
}

static int gxm_init(void)
{
    int i;
    if (vita2d_init() < 0)
        return -1;
    /* vita2d's display callback waits for a vblank after each flip by
     * default: with the audio queue also pacing, a frame finished just past
     * a vblank waited for the next one (5 ms a frame, 59.5 fps). The flip
     * still latches at a vblank; only the wait goes, so audio is the one
     * pacing clock, as on the CPU presentation path. */
    vita2d_set_vblank_wait(0);
    vita2d_set_clear_color(0xff000000u);
    vita2d_texture_set_alloc_memblock_type(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
    for (i = 0; i < GXM_TEX_COUNT; i++) {
        g_gxm_tex[i] = vita2d_create_empty_texture_format(1024, 512,
            SCE_GXM_TEXTURE_FORMAT_X1U5U5U5_1BGR);
        if (!g_gxm_tex[i])
            return -1;
        if (g_scale_mode == SCALE_BILINEAR)
            vita2d_texture_set_filters(g_gxm_tex[i], SCE_GXM_TEXTURE_FILTER_LINEAR,
                SCE_GXM_TEXTURE_FILTER_LINEAR);
        else
            vita2d_texture_set_filters(g_gxm_tex[i], SCE_GXM_TEXTURE_FILTER_POINT,
                SCE_GXM_TEXTURE_FILTER_POINT);
    }
    pcsx_raw_frame_cb = gxm_raw_frame;
    return 0;
}

static void gxm_finish(void)
{
    int i;
    pcsx_raw_frame_cb = NULL;
    vita2d_wait_rendering_done();
    for (i = 0; i < GXM_TEX_COUNT; i++)
        if (g_gxm_tex[i]) vita2d_free_texture(g_gxm_tex[i]);
    if (g_gxm_prescale) vita2d_free_texture(g_gxm_prescale);
    g_gxm_prescale = NULL;
    g_prescale_w = g_prescale_h = 0;
    vita2d_fini();
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
    SceDisplayFrameBuf frame;
    uint32_t *dst;
    int back;
    int y, x;

#ifdef REARCADE_NEON
    glp_video(data, width, height, pitch);
    return;
#endif
    if (g_gxm) {
        if (g_gxm_ready < 0 && data && width && height)
            gxm_rgb565_frame(data, width, height, pitch);
        gxm_present();
        return;
    }
    if (!data || width == 0 || height == 0)
        return;

    uint64_t copy_started = pcsx_profile_begin(PCSX_PROF_PRESENT_COPY, 1);
    if (g_capture_path[0] && width <= 1024 && height <= 512 && height != 0 &&
        pitch >= (size_t)width * sizeof(uint16_t) &&
        (size_t)width <= SIZE_MAX / height / sizeof(uint16_t) &&
        (pitch == 0 || height <= SIZE_MAX / pitch)) {
        size_t pixels = (size_t)width * height;
        if (pixels > g_capture_capacity) {
            uint16_t *replacement = realloc(g_capture_pixels, pixels * sizeof(uint16_t));
            if (replacement) {
                g_capture_pixels = replacement;
                g_capture_capacity = pixels;
            }
        }
        if (pixels <= g_capture_capacity) {
            unsigned row;
            for (row = 0; row < height; row++)
                memcpy(g_capture_pixels + (size_t)row * width,
                       (const uint8_t *)data + (size_t)row * pitch,
                       (size_t)width * sizeof(uint16_t));
            g_capture_width = width;
            g_capture_height = height;
        }
    }

    if (g_map_w != (int)width || g_map_h != (int)height)
        rebuild_scale_map(width, height);

    /* The back buffer is scanned out until the frame queued last time is
     * latched at the next vblank. Wait for that only here, before writing:
     * when a vblank has passed since that queue, this returns at once, and
     * the audio queue stays the only pacing clock. Waiting for a vblank after
     * every queue as well made a frame delayed past one by the audio wait
     * cost two vblanks. */
    uint64_t wait_started = pcsx_profile_begin(PCSX_PROF_PRESENT_WAIT, 1);
    sceDisplayWaitSetFrameBuf();
    pcsx_profile_end(PCSX_PROF_PRESENT_WAIT, wait_started);

    back = g_front ^ 1;
    dst = g_fb[back];

    {
        int source_row = -1;
        for (y = 0; y < DISPLAY_H; y++) {
            if (g_ymap[y] != source_row) {
                source_row = g_ymap[y];
                convert_row((const uint16_t *)((const uint8_t *)data +
                    (size_t)source_row * pitch), width > 1024 ? 1024 : width);
                for (x = 0; x < GAME_W; x++)
                    g_row_scaled[x] = g_row_abgr[g_xmap[x]];
            }
            memcpy(dst + y * DISPLAY_PITCH + GAME_X, g_row_scaled, sizeof(g_row_scaled));
        }
    }

    pcsx_profile_end(PCSX_PROF_PRESENT_COPY, copy_started);
    frame.size = sizeof(frame);
    frame.base = dst;
    frame.pitch = DISPLAY_PITCH;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = DISPLAY_W;
    frame.height = DISPLAY_H;
    sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
    g_front = back;
}

#ifdef STANDALONE_VITAGL
/* rgba: glReadPixels rows y..y+rows-1, counted from the bottom. The picture
 * lives in its own memory block: the newlib heap is mostly board ROMs. */
static void capture_gl_frame(const uint8_t *rgba, unsigned y, unsigned rows,
                             unsigned width, unsigned height)
{
    static SceUID block = -1;
    static size_t block_size;
    size_t bytes = (size_t)width * height * 3;
    unsigned x, row;
    if (block < 0 || block_size < bytes) {
        void *base = NULL;
        if (block >= 0)
            sceKernelFreeMemBlock(block);
        block_size = (bytes + 0xfff) & ~(size_t)0xfff;
        block = sceKernelAllocMemBlock("pcsx-capture", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                       block_size, NULL);
        g_capture_rgb = NULL;
        if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0)
            return;
        g_capture_rgb = base;
    }
    for (row = 0; row < rows; row++) {
        const uint8_t *src = rgba + (size_t)row * width * 4;
        uint8_t *dst = g_capture_rgb + (size_t)(height - 1 - (y + row)) * width * 3;
        for (x = 0; x < width; x++) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }
    if (y + rows == height) {
        g_capture_width = width;
        g_capture_height = height;
    }
}
#endif

static int write_capture_ppm(void)
{
    FILE *out;
    unsigned x, y;
    if (!g_capture_path[0])
        return 0;
    if (g_capture_rgb && g_capture_width && g_capture_height) {
        size_t bytes = (size_t)g_capture_width * g_capture_height * 3;
        out = fopen(g_capture_path, "wb");
        if (!out)
            return -1;
        if (fprintf(out, "P6\n%u %u\n255\n", g_capture_width, g_capture_height) < 0 ||
            fwrite(g_capture_rgb, 1, bytes, out) != bytes) {
            fclose(out);
            return -1;
        }
        return fclose(out) == 0 ? 0 : -1;
    }
    if (!g_capture_pixels || !g_capture_width || !g_capture_height)
        return -1;
    out = fopen(g_capture_path, "wb");
    if (!out)
        return -1;
    if (fprintf(out, "P6\n%u %u\n255\n", g_capture_width, g_capture_height) < 0) {
        fclose(out);
        return -1;
    }
    for (y = 0; y < g_capture_height; y++) {
        uint8_t row[1024 * 3];
        for (x = 0; x < g_capture_width; x++) {
            uint16_t p = g_capture_pixels[(size_t)y * g_capture_width + x];
            unsigned r = (p >> 11) & 31;
            unsigned g = (p >> 5) & 63;
            unsigned b = p & 31;
            row[x * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
            row[x * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
            row[x * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
        }
        if (fwrite(row, 3, g_capture_width, out) != g_capture_width) {
            fclose(out);
            return -1;
        }
    }
    return fclose(out) == 0 ? 0 : -1;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{

    if (!g_audio_stream || !data || frames == 0)
        return frames;

    if (!SDL_PutAudioStreamData(g_audio_stream, data, (int)(frames * AUDIO_FRAME_BYTES))) {
        if (g_audio_put_errors < 8u)
            sceClibPrintf("pcsx-vita: SDL_PutAudioStreamData failed: %s\n", SDL_GetError());
        g_audio_put_errors++;
        return frames;
    }
    g_audio_put_frames += frames;
    if (g_report_path[0] || g_replay_active) {
        for (size_t i = 0; i < frames * 2; i++) {
            unsigned magnitude = data[i] < 0 ? -(int)data[i] : data[i];
            if (magnitude > g_audio_peak) g_audio_peak = magnitude;
            if (data[i]) g_audio_nonzero++;
        }
    }

    /* With vblank pacing the queue only caps latency (the ratio holds it at the
     * target); with audio pacing it is the clock. */
    const int limit = g_pacing == PACING_VBLANK ? 3 * g_audio_queue_target_bytes : g_audio_queue_target_bytes;
    uint64_t wait_started = pcsx_profile_begin(PCSX_PROF_AUDIO_WAIT, 1);
    while (!g_exit_requested && SDL_GetAudioStreamQueued(g_audio_stream) > limit)
        sceKernelDelayThread(1000);
    pcsx_profile_end(PCSX_PROF_AUDIO_WAIT, wait_started);

    return frames;
}

static int replay_control_binding(const char *name, unsigned *port, unsigned *id)
{
    static const struct {
        const char *name;
        unsigned port;
        unsigned id;
    } bindings[] = {
        { "p1up", 0, RETRO_DEVICE_ID_JOYPAD_UP },
        { "p1down", 0, RETRO_DEVICE_ID_JOYPAD_DOWN },
        { "p1left", 0, RETRO_DEVICE_ID_JOYPAD_LEFT },
        { "p1right", 0, RETRO_DEVICE_ID_JOYPAD_RIGHT },
        { "p1button1", 0, RETRO_DEVICE_ID_JOYPAD_B },
        { "p1button2", 0, RETRO_DEVICE_ID_JOYPAD_A },
        { "p1button3", 0, RETRO_DEVICE_ID_JOYPAD_Y },
        { "p1button4", 0, RETRO_DEVICE_ID_JOYPAD_X },
        { "p1button5", 0, RETRO_DEVICE_ID_JOYPAD_L },
        { "p1button6", 0, RETRO_DEVICE_ID_JOYPAD_R },
        { "p1button7", 0, RETRO_DEVICE_ID_JOYPAD_L2 },
        { "p1button8", 0, RETRO_DEVICE_ID_JOYPAD_R2 },
        { "p1start", 0, RETRO_DEVICE_ID_JOYPAD_START },
        { "p2up", 1, RETRO_DEVICE_ID_JOYPAD_UP },
        { "p2down", 1, RETRO_DEVICE_ID_JOYPAD_DOWN },
        { "p2left", 1, RETRO_DEVICE_ID_JOYPAD_LEFT },
        { "p2right", 1, RETRO_DEVICE_ID_JOYPAD_RIGHT },
        { "p2button1", 1, RETRO_DEVICE_ID_JOYPAD_B },
        { "p2button2", 1, RETRO_DEVICE_ID_JOYPAD_A },
        { "p2button3", 1, RETRO_DEVICE_ID_JOYPAD_Y },
        { "p2button4", 1, RETRO_DEVICE_ID_JOYPAD_X },
        { "p2button5", 1, RETRO_DEVICE_ID_JOYPAD_L },
        { "p2button6", 1, RETRO_DEVICE_ID_JOYPAD_R },
        { "p2button7", 1, RETRO_DEVICE_ID_JOYPAD_L2 },
        { "p2button8", 1, RETRO_DEVICE_ID_JOYPAD_R2 },
        { "p2start", 1, RETRO_DEVICE_ID_JOYPAD_START },
        { "coin1", 0, RETRO_DEVICE_ID_JOYPAD_L3 },
        { "coin2", 1, RETRO_DEVICE_ID_JOYPAD_L3 },
        { "service", 0, RETRO_DEVICE_ID_JOYPAD_SELECT },
        { "test", 1, RETRO_DEVICE_ID_JOYPAD_SELECT },
    };
    size_t i;

    for (i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (!strcmp(name, bindings[i].name)) {
            *port = bindings[i].port;
            *id = bindings[i].id;
            return 0;
        }
    }
    return -1;
}

static char *trim_field(char *text)
{
    char *end;
    while (*text && isspace((unsigned char)*text)) text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static int parse_decimal(const char *text, unsigned maximum, unsigned *value)
{
    const unsigned char *p = (const unsigned char *)text;
    char *end;
    unsigned long parsed;
    if (!*p) return -1;
    for (; *p; p++) if (!isdigit(*p)) return -1;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || end == text || *end || parsed > maximum) return -1;
    *value = (unsigned)parsed;
    return 0;
}

static int load_replay_inputs(const char *path, unsigned frames)
{
    FILE *file;
    char line[INPUT_CSV_LINE_BYTES];
    size_t total_bytes = 0, raw_length, length;
    unsigned line_number = 0, row_count = 0;
    unsigned previous_frame = 0, previous_pressed = 0;
    char previous_control[32] = "";
    bool have_header = false, have_previous = false;
    int failed = 0, line_result;

    file = fopen(path, "rb");
    if (!file) {
        sceClibPrintf("pcsx-vita: cannot open inputs CSV %s\n", path);
        return -1;
    }
    while ((line_result = vita_read_input_line(file, line, sizeof(line),
                                               &length, &raw_length)) > 0) {
        char *fields[3], *comma;
        unsigned frame, pressed, port, id;
        char *frame_text, *control, *pressed_text;
        int order;

        total_bytes += raw_length;
        line_number++;
        if (total_bytes > INPUT_CSV_MAX_BYTES) {
            failed = 1;
            break;
        }
        if (length && line[length - 1] == '\r') line[--length] = '\0';
        frame_text = trim_field(line);
        if (!*frame_text || *frame_text == '#') continue;
        if (!have_header) {
            if (strcmp(frame_text, "frame,control,pressed")) {
                failed = 1;
                break;
            }
            have_header = true;
            continue;
        }
        fields[0] = frame_text;
        comma = strchr(fields[0], ',');
        if (!comma) { failed = 1; break; }
        *comma++ = '\0';
        fields[1] = comma;
        comma = strchr(fields[1], ',');
        if (!comma) { failed = 1; break; }
        *comma++ = '\0';
        fields[2] = comma;
        if (strchr(fields[2], ',')) { failed = 1; break; }

        frame_text = trim_field(fields[0]);
        control = trim_field(fields[1]);
        pressed_text = trim_field(fields[2]);
        if (strlen(control) >= sizeof(g_input_events[0].control) ||
            parse_decimal(frame_text, 216000u, &frame) ||
            parse_decimal(pressed_text, 1u, &pressed) || frame >= frames ||
            replay_control_binding(control, &port, &id)) {
            failed = 1;
            break;
        }
        if (have_previous) {
            order = strcmp(control, previous_control);
            if (frame < previous_frame ||
                (frame == previous_frame &&
                 (order < 0 || (order == 0 && pressed <= previous_pressed)))) {
                failed = 1;
                break;
            }
        }
        if (row_count >= INPUT_CSV_MAX_EVENTS) { failed = 1; break; }
        g_input_events[row_count].frame = frame;
        g_input_events[row_count].port = port;
        g_input_events[row_count].id = id;
        g_input_events[row_count].pressed = pressed;
        strcpy(g_input_events[row_count].control, control);
        row_count++;
        previous_frame = frame;
        previous_pressed = pressed;
        strcpy(previous_control, control);
        have_previous = true;
    }
    if (line_result < 0 || ferror(file)) failed = 1;
    if (fclose(file)) failed = 1;
    if (!have_header || !row_count) failed = 1;
    if (failed) {
        sceClibPrintf("pcsx-vita: invalid inputs CSV line=%u (expected ordered frame,control,pressed rows)\n",
                      line_number);
        return -1;
    }
    g_input_event_count = row_count;
    g_next_input_event = 0;
    g_input_frame = 0;
    g_replay_joypad[0] = g_replay_joypad[1] = 0;
    g_replay_active = true;
    sceClibPrintf("pcsx-vita: inputs loaded events=%u frames=%u\n", row_count, frames);
    return 0;
}

static void apply_replay_inputs(unsigned frame)
{
    while (g_next_input_event < g_input_event_count &&
           g_input_events[g_next_input_event].frame <= frame) {
        vita_input_event *event = &g_input_events[g_next_input_event++];
        uint16_t mask = (uint16_t)(1u << event->id);
        if (event->pressed) g_replay_joypad[event->port] |= mask;
        else g_replay_joypad[event->port] &= (uint16_t)~mask;
    }
}

static SceTouchPanelInfo g_touch_panel;
static bool g_touch_ready, g_touch_pressed, g_touch_wait_release;
static int g_touch_id = -1;
static unsigned g_touch_x = 32768, g_touch_y = 32768;

static void poll_gun_touch(void)
{
    SceTouchData data;
    int rect[4] = { GAME_X, 0, GAME_W, DISPLAY_H };
    bool was_pressed = g_touch_pressed;
    g_touch_pressed = false;
    if (!g_touch_ready || g_replay_active || g_pause_requested ||
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &data, 1) <= 0) {
        g_touch_id = -1; return;
    }
#ifdef STANDALONE_VITAGL
    extern void VitaCtrNativeRenderer_GetFrameViewport(int rect[4]);
    VitaCtrNativeRenderer_GetFrameViewport(rect);
    rect[1] = DISPLAY_H - rect[1] - rect[3];
#elif defined(REARCADE_NEON)
    glp_get_viewport(rect);
#endif
    if (g_touch_wait_release) {
        if (!data.reportNum) g_touch_wait_release = false;
        g_touch_id = -1; return;
    }
    for (unsigned i = 0; i < data.reportNum && i < SCE_TOUCH_MAX_REPORT; i++) {
        const SceTouchReport *report = &data.report[i];
        if (was_pressed && report->id != g_touch_id) continue;
        float px = (float)(report->x - g_touch_panel.minDispX) * (DISPLAY_W - 1) /
            (g_touch_panel.maxDispX - g_touch_panel.minDispX);
        float py = (float)(report->y - g_touch_panel.minDispY) * (DISPLAY_H - 1) /
            (g_touch_panel.maxDispY - g_touch_panel.minDispY);
        unsigned x, y;
        if (!gun_pointer_position(px, py, rect, &x, &y)) continue;
        g_touch_x = x; g_touch_y = y;
        g_touch_id = report->id; g_touch_pressed = true; return;
    }
    g_touch_id = -1;
}

static void input_poll_cb(void)
{
    const uint32_t quit_mask = SCE_CTRL_SELECT | SCE_CTRL_START | SCE_CTRL_L1 | SCE_CTRL_R1;
    memset(&g_pad, 0, sizeof(g_pad));
    if (sceCtrlPeekBufferPositiveExt2(0, &g_pad, 1) < 0)
        sceCtrlPeekBufferPositive(0, &g_pad, 1);

    if (g_replay_active) {
        apply_replay_inputs(g_input_frame++);
    }

    if (g_ui_mode) {
        const uint32_t pause_mask = SCE_CTRL_SELECT | SCE_CTRL_START;
        const bool combo_down = (g_pad.buttons & pause_mask) == pause_mask;
        if (combo_down && !g_pause_combo_down) {
            g_pause_requested = true;
#ifdef STANDALONE_VITAGL
            /* The frame this run presents is the menu's backdrop. */
            pcsx_menu_backdrop_request = 1;
#endif
        }
        g_pause_combo_down = combo_down;
        poll_gun_touch();
        return;
    }

    if ((g_pad.buttons & quit_mask) == quit_mask) {
        if (++g_exit_hold >= 120)
            g_exit_requested = true;
    } else {
        g_exit_hold = 0;
    }
    poll_gun_touch();
}

static int16_t analog_value(uint8_t v)
{
    return (int16_t)(((int)v - 128) * 256);
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    uint32_t button = 0;

    device &= RETRO_DEVICE_MASK;
    if (g_replay_active) {
        if (device == RETRO_DEVICE_JOYPAD && port < 2) {
            if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
                return (int16_t)g_replay_joypad[port];
            if (id <= RETRO_DEVICE_ID_JOYPAD_R3)
                return (g_replay_joypad[port] & (uint16_t)(1u << id)) != 0;
        }
        return 0;
    }
    if (port != 0)
        return 0;

    if (device == RETRO_DEVICE_POINTER) {
        if (index != 0) return 0;
        switch (id) {
        case RETRO_DEVICE_ID_POINTER_X: return gun_pointer_to_retro(g_touch_x);
        case RETRO_DEVICE_ID_POINTER_Y: return gun_pointer_to_retro(g_touch_y);
        case RETRO_DEVICE_ID_POINTER_PRESSED: return g_touch_pressed;
        default: return 0;
        }
    }

    if (device == RETRO_DEVICE_ANALOG) {
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
            return id == RETRO_DEVICE_ID_ANALOG_X ? analog_value(g_pad.lx) : analog_value(g_pad.ly);
        if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT)
            return id == RETRO_DEVICE_ID_ANALOG_X ? analog_value(g_pad.rx) : analog_value(g_pad.ry);
        return 0;
    }
    if (device != RETRO_DEVICE_JOYPAD)
        return 0;

    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_B:      button = SCE_CTRL_CROSS; break;
    case RETRO_DEVICE_ID_JOYPAD_A:      button = SCE_CTRL_CIRCLE; break;
    case RETRO_DEVICE_ID_JOYPAD_Y:      button = SCE_CTRL_SQUARE; break;
    case RETRO_DEVICE_ID_JOYPAD_X:      button = SCE_CTRL_TRIANGLE; break;
    case RETRO_DEVICE_ID_JOYPAD_L:      button = SCE_CTRL_L1; break;
    case RETRO_DEVICE_ID_JOYPAD_R:      button = SCE_CTRL_R1; break;
    case RETRO_DEVICE_ID_JOYPAD_L2:     button = SCE_CTRL_L2; break;
    case RETRO_DEVICE_ID_JOYPAD_R2:     button = SCE_CTRL_R2; break;
    case RETRO_DEVICE_ID_JOYPAD_L3:     button = SCE_CTRL_L3; break;
    case RETRO_DEVICE_ID_JOYPAD_R3:     button = SCE_CTRL_R3; break;
    case RETRO_DEVICE_ID_JOYPAD_UP:     button = SCE_CTRL_UP; break;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   button = SCE_CTRL_DOWN; break;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   button = SCE_CTRL_LEFT; break;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  button = SCE_CTRL_RIGHT; break;
    case RETRO_DEVICE_ID_JOYPAD_START:  button = SCE_CTRL_START; break;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: button = SCE_CTRL_SELECT; break;
    default: return 0;
    }
    return (g_pad.buttons & button) != 0;
}

static int display_init(void)
{
    SceDisplayFrameBuf frame;
    int i;

#ifdef REARCADE_NEON
    /* vitaGL starts after retro_init (glp_init in main). */
    return 0;
#endif
    if (g_gxm)
        return gxm_init();
    for (i = 0; i < 2; i++) {
        g_fb_uid[i] = sceKernelAllocMemBlock(i ? "pcsx-fb1" : "pcsx-fb0",
            g_fb_main ? SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW
                      : SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, DISPLAY_MEM_SIZE, NULL);
        if (g_fb_uid[i] < 0)
            return g_fb_uid[i];
        if (sceKernelGetMemBlockBase(g_fb_uid[i], (void **)&g_fb[i]) < 0)
            return -1;
        memset(g_fb[i], 0, DISPLAY_MEM_SIZE);
    }

    frame.size = sizeof(frame);
    frame.base = g_fb[0];
    frame.pitch = DISPLAY_PITCH;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = DISPLAY_W;
    frame.height = DISPLAY_H;
    g_front = 0;
    return sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static void display_finish(void)
{
    int i;
#ifdef REARCADE_NEON
    glp_finish();
    return;
#endif
    if (g_gxm) {
        gxm_finish();
        return;
    }
    /* The display retains a framebuffer after the last video callback. Stop
     * scanout and wait for that update before releasing its backing blocks. */
    int ret = sceDisplaySetFrameBuf(NULL, SCE_DISPLAY_SETBUF_NEXTFRAME);
    if (ret >= 0)
        ret = sceDisplayWaitSetFrameBuf();
    if (ret < 0) {
        sceClibPrintf("pcsx-vita: display detach failed 0x%x; retaining buffers\n", ret);
        return;
    }
    for (i = 0; i < 2; i++) {
        if (g_fb_uid[i] >= 0)
            sceKernelFreeMemBlock(g_fb_uid[i]);
        g_fb_uid[i] = -1;
        g_fb[i] = NULL;
    }
}

static int audio_init(int sample_rate)
{
    SDL_AudioSpec want;
    SDL_AudioSpec src_spec;
    SDL_AudioSpec dst_spec;
    SDL_AudioSpec device_spec;
    int device_frames = 0;

    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        sceClibPrintf("pcsx-vita: SDL audio init failed: %s\n", SDL_GetError());
        return -1;
    }

    memset(&want, 0, sizeof(want));
    /* The Vita output port cannot accept System12's 88200 Hz source.
     * Open a supported device rate and let SDL convert the core's samples. */
    want.freq = AUDIO_DEVICE_RATE;
    want.format = SDL_AUDIO_S16;
    want.channels = 2;

    g_audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, NULL, NULL);
    if (!g_audio_stream) {
        sceClibPrintf("pcsx-vita: SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -1;
    }

    want.freq = sample_rate;
    g_audio_queue_target_bytes = (int)((int64_t)sample_rate * AUDIO_QUEUE_TARGET_DEVICE_BUFFERS *
                                       AUDIO_DEVICE_FRAMES / AUDIO_DEVICE_RATE) * (int)AUDIO_FRAME_BYTES;
    if (!SDL_SetAudioStreamFormat(g_audio_stream, &want, NULL)) {
        sceClibPrintf("pcsx-vita: SDL_SetAudioStreamFormat failed: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(g_audio_stream);
        g_audio_stream = NULL;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -1;
    }

    memset(&src_spec, 0, sizeof(src_spec));
    memset(&dst_spec, 0, sizeof(dst_spec));
    memset(&device_spec, 0, sizeof(device_spec));
    SDL_GetAudioStreamFormat(g_audio_stream, &src_spec, &dst_spec);
    SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_audio_stream),
        &device_spec, &device_frames);
    sceClibPrintf("pcsx-vita: audio stream src=%dHz dst=%dHz device=%dHz frames=%d\n",
        src_spec.freq, dst_spec.freq, device_spec.freq, device_frames);

    if (!SDL_ResumeAudioStreamDevice(g_audio_stream)) {
        sceClibPrintf("pcsx-vita: SDL_ResumeAudioStreamDevice failed: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(g_audio_stream);
        g_audio_stream = NULL;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -1;
    }

    return 0;
}

static void audio_finish(void)
{
    if (g_audio_stream) {
        SDL_DestroyAudioStream(g_audio_stream);
        g_audio_stream = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static bool has_ext(const char *name, const char *ext)
{
    size_t nl = strlen(name), el = strlen(ext), i;
    if (nl < el)
        return false;
    name += nl - el;
    for (i = 0; i < el; i++) {
        char a = name[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return true;
}

static int extension_rank(const char *name)
{
    /* .zip: a MAME romset of System 10, 11, 12 or Capcom ZN; the core identifies the set. */
    static const char *exts[] = { ".cue", ".chd", ".pbp", ".m3u", ".iso", ".img", ".bin", ".exe", ".zip" };
    int i;
    for (i = 0; i < (int)(sizeof(exts) / sizeof(exts[0])); i++)
        if (has_ext(name, exts[i]))
            return i;
    return 999;
}

static bool find_game(char *out, size_t out_size)
{
    SceUID dfd;
    SceIoDirent ent;
    char best[256] = {0};
    int best_rank = 999;

    dfd = sceIoDopen(APP_ROOT);
    if (dfd < 0)
        return false;

    memset(&ent, 0, sizeof(ent));
    while (sceIoDread(dfd, &ent) > 0) {
        int rank = extension_rank(ent.d_name);
        if (rank < best_rank) {
            snprintf(best, sizeof(best), "%s", ent.d_name);
            best_rank = rank;
        }
        memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(dfd);

    if (best_rank == 999)
        return false;
    snprintf(out, out_size, "%s/%s", APP_ROOT, best);
    return true;
}

static bool is_game_path(const char *path)
{
    const char *base;

    if (path == NULL || path[0] == '\0' || extension_rank(path) == 999)
        return false;

    base = strrchr(path, '/');
    {
        const char *backslash = strrchr(path, '\\');
        if (backslash != NULL && (base == NULL || backslash > base))
            base = backslash;
    }
    base = base != NULL ? base + 1 : path;

    if (strcasecmp(base, "eboot.bin") == 0 ||
        strcasecmp(base, "pcsx-rearmed-vita.elf") == 0 ||
        strcasecmp(base, "pcsx-rearmed-vita.velf") == 0)
        return false;

    return true;
}
static void make_dirs(void)
{
    sceIoMkdir(APP_ROOT, 0777);
    sceIoMkdir(BIOS_DIR, 0777);
    sceIoMkdir(SAVE_DIR, 0777);
}

/* Launched from the shell (e.g. by vitacompanion) the app has no arguments:
 * read whitespace-separated ones from APP_ROOT/args.txt instead. */
static char g_args_text[2048];
static char *g_args_argv[64];

static void load_args_file(int *argc, char ***argv)
{
    SceUID fd = sceIoOpen(APP_ROOT "/args.txt", SCE_O_RDONLY, 0);
    int n, count = 0;
    char *p;
    if (fd < 0)
        return;
    n = sceIoRead(fd, g_args_text, sizeof(g_args_text) - 1);
    sceIoClose(fd);
    if (n <= 0)
        return;
    g_args_text[n] = '\0';
    g_args_argv[count++] = *argc > 0 ? (*argv)[0] : "eboot.bin";
    for (p = g_args_text; *p && count < (int)(sizeof(g_args_argv) / sizeof(g_args_argv[0])) - 1;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        g_args_argv[count++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }
    g_args_argv[count] = NULL;
    *argc = count;
    *argv = g_args_argv;
}

/* Microseconds and count of the vblank waits in pace_frame. */
static unsigned long long g_vblank_wait_us;
static unsigned g_vblank_wait_n;

/* Where the threads waited since the last progress line, in ms per frame
 * (and waits per frame): the frontend for the renderer by reason, the
 * renderer for work, in vglSwapBuffers and working, the main thread for the
 * board thread and the vblank. */
static void print_wait_stats(unsigned runs)
{
#ifdef STANDALONE_VITAGL
    extern unsigned long long pcsx_vita_ctr_wait_us[];
    extern unsigned pcsx_vita_ctr_wait_n[];
    static const char *const names[] = {
        "submit", "partial", "task_prev", "task", "sync",
        "r_idle", "r_swap", "r_packet", "r_task" };
    enum { SLOTS = sizeof(names) / sizeof(names[0]) };
    static unsigned long long last_us[SLOTS];
    static unsigned last_n[SLOTS];
#endif
#ifdef ARCADE_SYSTEM12
    extern unsigned long long system12_board_wait_us;
    extern unsigned system12_board_wait_n;
    static unsigned long long last_board_us;
    static unsigned last_board_n;
#endif
    static unsigned long long last_vblank_us;
    static unsigned last_vblank_n, last_runs;
    const unsigned frames = runs - last_runs;
    char line[1024];
    int at = 0;
    if (!frames)
        return;
#define WAIT_FIELD(name, us, n) \
    at += snprintf(line + at, sizeof(line) - at, " %s=%.2f/%.2f", (name), \
        (double)(us) / 1000.0 / frames, (double)(n) / frames)
#ifdef STANDALONE_VITAGL
    for (int i = 0; i < SLOTS; i++) {
        WAIT_FIELD(names[i], pcsx_vita_ctr_wait_us[i] - last_us[i], pcsx_vita_ctr_wait_n[i] - last_n[i]);
        last_us[i] = pcsx_vita_ctr_wait_us[i];
        last_n[i] = pcsx_vita_ctr_wait_n[i];
    }
#endif
#if defined(STANDALONE_VITAGL) && defined(VITA_DEBUG_SHOT)
    {
        extern void *pcsx_vita_ctr_task_fn[8];
        extern unsigned pcsx_vita_ctr_task_n[8];
        extern unsigned long long pcsx_vita_ctr_task_us[8];
        static unsigned last_task_n[8];
        static unsigned long long last_task_us[8];
        extern int pcsx_vita_ctr_read_rect[4];
        extern unsigned pcsx_vita_ctr_read_n;
        static unsigned last_read_n;
        if (pcsx_vita_ctr_read_n != last_read_n)
            at += snprintf(line + at, sizeof(line) - at, " read=%u@%d,%d,%dx%d", pcsx_vita_ctr_read_n - last_read_n,
                pcsx_vita_ctr_read_rect[0], pcsx_vita_ctr_read_rect[1], pcsx_vita_ctr_read_rect[2], pcsx_vita_ctr_read_rect[3]);
        last_read_n = pcsx_vita_ctr_read_n;
        for (int i = 0; i < 8 && pcsx_vita_ctr_task_fn[i]; i++) {
            char name[24];
            if (pcsx_vita_ctr_task_n[i] == last_task_n[i])
                continue;
            snprintf(name, sizeof(name), "t%p", pcsx_vita_ctr_task_fn[i]);
            WAIT_FIELD(name, pcsx_vita_ctr_task_us[i] - last_task_us[i], pcsx_vita_ctr_task_n[i] - last_task_n[i]);
            last_task_n[i] = pcsx_vita_ctr_task_n[i];
            last_task_us[i] = pcsx_vita_ctr_task_us[i];
        }
    }
#endif
#ifdef REARCADE_NEON
    {
        extern unsigned long long glp_busy_us;
        static unsigned long long last_glp_us;
        WAIT_FIELD("present", glp_busy_us - last_glp_us, frames);
        last_glp_us = glp_busy_us;
    }
#endif
#ifdef ARCADE_SYSTEM12
    WAIT_FIELD("board", system12_board_wait_us - last_board_us, system12_board_wait_n - last_board_n);
    {
        extern unsigned long long system12_board_busy_us;
        static unsigned long long last_board_busy;
        WAIT_FIELD("b_busy", system12_board_busy_us - last_board_busy, 0);
        last_board_busy = system12_board_busy_us;
        extern unsigned long long system12_board_cpu_us;
        static unsigned long long last_board_cpu;
        WAIT_FIELD("b_cpu", system12_board_cpu_us - last_board_cpu, 0);
        last_board_cpu = system12_board_cpu_us;
        extern unsigned long long system12_h8_us, system12_pcm_us;
        static unsigned long long last_h8, last_pcm;
        WAIT_FIELD("b_h8", system12_h8_us - last_h8, 0);
        WAIT_FIELD("b_c352", system12_pcm_us - last_pcm, 0);
        last_h8 = system12_h8_us;
        last_pcm = system12_pcm_us;
        extern unsigned long long system12_c352_access_us, system12_c352_horizon_us, system12_c352_accesses;
        static unsigned long long last_acc_us, last_hor_us, last_acc;
        WAIT_FIELD("c_acc", system12_c352_access_us - last_acc_us, system12_c352_accesses - last_acc);
        WAIT_FIELD("c_hor", system12_c352_horizon_us - last_hor_us, 0);
        last_acc_us = system12_c352_access_us; last_hor_us = system12_c352_horizon_us; last_acc = system12_c352_accesses;
        extern unsigned long long system12_h8_instructions, system12_h8_shared;
        static unsigned long long last_ins, last_shared;
        at += snprintf(line + at, sizeof(line) - at, " h8_ins=%llu h8_shared=%llu",
            (system12_h8_instructions - last_ins) / frames, (system12_h8_shared - last_shared) / frames);
        last_ins = system12_h8_instructions;
        last_shared = system12_h8_shared;
    }
    last_board_us = system12_board_wait_us;
    last_board_n = system12_board_wait_n;
#endif
#ifdef USE_ASYNC_GPU
    {
        extern unsigned long long gpu_async_wait_us;
        extern unsigned gpu_async_wait_n;
        static unsigned long long last_gpu_us;
        static unsigned last_gpu_n;
        WAIT_FIELD("gpu_thread", gpu_async_wait_us - last_gpu_us, gpu_async_wait_n - last_gpu_n);
        last_gpu_us = gpu_async_wait_us;
        last_gpu_n = gpu_async_wait_n;
#ifdef VITA_DEBUG_SHOT
        {
            /* The GPU thread's busy time: lists, presents, DMA writes;
             * and the command words per frame. */
            extern unsigned long long gpu_async_busy_us[3], gpu_async_words;
            static unsigned long long last_busy[3], last_words;
            static const char *const busy_names[3] = { "g_list", "g_present", "g_dma" };
            for (int i = 0; i < 3; i++) {
                WAIT_FIELD(busy_names[i], gpu_async_busy_us[i] - last_busy[i], 0);
                last_busy[i] = gpu_async_busy_us[i];
            }
            at += snprintf(line + at, sizeof(line) - at, " words=%llu", (gpu_async_words - last_words) / frames);
            last_words = gpu_async_words;
            {
                extern unsigned gpu_async_waits_by[4];
                extern unsigned long long gpu_async_wait_us_by[4];
                static unsigned last_by[4];
                static unsigned long long last_us_by[4];
                static const char *const why[4] = { "w_other", "w_vramwrite", "w_vramread", "w_full" };
                extern int gpu_async_write_last[4];
                at += snprintf(line + at, sizeof(line) - at, " lastwrite=%dx%d@%d,%d/%dw",
                    gpu_async_write_last[0], gpu_async_write_last[1], gpu_async_write_last[3] & 0xffff,
                    gpu_async_write_last[3] >> 16, gpu_async_write_last[2]);
                for (int i = 0; i < 4; i++) {
                    WAIT_FIELD(why[i], gpu_async_wait_us_by[i] - last_us_by[i], gpu_async_waits_by[i] - last_by[i]);
                    last_by[i] = gpu_async_waits_by[i];
                    last_us_by[i] = gpu_async_wait_us_by[i];
                }
            }
        }
#endif
    }
#endif
    WAIT_FIELD("vblank", g_vblank_wait_us - last_vblank_us, g_vblank_wait_n - last_vblank_n);
#undef WAIT_FIELD
    last_vblank_us = g_vblank_wait_us;
    last_vblank_n = g_vblank_wait_n;
    last_runs = runs;
    sceClibPrintf("pcsx-vita: waits ms/frame (waits/frame)%s\n", line);
}

/* Vblank pacing, once per emulated frame: at most one frame per display
 * refresh (a frame finished late does not wait), and the audio resampling
 * ratio follows the queue, within 0.5% of the nominal ratio, so the queue
 * stays at its target without the emulation waiting on it. */
static void pace_frame(void)
{
    if (g_pacing != PACING_VBLANK)
        return;
    if (g_audio_stream && g_audio_queue_target_bytes > 0) {
        float fill = (float)(SDL_GetAudioStreamQueued(g_audio_stream) - g_audio_queue_target_bytes) /
                     (float)g_audio_queue_target_bytes;
        if (fill > 1.0f) fill = 1.0f;
        if (fill < -1.0f) fill = -1.0f;
        g_audio_ratio = g_audio_base_ratio * (1.0f + 0.005f * fill);
        SDL_SetAudioStreamFrequencyRatio(g_audio_stream, g_audio_ratio);
    }
    int now = sceDisplayGetVcount();
    if (now == g_last_vcount) {
        const SceUInt64 start = sceKernelGetProcessTimeWide();
        sceDisplayWaitVblankStart();
        g_vblank_wait_us += sceKernelGetProcessTimeWide() - start;
        g_vblank_wait_n++;
        now = sceDisplayGetVcount();
    }
    g_last_vcount = now;
}

/* Opens audio at the loaded game's rate and sets the pacing ratio. */
static void start_audio(const struct retro_system_av_info *av)
{
    float refresh = 0.0f;
    int ret = audio_init((int)av->timing.sample_rate);

    if (ret < 0)
        sceClibPrintf("pcsx-vita: audio init failed 0x%x; continuing silent\n", ret);
    else
        sceClibPrintf("pcsx-vita: audio initialized\n");
    if (sceDisplayGetRefreshRate(&refresh) < 0 || refresh <= 0.0f)
        refresh = 59.94f;
    if (av->timing.fps > 0.0)
        g_audio_base_ratio = (float)(refresh / av->timing.fps);
    sceClibPrintf("pcsx-vita: pacing=%s display=%.4fHz core=%.4fHz audio_ratio=%.5f\n",
        g_pacing == PACING_VBLANK ? "vblank" : "audio", refresh, av->timing.fps,
        g_pacing == PACING_VBLANK ? g_audio_base_ratio : 1.0f);
}

#ifdef STANDALONE_UI
static void wait_for_menu_input_release(void)
{
    const uint32_t mask =
        SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT |
        SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_SQUARE |
        SCE_CTRL_START | SCE_CTRL_SELECT;

    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) < 0)
            sceCtrlPeekBufferPositive(0, &pad, 1);
        if ((pad.buttons & mask) == 0)
            break;
        sceKernelDelayThread(1000);
    }
    memset(&g_pad, 0, sizeof(g_pad));
    g_pause_combo_down = false;
    g_pause_requested = false;
    g_touch_pressed = false; g_touch_id = -1; g_touch_wait_release = true;
}

/* The pause menu, until it resumes, restarts or closes the game. Save and
 * load keep it open on the save-state page, as a failed load does. */
static int run_pause_menu(const char *game_path)
{
    bool reopen_save_states = false;
    int ret;

    if (g_audio_stream)
        SDL_PauseAudioStreamDevice(g_audio_stream);
#ifdef STANDALONE_VITAGL
    /* The menu draws on the renderer thread: no frame may be in flight. */
    gpu_sync_renderer();
#endif
    for (;;) {
#if defined(ARCADE_SHELF) || defined(REARCADE_NEON)
        ret = VitaShelfPause_Run(game_path, reopen_save_states ? 1 : 0);
#else
        ret = VitaPauseMenu_Run(game_path, reopen_save_states ? 1 : 0);
#endif
        reopen_save_states = false;
        wait_for_menu_input_release();

        if (ret == VITA_PAUSE_SAVE_STATE) {
            const int slot = VitaSaveStates_GetSlot();
            sceClibPrintf("pcsx-vita: %s state slot %d\n",
                VitaSaveStates_Save(game_path, slot) == 0 ? "saved" : "failed to save", slot);
            reopen_save_states = true;
            continue;
        }
        if (ret == VITA_PAUSE_LOAD_STATE) {
            const int slot = VitaSaveStates_GetSlot();
            if (VitaSaveStates_Load(game_path, slot) == 0) {
                sceClibPrintf("pcsx-vita: loaded state slot %d\n", slot);
                if (g_audio_stream)
                    SDL_ClearAudioStream(g_audio_stream);
                ret = VITA_PAUSE_RESUME;
                break;
            }
            sceClibPrintf("pcsx-vita: failed to load state slot %d\n", slot);
            reopen_save_states = true;
            continue;
        }
        break;
    }
    if (ret == VITA_PAUSE_RESUME && g_audio_stream)
        SDL_ResumeAudioStreamDevice(g_audio_stream);
    return ret;
}

/* One game in menu mode: it runs until the pause menu closes it (0) or
 * the app is asked to exit (1); 3 if it does not load. */
static int run_ui_game(const char *game_path)
{
    struct retro_game_info game;
    struct retro_system_av_info av;
    int ret;

    do {
        unsigned runs = 0;

        sceClibPrintf("pcsx-vita: booting %s\n", game_path);
        /* The test switch is for one visit to a game's test menu. */
        VitaSettings_SetOption("pcsx_rearmed_arcade_test", "disabled");
        memset(&game, 0, sizeof(game));
        game.path = game_path;
        if (!retro_load_game(&game)) {
            sceClibPrintf("pcsx-vita: retro_load_game failed\n");
            return 3;
        }
        retro_get_system_av_info(&av);
        start_audio(&av);
        memset(&g_pad, 0, sizeof(g_pad));
        g_pause_requested = false;
        g_pause_combo_down = false;
        g_touch_pressed = false; g_touch_id = -1; g_touch_wait_release = true;
        ret = VITA_PAUSE_CLOSE_GAME;
        while (!g_exit_requested) {
#ifdef VITA_DEBUG_SHOT
            const bool shot = VitaShelf_ShotRequested();
#ifdef STANDALONE_VITAGL
            /* The frame this run presents is kept for the shot. */
            if (shot)
                pcsx_menu_backdrop_request = 1;
#endif
#endif
            retro_run();
#ifdef VITA_DEBUG_SHOT
            if (shot) {
#ifdef STANDALONE_VITAGL
                gpu_sync_renderer();
#endif
                VitaShelf_ShotGame();
            }
#endif
            pace_frame();
            ++runs;
#ifndef PCSX_RELEASE
            /* Development builds keep the console awake for remote runs;
             * releases leave suspend and screen off to the system. */
            if (runs % 60 == 0)
                sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
#endif
#ifdef VITA_DEBUG_SHOT
            if (runs % 60 == 0) {
                static SceUInt64 last;
                const SceUInt64 now = sceKernelGetProcessTimeWide();
                if (last && runs > 60)
                    sceClibPrintf("pcsx-vita: frame %u: %.2f fps\n", runs, 60e6 / (double)(now - last));
                last = now;
                print_wait_stats(runs);
            }
#endif
            if (!g_pause_requested)
                continue;
            ret = run_pause_menu(game_path);
            if (ret != VITA_PAUSE_RESUME)
                break;
        }
        audio_finish();
        retro_unload_game();
        if (g_exit_requested)
            return 1;
    } while (ret == VITA_PAUSE_RESTART);
    sceClibPrintf("pcsx-vita: closed %s\n", game_path);
    return 0;
}

#ifdef REARCADE_NEON
/* The NEON executable runs the one game the selector gave it, then hands
 * the Vita back to the selector (eboot.bin). */
static char g_ui_game[1024], g_ui_title[256];

static int run_ui(void)
{
    char *const args[] = { NULL };
    VitaShelf_SetGame(g_ui_game, g_ui_title[0] ? g_ui_title : NULL);
    VitaShelf_Presenter()->set_aspect(VitaShelf_AspectMode());
    const int ret = run_ui_game(g_ui_game);
    if (ret != 1) {
        retro_deinit();
        display_finish();
        sceAppMgrLoadExec("app0:eboot.bin", args, NULL);
    }
    return ret == 3 ? 3 : 0;
}
#else
/* Menu mode: selector, game, pause menu, back to the selector. Returns the
 * process exit code. */
static int run_ui(void)
{
    char game_path[1024];
    int ret;

    for (;;) {
#ifdef ARCADE_SHELF
        ret = VitaShelfSelector_Run(game_path, sizeof(game_path));
#else
        ret = VitaRomSelector_Run(game_path, sizeof(game_path));
#endif
        if (ret <= 0) {
            sceClibPrintf("pcsx-vita: ROM selector %s\n", ret < 0 ? "failed" : "closed");
            return ret < 0 ? 2 : 0;
        }
#ifdef ARCADE_SHELF
        {
            /* The NEON renderer is the other executable: hand it the game. */
            const char *renderer = VitaSettings_GetOption("pcsx_rearcade_renderer");
            if (renderer && !strcmp(renderer, "neon")) {
                char title[256];
                VitaShelf_GetGameTitle(game_path, title, sizeof(title));
                char *const args[] = { (char *)"--ui-game", game_path, (char *)"--ui-title", title, NULL };
                sceClibPrintf("pcsx-vita: %s on NEON\n", game_path);
                sceAppMgrLoadExec("app0:eboot_neon.bin", args, NULL);
                sceClibPrintf("pcsx-vita: eboot_neon.bin did not start\n");
            }
            /* The renderer starts with the aspect in settings.cfg. */
            VitaShelf_Presenter()->set_aspect(VitaShelf_AspectMode());
        }
#endif
        ret = run_ui_game(game_path);
        if (ret == 1)
            return 0;
        if (ret == 3)
            return 3;
    }
}
#endif
#endif

int main(int argc, char **argv)
{
    struct retro_game_info game;
    char game_path[1024];
    int ret;
    unsigned validation_frames = 0;
    bool verify_smoke = false;
    bool verify_system12_vram = false;
    bool verify_system12_vram_pattern = false;
    bool verify_system12_framebuffer = false;
    bool verify_system12_texture_window = false;
    bool verify_system12_vram_x_wrap = false;
    bool verify_system12_coord_alias = false;
    bool verify_system12_coord_span_cull = false;
    unsigned verify_system12_drc_ram_mb = 0;
    bool system12_profile = false;
    bool other_arcade_profile = false;
    struct retro_system_av_info av;

    game_path[0] = '\0';
    /* Main CPU on core 0; the GPU and board threads take cores 1 and 2. */
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    make_dirs();
#ifdef REARCADE_NEON
    /* Started by eboot.bin: its log goes on. */
    g_log_fd = sceIoOpen(APP_ROOT "/log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
#else
    g_log_fd = sceIoOpen(APP_ROOT "/log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
#endif
#ifdef PCSX_SYSTEM12_PROFILE
    pcsx_profile_log = log_text;
#endif
    if (argc <= 1)
        load_args_file(&argc, &argv);
    for (ret = 1; ret < argc; ret++)
        sceClibPrintf("pcsx-vita: arg[%d]=%s\n", ret, argv[ret]);
    for (ret = 1; ret < argc; ret++) {
        if (!strcmp(argv[ret], "--verify-smoke")) { verify_smoke = true; continue; }
        /* Needs a VITA_GPU_THREAD=1 build; otherwise the core has no worker. */
        if (!strcmp(argv[ret], "--gpu-thread")) { g_gpu_thread = true; continue; }
        if (!strcmp(argv[ret], "--board-thread")) { g_board_thread = true; continue; }
        if (!strcmp(argv[ret], "--pacing") && ret + 1 < argc) {
            g_pacing = !strcmp(argv[++ret], "audio") ? PACING_AUDIO : PACING_VBLANK;
            continue;
        }
        if (!strcmp(argv[ret], "--scale") && ret + 1 < argc) {
            const char *mode = argv[++ret];
            g_scale_mode = !strcmp(mode, "nearest") ? SCALE_NEAREST
                         : !strcmp(mode, "bilinear") ? SCALE_BILINEAR : SCALE_SHARP;
            continue;
        }
        if (!strcmp(argv[ret], "--stock-clocks")) { g_stock_clocks = true; continue; }
        if (!strcmp(argv[ret], "--gpu2")) { g_gpu2 = true; continue; }
        if (!strcmp(argv[ret], "--gpu2-core") && ret + 1 < argc) {
            extern int pcsxr_gpu2_core;
            pcsxr_gpu2_core = atoi(argv[++ret]);
            continue;
        }
        if (!strcmp(argv[ret], "--fb-main")) { g_fb_main = true; continue; }
#ifdef REARCADE_NEON
        /* From the selector (eboot.bin): the game and its title. */
        if (!strcmp(argv[ret], "--ui-game") && ret + 1 < argc) {
            snprintf(g_ui_game, sizeof(g_ui_game), "%s", argv[++ret]);
            continue;
        }
        if (!strcmp(argv[ret], "--ui-title") && ret + 1 < argc) {
            snprintf(g_ui_title, sizeof(g_ui_title), "%s", argv[++ret]);
            continue;
        }
#endif
#ifdef STANDALONE_VITAGL
        /* A/B switches for hardware measurements of the HD presentation. */
        if (!strcmp(argv[ret], "--gl-1x")) {
            extern int pcsx_vita_gl_hd;
            pcsx_vita_gl_hd = 0;
            continue;
        }
        if (!strcmp(argv[ret], "--gl-msaa") && ret + 1 < argc) {
            extern int pcsx_vita_gl_msaa;
            pcsx_vita_gl_msaa = atoi(argv[++ret]);
            continue;
        }
#endif
#ifndef STANDALONE_VITAGL
        if (!strcmp(argv[ret], "--gxm")) { g_gxm = true; continue; }
#endif
        if (!strcmp(argv[ret], "--cpu-present")) { g_gxm = false; continue; }
        if (!strcmp(argv[ret], "--verify-system12-vram-pattern")) {
            verify_system12_vram = true;
            verify_system12_vram_pattern = true;
            continue;
        }
        if (!strcmp(argv[ret], "--verify-system12-vram")) { verify_system12_vram = true; continue; }
        if (!strcmp(argv[ret], "--verify-system12-framebuffer")) {
            verify_system12_framebuffer = true;
            continue;
        }
        if (!strcmp(argv[ret], "--verify-system12-texture-window")) {
            verify_system12_texture_window = true;
            continue;
        }
        if (!strcmp(argv[ret], "--verify-system12-vram-x-wrap")) {
            verify_system12_vram_x_wrap = true;
            continue;
        }
        if (!strcmp(argv[ret], "--verify-system12-coord-alias")) {
            verify_system12_coord_alias = true;
            continue;
        }
        if (!strcmp(argv[ret], "--verify-system12-coord-span-cull")) {
            verify_system12_coord_span_cull = true;
            continue;
        }
        if (!strcmp(argv[ret], "--verify-system12-drc")) {
            char *end;
            unsigned long ram_mb;
            if (ret + 1 >= argc) return 4;
            ram_mb = strtoul(argv[++ret], &end, 10);
            if (*end || (ram_mb != 4 && ram_mb != 16)) return 4;
            verify_system12_drc_ram_mb = (unsigned)ram_mb;
            continue;
        }
        if (!strcmp(argv[ret], "--frames")) {
            char *end;
            unsigned long count;
            if (ret + 1 >= argc) return 4;
            count = strtoul(argv[++ret], &end, 10);
            if (*end || !count || count > 216000) return 4;
            validation_frames = (unsigned)count; continue;
        }
        if (!strcmp(argv[ret], "--inputs")) {
            if (g_inputs_path[0] || ret + 1 >= argc || !argv[ret + 1][0] ||
                strlen(argv[ret + 1]) >= sizeof(g_inputs_path)) return 4;
            snprintf(g_inputs_path, sizeof(g_inputs_path), "%s", argv[++ret]);
            continue;
        }
        if (!strcmp(argv[ret], "--system12") || !strcmp(argv[ret], "--arcade")) {
#ifdef ARCADE_SYSTEM12
            if (ret + 1 >= argc || !argv[ret + 1][0]) return 4;
            if (system12_profile) return 4;
            other_arcade_profile = !strcmp(argv[ret], "--arcade");
            system12_profile = true;
            snprintf(game_path, sizeof(game_path), "%s", argv[++ret]);
            continue;
#else
            sceClibPrintf("pcsx-vita: --system12 requires ARCADE_SYSTEM12=1\n");
            return 4;
#endif
        }
#ifdef ARCADE_SYSTEM12
        if (!strcmp(argv[ret], "--report-system12") || !strcmp(argv[ret], "--report-arcade")) {
            if (ret + 1 >= argc || !argv[ret + 1][0] ||
                strlen(argv[ret + 1]) >= sizeof(g_report_path)) return 4;
            snprintf(g_report_path, sizeof(g_report_path), "%s", argv[++ret]);
            continue;
        }
#endif
        if (!strcmp(argv[ret], "--progress-every") && ret + 1 < argc) {
            g_progress_every = (unsigned)atoi(argv[++ret]);
            if (!g_progress_every) g_progress_every = 600;
            continue;
        }
        if (!strcmp(argv[ret], "--capture-ppm")) {
            if (ret + 1 >= argc || !argv[ret + 1][0] ||
                strlen(argv[ret + 1]) >= sizeof(g_capture_path)) return 4;
            snprintf(g_capture_path, sizeof(g_capture_path), "%s", argv[++ret]);
            continue;
        }
        if (!system12_profile && is_game_path(argv[ret]))
            snprintf(game_path, sizeof(game_path), "%s", argv[ret]);
    }
    if (other_arcade_profile && (verify_system12_drc_ram_mb || verify_system12_vram ||
        verify_system12_framebuffer || verify_system12_texture_window ||
        verify_system12_vram_x_wrap || verify_system12_coord_alias ||
        verify_system12_coord_span_cull)) return 4;
    if (system12_profile && verify_smoke) {
        sceClibPrintf("pcsx-vita: --verify-smoke requires the console smoke executable\n");
        return 4;
    }
    if (g_report_path[0] && !system12_profile) {
        sceClibPrintf("pcsx-vita: System12 report requires --system12\n");
        return 4;
    }
    if (g_capture_path[0] && !system12_profile &&
        (!validation_frames || !game_path[0] || verify_smoke)) return 4;
    if (verify_system12_drc_ram_mb && !system12_profile) {
        sceClibPrintf("pcsx-vita: --verify-system12-drc requires --system12 profile-directory\n");
        return 4;
    }
    if (verify_system12_drc_ram_mb && verify_smoke) return 4;
    if (verify_system12_vram && (!system12_profile || !validation_frames ||
        verify_smoke || verify_system12_drc_ram_mb)) return 4;
    if (verify_system12_framebuffer && (!system12_profile || !validation_frames ||
        verify_smoke || verify_system12_drc_ram_mb || verify_system12_vram)) return 4;
    if (verify_system12_texture_window && (!system12_profile || !validation_frames ||
        verify_smoke || verify_system12_drc_ram_mb || verify_system12_vram ||
        verify_system12_framebuffer)) return 4;
    if (verify_system12_vram_x_wrap && (!system12_profile || !validation_frames ||
        verify_smoke || verify_system12_drc_ram_mb || verify_system12_vram ||
        verify_system12_framebuffer || verify_system12_texture_window)) return 4;
    if (verify_system12_coord_alias && (!system12_profile || !validation_frames ||
        verify_smoke || verify_system12_drc_ram_mb || verify_system12_vram ||
        verify_system12_framebuffer || verify_system12_texture_window ||
        verify_system12_vram_x_wrap)) return 4;
    if (verify_system12_coord_span_cull && (!system12_profile || !validation_frames ||
        verify_smoke || verify_system12_drc_ram_mb || verify_system12_vram ||
        verify_system12_framebuffer || verify_system12_texture_window ||
        verify_system12_vram_x_wrap || verify_system12_coord_alias)) return 4;
    if (g_inputs_path[0] && (!game_path[0] || !validation_frames || verify_smoke ||
                            verify_system12_drc_ram_mb || verify_system12_vram ||
                            verify_system12_framebuffer || verify_system12_texture_window ||
                            verify_system12_vram_x_wrap || verify_system12_coord_alias ||
                            verify_system12_coord_span_cull)) {
        sceClibPrintf("pcsx-vita: --inputs requires a finite game run\n");
        return 4;
    }
    if (g_report_path[0] && g_capture_path[0] &&
        !strcmp(g_report_path, g_capture_path)) return 4;
    if ((g_inputs_path[0] && !strcmp(g_inputs_path, g_report_path)) ||
        (g_inputs_path[0] && !strcmp(g_inputs_path, g_capture_path))) return 4;
    if (system12_profile) {
        size_t profile_len = strlen(game_path);
        if ((g_report_path[0] && !strncmp(g_report_path, game_path, profile_len) &&
             (g_report_path[profile_len] == '/' || g_report_path[profile_len] == '\\')) ||
            (g_capture_path[0] && !strncmp(g_capture_path, game_path, profile_len) &&
             (g_capture_path[profile_len] == '/' || g_capture_path[profile_len] == '\\')))
            return 4;
    }
    /* PPM captures validate the CPU presentation path. */
    if (g_capture_path[0])
        g_gxm = false;
    if (g_inputs_path[0] && load_replay_inputs(g_inputs_path, validation_frames))
        return 4;
#ifndef ARCADE_SYSTEM12
    if (g_report_path[0]) return 4;
#endif

#ifdef STANDALONE_UI
#ifdef REARCADE_NEON
    /* The selector's game (--ui-game): menu mode. */
    if (g_ui_game[0]) {
#else
    /* No game and no validation run: menu mode. */
    if (game_path[0] == '\0' && !system12_profile && !validation_frames && !verify_smoke &&
        !verify_system12_drc_ram_mb) {
#endif
        g_ui_mode = true;
        VitaSettings_Init();
        VitaSaveStates_Init();
#ifdef VITA_DEBUG_SHOT
        {
            /* APP_ROOT/perf.txt: time the board's and GPU thread's work
             * piece by piece (a clock read per board command, which itself
             * costs a few ms a frame: off for fps measurements). */
            SceIoStat stat;
            if (sceIoGetstat(APP_ROOT "/perf.txt", &stat) >= 0) {
                extern int system12_perf_timing;
                system12_perf_timing = 1;
#ifdef USE_ASYNC_GPU
                extern int gpu_async_perf_timing;
                gpu_async_perf_timing = 1;
#endif
                sceClibPrintf("pcsx-vita: per-command timing on\n");
            }
        }
        {
            /* Measurements: APP_ROOT/gpu_core.txt, "GPU BOARD", picks the
             * cores of the GPU and board threads (-1: the default). */
            extern int pcsxr_gpu_core, pcsxr_board_core;
            char text[16] = { 0 };
            const SceUID fd = sceIoOpen(APP_ROOT "/gpu_core.txt", SCE_O_RDONLY, 0);
            if (fd >= 0) {
                sceIoRead(fd, text, sizeof(text) - 1);
                sceIoClose(fd);
                if (sscanf(text, "%d %d", &pcsxr_gpu_core, &pcsxr_board_core) < 1)
                    pcsxr_gpu_core = -1;
                sceClibPrintf("pcsx-vita: GPU thread on core %d, board on %d\n", pcsxr_gpu_core, pcsxr_board_core);
            }
        }
#endif
#ifdef PCSX_REARCADE
        /* The app runs as fast as the measurements do: System 12's board
         * on its own thread, and gpulib's GPU thread. */
        g_board_thread = true;
#ifdef USE_ASYNC_GPU
        g_gpu_thread = true;
#endif
#endif
    }
#endif
    sceClibPrintf("pcsx-vita: main entered\n");
    /* Apps start at the system's clocks (GPU 111 MHz): the vitaGL renderer
     * was GPU-bound at ~100 ms a frame there. Raise each clock to the Vita's
     * usual maximum, never lowering one an overclock plugin already set. */
    if (!g_stock_clocks) {
        if (scePowerGetArmClockFrequency() < 444) scePowerSetArmClockFrequency(444);
        if (scePowerGetBusClockFrequency() < 222) scePowerSetBusClockFrequency(222);
        if (scePowerGetGpuClockFrequency() < 222) scePowerSetGpuClockFrequency(222);
        if (scePowerGetGpuXbarClockFrequency() < 166) scePowerSetGpuXbarClockFrequency(166);
    }
    sceClibPrintf("pcsx-vita: clocks arm=%d bus=%d gpu=%d xbar=%d\n", scePowerGetArmClockFrequency(),
        scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    make_dirs();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    g_touch_ready = sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &g_touch_panel) >= 0 &&
        g_touch_panel.maxDispX > g_touch_panel.minDispX &&
        g_touch_panel.maxDispY > g_touch_panel.minDispY &&
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START) >= 0;

    ret = display_init();
    if (ret < 0) {
        sceClibPrintf("pcsx-vita: display init failed 0x%x\n", ret);
        return 1;
    }
    sceClibPrintf("pcsx-vita: display initialized\n");

    sceClibPrintf("pcsx-vita: entering retro_init\n");
    retro_set_environment(environment_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(NULL);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
#ifdef VITA_GPUPROF
    /* The renderer thread starts GXM during retro_load_game. */
    vita_gpuprof_install();
    vita_gpuprof_session_begin("pcsx");
    pcsx_vita_swap_cb = gpuprof_swap;
    pcsx_vita_gpu_label_cb = vita_gpuprof_label;
#endif
    retro_init();
    sceClibPrintf("pcsx-vita: retro_init complete\n");
#ifdef REARCADE_NEON
    /* vitaGL takes all the free memory but a threshold: the core's memory
     * blocks (retro_init) come first. */
    if (glp_init() < 0) {
        sceClibPrintf("pcsx-vita: vitaGL init failed\n");
        return 2;
    }
#endif

#ifdef STANDALONE_UI
    if (g_ui_mode) {
        ret = run_ui();
        retro_deinit();
        audio_finish();
        display_finish();
        if (g_vm_block > 0)
            sceKernelFreeMemBlock(g_vm_block);
        return ret;
    }
#endif
    if (game_path[0] == '\0' && !find_game(game_path, sizeof(game_path))) {
        sceClibPrintf("pcsx-vita: core initialized; no game found in %s\n", APP_ROOT);
        retro_deinit();
        audio_finish();
        display_finish();
        if (g_vm_block > 0)
            sceKernelFreeMemBlock(g_vm_block);
        return 2;
    }

    sceClibPrintf("pcsx-vita: booting %s\n", game_path);

    memset(&game, 0, sizeof(game));
    game.path = game_path;
    if (!retro_load_game(&game)) {
        sceClibPrintf("pcsx-vita: retro_load_game failed\n");
        retro_deinit();
        audio_finish();
        display_finish();
        if (g_vm_block > 0)
            sceKernelFreeMemBlock(g_vm_block);
        return 3;
    }

    retro_get_system_av_info(&av);
    start_audio(&av);

    unsigned runs = 0;
    const bool report_progress = validation_frames && (g_report_path[0] || g_replay_active);
    const uint64_t validation_started_ms = SDL_GetTicks();
    if (report_progress)
        sceClibPrintf("pcsx-vita: system12-progress frames=0/%u elapsed_ms=0 inputs=%u/%u\n",
            validation_frames, g_next_input_event, g_input_event_count);
    sceClibPrintf("pcsx-vita: cpu_driver=%s\n", pcsx_vita_uses_dynarec() ? "ari64" : "interpreter");
    while (!g_exit_requested && (!validation_frames || runs < validation_frames)) {
#ifdef STANDALONE_VITAGL
        /* Ask for each of the last frames' pictures; the GPU thread presents
         * them later, and the last one read back is kept. */
        if (g_capture_path[0] && validation_frames && runs + 30 >= validation_frames) {
            pcsx_vita_gl_capture_store = capture_gl_frame;
            pcsx_vita_gl_capture_request = 1;
        }
#endif
        retro_run(); ++runs;
        pace_frame();
        /* Keep the console awake: no auto suspend or screen off while
         * emulating (input from the replay never touches the system). */
        if (runs % 60 == 0)
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        if (report_progress && (runs % g_progress_every == 0 || runs == validation_frames)) {
            /* Source frames the audio device consumed since the last line, per
             * second of wall time: the pacing clock's real rate. */
            static uint64_t last_consumed, last_ms;
            const uint64_t now_ms = SDL_GetTicks();
            const uint64_t consumed = g_audio_put_frames -
                (g_audio_stream ? (uint64_t)SDL_GetAudioStreamQueued(g_audio_stream) / AUDIO_FRAME_BYTES : 0);
            const unsigned audio_hz = now_ms > last_ms && last_ms
                ? (unsigned)((consumed - last_consumed) * 1000 / (now_ms - last_ms)) : 0;
            last_consumed = consumed;
            last_ms = now_ms;
            sceClibPrintf("pcsx-vita: audio consumed_src_hz=%u ratio=%.5f queued_ms=%u\n", audio_hz,
                g_audio_ratio, g_audio_stream && av.timing.sample_rate > 0
                    ? (unsigned)((uint64_t)SDL_GetAudioStreamQueued(g_audio_stream) / AUDIO_FRAME_BYTES * 1000 /
                                 (uint64_t)av.timing.sample_rate) : 0u);
            SceKernelFreeMemorySizeInfo mem = { .size = sizeof(mem) };
            sceKernelGetFreeMemorySize(&mem);
#ifdef STANDALONE_VITAGL
            const unsigned gl_free_kb = (unsigned)(vglMemFree(VGL_MEM_ALL) / 1024);
#else
            const unsigned gl_free_kb = 0;
#endif
            sceClibPrintf("pcsx-vita: system12-progress frames=%u/%u elapsed_ms=%llu inputs=%u/%u free_user_kb=%u gl_free_kb=%u\n",
                runs, validation_frames,
                (unsigned long long)(SDL_GetTicks() - validation_started_ms),
                g_next_input_event, g_input_event_count, (unsigned)(mem.size_user / 1024), gl_free_kb);
#ifdef ARCADE_SYSTEM12
            print_wait_stats(runs);
#endif
        }
    }
    int validation_result = 0;
    if (g_report_path[0] || g_replay_active)
        sceClibPrintf("pcsx-vita: arcade-audio rate=%u frames=%llu peak=%u nonzero=%llu errors=%u\n",
            (unsigned)av.timing.sample_rate, (unsigned long long)g_audio_put_frames,
            g_audio_peak, (unsigned long long)g_audio_nonzero, g_audio_put_errors);
    if (g_replay_active) {
        bool replay_complete = g_next_input_event == g_input_event_count &&
                               runs == validation_frames;
        sceClibPrintf("pcsx-vita: inputs replay %s events=%u/%u frames=%u/%u final=%04x/%04x\n",
            replay_complete ? "PASS" : "FAIL", g_next_input_event, g_input_event_count,
            runs, validation_frames, g_replay_joypad[0], g_replay_joypad[1]);
        if (!replay_complete) validation_result = 8;
    }
    if (verify_smoke) {
        pcsx_vita_smoke_result result;
        pcsx_vita_read_smoke(&result);
        validation_result = !pcsx_vita_uses_dynarec() || result.triangle != 0x03ff ||
                            result.background != 0x2082 || result.sentinel != 0x534d4f4b;
        sceClibPrintf("pcsx-vita: smoke %s cpu=%s triangle=%04x background=%04x sentinel=%08x runs=%u\n",
            validation_result ? "FAIL" : "PASS", pcsx_vita_uses_dynarec() ? "ari64" : "interpreter",
            result.triangle, result.background, result.sentinel, runs);
    }
    if (verify_system12_vram) {
        const uint8_t *ram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        enum { VRAM_PATTERN_MAGIC = 0x53315458, VRAM_PATTERN_SAMPLES = 48,
               VRAM_PATTERN_FIRST_SAMPLE = 13,
               VRAM_PATTERN_WORDS = VRAM_PATTERN_FIRST_SAMPLE + VRAM_PATTERN_SAMPLES };
        uint32_t result[VRAM_PATTERN_WORDS] = {0};
        uint32_t texture_signature = 2166136261u;
        bool have_texture_results = false;
        bool texture_pass = false;
        size_t result_bytes = verify_system12_vram_pattern ? sizeof(result) : 12 * sizeof(result[0]);
        if (ram && retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) >= 0x23f100 + result_bytes) {
            memcpy(result, ram + 0x23f100, result_bytes);
            have_texture_results = verify_system12_vram_pattern;
        }
        if (have_texture_results) {
            texture_pass = result[12] == VRAM_PATTERN_MAGIC;
            for (unsigned i = VRAM_PATTERN_FIRST_SAMPLE; i < VRAM_PATTERN_WORDS; i++) {
                uint16_t pixel = (uint16_t)result[i];
                texture_signature = (texture_signature ^ (pixel & 0xff)) * 16777619u;
                texture_signature = (texture_signature ^ (pixel >> 8)) * 16777619u;
            }
            texture_pass = texture_pass && texture_signature == 0xfec74789u;
        }
        validation_result = !pcsx_vita_uses_dynarec() || result[0] != 0x53313256 ||
            result[1] != 0x7c00 || result[2] != 0x7c00 || result[3] != 0x7c00 ||
            result[4] != 0x03e0 || result[5] != 2 || result[6] != 0x1234 ||
            result[7] != 0x5678 || result[8] != 0x001f || result[9] != 0x7c00 ||
            result[10] != 0x03e0 || result[11] != 0x03e0;
        if (verify_system12_vram_pattern) {
            validation_result = validation_result || !texture_pass;
            sceClibPrintf("pcsx-vita: system12-vram %s cpu=%s magic=%08x p4=%04x p8=%04x odd8=%04x p16=%04x lower=%04x last=%04x wrap=%04x lower4=%04x upper4=%04x filllast=%04x fillwrap=%04x runs=%u texture=%s sig=%08x samples=%u\n",
                validation_result ? "FAIL" : "PASS",
                pcsx_vita_uses_dynarec() ? "ari64" : "interpreter", result[0],
                result[1], result[2], result[3], result[4], result[5], result[6], result[7], result[8], result[9], result[10], result[11], runs,
                texture_pass ? "PASS" : "FAIL", texture_signature, VRAM_PATTERN_SAMPLES);
        } else {
            sceClibPrintf("pcsx-vita: system12-vram %s cpu=%s magic=%08x p4=%04x p8=%04x odd8=%04x p16=%04x lower=%04x last=%04x wrap=%04x lower4=%04x upper4=%04x filllast=%04x fillwrap=%04x runs=%u\n",
                validation_result ? "FAIL" : "PASS",
                pcsx_vita_uses_dynarec() ? "ari64" : "interpreter", result[0],
                result[1], result[2], result[3], result[4], result[5], result[6], result[7], result[8], result[9], result[10], result[11], runs);
        }
    }
    if (verify_system12_framebuffer) {
        const uint8_t *ram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        uint32_t result[19] = {0};
        if (ram && retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) >= 0x23f100 + sizeof(result))
            memcpy(result, ram + 0x23f100, sizeof(result));
        validation_result = !pcsx_vita_uses_dynarec() || result[0] != 0x53314642 ||
            result[1] != 0x001f || result[2] != 0x03e0 || result[3] != 0x001f ||
            result[4] != 0x7c00 || result[5] != 0x7fff || result[6] != 0x03e0 ||
            result[7] != 0x7c1f || result[8] != 0x7c1f ||
            result[9] != 0x001f || result[10] != 0x03e0 || result[11] != 0x7c00 ||
            result[12] != 0x7fe0 || result[13] != 0x7c1f || result[14] != 0x7fff ||
            result[15] != 0x7fe0 || result[16] != 0x7c1f || result[17] != 0x7fff ||
            result[18] != 0x4210;
        sceClibPrintf("pcsx-vita: system12-framebuffer %s cpu=%s magic=%08x page0=%04x page1=%04x page0again=%04x clip=%04x upload=%04x postupload=%04x move=%04x source=%04x ft3a_tl=%04x ft3a_tr=%04x ft3a_bl=%04x ft3b_tl=%04x ft3b_tr=%04x ft3b_bl=%04x ft4_tl=%04x ft4_tr=%04x ft4_bl=%04x ft4_br=%04x runs=%u\n",
            validation_result ? "FAIL" : "PASS",
            pcsx_vita_uses_dynarec() ? "ari64" : "interpreter", result[0],
            result[1], result[2], result[3], result[4], result[5], result[6],
            result[7], result[8], result[9], result[10], result[11], result[12],
            result[13], result[14], result[15], result[16], result[17], result[18], runs);
    }
    if (verify_system12_texture_window) {
        const uint8_t *ram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        uint32_t result[25] = {0};
        if (ram && retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) >= 0x23f100 + sizeof(result))
            memcpy(result, ram + 0x23f100, sizeof(result));
        validation_result = !pcsx_vita_uses_dynarec() || result[0] != 0x53314642 ||
            result[1] != 0x001f || result[2] != 0x03e0 || result[3] != 0x001f ||
            result[4] != 0x7c00 || result[5] != 0x7fff || result[6] != 0x03e0 ||
            result[7] != 0x7c1f || result[8] != 0x7c1f ||
            result[9] != 0x001f || result[10] != 0x03e0 || result[11] != 0x7c00 ||
            result[12] != 0x7fe0 || result[13] != 0x7c1f || result[14] != 0x7fff ||
            result[15] != 0x7fe0 || result[16] != 0x7c1f || result[17] != 0x7fff ||
            result[18] != 0x4210 || result[19] != 0x7c1f || result[20] != 0x7c1f ||
            result[21] != 0x4210 || result[22] != 0x7fe0 || result[23] != 0x7c1f ||
            result[24] != 0x7fff;
        sceClibPrintf("pcsx-vita: system12-texture-window %s cpu=%s magic=%08x "
            "ft3a_tl=%04x ft3a_tr=%04x ft3a_bl=%04x ft3b_tl=%04x ft3b_tr=%04x ft3b_bl=%04x "
            "ft4_tl=%04x ft4_tr=%04x ft4_bl=%04x ft4_br=%04x "
            "tw_tl=%04x tw_tr=%04x tw_br=%04x reset_tl=%04x reset_tr=%04x reset_bl=%04x runs=%u\n",
            validation_result ? "FAIL" : "PASS",
            pcsx_vita_uses_dynarec() ? "ari64" : "interpreter", result[0],
            result[9], result[10], result[11], result[12], result[13], result[14],
            result[15], result[16], result[17], result[18], result[19], result[20],
            result[21], result[22], result[23], result[24], runs);
    }
    if (verify_system12_vram_x_wrap || verify_system12_coord_alias ||
        verify_system12_coord_span_cull) {
        const uint8_t *ram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        uint32_t result[33] = {0};
        const uint32_t *samples;
        static const uint16_t vram_x_wrap_expected[9] = {
            0x7fe0, 0x7fe0, 0x7fe0, 0x7c1f, 0x7c1f,
            0x001f, 0x03e0, 0x001f, 0x03e0
        };
        static const uint16_t coord_alias_expected[9] = {
            0x001f, 0x03e0, 0x03e0,
            0x001f, 0x03e0, 0x03e0,
            0x001f, 0x03e0, 0x03e0
        };
        static const uint16_t coord_span_cull_expected[14] = {
            0x001f, 0x001f, 0x001f, 0x03e0,
            0x001f, 0x001f, 0x001f, 0x03e0,
            0x001f, 0x001f, 0x001f, 0x03e0,
            0x03e0, 0x03e0
        };
        bool vram_x_mode = verify_system12_vram_x_wrap;
        bool coord_span_mode = verify_system12_coord_span_cull;
        const uint16_t *expected = vram_x_mode ? vram_x_wrap_expected :
            coord_span_mode ? coord_span_cull_expected : coord_alias_expected;
        unsigned sample_count = coord_span_mode ? 14 : 9;
        if (ram && retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) >= 0x23f100 + sizeof(result))
            memcpy(result, ram + 0x23f100, sizeof(result));
        samples = result + 19;
        validation_result = !pcsx_vita_uses_dynarec() || result[0] != 0x53314642;
        for (unsigned i = 0; i < sample_count; i++)
            validation_result = validation_result || (uint16_t)samples[i] != expected[i];
        if (vram_x_mode) {
            sceClibPrintf("pcsx-vita: system12-vram-x-wrap %s cpu=%s magic=%08x p8_poly_u128=%04x p8_poly_u136=%04x p8_sprite_u120=%04x p8_sprite_u136=%04x p8_clut_x4=%04x p16_poly_u64=%04x p16_poly_u72=%04x p16_sprite_u56=%04x p16_sprite_u64=%04x runs=%u\n",
                validation_result ? "FAIL" : "PASS",
                pcsx_vita_uses_dynarec() ? "ari64" : "interpreter", result[0],
                samples[0], samples[1], samples[2], samples[3], samples[4],
                samples[5], samples[6], samples[7], samples[8], runs);
        } else if (coord_span_mode) {
            sceClibPrintf("pcsx-vita: system12-coord-span-cull %s cpu=%s magic=%08x "
                "f3_1024_before=%04x f3_1024_after=%04x f3_1023_before=%04x f3_1023_after=%04x "
                "f4_invalid_before=%04x f4_valid_before=%04x f4_invalid_after=%04x f4_valid_after=%04x "
                "ft4_invalid_before=%04x ft4_valid_before=%04x ft4_invalid_after=%04x ft4_valid_after=%04x "
                "culled_ft3_tpage=%04x g4_valid_half=%04x runs=%u\n",
                validation_result ? "FAIL" : "PASS",
                pcsx_vita_uses_dynarec() ? "ari64" : "interpreter", result[0],
                samples[0], samples[1], samples[2], samples[3],
                samples[4], samples[5], samples[6], samples[7],
                samples[8], samples[9], samples[10], samples[11],
                samples[12], samples[13], runs);
        } else {
            sceClibPrintf("pcsx-vita: system12-coord-alias %s cpu=%s magic=%08x ft3_control=%04x ft3_upper=%04x ft3_negative=%04x ft4_control=%04x ft4_upper=%04x ft4_negative=%04x sprite_control=%04x sprite_upper=%04x sprite_negative=%04x runs=%u\n",
                validation_result ? "FAIL" : "PASS",
                pcsx_vita_uses_dynarec() ? "ari64" : "interpreter", result[0],
                samples[0], samples[1], samples[2], samples[3], samples[4],
                samples[5], samples[6], samples[7], samples[8], runs);
        }
    }
    if (verify_system12_drc_ram_mb) {
        pcsx_vita_system12_drc_result result;
        uint32_t expected_alias = verify_system12_drc_ram_mb == 4 ? 0x1357 : 0x2468;
        memset(&result, 0, sizeof(result));
        int probe_result = pcsx_vita_read_system12_drc(&result);
        validation_result = !pcsx_vita_uses_dynarec() || probe_result != 0 ||
            result.magic != 0x53313244 || result.ram_mb != verify_system12_drc_ram_mb ||
            result.bank0 != 0x1111 || result.bank1 != 0x2222 ||
            result.upper_ram != 0x51a7cafe || result.alias != expected_alias ||
            result.smc_before != 0x1234 || result.smc_after != 0x5678 ||
            result.dma5 != 0x3333 || result.shared_mmio != 0x76543210 ||
            result.shared_mmio_half != 0x1357 || result.shared_mmio_byte != 0x89;
        sceClibPrintf("pcsx-vita: system12-drc %s cpu=%s ram=%uM magic=%08x bank0=%08x bank1=%08x upper=%08x alias=%08x smc=%08x/%08x dma5=%08x mmio=%08x half=%08x byte=%08x runs=%u\n",
            validation_result ? "FAIL" : "PASS",
            pcsx_vita_uses_dynarec() ? "ari64" : "interpreter",
            result.ram_mb, result.magic, result.bank0, result.bank1,
            result.upper_ram, result.alias, result.smc_before, result.smc_after,
            result.dma5, result.shared_mmio, result.shared_mmio_half,
            result.shared_mmio_byte, runs);
    }

#ifdef ARCADE_SYSTEM12
    if (g_report_path[0]) {
        int report_result = pcsx_vita_write_system12_report(g_report_path);
        sceClibPrintf("pcsx-vita: system12-report %s path=%s\n",
            report_result == 0 ? "PASS" : "FAIL", g_report_path);
        if (report_result != 0)
            validation_result = 6;
    }
#endif
    if (g_capture_path[0]) {
        int capture_result;
#ifdef STANDALONE_VITAGL
        for (int wait = 0; wait < 100 && !g_capture_width; wait++)
            sceKernelDelayThread(10000);
#endif
        capture_result = write_capture_ppm();
        sceClibPrintf("pcsx-vita: system12-capture %s path=%s width=%u height=%u\n",
            capture_result == 0 ? "PASS" : "FAIL", g_capture_path,
            g_capture_width, g_capture_height);
        if (capture_result != 0)
            validation_result = 7;
    }

    /* Stop SDL playback before unloading any core board or plugins. */
    audio_finish();
    retro_unload_game();
    retro_deinit();
    display_finish();
    if (g_vm_block > 0)
        sceKernelFreeMemBlock(g_vm_block);
    free(g_capture_pixels);
    return validation_result ? 5 : 0;
}
