/* PlayStation Vita hardware GPU backend for PCSX-ReARMed. The native renderer is derived from REDRIVER2/PsyCross MIT code through CTR Native / the PSXRecomp Vita integration. Keep the corresponding third-party notices when redistributing this backend. */

#include <SDL3/SDL.h>
#include "platform/native_vita_path.h"
#ifdef __vita__
#include "app_root.h"
#endif
#if defined(__vita__)
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <vitaGL.h>
#elif defined(PCSX_NATIVE_VITA_PATH)
#include "platform/mac_vita_compat.h"
#endif

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PCSX_GP0_ROI_TRACE
#include "gp0_roi_trace.h"
#endif

#include <macros.h>
#include <platform.h>
#include "platform/native_gpu.h"
#include "platform/native_gpu_links.h"
#include "platform/native_renderer.h"

#ifdef force_inline
#undef force_inline
#endif
#include "../gpulib/gpu.h"
#include "../gpulib/gpu_timing.h"
#include "../../frontend/plugin_lib.h"

SDL_Window *g_window;

#if !defined(__vita__)
/* gpulib's GPU thread (Mac harness, USE_ASYNC_GPU): the renderer has no
 * thread of its own there, so GL is called from the GPU thread (lists,
 * presents) and from the emulation thread (VRAM reads), never at once:
 * gpulib syncs the GPU thread first. Each entry makes the context current
 * on the calling thread. */
SDL_GLContext g_glContext;
void renderer_gl_current(void)
{
    if (g_glContext && g_window && SDL_GL_GetCurrentContext() != g_glContext)
        SDL_GL_MakeCurrent(g_window, g_glContext);
}
void renderer_gl_thread(int attach)
{
    if (attach)
        renderer_gl_current();
}
#define GL_ENTER() renderer_gl_current()
#else
#define GL_ENTER()
#endif
int gNativeAntiAliasingEnabled;
int gNativeDitheringEnabled = 1;
int g_dbg_emulatorPaused;
int g_dbg_polygonSelected = -1;

/* Set while the backend is stopped; shader/storage geometry stays fixed per context. */
unsigned g_nativeVramHeight = GPU_VRAM_ROWS_CONSOLE;

static int s_context_ready;
static int s_frame_started;
static uint16_t *s_guest_vram;

#ifdef PCSX_GP0_ROI_TRACE
static void gp0_roi_trace_record_rect(int kind, int x, int y, int w, int h);
static void gp0_roi_trace_dump(void);
#endif

void Platform_Log(const char *fmt, ...);

extern int ParseTaglessPrimitive(u32 *command);
extern void VitaCtrNativeGpu_SetDisplayEnv(int x, int y, int w, int h, int pic_x, int pic_y, int pic_w, int pic_h,
    int depth24, int wide3d);
extern unsigned gte_widescreen_ops;
extern void VitaCtrNativeRenderer_SetPresentationAspect(int width, int height);
extern void VitaCtrNativeRenderer_SetAspectMode(int mode);
extern int VitaCtrNativeRenderer_GetAspectMode(void);
extern void VitaCtrNativeRenderer_CopyGuestVRAM(const u16 *src, int x, int y, int w, int h);
extern void VitaCtrNativeRenderer_ReadGuestVRAM(u16 *dst, int x, int y, int w, int h);
extern void NativeGpu_SetFrontendDisplayBlank(int blank);
extern int NativeGpu_GetRenderDisplayBlank(void);
extern void NativeGpu_ForceSynchronousFrame(void);
extern void NativeRenderer_PresentBlank(void);

void Platform_Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

void Platform_LogError(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void Platform_LogWarn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void Platform_LogFlush(void)
{
    fflush(stdout);
    fflush(stderr);
}

void Platform_PollHostEvents(void) {}

int NativeAdhoc_IsDialogRunning(void)
{
    return 0;
}

b32 NativeGhostInput_GetReplayOverlayState(u32 *buttonsHeld, u8 *stickLX,
    u8 *stickLY, u8 *stickRX, u8 *stickRY)
{
    (void)buttonsHeld;
    (void)stickLX;
    (void)stickLY;
    (void)stickRX;
    (void)stickRY;
    return 0;
}

int NativeGpuLinks_IsRegisteredHostPointer(const void *hostPtr)
{
    (void)hostPtr;
    return 0;
}

int NativeGpuLinks_IsRegisteredHostRange(const void *hostPtr, size_t size)
{
    (void)hostPtr;
    (void)size;
    return 0;
}

int Platform_BeginScene(void)
{
    NativeRenderer_BeginScene();
    return 1;
}

#if defined(__vita__)
/* A frontend's picture capture (the standalone's --capture-ppm): once the
 * request is set, the next presented picture is read back in bands of RGBA
 * glReadPixels rows (row 0 at the bottom) and handed to the callback. */
volatile int pcsx_vita_gl_capture_request;
void (*pcsx_vita_gl_capture_store)(const uint8_t *rgba, unsigned y, unsigned rows,
                                   unsigned width, unsigned height);

static void capture_gl_frame(void)
{
    enum { BAND = 16, MAX_WIDTH = 1024 };
    static uint8_t band[MAX_WIDTH * BAND * 4];
    extern int g_windowWidth, g_windowHeight;
    const int width = g_windowWidth, height = g_windowHeight;
    if (width <= 0 || width > MAX_WIDTH || height <= 0)
        return;
    /* After vglSwapBuffers the front buffer holds the picture; vitaGL is
     * built with READBACKS_SPEEDHACK, so wait for the GPU explicitly. */
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadBuffer(GL_FRONT);
    for (int y = 0; y < height; y += BAND) {
        const int rows = height - y < BAND ? height - y : BAND;
        glReadPixels(0, y, width, rows, GL_RGBA, GL_UNSIGNED_BYTE, band);
        pcsx_vita_gl_capture_store(band, y, rows, width, height);
    }
    glReadBuffer(GL_BACK);
}
#endif

/* Set by a frontend when a menu is about to open over the game: the next
 * present keeps a copy of the frame (VitaCtrNativeRenderer_PresentCurrentFrame). */
volatile int pcsx_menu_backdrop_request;

void Platform_EndScene(void)
{
    const DISPENV *display = NativeGpu_GetRenderDispEnv();
    const DRAWENV *draw = NativeGpu_GetRenderDrawEnv();
    const int blank = NativeGpu_GetRenderDisplayBlank();

    NativeRenderer_EndScene();
    if (blank) {
        if (!display->isrgb24)
            NativeRenderer_StoreFrameBuffer(draw->clip.x, draw->clip.y, draw->clip.w, draw->clip.h);
        NativeRenderer_PresentBlank();
    }
    else if (display->isrgb24) {
        NativeRenderer_PresentVRAMDisplay();
    }
    else {
        NativeRenderer_StoreFrameBuffer(draw->clip.x, draw->clip.y, draw->clip.w, draw->clip.h);
        /* Scanout reads DISPENV, independently of the final GP0 draw page;
         * the high-resolution target covers it where it still holds it. */
        NativeRenderer_PresentVRAMRect(display->disp.x, display->disp.y, display->disp.w, display->disp.h);
        NativeRenderer_PresentMainOverlay(display->disp.x, display->disp.y, display->disp.w, display->disp.h);
    }
    NativeRenderer_FinishPresentation();
    const int keep_backdrop = pcsx_menu_backdrop_request;
    pcsx_menu_backdrop_request = 0;
    if (!keep_backdrop)
        NativeRenderer_InvalidateBackdrop();
#ifndef __vita__
    else
        NativeRenderer_CaptureBackdrop(); /* the back buffer, before the swap */
#endif
#if defined(PCSX_NATIVE_VITA_PATH) && !defined(__vita__)
    extern void harness_capture_gl_frame(unsigned int frame);
    if (pl_rearmed_cbs.gpu_frame_count != NULL)
        harness_capture_gl_frame(*pl_rearmed_cbs.gpu_frame_count);
#endif
    NativeRenderer_SwapWindow();
#if defined(__vita__)
    if (keep_backdrop) {
        /* vitaGL ends the scene at the swap: the front buffer holds it. */
        glReadBuffer(GL_FRONT);
        NativeRenderer_CaptureBackdrop();
        glReadBuffer(GL_BACK);
    }
    if (pcsx_vita_gl_capture_request && pcsx_vita_gl_capture_store) {
        pcsx_vita_gl_capture_request = 0;
        capture_gl_frame();
    }
#endif
}

/* The last frame again, for a menu drawn over it on the renderer thread. */
void VitaCtrNativeRenderer_PresentCurrentFrame(void)
{
    NativeRenderer_BeginMenuRedraw();
    /* The frame as it was presented, if it was kept for this menu. */
    if (NativeRenderer_PresentBackdrop())
        return;
    const DISPENV *display = NativeGpu_GetRenderDispEnv();

    if (NativeGpu_GetRenderDisplayBlank())
        NativeRenderer_PresentBlank();
    else if (display->isrgb24)
        NativeRenderer_PresentVRAMDisplay();
    else {
        NativeRenderer_PresentVRAMRect(display->disp.x, display->disp.y, display->disp.w, display->disp.h);
        NativeRenderer_PresentShownOverlay(display->disp.x, display->disp.y, display->disp.w, display->disp.h);
    }
    NativeRenderer_FinishPresentation();
}

typedef struct VitaCtrVramTask {
    uint16_t *pixels;
    int x, y, w, h;
    int dst_x, dst_y;
    uint8_t r, g, b;
} VitaCtrVramTask;

static void backend_clear(void *arg)
{
    VitaCtrVramTask *task = (VitaCtrVramTask *)arg;
    NativeRenderer_EndScene();
    NativeRenderer_ClearVRAM(task->x, task->y, task->w, task->h,
        task->r, task->g, task->b);
}

static void backend_copy(void *arg)
{
    VitaCtrVramTask *task = (VitaCtrVramTask *)arg;
    NativeRenderer_EndScene();
    NativeRenderer_CopyVRAM(task->pixels, task->x, task->y, task->w, task->h,
        task->dst_x, task->dst_y);
}

static void backend_read(void *arg)
{
    VitaCtrVramTask *task = (VitaCtrVramTask *)arg;
    NativeRenderer_EndScene();
    NativeRenderer_ReadVRAM(task->pixels, task->x, task->y, task->w, task->h);
}

static void backend_copy_guest(void *arg)
{
    VitaCtrVramTask *task = (VitaCtrVramTask *)arg;
    NativeRenderer_EndScene();
    VitaCtrNativeRenderer_CopyGuestVRAM(s_guest_vram,
        task->x, task->y, task->w, task->h);
    NativeRenderer_UpdateVRAM();
}

static void backend_read_guest(void *arg)
{
    VitaCtrVramTask *task = (VitaCtrVramTask *)arg;
    NativeRenderer_EndScene();
    VitaCtrNativeRenderer_ReadGuestVRAM(s_guest_vram,
        task->x, task->y, task->w, task->h);
}

static void vram_task_sync(NativeGpuBackendTaskFn callback, VitaCtrVramTask *task)
{
    /* Fills and image copies write the renderer's VRAM, not the guest's. */
    NativeGpu_GuestVRAMWritten(task->x, task->y, task->w, task->h);
    NativeGpu_GuestVRAMWritten(task->dst_x, task->dst_y, task->w, task->h);
    if (NativeGpu_HasPendingSplits()) {
        NativeGpu_ForceSynchronousFrame();
        NativeGpu_FlushFrontendSplitsSync();
    }
    NativeGpu_RunBackendTaskSync(callback, task);
}

int ClearImage(RECT16 *rect, uint8_t r, uint8_t g, uint8_t b)
{
    VitaCtrVramTask task;
    if (!rect)
        return 0;
    memset(&task, 0, sizeof(task));
    task.x = rect->x;
    task.y = rect->y;
    task.w = rect->w;
    task.h = rect->h;
    task.r = r;
    task.g = g;
    task.b = b;
    vram_task_sync(backend_clear, &task);
    return 0;
}

int LoadImage(RECT16 *rect, void *p)
{
    VitaCtrVramTask task;
    if (!rect || !p)
        return 0;
#ifdef PCSX_GP0_ROI_TRACE
    gp0_roi_trace_record_rect(GP0_ROI_TRACE_LOAD_IMAGE,
        rect->x, rect->y, rect->w, rect->h);
#endif
    memset(&task, 0, sizeof(task));
    task.pixels = (uint16_t *)p;
    task.w = rect->w;
    task.h = rect->h;
    task.dst_x = rect->x;
    task.dst_y = rect->y;
    vram_task_sync(backend_copy, &task);
    return 0;
}

int MoveImage(RECT16 *rect, int x, int y)
{
    VitaCtrVramTask task;
    if (!rect)
        return 0;
    memset(&task, 0, sizeof(task));
    task.x = rect->x;
    task.y = rect->y;
    task.w = rect->w;
    task.h = rect->h;
    task.dst_x = x;
    task.dst_y = y;
    vram_task_sync(backend_copy, &task);
    return 0;
}

int StoreImage(RECT16 *rect, uint32_t *p)
{
    VitaCtrVramTask task;
    if (!rect || !p)
        return 0;
    memset(&task, 0, sizeof(task));
    task.pixels = (uint16_t *)p;
    task.x = rect->x;
    task.y = rect->y;
    task.w = rect->w;
    task.h = rect->h;
    vram_task_sync(backend_read, &task);
    return 0;
}

static uint32_t normalize_gp0_xy(uint32_t word)
{
    int x = word & 0x7ff;
    int y = (word >> 16) & 0x7ff;
    if (x & 0x400) x -= 0x800;
    if (y & 0x400) y -= 0x800;
    return (uint16_t)x | ((uint32_t)(uint16_t)y << 16);
}

static void normalize_gp0_coordinates(uint32_t *words, int count)
{
    const unsigned command = words[0] >> 24;
    if ((command & 0xe0) == 0x20) {
        const int vertices = (command & 8) ? 4 : 3;
        const int stride = 1 + !!(command & 4) + !!(command & 16);
        for (int i = 0, offset = 1; i < vertices && offset < count; i++, offset += stride)
            words[offset] = normalize_gp0_xy(words[offset]);
    } else if ((command & 0xe0) == 0x40) {
        const int stride = (command & 16) ? 2 : 1;
        for (int offset = 1; offset < count; offset += stride) {
            if ((command & 8) && (words[offset] & 0xf000f000u) == 0x50005000u)
                break;
            words[offset] = normalize_gp0_xy(words[offset]);
        }
    } else if ((command & 0xe0) == 0x60 && count > 1) {
        words[1] = normalize_gp0_xy(words[1]);
    }
}

#ifdef PCSX_GP0_ROI_TRACE
static uint32_t gp0_roi_trace_frame(void)
{
	return pl_rearmed_cbs.gpu_frame_count != NULL
		? (uint32_t)*pl_rearmed_cbs.gpu_frame_count : UINT32_MAX;
}

static int gp0_roi_trace_frame_enabled(uint32_t frame)
{
	return frame >= GP0_ROI_TRACE_FIRST_FRAME && frame <= GP0_ROI_TRACE_LAST_FRAME;
}

static int gp0_roi_trace_point_mask(int x0, int y0, int x1, int y1)
{
	static const int points[5][2] = {
		{ 64, 100 }, { 63, 100 }, { 65, 100 }, { 64, 99 }, { 64, 101 }
	};
	int mask = 0;
	for (int i = 0; i < 5; i++) {
		if (points[i][0] >= x0 && points[i][0] <= x1 &&
		    points[i][1] >= y0 && points[i][1] <= y1)
			mask |= 1 << i;
	}
	return mask;
}

static int gp0_roi_trace_wrapped_rect_point_mask(int x, int y, int w, int h)
{
	static const int points[5][2] = {
		{ 64, 100 }, { 63, 100 }, { 65, 100 }, { 64, 99 }, { 64, 101 }
	};
	int mask = 0;
	if (w <= 0 || h <= 0 || gpu_vram_height == 0)
		return 0;
	for (int i = 0; i < 5; i++) {
		const int dx = (points[i][0] - x + 1024) & 0x3ff;
		const int dy = (points[i][1] - y + (int)gpu_vram_height) % (int)gpu_vram_height;
		if (dx < w && dy < h)
			mask |= 1 << i;
	}
	return mask;
}

static int gp0_roi_signed11(uint32_t value)
{
	value &= 0x7ff;
	return (value & 0x400) ? (int)value - 0x800 : (int)value;
}

static int gp0_roi_packet_bounds(const uint32_t *words, int count,
				 int *x0, int *y0, int *x1, int *y1)
{
	const unsigned command = words[0] >> 24;
	const int ox = gp0_roi_signed11(gpu.ex_regs[5]);
	const int oy = gp0_roi_signed11(gpu.ex_regs[5] >> 11);
	int have = 0;

	if (command == 0x02 && count >= 3) {
		/* GP0 fill is VRAM-relative: it ignores E5 and uses 10-bit VRAM
		 * coordinates. X is aligned down to 16 pixels; Y wraps at the
		 * active VRAM height (1024 rows for System 12). */
		const int x = (words[1] & 0x3f0);
		const int y = (words[1] >> 16) & GPU_VRAM_Y_MASK;
		int w = (words[2] & 0x3ff) + 0xf;
		int h = (words[2] >> 16) & GPU_VRAM_Y_MASK;
		w &= ~0xf;
		if (w <= 0 || h <= 0) return 0;
		*x0 = x; *y0 = y; *x1 = x + w - 1; *y1 = y + h - 1;
		return 1;
	}
	if ((command & 0xe0) == 0x20) {
		const int vertices = (command & 8) ? 4 : 3;
		const int stride = 1 + !!(command & 4) + !!(command & 16);
		if (count < 1 + (vertices - 1) * stride + 1) return 0;
		for (int i = 0; i < vertices; i++) {
			const uint32_t xy = words[1 + i * stride];
			const int x = (int16_t)(xy & 0xffff) + ox;
			const int y = (int16_t)(xy >> 16) + oy;
			if (!have) { *x0 = *x1 = x; *y0 = *y1 = y; have = 1; }
			else {
				if (x < *x0) *x0 = x; if (x > *x1) *x1 = x;
				if (y < *y0) *y0 = y; if (y > *y1) *y1 = y;
			}
		}
		return have;
	}
	if ((command & 0xe0) == 0x40) {
		const int stride = (command & 16) ? 2 : 1;
		for (int offset = 1; offset < count; offset += stride) {
			if ((command & 8) && (words[offset] & 0xf000f000u) == 0x50005000u)
				break;
			const uint32_t xy = words[offset];
			const int x = (int16_t)(xy & 0xffff) + ox;
			const int y = (int16_t)(xy >> 16) + oy;
			if (!have) { *x0 = *x1 = x; *y0 = *y1 = y; have = 1; }
			else {
				if (x < *x0) *x0 = x; if (x > *x1) *x1 = x;
				if (y < *y0) *y0 = y; if (y > *y1) *y1 = y;
			}
		}
		return have;
	}
	if ((command & 0xe0) == 0x60 && count > 1) {
		const uint32_t xy = words[1];
		const int x = (int16_t)(xy & 0xffff) + ox;
		const int y = (int16_t)(xy >> 16) + oy;
		int w, h;
		if (command >= 0x78) w = h = 16;
		else if (command >= 0x70) w = h = 8;
		else if (command >= 0x68) w = h = 1;
		else if (count > ((command & 4) ? 3 : 2)) {
			const int size_index = (command & 4) ? 3 : 2;
			w = words[size_index] & 0x3ff;
			h = (words[size_index] >> 16) & 0x1ff;
		} else return 0;
		if (w <= 0 || h <= 0) return 0;
		*x0 = x; *y0 = y; *x1 = x + w - 1; *y1 = y + h - 1;
		return 1;
	}
	return 0;
}

static void gp0_roi_trace_record_packet(const uint32_t *raw,
					const uint32_t *normalized, int count,
					const uint32_t *ex_regs,
					unsigned accepted_mask)
{
	gp0_roi_trace_record *record;
	uint32_t frame = gp0_roi_trace_frame();
	int x0, y0, x1, y1;
	int mask;
	unsigned command;
	if (!gp0_roi_trace_frame_enabled(frame) || count <= 0 ||
	    !gp0_roi_packet_bounds(normalized, count, &x0, &y0, &x1, &y1))
		return;
	command = normalized[0] >> 24;
	if (command == 0x02 && count >= 3) {
		const int x = normalized[1] & 0x3f0;
		const int y = (normalized[1] >> 16) & GPU_VRAM_Y_MASK;
		const int w = ((normalized[2] & 0x3ff) + 0xf) & ~0xf;
		const int h = (normalized[2] >> 16) & GPU_VRAM_Y_MASK;
		mask = gp0_roi_trace_wrapped_rect_point_mask(x, y, w, h);
	} else {
		mask = gp0_roi_trace_point_mask(x0, y0, x1, y1);
	}
	if (!mask) return;
	record = &gp0_roi_trace_ring[gp0_roi_trace_sequence % GP0_ROI_TRACE_CAPACITY];
	memset(record, 0, sizeof(*record));
	record->frame = frame;
	record->sequence = gp0_roi_trace_sequence++;
	record->kind = GP0_ROI_TRACE_PACKET;
	record->count = (uint16_t)count;
	record->stored_words = (uint8_t)(count < GP0_ROI_TRACE_WORDS ? count : GP0_ROI_TRACE_WORDS);
	record->x0 = (uint16_t)x0; record->y0 = (uint16_t)y0;
	record->x1 = (uint16_t)x1; record->y1 = (uint16_t)y1;
	record->point_mask = (uint8_t)mask;
	record->accepted_mask = (uint8_t)accepted_mask;
	if (ex_regs != NULL)
		for (int i = 1; i <= 6; i++) record->regs[i] = ex_regs[i];
	for (int i = 0; i < record->stored_words; i++) {
		record->raw[i] = raw[i];
		record->normalized[i] = normalized[i];
	}
	record->color = normalized[0] & 0x00ffffffu;
	if ((command & 0xe0) == 0x20) {
		const int vertices = (command & 8) ? 4 : 3;
		const int stride = 1 + !!(command & 4) + !!(command & 16);
		for (int i = 0; i < vertices; i++) {
			const int uv_index = 1 + i * stride + 1;
			if ((command & 4) && uv_index < count)
				record->uv[i] = normalized[uv_index];
		}
		if (command & 4) {
			record->clut = (uint16_t)(record->uv[0] >> 16);
			record->tpage = (uint16_t)(record->uv[1] >> 16);
		}
	} else if ((command & 0xe0) == 0x60 && (command & 4) && count > 2) {
		record->uv[0] = normalized[2];
		record->clut = (uint16_t)(normalized[2] >> 16);
		record->tpage = (uint16_t)record->regs[1];
	}
}

static void gp0_roi_trace_record_rect(int kind, int x, int y, int w, int h)
{
	gp0_roi_trace_record *record;
	uint32_t frame = gp0_roi_trace_frame();
	const int mask = gp0_roi_trace_point_mask(x, y, x + w - 1, y + h - 1);
	if (!gp0_roi_trace_frame_enabled(frame) || !mask || w <= 0 || h <= 0)
		return;
	record = &gp0_roi_trace_ring[gp0_roi_trace_sequence % GP0_ROI_TRACE_CAPACITY];
	memset(record, 0, sizeof(*record));
	record->frame = frame;
	record->sequence = gp0_roi_trace_sequence++;
	record->kind = (uint8_t)kind;
	record->x0 = (uint16_t)x; record->y0 = (uint16_t)y;
	record->x1 = (uint16_t)(x + w - 1); record->y1 = (uint16_t)(y + h - 1);
	record->point_mask = (uint8_t)mask;
	for (int i = 1; i <= 6; i++) record->regs[i] = gpu.ex_regs[i];
}

static void gp0_roi_trace_dump(void)
{
	const uint32_t first = gp0_roi_trace_sequence > GP0_ROI_TRACE_CAPACITY
		? gp0_roi_trace_sequence - GP0_ROI_TRACE_CAPACITY : 0;
	fprintf(stderr, "GP0 ROI trace frames=%d..%d candidates=%u ring=%u\n",
		GP0_ROI_TRACE_FIRST_FRAME, GP0_ROI_TRACE_LAST_FRAME,
		gp0_roi_trace_sequence, GP0_ROI_TRACE_CAPACITY);
	for (uint32_t sequence = first; sequence < gp0_roi_trace_sequence; sequence++) {
		const gp0_roi_trace_record *record =
			&gp0_roi_trace_ring[sequence % GP0_ROI_TRACE_CAPACITY];
		if (record->sequence != sequence) continue;
		fprintf(stderr,
			"GP0 ROI seq=%u frame=%u kind=%u box=%d,%d..%d,%d points=%02x accepted=%02x "
			"E1=%08x E2=%08x E3=%08x E4=%08x E5=%08x E6=%08x color=%06x CLUT=%04x TPAGE=%04x count=%u\n",
			record->sequence, record->frame, record->kind,
			(int16_t)record->x0, (int16_t)record->y0,
			(int16_t)record->x1, (int16_t)record->y1,
			record->point_mask, record->accepted_mask,
			record->regs[1], record->regs[2], record->regs[3], record->regs[4],
			record->regs[5], record->regs[6], record->color,
			record->clut, record->tpage, record->count);
		for (int i = 0; i < record->stored_words; i++)
			fprintf(stderr, "  w[%02d] raw=%08x normalized=%08x\n",
				i, record->raw[i], record->normalized[i]);
		for (int i = 0; i < 4; i++)
			if (record->uv[i] != 0)
				fprintf(stderr, "  uv[%d]=%08x\n", i, record->uv[i]);
	}
	if (s_guest_vram != NULL) {
		static const int points[5][2] = {
			{ 64, 100 }, { 63, 100 }, { 65, 100 }, { 64, 99 }, { 64, 101 }
		};
		for (int i = 0; i < 5; i++) {
			const int x = points[i][0], y = points[i][1];
			fprintf(stderr, "GP0 ROI witness x=%d y=%d guest_vram=%04x\n",
				x, y, s_guest_vram[y * VRAM_WIDTH + x]);
		}
	}
}
#endif

static int gp0_triangle_x_span_valid(const uint32_t *words, int stride,
                                     int a, int b, int c)
{
    int xa = (int16_t)(words[1 + a * stride] & 0xffff);
    int xb = (int16_t)(words[1 + b * stride] & 0xffff);
    int xc = (int16_t)(words[1 + c * stride] & 0xffff);
    int min_x = xa < xb ? xa : xb;
    int max_x = xa > xb ? xa : xb;

    if (xc < min_x) min_x = xc;
    if (xc > max_x) max_x = xc;
    return max_x - min_x < 1024;
}

/* Keep the guest registers in hardware format. Normalize only the parser's
 * copy; bit 13 remains the type-1 indexed-texture interleave flag. */
static uint32_t native_draw_mode(uint32_t word)
{
    return (word & 0xff000000u) | gpu_tpage_to_renderer(word);
}

static void normalize_type1_packet(uint32_t *words)
{
    if (gpu_get_model() != 1) return;
    unsigned cmd = words[0] >> 24;
    if (cmd >= 0x24 && cmd <= 0x3f && (cmd & 4)) {
        unsigned index = 4 + ((cmd >> 4) & 1);
        words[index] = (words[index] & 0xffffu) |
            (gpu_tpage_to_renderer(words[index] >> 16) << 16);
    } else if (cmd == 0xe1) {
        words[0] = native_draw_mode(words[0]);
    } else if (cmd == 0xe3 || cmd == 0xe4) {
        words[0] = (words[0] & 0xff0003ffu) | (((words[0] >> 12) & 0x3ffu) << 10);
    } else if (cmd == 0xe5) {
        words[0] = (words[0] & 0xff0007ffu) | (((words[0] >> 12) & 0x7ffu) << 11);
    }
}

/* NEON/MAME reject each polygon triangle whose X span reaches 1024 pixels.
 * Keep this at the GP0 boundary so the renderer sees the same geometry and
 * texture state that the software rasterizer would. */
static unsigned submit_gp0_polygon(uint32_t *words, int count, const uint32_t *ex_regs)
{
    const unsigned command = words[0] >> 24;
    const int is_quad = (command & 8) != 0;
    const int textured = (command & 4) != 0;
    const int gouraud = (command & 16) != 0;
    const int stride = 1 + textured + gouraud;
    if (count < 1 + (is_quad ? 4 : 3) * stride - gouraud)
        return 0;
    const int valid_012 = gp0_triangle_x_span_valid(words, stride, 0, 1, 2);

    if (!is_quad) {
        if (valid_012) {
            ParseTaglessPrimitive((u32 *)words);
        } else if (textured && ex_regs) {
            /* The GP0 parser keeps draw state apart from gpulib's ex_regs. */
            uint32_t draw_env = gpu_get_model() == 1 ? native_draw_mode(ex_regs[1]) : ex_regs[1];
            ParseTaglessPrimitive((u32 *)&draw_env);
        }
        return valid_012 ? 1u : 0u;
    }

    const int valid_123 = gp0_triangle_x_span_valid(words, stride, 1, 2, 3);
    if (valid_012 && valid_123) {
        ParseTaglessPrimitive((u32 *)words);
        return 3u;
    }

    if (!valid_012 && !valid_123) {
        if (textured && ex_regs) {
            uint32_t draw_env = gpu_get_model() == 1 ? native_draw_mode(ex_regs[1]) : ex_regs[1];
            ParseTaglessPrimitive((u32 *)&draw_env);
        }
        return 0;
    }

    /* Rebuild only the valid half. Gouraud colors follow the previous
     * vertex's attributes; textured triangles carry CLUT/TPAGE in UV0/UV1. */
    const int vertices[3] = { valid_012 ? 0 : 1,
                               valid_012 ? 1 : 2,
                               valid_012 ? 2 : 3 };
    uint32_t triangle[1 + 1 + 3 * 3];
    int out = 1;
    triangle[out] = words[0] & ~(8u << 24);
    if (gouraud && vertices[0] > 0) {
        const int prior_color = 1 + (vertices[0] - 1) * stride + stride - 1;
        triangle[out] = (triangle[out] & 0xff000000u) | (words[prior_color] & 0x00ffffffu);
    }
    out++;
    for (int i = 0; i < 3; i++) {
        const int vertex = vertices[i];
        if (i > 0 && gouraud) {
            const int prior_color = 1 + (vertex - 1) * stride + stride - 1;
            triangle[out++] = words[prior_color];
        }
        triangle[out++] = words[1 + vertex * stride];
        if (textured) {
            uint32_t uv = words[1 + vertex * stride + 1];
            if (i < 2) {
                const int sideband = 1 + i * stride + 1;
                uv = (uv & 0x0000ffffu) | (words[sideband] & 0xffff0000u);
            }
            triangle[out++] = uv;
        }
    }
    triangle[0] = ((uint32_t)(out - 1) << 24) | 0x00ffffffu;
    ParseTaglessPrimitive((u32 *)&triangle[1]);
    return valid_012 ? 1u : 2u;
}

static void submit_gp0_packet(const uint32_t *words, int count, const uint32_t *ex_regs)
{
    uint32_t packet[257];

    if (!s_context_ready || !words || count <= 0 || count > 255)
        return;

    /* GP0 control words are not PsyCross retail primitives. In particular,
     * GP0(01h) clears the PSX texture cache in one word, while the native
     * parser uses 01h for a five-word DR_MOVE. Forwarding the one-word packet
     * reads its nonexistent rectangle and can write outside VRAM. This
     * backend maintains coherent texture caches on VRAM changes, so consume
     * the cache-clear/NOP/IRQ words as gpulib's direct control path does.
     * The only drawable GP0 command in this group is 02h (VRAM fill). */
    if ((LE32TOH(words[0]) >> 24) < 0x20 && (LE32TOH(words[0]) >> 24) != 0x02)
        return;

    packet[0] = ((uint32_t)count << 24) | 0x00ffffffu;
    memcpy(packet + 1, words, (size_t)count * sizeof(uint32_t));
    normalize_type1_packet(packet + 1);
    /* GP0 ignores coordinate bits 15:11. Normalize only geometry here;
     * VRAM transfer positions, dimensions, colors and UVs retain their ABI. */
    normalize_gp0_coordinates(packet + 1, count);
    const unsigned command = packet[1] >> 24;
    if ((command & 0xe0) == 0x20) {
        const unsigned accepted = submit_gp0_polygon(packet + 1, count, ex_regs);
#ifdef PCSX_GP0_ROI_TRACE
        gp0_roi_trace_record_packet(words, packet + 1, count, ex_regs, accepted);
#endif
    } else {
        ParseTaglessPrimitive((u32 *)(packet + 1));
#ifdef PCSX_GP0_ROI_TRACE
        gp0_roi_trace_record_packet(words, packet + 1, count, ex_regs, 1);
#endif
    }
}

typedef struct RendererInitTask {
    int result;
} RendererInitTask;

static void backend_renderer_init(void *arg)
{
    RendererInitTask *task = (RendererInitTask *)arg;
    task->result = 0;

    if (!NativeRenderer_InitialiseRender((char *)"PCSX-ReARMed", 960, 544, 0))
        return;
    if (!NativeRenderer_InitialisePSX())
        return;

    VitaCtrNativeRenderer_SetAspectMode(VitaCtrNativeRenderer_GetAspectMode());
    VitaCtrNativeRenderer_CopyGuestVRAM(s_guest_vram, 0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    NativeRenderer_UpdateVRAM();
    task->result = 1;
}

static void backend_renderer_shutdown(void *arg)
{
    (void)arg;
#ifdef VITA_CTR_GPU_TRACE
    SysPrintf("pcsx-vita: teardown-trace GPU wait begin\n");
#endif
    /* The worker being idle does not mean GXM has finished consuming VAOs
     * and transfer storage. Wait before any of those objects are released. */
    glFinish();
#ifdef VITA_CTR_GPU_TRACE
    SysPrintf("pcsx-vita: teardown-trace renderer delete begin\n");
#endif
    NativeRenderer_Shutdown();
    /* Destroy plugin GL objects, flush their GXM work, then release vitaGL's
     * context and GXM resources before the guest CRT tears down its heap. */
    glFinish();
#ifdef VITA_CTR_GPU_TRACE
    SysPrintf("pcsx-vita: teardown-trace vitaGL shutdown begin\n");
#endif
    int shutdown_result = vglShutdown();
    if (shutdown_result < 0)
        SysPrintf("pcsx-vita: vitaGL shutdown FAIL (%d)\n", shutdown_result);
    else
        SysPrintf("pcsx-vita: vitaGL shutdown PASS (%d)\n", shutdown_result);
}

static int init_context(void)
{
    RendererInitTask task;

    if (s_context_ready)
        return 1;
    if (!s_guest_vram)
        return 0;
    g_nativeGpuModel = gpu_get_model();
    g_nativeVramHeight = gpu_get_vram_height();
    if (g_nativeVramHeight != GPU_VRAM_ROWS_CONSOLE &&
        g_nativeVramHeight != GPU_VRAM_ROWS_SYSTEM12)
        return 0;

#ifdef __vita__
    sceIoMkdir(APP_ROOT, 0777);
    sceIoMkdir(APP_ROOT "/shader_cache", 0777);
#endif

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        SysPrintf("pcsx-vita: SDL video init failed: %s\n", SDL_GetError());
        return 0;
    }
    if (!NativeGpu_InitBackend(960, 544)) {
        SysPrintf("pcsx-vita: High Octane GPU thread init failed\n");
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return 0;
    }

    memset(&task, 0, sizeof(task));
    NativeGpu_RunBackendTaskSync(backend_renderer_init, &task);
    if (!task.result) {
        SysPrintf("pcsx-vita: High Octane renderer init failed\n");
        /* vitaGL was initialized by the backend thread before renderer setup.
         * Even a partial renderer init must release that context before CRT
         * teardown reclaims the guest heap. The renderer shutdown tolerates
         * zero GL names and clears any objects created before the failure. */
        NativeGpu_RunBackendTaskSync(backend_renderer_shutdown, NULL);
        NativeGpu_ShutdownBackend();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return 0;
    }

    VitaCtrNativeGpu_SetDisplayEnv(0, 0, 320, 240, 0, 0, 320, 240, 0, 0);
    NativeGpu_BeginFrontendFrame();
    s_frame_started = 1;
    s_context_ready = 1;
    SysPrintf("pcsx-vita: GPU = CTR High Octane native, VRAM=%ux%u, 4:3 725x544 target\n",
        (unsigned)VRAM_WIDTH, g_nativeVramHeight);
    return 1;
}

static int command_words(const uint32_t *list, int count, int *cmd_out)
{
    int cmd;
    int len;
    int n;

    if (count <= 0)
        return 0;
    cmd = (int)(LE32TOH(list[0]) >> 24);
    *cmd_out = cmd;
    len = 1 + cmd_lengths[cmd];
    if (len > count)
        return 0;

    if (cmd >= 0x48 && cmd <= 0x4f) {
        for (n = 2; ; n++) {
            if (n + 1 >= count)
                return 0;
            if ((LE32TOH(list[n + 1]) & 0xf000f000u) == 0x50005000u)
                break;
        }
        len += n - 2;
    }
    else if (cmd >= 0x58 && cmd <= 0x5f) {
        for (n = 2; ; n++) {
            if (n * 2 >= count)
                return 0;
            if ((LE32TOH(list[n * 2]) & 0xf000f000u) == 0x50005000u)
                break;
        }
        len += (n - 2) * 2;
    }
    return len;
}

static void account_timing(const uint32_t *list, int cmd,
    uint32_t *cycle_sum, uint32_t *cycle_last)
{
    const int16_t *slist = (const int16_t *)list;
    int w, h;

    switch (cmd) {
    case 0x02:
        w = ((LE16TOH(slist[4]) & 0x3ff) + 0xf) & ~0xf;
        h = LE16TOH(slist[5]) & GPU_VRAM_Y_MASK;
        gput_sum(*cycle_sum, *cycle_last, gput_fill(w, h));
        break;
    case 0x20 ... 0x23: gput_sum(*cycle_sum, *cycle_last, gput_poly_base()); break;
    case 0x24 ... 0x27: gput_sum(*cycle_sum, *cycle_last, gput_poly_base_t()); break;
    case 0x28 ... 0x2b: gput_sum(*cycle_sum, *cycle_last, gput_quad_base()); break;
    case 0x2c ... 0x2f: gput_sum(*cycle_sum, *cycle_last, gput_quad_base_t()); break;
    case 0x30 ... 0x33: gput_sum(*cycle_sum, *cycle_last, gput_poly_base_g()); break;
    case 0x34 ... 0x37: gput_sum(*cycle_sum, *cycle_last, gput_poly_base_gt()); break;
    case 0x38 ... 0x3b: gput_sum(*cycle_sum, *cycle_last, gput_quad_base_g()); break;
    case 0x3c ... 0x3f: gput_sum(*cycle_sum, *cycle_last, gput_quad_base_gt()); break;
    case 0x40 ... 0x4f:
    case 0x50 ... 0x5f:
        gput_sum(*cycle_sum, *cycle_last, gput_line(0));
        break;
    case 0x60 ... 0x63:
        w = LE16TOH(slist[4]) & 0x3ff;
        h = LE16TOH(slist[5]) & GPU_VRAM_Y_MASK;
        gput_sum(*cycle_sum, *cycle_last, gput_sprite(w, h));
        break;
    case 0x64 ... 0x67:
        w = LE16TOH(slist[6]) & 0x3ff;
        h = LE16TOH(slist[7]) & GPU_VRAM_Y_MASK;
        gput_sum(*cycle_sum, *cycle_last, gput_sprite(w, h));
        break;
    case 0x68 ... 0x6b:
        gput_sum(*cycle_sum, *cycle_last, gput_sprite(1, 1));
        break;
    case 0x70 ... 0x77:
        gput_sum(*cycle_sum, *cycle_last, gput_sprite(8, 8));
        break;
    case 0x78 ... 0x7f:
        gput_sum(*cycle_sum, *cycle_last, gput_sprite(16, 16));
        break;
    default:
        break;
    }
}

int renderer_do_cmd_list(uint32_t *list, int count, uint32_t *ex_regs,
    int *cycles_sum, int *cycles_last, int *last_cmd)
{
    GL_ENTER();
    uint32_t cyc_sum = 0;
    uint32_t cyc = (uint32_t)*cycles_last;
    int pos = 0;
    int cmd = 0;

    while (pos < count) {
        int len = command_words(list + pos, count - pos, &cmd);
        uint32_t *packet = list + pos;

        if (len <= 0) {
            cmd = -1;
            break;
        }
        if (cmd >= 0x80 && cmd <= 0xdf)
            break;

        account_timing(packet, cmd, &cyc_sum, &cyc);

        if (cmd >= 0x24 && cmd <= 0x3f && (cmd & 4)) {
            const int tpage_index = 4 + ((cmd >> 4) & 1);
            const unsigned mask = gpu_get_model() == 1 ? 0x3fffu : 0x1fffu;
            ex_regs[1] = (ex_regs[1] & ~mask) |
                ((LE32TOH(packet[tpage_index]) >> 16) & mask);
            if (gpu_get_model() == 1)
                gpu.type1_status = (gpu.type1_status & ~0x1fffu) | (ex_regs[1] & 0x1fffu);
        }
        if (cmd >= 0xe0 && cmd <= 0xe7)
            ex_regs[cmd & 7] = LE32TOH(packet[0]);
        if (gpu_get_model() == 1 && cmd == 0xe1)
            gpu.type1_status = (gpu.type1_status & ~0x1fffu) | (ex_regs[1] & 0x1fffu);
        if (gpu_get_model() == 1 && cmd == 0xe6)
            gpu.type1_status = (gpu.type1_status & ~0x1800u) | ((ex_regs[6] & 3u) << 11);

        submit_gp0_packet(packet, len, ex_regs);
        pos += len;
    }

    *cycles_sum += (int)cyc_sum;
    *cycles_last = (int)cyc;
    *last_cmd = cmd;
    return pos;
}

int renderer_init(void)
{
    /* gpulib maps VRAM later from GPUrearmedCallbacks(). Keep GPUinit cheap and create the vitaGL backend once gpu.vram is available. */
    s_guest_vram = gpu.vram;
    return 0;
}

void renderer_finish(void)
{
    GL_ENTER();
#ifdef PCSX_GP0_ROI_TRACE
    gp0_roi_trace_dump();
#endif
    if (!s_context_ready)
        return;

#ifdef VITA_CTR_GPU_TRACE
    SysPrintf("pcsx-vita: teardown-trace worker sync begin frame_started=%d\n", s_frame_started);
#endif
    NativeGpu_SyncBackend();
#ifdef VITA_CTR_GPU_TRACE
    SysPrintf("pcsx-vita: teardown-trace worker sync done\n");
#endif
    NativeGpu_RunBackendTaskSync(backend_renderer_shutdown, NULL);
    NativeGpu_ShutdownBackend();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    s_context_ready = 0;
    s_frame_started = 0;
    s_guest_vram = NULL;
}

void renderer_sync_ecmds(uint32_t *ecmds)
{
    GL_ENTER();
    int i;
    if (!s_context_ready || !ecmds)
        return;
    for (i = 1; i <= 6; i++)
        submit_gp0_packet(&ecmds[i], 1, NULL);
}

void renderer_update_caches(int x, int y, int w, int h, int state_changed)
{
    GL_ENTER();
    VitaCtrVramTask task;
    (void)state_changed;
    if (!s_context_ready || !s_guest_vram || w <= 0 || h <= 0)
        return;
#ifdef PCSX_GP0_ROI_TRACE
    gp0_roi_trace_record_rect(GP0_ROI_TRACE_VRAM_UPDATE, x, y, w, h);
#endif

    /* A GP0 upload is ordered after any earlier draws even if the frontend
     * has not submitted its frame packet yet. Flush those splits before the
     * backend copies guest VRAM into the persistent GL texture, unless none
     * of them reads or writes the uploaded rect: then they can still be
     * drawn after it, and the frame stays one asynchronous packet. Such
     * uploads are queued with the packet instead, when there is room. One
     * they do touch ends the packet without ending the frame, and is queued
     * at the start of the next packet. This callback runs on the
     * GPU/frontend thread; backend tasks never call it. */
    if (NativeGpu_QueueUpload(s_guest_vram, x, y, w, h))
        return;
    if (NativeGpu_PendingSplitsTouch(x, y, w, h) && NativeGpu_SubmitPartialFrame() &&
        NativeGpu_QueueUpload(s_guest_vram, x, y, w, h))
        return;
    if (NativeGpu_PendingSplitsTouch(x, y, w, h))
        renderer_barrier();
    else
        NativeGpu_SyncBackend();

    memset(&task, 0, sizeof(task));
    task.x = x;
    task.y = y;
    task.w = w;
    task.h = h;
    NativeGpu_RunBackendTaskSync(backend_copy_guest, &task);
}

void renderer_flush_queues(void)
{
    /* Submission happens at vout_update. Keeping this non-blocking preserves the renderer thread overlap that CTR Native relies on. */
}

void renderer_barrier(void)
{
    GL_ENTER();
    if (!s_context_ready)
        return;
    if (NativeGpu_HasPendingSplits()) {
        NativeGpu_ForceSynchronousFrame();
        NativeGpu_FlushFrontendSplitsSync();
    }
    NativeGpu_SyncBackend();
}

/* The last rect read back from the renderer (before tile rounding), and
 * how many reads there were: the frontend's progress lines print them. */
int pcsx_vita_ctr_read_rect[4];
unsigned pcsx_vita_ctr_read_n;

void renderer_sync_vram_to_cpu(int x, int y, int w, int h)
{
    GL_ENTER();
    VitaCtrVramTask task;
    if (!s_context_ready || !s_guest_vram || w <= 0 || h <= 0)
        return;
    /* Where the renderer never drew, the guest VRAM already holds what the
     * CPU wrote: no flush, no wait for the renderer thread. */
    if (!NativeGpu_GuestVRAMStale(x, y, w, h))
        return;
    pcsx_vita_ctr_read_rect[0] = x;
    pcsx_vita_ctr_read_rect[1] = y;
    pcsx_vita_ctr_read_rect[2] = w;
    pcsx_vita_ctr_read_rect[3] = h;
    pcsx_vita_ctr_read_n++;
#ifndef __vita__
    if (getenv("PCSX_TRACE_READS")) {
        fprintf(stderr, "read-trace %d,%d %dx%d caller=%p\n", x, y, w, h, __builtin_return_address(0));
    }
#endif

    /* Read back whole 64x64 tiles, so later reads near this one find the
     * guest copy current (a few draws at boot otherwise keep the whole VRAM
     * stale, and Soul Calibur reads small rects there every frame). The
     * barrier applies queued uploads first: the renderer's copy is newest. */
    if (x + w <= VRAM_WIDTH && y + h <= VRAM_HEIGHT) {
        const int x1 = (x + w + 63) & ~63, y1 = (y + h + 63) & ~63;
        x &= ~63;
        y &= ~63;
        w = (x1 < VRAM_WIDTH ? x1 : VRAM_WIDTH) - x;
        h = (y1 < VRAM_HEIGHT ? y1 : VRAM_HEIGHT) - y;
    }
    renderer_barrier();
    memset(&task, 0, sizeof(task));
    task.x = x;
    task.y = y;
    task.w = w;
    task.h = h;
    NativeGpu_RunBackendTaskSync(backend_read_guest, &task);
    NativeGpu_GuestVRAMSynced(x, y, w, h);
}

void renderer_set_interlace(int enable, int is_odd)
{
    GL_ENTER();
    (void)enable;
    (void)is_odd;
}

void renderer_set_config(const struct rearmed_cbs *cbs)
{
    s_guest_vram = gpu.vram;
    if (!s_context_ready && !init_context()) {
        SysPrintf("pcsx-vita: deferred High Octane renderer init failed\n");
        return;
    }
    if (cbs->pl_set_gpu_caps)
        cbs->pl_set_gpu_caps(GPU_CAP_OWNS_DISPLAY);
    gNativeDitheringEnabled = cbs->dithering != 0;
}

void renderer_notify_screen_change(const struct psx_gpu_screen *screen)
{
    (void)screen;
}

int renderer_present_size(const struct psx_gpu *psx_gpu, int *width, int *height)
{
    *width = psx_gpu->screen.hres > 0 ? psx_gpu->screen.hres : psx_gpu->screen.w;
    /* Borderless (Screen Borders): the picture is the whole display, as
     * Rinnegatamante's f3e05d98 made it; otherwise the console's canvas. */
    *height = psx_gpu->state.screen_centering_type == C_BORDERLESS || psx_gpu->screen.vres <= 0
        ? psx_gpu->screen.h : psx_gpu->screen.vres;
    return *width > 0 && *height > 0;
}

int renderer_present_frame(struct psx_gpu *psx_gpu, int src_x, int src_y,
    int pic_x, int pic_y, int pic_w, int pic_h)
{
    int width;
    int height;

    if (!renderer_present_size(psx_gpu, &width, &height))
        return 0;
    return renderer_present_frame_rect(src_x, src_y, width, height, pic_x, pic_y, pic_w, pic_h,
        !!(psx_gpu->status & PSX_GPU_STATUS_RGB24));
}

/* The present of renderer_present_frame, from values taken when it was
 * requested: gpulib's GPU thread runs it in order with the commands. */
/* Whether the frame shows 3D for the GTE widescreen hook: it squeezed a
 * projection within the last 30 presents (half a second; a 30 fps game
 * shows each frame twice). Only reads the CPU's counter: with gpulib's GPU
 * thread this runs there. */
static int widescreen_3d_frame(void)
{
    static unsigned seen, presents_since = 1000;
    const unsigned ops = gte_widescreen_ops;
    if (ops != seen) {
        seen = ops;
        presents_since = 0;
    }
    else if (presents_since < 1000)
        presents_since++;
    return presents_since < 30;
}

int renderer_present_frame_rect(int src_x, int src_y, int width, int height,
    int pic_x, int pic_y, int pic_w, int pic_h, int rgb24)
{
    GL_ENTER();
    if (!s_context_ready)
        return 0;
#ifndef __vita__
    if (getenv("PCSX_TRACE_PRESENT"))
        fprintf(stderr, "present src %d,%d disp %dx%d pic %d,%d %dx%d\n", src_x, src_y, width, height, pic_x, pic_y, pic_w, pic_h);
#endif

    VitaCtrNativeGpu_SetDisplayEnv(src_x, src_y, width, height, pic_x, pic_y, pic_w, pic_h, rgb24,
        widescreen_3d_frame());
    NativeGpu_SetFrontendDisplayBlank(0);
    if (!s_frame_started) {
        NativeGpu_BeginFrontendFrame();
        s_frame_started = 1;
    }
    NativeGpu_SubmitFrontendFrame();
    NativeGpu_BeginFrontendFrame();
    s_frame_started = 1;
    return 1;
}

void renderer_present_blank(void)
{
    GL_ENTER();
    if (!s_context_ready)
        return;

    if (!s_frame_started) {
        NativeGpu_BeginFrontendFrame();
        s_frame_started = 1;
    }
    NativeGpu_SetFrontendDisplayBlank(1);
    NativeGpu_SubmitFrontendFrame();
    NativeGpu_BeginFrontendFrame();
    s_frame_started = 1;
}
