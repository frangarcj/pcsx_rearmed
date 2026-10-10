/* Derived from REDRIVER2/PsyCross MIT source: externals/PsyCross/src/render/PsyX_render.cpp See THIRD_PARTY_NOTICES.md for copyright and license details. */

#include <SDL3/SDL.h>
#include <macros.h>
#include "../../libpcsxcore/system.h"
#include "platform/native_vita_path.h"
#include "platform/native_renderer_types.h"
#include "namespace_Gamepad.h"

#ifndef NATIVE_GHOST_OVERLAY_TOUCH_FRONT_LEFT
#define NATIVE_GHOST_OVERLAY_TOUCH_FRONT_LEFT  (1u << 19)
#define NATIVE_GHOST_OVERLAY_TOUCH_FRONT_RIGHT (1u << 20)
#define NATIVE_GHOST_OVERLAY_TOUCH_REAR_LEFT   (1u << 21)
#define NATIVE_GHOST_OVERLAY_TOUCH_REAR_RIGHT  (1u << 22)
#endif
extern b32 NativeGhostInput_GetReplayOverlayState(u32 *buttonsHeld, u8 *stickLX, u8 *stickLY, u8 *stickRX, u8 *stickRY);

#include "platform/native_assets.h"
#include "platform/native_gpu.h"
#include "platform/native_adhoc.h"
#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#elif defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>
#elif defined(PCSX_NATIVE_VITA_PATH)
#include "platform/mac_vita_compat.h"
#else
#include "platform/native_glad.h"
#endif
#include "platform/native_log.h"
#include "platform/native_renderer.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
#include <png.h>
#include "vita_rgb24.h"
#else
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "../externals/SDL/src/video/stb_image.h"
#endif

#ifdef _WIN32
#include "platform/native_win32.h"

__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

#endif // def WIN32

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
#define VRAM_FORMAT          GL_RGBA
#define VRAM_INTERNAL_FORMAT GL_RGBA
#else
#define VRAM_FORMAT          GL_RG
// NOTE(penta3): VRAM holds packed 16-bit PSX pixels as two bytes (R=low, G=high), uploaded as GL_RG + GL_UNSIGNED_BYTE. RG8 is the faithful storage: 2 bytes/texel = real PS1's 1MB VRAM, vs RG32F's 8 bytes/texel (4MB). With NEAREST sampling RG8 returns byte/255 exactly, identical to what the shader got from RG32F, so CLUT/texture-page reconstruction is unchanged.
#define VRAM_INTERNAL_FORMAT GL_RG8
#endif

#define NATIVE_RENDER_TARGET_TYPE GL_UNSIGNED_BYTE

extern SDL_Window *g_window;

#define NATIVE_RENDERER_LOG(fmt, ...)   Platform_Log("[CTR Renderer] " fmt, __VA_ARGS__)
#define NATIVE_RENDERER_ERROR(fmt, ...) Platform_LogError("[CTR Renderer] [%s] - " fmt, __func__, __VA_ARGS__)

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
#define MAX_NUM_VERTEX_BUFFERS (1)
#else
#define MAX_NUM_VERTEX_BUFFERS (2)
#endif
#define PSX_SCREEN_ASPECT               (240.0f / 320.0f) // PSX screen is mapped always to this aspect

global_variable BlendMode s_previousBlendMode = BM_NONE;
global_variable int s_previousMixedSTPBlend = 0;
global_variable int s_previousDepthMode = 0;
global_variable int s_previousDepthWrite = 1;
global_variable int s_previousStencilMode = 0;
global_variable int s_previousScissorState = 0;
global_variable int s_previousScissorRectValid = 0;
global_variable int s_previousScissorX = 0;
global_variable int s_previousScissorY = 0;
global_variable int s_previousScissorW = 0;
global_variable int s_previousScissorH = 0;
global_variable int s_previousOffscreenState = 0;
global_variable RECT16 s_previousOffscreen = {0, 0, 0, 0};

global_variable ShaderID s_previousShader = (ShaderID)-1;

internal void NativeRenderer_InvalidateScissorRectCache(void)
{
	s_previousScissorRectValid = 0;
}

internal void NativeRenderer_SetScissorRectCached(int x, int y, int width, int height)
{
	if (s_previousScissorRectValid && s_previousScissorX == x && s_previousScissorY == y && s_previousScissorW == width &&
	    s_previousScissorH == height)
	{
		return;
	}

	glScissor(x, y, width, height);
	s_previousScissorRectValid = 1;
	s_previousScissorX = x;
	s_previousScissorY = y;
	s_previousScissorW = width;
	s_previousScissorH = height;
}

global_variable TextureID s_rgLutTexture = (TextureID)-1;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
global_variable TextureID s_presentLutTexture = (TextureID)-1;
#endif
// NOTE(penta3): Single persistent VRAM texture, matching real PS1's single 1MB VRAM. PS1 page-flips two windows *inside* one VRAM; it never keeps two full copies. The old double buffer existed only to orphan the texture on the per-frame full re-upload, which no longer happens (we upload dirty rects).
#define NATIVE_VRAM_DIRTY_RECT_CAP 128
#define NATIVE_VRAM_TILE_SIZE      8
#define NATIVE_VRAM_TILE_COLS      (VRAM_WIDTH / NATIVE_VRAM_TILE_SIZE)
#define NATIVE_VRAM_TILE_ROWS      (VRAM_MAX_HEIGHT / NATIVE_VRAM_TILE_SIZE)
#define NATIVE_VRAM_TILE_COUNT     (NATIVE_VRAM_TILE_COLS * NATIVE_VRAM_TILE_ROWS)
#define NATIVE_VRAM_TILE_WORDS     (NATIVE_VRAM_TILE_COUNT / 32)

// NOTE(aalhendi): Native splits PS1 VRAM between a CPU mirror and one packed GPU texture. CPU writes are uploaded before GPU reads; GPU-newer tiles are resolved into the CPU mirror only when game code reads those VRAM regions.
struct NativeVramState
{
	TextureID texture;
	u16 cpuPixels[VRAM_WIDTH * VRAM_MAX_HEIGHT];
	RECT16 cpuDirtyRects[NATIVE_VRAM_DIRTY_RECT_CAP];
	u32 gpuNewerTiles[NATIVE_VRAM_TILE_WORDS];
	s32 cpuDirtyRectCount;
};

global_variable struct NativeVramState s_vram;

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
#define NATIVE_VITA_VRAM_TRANSFER_BYTES (VRAM_WIDTH * VRAM_MAX_HEIGHT * 4u)
global_variable u8 *s_vitaVramTransferPixels;
/* Set by the Vita frontend; runs on this thread right after each present. */
void (*pcsx_vita_swap_cb)(void);
/* GPU profiler labels, set by a frontend built with VITA_GPUPROF: kind 0
 * opens a pass, 1 closes it, 2 names the draws that follow (NULL clears),
 * the handle argument is unused (0). */
void (*pcsx_vita_gpu_label_cb)(int kind, const char *name, unsigned handle);

#define NATIVE_P4_CACHE_CAPACITY 320
#define NATIVE_P4_HASH_CAPACITY 1024
#define NATIVE_P4_PALETTE_BYTES  (16 * 4)
#define NATIVE_P4_INDEX_BYTES    (TPAGE_WIDTH * TPAGE_HEIGHT / 2)
#define NATIVE_P4_UPLOAD_BYTES   (NATIVE_P4_PALETTE_BYTES + NATIVE_P4_INDEX_BYTES)
#define NATIVE_PALETTE_CLUT_COUNT (64 * VRAM_MAX_HEIGHT)

/* A cached 4-bit texture: one (page, CLUT, tint), built from the CPU's VRAM
 * copy. It is current while the versions it was built from are those of
 * its page and CLUT (s_p4PageVersion, s_p4ClutVersion). */
struct NativeP4CacheEntry
{
	TextureID texture;
	s16 page;
	u16 clut;
	u8 superTurboTint;
	u32 lastUse;
	u32 lastUseFrame;
	u32 builtPageVersion; /* 0: not built */
	u32 builtClutVersion;
#if defined(__vita__)
	void *palette; /* GXM palette (16 ABGR words) of this entry */
#endif
	b32 occupied;
};

/* Bumped by every VRAM write over a 64x256 page or a 16-texel CLUT; never 0. */
global_variable u32 s_p4VersionSerial = 1;
global_variable u32 s_p4PageVersion[(1024 / TPAGE_HEIGHT) * 16];
global_variable u32 s_p4ClutVersion[NATIVE_PALETTE_CLUT_COUNT];
#if defined(__vita__)
/* A page's indices in GXM P4 layout (the PSX's: 4-bit texels, first in the
 * low nibble), shared by every entry of that page. A new version goes to a
 * new buffer: the old one is freed once the GPU is done with it. */
#define NATIVE_P4_PAGE_BYTES (TPAGE_WIDTH / 2 * TPAGE_HEIGHT)
global_variable struct
{
	void *data;
	u32 version;
} s_p4PageData[(1024 / TPAGE_HEIGHT) * 16];
#endif

global_variable struct NativeP4CacheEntry s_p4Cache[NATIVE_P4_CACHE_CAPACITY];
global_variable u32 s_p4CacheUseCounter;
#if !defined(__vita__)
/* A cached texture as GL_PALETTE4_RGBA8_OES data, for the macOS harness. */
global_variable u8 s_p4UploadData[NATIVE_P4_UPLOAD_BYTES];
#endif
global_variable u32 s_p4FrameSerial = 1;
global_variable s32 s_p4LastLookupIndex = -1;
global_variable u32 s_p4LastLookupKey = 0xffffffffu;

struct NativeP4HashEntry
{
	u32 key;
	s16 cacheIndex;
};

global_variable struct NativeP4HashEntry s_p4Hash[NATIVE_P4_HASH_CAPACITY];
global_variable u32 s_paletteRowGeneration[VRAM_MAX_HEIGHT];
global_variable u32 s_paletteCacheGeneration[2][NATIVE_PALETTE_CLUT_COUNT];
global_variable u8 s_paletteCacheProperties[2][NATIVE_PALETTE_CLUT_COUNT];

internal u32 NativeRenderer_P4HashSlot(u32 key)
{
	return (key * 2654435761u) & (NATIVE_P4_HASH_CAPACITY - 1);
}

internal u32 NativeRenderer_P4Key(int page, int clut, int superTurboTint)
{
	return ((u32)(page & 0x3f) << 17) | ((u32)(superTurboTint != 0) << 16) | (u16)clut;
}

internal void NativeRenderer_InsertP4Hash(u32 key, int cacheIndex)
{
	u32 slot = NativeRenderer_P4HashSlot(key);
	for (u32 probe = 0; probe < NATIVE_P4_HASH_CAPACITY; probe++)
	{
		struct NativeP4HashEntry *hashEntry = &s_p4Hash[slot];
		if (hashEntry->key == 0xffffffffu || hashEntry->key == key)
		{
			hashEntry->key = key;
			hashEntry->cacheIndex = (s16)cacheIndex;
			return;
		}
		slot = (slot + 1) & (NATIVE_P4_HASH_CAPACITY - 1);
	}
}

internal void NativeRenderer_RebuildP4Hash(void)
{
	memset(s_p4Hash, 0xff, sizeof(s_p4Hash));
	for (int cacheIndex = 0; cacheIndex < NATIVE_P4_CACHE_CAPACITY; cacheIndex++)
	{
		const struct NativeP4CacheEntry *entry = &s_p4Cache[cacheIndex];
		if (entry->occupied)
		{
			const u32 key = NativeRenderer_P4Key(entry->page, entry->clut, entry->superTurboTint);
			NativeRenderer_InsertP4Hash(key, cacheIndex);
		}
	}
}

internal int NativeRenderer_FindP4Hash(u32 key)
{
	u32 slot = NativeRenderer_P4HashSlot(key);
	for (u32 probe = 0; probe < NATIVE_P4_HASH_CAPACITY; probe++)
	{
		const struct NativeP4HashEntry *hashEntry = &s_p4Hash[slot];
		if (hashEntry->key == 0xffffffffu)
		{
			return -1;
		}
		if (hashEntry->key == key)
		{
			const int cacheIndex = hashEntry->cacheIndex;
			if (cacheIndex >= 0 && cacheIndex < NATIVE_P4_CACHE_CAPACITY)
			{
				const struct NativeP4CacheEntry *entry = &s_p4Cache[cacheIndex];
				if (entry->occupied && NativeRenderer_P4Key(entry->page, entry->clut, entry->superTurboTint) == key)
				{
					return cacheIndex;
				}
			}
		}
		slot = (slot + 1) & (NATIVE_P4_HASH_CAPACITY - 1);
	}
	return -1;
}
#endif

struct NativeRenderTarget
{
	TextureID texture;
	GLuint framebuffer;
	GLuint stencilBuffer;
	s32 width;
	s32 height;
	s32 logicalWidth;
	s32 logicalHeight;
	/* Allocated size; width x height is the part in use (bottom-left). */
	s32 allocWidth;
	s32 allocHeight;
	s32 growable; /* allocated in 32-pixel steps, used in part */
	s32 multisampled; /* the display's MSAA (host-resolution targets) or none */
};

global_variable struct NativeRenderTarget s_mainRenderTarget;
global_variable struct NativeRenderTarget s_offscreenRenderTarget;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
/* A target owns one absolute VRAM rect. GP0 state may change after binding;
 * never repack that texture using a later draw environment. */
global_variable RECT16 s_mainDrawRect;
global_variable int s_mainLoaded;
global_variable int s_mainDirty;
global_variable int s_offscreenLoaded;
global_variable int s_offscreenDirty;
/* The main target of the previous scene. A double-buffered game displays a
 * page one frame after drawing it, so presentation needs the target that
 * drew that page. It is valid until a VRAM write overlaps its rect. */
global_variable struct NativeRenderTarget s_prevMainRenderTarget;
/* VRAM rects drawn since they were last packed (see
 * NativeRenderer_NoteTargetDraw). */
#define NATIVE_DRAWN_RECTS 32
global_variable struct
{
	s32 x0, y0, x1, y1; /* x1/y1 exclusive */
} s_drawnRects[NATIVE_DRAWN_RECTS];
global_variable int s_drawnRectCount;
/* The frame each 64x256 texture page last had tiles marked GPU-newer (0:
 * none since it was found clean), and the stale ones to read back. */
#define NATIVE_STALE_P4_FRAMES 30
global_variable u32 s_gpuNewerPageFrame[(1024 / TPAGE_HEIGHT) * 16];
global_variable RECT16 s_staleP4Sync;
/* Spare targets by size. The main target alternates between a few sizes (the
 * display page at host resolution, small areas at 1x) and resizing it in
 * place reallocated its store and GXM render target each time: a target of
 * another size is swapped in from here instead. */
#define NATIVE_RT_POOL_SIZE 8
global_variable struct NativeRenderTarget s_rtPool[NATIVE_RT_POOL_SIZE];
global_variable u32 s_rtPoolLastUse[NATIVE_RT_POOL_SIZE];
global_variable u32 s_rtPoolTick;
global_variable RECT16 s_prevMainRect;
global_variable int s_prevMainValid;
#endif

#if defined(__vita__) && defined(VITA_CTR_GPU_TRACE)
global_variable RECT16 s_traceBeginClip;
global_variable u32 s_traceScene;

/* Keep complete, distinct geometry records without logging every buffer swap.
 * This records state only; it does not change clipping or texture contents. */
internal void NativeRenderer_TraceFrameGeometry(int storing, int x, int y, int w, int h)
{
	const DRAWENV *draw = NativeGpu_GetRenderDrawEnv();
	const DISPENV *disp = NativeGpu_GetRenderDispEnv();
	const int geometry[] = {
		storing, draw->clip.x, draw->clip.y, draw->clip.w, draw->clip.h,
		draw->ofs[0], draw->ofs[1], draw->dfe, draw->isbg,
		disp->disp.x, disp->disp.y, disp->disp.w, disp->disp.h,
		s_mainRenderTarget.width, s_mainRenderTarget.height,
		s_mainRenderTarget.logicalWidth, s_mainRenderTarget.logicalHeight,
		s_traceBeginClip.x, s_traceBeginClip.y, s_traceBeginClip.w, s_traceBeginClip.h,
		x, y, w, h, s_previousOffscreenState
	};
	static int seen[256][26];
	static unsigned seenCount;
	for (unsigned i = 0; i < seenCount; ++i)
		if (memcmp(seen[i], geometry, sizeof(geometry)) == 0)
			return;
	if (seenCount == 256)
		return;
	memcpy(seen[seenCount++], geometry, sizeof(geometry));
	SysPrintf("pcsx-vita: frame-geometry scene=%u stage=%s draw=%d,%d,%d,%d ofs=%d,%d dfe=%d bg=%d disp=%d,%d,%d,%d target=%dx%d logical=%dx%d begin=%d,%d,%d,%d pack=%d,%d,%d,%d offscreen=%d\n",
		s_traceScene, storing ? "store" : "begin",
		draw->clip.x, draw->clip.y, draw->clip.w, draw->clip.h,
		draw->ofs[0], draw->ofs[1], draw->dfe, draw->isbg,
		disp->disp.x, disp->disp.y, disp->disp.w, disp->disp.h,
		s_mainRenderTarget.width, s_mainRenderTarget.height,
		s_mainRenderTarget.logicalWidth, s_mainRenderTarget.logicalHeight,
		s_traceBeginClip.x, s_traceBeginClip.y, s_traceBeginClip.w, s_traceBeginClip.h,
		x, y, w, h, s_previousOffscreenState);
}
#endif

global_variable TextureID s_whiteTexture = (TextureID)-1;
global_variable TextureID s_lastBoundTexture = (TextureID)-1;

global_variable TextureID s_ghostReplayControllerTexture;
global_variable TextureID s_ghostReplayHighlightTexture;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
global_variable TextureID s_ghostReplayShoulderTexture[2];
#endif
global_variable int s_ghostReplayControllerWidth;
global_variable int s_ghostReplayControllerHeight;
global_variable b32 s_ghostReplayOverlayLoadAttempted;

TextureID NativeRenderer_GetVRAMTexture(void)
{
	return s_vram.texture;
}

TextureID NativeRenderer_GetWhiteTexture(void)
{
	return s_whiteTexture;
}

TextureID NativeRenderer_CreateStreamingTexture(int width, int height)
{
	TextureID texture = 0;

	if ((width <= 0) || (height <= 0))
	{
		return 0;
	}

	glGenTextures(1, &texture);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	s_lastBoundTexture = (TextureID)-1;
	return texture;
}

void NativeRenderer_UpdateStreamingTexture(TextureID texture, int width, int height, const u8 *rgbaPixels)
{
	if ((texture == 0) || (width <= 0) || (height <= 0) || (rgbaPixels == NULL))
	{
		return;
	}

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);
	s_lastBoundTexture = (TextureID)-1;
}

void NativeRenderer_DestroyStreamingTexture(TextureID texture)
{
	if (texture == 0)
	{
		return;
	}

	if (s_lastBoundTexture == texture)
	{
		s_lastBoundTexture = (TextureID)-1;
	}
	glDeleteTextures(1, &texture);
}

int g_windowWidth = 0;
int g_windowHeight = 0;
#ifndef __vita__
extern int gNativeAntiAliasingEnabled;
extern int gNativeDitheringEnabled;
#endif

global_variable int s_presentAspectW = 4;
global_variable int s_presentAspectH = 3;
global_variable SDL_Rect s_presentViewport = {0, 0, 0, 0};

int g_dbg_wireframeMode = 0;
int g_dbg_texturelessMode = 0;

int g_cfg_bilinearFiltering = 0;

// NOTE(aalhendi): Pack native RGBA render targets into the persistent RG8 VRAM texture on the GPU instead of a GPU-to-CPU-to-GPU round trip.
global_variable GLuint s_packShader = 0;
global_variable GLint s_packFlipYLoc = -1;
global_variable GLint s_packUvScaleLoc = -1;
global_variable GLuint s_presentVramShader = 0;
global_variable GLint s_presentVramSourceRectLoc = -1;
global_variable GLuint s_presentRgbaShader = 0;
global_variable GLint s_presentRgbaFlipYLoc = -1;
global_variable GLint s_presentRgbaUvScaleLoc = -1;
#ifndef __vita__
global_variable GLint s_presentRgbaTexelSizeLoc = -1;
global_variable GLint s_presentRgbaFxaaLoc = -1;
#endif
global_variable GLuint s_vramQuadVAO = 0;
global_variable GLuint s_vramQuadVBO = 0;

#ifndef __vita__
internal int NativeRenderer_InitialiseGLContext(char *windowName, int fullscreen);
#endif
internal int NativeRenderer_InitialiseGLExt(void);
internal void NativeRenderer_DestroyTexture(TextureID texture);
internal void NativeRenderer_SetScissorState(int enable);
internal void NativeRenderer_EnableDepth(int enable);
internal void NativeRenderer_SetViewPort(int x, int y, int width, int height);
internal void NativeRenderer_SetPresentationAspect(int width, int height);
internal void NativeRenderer_UpdatePresentationViewport(void);
internal void NativeRenderer_ClearPresentationBars(void);
internal void NativeRenderer_SetWireframe(int enable);
internal void NativeRenderer_InitRenderTarget(struct NativeRenderTarget *target);
internal void NativeRenderer_DestroyRenderTarget(struct NativeRenderTarget *target);
internal void NativeRenderer_EnsureRenderTarget(struct NativeRenderTarget *target, int width, int height);
internal void NativeRenderer_ResizeRenderTarget(struct NativeRenderTarget *target, int width, int height);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
internal void NativeRenderer_EnsureTargetSize(struct NativeRenderTarget *target, int width, int height, int grow, int multisampled);
#endif
internal void NativeRenderer_BindMainRenderTarget(void);
internal void NativeRenderer_BindMainRect(const RECT16 *rect);
internal void NativeRenderer_CommitTargets(void);
internal void NativeRenderer_FlushMainToVRAM(void);
internal void NativeRenderer_FlushOffscreenToVRAM(void);
internal int NativeRenderer_RectEquals(const RECT16 *a, const RECT16 *b);
internal void NativeRenderer_DrawVRAMRegion(int x, int y, int width, int height);
internal void NativeRenderer_LoadRenderTargetFromVRAM(struct NativeRenderTarget *target, int x, int y, int logicalWidth, int logicalHeight);
internal void NativeRenderer_DestroyPSXShaders(void);

global_variable GLuint s_glVertexArray[MAX_NUM_VERTEX_BUFFERS];
global_variable GLuint s_glVertexBuffer[MAX_NUM_VERTEX_BUFFERS];
global_variable int s_curVertexBuffer = 0;
global_variable int s_boundVertexBuffer = -1;

global_variable GLuint s_glVramFramebuffer;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
global_variable TextureID s_vramUploadTexture;
global_variable ShaderID s_vramUploadShader;
global_variable GLint s_vramUploadRectLoc;
global_variable GLint s_vramUploadSourceRectLoc;
#endif

#ifndef __vita__
internal int NativeRenderer_InitialiseGLContext(char *windowName, int fullscreen)
{
	SDL_WindowFlags windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
#ifndef __vita__
	/* PCSX_GL_LOWDPI=1: the Vita's 960x544 back buffer on a Retina Mac. */
	if (!getenv("PCSX_GL_LOWDPI"))
		windowFlags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif

	if (fullscreen)
	{
		windowFlags |= SDL_WINDOW_FULLSCREEN;
	}

	g_window = SDL_CreateWindow(windowName, g_windowWidth, g_windowHeight, windowFlags);

	if (g_window == NULL)
	{
		NATIVE_RENDERER_ERROR("%s\n", "Failed to initialise SDL window!");
		return 0;
	}

	int major_version = 3;
#ifdef __EMSCRIPTEN__
	int minor_version = 0;
	int profile = SDL_GL_CONTEXT_PROFILE_ES;
#else
	int minor_version = 3;
	int profile = SDL_GL_CONTEXT_PROFILE_CORE;
#endif

	// find best OpenGL version
	do
	{
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major_version);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor_version);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);

		if (SDL_GL_CreateContext(g_window))
		{
			break;
		}

		minor_version--;

	} while (minor_version >= 0);

	if (minor_version == -1)
	{
		NATIVE_RENDERER_ERROR("%s\n", "Failed to initialise - OpenGL 3.x is not supported. Please update video drivers.");
		return 0;
	}

	return 1;
}
#endif

internal int NativeRenderer_InitialiseGLExt(void)
{
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH) && !defined(__EMSCRIPTEN__)
	GLenum err = gladLoadGL();

	if (err == 0)
	{
		return 0;
	}
#endif

	const char *rend = (const char *)glGetString(GL_RENDERER);
	const char *vendor = (const char *)glGetString(GL_VENDOR);
	NATIVE_RENDERER_LOG("*Video adapter: %s by %s\n", rend, vendor);

	const char *versionStr = (const char *)glGetString(GL_VERSION);
	NATIVE_RENDERER_LOG("*OpenGL version: %s\n", versionStr);

	const char *glslVersionStr = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
	NATIVE_RENDERER_LOG("*GLSL version: %s\n", glslVersionStr);
	return 1;
}

int NativeRenderer_InitialiseRender(char *windowName, int width, int height, int fullscreen)
{
	g_windowWidth = width;
	g_windowHeight = height;
#ifdef __vita__
	(void)windowName;
	(void)fullscreen;
	/* NativeGpu_InitBackend owns the VitaGL context on the dedicated renderer thread. Keep renderer object/shader setup on that same thread. */
	NativeRenderer_SetPresentationAspect(4, 3);
#else
	NativeRenderer_SetPresentationAspect(width, height);

	// Due to debugging in fullscreen
	SDL_SetHint(SDL_HINT_WINDOW_ALLOW_TOPMOST, "0");
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);

	if (!NativeRenderer_InitialiseGLContext(windowName, fullscreen))
	{
		NATIVE_RENDERER_ERROR("%s\n", "Failed to Initialise GL Context!");
		return 0;
	}
	SDL_GetWindowSizeInPixels(g_window, &g_windowWidth, &g_windowHeight);
#endif

	if (!NativeRenderer_InitialiseGLExt())
	{
		NATIVE_RENDERER_ERROR("%s\n", "Failed to Intialise GL extensions");
		return 0;
	}

	return 1;
}

void NativeRenderer_Shutdown(void)
{
#if defined(__vita__) && defined(VITA_CTR_GPU_TRACE)
	SysPrintf("pcsx-vita: teardown-trace delete vertex storage\n");
#endif
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	/* vglBufferData borrows each batch from vitaGL's scratch pool. It does
	 * not transfer ownership to the VBO, but glDeleteBuffers otherwise frees
	 * its current pointer as an allocation. Detach the borrowed slices first;
	 * the separately allocated quad VBO still follows normal GL ownership. */
	for (int i = 0; i < MAX_NUM_VERTEX_BUFFERS; i++)
	{
		if (s_glVertexBuffer[i] != 0)
		{
			glBindBuffer(GL_ARRAY_BUFFER, s_glVertexBuffer[i]);
			vglBufferData(GL_ARRAY_BUFFER, NULL);
		}
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);
#endif
	glDeleteVertexArrays(MAX_NUM_VERTEX_BUFFERS, s_glVertexArray);
	glDeleteBuffers(MAX_NUM_VERTEX_BUFFERS, s_glVertexBuffer);

#if defined(__vita__) && defined(VITA_CTR_GPU_TRACE)
	SysPrintf("pcsx-vita: teardown-trace delete render targets\n");
#endif
	NativeRenderer_DestroyRenderTarget(&s_mainRenderTarget);
	NativeRenderer_DestroyRenderTarget(&s_offscreenRenderTarget);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	NativeRenderer_DestroyRenderTarget(&s_prevMainRenderTarget);
	for (int i = 0; i < NATIVE_RT_POOL_SIZE; i++)
	{
		if (s_rtPool[i].framebuffer != 0)
			NativeRenderer_DestroyRenderTarget(&s_rtPool[i]);
		s_rtPoolLastUse[i] = 0;
	}
#endif
	glDeleteFramebuffers(1, &s_glVramFramebuffer);

	NativeRenderer_DestroyTexture(s_vram.texture);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	NativeRenderer_DestroyTexture(s_vramUploadTexture);
	if (s_vramUploadShader != 0)
	{
		glDeleteProgram(s_vramUploadShader);
	}
#endif

	NativeRenderer_DestroyTexture(s_whiteTexture);
	NativeRenderer_DestroyTexture(s_rgLutTexture);
	NativeRenderer_DestroyTexture(s_ghostReplayControllerTexture);
	NativeRenderer_DestroyTexture(s_ghostReplayHighlightTexture);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	NativeRenderer_DestroyTexture(s_ghostReplayShoulderTexture[0]);
	NativeRenderer_DestroyTexture(s_ghostReplayShoulderTexture[1]);
#endif
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	#ifdef VITA_CTR_GPU_TRACE
	SysPrintf("pcsx-vita: teardown-trace delete P4 cache\n");
	#endif
	for (int cacheIndex = 0; cacheIndex < NATIVE_P4_CACHE_CAPACITY; cacheIndex++)
	{
		struct NativeP4CacheEntry *entry = &s_p4Cache[cacheIndex];
		if (entry->texture != 0)
		{
#if defined(__vita__)
			/* The texture's indices are the page's buffer, freed below. */
			glBindTexture(GL_TEXTURE_2D, entry->texture);
			vglOverloadTexDataPointer(GL_TEXTURE_2D, NULL);
#endif
			NativeRenderer_DestroyTexture(entry->texture);
		}
#if defined(__vita__)
		vglLazyFree(entry->palette);
#endif
	}
#if defined(__vita__)
	for (int page = 0; page < (int)SDL_arraysize(s_p4PageData); page++)
		vglLazyFree(s_p4PageData[page].data);
	SDL_memset(s_p4PageData, 0, sizeof(s_p4PageData));
#endif
	NativeRenderer_DestroyTexture(s_presentLutTexture);
#endif
#if defined(__vita__) && defined(VITA_CTR_GPU_TRACE)
	SysPrintf("pcsx-vita: teardown-trace delete shaders\n");
#endif
	NativeRenderer_DestroyPSXShaders();
	if (s_packShader != 0)
	{
		glDeleteProgram(s_packShader);
	}
	if (s_presentVramShader != 0)
	{
		glDeleteProgram(s_presentVramShader);
	}
	if (s_presentRgbaShader != 0)
	{
		glDeleteProgram(s_presentRgbaShader);
	}
	glDeleteVertexArrays(1, &s_vramQuadVAO);
	glDeleteBuffers(1, &s_vramQuadVBO);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	#ifdef VITA_CTR_GPU_TRACE
	SysPrintf("pcsx-vita: teardown-trace free transfer storage\n");
	#endif
	if (s_vitaVramTransferPixels != NULL)
	{
		vglFree(s_vitaVramTransferPixels);
		s_vitaVramTransferPixels = NULL;
	}
#endif
#if defined(__vita__) && defined(VITA_CTR_GPU_TRACE)
	SysPrintf("pcsx-vita: teardown-trace renderer delete done\n");
#endif
}

#if defined(CTR_INTERNAL)
#endif

void NativeRenderer_UpdateSwapIntervalState(int swapInterval)
{
	SDL_GL_SetSwapInterval(swapInterval);
}

void NativeRenderer_BeginScene(void)
{

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (++s_p4FrameSerial == 0)
		s_p4FrameSerial = 1;
	NativeRenderer_RebuildP4Hash();
#endif
	s_lastBoundTexture = 0;

	NativeRenderer_UpdatePresentationViewport();
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	s_mainLoaded = s_mainDirty = s_offscreenLoaded = s_offscreenDirty = 0;
	s_previousOffscreenState = 0;
#endif
	NativeRenderer_BindMainRenderTarget();
#if defined(__vita__) && defined(VITA_CTR_GPU_TRACE)
	s_traceBeginClip = NativeGpu_GetRenderDrawEnv()->clip;
	++s_traceScene;
	NativeRenderer_TraceFrameGeometry(0, 0, 0, 0, 0);
#endif
	NativeRenderer_SetDepthState(0, 1);

	NativeRenderer_UpdateVRAM();
	if (!NativeGpu_GetRenderDrawEnv()->isbg)
	{
		NativeRenderer_LoadRenderTargetFromVRAM(&s_mainRenderTarget, NativeGpu_GetRenderDrawEnv()->clip.x, NativeGpu_GetRenderDrawEnv()->clip.y,
		                                        s_mainRenderTarget.logicalWidth, s_mainRenderTarget.logicalHeight);
	}
	else
	{
		const GLboolean previousScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
		glDisable(GL_SCISSOR_TEST);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
		glClear(GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#else
		glClear(GL_STENCIL_BUFFER_BIT);
#endif
		if (previousScissorEnabled)
		{
			glEnable(GL_SCISSOR_TEST);
		}
	}
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	s_mainLoaded = 1;
#endif
	NativeRenderer_SetViewPort(0, 0, s_mainRenderTarget.width, s_mainRenderTarget.height);

	if (g_dbg_wireframeMode)
	{
		NativeRenderer_SetWireframe(1);

		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
}

void NativeRenderer_EndScene(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	NativeRenderer_CommitTargets();
	s_previousOffscreenState = 0;
	if (s_mainRenderTarget.framebuffer)
		glBindFramebuffer(GL_FRAMEBUFFER, s_mainRenderTarget.framebuffer);
#else
	if (s_previousOffscreenState)
	{
		NativeRenderer_SetOffscreenState(&s_previousOffscreen, 0);
	}
#endif

	if (g_dbg_wireframeMode)
	{
		NativeRenderer_SetWireframe(0);
	}

	glBindVertexArray(0);
}

// ----------------------------------------------------------------------------------------

global_variable u8 rgLUT[LUT_WIDTH * LUT_HEIGHT * sizeof(u32)];

internal int NativeRenderer_IntAbs(int value)
{
	return value < 0 ? -value : value;
}

internal int NativeRenderer_GCD(int a, int b)
{
	a = NativeRenderer_IntAbs(a);
	b = NativeRenderer_IntAbs(b);

	while (b != 0)
	{
		int t = a % b;
		a = b;
		b = t;
	}

	return a;
}

internal void NativeRenderer_SetPresentationAspect(int width, int height)
{
	const int divisor = NativeRenderer_GCD(width, height);

	if ((width <= 0) || (height <= 0) || (divisor <= 0))
	{
		return;
	}

	s_presentAspectW = width / divisor;
	s_presentAspectH = height / divisor;
}

internal void NativeRenderer_UpdatePresentationViewport(void)
{
	int viewportW;
	int viewportH;

	if ((g_windowWidth <= 0) || (g_windowHeight <= 0) || (s_presentAspectW <= 0) || (s_presentAspectH <= 0))
	{
		s_presentViewport.x = 0;
		s_presentViewport.y = 0;
		s_presentViewport.w = g_windowWidth;
		s_presentViewport.h = g_windowHeight;
		return;
	}

	viewportW = g_windowWidth;
	viewportH = (viewportW * s_presentAspectH) / s_presentAspectW;

	if (viewportH > g_windowHeight)
	{
		viewportH = g_windowHeight;
		viewportW = (viewportH * s_presentAspectW) / s_presentAspectH;
	}

	if (viewportW < 1)
	{
		viewportW = 1;
	}
	if (viewportH < 1)
	{
		viewportH = 1;
	}

	s_presentViewport.w = viewportW;
	s_presentViewport.h = viewportH;
	s_presentViewport.x = (g_windowWidth - viewportW) / 2;
	s_presentViewport.y = (g_windowHeight - viewportH) / 2;
}

internal void NativeRenderer_InitRenderTarget(struct NativeRenderTarget *target)
{
	target->texture = (TextureID)-1;
	glGenTextures(1, &target->texture);
	glBindTexture(GL_TEXTURE_2D, target->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, NATIVE_RENDER_TARGET_TYPE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenRenderbuffers(1, &target->stencilBuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, target->stencilBuffer);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1, 1);
#else
	glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, 1, 1);
#endif
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glGenFramebuffers(1, &target->framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target->texture, 0);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, target->stencilBuffer);
#else
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, target->stencilBuffer);
#endif
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		NATIVE_RENDERER_ERROR("%s\n", "failed to create RGBA/stencil render target");
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	target->logicalWidth = 0;
	target->logicalHeight = 0;
	target->multisampled = 1; /* vitaGL multisamples new framebuffers as the display */
}

internal void NativeRenderer_DestroyRenderTarget(struct NativeRenderTarget *target)
{
	glDeleteFramebuffers(1, &target->framebuffer);
	glDeleteRenderbuffers(1, &target->stencilBuffer);
	NativeRenderer_DestroyTexture(target->texture);
	SDL_memset(target, 0, sizeof(*target));
	target->texture = (TextureID)-1;
}

internal void NativeRenderer_EnsureRenderTarget(struct NativeRenderTarget *target, int width, int height)
{
	if (width < 1)
	{
		width = 1;
	}
	if (height < 1)
	{
		height = 1;
	}

	if ((target->width == width) && (target->height == height))
	{
		return;
	}
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	NativeRenderer_EnsureTargetSize(target, width, height, 0, target->multisampled);
	return;
#endif
	NativeRenderer_ResizeRenderTarget(target, width, height);
}

/* Reallocates the target's store at width x height (contents are lost). */
internal void NativeRenderer_ResizeRenderTarget(struct NativeRenderTarget *target, int width, int height)
{

	glBindTexture(GL_TEXTURE_2D, target->texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, NATIVE_RENDER_TARGET_TYPE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindRenderbuffer(GL_RENDERBUFFER, target->stencilBuffer);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
#else
	glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);
#endif
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	target->width = width;
	target->height = height;
	target->allocWidth = width;
	target->allocHeight = height;
	s_lastBoundTexture = (TextureID)-1;
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
/* Targets are never resized in place while the game changes draw areas:
 * that reallocated the store and the GXM render target, and the main target
 * alternates between a few sizes (the display page at host resolution, small
 * areas at 1x) while some effects grow an area by a few pixels every frame
 * (zooms). Exact targets (host resolution) are kept by size in a pool and
 * swapped in; growable ones (1x) are allocated in 32-pixel steps and drawn
 * into their bottom-left width x height, which the pack and presentation
 * sample. */
internal int NativeRenderer_TargetFits(const struct NativeRenderTarget *t, int width, int height, int grow, int multisampled)
{
	if (t->framebuffer == 0 || t->multisampled != multisampled)
		return 0;
	/* Within four times the area in use: a GXM scene processes the tiles of
	 * the whole target, so a tiny area must not land in a host-sized one. */
	if (grow)
		return t->growable && t->allocWidth >= width && t->allocHeight >= height &&
		       (s64)t->allocWidth * t->allocHeight <= 4 * SDL_max((s64)width * height, 32 * 32);
	return !t->growable && t->allocWidth == width && t->allocHeight == height;
}

/* Allocation steps: powers of two up to 512 (an effect growing an area every
 * frame crosses only a few, each within 4x of the area in use), 32-pixel
 * steps above (host-resolution targets, whose scenes pay for every tile). */
internal int NativeRenderer_TargetAllocStep(int n)
{
	if (n > 512)
		return (n + 31) & ~31;
	int step = 32;
	while (step < n)
		step <<= 1;
	return step;
}

/* vitaGL multisamples every framebuffer as the display unless told: only
 * host-resolution targets keep it; VRAM and 1x targets (packed back to VRAM
 * for the game, drawn with axis-aligned quads or at PSX resolution) do not
 * need it, and each of their scenes paid for four samples per pixel. */
internal void NativeRenderer_SetTargetMultisample(GLuint framebuffer, int multisampled)
{
#ifdef __vita__
	extern int pcsx_vita_gl_msaa;
	vglFramebufferMultisample(framebuffer, !multisampled ? SCE_GXM_MULTISAMPLE_NONE
	                                       : pcsx_vita_gl_msaa >= 4 ? SCE_GXM_MULTISAMPLE_4X
	                                       : pcsx_vita_gl_msaa >= 2 ? SCE_GXM_MULTISAMPLE_2X
	                                                                : SCE_GXM_MULTISAMPLE_NONE);
#else
	(void)framebuffer;
	(void)multisampled;
#endif
}

internal void NativeRenderer_EnsureTargetSize(struct NativeRenderTarget *target, int width, int height, int grow, int multisampled)
{
	if (width < 1)
		width = 1;
	if (height < 1)
		height = 1;
	if (!NativeRenderer_TargetFits(target, width, height, grow, multisampled))
	{
		int slot = -1;
		for (int i = 0; i < NATIVE_RT_POOL_SIZE && slot < 0; i++)
			if (NativeRenderer_TargetFits(&s_rtPool[i], width, height, grow, multisampled))
				slot = i;
		const int reuse = slot >= 0;
		if (!reuse)
		{
			slot = 0;
			for (int i = 0; i < NATIVE_RT_POOL_SIZE; i++)
			{
				if (s_rtPool[i].framebuffer == 0)
				{
					slot = i;
					break;
				}
				if (s_rtPoolLastUse[i] < s_rtPoolLastUse[slot])
					slot = i;
			}
			if (s_rtPool[slot].framebuffer == 0)
				NativeRenderer_InitRenderTarget(&s_rtPool[slot]);
		}
		const struct NativeRenderTarget swap = s_rtPool[slot];
		s_rtPool[slot] = *target;
		*target = swap;
		s_rtPoolLastUse[slot] = ++s_rtPoolTick;
		if (!reuse)
		{
			NativeRenderer_ResizeRenderTarget(target, grow ? NativeRenderer_TargetAllocStep(width) : width,
			                                  grow ? NativeRenderer_TargetAllocStep(height) : height);
			target->growable = grow;
			if (target->multisampled != multisampled)
			{
				NativeRenderer_SetTargetMultisample(target->framebuffer, multisampled);
				target->multisampled = multisampled;
			}
		}
	}
	target->width = width;
	target->height = height;
	s_lastBoundTexture = (TextureID)-1;
}
#endif

internal void NativeRenderer_BindMainRenderTarget(void)
{
	RECT16 rect = NativeGpu_GetRenderDrawEnv()->clip;
	if (rect.w <= 0 || rect.h <= 0)
		rect = NativeGpu_GetRenderDispEnv()->disp;
	NativeRenderer_BindMainRect(&rect);
}

/* 0 renders the main target at the PSX resolution even in an HD build
 * (the standalone frontend's --gl-1x), for comparisons on hardware. */
int pcsx_vita_gl_hd = 1;

internal void NativeRenderer_BindMainRect(const RECT16 *rect)
{
	int logicalWidth = rect->w;
	int logicalHeight = rect->h;
	if ((logicalWidth <= 0) || (logicalHeight <= 0))
	{
		logicalWidth = NativeGpu_GetRenderDispEnv()->disp.w;
		logicalHeight = NativeGpu_GetRenderDispEnv()->disp.h;
	}

	int physicalWidth = logicalWidth;
	int physicalHeight = logicalHeight;
	int hostResolution = 0;
#ifndef CTR_NATIVE_WIDESCREEN
#define CTR_NATIVE_WIDESCREEN 0
#endif
#if CTR_NATIVE_WIDESCREEN
	const RECT16 *displayed = &NativeGpu_GetRenderDispEnv()->disp;
	/* Only a display page is drawn at the host's resolution. Games also
	 * draw small areas elsewhere and read them back (Soul Calibur, every
	 * frame): packed from a host-resolution target they read other values,
	 * and the game diverged. */
	const int displayPage = displayed->w > 0 && displayed->h > 0 && logicalWidth == displayed->w &&
	                        logicalHeight <= displayed->h && 2 * logicalHeight >= displayed->h;
	if (pcsx_vita_gl_hd && displayPage && (g_windowWidth > 0) && (g_windowHeight > 0))
	{
		/* One target pixel per presented pixel: the displayed rect fills the
		 * viewport, and the draw area is usually a part of it (Soul Calibur
		 * draws 228 of 240 lines). A target the size of the whole viewport
		 * was shrunk when presented, and nearest sampling dropped rows of
		 * thin text. */
		const int viewW = (s_presentViewport.w > 0) ? s_presentViewport.w : g_windowWidth;
		const int viewH = (s_presentViewport.h > 0) ? s_presentViewport.h : g_windowHeight;
		const RECT16 *disp = displayed;
		physicalWidth = (disp->w > 0) ? (logicalWidth * viewW + disp->w / 2) / disp->w : viewW;
		physicalHeight = (disp->h > 0) ? (logicalHeight * viewH + disp->h / 2) / disp->h : viewH;
		if (physicalWidth < logicalWidth || physicalWidth > 2 * viewW)
			physicalWidth = viewW;
		if (physicalHeight < logicalHeight || physicalHeight > 2 * viewH)
			physicalHeight = viewH;
		hostResolution = 1;
	}
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	NativeRenderer_EnsureTargetSize(&s_mainRenderTarget, physicalWidth, physicalHeight, 1, hostResolution);
#else
	(void)hostResolution;
	NativeRenderer_EnsureRenderTarget(&s_mainRenderTarget, physicalWidth, physicalHeight);
#endif
	s_mainRenderTarget.logicalWidth = logicalWidth;
	s_mainRenderTarget.logicalHeight = logicalHeight;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	s_mainDrawRect = *rect;
#endif
	glBindFramebuffer(GL_FRAMEBUFFER, s_mainRenderTarget.framebuffer);
}

internal void NativeRenderer_DrawVRAMRegion(int x, int y, int width, int height)
{
	glUseProgram(s_presentVramShader);
	glUniform4f(s_presentVramSourceRectLoc, (float)x, (float)y, (float)width, (float)height);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, s_vram.texture);
	glBindVertexArray(s_vramQuadVAO);
	NativeRenderer_DrawTriangles(0, 2);
}

internal void NativeRenderer_LoadRenderTargetFromVRAM(struct NativeRenderTarget *target, int x, int y, int logicalWidth, int logicalHeight)
{
	NATIVE_GPU_LABEL(0, "load-from-vram", 0);

	const ShaderID previousShader = s_previousShader;
	const TextureID previousTexture = s_lastBoundTexture;
	const BlendMode previousBlendMode = s_previousBlendMode;
	const int previousMixedSTPBlend = s_previousMixedSTPBlend;
	const int previousDepthMode = s_previousDepthMode;
	const int previousDepthWrite = s_previousDepthWrite;
	const int previousScissorState = s_previousScissorState;
	const GLboolean previousStencilEnabled = glIsEnabled(GL_STENCIL_TEST);

	NativeRenderer_UpdateVRAM();
	glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
	NativeRenderer_SetDepthState(0, 1);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glViewport(0, 0, target->width, target->height);
	NativeRenderer_DrawVRAMRegion(x, y, logicalWidth, logicalHeight);
	glClear(GL_STENCIL_BUFFER_BIT);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	glClear(GL_DEPTH_BUFFER_BIT);
#endif
	if (previousStencilEnabled)
	{
		glEnable(GL_STENCIL_TEST);
	}

	if (s_boundVertexBuffer >= 0)
	{
		glBindVertexArray(s_glVertexArray[s_boundVertexBuffer]);
	}
	else
	{
		glBindVertexArray(0);
	}
	glUseProgram(previousShader == (ShaderID)-1 ? 0 : previousShader);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, previousTexture == (TextureID)-1 ? 0 : previousTexture);
	s_previousShader = previousShader;
	s_lastBoundTexture = previousTexture;
	s_previousBlendMode = BM_NONE;
	s_previousMixedSTPBlend = 0;
	s_previousScissorState = 0;
	if (previousMixedSTPBlend)
	{
		NativeRenderer_SetMixedSTPBlendMode(previousBlendMode);
	}
	else
	{
		NativeRenderer_SetBlendMode(previousBlendMode);
	}
	NativeRenderer_SetDepthState(previousDepthMode, previousDepthWrite);
	NativeRenderer_SetScissorState(previousScissorState);
	NATIVE_GPU_LABEL(1, NULL, 0);
}

internal void NativeRenderer_ClearHostRect(int x, int y, int width, int height)
{
	if ((width <= 0) || (height <= 0))
	{
		return;
	}

	glScissor(x, y, width, height);
	glClear(GL_COLOR_BUFFER_BIT);
}

internal void NativeRenderer_ClearPresentationBars(void)
{
	GLint previousScissorBox[4];
	GLfloat previousClearColor[4];
	const GLboolean previousScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
	const int viewportRight = s_presentViewport.x + s_presentViewport.w;
	const int viewportTop = s_presentViewport.y + s_presentViewport.h;

	if ((g_windowWidth <= 0) || (g_windowHeight <= 0))
	{
		return;
	}

	if ((s_presentViewport.x == 0) && (s_presentViewport.y == 0) && (s_presentViewport.w == g_windowWidth) && (s_presentViewport.h == g_windowHeight))
	{
		return;
	}

	glGetIntegerv(GL_SCISSOR_BOX, previousScissorBox);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);

	glEnable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

	NativeRenderer_ClearHostRect(0, 0, s_presentViewport.x, g_windowHeight);
	NativeRenderer_ClearHostRect(viewportRight, 0, g_windowWidth - viewportRight, g_windowHeight);
	NativeRenderer_ClearHostRect(s_presentViewport.x, 0, s_presentViewport.w, s_presentViewport.y);
	NativeRenderer_ClearHostRect(s_presentViewport.x, viewportTop, s_presentViewport.w, g_windowHeight - viewportTop);

	if (previousScissorEnabled)
	{
		glEnable(GL_SCISSOR_TEST);
		glScissor(previousScissorBox[0], previousScissorBox[1], previousScissorBox[2], previousScissorBox[3]);
	}
	else
	{
		glDisable(GL_SCISSOR_TEST);
	}

	glClearColor(previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
	s_previousScissorState = previousScissorEnabled ? 1 : 0;
	NativeRenderer_InvalidateScissorRectCache();
}

void NativeRenderer_ResetDevice(void)
{
	NativeRenderer_UpdatePresentationViewport();
	NativeRenderer_UpdateSwapIntervalState(0);
}

typedef struct
{
	// shader itself
	ShaderID shader;

	GLint projectionLoc;
	GLint bilinearFilterLoc;
	GLint texelSizeLoc;
	GLint texLoc;
	GLint lutLoc;
	GLint textureWindowLoc;
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	GLint psxSemiTransPassLoc;
	GLint psxDitherEnabledLoc;
#endif
	GLint psxDrawMaskSetLoc;
	GLint psxTextureOutputStpLoc;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	/* The program applies no texture window (PSX_TEXTURE_WINDOW undefined):
	 * the twin with one is compiled from the same source on first use. */
	const char *source;
	const char *defines;
	void *windowed;
	char label[64]; /* draw name in GPU profiler captures */
#endif
} GTEShader;

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
enum NativePsxShaderVariant
{
	NATIVE_PSX_SHADER_OPAQUE,
	NATIVE_PSX_SHADER_OPAQUE_AVERAGE,
	NATIVE_PSX_SHADER_OPAQUE_QUARTER,
	NATIVE_PSX_SHADER_NON_STP,
	NATIVE_PSX_SHADER_STP,
	NATIVE_PSX_SHADER_STP_AVERAGE,
	NATIVE_PSX_SHADER_STP_QUARTER,
	NATIVE_PSX_SHADER_MIXED_AVERAGE,
	NATIVE_PSX_SHADER_MIXED_ADD,
	NATIVE_PSX_SHADER_MIXED_QUARTER,
	NATIVE_PSX_SHADER_VARIANT_COUNT
};
#endif

internal int NativeRenderer_Shader_CheckShaderStatus(GLuint shader);
internal int NativeRenderer_Shader_CheckProgramStatus(GLuint program);
internal ShaderID NativeRenderer_Shader_Compile(const char *source, bool isPsxShader, const char *fragmentDefines);
internal void NativeRenderer_ProgramLabel(char *name, size_t size, const char *source, const char *fragmentDefines);
internal void NativeRenderer_GenerateCommonTextures(void);
internal int NativeRenderer_CompilePSXShader(GTEShader *sh, const char *source, const char *fragmentDefines);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
internal void NativeRenderer_DestroyPSXShader(GTEShader *sh);
#endif
internal int NativeRenderer_InitialisePSXShaders(void);
internal void NativeRenderer_InitRG8LUT(void);
internal void NativeRenderer_Ortho2D(float left, float right, float bottom, float top, float znear, float zfar);
internal void NativeRenderer_SetShader(const ShaderID shader);
internal void NativeRenderer_SyncGpuVRAMToCPU(int x, int y, int w, int h);
internal void NativeRenderer_ResolveVRAMRead(int x, int y, int w, int h);
internal void NativeRenderer_GpuPackTextureToVRAM(const struct NativeRenderTarget *source, int x, int y, int w, int h, b32 flipY);

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
global_variable GTEShader s_gteShaderVariants[4][NATIVE_PSX_SHADER_VARIANT_COUNT];
global_variable GTEShader s_gteCachedP4ShaderVariants[NATIVE_PSX_SHADER_VARIANT_COUNT];
/* Cached 4-bit textures whose palettes have no transparent entry: no discard,
 * so PowerVR's hidden surface removal skips occluded fragments. */
global_variable GTEShader s_gteCachedP4FullyOpaqueShaderVariants[3];
global_variable GTEShader s_gteFullyOpaqueShaderVariants[2][3];
global_variable GTEShader s_gteUntexturedShaderVariants[3];
#else
global_variable GTEShader s_gteShader4;
global_variable GTEShader s_gteShader8;
global_variable GTEShader s_gteShader16;
global_variable GTEShader s_gteShader4SuperTurbo;
global_variable GTEShader s_gteShader8SuperTurbo;
global_variable GTEShader s_gteShader16SuperTurbo;
global_variable GTEShader s_gteShader32Rgba;
#endif

GLint u_projectionLoc;
GLint u_bilinearFilterLoc;
GLint u_texelSizeLoc;
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
GLint u_psxSemiTransPassLoc;
GLint u_psxDitherEnabledLoc;
#endif
GLint u_psxDrawMaskSetLoc;
GLint u_psxTextureOutputStpLoc;

internal void NativeRenderer_DestroyPSXShaders(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	for (int format = TF_4_BIT; format <= TF_32_BIT_RGBA; format++)
	{
		for (int variant = 0; variant < NATIVE_PSX_SHADER_VARIANT_COUNT; variant++)
		{
			NativeRenderer_DestroyPSXShader(&s_gteShaderVariants[format][variant]);
		}
	}
	for (int variant = 0; variant < NATIVE_PSX_SHADER_VARIANT_COUNT; variant++)
	{
		NativeRenderer_DestroyPSXShader(&s_gteCachedP4ShaderVariants[variant]);
		if (variant < 3)
			NativeRenderer_DestroyPSXShader(&s_gteCachedP4FullyOpaqueShaderVariants[variant]);
	}
	for (int format = TF_4_BIT; format <= TF_8_BIT; format++)
	{
		for (int variant = 0; variant < 3; variant++)
		{
			NativeRenderer_DestroyPSXShader(&s_gteFullyOpaqueShaderVariants[format][variant]);
		}
	}
	for (int variant = 0; variant < 3; variant++)
	{
		NativeRenderer_DestroyPSXShader(&s_gteUntexturedShaderVariants[variant]);
	}
#else
	if (s_gteShader4.shader != 0)
	{
		glDeleteProgram(s_gteShader4.shader);
	}
	if (s_gteShader8.shader != 0)
	{
		glDeleteProgram(s_gteShader8.shader);
	}
	if (s_gteShader16.shader != 0)
	{
		glDeleteProgram(s_gteShader16.shader);
	}
	if (s_gteShader4SuperTurbo.shader != 0)
	{
		glDeleteProgram(s_gteShader4SuperTurbo.shader);
	}
	if (s_gteShader8SuperTurbo.shader != 0)
	{
		glDeleteProgram(s_gteShader8SuperTurbo.shader);
	}
	if (s_gteShader16SuperTurbo.shader != 0)
	{
		glDeleteProgram(s_gteShader16SuperTurbo.shader);
	}
	if (s_gteShader32Rgba.shader != 0)
	{
		glDeleteProgram(s_gteShader32Rgba.shader);
	}
#endif
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
#define GPU_SAMPLE_TEXTURE_4BIT_FUNC                                                                                \
	"\tvec2 samplePSX(vec2 tc) {\n"                                                                               \
	"\t\ttc = applyTextureWindow(tc);\n"                                                                              \
	"\t\tvec2 texel = floor(tc);\n"                                                             \
	"\t\tfloat texelX = texel.x;\n"                                                                            \
	"\t\tfloat lanePhase = fract(texelX * 0.25);\n"                                                        \
	"\t\tfloat highByte = step(0.5, lanePhase);\n"                                                        \
	"\t\tfloat highNibbleSel = step(0.5, fract(texelX * 0.5));\n"                                     \
	"\t\tvec2 packedPixel = v_page_clut.xy + vec2(floor(texelX * 0.25), texel.y);\n"                      \
	"\t\tvec2 packedRg = VRAM((packedPixel + vec2(0.5)) * c_VRAMTexel);\n"                              \
	"\t\tfloat packedByte = floor(mix(packedRg.x, packedRg.y, highByte) * 255.0 + 0.5);\n"                 \
	"\t\tfloat highNibble = floor(packedByte * (1.0 / 16.0));\n"                                     \
	"\t\tfloat paletteIndex = mix(packedByte - highNibble * 16.0, highNibble, highNibbleSel);\n"       \
	"\t\treturn samplePSXPalette(paletteIndex);\n"                                                        \
	"\t}\n"
#else
#define GPU_SAMPLE_TEXTURE_4BIT_FUNC                                                              \
	"	// returns 16 bit colour\n"                                                                 \
	"	vec2 samplePSX(vec2 tc) {\n"                                                        \
	"		vec2 uv = (tc * vec2(0.25, 1.0) + v_page_clut.xy) * c_VRAMTexel;\n"                \
	"		vec2 comp = VRAM(uv);\n"                                                           \
	"		float lane = mod(floor(tc.x + 0.0001), 4.0);\n"                                 \
	"		float byteValue = floor(mix(comp.x, comp.y, step(1.5, lane)) * 255.0 + 0.5);\n" \
	"		float lowNibble = mod(byteValue, 16.0);\n"                                        \
	"		float highNibble = floor(byteValue * (1.0 / 16.0));\n"                         \
	"		float paletteIndex = mix(lowNibble, highNibble, mod(lane, 2.0));\n"              \
	"		vec2 clut_pos = v_page_clut.zw;\n"                                                 \
	"		clut_pos.x += paletteIndex * c_VRAMTexel.x;\n"                                   \
	"		return VRAM(clut_pos);\n"                                                          \
	"	}\n"
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
#define GPU_SAMPLE_TEXTURE_8BIT_FUNC                                                                                \
	"\tvec2 samplePSX(vec2 tc) {\n"                                                                               \
	"\t\ttc = applyTextureWindow(tc);\n"                                                                              \
	"\t\tfloat highByte = step(0.5, fract(floor(tc.x) * 0.5 + 0.25));\n"                                      \
	"\t\tvec2 packedRg = VRAM((v_page_clut.xy + vec2(floor(tc.x) * 0.5 + 0.25, floor(tc.y) + 0.5)) * c_VRAMTexel);\n" \
	"\t\tfloat paletteIndex = floor(mix(packedRg.x, packedRg.y, highByte) * 255.0 + 0.5);\n"              \
	"\t\treturn samplePSXPalette(paletteIndex);\n"                                                        \
	"\t}\n"
#else
#define GPU_SAMPLE_TEXTURE_8BIT_FUNC                                                              \
	"	// returns 16 bit colour\n"                                                                 \
	"	vec2 samplePSX(vec2 tc) {\n"                                                        \
	"		vec2 uv = (tc * vec2(0.5, 1.0) + v_page_clut.xy) * c_VRAMTexel;\n"                 \
	"		vec2 comp = VRAM(uv);\n"                                                           \
	"		float lane = mod(floor(tc.x + 0.0001), 2.0);\n"                                 \
	"		float paletteIndex = mix(comp.x, comp.y, lane) * 255.0;\n"                       \
	"		vec2 clut_pos = v_page_clut.zw;\n"                                                 \
	"		clut_pos.x += paletteIndex * c_VRAMTexel.x;\n"                                   \
	"		return VRAM(clut_pos);\n"                                                          \
	"	}\n"
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
#define GPU_SAMPLE_TEXTURE_16BIT_FUNC                                                       \
	"	vec2 samplePSX(vec2 tc) {\n"                                                          \
	"\t\ttc = applyTextureWindow(tc);\n"                                                       \
	"\t\treturn VRAM((v_page_clut.xy + floor(tc) + vec2(0.5)) * c_VRAMTexel);\n"             \
	"	}\n"
#else
#define GPU_SAMPLE_TEXTURE_16BIT_FUNC                    \
	"	vec2 samplePSX(vec2 tc) {\n"                       \
	"		vec2 uv = (tc + v_page_clut.xy) * c_VRAMTexel;\n" \
	"		return VRAM(uv);\n"                               \
	"	}\n"
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
/* CLUT entries are consecutive VRAM words: crossing X=1023 carries into
 * the following row. Texture-page X wrapping alone must not lose that carry. */
#define GPU_PALETTE_SAMPLE_FUNC                                                                  \
	"\tvec2 samplePSXPalette(float paletteIndex) {\n"                                            \
	"\t\tfloat paletteX = v_page_clut.z + paletteIndex;\n"                                   \
	"\t\tvec2 pixel = vec2(mod(paletteX, 1024.0), v_page_clut.w + floor(paletteX / 1024.0));\n" \
	"\t\treturn VRAM((pixel + vec2(0.5)) * c_VRAMTexel);\n"                                  \
	"\t}\n"
#else
#define GPU_PALETTE_SAMPLE_FUNC ""
#endif

#define GPU_FETCH_VRAM_FUNC                                        \
	"	const vec2 c_VRAMTexel = vec2(1.0 / 1024.0, 1.0 / VRAM_HEIGHT);\n" \
	"	uniform sampler2D s_texture;\n"                              \
	"	vec2 VRAM(vec2 uv) { return texture2D(s_texture, uv).rg; }\n" \
	GPU_PALETTE_SAMPLE_FUNC

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
#define GPU_TEXTURE_WINDOW_FUNC                                                                    \
	"#ifdef PSX_TEXTURE_WINDOW\n"                                                                  \
	"\tuniform vec4 s_textureWindow;\n"                                                           \
	"\tfloat textureWindowBit(float texel, float mask, float offset, float bitValue) {\n" \
	"\t\tfloat sourceBit = mod(floor(texel / bitValue), 2.0);\n"                          \
	"\t\tfloat offsetBit = mod(floor(offset / (bitValue / 8.0)), 2.0);\n"                          \
	"\t\tfloat maskSet = mod(floor(mask / (bitValue / 8.0)), 2.0);\n"                            \
	"\t\treturn mix(sourceBit, offsetBit, maskSet) * bitValue;\n"                          \
	"\t}\n"                                                                                         \
	"\tvec2 applyTextureWindow(vec2 tc) {\n"                                                        \
	"\t\tvec2 texel = floor(tc);\n"                                                            \
	"\t\tvec2 result = mod(texel, 8.0);\n"                                                     \
	"\t\tresult += floor(texel / 256.0) * 256.0;\n"                                              \
	"\t\tresult.x += textureWindowBit(texel.x, s_textureWindow.x, s_textureWindow.z, 8.0);\n" \
	"\t\tresult.x += textureWindowBit(texel.x, s_textureWindow.x, s_textureWindow.z, 16.0);\n" \
	"\t\tresult.x += textureWindowBit(texel.x, s_textureWindow.x, s_textureWindow.z, 32.0);\n" \
	"\t\tresult.x += textureWindowBit(texel.x, s_textureWindow.x, s_textureWindow.z, 64.0);\n" \
	"\t\tresult.x += textureWindowBit(texel.x, s_textureWindow.x, s_textureWindow.z, 128.0);\n" \
	"\t\tresult.y += textureWindowBit(texel.y, s_textureWindow.y, s_textureWindow.w, 8.0);\n" \
	"\t\tresult.y += textureWindowBit(texel.y, s_textureWindow.y, s_textureWindow.w, 16.0);\n" \
	"\t\tresult.y += textureWindowBit(texel.y, s_textureWindow.y, s_textureWindow.w, 32.0);\n" \
	"\t\tresult.y += textureWindowBit(texel.y, s_textureWindow.y, s_textureWindow.w, 64.0);\n" \
	"\t\tresult.y += textureWindowBit(texel.y, s_textureWindow.y, s_textureWindow.w, 128.0);\n" \
	"\t\treturn result + fract(tc);\n"                                                           \
	"\t}\n"                                                                                         \
	"#else\n"                                                                                       \
	"\tvec2 applyTextureWindow(vec2 tc) { return floor(tc) + fract(tc); }\n"                      \
	"#endif\n"
#else
#define GPU_TEXTURE_WINDOW_FUNC "vec2 applyTextureWindow(vec2 tc) { return tc; }\n"
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
#define GPU_STP_PASS_FUNC                                                                                 \
	"	float texelVisible(vec2 rg) { return step(0.5 / 255.0, rg.x + rg.y); }\n"                       \
	"	float stpWeight(vec2 rg) { return step(0.5, rg.y); }\n"                                           \
	"	bool discardForSemiTransPass(float visible, float stpClass) {\n"                             \
	"		if (visible < 0.5) { return true; }\n"                                                        \
	"#ifdef PSX_PASS_NON_STP\n		if (stpClass >= 0.5) { return true; }\n#endif\n"                         \
	"#ifdef PSX_PASS_STP\n		if (stpClass < 0.5) { return true; }\n#endif\n"                             \
	"		return false;\n"                                                                                 \
	"	}\n"
#define GPU_SEMI_TRANS_UNIFORM
#else
#define GPU_STP_PASS_FUNC                                                                                 \
	"	float texelVisible(vec2 rg) { return float(rg.x + rg.y > 0.0); }\n"                               \
	"	float stpWeight(vec2 rg) { return step(0.5, rg.y); }\n"                                           \
	"	bool discardForSemiTransPass(float visible, float stpClass) {\n"                             \
	"		if (visible < 0.5) { return true; }\n"                                                       \
	"		if (psxSemiTransPass == 1 && stpClass >= 0.5) { return true; }\n"                         \
	"		if (psxSemiTransPass == 2 && stpClass < 0.5) { return true; }\n"                          \
	"		return false;\n"                                                                                 \
	"	}\n"
#define GPU_SEMI_TRANS_UNIFORM "\tuniform int psxSemiTransPass;\n"
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
#define GPU_DITHERING "\tvec4 dither(vec4 color) { return color; }\n"
#else
#define GPU_DITHERING                                             \
	"\tuniform int psxDitherEnabled;\n"                            \
	"	const mat4 c_dither = mat4(\n"                              \
	"		-4.0,  +0.0,  -3.0,  +1.0,\n"                              \
	"		+2.0,  -2.0,  +3.0,  -1.0,\n"                              \
	"		-3.0,  +1.0,  -4.0,  +0.0,\n"                              \
	"		+3.0,  -1.0,  +2.0,  -2.0) / 255.0;\n"                     \
	"	vec4 dither(vec4 color) {\n"                                \
	"		ivec2 dc = ivec2(mod(floor(v_ditherCoord), 4.0));\n"       \
	"		color.xyz += vec3(c_dither[dc.x][dc.y] * v_texcoord.w * float(psxDitherEnabled));\n" \
	"		return color;\n"                                           \
	"	}\n"
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
#define GPU_PSX_COLOR_UNIFORM
#define GPU_PSX_COLOR_DECODE                                                                                   \
	"	vec4 decodePSX(vec2 rg) {\n"                                                                         \
	"		vec2 scaled = rg * vec2(255.0 / 32.0, 255.0 / 4.0);\n"                                  \
	"		vec2 whole = floor(scaled + vec2(0.001));\n"                                                \
	"		vec2 part = scaled - whole;\n"                                                               \
	"		float blue = whole.y - step(32.0, whole.y) * 32.0;\n"                                      \
	"		vec3 color5 = vec3(part.x * 32.0, whole.x + part.y * 32.0, blue);\n"                       \
	"		return vec4(color5 * (8.0 / 255.0), 1.0);\n"                                            \
	"	}\n"
#define GPU_PSX_FRAGMENT_OUTPUT                                                                                \
	"		gl_FragColor.rgb = color.rgb * v_color.rgb;\n"                                                  \
	"		gl_FragColor.a = max(psxDrawMaskSet, psxTextureOutputStp * sampledStp);\n"
#else
#define GPU_PSX_COLOR_UNIFORM "\tuniform sampler2D s_rgLut;\n"
#define GPU_PSX_COLOR_DECODE                                                                                   \
	"	const vec2 c_LUTTexel = vec2(1.0 / 256.0, 1.0 / 256.0);\n"                                      \
	"	vec4 decodePSX(vec2 rg) { return texture2D(s_rgLut, rg - c_LUTTexel * 0.0001); }\n"
#define GPU_PSX_FRAGMENT_OUTPUT                                                                                \
	"#ifdef SUPER_TURBO_TINT\n"                                                                        \
	"		vec3 stpColor5 = floor(color.rgb * (255.0 / 8.0) + vec3(0.001));\n"                        \
	"		float stpLum5 = max(stpColor5.r, max(stpColor5.g, stpColor5.b));\n"                           \
	"		float stpBoost5 = min(31.0, floor((stpLum5 * 5.0 + 3.0) * 0.25));\n"                      \
	"		color.rgb = vec3(floor(stpLum5 * 0.3), stpBoost5, min(31.0, stpBoost5 + 2.0)) * (8.0 / 255.0);\n" \
	"#endif\n"                                                                                         \
	"		gl_FragColor = dither(color * v_color);\n"                                                   \
	"		gl_FragColor.a = max(psxDrawMaskSet, psxTextureOutputStp * sampledStp);\n"
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
// This is used to simulate GL_CONSTANT_COLOR blending since sceGxm has no equivalents for them
#define GPU_PSX_BLEND_APPLY                                                                        \
	"#ifdef PSX_BLEND_AVERAGE\n		gl_FragColor.a = (127.0 + gl_FragColor.a) / 255.0;\n#endif\n" \
	"#ifdef PSX_BLEND_QUARTER\n		gl_FragColor.rgb *= 0.25;\n#endif\n"                              \
	"#ifdef PSX_MIXED_AVERAGE\n		gl_FragColor.rgb *= 1.0 - sampledStp * 0.5;\n		gl_FragColor.a *= 0.5;\n#endif\n" \
	"#ifdef PSX_MIXED_QUARTER\n		gl_FragColor.rgb *= 1.0 - sampledStp * 0.75;\n#endif\n"
#else
#define GPU_PSX_BLEND_APPLY
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
#define GPU_TEXTURE_SAMPLE_MAIN "		vec4 color = nearestTextureSample(v_texcoord.xy);\n"
#else
#define GPU_TEXTURE_SAMPLE_MAIN \
	"		vec4 color = (bilinearFilter > 0) ? bilinearTextureSample(v_texcoord.xy) : nearestTextureSample(v_texcoord.xy);\n"
#endif

#define GPU_FRAGMENT_SAMPLE_SHADER(bit)                                                                                                               \
	GPU_FETCH_VRAM_FUNC                                                                                                                               \
	GPU_TEXTURE_WINDOW_FUNC                                                                                                                          \
	GPU_SAMPLE_TEXTURE_##bit##BIT_FUNC                                                                                                                \
	    GPU_PSX_COLOR_UNIFORM                                                                                                                         \
	    "#ifndef VITA_NEAREST_ONLY\n\tuniform int bilinearFilter;\n#endif\n"                                                                                 \
	    GPU_SEMI_TRANS_UNIFORM                                                                                                                        \
	    "	uniform float psxDrawMaskSet;\n"                                                                                                            \
	    "	uniform float psxTextureOutputStp;\n"                                                                                                       \
	    "	float sampledStp = 0.0;\n"                                                                                                                  \
	    GPU_PSX_COLOR_DECODE GPU_STP_PASS_FUNC "#ifndef VITA_NEAREST_ONLY\n\tvec4 bilinearTextureSample(vec2 P) {\n"                          \
	    "		vec2 _frac = fract(P);\n"                                                                                                                   \
	    "		vec2 pixel = floor(P);\n"                                                                                                                  \
	    "		vec2 C11 = samplePSX(pixel);\n"                                                                                                            \
	    "		vec2 C21 = samplePSX(pixel + vec2(1.0, 0.0));\n"                                                                                           \
	    "		vec2 C12 = samplePSX(pixel + vec2(0.0, 1.0));\n"                                                                                           \
	    "		vec2 C22 = samplePSX(pixel + vec2(1.0, 1.0));\n"                                                                                           \
	    "		float v11 = texelVisible(C11);\n"                                                                                                          \
	    "		float v21 = texelVisible(C21);\n"                                                                                                          \
	    "		float v12 = texelVisible(C12);\n"                                                                                                          \
	    "		float v22 = texelVisible(C22);\n"                                                                                                          \
	    "		float s11 = v11 * stpWeight(C11);\n"                                                                                                       \
	    "		float s21 = v21 * stpWeight(C21);\n"                                                                                                       \
	    "		float s12 = v12 * stpWeight(C12);\n"                                                                                                       \
	    "		float s22 = v22 * stpWeight(C22);\n"                                                                                                       \
	    "		float n11 = v11 - s11;\n"                                                                                                                  \
	    "		float n21 = v21 - s21;\n"                                                                                                                  \
	    "		float n12 = v12 - s12;\n"                                                                                                                  \
	    "		float n22 = v22 - s22;\n"                                                                                                                  \
	    "		float ax1 = mix(v11, v21, _frac.x);\n"                                                                                                      \
	    "		float ax2 = mix(v12, v22, _frac.x);\n"                                                                                                      \
	    "		float axm = mix(ax1, ax2, _frac.y);\n"                                                                                                      \
	    "		float sx1 = mix(s11, s21, _frac.x);\n"                                                                                                      \
	    "		float sx2 = mix(s12, s22, _frac.x);\n"                                                                                                      \
	    "		float stp = mix(sx1, sx2, _frac.y);\n"                                                                                                      \
	    "		float nx1 = mix(n11, n21, _frac.x);\n"                                                                                                      \
	    "		float nx2 = mix(n12, n22, _frac.x);\n"                                                                                                      \
	    "		float nonStp = mix(nx1, nx2, _frac.y);\n"                                                                                                   \
	    "		vec2 rg = mix(mix(C11, C21, _frac.x), mix(C12, C22, _frac.x), _frac.y);\n"                                                                    \
	    "		float stpClass = step(nonStp, stp);\n"                                                                                                    \
	    "		sampledStp = stpClass;\n"                                                                                                                  \
	    "		if(discardForSemiTransPass(axm, stpClass)) { discard; }\n"                                                                                 \
	    "		vec4 x1 = mix(decodePSX(C11), decodePSX(C21), _frac.x);\n"                                                                              \
	    "		vec4 x2 = mix(decodePSX(C12), decodePSX(C22), _frac.x);\n"                                                                              \
	    "		vec4 t = mix(x1, x2, _frac.y);\n"                                                                                                           \
	    "		return t;\n"                                                                                                                               \
	    "	}\n#endif\n"                                                                                                                                \
	    "	vec4 nearestTextureSample(vec2 P) {\n"                                                                                                      \
	    "		vec2 rg = samplePSX(P);\n"                                                                                                                 \
	    "#ifdef PSX_FULLY_OPAQUE\n"                                                                                                                      \
	    "		sampledStp = stpWeight(rg);\n"                                                                                                            \
	    "#else\n"                                                                                                                                       \
	    "		float visible = texelVisible(rg);\n"                                                                                                       \
	    "		sampledStp = visible * stpWeight(rg);\n"                                                                                                   \
	    "		if(discardForSemiTransPass(visible, sampledStp)) { discard; }\n"                                                                         \
	    "#endif\n"                                                                                                                                      \
	    "		vec4 t = decodePSX(rg);\n"                                                                                                                 \
	    "		return t;\n"                                                                                                                               \
	    "	}\n"                                                                                                                                        \
	    "	void main() {\n"                                                                                                                            \
	    GPU_TEXTURE_SAMPLE_MAIN                                                                                                                         \
	    GPU_PSX_FRAGMENT_OUTPUT                                                                                                                         \
	    GPU_PSX_BLEND_APPLY                                                                                                                            \
	    "	}\n"

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
global_variable const char *gte_shader_cached_p4 =
    "\tuniform sampler2D s_texture;\n"
    "#ifdef PSX_TEXTURE_WINDOW\n"
    "\tuniform vec4 s_textureWindow;\n"
    "\tfloat textureWindowBit(float texel, float mask, float offset, float bitValue) {\n"
    "\t\tfloat sourceBit = mod(floor(texel / bitValue), 2.0);\n"
    "\t\tfloat offsetBit = mod(floor(offset / (bitValue / 8.0)), 2.0);\n"
    "\t\tfloat maskSet = mod(floor(mask / (bitValue / 8.0)), 2.0);\n"
    "\t\treturn mix(sourceBit, offsetBit, maskSet) * bitValue;\n"
    "\t}\n"
    "\tvec2 applyTextureWindow(vec2 tc) {\n"
    "\t\tvec2 texel = floor(tc);\n"
    "\t\tvec2 result = mod(texel, 8.0) + floor(texel / 256.0) * 256.0;\n"
    "\t\tresult.x += textureWindowBit(texel.x, s_textureWindow.x, s_textureWindow.z, 8.0);\n"
    "\t\tresult.x += textureWindowBit(texel.x, s_textureWindow.x, s_textureWindow.z, 16.0);\n"
    "\t\tresult.x += textureWindowBit(texel.x, s_textureWindow.x, s_textureWindow.z, 32.0);\n"
    "\t\tresult.x += textureWindowBit(texel.x, s_textureWindow.x, s_textureWindow.z, 64.0);\n"
    "\t\tresult.x += textureWindowBit(texel.x, s_textureWindow.x, s_textureWindow.z, 128.0);\n"
    "\t\tresult.y += textureWindowBit(texel.y, s_textureWindow.y, s_textureWindow.w, 8.0);\n"
    "\t\tresult.y += textureWindowBit(texel.y, s_textureWindow.y, s_textureWindow.w, 16.0);\n"
    "\t\tresult.y += textureWindowBit(texel.y, s_textureWindow.y, s_textureWindow.w, 32.0);\n"
    "\t\tresult.y += textureWindowBit(texel.y, s_textureWindow.y, s_textureWindow.w, 64.0);\n"
    "\t\tresult.y += textureWindowBit(texel.y, s_textureWindow.y, s_textureWindow.w, 128.0);\n"
    "\t\treturn result + fract(tc);\n"
    "\t}\n"
    "#else\n"
    "\tvec2 applyTextureWindow(vec2 tc) { return floor(tc) + fract(tc); }\n"
    "#endif\n"
    "\tuniform float psxDrawMaskSet;\n"
    "\tuniform float psxTextureOutputStp;\n"
    "\tfloat sampledStp = 0.0;\n"
    "\tbool discardCachedP4(float visible, float stpClass) {\n"
    "\t\tif (visible < 0.5) { return true; }\n"
    "#ifdef PSX_PASS_NON_STP\n\t\tif (stpClass >= 0.5) { return true; }\n#endif\n"
    "#ifdef PSX_PASS_STP\n\t\tif (stpClass < 0.5) { return true; }\n#endif\n"
    "\t\treturn false;\n"
    "\t}\n"
    "\tvec4 nearestTextureSample(vec2 P) {\n"
    "\t\tvec2 texel = floor(applyTextureWindow(P));\n"
    "\t\tvec4 t = texture2D(s_texture, (texel + vec2(0.5)) * (1.0 / 256.0));\n"
    "\t\tfloat visible = step(0.25, t.a);\n"
    "\t\tsampledStp = step(0.75, t.a);\n"
    "#ifndef PSX_FULLY_OPAQUE\n"
    "\t\tif (discardCachedP4(visible, sampledStp)) { discard; }\n"
    "#endif\n"
    "\t\tt.a = 1.0;\n"
    "\t\treturn t;\n"
    "\t}\n"
    "\tvoid main() {\n"
    "\t\tvec4 color = nearestTextureSample(v_texcoord.xy);\n"
    GPU_PSX_FRAGMENT_OUTPUT
    GPU_PSX_BLEND_APPLY
    "\t}\n";
#endif

global_variable const char *gpu_shader_common = "	centroid varying vec4 v_texcoord;\n"
                                                "	varying vec4 v_color;\n"
                                                "	varying vec4 v_page_clut;\n"
                                                "	varying vec2 v_ditherCoord;\n"
                                                "	varying float v_z;\n";

const char *gte_shader_4 = GPU_FRAGMENT_SAMPLE_SHADER(4);
const char *gte_shader_8 = GPU_FRAGMENT_SAMPLE_SHADER(8);
const char *gte_shader_16 = GPU_FRAGMENT_SAMPLE_SHADER(16);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
const char *gte_shader_untextured = "\tuniform float psxDrawMaskSet;\n"
                                    "\tvoid main() {\n"
                                    "\t\tgl_FragColor.rgb = v_color.rgb * (248.0 / 255.0);\n"
                                    "\t\tgl_FragColor.a = psxDrawMaskSet;\n"
                                    GPU_PSX_BLEND_APPLY
                                    "\t}\n";

#define GPU_RGBA_FRAGMENT_OUTPUT                                    \
	"		gl_FragColor.rgb = color.rgb * v_color.rgb;\n"             \
	"		gl_FragColor.a = psxDrawMaskSet;\n"
#else
#define GPU_RGBA_FRAGMENT_OUTPUT                                    \
	"		gl_FragColor = dither(color * v_color);\n"              \
	"		gl_FragColor.a = psxDrawMaskSet;\n"
#endif

const char *gte_shader_32_rgba = "	uniform sampler2D s_texture;\n"
                                 GPU_SEMI_TRANS_UNIFORM
                                 "	uniform float psxDrawMaskSet;\n"
                                 "	uniform vec2 texelSize;\n"
                                 "	void main() {\n"
                                 "		vec2 tc = v_texcoord.xy * texelSize + texelSize * 0.5;\n"
                                 "		vec4 color = texture2D(s_texture, tc);\n"
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
                                 "#if defined(PSX_PASS_NON_STP) || defined(PSX_PASS_STP)\n"
                                 "		if (color.a < 0.25) { discard; }\n"
                                 "		float sampledStp = step(0.75, color.a);\n"
                                 "#ifdef PSX_PASS_NON_STP\n		if (sampledStp >= 0.5) { discard; }\n#endif\n"
                                 "#ifdef PSX_PASS_STP\n		if (sampledStp < 0.5) { discard; }\n#endif\n"
                                 "#else\n"
                                 "		if (color.a < 0.5) { discard; }\n"
                                 "#endif\n"
#else
                                 "		if (psxSemiTransPass == 0) {\n"
                                 "			if (color.a < 0.5) { discard; }\n"
                                 "		} else {\n"
                                 "			if (color.a < 0.25) { discard; }\n"
                                 "			float sampledStp = step(0.75, color.a);\n"
                                 "			if (psxSemiTransPass == 1 && sampledStp >= 0.5) { discard; }\n"
                                 "			if (psxSemiTransPass == 2 && sampledStp < 0.5) { discard; }\n"
                                 "		}\n"
#endif
                                 GPU_RGBA_FRAGMENT_OUTPUT
                                 GPU_PSX_BLEND_APPLY
                                 "	}\n";

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
#define GTE_ORDER_DEPTH_ATTRIBUTE   "\tattribute float a_orderDepth;\n"
#define GTE_PERSPECTIVE_CORRECTION                                                                 \
	"\tgl_Position = Projection * vec4(a_position.xy, 0.0, 1.0);\n"                           \
	"\tgl_Position.z = 1.0 - a_orderDepth * (2.0 / 65535.0);\n"
#else
#define GTE_ORDER_DEPTH_ATTRIBUTE   ""
#define GTE_PERSPECTIVE_CORRECTION "\tgl_Position = Projection * vec4(a_position.xy, 0.0, 1.0);\n"
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
#define GTE_PAGE_CLUT_SETUP                                                                                     \
	"\t\tv_page_clut.x = fract(a_position.z / 16.0) * 1024.0;\n"                                           \
	"\t\tv_page_clut.y = floor(a_position.z / 16.0) * 256.0;\n"                                            \
	"\t\tv_page_clut.z = fract(a_position.w / 64.0) * 1024.0;\n"                                           \
	"\t\tv_page_clut.w = floor(mod(a_position.w, 65536.0) / 64.0);\n"
#else
#define GTE_PAGE_CLUT_SETUP                                                                                     \
	"\t\tv_page_clut.x = fract(a_position.z / 16.0) * 1024.0;\n"                                           \
	"\t\tv_page_clut.y = floor(a_position.z / 16.0) * 256.0;\n"                                            \
	"\t\tv_page_clut.z = fract(a_position.w / 64.0);\n"                                                    \
	"\t\tv_page_clut.w = floor(mod(a_position.w, 65536.0) / 64.0) / VRAM_HEIGHT;\n"                                            \
	"\t\tv_page_clut.xy += c_UVFudge;\n"                                                                   \
	"\t\tv_page_clut.zw += c_UVFudge;\n"
#endif

#define GTE_VERTEX_SHADER                                                                                          \
	"	attribute vec4 a_position;\n"                                                                                \
	"	attribute vec4 a_texcoord; // uv, color multiplier, dither\n"                                                \
		"	attribute vec4 a_color;\n"                                                                                   \
		"	attribute vec4 a_extra; // texcoord.xy ofs, unused.xy\n"                                                     \
		GTE_ORDER_DEPTH_ATTRIBUTE                                                                                         \
		"	uniform mat4 Projection;\n"                                                                                  \
	"	const vec2 c_UVFudge = vec2(0.00025, 0.00025);\n"                                                            \
	"	void main() {\n"                                                                                             \
	"		v_ditherCoord = a_position.xy;\n"                                                                           \
	"		v_texcoord = a_texcoord;\n"                                                                                 \
	"		v_texcoord.xy += a_extra.xy * 0.5;\n"                                                                       \
	"		v_color = a_color;\n"                                                                                       \
	"		v_color.xyz *= a_texcoord.z;\n"                                                                             \
	GTE_PAGE_CLUT_SETUP GTE_PERSPECTIVE_CORRECTION "		v_z = (gl_Position.z - 40.0) * 0.005;\n" \
	"	}\n"

internal int NativeRenderer_Shader_CheckShaderStatus(GLuint shader)
{
	char info[1024] = {0};
	GLint result;

	glGetShaderiv(shader, GL_COMPILE_STATUS, &result);

	if (result == GL_TRUE)
	{
		return 1;
	}

	glGetShaderInfoLog(shader, sizeof(info), NULL, info);
	if (info[0])
	{
		NATIVE_RENDERER_ERROR("%s\n", info);
		for (char *ch = info; *ch; ch++)
		{
			if (*ch == '\n' || *ch == '\r')
			{
				*ch = ' ';
			}
		}
		SysPrintf("pcsx-vita: shader compiler: %s\n", info);
	}

	return 0;
}

internal int NativeRenderer_Shader_CheckProgramStatus(GLuint program)
{
	char info[1024] = {0};
	GLint result;

	glGetProgramiv(program, GL_LINK_STATUS, &result);

	if (result == GL_TRUE)
	{
		return 1;
	}

	glGetProgramInfoLog(program, sizeof(info), NULL, info);
	if (info[0])
	{
		NATIVE_RENDERER_ERROR("%s\n", info);
		for (char *ch = info; *ch; ch++)
		{
			if (*ch == '\n' || *ch == '\r')
			{
				*ch = ' ';
			}
		}
		SysPrintf("pcsx-vita: shader linker: %s\n", info);
	}

	return 0;
}

internal ShaderID NativeRenderer_Shader_Compile(const char *source, bool isPsxShader, const char *fragmentDefines)
{
	const char *GLSL_HEADER_VERT =
#ifdef __EMSCRIPTEN__
	                               "	#version 300 es\n"
#elif defined(__vita__)
	                               "	#version 140\n"
	                               "	precision lowp  int;\n"
	                               "	precision highp float;\n"
#elif defined(PCSX_NATIVE_VITA_PATH)
	                               "	#version 140\n"
	                               "	#define varying   out\n"
	                               "	#define attribute in\n"
	                               "	#define texture2D texture\n"
	                               "	#define lowp\n"
	                               "	#define mediump\n"
	                               "	#define highp\n"
#else
	                               "	#version 140\n"
	                               "	precision lowp  int;\n"
	                               "	precision highp float;\n"
	                               "	#define varying   out\n"
	                               "	#define attribute in\n"
	                               "	#define texture2D texture\n"
#endif
	                               ;

	const char *GLSL_HEADER_FRAG =
#ifdef __EMSCRIPTEN__
	                               "	#version 300 es\n"
#elif defined(__vita__)
	                               "	#version 140\n"
	                               "	precision lowp  int;\n"
	                               "	precision highp float;\n"
#elif defined(PCSX_NATIVE_VITA_PATH)
	                               "	#version 140\n"
	                               "	#define varying     in\n"
	                               "	#define texture2D   texture\n"
	                               "	#define lowp\n"
	                               "	#define mediump\n"
	                               "	#define highp\n"
	                               "	out vec4 fragColor;\n"
	                               "	#define gl_FragColor fragColor\n"
#else
	                               "	#version 140\n"
	                               "	precision lowp  int;\n"
	                               "	precision highp float;\n"
	                               "	#define varying     in\n"
	                               "	#define texture2D   texture\n"
	                               "	out vec4 fragColor;\n"
#ifdef __EMSCRIPTEN__
	                               "	#define gl_FragColor fragColor\n"
#endif
#endif
	                               ;
	char extra_vs_defines[1024];
	char extra_fs_defines[1024];
	char vramHeightDefine[64];
	extra_vs_defines[0] = 0;
	extra_fs_defines[0] = 0;
	SDL_snprintf(vramHeightDefine, sizeof(vramHeightDefine), "#define VRAM_HEIGHT %u.0\n", VRAM_HEIGHT);

	strcat(extra_vs_defines, "#define VERTEX\n");
	strcat(extra_fs_defines, "#define FRAGMENT\n");
	strcat(extra_vs_defines, vramHeightDefine);
	strcat(extra_fs_defines, vramHeightDefine);
	if (fragmentDefines != NULL)
	{
		strcat(extra_fs_defines, fragmentDefines);
	}
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	strcat(extra_fs_defines, "#define VITA_NEAREST_ONLY\n");
#endif
	if (g_cfg_bilinearFiltering)
	{
		strcat(extra_fs_defines, "#define BILINEAR_FILTER\n");
	}

	const char *vs_list_psx[] = {GLSL_HEADER_VERT, extra_vs_defines, gpu_shader_common, GTE_VERTEX_SHADER};
	const char *fs_list_psx[] = {GLSL_HEADER_FRAG, extra_fs_defines, gpu_shader_common, GPU_DITHERING, source};
	const char *vs_list_src[] = {
	    GLSL_HEADER_VERT,
	    extra_vs_defines,
	    source,
	};
	const char *fs_list_src[] = {GLSL_HEADER_FRAG, extra_fs_defines, source};

	const char **vs_list = isPsxShader ? vs_list_psx : vs_list_src;
	const char **fs_list = isPsxShader ? fs_list_psx : fs_list_src;
	const int vs_list_cnt = isPsxShader ? 4 : 3;
	const int fs_list_cnt = isPsxShader ? 5 : 3;

	GLuint program = glCreateProgram();
	if (program == 0)
	{
		NATIVE_RENDERER_ERROR("%s\n", "Failed to create Shader program!");
		SysPrintf("pcsx-vita: failed to create shader program\n");
		return 0;
	}

	{
		GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
		if (vertexShader == 0)
		{
			glDeleteProgram(program);
			NATIVE_RENDERER_ERROR("%s\n", "Failed to create Vertex Shader!");
			SysPrintf("pcsx-vita: failed to create vertex shader\n");
			return 0;
		}
		glShaderSource(vertexShader, vs_list_cnt, vs_list, NULL);
		glCompileShader(vertexShader);

		if (NativeRenderer_Shader_CheckShaderStatus(vertexShader) == 0)
		{
			NATIVE_RENDERER_ERROR("%s\n", "Failed to compile Vertex Shader!");
			SysPrintf("pcsx-vita: vertex shader compilation failed\n");
			glDeleteShader(vertexShader);
			glDeleteProgram(program);
			return 0;
		}

		glAttachShader(program, vertexShader);
		glDeleteShader(vertexShader);
	}

	{
		GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
		if (fragmentShader == 0)
		{
			glDeleteProgram(program);
			NATIVE_RENDERER_ERROR("%s\n", "Failed to create Fragment Shader!");
			SysPrintf("pcsx-vita: failed to create fragment shader\n");
			return 0;
		}
		glShaderSource(fragmentShader, fs_list_cnt, fs_list, NULL);
		glCompileShader(fragmentShader);

		if (NativeRenderer_Shader_CheckShaderStatus(fragmentShader) == 0)
		{
			NATIVE_RENDERER_ERROR("%s\n", "Failed to compile Fragment Shader!");
			SysPrintf("pcsx-vita: fragment shader compilation failed\n");
			glDeleteShader(fragmentShader);
			glDeleteProgram(program);
			return 0;
		}

		glAttachShader(program, fragmentShader);
		glDeleteShader(fragmentShader);
	}

	glBindAttribLocation(program, a_position, "a_position");
	glBindAttribLocation(program, a_texcoord, "a_texcoord");
	glBindAttribLocation(program, a_color, "a_color");
	glBindAttribLocation(program, a_extra, "a_extra");
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	glBindAttribLocation(program, a_order_depth, "a_orderDepth");
#endif

	glLinkProgram(program);
	if (NativeRenderer_Shader_CheckProgramStatus(program) == 0)
	{
		NATIVE_RENDERER_ERROR("%s\n", "Failed to link Shader!");
		SysPrintf("pcsx-vita: shader program link failed\n");
		glDeleteProgram(program);
		return 0;
	}

	GLint textureSampler = 0;
	GLint presentLutSampler = 2;
	glUseProgram(program);
	glUniform1iv(glGetUniformLocation(program, "s_texture"), 1, &textureSampler);
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	GLint lutSampler = 1;
	glUniform1iv(glGetUniformLocation(program, "s_rgLut"), 1, &lutSampler);
#endif
	glUniform1iv(glGetUniformLocation(program, "s_presentLut"), 1, &presentLutSampler);
	glUseProgram(0);

	return program;
}

// --------------------------------------------------------------------------------------------

internal void NativeRenderer_GenerateCommonTextures(void)
{
	u32 whitePixelData = 0xFFFFFFFF;

	glGenTextures(1, &s_whiteTexture);
	{
		glBindTexture(GL_TEXTURE_2D, s_whiteTexture);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &whitePixelData);

		glBindTexture(GL_TEXTURE_2D, 0);
	}

#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	glGenTextures(1, &s_rgLutTexture);
	{
		// Texture unit 1 is reserved for the immutable PSX color lookup table.
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, s_rgLutTexture);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LUT_WIDTH, LUT_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, &rgLUT);

		glActiveTexture(GL_TEXTURE0);
	}
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	for (u16 y = 0; y < LUT_HEIGHT; y++)
	{
		u8 *row = rgLUT + y * (LUT_WIDTH * 4);
		for (u16 x = 0; x < LUT_WIDTH; x++)
		{
			const u16 c = (y << 8) | x;
			const u8 r5 = (u8)(c & 31);
			const u8 g5 = (u8)((c >> 5) & 31);
			const u8 b5 = (u8)((c >> 10) & 31);
			u8 *pixel = row + x * 4;
			pixel[0] = (u8)((r5 << 3) | (r5 >> 2));
			pixel[1] = (u8)((g5 << 3) | (g5 >> 2));
			pixel[2] = (u8)((b5 << 3) | (b5 >> 2));
			pixel[3] = (c & 0x8000) ? 255 : 0;
		}
	}

	glGenTextures(1, &s_presentLutTexture);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, s_presentLutTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LUT_WIDTH, LUT_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, &rgLUT);
	glActiveTexture(GL_TEXTURE0);
#endif
}

internal int NativeRenderer_CompilePSXShader(GTEShader *sh, const char *source, const char *fragmentDefines)
{
	sh->shader = NativeRenderer_Shader_Compile(source, true, fragmentDefines);
	if (sh->shader == 0)
	{
		return 0;
	}

	sh->bilinearFilterLoc = glGetUniformLocation(sh->shader, "bilinearFilter");
	sh->projectionLoc = glGetUniformLocation(sh->shader, "Projection");
	sh->texelSizeLoc = glGetUniformLocation(sh->shader, "texelSize");
	sh->texLoc = glGetUniformLocation(sh->shader, "s_texture");
	sh->lutLoc = glGetUniformLocation(sh->shader, "s_rgLut");
	sh->textureWindowLoc = glGetUniformLocation(sh->shader, "s_textureWindow");
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	sh->psxSemiTransPassLoc = glGetUniformLocation(sh->shader, "psxSemiTransPass");
	sh->psxDitherEnabledLoc = glGetUniformLocation(sh->shader, "psxDitherEnabled");
#endif
	sh->psxDrawMaskSetLoc = glGetUniformLocation(sh->shader, "psxDrawMaskSet");
	sh->psxTextureOutputStpLoc = glGetUniformLocation(sh->shader, "psxTextureOutputStp");
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	sh->source = source;
	sh->defines = fragmentDefines;
	NativeRenderer_ProgramLabel(sh->label, sizeof(sh->label), source, fragmentDefines);
#endif
	return 1;
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
/* A texture window with a mask costs ~130 ALU operations per fragment; with
 * mask 0 it is the identity, so programs leave it out and get a twin with it
 * when a draw first needs one. */
internal GTEShader *NativeRenderer_WindowedShader(GTEShader *base)
{
	if (base->windowed == NULL && base->source != NULL)
	{
		GTEShader *windowed = (GTEShader *)SDL_calloc(1, sizeof(*windowed));
		char defines[256];
		SDL_snprintf(defines, sizeof(defines), "#define PSX_TEXTURE_WINDOW\n%s", base->defines ? base->defines : "");
		if (windowed == NULL || !NativeRenderer_CompilePSXShader(windowed, base->source, defines))
		{
			SDL_free(windowed);
			return base;
		}
		windowed->defines = NULL;
		base->windowed = windowed;
	}
	return base->windowed ? (GTEShader *)base->windowed : base;
}

internal void NativeRenderer_DestroyPSXShader(GTEShader *sh)
{
	if (sh->windowed != NULL)
	{
		GTEShader *windowed = (GTEShader *)sh->windowed;
		if (windowed->shader != 0)
			glDeleteProgram(windowed->shader);
		SDL_free(windowed);
	}
	if (sh->shader != 0)
		glDeleteProgram(sh->shader);
	SDL_memset(sh, 0, sizeof(*sh));
}
#endif

internal int NativeRenderer_InitialisePSXShaders(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	local_persist const char *variantDefines[NATIVE_PSX_SHADER_VARIANT_COUNT] = {
	    "",
	    "#define PSX_BLEND_AVERAGE\n",
	    "#define PSX_BLEND_QUARTER\n",
	    "#define PSX_PASS_NON_STP\n",
	    "#define PSX_PASS_STP\n",
	    "#define PSX_PASS_STP\n#define PSX_BLEND_AVERAGE\n",
	    "#define PSX_PASS_STP\n#define PSX_BLEND_QUARTER\n",
	    "#define PSX_MIXED_AVERAGE\n",
	    "#define PSX_MIXED_ADD\n",
	    "#define PSX_MIXED_QUARTER\n",
	};
	local_persist const char *fullyOpaqueVariantDefines[3] = {
	    "#define PSX_FULLY_OPAQUE\n",
	    "#define PSX_FULLY_OPAQUE\n#define PSX_BLEND_AVERAGE\n",
	    "#define PSX_FULLY_OPAQUE\n#define PSX_BLEND_QUARTER\n",
	};
	const char *sources[4] = {gte_shader_4, gte_shader_8, gte_shader_16, gte_shader_32_rgba};

	for (int format = TF_4_BIT; format <= TF_32_BIT_RGBA; format++)
	{
		const int variantCount = format == TF_32_BIT_RGBA ? NATIVE_PSX_SHADER_STP_QUARTER + 1 : NATIVE_PSX_SHADER_VARIANT_COUNT;
		for (int variant = 0; variant < variantCount; variant++)
		{
			if (!NativeRenderer_CompilePSXShader(&s_gteShaderVariants[format][variant], sources[format], variantDefines[variant]))
			{
				return 0;
			}
		}
	}
	for (int variant = 0; variant < NATIVE_PSX_SHADER_VARIANT_COUNT; variant++)
	{
		if (!NativeRenderer_CompilePSXShader(&s_gteCachedP4ShaderVariants[variant], gte_shader_cached_p4, variantDefines[variant]))
		{
			return 0;
		}
	}
	for (int variant = 0; variant < 3; variant++)
	{
		if (!NativeRenderer_CompilePSXShader(&s_gteCachedP4FullyOpaqueShaderVariants[variant], gte_shader_cached_p4,
		                                     fullyOpaqueVariantDefines[variant]))
		{
			return 0;
		}
	}
	for (int format = TF_4_BIT; format <= TF_8_BIT; format++)
	{
		for (int variant = 0; variant < 3; variant++)
		{
			if (!NativeRenderer_CompilePSXShader(&s_gteFullyOpaqueShaderVariants[format][variant], sources[format],
			                                     fullyOpaqueVariantDefines[variant]))
			{
				return 0;
			}
		}
	}
	for (int variant = 0; variant < 3; variant++)
	{
		if (!NativeRenderer_CompilePSXShader(&s_gteUntexturedShaderVariants[variant], gte_shader_untextured,
		                                     variantDefines[variant]))
		{
			return 0;
		}
	}
#else
	if (!NativeRenderer_CompilePSXShader(&s_gteShader4, gte_shader_4, NULL) ||
	    !NativeRenderer_CompilePSXShader(&s_gteShader8, gte_shader_8, NULL) ||
	    !NativeRenderer_CompilePSXShader(&s_gteShader16, gte_shader_16, NULL) ||
	    !NativeRenderer_CompilePSXShader(&s_gteShader4SuperTurbo, gte_shader_4, "#define SUPER_TURBO_TINT\n") ||
	    !NativeRenderer_CompilePSXShader(&s_gteShader8SuperTurbo, gte_shader_8, "#define SUPER_TURBO_TINT\n") ||
	    !NativeRenderer_CompilePSXShader(&s_gteShader16SuperTurbo, gte_shader_16, "#define SUPER_TURBO_TINT\n") ||
	    !NativeRenderer_CompilePSXShader(&s_gteShader32Rgba, gte_shader_32_rgba, NULL))
	{
		return 0;
	}
#endif
	return 1;
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
global_variable const char *ctr_pack_shader = "#ifdef VERTEX\n"
                                              "attribute vec2 a_position;\n"
                                              "varying vec2 v_uv;\n"
                                              "uniform int flipY;\n"
                                              "uniform vec2 uvScale;\n"
                                              "void main() {\n"
                                              "    v_uv = a_position * 0.5 + 0.5;\n"
                                              "    if (flipY != 0) { v_uv.y = 1.0 - v_uv.y; }\n"
                                              "    v_uv *= uvScale;\n"
                                              "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
                                              "}\n"
                                              "#endif\n"
                                              "#ifdef FRAGMENT\n"
                                              "varying vec2 v_uv;\n"
                                              "uniform sampler2D s_src;\n"
                                              "void main() {\n"
                                              "    vec4 c = texture2D(s_src, v_uv);\n"
                                              "    vec3 color5 = floor(c.rgb * (255.0 / 8.0) + 0.001);\n"
                                              "    float lowByte = color5.r + mod(color5.g, 8.0) * 32.0;\n"
                                              "    float highByte = floor(color5.g / 8.0) + color5.b * 4.0 + step(0.5, c.a) * 128.0;\n"
                                              "    gl_FragColor = vec4(lowByte, highByte, 0.0, 0.0) / 255.0;\n"
                                              "}\n"
                                              "#endif\n";

global_variable const char *ctr_present_vram_shader = "#ifdef VERTEX\n"
                                                      "attribute vec2 a_position;\n"
                                                      "varying vec2 v_uv;\n"
                                                      "uniform vec4 sourceRect;\n"
                                                      "void main() {\n"
                                                      "    vec2 screenUV = a_position * 0.5 + 0.5;\n"
                                                      "    vec2 sourcePixel = sourceRect.xy + vec2(screenUV.x, 1.0 - screenUV.y) * sourceRect.zw;\n"
                                                      "    v_uv = sourcePixel / vec2(1024.0, VRAM_HEIGHT);\n"
                                                      "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
                                                      "}\n"
                                                      "#endif\n"
                                                      "#ifdef FRAGMENT\n"
                                                      "varying vec2 v_uv;\n"
                                                      "uniform sampler2D s_texture;\n"
                                                      "uniform sampler2D s_presentLut;\n"
                                                      "const vec2 c_LUTTexel = vec2(1.0 / 256.0, 1.0 / 256.0);\n"
                                                      "void main() {\n"
                                                      "    vec2 packedRg = texture2D(s_texture, v_uv).rg;\n"
                                                      "    gl_FragColor = texture2D(s_presentLut, packedRg - c_LUTTexel * 0.0001);\n"
                                                      "}\n"
                                                      "#endif\n";
#else
// NOTE(aalhendi): GPU VRAM pack. Samples an RGBA render texture and writes PS1 5:5:5:1 pixels into RG8 VRAM, low byte in R and high byte in G. NEAREST sampling and integer channel shifts preserve the packed PS1 pixel value.
global_variable const char *ctr_pack_shader = "#ifdef VERTEX\n"
                                              "attribute vec2 a_position;\n"
                                              "varying vec2 v_uv;\n"
                                              "uniform int flipY;\n"
                                              "void main() {\n"
                                              "	v_uv = a_position * 0.5 + 0.5;\n"
                                              "	if (flipY != 0) { v_uv.y = 1.0 - v_uv.y; }\n"
                                              "	gl_Position = vec4(a_position, 0.0, 1.0);\n"
                                              "}\n"
                                              "#endif\n"
                                              "#ifdef FRAGMENT\n"
                                              "varying vec2 v_uv;\n"
                                              "uniform sampler2D s_src;\n"
                                              "void main() {\n"
                                              "	ivec4 c = ivec4(texture2D(s_src, v_uv) * 255.0 + 0.5);\n"
                                              "	int px16 = (c.r >> 3) | ((c.g >> 3) << 5) | ((c.b >> 3) << 10) | ((c.a >> 7) << 15);\n"
                                              "	gl_FragColor = vec4(float(px16 & 0xFF) / 255.0, float((px16 >> 8) & 0xFF) / 255.0, 0.0, 0.0);\n"
                                              "}\n"
                                              "#endif\n";

// NOTE(aalhendi): Expand packed VRAM without losing bit 15. Internal render targets carry that PS1 STP/mask bit in alpha so packing them is lossless.
global_variable const char *ctr_present_vram_shader = "#ifdef VERTEX\n"
                                                      "attribute vec2 a_position;\n"
                                                      "varying vec2 v_uv;\n"
                                                      "uniform vec4 sourceRect;\n"
                                                      "void main() {\n"
                                                      "\tvec2 screenUV = a_position * 0.5 + 0.5;\n"
                                                      "\tvec2 sourcePixel = sourceRect.xy + vec2(screenUV.x, 1.0 - screenUV.y) * sourceRect.zw;\n"
                                                      "\tv_uv = sourcePixel / vec2(1024.0, VRAM_HEIGHT);\n"
                                                      "\tgl_Position = vec4(a_position, 0.0, 1.0);\n"
                                                      "}\n"
                                                      "#endif\n"
                                                      "#ifdef FRAGMENT\n"
                                                      "varying vec2 v_uv;\n"
                                                      "uniform sampler2D s_texture;\n"
                                                      "void main() {\n"
                                                      "\tivec2 packedBytes = ivec2(texture2D(s_texture, v_uv).rg * 255.0 + 0.5);\n"
                                                      "\tint pixel = packedBytes.r | (packedBytes.g << 8);\n"
                                                      "\tivec3 color5 = ivec3(pixel & 31, (pixel >> 5) & 31, (pixel >> 10) & 31);\n"
                                                      "\tivec3 color8 = (color5 << 3) | (color5 >> 2);\n"
                                                      "\tfloat stp = float((pixel >> 15) & 1);\n"
                                                      "\tgl_FragColor = vec4(vec3(color8) / 255.0, stp);\n"
                                                      "}\n"
                                                      "#endif\n";
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
global_variable const char *ctr_present_rgba_shader = "#ifdef VERTEX\n"
                                                       "attribute vec2 a_position;\n"
                                                       "varying vec2 v_uv;\n"
                                                       "void main() {\n"
                                                       "    v_uv = a_position * 0.5 + 0.5;\n"
                                                       "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
                                                       "}\n"
                                                       "#endif\n"
                                                       "#ifdef FRAGMENT\n"
                                                       "varying vec2 v_uv;\n"
                                                       "uniform sampler2D s_src;\n"
                                                       "uniform float flipY;\n"
                                                       "uniform vec2 uvScale;\n"
                                                       "void main() {\n"
                                                       "    gl_FragColor = texture2D(s_src, vec2(v_uv.x, mix(v_uv.y, 1.0 - v_uv.y, flipY)) * uvScale);\n"
                                                       "}\n"
                                                       "#endif\n";
#else
global_variable const char *ctr_present_rgba_shader = "#ifdef VERTEX\n"
                                                       "attribute vec2 a_position;\n"
                                                       "varying vec2 v_uv;\n"
                                                       "void main() {\n"
                                                       "    v_uv = a_position * 0.5 + 0.5;\n"
                                                       "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
                                                       "}\n"
                                                       "#endif\n"
                                                       "#ifdef FRAGMENT\n"
                                                       "varying vec2 v_uv;\n"
                                                       "uniform sampler2D s_src;\n"
                                                       "uniform float flipY;\n"
                                                       "uniform vec2 texelSize;\n"
                                                       "uniform int fxaaEnabled;\n"
                                                       "float fxaaLuma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
                                                       "vec4 fxaaSample(vec2 uv) {\n"
                                                       "    vec4 center = texture2D(s_src, uv);\n"
                                                       "    vec3 nw = texture2D(s_src, uv + texelSize * vec2(-1.0, -1.0)).rgb;\n"
                                                       "    vec3 ne = texture2D(s_src, uv + texelSize * vec2( 1.0, -1.0)).rgb;\n"
                                                       "    vec3 sw = texture2D(s_src, uv + texelSize * vec2(-1.0,  1.0)).rgb;\n"
                                                       "    vec3 se = texture2D(s_src, uv + texelSize * vec2( 1.0,  1.0)).rgb;\n"
                                                       "    float lNW = fxaaLuma(nw), lNE = fxaaLuma(ne), lSW = fxaaLuma(sw), lSE = fxaaLuma(se), lM = fxaaLuma(center.rgb);\n"
                                                       "    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));\n"
                                                       "    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
                                                       "    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));\n"
                                                       "    float reduce = max((lNW + lNE + lSW + lSE) * 0.0078125, 0.0009765625);\n"
                                                       "    float invMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n"
                                                       "    dir = clamp(dir * invMin, vec2(-8.0), vec2(8.0)) * texelSize;\n"
                                                       "    vec3 a = 0.5 * (texture2D(s_src, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture2D(s_src, uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
                                                       "    vec3 b = a * 0.5 + 0.25 * (texture2D(s_src, uv + dir * -0.5).rgb + texture2D(s_src, uv + dir * 0.5).rgb);\n"
                                                       "    float lB = fxaaLuma(b);\n"
                                                       "    return vec4((lB < lMin || lB > lMax) ? a : b, center.a);\n"
                                                       "}\n"
                                                       "void main() {\n"
                                                       "    vec2 uv = vec2(v_uv.x, mix(v_uv.y, 1.0 - v_uv.y, flipY));\n"
                                                       "    gl_FragColor = (fxaaEnabled != 0) ? fxaaSample(uv) : texture2D(s_src, uv);\n"
                                                       "}\n"
                                                       "#endif\n";
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
global_variable const char *ctr_vram_upload_shader = "#ifdef VERTEX\n"
                                                    "attribute vec2 a_position;\n"
                                                    "uniform vec4 targetRect;\n"
                                                    "uniform vec4 sourceRect;\n"
                                                    "varying vec2 v_uv;\n"
                                                    "void main() {\n"
                                                    "    vec2 unit = a_position * 0.5 + 0.5;\n"
                                                    "    v_uv = sourceRect.xy + unit * sourceRect.zw;\n"
                                                    "    gl_Position = vec4(targetRect.xy + unit * targetRect.zw, 0.0, 1.0);\n"
                                                    "}\n"
                                                    "#endif\n"
                                                    "#ifdef FRAGMENT\n"
                                                    "uniform sampler2D s_src;\n"
                                                    "varying vec2 v_uv;\n"
                                                    "void main() { gl_FragColor = texture2D(s_src, v_uv); }\n"
                                                    "#endif\n";
#endif

internal int NativeRenderer_InitVRAMPipelines(void)
{
	local_persist const float quad[12] = {-1.f, -1.f, -1.f, 1.f, 1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f, -1.f};

	s_packShader = NativeRenderer_Shader_Compile(ctr_pack_shader, false, NULL);
	if (s_packShader == 0)
	{
		return 0;
	}
	glUseProgram(s_packShader);
	const GLint packSrcLoc = glGetUniformLocation(s_packShader, "s_src");
	s_packFlipYLoc = glGetUniformLocation(s_packShader, "flipY");
	s_packUvScaleLoc = glGetUniformLocation(s_packShader, "uvScale");
	glUniform1i(packSrcLoc, 0);
	if (s_packUvScaleLoc >= 0)
		glUniform2f(s_packUvScaleLoc, 1.0f, 1.0f);
	glUseProgram(0);

	s_presentVramShader = NativeRenderer_Shader_Compile(ctr_present_vram_shader, false, NULL);
	if (s_presentVramShader == 0)
	{
		return 0;
	}
	s_presentVramSourceRectLoc = glGetUniformLocation(s_presentVramShader, "sourceRect");

	s_presentRgbaShader = NativeRenderer_Shader_Compile(ctr_present_rgba_shader, false, NULL);
	if (s_presentRgbaShader == 0)
	{
		return 0;
	}
	glUseProgram(s_presentRgbaShader);
	const GLint presentRgbaSrcLoc = glGetUniformLocation(s_presentRgbaShader, "s_src");
	s_presentRgbaFlipYLoc = glGetUniformLocation(s_presentRgbaShader, "flipY");
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	s_presentRgbaTexelSizeLoc = glGetUniformLocation(s_presentRgbaShader, "texelSize");
	s_presentRgbaFxaaLoc = glGetUniformLocation(s_presentRgbaShader, "fxaaEnabled");
#endif
	glUniform1i(presentRgbaSrcLoc, 0);
	glUniform1f(s_presentRgbaFlipYLoc, 0.0f);
	s_presentRgbaUvScaleLoc = glGetUniformLocation(s_presentRgbaShader, "uvScale");
	if (s_presentRgbaUvScaleLoc >= 0)
		glUniform2f(s_presentRgbaUvScaleLoc, 1.0f, 1.0f);
	glUseProgram(0);

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	s_vramUploadShader = NativeRenderer_Shader_Compile(ctr_vram_upload_shader, false, NULL);
	if (s_vramUploadShader == 0)
	{
		return 0;
	}
	glUseProgram(s_vramUploadShader);
	glUniform1i(glGetUniformLocation(s_vramUploadShader, "s_src"), 0);
	s_vramUploadRectLoc = glGetUniformLocation(s_vramUploadShader, "targetRect");
	s_vramUploadSourceRectLoc = glGetUniformLocation(s_vramUploadShader, "sourceRect");
	glUseProgram(0);
#endif

	glGenVertexArrays(1, &s_vramQuadVAO);
	glGenBuffers(1, &s_vramQuadVBO);
	glBindVertexArray(s_vramQuadVAO);
	glBindBuffer(GL_ARRAY_BUFFER, s_vramQuadVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(a_position);
	glVertexAttribPointer(a_position, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	return 1;
}

internal void NativeRenderer_InitRG8LUT(void)
{
	for (u16 y = 0; y < LUT_HEIGHT; y++)
	{
		u8 *row = rgLUT + y * (LUT_WIDTH * 4);
		for (u16 x = 0; x < LUT_WIDTH; x++)
		{
			const u16 c = (y << 8) | x;
			u8 *pixel = row + x * 4;
			pixel[0] = (u8)((c & 31) << 3);
			pixel[1] = (u8)(((c >> 5) & 31) << 3);
			pixel[2] = (u8)(((c >> 10) & 31) << 3);
			pixel[3] = (u8)(((c >> 15) & 1) << 7);
		}
	}
}

int NativeRenderer_InitialisePSX(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	s_mainLoaded = s_mainDirty = s_offscreenLoaded = s_offscreenDirty = 0;
	s_previousOffscreenState = 0;
	s_prevMainValid = 0;
#endif
	SDL_memset(s_vram.cpuPixels, 0, sizeof(s_vram.cpuPixels));
	s_vram.cpuDirtyRects[0].x = 0;
	s_vram.cpuDirtyRects[0].y = 0;
	s_vram.cpuDirtyRects[0].w = VRAM_WIDTH;
	s_vram.cpuDirtyRects[0].h = VRAM_HEIGHT;
	s_vram.cpuDirtyRectCount = 1;
	SDL_memset(s_vram.gpuNewerTiles, 0, sizeof(s_vram.gpuNewerTiles));
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	s_drawnRectCount = 0;
	SDL_memset(s_gpuNewerPageFrame, 0, sizeof(s_gpuNewerPageFrame));
	s_staleP4Sync.w = 0;
#endif
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	SDL_memset(s_p4Cache, 0, sizeof(s_p4Cache));
	s_p4VersionSerial = 1;
	for (int page = 0; page < (int)SDL_arraysize(s_p4PageVersion); page++)
		s_p4PageVersion[page] = 1;
	for (int clut = 0; clut < (int)SDL_arraysize(s_p4ClutVersion); clut++)
		s_p4ClutVersion[clut] = 1;
#if defined(__vita__)
	for (int page = 0; page < (int)SDL_arraysize(s_p4PageData); page++)
		vglLazyFree(s_p4PageData[page].data);
	SDL_memset(s_p4PageData, 0, sizeof(s_p4PageData));
#endif
	s_p4CacheUseCounter = 0;
	s_p4FrameSerial = 1;
	s_p4LastLookupIndex = -1;
	s_p4LastLookupKey = 0xffffffffu;
	SDL_memset(s_paletteCacheGeneration, 0, sizeof(s_paletteCacheGeneration));
	SDL_memset(s_paletteCacheProperties, 0, sizeof(s_paletteCacheProperties));
	for (int row = 0; row < VRAM_HEIGHT; row++)
	{
		s_paletteRowGeneration[row] = 1;
	}
#endif
    NativeRenderer_InitRG8LUT();
    NativeRenderer_GenerateCommonTextures();
	if (!NativeRenderer_InitialisePSXShaders() || !NativeRenderer_InitVRAMPipelines())
	{
		return 0;
	}

	glDepthFunc(GL_LEQUAL);
	glEnable(GL_STENCIL_TEST);
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	glBlendColor(0.5f, 0.5f, 0.5f, 0.25f);
#endif

	// Main and offscreen draws share one explicit render-target contract. The main target stays at CTR's logical display size; host scaling is deferred to presentation.
	NativeRenderer_InitRenderTarget(&s_mainRenderTarget);
	NativeRenderer_InitRenderTarget(&s_offscreenRenderTarget);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	NativeRenderer_InitRenderTarget(&s_prevMainRenderTarget);
#endif

	// gen VRAM texture (single, persistent - mirrors PS1's single 1MB VRAM)
	{
		glGenTextures(1, &s_vram.texture);

		glBindTexture(GL_TEXTURE_2D, s_vram.texture);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

		// set storage size
		glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_WIDTH, VRAM_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE, NULL);

		glBindTexture(GL_TEXTURE_2D, 0);

		// VRAM framebuffer for offscreen blitting to VRAM
		glGenFramebuffers(1, &s_glVramFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, s_glVramFramebuffer);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_vram.texture, 0);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
			/* Uploads, packs and fills: axis-aligned quads, no multisampling. */
			NativeRenderer_SetTargetMultisample(s_glVramFramebuffer, 0);
#endif
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
#else
			GLuint depthstencil;
			glGenRenderbuffers(1, &depthstencil);
			glBindRenderbuffer(GL_RENDERBUFFER, depthstencil);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, VRAM_WIDTH, VRAM_HEIGHT);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthstencil);
#endif

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	// gen vertex buffer and index buffer
	{
		int i;

		glGenBuffers(MAX_NUM_VERTEX_BUFFERS, s_glVertexBuffer);
		glGenVertexArrays(MAX_NUM_VERTEX_BUFFERS, s_glVertexArray);
		for (i = 0; i < MAX_NUM_VERTEX_BUFFERS; i++)
		{
			glBindVertexArray(s_glVertexArray[i]);
			glBindBuffer(GL_ARRAY_BUFFER, s_glVertexBuffer[i]);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
			// Initialise vitaGL's VBO metadata. Each submitted batch later replaces this pointer with mapped scratch storage, without a copy.
			glBufferData(GL_ARRAY_BUFFER, sizeof(GrVertex), NULL, GL_STREAM_DRAW);
#else
			glBufferData(GL_ARRAY_BUFFER, sizeof(GrVertex) * MAX_VERTEX_BUFFER_SIZE, NULL, GL_DYNAMIC_DRAW);
#endif
			glEnableVertexAttribArray(a_position);
			glEnableVertexAttribArray(a_texcoord);
			glEnableVertexAttribArray(a_color);
			glEnableVertexAttribArray(a_extra);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
			glEnableVertexAttribArray(a_order_depth);
#endif

			glVertexAttribPointer(a_position, 4, GL_SHORT, GL_FALSE, sizeof(GrVertex), &((GrVertex *)NULL)->x);
			glVertexAttribPointer(a_texcoord, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GrVertex), &((GrVertex *)NULL)->u);
			glVertexAttribPointer(a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GrVertex), &((GrVertex *)NULL)->r);
			glVertexAttribPointer(a_extra, 4, GL_BYTE, GL_FALSE, sizeof(GrVertex), &((GrVertex *)NULL)->tcx);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
			glVertexAttribPointer(a_order_depth, 1, GL_UNSIGNED_SHORT, GL_FALSE, sizeof(GrVertex), &((GrVertex *)NULL)->orderDepth);
#endif
		}
		glBindVertexArray(0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	NativeRenderer_ResetDevice();
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (s_vitaVramTransferPixels == NULL)
	{
		s_vitaVramTransferPixels = (u8 *)vglMemalign(64, NATIVE_VITA_VRAM_TRANSFER_BYTES);
	}
	if (s_vitaVramTransferPixels == NULL)
	{
		SysPrintf("pcsx-vita: failed to allocate mapped VRAM readback buffer (%u bytes)\n", (unsigned)NATIVE_VITA_VRAM_TRANSFER_BYTES);
		return 0;
	}
#endif

	return 1;
}

internal void NativeRenderer_Ortho2D(float left, float right, float bottom, float top, float znear, float zfar)
{
	float a = 2.0f / (right - left);
	float b = 2.0f / (top - bottom);
	float c = 2.0f / (znear - zfar);

	float x = (left + right) / (left - right);
	float y = (bottom + top) / (bottom - top);

	// -1..1
	float z = (znear + zfar) / (znear - zfar);

	float ortho[16] = {a, 0, 0, 0, 0, b, 0, 0, 0, 0, c, 0, x, y, z, 1};

	glUniformMatrix4fv(u_projectionLoc, 1, GL_FALSE, ortho);
}

void NativeRenderer_SetupClipMode(const RECT16 *rect, const DISPENV *displayEnv, int enable)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	(void)displayEnv;
	NativeRenderer_SetScissorState(enable != 0);
	if (enable)
		NativeRenderer_SetScissorRectCached(0, 0,
			rect->w > 0 && rect->h > 0 ? s_mainRenderTarget.width : 0,
			rect->w > 0 && rect->h > 0 ? s_mainRenderTarget.height : 0);
#else
	if ((displayEnv->disp.w <= 0) || (displayEnv->disp.h <= 0))
	{
		NativeRenderer_SetScissorState(0);
		return;
	}

	if ((rect->w <= 0) || (rect->h <= 0))
	{
		// NOTE(aalhendi): Retail draw-area commands define inclusive corners. Collapsed areas clip all pixels; GL scissor rejects negative sizes.
		NativeRenderer_SetScissorState(enable != 0);
		if (enable)
		{
			NativeRenderer_SetScissorRectCached(0, 0, 0, 0);
		}
		return;
	}

	// [A] isinterlaced dirty hack for widescreen
	const bool scissorOn = enable && (displayEnv->isinter || (rect->x - displayEnv->disp.x > 0 || rect->y - displayEnv->disp.y > 0 ||
	                                                          rect->w < displayEnv->disp.w || rect->h < displayEnv->disp.h));

	NativeRenderer_SetScissorState(scissorOn);

	if (!scissorOn)
	{
		return;
	}

	const int displayX = displayEnv->disp.x;
	const int displayY = displayEnv->disp.y;
	const int displayW = displayEnv->disp.w;
	const int displayH = displayEnv->disp.h;
	const int clipRight = rect->x + rect->w;
	const int clipBottom = rect->y + rect->h;
	const int displayRight = displayX + displayW;
	const int displayBottom = displayY + displayH;
	const int overlapX = rect->x > displayX ? rect->x : displayX;
	const int overlapY = rect->y > displayY ? rect->y : displayY;
	const int overlapRight = clipRight < displayRight ? clipRight : displayRight;
	const int overlapBottom = clipBottom < displayBottom ? clipBottom : displayBottom;

	if ((overlapRight <= overlapX) || (overlapBottom <= overlapY))
	{
		NativeRenderer_SetScissorRectCached(0, 0, 0, 0);
		return;
	}

	const int physicalW = s_mainRenderTarget.width > 0 ? s_mainRenderTarget.width : displayW;
	const int physicalH = s_mainRenderTarget.height > 0 ? s_mainRenderTarget.height : displayH;
	const int relLeft = overlapX - displayX;
	const int relRight = overlapRight - displayX;
	const int relTop = overlapY - displayY;
	const int relBottom = overlapBottom - displayY;
	const int scissorX = relLeft * physicalW / displayW;
	const int scissorRight = (relRight * physicalW + displayW - 1) / displayW;
	const int scissorY = (displayH - relBottom) * physicalH / displayH;
	const int scissorTop = ((displayH - relTop) * physicalH + displayH - 1) / displayH;

	NativeRenderer_SetScissorRectCached(scissorX, scissorY, scissorRight - scissorX, scissorTop - scissorY);
#endif
}

internal void NativeRenderer_SetShader(const ShaderID shader)
{
	if (s_previousShader != shader)
	{
		glUseProgram(shader);

		s_previousShader = shader;
	}
}

void NativeRenderer_SetTexture(TextureID texture, TexFormat texFormat, int semiTransPass, BlendMode blendMode, int textured, int superTurboTint,
                               int textureFullyOpaque, int cachedP4, const RECT16 *textureWindow)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	(void)superTurboTint;
	int variant;
	if (semiTransPass == 1)
	{
		variant = NATIVE_PSX_SHADER_NON_STP;
	}
	else if (semiTransPass == 2)
	{
		variant = blendMode == BM_AVERAGE         ? NATIVE_PSX_SHADER_STP_AVERAGE
		          : blendMode == BM_ADD_QUATER_SOURCE ? NATIVE_PSX_SHADER_STP_QUARTER
		                                               : NATIVE_PSX_SHADER_STP;
	}
	else if (semiTransPass == 3)
	{
		variant = blendMode == BM_AVERAGE              ? NATIVE_PSX_SHADER_MIXED_AVERAGE
		          : blendMode == BM_ADD_QUATER_SOURCE ? NATIVE_PSX_SHADER_MIXED_QUARTER
		                                               : NATIVE_PSX_SHADER_MIXED_ADD;
	}
	else
	{
		variant = blendMode == BM_AVERAGE         ? NATIVE_PSX_SHADER_OPAQUE_AVERAGE
		          : blendMode == BM_ADD_QUATER_SOURCE ? NATIVE_PSX_SHADER_OPAQUE_QUARTER
		                                               : NATIVE_PSX_SHADER_OPAQUE;
	}
	if (texFormat < TF_4_BIT || texFormat > TF_32_BIT_RGBA)
	{
		return;
	}
	GTEShader *shader;
	const int opaqueVariant = blendMode == BM_AVERAGE              ? NATIVE_PSX_SHADER_OPAQUE_AVERAGE
	                          : blendMode == BM_ADD_QUATER_SOURCE ? NATIVE_PSX_SHADER_OPAQUE_QUARTER
	                                                               : NATIVE_PSX_SHADER_OPAQUE;
	if (cachedP4 && textureFullyOpaque)
	{
		shader = &s_gteCachedP4FullyOpaqueShaderVariants[opaqueVariant];
	}
	else if (cachedP4)
	{
		shader = &s_gteCachedP4ShaderVariants[variant];
	}
	else if (!textured)
	{
		shader = &s_gteUntexturedShaderVariants[variant < 3 ? variant : 0];
	}
	else if (textureFullyOpaque && texFormat <= TF_8_BIT)
	{
		shader = &s_gteFullyOpaqueShaderVariants[texFormat][opaqueVariant];
	}
	else
	{
		shader = &s_gteShaderVariants[texFormat][variant];
	}
	if (textured && textureWindow != NULL && (textureWindow->w != 0 || textureWindow->h != 0))
	{
		shader = NativeRenderer_WindowedShader(shader);
	}
	NATIVE_GPU_LABEL(2, shader->label, 0);
#else
	(void)textured;
	(void)textureFullyOpaque;
	(void)cachedP4;
	GTEShader *shader = NULL;
	switch (texFormat)
	{
	case TF_4_BIT:
		shader = superTurboTint ? &s_gteShader4SuperTurbo : &s_gteShader4;
		break;
	case TF_8_BIT:
		shader = superTurboTint ? &s_gteShader8SuperTurbo : &s_gteShader8;
		break;
	case TF_16_BIT:
		shader = superTurboTint ? &s_gteShader16SuperTurbo : &s_gteShader16;
		break;
	case TF_32_BIT_RGBA:
		shader = &s_gteShader32Rgba;
		break;
	}
	if (shader == NULL)
	{
		return;
	}
#endif

	NativeRenderer_SetShader(shader->shader);
	u_bilinearFilterLoc = shader->bilinearFilterLoc;
	u_projectionLoc = shader->projectionLoc;
	u_texelSizeLoc = texFormat == TF_32_BIT_RGBA ? shader->texelSizeLoc : -1;
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	u_psxSemiTransPassLoc = shader->psxSemiTransPassLoc;
	u_psxDitherEnabledLoc = shader->psxDitherEnabledLoc;
#endif
	u_psxDrawMaskSetLoc = shader->psxDrawMaskSetLoc;
	u_psxTextureOutputStpLoc = shader->psxTextureOutputStpLoc;
	if (shader->textureWindowLoc >= 0 && textureWindow != NULL)
	{
		glUniform4f(shader->textureWindowLoc, (float)textureWindow->w, (float)textureWindow->h,
		            (float)textureWindow->x, (float)textureWindow->y);
	}

	if (g_dbg_texturelessMode)
	{
		texture = s_whiteTexture;
	}

	// NOTE(penta3): s_texture (unit 0) and s_rgLut (unit 1) sampler bindings are baked into each program at compile time (NativeRenderer_Shader_Compile) and uniform values persist per-program, so re-setting them on every split was redundant GL churn. bilinearFilter stays here because it toggles at runtime (debug key).
	if (u_bilinearFilterLoc >= 0)
	{
		glUniform1i(u_bilinearFilterLoc, g_cfg_bilinearFiltering);
	}
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	if (u_psxSemiTransPassLoc >= 0)
	{
		glUniform1i(u_psxSemiTransPassLoc, semiTransPass);
	}
	if (u_psxDitherEnabledLoc >= 0)
	{
		glUniform1i(u_psxDitherEnabledLoc, gNativeDitheringEnabled != 0);
	}
#endif

	if (s_lastBoundTexture == texture)
	{
		return;
	}

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);

	s_lastBoundTexture = texture;
}

void NativeRenderer_SetOverrideTextureSize(int width, int height)
{
	if (u_texelSizeLoc == -1)
	{
		return;
	}

	float vec[] = {1.0f / (float)width, 1.0f / (float)height};
	glUniform2fv(u_texelSizeLoc, 1, vec);
}

void NativeRenderer_SetPSXTextureOutputSTP(int enabled)
{
	if (u_psxTextureOutputStpLoc >= 0)
	{
		glUniform1f(u_psxTextureOutputStpLoc, enabled ? 1.0f : 0.0f);
	}
}

void NativeRenderer_SetPSXDrawMaskSet(int maskSet)
{
	if (u_psxDrawMaskSetLoc >= 0)
	{
		glUniform1f(u_psxDrawMaskSetLoc, maskSet ? 1.0f : 0.0f);
	}
}

internal void NativeRenderer_DestroyTexture(TextureID texture)
{
	if (texture == (TextureID)-1)
	{
		return;
	}

	glDeleteTextures(1, &texture);
}

internal u16 NativeRenderer_PackRGB24ToPSX15(u8 r, u8 g, u8 b)
{
	return (u16)(((r >> 3) & 0x1f) | (((g >> 3) & 0x1f) << 5) | (((b >> 3) & 0x1f) << 10));
}

internal float NativeRenderer_PSXColorComponentFloat(u8 value)
{
	const u8 psx8 = (u8)((value >> 3) << 3);
	return (float)psx8 / 255.0f;
}

internal int NativeRenderer_ClipVRAMRect(RECT16 *out, int x, int y, int w, int h)
{
	if ((w <= 0) || (h <= 0))
	{
		return 0;
	}

	if (x < 0)
	{
		w += x;
		x = 0;
	}
	if (y < 0)
	{
		h += y;
		y = 0;
	}
	if (x + w > VRAM_WIDTH)
	{
		w = VRAM_WIDTH - x;
	}
	if (y + h > VRAM_HEIGHT)
	{
		h = VRAM_HEIGHT - y;
	}
	if ((w <= 0) || (h <= 0))
	{
		return 0;
	}

	out->x = (s16)x;
	out->y = (s16)y;
	out->w = (s16)w;
	out->h = (s16)h;
	return 1;
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
internal int NativeRenderer_HasGpuNewerVRAMTiles(int x, int y, int w, int h)
{
	const int tileX0 = x / NATIVE_VRAM_TILE_SIZE;
	const int tileY0 = y / NATIVE_VRAM_TILE_SIZE;
	const int tileX1 = (x + w - 1) / NATIVE_VRAM_TILE_SIZE;
	const int tileY1 = (y + h - 1) / NATIVE_VRAM_TILE_SIZE;
	for (int tileY = tileY0; tileY <= tileY1; tileY++)
	{
		for (int tileX = tileX0; tileX <= tileX1; tileX++)
		{
			const int tileIndex = tileY * NATIVE_VRAM_TILE_COLS + tileX;
			if ((s_vram.gpuNewerTiles[tileIndex >> 5] & (1u << (tileIndex & 31))) != 0)
			{
				return 1;
			}
		}
	}
	return 0;
}

internal int NativeRenderer_VRAMRectsOverlap(const RECT16 *a, const RECT16 *b)
{
	return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

internal void NativeRenderer_GetP4SourceRects(int page, int clut, RECT16 *pageRect, RECT16 *clutRect)
{
	pageRect->x = (s16)((page & 15) * 64);
	pageRect->y = (s16)((page >> 4) * 256);
	pageRect->w = 64;
	pageRect->h = 256;

	clutRect->x = (s16)((clut & 0x3f) << 4);
	clutRect->y = (s16)(clut >> 6);
	clutRect->w = 16;
	clutRect->h = 1;
}

internal void NativeRenderer_InvalidatePaletteCacheRows(const RECT16 *rect)
{
	const int rowStart = rect->y < 0 ? 0 : rect->y;
	const int rowEnd = rect->y + rect->h > VRAM_HEIGHT ? VRAM_HEIGHT : rect->y + rect->h;
	for (int row = rowStart; row < rowEnd; row++)
	{
		u32 generation = ++s_paletteRowGeneration[row];
		if (generation == 0)
		{
			s_paletteRowGeneration[row] = 1;
			for (int clutX = 0; clutX < 64; clutX++)
			{
				const int clut = row * 64 + clutX;
				s_paletteCacheGeneration[0][clut] = 0;
				s_paletteCacheGeneration[1][clut] = 0;
			}
		}
	}
}

internal void NativeRenderer_InvalidateP4CacheRect(const RECT16 *rect)
{
	if (++s_p4VersionSerial == 0)
		s_p4VersionSerial = 1;
	const u32 version = s_p4VersionSerial;
	for (int pageY = rect->y / TPAGE_HEIGHT; pageY <= (rect->y + rect->h - 1) / TPAGE_HEIGHT; pageY++)
	{
		for (int pageX = rect->x / 64; pageX <= (rect->x + rect->w - 1) / 64; pageX++)
			s_p4PageVersion[pageY * 16 + pageX] = version;
	}
	for (int row = rect->y; row < rect->y + rect->h; row++)
	{
		for (int clutX = rect->x / 16; clutX <= (rect->x + rect->w - 1) / 16; clutX++)
			s_p4ClutVersion[row * 64 + clutX] = version;
	}
}

internal int NativeRenderer_EnsureP4Texture(struct NativeP4CacheEntry *entry)
{
	if (entry->texture != 0)
	{
		return 1;
	}

	glGenTextures(1, &entry->texture);
	if (entry->texture == 0)
	{
		return 0;
	}

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, entry->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	s_lastBoundTexture = (TextureID)-1;
	return 1;
}

/* The entry's palette as GXM P4_ABGR words (byte order R, G, B, A). */
internal void NativeRenderer_ConvertP4Palette(const struct NativeP4CacheEntry *entry, const RECT16 *clutRect, u8 *paletteOut)
{
	const u16 *palette = s_vram.cpuPixels + (size_t)clutRect->y * VRAM_WIDTH + clutRect->x;
	for (int colorIndex = 0; colorIndex < 16; colorIndex++)
	{
		const u16 color = palette[colorIndex];
		u8 r = (u8)(color & 31);
		u8 g = (u8)((color >> 5) & 31);
		u8 b = (u8)((color >> 10) & 31);
		if (entry->superTurboTint && color != 0)
		{
			const u8 luminance = r > g ? (r > b ? r : b) : (g > b ? g : b);
			const int boosted = (luminance * 5 + 3) / 4;
			r = (u8)((luminance * 3) / 10);
			g = (u8)(boosted > 31 ? 31 : boosted);
			b = (u8)(boosted + 2 > 31 ? 31 : boosted + 2);
		}
		paletteOut[colorIndex * 4 + 0] = (u8)(r << 3);
		paletteOut[colorIndex * 4 + 1] = (u8)(g << 3);
		paletteOut[colorIndex * 4 + 2] = (u8)(b << 3);
		paletteOut[colorIndex * 4 + 3] = color == 0 ? 0 : (color & 0x8000) != 0 ? 255 : 128;
	}
}

internal int NativeRenderer_BuildP4Texture(struct NativeP4CacheEntry *entry, const RECT16 *pageRect, const RECT16 *clutRect)
{
	const u32 pageVersion = s_p4PageVersion[entry->page];
	const u32 clutVersion = s_p4ClutVersion[clutRect->y * 64 + clutRect->x / 16];
	if (!NativeRenderer_EnsureP4Texture(entry))
		return 0;
#if defined(__vita__)
	/* The page's indices once for all its CLUTs, the entry's palette, and a
	 * texture pointing at both: nothing is copied per entry. */
	if (s_p4PageData[entry->page].data == NULL || s_p4PageData[entry->page].version != pageVersion)
	{
		u8 *indices = (u8 *)vglMemalign(128, NATIVE_P4_PAGE_BYTES);
		if (indices == NULL)
			return 0;
		for (int row = 0; row < TPAGE_HEIGHT; row++)
			SDL_memcpy(indices + row * (TPAGE_WIDTH / 2),
			           s_vram.cpuPixels + (size_t)(pageRect->y + row) * VRAM_WIDTH + pageRect->x, TPAGE_WIDTH / 2);
		vglLazyFree(s_p4PageData[entry->page].data);
		s_p4PageData[entry->page].data = indices;
		s_p4PageData[entry->page].version = pageVersion;
	}
	if (entry->palette == NULL || entry->builtClutVersion != clutVersion)
	{
		u8 *palette = (u8 *)vglMemalign(SCE_GXM_PALETTE_ALIGNMENT, NATIVE_P4_PALETTE_BYTES);
		if (palette == NULL)
			return 0;
		NativeRenderer_ConvertP4Palette(entry, clutRect, palette);
		vglLazyFree(entry->palette);
		entry->palette = palette;
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, entry->texture);
	/* Keep vitaGL from freeing the page's buffer the texture pointed at. */
	vglOverloadTexDataPointer(GL_TEXTURE_2D, NULL);
	vglSetupTexture(GL_TEXTURE_2D, s_p4PageData[entry->page].data, SCE_GXM_TEXTURE_FORMAT_P4_ABGR,
	                SCE_GXM_TEXTURE_LINEAR, TPAGE_WIDTH, TPAGE_HEIGHT, 1);
	sceGxmTextureSetPalette(vglGetGxmTexture(GL_TEXTURE_2D), entry->palette);
#else
	NativeRenderer_ConvertP4Palette(entry, clutRect, s_p4UploadData);
	u8 *indicesOut = s_p4UploadData + NATIVE_P4_PALETTE_BYTES;
	for (int row = 0; row < TPAGE_HEIGHT; row++)
	{
		const u8 *src = (const u8 *)(s_vram.cpuPixels + (size_t)(pageRect->y + row) * VRAM_WIDTH + pageRect->x);
		u8 *dst = indicesOut + row * (TPAGE_WIDTH / 2);
		for (int packedIndex = 0; packedIndex < TPAGE_WIDTH / 2; packedIndex++)
		{
			const u8 packed = src[packedIndex];
			dst[packedIndex] = (u8)((packed << 4) | (packed >> 4));
		}
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, entry->texture);
	PCSXMacVitaGL_UploadP4Texture(TPAGE_WIDTH, TPAGE_HEIGHT, s_p4UploadData);
#endif
	s_lastBoundTexture = (TextureID)-1;
	entry->builtPageVersion = pageVersion;
	entry->builtClutVersion = clutVersion;
	return 1;
}

/* A source rect the GPU drew long ago and nothing drew since keeps the
 * 4-bit cache off for good (a boot-time clear under textures the game
 * then never rewrites whole). Such rects are read back once, before the
 * frame draws, so the cache can build from VRAM again. */
internal void NativeRenderer_QueueStaleP4Rect(const RECT16 *rect, int wholePage)
{
	u32 *frame = &s_gpuNewerPageFrame[(rect->y / TPAGE_HEIGHT) * 16 + rect->x / 64];
	if (*frame == 0 || s_p4FrameSerial - *frame < NATIVE_STALE_P4_FRAMES)
		return;
	if (!NativeRenderer_HasGpuNewerVRAMTiles(rect->x, rect->y, rect->w, rect->h))
	{
		if (wholePage)
			*frame = 0;
		return;
	}
	if (s_staleP4Sync.w == 0)
	{
		s_staleP4Sync = *rect;
		return;
	}
	const int x0 = SDL_min(s_staleP4Sync.x, rect->x), y0 = SDL_min(s_staleP4Sync.y, rect->y);
	const int x1 = SDL_max(s_staleP4Sync.x + s_staleP4Sync.w, rect->x + rect->w);
	const int y1 = SDL_max(s_staleP4Sync.y + s_staleP4Sync.h, rect->y + rect->h);
	s_staleP4Sync.x = (s16)x0;
	s_staleP4Sync.y = (s16)y0;
	s_staleP4Sync.w = (s16)(x1 - x0);
	s_staleP4Sync.h = (s16)(y1 - y0);
}

void NativeRenderer_QueueStaleP4Sources(int page, int clut)
{
	if (page < 0 || page >= (int)(VRAM_HEIGHT / TPAGE_HEIGHT) * 16 || clut < 0)
		return;
	RECT16 pageRect;
	RECT16 clutRect;
	NativeRenderer_GetP4SourceRects(page, clut, &pageRect, &clutRect);
	if (clutRect.y < 0 || clutRect.y >= VRAM_HEIGHT || clutRect.x < 0 || clutRect.x + clutRect.w > VRAM_WIDTH)
		return;
	NativeRenderer_QueueStaleP4Rect(&pageRect, 1);
	NativeRenderer_QueueStaleP4Rect(&clutRect, 0);
}

void NativeRenderer_SyncStaleP4Sources(void)
{
	if (s_staleP4Sync.w == 0)
		return;
	NATIVE_GPU_LABEL(0, "stale p4 sync", 0);
	NativeRenderer_SyncGpuVRAMToCPU(s_staleP4Sync.x, s_staleP4Sync.y, s_staleP4Sync.w, s_staleP4Sync.h);
	NATIVE_GPU_LABEL(1, NULL, 0);
	s_staleP4Sync.w = 0;
}

TextureID NativeRenderer_GetCachedP4Texture(int page, int clut, int superTurboTint)
{

	if (page < 0 || page >= (int)(VRAM_HEIGHT / TPAGE_HEIGHT) * 16 || clut < 0)
	{
		return (0);
	}

	RECT16 pageRect;
	RECT16 clutRect;
	NativeRenderer_GetP4SourceRects(page, clut, &pageRect, &clutRect);
	if (clutRect.y < 0 || clutRect.y >= VRAM_HEIGHT || clutRect.x < 0 || clutRect.x + clutRect.w > VRAM_WIDTH)
	{
		return (0);
	}

	const u8 tint = (u8)(superTurboTint != 0);
	const u32 key = NativeRenderer_P4Key(page, clut, tint);
	struct NativeP4CacheEntry *entry = NULL;
	struct NativeP4CacheEntry *replacement = NULL;

	if (s_p4LastLookupIndex >= 0 && s_p4LastLookupIndex < NATIVE_P4_CACHE_CAPACITY && s_p4LastLookupKey == key)
	{
		struct NativeP4CacheEntry *candidate = &s_p4Cache[s_p4LastLookupIndex];
		if (candidate->occupied && candidate->page == page && candidate->clut == (u16)clut && candidate->superTurboTint == tint)
		{
			entry = candidate;
		}
	}

	if (entry == NULL)
	{
		const int cacheIndex = NativeRenderer_FindP4Hash(key);
		if (cacheIndex >= 0)
		{
			entry = &s_p4Cache[cacheIndex];
			s_p4LastLookupIndex = cacheIndex;
			s_p4LastLookupKey = key;
		}
	}

	if (entry != NULL && entry->builtPageVersion == s_p4PageVersion[page] &&
	    entry->builtClutVersion == s_p4ClutVersion[clutRect.y * 64 + clutRect.x / 16])
	{
		entry->lastUse = ++s_p4CacheUseCounter;
		entry->lastUseFrame = s_p4FrameSerial;
		return (entry->texture);
	}

	if (NativeRenderer_HasGpuNewerVRAMTiles(pageRect.x, pageRect.y, pageRect.w, pageRect.h) ||
	    NativeRenderer_HasGpuNewerVRAMTiles(clutRect.x, clutRect.y, clutRect.w, clutRect.h))
	{
		return (0);
	}

	if (entry == NULL)
	{
		for (int cacheIndex = 0; cacheIndex < NATIVE_P4_CACHE_CAPACITY; cacheIndex++)
		{
			struct NativeP4CacheEntry *candidate = &s_p4Cache[cacheIndex];
			if (!candidate->occupied)
			{
				if (replacement == NULL || replacement->occupied)
				{
					replacement = candidate;
				}
			}
			else if ((candidate->lastUseFrame + 2u < s_p4FrameSerial) &&
			         (replacement == NULL || (replacement->occupied && candidate->lastUse < replacement->lastUse)))
			{
				replacement = candidate;
			}
		}
	}

	if (entry == NULL)
	{
		entry = replacement;
		if (entry == NULL)
		{
			return (0);
		}

		const int entryIndex = (int)(entry - s_p4Cache);
		entry->page = (s16)page;
		entry->clut = (u16)clut;
		entry->superTurboTint = tint;
		entry->occupied = true;
		entry->builtPageVersion = 0;
		entry->builtClutVersion = 0;
		s_p4LastLookupIndex = entryIndex;
		s_p4LastLookupKey = key;
		NativeRenderer_InsertP4Hash(key, entryIndex);
	}

	entry->lastUse = ++s_p4CacheUseCounter;
	entry->lastUseFrame = s_p4FrameSerial;
	if (!NativeRenderer_BuildP4Texture(entry, &pageRect, &clutRect))
	{
		return (0);
	}
	return (entry->texture);
}

int NativeRenderer_GetPaletteProperties(TexFormat format, int clut)
{
	const int formatIndex = format == TF_4_BIT ? 0 : format == TF_8_BIT ? 1 : -1;
	const int width = formatIndex == 0 ? 16 : formatIndex == 1 ? 256 : 0;
	const int x = (clut & 0x3f) << 4;
	const int y = clut >> 6;
	if (formatIndex < 0 || clut < 0 || clut >= NATIVE_PALETTE_CLUT_COUNT || y < 0 || y >= VRAM_HEIGHT || x < 0 || x + width > VRAM_WIDTH)
	{
		return NATIVE_PALETTE_HAS_TRANSPARENT | NATIVE_PALETTE_HAS_OPAQUE | NATIVE_PALETTE_HAS_STP;
	}

	const u32 generation = s_paletteRowGeneration[y];
	if (s_paletteCacheGeneration[formatIndex][clut] == generation)
	{
		return s_paletteCacheProperties[formatIndex][clut];
	}

	if (NativeRenderer_HasGpuNewerVRAMTiles(x, y, width, 1))
	{
		return NATIVE_PALETTE_HAS_TRANSPARENT | NATIVE_PALETTE_HAS_OPAQUE | NATIVE_PALETTE_HAS_STP;
	}

	int properties = 0;
	const u16 *palette = s_vram.cpuPixels + y * VRAM_WIDTH + x;
	for (int colorIndex = 0; colorIndex < width; colorIndex++)
	{
		const u16 color = palette[colorIndex];
		if (color == 0)
		{
			properties |= NATIVE_PALETTE_HAS_TRANSPARENT;
		}
		else if ((color & 0x8000) != 0)
		{
			properties |= NATIVE_PALETTE_HAS_STP;
		}
		else
		{
			properties |= NATIVE_PALETTE_HAS_OPAQUE;
		}
		if (properties == (NATIVE_PALETTE_HAS_TRANSPARENT | NATIVE_PALETTE_HAS_OPAQUE | NATIVE_PALETTE_HAS_STP))
		{
			break;
		}
	}

	s_paletteCacheProperties[formatIndex][clut] = (u8)properties;
	s_paletteCacheGeneration[formatIndex][clut] = generation;
	return properties;
}

#endif

internal int NativeRenderer_DirtyRectContains(const RECT16 *outer, const RECT16 *inner)
{
	const int outerRight = outer->x + outer->w;
	const int outerBottom = outer->y + outer->h;
	const int innerRight = inner->x + inner->w;
	const int innerBottom = inner->y + inner->h;

	return (outer->x <= inner->x) && (outer->y <= inner->y) && (outerRight >= innerRight) && (outerBottom >= innerBottom);
}

internal int NativeRenderer_TryMergeDirtyRect(RECT16 *dst, const RECT16 *src)
{
	const int dstRight = dst->x + dst->w;
	const int dstBottom = dst->y + dst->h;
	const int srcRight = src->x + src->w;
	const int srcBottom = src->y + src->h;

	if ((dst->y == src->y) && (dst->h == src->h) && (src->x <= dstRight) && (dst->x <= srcRight))
	{
		const int x0 = (src->x < dst->x) ? src->x : dst->x;
		const int x1 = (srcRight > dstRight) ? srcRight : dstRight;
		dst->x = (s16)x0;
		dst->w = (s16)(x1 - x0);
		return 1;
	}

	if ((dst->x == src->x) && (dst->w == src->w) && (src->y <= dstBottom) && (dst->y <= srcBottom))
	{
		const int y0 = (src->y < dst->y) ? src->y : dst->y;
		const int y1 = (srcBottom > dstBottom) ? srcBottom : dstBottom;
		dst->y = (s16)y0;
		dst->h = (s16)(y1 - y0);
		return 1;
	}

	return 0;
}

internal void NativeRenderer_AppendCpuDirtyRect(const RECT16 *rect)
{
	for (s32 i = 0; i < s_vram.cpuDirtyRectCount; i++)
	{
		if (NativeRenderer_DirtyRectContains(&s_vram.cpuDirtyRects[i], rect))
		{
			return;
		}

		if (NativeRenderer_DirtyRectContains(rect, &s_vram.cpuDirtyRects[i]))
		{
			s_vram.cpuDirtyRects[i] = *rect;
			return;
		}

		if (NativeRenderer_TryMergeDirtyRect(&s_vram.cpuDirtyRects[i], rect))
		{
			return;
		}
	}

	if (s_vram.cpuDirtyRectCount >= NATIVE_VRAM_DIRTY_RECT_CAP)
	{
		NativeRenderer_UpdateVRAM();
	}

	if (s_vram.cpuDirtyRectCount < NATIVE_VRAM_DIRTY_RECT_CAP)
	{
		s_vram.cpuDirtyRects[s_vram.cpuDirtyRectCount] = *rect;
		s_vram.cpuDirtyRectCount++;
	}
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
/* The CPU copy is the newest for what a CPU write covers: its upload puts it
 * in the GPU's VRAM too. Tiles the write covers whole stop being GPU-newer
 * (only a CPU read cleared them before, so pages drawn over once, as games do
 * across VRAM at boot, kept the 4-bit texture cache off for the rest of the
 * game). Callers that mark a write row by row pass its whole rect here. */
internal void NativeRenderer_ClearGpuNewerCovered(int x, int y, int w, int h)
{
	RECT16 rect;
	if (!NativeRenderer_ClipVRAMRect(&rect, x, y, w, h))
		return;
	const int tileX0 = (rect.x + NATIVE_VRAM_TILE_SIZE - 1) / NATIVE_VRAM_TILE_SIZE;
	const int tileY0 = (rect.y + NATIVE_VRAM_TILE_SIZE - 1) / NATIVE_VRAM_TILE_SIZE;
	const int tileX1 = (rect.x + rect.w) / NATIVE_VRAM_TILE_SIZE; /* exclusive */
	const int tileY1 = (rect.y + rect.h) / NATIVE_VRAM_TILE_SIZE;
	for (int tileY = tileY0; tileY < tileY1; tileY++)
	{
		for (int tileX = tileX0; tileX < tileX1; tileX++)
		{
			const int tileIndex = tileY * NATIVE_VRAM_TILE_COLS + tileX;
			s_vram.gpuNewerTiles[tileIndex >> 5] &= ~(1u << (tileIndex & 31));
		}
	}
}
#endif

internal void NativeRenderer_MarkVRAMDirty(int x, int y, int w, int h)
{
	RECT16 rect;

	if (!NativeRenderer_ClipVRAMRect(&rect, x, y, w, h))
	{
		return;
	}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	/* Only a target whose rect the write touches is stale; reloading the
	 * others from VRAM would drop their host-resolution pixels. */
	if (NativeRenderer_VRAMRectsOverlap(&rect, &s_mainDrawRect))
		s_mainLoaded = 0;
	if (NativeRenderer_VRAMRectsOverlap(&rect, &s_previousOffscreen))
		s_offscreenLoaded = 0;
	if (NativeRenderer_VRAMRectsOverlap(&rect, &s_prevMainRect))
		s_prevMainValid = 0;
	NativeRenderer_InvalidateP4CacheRect(&rect);
	NativeRenderer_InvalidatePaletteCacheRows(&rect);
	NativeRenderer_ClearGpuNewerCovered(rect.x, rect.y, rect.w, rect.h);
#endif
	NativeRenderer_AppendCpuDirtyRect(&rect);
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
/* What primitives drew into each target area since it was last packed, in
 * VRAM coordinates. A pack writes the target's whole area to VRAM, but where
 * nothing was drawn it writes back what was loaded (load and pack sample at
 * pixel centres: the round trip is exact), so only the drawn part is newer on
 * the GPU. Marking all of it disabled the 4-bit texture cache over texture
 * pages forever: games draw over all of VRAM at boot, and a texture page the
 * CPU never writes again stayed GPU-newer. */

/* Vertices are relative to the area; primitives cover up to their last
 * vertex, and the area clips them. */
internal int NativeRenderer_DrawnRect(const RECT16 *area, int minX, int minY, int maxX, int maxY, s32 *x0, s32 *y0, s32 *x1, s32 *y1)
{
	*x0 = area->x + SDL_max(minX, 0);
	*y0 = area->y + SDL_max(minY, 0);
	*x1 = area->x + SDL_min(maxX + 1, (int)area->w);
	*y1 = area->y + SDL_min(maxY + 1, (int)area->h);
	return *x1 > *x0 && *y1 > *y0;
}

int NativeRenderer_TargetDrawCovered(const RECT16 *area, int minX, int minY, int maxX, int maxY)
{
	s32 x0, y0, x1, y1;
	if (!NativeRenderer_DrawnRect(area, minX, minY, maxX, maxY, &x0, &y0, &x1, &y1))
		return 1;
	for (int i = 0; i < s_drawnRectCount; i++)
	{
		if (s_drawnRects[i].x0 <= x0 && s_drawnRects[i].y0 <= y0 && s_drawnRects[i].x1 >= x1 && s_drawnRects[i].y1 >= y1)
			return 1;
	}
	return 0;
}

/* A drawn rect joins a recorded one when their bounding box is no larger
 * than both, so separate draws keep separate rects (marking the box
 * between them would block the 4-bit cache there). When the record is
 * full, it joins the one that grows least. */
void NativeRenderer_NoteTargetDraw(const RECT16 *area, int minX, int minY, int maxX, int maxY)
{
	s32 x0, y0, x1, y1;
	if (!NativeRenderer_DrawnRect(area, minX, minY, maxX, maxY, &x0, &y0, &x1, &y1))
		return;
	const s64 drawnArea = (s64)(x1 - x0) * (y1 - y0);
	int best = -1;
	s64 bestGrowth = 0;
	for (int i = 0; i < s_drawnRectCount; i++)
	{
		const s64 recorded = (s64)(s_drawnRects[i].x1 - s_drawnRects[i].x0) * (s_drawnRects[i].y1 - s_drawnRects[i].y0);
		const s64 joined = (s64)(SDL_max(s_drawnRects[i].x1, x1) - SDL_min(s_drawnRects[i].x0, x0)) *
		                   (SDL_max(s_drawnRects[i].y1, y1) - SDL_min(s_drawnRects[i].y0, y0));
		if (joined <= recorded + drawnArea)
		{
			best = i;
			bestGrowth = 0;
			break;
		}
		if (best < 0 || joined - recorded < bestGrowth)
		{
			best = i;
			bestGrowth = joined - recorded;
		}
	}
	if (best < 0 || (bestGrowth > 0 && s_drawnRectCount < NATIVE_DRAWN_RECTS))
	{
		s_drawnRects[s_drawnRectCount].x0 = x0;
		s_drawnRects[s_drawnRectCount].y0 = y0;
		s_drawnRects[s_drawnRectCount].x1 = x1;
		s_drawnRects[s_drawnRectCount].y1 = y1;
		s_drawnRectCount++;
		return;
	}
	s_drawnRects[best].x0 = SDL_min(s_drawnRects[best].x0, x0);
	s_drawnRects[best].y0 = SDL_min(s_drawnRects[best].y0, y0);
	s_drawnRects[best].x1 = SDL_max(s_drawnRects[best].x1, x1);
	s_drawnRects[best].y1 = SDL_max(s_drawnRects[best].y1, y1);
}

internal void NativeRenderer_MarkGpuVRAMNewer(int x, int y, int w, int h);

/* After packing the rect: mark GPU-newer what was drawn in it, and forget
 * the drawing it covered. */
internal void NativeRenderer_MarkPackedDraws(int x, int y, int w, int h)
{
	for (int i = 0; i < s_drawnRectCount;)
	{
		const int ix0 = SDL_max(s_drawnRects[i].x0, x), iy0 = SDL_max(s_drawnRects[i].y0, y);
		const int ix1 = SDL_min(s_drawnRects[i].x1, x + w), iy1 = SDL_min(s_drawnRects[i].y1, y + h);
		if (ix1 > ix0 && iy1 > iy0)
			NativeRenderer_MarkGpuVRAMNewer(ix0, iy0, ix1 - ix0, iy1 - iy0);
		if (s_drawnRects[i].x0 >= x && s_drawnRects[i].y0 >= y && s_drawnRects[i].x1 <= x + w && s_drawnRects[i].y1 <= y + h)
			s_drawnRects[i] = s_drawnRects[--s_drawnRectCount];
		else
			i++;
	}
}
#endif

internal void NativeRenderer_MarkGpuVRAMNewer(int x, int y, int w, int h)
{
	RECT16 rect;

	if (!NativeRenderer_ClipVRAMRect(&rect, x, y, w, h))
	{
		return;
	}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (NativeRenderer_VRAMRectsOverlap(&rect, &s_prevMainRect))
		s_prevMainValid = 0;
	NativeRenderer_InvalidateP4CacheRect(&rect);
	NativeRenderer_InvalidatePaletteCacheRows(&rect);
	for (int pageY = rect.y / TPAGE_HEIGHT; pageY <= (rect.y + rect.h - 1) / TPAGE_HEIGHT; pageY++)
	{
		for (int pageX = rect.x / 64; pageX <= (rect.x + rect.w - 1) / 64; pageX++)
			s_gpuNewerPageFrame[pageY * 16 + pageX] = s_p4FrameSerial;
	}
#endif
	const int tileX0 = rect.x / NATIVE_VRAM_TILE_SIZE;
	const int tileY0 = rect.y / NATIVE_VRAM_TILE_SIZE;
	const int tileX1 = (rect.x + rect.w - 1) / NATIVE_VRAM_TILE_SIZE;
	const int tileY1 = (rect.y + rect.h - 1) / NATIVE_VRAM_TILE_SIZE;

	for (int tileY = tileY0; tileY <= tileY1; tileY++)
	{
		for (int tileX = tileX0; tileX <= tileX1; tileX++)
		{
			const int tileIndex = tileY * NATIVE_VRAM_TILE_COLS + tileX;
			s_vram.gpuNewerTiles[tileIndex >> 5] |= 1u << (tileIndex & 31);
		}
	}
}

void NativeRenderer_ClearVRAM(int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
    if (w <= 0 || h <= 0)
        return;
    NativeRenderer_CommitTargets();
    x &= VRAM_WIDTH - 1;
    y &= VRAM_HEIGHT - 1;
    w = SDL_min(w, VRAM_WIDTH);
    h = SDL_min(h, (int)VRAM_HEIGHT);
    const u16 color = NativeRenderer_PackRGB24ToPSX15(r, g, b);
    const int firstWidth = SDL_min(w, VRAM_WIDTH - x);
    for (int row = 0; row < h; row++)
    {
        const int yy = (y + row) & (VRAM_HEIGHT - 1);
        u16 *dst = s_vram.cpuPixels + yy * VRAM_WIDTH;
        for (int col = 0; col < firstWidth; col++)
            dst[x + col] = color;
        for (int col = firstWidth; col < w; col++)
            dst[col - firstWidth] = color;
    }
    /* Marked per rect, not per row: each mark walks the caches. */
    const int firstHeight = SDL_min(h, (int)VRAM_HEIGHT - y);
    NativeRenderer_MarkVRAMDirty(x, y, firstWidth, firstHeight);
    if (firstWidth < w)
        NativeRenderer_MarkVRAMDirty(0, y, w - firstWidth, firstHeight);
    if (firstHeight < h)
    {
        NativeRenderer_MarkVRAMDirty(x, 0, firstWidth, h - firstHeight);
        if (firstWidth < w)
            NativeRenderer_MarkVRAMDirty(0, 0, w - firstWidth, h - firstHeight);
    }
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
    NativeRenderer_ClearGpuNewerCovered(x, y, firstWidth, h);
#endif
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
/* A GP0 fill inside the area the main target holds is cleared in the target
 * itself, in the scene already open. Going through VRAM cost a CPU fill, a
 * texture upload into the VRAM framebuffer (a scene) and a reload of the
 * whole target (another scene and a full-target quad). The CPU copy of VRAM
 * gets the colour too, exactly, and the target's next pack writes the same
 * pixels to the GPU's VRAM. Returns 0 when the fill must go through VRAM. */
/* The first host pixel whose centre lies at or after PSX edge e (0..psx):
 * ceil((e * host - psx / 2) / psx), in integers. */
internal int NativeRenderer_HostEdge(int e, int host, int psx)
{
	const int n = 2 * e * host - psx;
	return n <= 0 ? 0 : (n + 2 * psx - 1) / (2 * psx);
}

int NativeRenderer_FillMainTarget(int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
	const RECT16 *area = &s_mainDrawRect;
	if (!s_mainLoaded || s_previousOffscreenState || w <= 0 || h <= 0 || area->w <= 0 || area->h <= 0 ||
	    s_mainRenderTarget.width <= 0 || s_mainRenderTarget.height <= 0)
		return 0;
	if (x < area->x || y < area->y || x + w > area->x + area->w || y + h > area->y + area->h ||
	    x + w > VRAM_WIDTH || y + h > (int)VRAM_HEIGHT)
		return 0;

	const u16 color = NativeRenderer_PackRGB24ToPSX15(r, g, b);
	for (int row = 0; row < h; row++)
	{
		u16 *dst = s_vram.cpuPixels + (size_t)(y + row) * VRAM_WIDTH + x;
		for (int col = 0; col < w; col++)
			dst[col] = color;
	}
	/* The CPU copy holds the fill exactly; the target's pack writes the same
	 * pixels to the GPU's VRAM. Only caches built from the old pixels go. */
	const RECT16 fillRect = {(s16)x, (s16)y, (s16)w, (s16)h};
	if (NativeRenderer_VRAMRectsOverlap(&fillRect, &s_prevMainRect))
		s_prevMainValid = 0;
	NativeRenderer_InvalidateP4CacheRect(&fillRect);
	NativeRenderer_InvalidatePaletteCacheRows(&fillRect);

	/* A host pixel belongs to the PSX pixel under its centre, as when the
	 * target is loaded from VRAM and packed back: host pixel i covers the PSX
	 * pixels from ceil(i * area / target - 0.5). Rows go down in PSX space and
	 * up in GL's (the area's top is the target's top). */
	const int targetW = s_mainRenderTarget.width;
	const int targetH = s_mainRenderTarget.height;
	const int left = NativeRenderer_HostEdge(x - area->x, targetW, area->w);
	const int right = NativeRenderer_HostEdge(x + w - area->x, targetW, area->w);
	const int rowTop = NativeRenderer_HostEdge(y - area->y, targetH, area->h);
	const int rowBottom = NativeRenderer_HostEdge(y + h - area->y, targetH, area->h);
	const int bottom = targetH - rowBottom;
	const int top = targetH - rowTop;
	if (right <= left || top <= bottom)
		return 1; /* covers no host pixel's centre */

	NATIVE_GPU_LABEL(2, "fill-target", 0);
	glBindFramebuffer(GL_FRAMEBUFFER, s_mainRenderTarget.framebuffer);
	GLint previousScissorBox[4];
	const GLboolean previousScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_SCISSOR_BOX, previousScissorBox);
	glEnable(GL_SCISSOR_TEST);
	glScissor(left, bottom, right - left, top - bottom);
	/* The 5-bit colour expanded as loading the target from VRAM does
	 * ((c << 3) | (c >> 2)): blending over the fill must see the same
	 * destination as before. */
	const float r8 = (float)(((r >> 3) << 3) | (r >> 5)) / 255.0f;
	const float g8 = (float)(((g >> 3) << 3) | (g >> 5)) / 255.0f;
	const float b8 = (float)(((b >> 3) << 3) | (b >> 5)) / 255.0f;
	glClearColor(r8, g8, b8, 0.0f);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	if (previousScissorEnabled)
		glScissor(previousScissorBox[0], previousScissorBox[1], previousScissorBox[2], previousScissorBox[3]);
	else
		glDisable(GL_SCISSOR_TEST);
	s_previousScissorState = previousScissorEnabled ? 1 : 0;
	NativeRenderer_InvalidateScissorRectCache();
	s_mainDirty = 1;
	return 1;
}
#endif

void NativeRenderer_Clear(int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
	NATIVE_GPU_LABEL(2, "clear", 0);
	if ((w <= 0) || (h <= 0))
	{
		return;
	}

	const DRAWENV *drawEnv = NativeGpu_GetRenderDrawEnv();
	int displayX = drawEnv->clip.x;
	int displayY = drawEnv->clip.y;
	int displayW = drawEnv->clip.w;
	int displayH = drawEnv->clip.h;

	/* GP0 fill rectangles target the current draw buffer in VRAM coordinates. With PS1 double buffering DISPENV points at the other page. */
	if ((displayW <= 0) || (displayH <= 0))
	{
		displayX = NativeGpu_GetRenderDispEnv()->disp.x;
		displayY = NativeGpu_GetRenderDispEnv()->disp.y;
		displayW = NativeGpu_GetRenderDispEnv()->disp.w;
		displayH = NativeGpu_GetRenderDispEnv()->disp.h;
	}

	if ((displayW <= 0) || (displayH <= 0))
	{
		return;
	}

	const int clearRight = x + w;
	const int clearBottom = y + h;
	const int displayRight = displayX + displayW;
	const int displayBottom = displayY + displayH;

	const int overlapX = x > displayX ? x : displayX;
	const int overlapY = y > displayY ? y : displayY;
	const int overlapRight = clearRight < displayRight ? clearRight : displayRight;
	const int overlapBottom = clearBottom < displayBottom ? clearBottom : displayBottom;

	if ((overlapRight <= overlapX) || (overlapBottom <= overlapY))
	{
		return;
	}

	const int physicalW = s_mainRenderTarget.width > 0 ? s_mainRenderTarget.width : displayW;
	const int physicalH = s_mainRenderTarget.height > 0 ? s_mainRenderTarget.height : displayH;
	const int relLeft = overlapX - displayX;
	const int relRight = overlapRight - displayX;
	const int relTop = overlapY - displayY;
	const int relBottom = overlapBottom - displayY;
	const int scissorX = relLeft * physicalW / displayW;
	const int scissorRight = (relRight * physicalW + displayW - 1) / displayW;
	const int scissorY = (displayH - relBottom) * physicalH / displayH;
	const int scissorTop = ((displayH - relTop) * physicalH + displayH - 1) / displayH;
	const int scissorW = scissorRight - scissorX;
	const int scissorH = scissorTop - scissorY;

	if ((scissorW <= 0) || (scissorH <= 0))
	{
		return;
	}

	GLint previousScissorBox[4];
	const GLboolean previousScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_SCISSOR_BOX, previousScissorBox);

	glEnable(GL_SCISSOR_TEST);
	glScissor(scissorX, scissorY, scissorW, scissorH);
	glClearColor(NativeRenderer_PSXColorComponentFloat(r), NativeRenderer_PSXColorComponentFloat(g), NativeRenderer_PSXColorComponentFloat(b), 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	if (previousScissorEnabled)
	{
		glEnable(GL_SCISSOR_TEST);
		glScissor(previousScissorBox[0], previousScissorBox[1], previousScissorBox[2], previousScissorBox[3]);
	}
	else
	{
		glDisable(GL_SCISSOR_TEST);
	}

	s_previousScissorState = previousScissorEnabled ? 1 : 0;
	NativeRenderer_InvalidateScissorRectCache();
}

void NativeRenderer_SaveVRAM(const char *outputFileName, int x, int y, int width, int height, int bReadFromFrameBuffer)
{
#define FLIP_Y (VRAM_HEIGHT - i - 1)

	(void)x;
	(void)y;
	(void)bReadFromFrameBuffer;

	NativeRenderer_SyncGpuVRAMToCPU(0, 0, VRAM_WIDTH, VRAM_HEIGHT);

	FILE *fp = fopen(outputFileName, "wb");
	if (fp == NULL)
	{
		return;
	}

	u8 TGAheader[12] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	u8 header[6];
	header[0] = (width % 256);
	header[1] = (width / 256);
	header[2] = (height % 256);
	header[3] = (height / 256);
	header[4] = 16;
	header[5] = 0;

	fwrite(TGAheader, sizeof(u8), 12, fp);
	fwrite(header, sizeof(u8), 6, fp);

	for (int i = 0; i < VRAM_HEIGHT; i++)
	{
		fwrite(s_vram.cpuPixels + VRAM_WIDTH * FLIP_Y, sizeof(u16), VRAM_WIDTH, fp);
	}

	fclose(fp);

#undef FLIP_Y
}

internal void NativeRenderer_SyncGpuVRAMToCPU(int x, int y, int w, int h)
{
	NativeRenderer_CommitTargets();
	RECT16 readRect;

	if (!NativeRenderer_ClipVRAMRect(&readRect, x, y, w, h))
	{
		return;
	}

	const int tileX0 = readRect.x / NATIVE_VRAM_TILE_SIZE;
	const int tileY0 = readRect.y / NATIVE_VRAM_TILE_SIZE;
	const int tileX1 = (readRect.x + readRect.w - 1) / NATIVE_VRAM_TILE_SIZE;
	const int tileY1 = (readRect.y + readRect.h - 1) / NATIVE_VRAM_TILE_SIZE;
	b32 needsReadback = false;

	for (int tileY = tileY0; tileY <= tileY1 && !needsReadback; tileY++)
	{
		for (int tileX = tileX0; tileX <= tileX1; tileX++)
		{
			const int tileIndex = tileY * NATIVE_VRAM_TILE_COLS + tileX;
			if ((s_vram.gpuNewerTiles[tileIndex >> 5] & (1u << (tileIndex & 31))) != 0)
			{
				needsReadback = true;
				break;
			}
		}
	}

	if (!needsReadback)
	{
		return;
	}

	readRect.x = (s16)(tileX0 * NATIVE_VRAM_TILE_SIZE);
	readRect.y = (s16)(tileY0 * NATIVE_VRAM_TILE_SIZE);
	readRect.w = (s16)((tileX1 - tileX0 + 1) * NATIVE_VRAM_TILE_SIZE);
	readRect.h = (s16)((tileY1 - tileY0 + 1) * NATIVE_VRAM_TILE_SIZE);

	// CPU writes must reach the persistent texture before a GPU-authored region is read back, preserving PS1 VRAM command order in the split host mirror.
	NativeRenderer_UpdateVRAM();

	GLint previousReadFramebuffer;
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	GLint previousPackRowLength;
	GLint previousPackAlignment;
#endif
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	glGetIntegerv(GL_PACK_ROW_LENGTH, &previousPackRowLength);
	glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
#endif

	glBindFramebuffer(GL_READ_FRAMEBUFFER, s_glVramFramebuffer);
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	glPixelStorei(GL_PACK_ROW_LENGTH, VRAM_WIDTH);
	glPixelStorei(GL_PACK_ALIGNMENT, sizeof(u16));
	glReadPixels(readRect.x, readRect.y, readRect.w, readRect.h, VRAM_FORMAT, GL_UNSIGNED_BYTE,
	             s_vram.cpuPixels + (size_t)readRect.y * VRAM_WIDTH + readRect.x);
#else
	// vitaGL always packs rows tightly.  Passing a pointer into cpuPixels here would advance each subsequent row by readRect.w instead of VRAM_WIDTH.
	// READBACKS_SPEEDHACK removes vitaGL's implicit scene submission/fence.
	// A guest VRAM read is synchronous, so finish GPU work explicitly here.
	glFinish();
	/* Use GXM transfer readback so render-target data is resolved before
	 * reading it. The CPU fast path can see stale render-surface storage. */
	/* The pack viewport stores guest Y directly in this FBO. With VitaGL's
	 * default flipped FBOs, GPU transfer readback also addresses those rows
	 * directly; converting Y again selects the opposite VRAM window. */
	#if !defined(__vita__)
	if (!vglReadPixelsChecked(readRect.x, readRect.y,
	                          readRect.w, readRect.h, VRAM_FORMAT, GL_UNSIGNED_BYTE, s_vitaVramTransferPixels))
	{
		SysPrintf("pcsx-vita: vitaGL readback FAIL VRAM x=%d y=%d w=%d h=%d\n", readRect.x, readRect.y, readRect.w, readRect.h);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previousReadFramebuffer);
		return;
	}
	#else
	vglReadPixels(readRect.x, readRect.y, readRect.w, readRect.h, VRAM_FORMAT, GL_UNSIGNED_BYTE, s_vitaVramTransferPixels);
	#endif
	for (int row = 0; row < readRect.h; row++)
	{
		const u8 *src = s_vitaVramTransferPixels + (size_t)row * readRect.w * 4;
		u16 *dst = s_vram.cpuPixels + (size_t)(readRect.y + row) * VRAM_WIDTH + readRect.x;
		for (int column = 0; column < readRect.w; column++)
		{
			dst[column] = (u16)(src[column * 4] | ((u16)src[column * 4 + 1] << 8));
		}
	}
#endif

	for (int tileY = tileY0; tileY <= tileY1; tileY++)
	{
		for (int tileX = tileX0; tileX <= tileX1; tileX++)
		{
			const int tileIndex = tileY * NATIVE_VRAM_TILE_COLS + tileX;
			s_vram.gpuNewerTiles[tileIndex >> 5] &= ~(1u << (tileIndex & 31));
		}
	}

#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	glPixelStorei(GL_PACK_ROW_LENGTH, previousPackRowLength);
	glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
#endif
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previousReadFramebuffer);
}

internal void NativeRenderer_ResolveVRAMRead(int x, int y, int w, int h)
{
	NativeRenderer_SyncGpuVRAMToCPU(x, y, w, h);
}

void NativeRenderer_SyncVRAMToCPU(int x, int y, int w, int h)
{
	NativeRenderer_SyncGpuVRAMToCPU(x, y, w, h);
}

internal int NativeRenderer_RectEquals(const RECT16 *a, const RECT16 *b)
{
	return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

internal void NativeRenderer_FlushOffscreenToVRAM(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (!s_offscreenDirty)
		return;
	s_offscreenDirty = 0;
#endif
	if (s_previousOffscreen.w <= 0 || s_previousOffscreen.h <= 0)
	{
		return;
	}

	// NOTE(aalhendi): Native offscreen draws produce RGBA pixels. Pack them into the persistent 5:5:5:1 VRAM texture instead of reading them through the CPU.
	NativeRenderer_GpuPackTextureToVRAM(&s_offscreenRenderTarget, s_previousOffscreen.x, s_previousOffscreen.y, s_previousOffscreen.w,
	                                    s_previousOffscreen.h, true);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	/* Returning to main must include any offscreen writes in VRAM. */
	if (NativeRenderer_VRAMRectsOverlap(&s_previousOffscreen, &s_mainDrawRect))
		s_mainLoaded = 0;
#endif
}

internal void NativeRenderer_FlushMainToVRAM(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (s_mainDirty)
		NativeRenderer_StoreFrameBuffer(s_mainDrawRect.x, s_mainDrawRect.y, s_mainDrawRect.w, s_mainDrawRect.h);
#endif
}

internal void NativeRenderer_CommitTargets(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	NativeRenderer_FlushMainToVRAM();
	NativeRenderer_FlushOffscreenToVRAM();
#endif
}

internal void NativeRenderer_SetScissorState(int enable)
{
	if (s_previousScissorState == enable)
	{
		return;
	}

	if (s_previousScissorState)
	{
		glDisable(GL_SCISSOR_TEST);
	}
	else
	{
		glEnable(GL_SCISSOR_TEST);
	}
	s_previousScissorState = enable;
}

void NativeRenderer_SetOffscreenState(const RECT16 *offscreenRect, int enable)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (offscreenRect->w <= 0 || offscreenRect->h <= 0)
		return;
	if (enable)
	{
		NativeRenderer_FlushMainToVRAM();
		if (!s_previousOffscreenState || !s_offscreenLoaded ||
		    !NativeRenderer_RectEquals(&s_previousOffscreen, offscreenRect))
		{
			NativeRenderer_FlushOffscreenToVRAM();
			s_previousOffscreenState = 1;
			s_previousOffscreen = *offscreenRect;
			NativeRenderer_EnsureTargetSize(&s_offscreenRenderTarget, offscreenRect->w, offscreenRect->h, 1, 0);
			s_offscreenRenderTarget.logicalWidth = offscreenRect->w;
			s_offscreenRenderTarget.logicalHeight = offscreenRect->h;
			NativeRenderer_LoadRenderTargetFromVRAM(&s_offscreenRenderTarget, offscreenRect->x, offscreenRect->y, offscreenRect->w, offscreenRect->h);
			s_offscreenLoaded = 1;
		}
		s_offscreenDirty = 1;
	}
	else
	{
		NativeRenderer_FlushOffscreenToVRAM();
		s_previousOffscreenState = 0;
		if (!s_mainLoaded || !NativeRenderer_RectEquals(&s_mainDrawRect, offscreenRect))
		{
			NativeRenderer_FlushMainToVRAM();
			NativeRenderer_BindMainRect(offscreenRect);
			NativeRenderer_LoadRenderTargetFromVRAM(&s_mainRenderTarget, offscreenRect->x, offscreenRect->y, offscreenRect->w, offscreenRect->h);
			s_mainLoaded = 1;
		}
		else
			glBindFramebuffer(GL_FRAMEBUFFER, s_mainRenderTarget.framebuffer);
		NativeRenderer_SetViewPort(0, 0, s_mainRenderTarget.width, s_mainRenderTarget.height);
		s_mainDirty = 1;
	}
#else
	const int sameOffscreenRect = NativeRenderer_RectEquals(&s_previousOffscreen, offscreenRect);
	if (!enable && !s_previousOffscreenState)
	{
		return;
	}

	if (enable && s_previousOffscreenState && sameOffscreenRect)
	{
		return;
	}

	if (enable)
	{
		if (s_previousOffscreenState)
		{
			NativeRenderer_FlushOffscreenToVRAM();
		}

		s_previousOffscreenState = 1;
		NativeRenderer_EnsureRenderTarget(&s_offscreenRenderTarget, offscreenRect->w, offscreenRect->h);
		s_offscreenRenderTarget.logicalWidth = offscreenRect->w;
		s_offscreenRenderTarget.logicalHeight = offscreenRect->h;
		s_previousOffscreen = *offscreenRect;
		NativeRenderer_LoadRenderTargetFromVRAM(&s_offscreenRenderTarget, offscreenRect->x, offscreenRect->y, offscreenRect->w, offscreenRect->h);
	}
	else
	{
		s_previousOffscreenState = 0;

		NativeRenderer_FlushOffscreenToVRAM();
		NativeRenderer_BindMainRenderTarget();
		NativeRenderer_SetViewPort(0, 0, s_mainRenderTarget.width, s_mainRenderTarget.height);
	}
#endif
}

void NativeRenderer_SetProjection(const RECT16 *drawRect, const DISPENV *displayEnv, int offscreen)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	(void)displayEnv;
	(void)offscreen;
	NativeRenderer_Ortho2D(0, drawRect->w, drawRect->h, 0, -1.0f, 1.0f);
#else
	if (offscreen)
	{
		NativeRenderer_Ortho2D(0, drawRect->w, drawRect->h, 0, -1.0f, 1.0f);
		return;
	}

	const int displayW = displayEnv->disp.w > 0 ? displayEnv->disp.w : 1;
	const int displayH = displayEnv->disp.h > 0 ? displayEnv->disp.h : 1;
	NativeRenderer_Ortho2D(0, displayW, displayH, 0, -1.0f, 1.0f);
#endif
}

// NOTE(aalhendi): Pack an RGBA render texture straight into the RG8 VRAM texture on the GPU, no CPU round trip. Restore or invalidate the render-state caches disturbed by this native bridge before the submit run continues.
internal void NativeRenderer_GpuPackTextureToVRAM(const struct NativeRenderTarget *source, int x, int y, int w, int h, b32 flipY)
{
	NATIVE_GPU_LABEL(0, "pack-to-vram", 0);
	const TextureID sourceTexture = source->texture;
	const ShaderID previousShader = s_previousShader;
	const TextureID previousTexture = s_lastBoundTexture;
	const BlendMode previousBlendMode = s_previousBlendMode;
	const int previousMixedSTPBlend = s_previousMixedSTPBlend;
	const int previousDepthMode = s_previousDepthMode;
	const int previousDepthWrite = s_previousDepthWrite;
	const int previousScissorState = s_previousScissorState;
	const GLboolean previousStencilEnabled = glIsEnabled(GL_STENCIL_TEST);

	NativeRenderer_UpdateVRAM();

	glBindFramebuffer(GL_FRAMEBUFFER, s_glVramFramebuffer);

	NativeRenderer_SetDepthState(0, 0);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glViewport(x, y, w, h);

	glUseProgram(s_packShader);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, sourceTexture);
	glUniform1i(s_packFlipYLoc, flipY);
	if (s_packUvScaleLoc >= 0)
		glUniform2f(s_packUvScaleLoc,
		            source->allocWidth > 0 ? (float)source->width / (float)source->allocWidth : 1.0f,
		            source->allocHeight > 0 ? (float)source->height / (float)source->allocHeight : 1.0f);

	glBindVertexArray(s_vramQuadVAO);
	NativeRenderer_DrawTriangles(0, 2);
	if (s_boundVertexBuffer >= 0)
	{
		glBindVertexArray(s_glVertexArray[s_boundVertexBuffer]);
	}
	else
	{
		glBindVertexArray(0);
	}

	if (s_previousOffscreenState)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, s_offscreenRenderTarget.framebuffer);
		glViewport(0, 0, s_offscreenRenderTarget.width, s_offscreenRenderTarget.height);
	}
	else
	{
		// Restore the existing target without resizing it from a newer draw environment.
		// Packing can run after the display dimensions change, before presentation.
		glBindFramebuffer(GL_FRAMEBUFFER, s_mainRenderTarget.framebuffer);
		glViewport(0, 0, s_mainRenderTarget.width, s_mainRenderTarget.height);
	}
	if (previousStencilEnabled)
	{
		glEnable(GL_STENCIL_TEST);
	}

	glUseProgram(previousShader == (ShaderID)-1 ? 0 : previousShader);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, previousTexture == (TextureID)-1 ? 0 : previousTexture);
	s_previousShader = previousShader;
	s_lastBoundTexture = previousTexture;
	s_previousBlendMode = BM_NONE;
	s_previousMixedSTPBlend = 0;
	s_previousScissorState = 0;
	if (previousMixedSTPBlend)
	{
		NativeRenderer_SetMixedSTPBlendMode(previousBlendMode);
	}
	else
	{
		NativeRenderer_SetBlendMode(previousBlendMode);
	}
	NativeRenderer_SetDepthState(previousDepthMode, previousDepthWrite);
	NativeRenderer_SetScissorState(previousScissorState);
	NativeRenderer_MarkPackedDraws(x, y, w, h);
	NATIVE_GPU_LABEL(1, NULL, 0);
}

// NOTE(aalhendi): PS1 draws into VRAM and can texture from that same VRAM. Native mirrors that by flushing pending CPU VRAM writes, then packing the presented framebuffer into the persistent RG8 VRAM texture. CPU-side VRAM reads pull from that packed texture lazily, avoiding the old per-frame GPU->CPU->GPU round trip.
void NativeRenderer_StoreFrameBuffer(int x, int y, int w, int h)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (!s_mainDirty)
		return;
	s_mainDirty = 0;
	x = s_mainDrawRect.x;
	y = s_mainDrawRect.y;
	w = s_mainDrawRect.w;
	h = s_mainDrawRect.h;
#endif
#if defined(__vita__) && defined(VITA_CTR_GPU_TRACE)
	NativeRenderer_TraceFrameGeometry(1, x, y, w, h);
	static int traceMainSampled;
	if (!traceMainSampled && x <= 160 && y <= 100 && x + w > 160 && y + h > 100 &&
	    s_mainRenderTarget.width > 0 && s_mainRenderTarget.height > 0 &&
	    NativeGpu_GetRenderDrawEnv()->clip.w > 0 && NativeGpu_GetRenderDrawEnv()->clip.h > 0)
	{
		const int sampleX[2] = {160, 10};
		const int sampleY[2] = {100, 10};
		GLint previousFramebuffer = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, s_mainRenderTarget.framebuffer);
		glFinish();
		for (int i = 0; i < 2; i++)
		{
			const RECT16 *clip = &s_mainDrawRect;
			const int pixelX = (sampleX[i] - clip->x) * s_mainRenderTarget.width / clip->w;
			const int pixelY = s_mainRenderTarget.height - 1 - (sampleY[i] - clip->y) * s_mainRenderTarget.height / clip->h;
			vglReadPixels(pixelX, pixelY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, s_vitaVramTransferPixels);
			SysPrintf("pcsx-vita: gpu-trace mainrt xy=%d,%d physical=%d,%d rgba=%u,%u,%u,%u size=%dx%d logical=%dx%d\n",
			          sampleX[i], sampleY[i], pixelX, pixelY,
			          s_vitaVramTransferPixels[0], s_vitaVramTransferPixels[1], s_vitaVramTransferPixels[2], s_vitaVramTransferPixels[3],
			          s_mainRenderTarget.width, s_mainRenderTarget.height,
			          NativeGpu_GetRenderDrawEnv()->clip.w, NativeGpu_GetRenderDrawEnv()->clip.h);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previousFramebuffer);
		traceMainSampled = 1;
	}
#endif
	NativeRenderer_GpuPackTextureToVRAM(&s_mainRenderTarget, x, y, w, h, true);

}

void NativeRenderer_CopyVRAM(u16 *src, int x, int y, int w, int h, int dst_x, int dst_y)
{
	NativeRenderer_CommitTargets();
	int stride = w;

	if (!src)
	{
		// NOTE(aalhendi): MoveImage reads exactly its PS1 VRAM source rectangle. Resolve only that GPU-authored region into the CPU mirror before copying it.
		NativeRenderer_ResolveVRAMRead(x, y, w, h);
		src = s_vram.cpuPixels;
		stride = VRAM_WIDTH;
	}

	src += x + y * stride;

	u16 *dst = s_vram.cpuPixels + dst_x + dst_y * VRAM_WIDTH;

	for (int i = 0; i < h; i++)
	{
		SDL_memcpy(dst, src, w * sizeof(u16));
		dst += VRAM_WIDTH;
		src += stride;
	}

	NativeRenderer_MarkVRAMDirty(dst_x, dst_y, w, h);
}

void NativeRenderer_ReadVRAM(u16 *dst, int x, int y, int dst_w, int dst_h)
{
	NativeRenderer_ResolveVRAMRead(x, y, dst_w, dst_h);

	u16 *src = s_vram.cpuPixels + x + VRAM_WIDTH * y;

	for (int i = 0; i < dst_h; i++)
	{
		SDL_memcpy(dst, src, dst_w * sizeof(u16));
		dst += dst_w;
		src += VRAM_WIDTH;
	}
}

int NativeRenderer_GetVRAMStateSize(void)
{
	return (int)(VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16));
}

int NativeRenderer_CaptureVRAMState(void *dst, int dstSize)
{
	const size_t activeVramBytes = (size_t)VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16);
	if ((dst == NULL) || (dstSize < (int)activeVramBytes))
	{
		return 0;
	}

	// NOTE(aalhendi): Save-states own the CPU-side PSX VRAM mirror, not GL textures. Pull pending GPU-authored VRAM into the mirror first.
	NativeRenderer_SyncGpuVRAMToCPU(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
	SDL_memcpy(dst, s_vram.cpuPixels, activeVramBytes);
	return 1;
}

int NativeRenderer_RestoreVRAMState(const void *src, int srcSize)
{
	local_persist const RECT16 zeroRect = {0, 0, 0, 0};
	const size_t activeVramBytes = (size_t)VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16);

	if ((src == NULL) || (srcSize < (int)activeVramBytes))
	{
		return 0;
	}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	s_mainDirty = s_offscreenDirty = 0;
#endif
	SDL_memcpy(s_vram.cpuPixels, src, activeVramBytes);
	// NOTE(aalhendi): Restored VRAM is authoritative PSX state. Host GL caches are rebuildable, so mark all of VRAM dirty and drop stale bindings.
	s_vram.cpuDirtyRectCount = 0;
	SDL_memset(s_vram.gpuNewerTiles, 0, sizeof(s_vram.gpuNewerTiles));
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	s_drawnRectCount = 0;
	SDL_memset(s_gpuNewerPageFrame, 0, sizeof(s_gpuNewerPageFrame));
	s_staleP4Sync.w = 0;
#endif
	NativeRenderer_MarkVRAMDirty(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
	s_mainRenderTarget.width = 0;
	s_mainRenderTarget.height = 0;
	s_mainRenderTarget.logicalWidth = 0;
	s_mainRenderTarget.logicalHeight = 0;
	s_offscreenRenderTarget.width = 0;
	s_offscreenRenderTarget.height = 0;
	s_offscreenRenderTarget.logicalWidth = 0;
	s_offscreenRenderTarget.logicalHeight = 0;
	s_previousOffscreen = zeroRect;
	s_previousOffscreenState = 0;
	s_previousShader = (ShaderID)-1;
	s_lastBoundTexture = (TextureID)-1;
	return 1;
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
internal void NativeRenderer_UploadVRAMRects(s32 rectCount)
{
	NATIVE_GPU_LABEL(0, "vram-upload", 0);
	GLint previousFramebuffer;
	GLint previousViewport[4];
	GLint previousProgram;
	GLint previousTexture;
	GLboolean previousDepthWrite;
	const GLboolean previousBlend = glIsEnabled(GL_BLEND);
	const GLboolean previousDepth = glIsEnabled(GL_DEPTH_TEST);
	const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
	const GLboolean previousStencil = glIsEnabled(GL_STENCIL_TEST);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFramebuffer);
	glGetIntegerv(GL_VIEWPORT, previousViewport);
	glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthWrite);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	if (s_vramUploadTexture == 0)
	{
		glGenTextures(1, &s_vramUploadTexture);
		glBindTexture(GL_TEXTURE_2D, s_vramUploadTexture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	else
	{
		glBindTexture(GL_TEXTURE_2D, s_vramUploadTexture);
	}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_glVramFramebuffer);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glViewport(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
	glUseProgram(s_vramUploadShader);
	glBindVertexArray(s_vramQuadVAO);
	/* The rects go up stacked in one RG8 texture (the PSX pixels' own two
	 * bytes, copied row by row: R = low byte, G = high, as the RGBA VRAM
	 * target gets them), one texture store per batch instead of per rect:
	 * each store of a texture still in use is a new allocation in vitaGL. */
	for (s32 first = 0; first < rectCount;)
	{
		int atlasWidth = 0, atlasHeight = 0;
		s32 last = first;
		for (; last < rectCount; ++last)
		{
			const RECT16 r = s_vram.cpuDirtyRects[last];
			const int width = r.w > atlasWidth ? r.w : atlasWidth;
			/* At most 512 rows: the shaders run in half precision, and a
			 * texel centre must stay well inside its texel. */
			if (last > first && (atlasHeight + r.h > 512 ||
			                     (size_t)width * (size_t)(atlasHeight + r.h) * sizeof(u16) > NATIVE_VITA_VRAM_TRANSFER_BYTES))
				break;
			atlasWidth = width;
			atlasHeight += r.h;
		}
		atlasWidth = (atlasWidth + 1) & ~1; /* rows of whole 4-byte words */
		u16 *atlas = (u16 *)s_vitaVramTransferPixels;
		int row = 0;
		for (s32 i = first; i < last; ++i)
		{
			const RECT16 r = s_vram.cpuDirtyRects[i];
			for (int y = 0; y < r.h; ++y, ++row)
				SDL_memcpy(atlas + (size_t)row * atlasWidth, s_vram.cpuPixels + (size_t)(r.y + y) * VRAM_WIDTH + r.x,
				           (size_t)r.w * sizeof(u16));
		}
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, atlasWidth, atlasHeight, 0, GL_RG, GL_UNSIGNED_BYTE, atlas);
		row = 0;
		for (s32 i = first; i < last; ++i)
		{
			const RECT16 r = s_vram.cpuDirtyRects[i];
			glUniform4f(s_vramUploadSourceRectLoc, 0.0f, (float)row / (float)atlasHeight,
			            (float)r.w / (float)atlasWidth, (float)r.h / (float)atlasHeight);
			glUniform4f(s_vramUploadRectLoc, r.x * (2.0f / VRAM_WIDTH) - 1.0f,
			            r.y * (2.0f / VRAM_HEIGHT) - 1.0f,
			            r.w * (2.0f / VRAM_WIDTH), r.h * (2.0f / VRAM_HEIGHT));
			NativeRenderer_DrawTriangles(0, 2);
			row += r.h;
		}
		first = last;
	}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousFramebuffer);
	glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
	glUseProgram(previousProgram);
	glBindTexture(GL_TEXTURE_2D, previousTexture);
	glBindVertexArray(s_boundVertexBuffer >= 0 ? s_glVertexArray[s_boundVertexBuffer] : 0);
	glDepthMask(previousDepthWrite);
	if (previousBlend) glEnable(GL_BLEND);
	if (previousDepth) glEnable(GL_DEPTH_TEST);
	if (previousScissor) glEnable(GL_SCISSOR_TEST);
	if (previousStencil) glEnable(GL_STENCIL_TEST);
	NATIVE_GPU_LABEL(1, NULL, 0);
}
#endif

void NativeRenderer_UpdateVRAM(void)
{
	if (s_vram.cpuDirtyRectCount == 0)
	{
		return;
	}

	const s32 rectCount = s_vram.cpuDirtyRectCount;
	s_vram.cpuDirtyRectCount = 0;

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	NativeRenderer_UploadVRAMRects(rectCount);
#else
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, s_vram.texture);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, VRAM_WIDTH);
	for (s32 i = 0; i < rectCount; i++)
	{
		const RECT16 r = s_vram.cpuDirtyRects[i];
		glTexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.w, r.h, VRAM_FORMAT, GL_UNSIGNED_BYTE, s_vram.cpuPixels + (size_t)r.y * VRAM_WIDTH + r.x);
	}
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
	s_lastBoundTexture = (TextureID)-1;

}

void NativeRenderer_PresentVRAMRect(int displayX, int displayY, int displayW, int displayH)
{
	NATIVE_GPU_LABEL(2, "present-vram", 0);
	if (displayW <= 0 || displayH <= 0)
	{
		return;
	}

	NativeRenderer_UpdateVRAM();

	NativeRenderer_SetViewPort(s_presentViewport.x, s_presentViewport.y, s_presentViewport.w, s_presentViewport.h);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	NativeRenderer_ClearPresentationBars();

	NativeRenderer_SetScissorState(0);
	NativeRenderer_SetBlendMode(BM_NONE);
	NativeRenderer_SetDepthState(0, 0);
	const GLboolean previousStencilEnabled = glIsEnabled(GL_STENCIL_TEST);
	glDisable(GL_STENCIL_TEST);

	NativeRenderer_DrawVRAMRegion(displayX, displayY, displayW, displayH);
	if (previousStencilEnabled)
	{
		glEnable(GL_STENCIL_TEST);
	}
	glBindVertexArray(0);

	s_previousShader = (ShaderID)-1;
	s_lastBoundTexture = (TextureID)-1;
}

void NativeRenderer_PresentBlank(void)
{
	GLfloat previousClearColor[4];
	const GLboolean previousStencilEnabled = glIsEnabled(GL_STENCIL_TEST);

	NativeRenderer_SetViewPort(0, 0, g_windowWidth, g_windowHeight);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	NativeRenderer_SetScissorState(0);
	NativeRenderer_SetBlendMode(BM_NONE);
	NativeRenderer_SetDepthState(0, 0);
	glDisable(GL_STENCIL_TEST);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glClearColor(previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
	if (previousStencilEnabled)
	{
		glEnable(GL_STENCIL_TEST);
	}
	glBindVertexArray(0);
	s_previousShader = (ShaderID)-1;
	s_lastBoundTexture = (TextureID)-1;
}
/* Draws the main render target into a host viewport rectangle. */
internal void NativeRenderer_DrawMainTarget(const struct NativeRenderTarget *target, int x, int y, int w, int h, int clearBars)
{
	NATIVE_GPU_LABEL(0, "present-target", 0);
	NativeRenderer_SetViewPort(x, y, w, h);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (clearBars)
		NativeRenderer_ClearPresentationBars();

	NativeRenderer_SetScissorState(0);
	NativeRenderer_SetBlendMode(BM_NONE);
	NativeRenderer_SetDepthState(0, 0);
	const GLboolean previousStencilEnabled = glIsEnabled(GL_STENCIL_TEST);
	glDisable(GL_STENCIL_TEST);

	glUseProgram(s_presentRgbaShader);
	glUniform1f(s_presentRgbaFlipYLoc, 0.0f);
	if (s_presentRgbaUvScaleLoc >= 0)
		glUniform2f(s_presentRgbaUvScaleLoc,
		            target->allocWidth > 0 ? (float)target->width / (float)target->allocWidth : 1.0f,
		            target->allocHeight > 0 ? (float)target->height / (float)target->allocHeight : 1.0f);
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	if (s_presentRgbaTexelSizeLoc >= 0)
	{
		glUniform2f(s_presentRgbaTexelSizeLoc, 1.0f / (float)target->width, 1.0f / (float)target->height);
	}
	if (s_presentRgbaFxaaLoc >= 0)
	{
		glUniform1i(s_presentRgbaFxaaLoc, gNativeAntiAliasingEnabled != 0);
	}
#endif
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, target->texture);
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	// FXAA uses subpixel samples from the final native-resolution render target.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
#endif
	glBindVertexArray(s_vramQuadVAO);
	NativeRenderer_DrawTriangles(0, 2);
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
#endif

	if (previousStencilEnabled)
	{
		glEnable(GL_STENCIL_TEST);
	}
	glBindVertexArray(0);
	if (s_presentRgbaUvScaleLoc >= 0)
		glUniform2f(s_presentRgbaUvScaleLoc, 1.0f, 1.0f);

	s_previousShader = (ShaderID)-1;
	s_lastBoundTexture = (TextureID)-1;
	NATIVE_GPU_LABEL(1, NULL, 0);
}

void NativeRenderer_PresentMainRenderTarget(void)
{
	if ((s_mainRenderTarget.texture == 0) || (s_mainRenderTarget.width <= 0) || (s_mainRenderTarget.height <= 0))
	{
		return;
	}
	NativeRenderer_DrawMainTarget(&s_mainRenderTarget, s_presentViewport.x, s_presentViewport.y, s_presentViewport.w, s_presentViewport.h, 1);
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
/* Draws a host-resolution target over its part of the displayed VRAM rect,
 * if it lies inside it. Returns 0 when it does not. */
internal int NativeRenderer_OverlayTarget(const struct NativeRenderTarget *target, const RECT16 *r,
                                          int displayX, int displayY, int displayW, int displayH)
{
	if (target->texture == 0 || target->width <= 0 || target->height <= 0 ||
	    displayW <= 0 || displayH <= 0 || r->w <= 0 || r->h <= 0 ||
	    r->x < displayX || r->y < displayY ||
	    r->x + r->w > displayX + displayW || r->y + r->h > displayY + displayH)
		return 0;
	const int vx = s_presentViewport.x, vy = s_presentViewport.y;
	const int vw = s_presentViewport.w, vh = s_presentViewport.h;
	const int x0 = vx + (r->x - displayX) * vw / displayW;
	const int x1 = vx + (r->x + r->w - displayX) * vw / displayW;
	const int top = (r->y - displayY) * vh / displayH;
	const int bottom = (r->y + r->h - displayY) * vh / displayH;
	NativeRenderer_DrawMainTarget(target, x0, vy + vh - bottom, x1 - x0, bottom - top, 0);
	return 1;
}

/* After the displayed VRAM rectangle has been presented, cover it with the
 * host-resolution (and multisampled) target that drew it: the current main
 * target if it still matches VRAM, else the previous scene's one, which is
 * what a double-buffered game shows. The VRAM under it is the same picture
 * packed to 15-bit PSX pixels; borders and CPU uploads stay as presented.
 * Then the main target becomes the previous one; the next scene reloads its
 * replacement from VRAM. */
void NativeRenderer_PresentMainOverlay(int displayX, int displayY, int displayW, int displayH)
{
	if (!s_mainLoaded || s_mainDirty ||
	    !NativeRenderer_OverlayTarget(&s_mainRenderTarget, &s_mainDrawRect, displayX, displayY, displayW, displayH))
	{
		if (s_prevMainValid)
			NativeRenderer_OverlayTarget(&s_prevMainRenderTarget, &s_prevMainRect, displayX, displayY, displayW, displayH);
	}
	if (s_mainLoaded && !s_mainDirty && s_mainDrawRect.w > 0 && s_mainDrawRect.h > 0)
	{
		const struct NativeRenderTarget swap = s_prevMainRenderTarget;
		s_prevMainRenderTarget = s_mainRenderTarget;
		s_mainRenderTarget = swap;
		s_prevMainRect = s_mainDrawRect;
		s_prevMainValid = 1;
		s_mainLoaded = 0;
	}
}
#endif

internal TextureID NativeRenderer_CreateGhostReplayTexture(int width, int height, const u8 *pixels)
{
	TextureID texture = 0;
	glGenTextures(1, &texture);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	s_lastBoundTexture = (TextureID)-1;
	return texture;
}

internal b32 NativeRenderer_LoadGhostReplayOverlay(void)
{
	u8 *pixels = NULL;
	u8 highlight[32 * 32 * 4];
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	u8 shoulder[2][48 * 16 * 4];
#endif

	if (s_ghostReplayOverlayLoadAttempted)
	{
	#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
		return (s_ghostReplayControllerTexture != 0) && (s_ghostReplayHighlightTexture != 0) &&
		       (s_ghostReplayShoulderTexture[0] != 0) && (s_ghostReplayShoulderTexture[1] != 0);
	#else
		return (s_ghostReplayControllerTexture != 0) && (s_ghostReplayHighlightTexture != 0);
	#endif
	}
	s_ghostReplayOverlayLoadAttempted = true;

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	png_image image;
	memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&image, "app0:/vita.png"))
	{
		NATIVE_RENDERER_ERROR("%s\n", "Failed to load app0:/vita.png for Ghost Replay overlay");
		return false;
	}

	image.format = PNG_FORMAT_RGBA;
	pixels = (u8 *)malloc(PNG_IMAGE_SIZE(image));
	if ((pixels == NULL) || !png_image_finish_read(&image, NULL, pixels, 0, NULL))
	{
		free(pixels);
		png_image_free(&image);
		NATIVE_RENDERER_ERROR("%s\n", "Failed to decode app0:/vita.png for Ghost Replay overlay");
		return false;
	}
	s_ghostReplayControllerWidth = (int)image.width;
	s_ghostReplayControllerHeight = (int)image.height;
#else
	struct NativeAssetsByteBuffer overlayBytes = {0};
	int imageWidth = 0;
	int imageHeight = 0;
	int imageChannels = 0;
	if (!NativeAssets_ReadBytes("dualshock.png", NATIVE_ASSET_READ_DATA_FILE, &overlayBytes))
	{
		NATIVE_RENDERER_ERROR("%s\n", "Failed to load assets/dualshock.png for Ghost Replay overlay");
		return false;
	}
	pixels = (u8 *)stbi_load_from_memory(overlayBytes.data, overlayBytes.size, &imageWidth, &imageHeight, &imageChannels, 4);
	NativeAssets_FreeBytes(&overlayBytes);
	if (pixels == NULL)
	{
		NATIVE_RENDERER_ERROR("%s\n", "Failed to decode assets/dualshock.png for Ghost Replay overlay");
		return false;
	}
	s_ghostReplayControllerWidth = imageWidth;
	s_ghostReplayControllerHeight = imageHeight;
#endif
	s_ghostReplayControllerTexture = NativeRenderer_CreateGhostReplayTexture(s_ghostReplayControllerWidth, s_ghostReplayControllerHeight, pixels);

	for (int y = 0; y < 32; y++)
	{
		for (int x = 0; x < 32; x++)
		{
			const int dx = (x * 2) - 31;
			const int dy = (y * 2) - 31;
			const int dist2 = dx * dx + dy * dy;
			const int pixel = (y * 32 + x) * 4;
			highlight[pixel + 0] = 255;
			highlight[pixel + 1] = 128;
			highlight[pixel + 2] = 0;
			highlight[pixel + 3] = (dist2 <= 729) ? 190 : ((dist2 <= 961) ? 100 : 0);
		}
	}
	s_ghostReplayHighlightTexture = NativeRenderer_CreateGhostReplayTexture(32, 32, highlight);

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	const int shoulderImageX[2] = {16, 237};
	for (int side = 0; side < 2; side++)
	{
		for (int y = 0; y < 16; y++)
		{
			for (int x = 0; x < 48; x++)
			{
				const int sourceX = shoulderImageX[side] + x;
				const int sourcePixel = (y * s_ghostReplayControllerWidth + sourceX) * 4;
				const int dstPixel = (y * 48 + x) * 4;
				const u8 sourceAlpha = pixels[sourcePixel + 3];
				const int luminance = ((int)pixels[sourcePixel + 0] + (int)pixels[sourcePixel + 1] + (int)pixels[sourcePixel + 2]) / 3;
				const u8 maskAlpha = (sourceAlpha > 32 && luminance > 70) ? (u8)((sourceAlpha * 220) / 255) : 0;

				shoulder[side][dstPixel + 0] = 255;
				shoulder[side][dstPixel + 1] = 128;
				shoulder[side][dstPixel + 2] = 0;
				shoulder[side][dstPixel + 3] = maskAlpha;
			}
		}
		s_ghostReplayShoulderTexture[side] = NativeRenderer_CreateGhostReplayTexture(48, 16, shoulder[side]);
	}
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	free(pixels);
	png_image_free(&image);
#else
	stbi_image_free(pixels);
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	return (s_ghostReplayControllerTexture != 0) && (s_ghostReplayHighlightTexture != 0) &&
	       (s_ghostReplayShoulderTexture[0] != 0) && (s_ghostReplayShoulderTexture[1] != 0);
#else
	return (s_ghostReplayControllerTexture != 0) && (s_ghostReplayHighlightTexture != 0);
#endif
}

internal void NativeRenderer_DrawGhostReplayQuad(TextureID texture, int x, int y, int width, int height)
{
	if ((texture == 0) || (width <= 0) || (height <= 0))
	{
		return;
	}

	NativeRenderer_SetViewPort(x, y, width, height);
	glUseProgram(s_presentRgbaShader);
	glUniform1f(s_presentRgbaFlipYLoc, 1.0f);
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	if (s_presentRgbaFxaaLoc >= 0)
	{
		glUniform1i(s_presentRgbaFxaaLoc, 0);
	}
#endif
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glBindVertexArray(s_vramQuadVAO);
	NativeRenderer_DrawTriangles(0, 2);
}

internal void NativeRenderer_DrawGhostReplayHighlight(int overlayX, int overlayY, int overlayW, int overlayH,
	                                                   int imageX, int imageY, int width, int height)
{
	const int centerX = overlayX + (imageX * overlayW) / s_ghostReplayControllerWidth;
	const int centerY = overlayY + overlayH - (imageY * overlayH) / s_ghostReplayControllerHeight;
	const int scaledW = (width * overlayW) / s_ghostReplayControllerWidth;
	const int scaledH = (height * overlayH) / s_ghostReplayControllerHeight;
	NativeRenderer_DrawGhostReplayQuad(s_ghostReplayHighlightTexture, centerX - scaledW / 2, centerY - scaledH / 2, scaledW, scaledH);
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
internal void NativeRenderer_DrawGhostReplayImageRegion(TextureID texture, int overlayX, int overlayY, int overlayW, int overlayH,
	                                                     int imageX, int imageY, int imageW, int imageH)
{
	const int x = overlayX + (imageX * overlayW) / s_ghostReplayControllerWidth;
	const int y = overlayY + overlayH - ((imageY + imageH) * overlayH) / s_ghostReplayControllerHeight;
	const int width = (imageW * overlayW) / s_ghostReplayControllerWidth;
	const int height = (imageH * overlayH) / s_ghostReplayControllerHeight;
	NativeRenderer_DrawGhostReplayQuad(texture, x, y, width, height);
}
#endif

void NativeRenderer_DrawGhostReplayOverlay(void)
{
	u32 buttonsHeld;
	u8 stickLX;
	u8 stickLY;
	u8 stickRX;
	u8 stickRY;

	if (!NativeGhostInput_GetReplayOverlayState(&buttonsHeld, &stickLX, &stickLY, &stickRX, &stickRY) || !NativeRenderer_LoadGhostReplayOverlay())
	{
		return;
	}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	const int overlayW = (s_presentViewport.w * 23) / 100;
#else
	const int overlayW = (s_presentViewport.w * 25) / 100;
#endif
	const int overlayH = (overlayW * s_ghostReplayControllerHeight) / s_ghostReplayControllerWidth;
	const int overlayX = s_presentViewport.x + 14;
	const int overlayY = s_presentViewport.y + 12;
	const GLboolean previousStencilEnabled = glIsEnabled(GL_STENCIL_TEST);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	NativeRenderer_SetScissorState(0);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	NativeRenderer_EnableDepth(0);
	NativeRenderer_SetBlendMode(BM_AVERAGE);
#else
	// Ghost Replay is modern RGBA UI, not a PS1 semi-transparency primitive. BM_AVERAGE on desktop deliberately uses a constant 0.5 alpha to emulate the PS1 ABR mode, which darkens/tints the entire controller overlay. Keep the renderer state cache at BM_NONE and use ordinary source-alpha blending.
	NativeRenderer_SetBlendMode(BM_NONE);
	NativeRenderer_EnableDepth(0);
	glEnable(GL_BLEND);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
#endif
	glDisable(GL_STENCIL_TEST);

	NativeRenderer_DrawGhostReplayQuad(s_ghostReplayControllerTexture, overlayX, overlayY, overlayW, overlayH);

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if ((buttonsHeld & BTN_UP) != 0)       NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 28, 39, 14, 14);
	if ((buttonsHeld & BTN_DOWN) != 0)     NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 28, 62, 14, 14);
	if ((buttonsHeld & BTN_LEFT) != 0)     NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 17, 51, 14, 14);
	if ((buttonsHeld & BTN_RIGHT) != 0)    NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 39, 51, 14, 14);
	if ((buttonsHeld & BTN_TRIANGLE) != 0) NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 272, 38, 14, 14);
	if ((buttonsHeld & BTN_CIRCLE) != 0)   NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 284, 51, 14, 14);
	if ((buttonsHeld & BTN_CROSS) != 0)    NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 272, 64, 14, 14);
	if ((buttonsHeld & BTN_SQUARE) != 0)   NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 260, 51, 14, 14);
	if ((buttonsHeld & (BTN_L1 | BTN_L2)) != 0) NativeRenderer_DrawGhostReplayImageRegion(s_ghostReplayShoulderTexture[0], overlayX, overlayY, overlayW, overlayH, 16, 0, 48, 16);
	if ((buttonsHeld & (BTN_R1 | BTN_R2)) != 0) NativeRenderer_DrawGhostReplayImageRegion(s_ghostReplayShoulderTexture[1], overlayX, overlayY, overlayW, overlayH, 237, 0, 48, 16);

	if ((buttonsHeld & BTN_SELECT) != 0) NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 224, 128, 12, 6);
	if ((buttonsHeld & BTN_START) != 0)  NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 242, 128, 12, 6);
	if ((buttonsHeld & NATIVE_GHOST_OVERLAY_TOUCH_FRONT_LEFT) != 0)  NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 66, 18, 82, 5);
	if ((buttonsHeld & NATIVE_GHOST_OVERLAY_TOUCH_FRONT_RIGHT) != 0) NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 152, 18, 82, 5);
	if ((buttonsHeld & NATIVE_GHOST_OVERLAY_TOUCH_REAR_LEFT) != 0)   NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 66, 127, 82, 5);
	if ((buttonsHeld & NATIVE_GHOST_OVERLAY_TOUCH_REAR_RIGHT) != 0)  NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 152, 127, 82, 5);

	const int stickTravel = 8;
	const int leftStickX = 38 + (((int)stickLX - 128) * stickTravel) / 127;
	const int leftStickY = 90 + (((int)stickLY - 128) * stickTravel) / 127;
	const int rightStickX = 262 + (((int)stickRX - 128) * stickTravel) / 127;
	const int rightStickY = 90 + (((int)stickRY - 128) * stickTravel) / 127;
	NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, leftStickX, leftStickY, 8, 8);
	NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, rightStickX, rightStickY, 8, 8);
	if ((buttonsHeld & BTN_L3) != 0) NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, leftStickX - 2, leftStickY - 2, 12, 12);
	if ((buttonsHeld & BTN_R3) != 0) NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, rightStickX - 2, rightStickY - 2, 12, 12);
#else
	// Coordinates below are measured directly in assets/dualshock.png. NativeRenderer_DrawGhostReplayHighlight takes top-origin image-space Y coordinates and converts them to the OpenGL bottom-origin viewport.
	if ((buttonsHeld & BTN_UP) != 0)       NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 418, 312, 104, 104);
	if ((buttonsHeld & BTN_DOWN) != 0)     NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 418, 461, 104, 104);
	if ((buttonsHeld & BTN_LEFT) != 0)     NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 342, 387, 104, 104);
	if ((buttonsHeld & BTN_RIGHT) != 0)    NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 494, 387, 104, 104);
	if ((buttonsHeld & BTN_TRIANGLE) != 0) NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 1253, 277, 112, 112);
	if ((buttonsHeld & BTN_CIRCLE) != 0)   NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 1369, 387, 112, 112);
	if ((buttonsHeld & BTN_CROSS) != 0)    NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 1253, 500, 112, 112);
	if ((buttonsHeld & BTN_SQUARE) != 0)   NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 1138, 387, 112, 112);

	// The front-facing asset exposes a single shoulder cap per side. CTR labels these controls simply as L/R, so both shoulder inputs highlight the same visible cap instead of a synthetic lower half.
		if ((buttonsHeld & (BTN_L1 | BTN_L2)) != 0) NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 418, 70, 170, 64);
		if ((buttonsHeld & (BTN_R1 | BTN_R2)) != 0) NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 1248, 70, 170, 64);

	if ((buttonsHeld & BTN_SELECT) != 0) NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 742, 407, 86, 54);
	if ((buttonsHeld & BTN_START) != 0)  NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, 929, 408, 88, 58);

	const int stickTravel = 45;
	const int leftStickX = 623 + (((int)stickLX - 128) * stickTravel) / 127;
	const int leftStickY = 613 + (((int)stickLY - 128) * stickTravel) / 127;
	const int rightStickX = 1047 + (((int)stickRX - 128) * stickTravel) / 127;
	const int rightStickY = 613 + (((int)stickRY - 128) * stickTravel) / 127;
	NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, leftStickX, leftStickY, 58, 58);
	NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, rightStickX, rightStickY, 58, 58);
	if ((buttonsHeld & BTN_L3) != 0) NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, leftStickX, leftStickY, 116, 116);
	if ((buttonsHeld & BTN_R3) != 0) NativeRenderer_DrawGhostReplayHighlight(overlayX, overlayY, overlayW, overlayH, rightStickX, rightStickY, 116, 116);
#endif

	if (previousStencilEnabled)
	{
		glEnable(GL_STENCIL_TEST);
	}
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	NativeRenderer_SetBlendMode(BM_NONE);
#else
	glDisable(GL_BLEND);
	NativeRenderer_EnableDepth(1);
#endif
	NativeRenderer_SetViewPort(s_presentViewport.x, s_presentViewport.y, s_presentViewport.w, s_presentViewport.h);
	glBindVertexArray(0);
	s_previousShader = (ShaderID)-1;
	s_lastBoundTexture = (TextureID)-1;
}

void NativeRenderer_PresentStreamingTexture(TextureID texture, int contentHeight, int displayHeight)
{
	if ((texture == 0) || (contentHeight <= 0) || (displayHeight < contentHeight))
	{
		return;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	NativeRenderer_SetScissorState(0);
	NativeRenderer_SetBlendMode(BM_NONE);
	NativeRenderer_SetDepthState(0, 0);
	const GLboolean previousStencilEnabled = glIsEnabled(GL_STENCIL_TEST);
	glDisable(GL_STENCIL_TEST);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	const int viewportH = (s_presentViewport.h * contentHeight + displayHeight / 2) / displayHeight;
	const int viewportY = s_presentViewport.y + (s_presentViewport.h - viewportH) / 2;
	NativeRenderer_SetViewPort(s_presentViewport.x, viewportY, s_presentViewport.w, viewportH);

	glUseProgram(s_presentRgbaShader);
	glUniform1f(s_presentRgbaFlipYLoc, 1.0f);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glBindVertexArray(s_vramQuadVAO);
	NativeRenderer_DrawTriangles(0, 2);

	if (previousStencilEnabled)
	{
		glEnable(GL_STENCIL_TEST);
	}
	glBindVertexArray(0);
	s_previousShader = (ShaderID)-1;
	s_lastBoundTexture = (TextureID)-1;
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
static void NativeRenderer_PresentRGB24(const DISPENV *display)
{
	static TextureID texture;
	static int textureWidth;
	static int textureHeight;
	const int width = display->disp.w;
	const int height = display->disp.h;
	if (width <= 0 || width > VRAM_WIDTH || height <= 0 || height > VRAM_HEIGHT)
		return;
	const int x = display->disp.x & 1023;
	const int y = display->disp.y & (VRAM_HEIGHT - 1);
	const int packedWidth = SDL_min((width * 3 + 1) / 2, VRAM_WIDTH - x);
	const int firstHeight = SDL_min(height, VRAM_HEIGHT - y);
	NativeRenderer_SyncGpuVRAMToCPU(x, y, packedWidth, firstHeight);
	if (firstHeight < height)
		NativeRenderer_SyncGpuVRAMToCPU(x, 0, packedWidth, height - firstHeight);
	if (textureWidth != width || textureHeight != height)
	{
		NativeRenderer_DestroyStreamingTexture(texture);
		texture = NativeRenderer_CreateStreamingTexture(width, height);
		textureWidth = width;
		textureHeight = height;
	}
	for (int row = 0; row < height; ++row)
		psx_vita_rgb24_row(s_vram.cpuPixels, x, y, row, width, VRAM_HEIGHT,
		                   s_vitaVramTransferPixels + (size_t)row * width * 4u);
	NativeRenderer_UpdateStreamingTexture(texture, width, height, s_vitaVramTransferPixels);
	NativeRenderer_PresentStreamingTexture(texture, height, height);
}
#endif

void NativeRenderer_PresentVRAMDisplay(void)
{
	const DISPENV *display = NativeGpu_GetRenderDispEnv();
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (display->isrgb24)
	{
		NativeRenderer_PresentRGB24(display);
		return;
	}
#endif
	NativeRenderer_PresentVRAMRect(display->disp.x, display->disp.y, display->disp.w, display->disp.h);
}

void NativeRenderer_SwapWindow(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	#if defined(VITA_CTR_GPU_TRACE)
	static int traceSwapCount;
	if (traceSwapCount++ < 2 && s_vitaVramTransferPixels != NULL && s_presentViewport.w > 0 && s_presentViewport.h > 0)
	{
		GLint framebuffer = -1;
		const int sampleX = s_presentViewport.x + (160 * s_presentViewport.w) / 320;
		const int sampleY = s_presentViewport.y + s_presentViewport.h - 1 - (100 * s_presentViewport.h) / 240;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
		glFinish();
		vglReadPixels(sampleX, sampleY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, s_vitaVramTransferPixels);
		SysPrintf("pcsx-vita: gpu-trace backbuffer xy=%d,%d fbo=%d rgba=%u,%u,%u,%u viewport=%d,%d %dx%d\n",
		          sampleX, sampleY, framebuffer,
		          s_vitaVramTransferPixels[0], s_vitaVramTransferPixels[1],
		          s_vitaVramTransferPixels[2], s_vitaVramTransferPixels[3],
		          s_presentViewport.x, s_presentViewport.y, s_presentViewport.w, s_presentViewport.h);
	}
	#endif
#if defined(__vita__)
	extern unsigned long long pcsx_vita_ctr_wait_us[NATIVE_GPU_WAIT_COUNT];
	extern unsigned pcsx_vita_ctr_wait_n[NATIVE_GPU_WAIT_COUNT];
	const SceUInt64 swapStart = sceKernelGetProcessTimeWide();
	vglSwapBuffers(GL_FALSE);
	pcsx_vita_ctr_wait_us[NATIVE_GPU_WAIT_RENDER_SWAP] += sceKernelGetProcessTimeWide() - swapStart;
	pcsx_vita_ctr_wait_n[NATIVE_GPU_WAIT_RENDER_SWAP]++;
#else
	vglSwapBuffers(GL_FALSE);
#endif
	if (pcsx_vita_swap_cb)
		pcsx_vita_swap_cb();
#else
	SDL_GL_SwapWindow(g_window);
#endif
}

void NativeRenderer_SetDepthState(int enable, int write)
{
	if (s_previousDepthMode != enable)
	{
		s_previousDepthMode = enable;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
		if (enable)
		{
			glEnable(GL_DEPTH_TEST);
		}
		else
		{
			glDisable(GL_DEPTH_TEST);
		}
#else
		glDisable(GL_DEPTH_TEST);
#endif
	}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (s_previousDepthWrite != write)
	{
		s_previousDepthWrite = write;
		glDepthMask(write ? GL_TRUE : GL_FALSE);
	}
#else
	(void)write;
#endif
}

internal void NativeRenderer_EnableDepth(int enable)
{
	NativeRenderer_SetDepthState(enable, enable);
}

void NativeRenderer_SetStencilMode(int drawPrim)
{
	if (s_previousStencilMode == drawPrim)
	{
		return;
	}

	s_previousStencilMode = drawPrim;

	if (drawPrim)
	{
		glStencilFunc(GL_ALWAYS, 1, 0x10);
		glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
	}
	else
	{
		glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
		glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
	}
}

void NativeRenderer_SetBlendMode(BlendMode blendMode)
{
	if (s_previousBlendMode == blendMode && !s_previousMixedSTPBlend)
	{
		return;
	}

	if (blendMode != BM_NONE)
	{
		if (s_previousBlendMode == BM_NONE)
		{
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
			glBlendColor(0.25f, 0.25f, 0.25f, 0.5f);
#endif
			glEnable(GL_BLEND);
		}

		NativeRenderer_EnableDepth(0);
	}

	switch (blendMode)
	{
	case BM_NONE:
		if (s_previousBlendMode != BM_NONE)
		{
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
			glBlendColor(1.f, 1.f, 1.f, 1.f);
#endif
			glDisable(GL_BLEND);
		}

		NativeRenderer_EnableDepth(1);
		break;
	case BM_AVERAGE:
		glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
#else
		// NOTE(aalhendi): keep RGB blend weight constant so alpha can carry the PS1 mask bit.
		glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
#endif
		break;
	case BM_ADD:
		glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
		glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
		break;
	case BM_SUBTRACT:
		glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
		glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
		break;
	case BM_ADD_QUATER_SOURCE:
		glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
		glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
#else
		glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_ONE, GL_ONE, GL_ZERO);
#endif
		break;
	}

	s_previousBlendMode = blendMode;
	s_previousMixedSTPBlend = 0;
}

void NativeRenderer_SetMixedSTPBlendMode(BlendMode blendMode)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (s_previousBlendMode == blendMode && s_previousMixedSTPBlend)
	{
		return;
	}

	if (blendMode == BM_NONE || blendMode == BM_SUBTRACT)
	{
		NativeRenderer_SetBlendMode(blendMode);
		return;
	}

	if (s_previousBlendMode == BM_NONE)
	{
		glEnable(GL_BLEND);
	}

	NativeRenderer_EnableDepth(0);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
	glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ONE, GL_ZERO);
	s_previousBlendMode = blendMode;
	s_previousMixedSTPBlend = 1;
#else
	NativeRenderer_SetBlendMode(blendMode);
#endif
}

internal void NativeRenderer_SetViewPort(int x, int y, int width, int height)
{
	glViewport(x, y, width, height);
}

internal void NativeRenderer_SetWireframe(int enable)
{
#ifdef __EMSCRIPTEN__
	(void)enable;
#else
	glPolygonMode(GL_FRONT_AND_BACK, enable ? GL_LINE : GL_FILL);
#endif
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
/* Binds the frame's vertex buffer and returns count vertices of GPU-visible
 * scratch memory to fill before NativeRenderer_CommitVertexBuffer. */
GrVertex *NativeRenderer_MapVertexBuffer(int count)
{
	if (count <= 0 || (u32)count > MAX_VERTEX_BUFFER_SIZE)
	{
		return NULL;
	}
	s_boundVertexBuffer = s_curVertexBuffer;
	glBindVertexArray(s_glVertexArray[s_curVertexBuffer]);
	glBindBuffer(GL_ARRAY_BUFFER, s_glVertexBuffer[s_curVertexBuffer]);
	return NativeRenderer_AllocateVertexBuffer(count);
}

void NativeRenderer_CommitVertexBuffer(GrVertex *mapped)
{
	vglBufferData(GL_ARRAY_BUFFER, mapped);
}
#endif

void NativeRenderer_UpdateVertexBuffer(const GrVertex *vertices, int num_vertices)
{
	if (num_vertices <= 0)
	{
		return;
	}
	if ((u32)num_vertices >= MAX_VERTEX_BUFFER_SIZE)
	{
		NATIVE_RENDERER_ERROR("%s\n", "MAX_VERTEX_BUFFER_SIZE reached, expect rendering errors");
		num_vertices = MAX_VERTEX_BUFFER_SIZE;
	}

	const int bufferIndex = s_curVertexBuffer;
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
	s_curVertexBuffer = (s_curVertexBuffer + 1) % MAX_NUM_VERTEX_BUFFERS;
#endif
	s_boundVertexBuffer = bufferIndex;
	glBindVertexArray(s_glVertexArray[bufferIndex]);
	glBindBuffer(GL_ARRAY_BUFFER, s_glVertexBuffer[bufferIndex]);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	GrVertex *gpuVertices = NativeRenderer_AllocateVertexBuffer(num_vertices);
	if (gpuVertices == NULL)
	{
		return;
	}
	memcpy(gpuVertices, vertices, (size_t)num_vertices * sizeof(GrVertex));
	vglBufferData(GL_ARRAY_BUFFER, gpuVertices);
#else
	glBufferSubData(GL_ARRAY_BUFFER, 0, num_vertices * sizeof(GrVertex), vertices);
#endif

}

GrVertex *NativeRenderer_AllocateVertexBuffer(int count)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (count <= 0 || (u32)count > MAX_VERTEX_BUFFER_SIZE)
	{
		return NULL;
	}
	return (GrVertex *)vglAllocFromScratch((size_t)count * sizeof(GrVertex));
#else
	(void)count;
	return NULL;
#endif
}

void NativeRenderer_DrawTriangles(int start_vertex, int triangles)
{
#if defined(__vita__) && defined(VITA_CTR_GPU_TRACE)
	static int traceDrawCount;
	if (traceDrawCount < 24)
	{
		GLint framebuffer = 0;
		GLint program = 0;
		GLint viewport[4] = {0, 0, 0, 0};
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
		glGetIntegerv(GL_CURRENT_PROGRAM, &program);
		glGetIntegerv(GL_VIEWPORT, viewport);
		SysPrintf("pcsx-vita: gpu-trace draw[%d] first=%d triangles=%d fbo=%d program=%d vbo-index=%d viewport=%d,%d %dx%d depth=%d blend=%d stencil=%d scissor=%d\n",
		          traceDrawCount, start_vertex, triangles, framebuffer, program, s_boundVertexBuffer,
		          viewport[0], viewport[1], viewport[2], viewport[3],
		          glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_BLEND), glIsEnabled(GL_STENCIL_TEST), glIsEnabled(GL_SCISSOR_TEST));
		traceDrawCount++;
	}
#endif
	glDrawArrays(GL_TRIANGLES, start_vertex, triangles * 3);
}

void NativeRenderer_PushDebugLabel(const char *label)
{
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH) && !defined(__EMSCRIPTEN__)
	if (!GLAD_GL_KHR_debug)
	{
		return;
	}
	glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0x8000, strlen(label), label);
#endif
}

void NativeRenderer_PopDebugLabel(void)
{
#if !defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH) && !defined(__EMSCRIPTEN__)
	if (!GLAD_GL_KHR_debug)
	{
		return;
	}
	glPopDebugGroup();
#endif
}

void VitaCtrNativeRenderer_SetPresentationAspect(int width, int height)
{
    NativeRenderer_SetPresentationAspect(width, height);
    NativeRenderer_UpdatePresentationViewport();
}

void VitaCtrNativeRenderer_CopyGuestVRAM(const u16 *src, int x, int y, int w, int h)
{
    if (src == NULL || w <= 0 || h <= 0)
        return;

    NativeRenderer_CommitTargets();
    for (int row = 0; row < h; row++)
    {
        const int yy = (y + row) & (VRAM_HEIGHT - 1);
        int xx = x & (VRAM_WIDTH - 1);
        int remaining = w;
        while (remaining > 0)
        {
            const int chunk = SDL_min(remaining, VRAM_WIDTH - xx);
            SDL_memcpy(s_vram.cpuPixels + yy * VRAM_WIDTH + xx,
                       src + yy * VRAM_WIDTH + xx,
                       (size_t)chunk * sizeof(u16));
            NativeRenderer_MarkVRAMDirty(xx, yy, chunk, 1);
            remaining -= chunk;
            xx = 0;
        }
    }
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
    NativeRenderer_ClearGpuNewerCovered(x & (VRAM_WIDTH - 1), y & (VRAM_HEIGHT - 1), w, h);
#endif
}

/* CopyGuestVRAM from a packed w x h block; marks each wrap segment once. */
void VitaCtrNativeRenderer_CopyPackedVRAM(const u16 *src, int x, int y, int w, int h)
{
    if (src == NULL || w <= 0 || h <= 0)
        return;

    /* A target is packed into VRAM first only when the upload writes into
     * its rect (its later store would overwrite the upload); a CLUT row
     * elsewhere leaves the targets as they are. */
    {
        RECT16 rect;
        rect.x = (s16)(x & (VRAM_WIDTH - 1));
        rect.y = (s16)(y & (VRAM_HEIGHT - 1));
        rect.w = (s16)w;
        rect.h = (s16)h;
        const int wraps = rect.x + w > VRAM_WIDTH || rect.y + h > VRAM_HEIGHT;
        if (wraps || NativeRenderer_VRAMRectsOverlap(&rect, &s_mainDrawRect))
            NativeRenderer_FlushMainToVRAM();
        if (wraps || NativeRenderer_VRAMRectsOverlap(&rect, &s_previousOffscreen))
            NativeRenderer_FlushOffscreenToVRAM();
    }
    x &= VRAM_WIDTH - 1;
    y &= VRAM_HEIGHT - 1;
    const int firstWidth = SDL_min(w, VRAM_WIDTH - x);
    for (int row = 0; row < h; row++)
    {
        const int yy = (y + row) & (VRAM_HEIGHT - 1);
        SDL_memcpy(s_vram.cpuPixels + yy * VRAM_WIDTH + x, src, (size_t)firstWidth * sizeof(u16));
        if (firstWidth < w)
            SDL_memcpy(s_vram.cpuPixels + yy * VRAM_WIDTH, src + firstWidth, (size_t)(w - firstWidth) * sizeof(u16));
        src += w;
    }
    const int firstHeight = SDL_min(h, VRAM_HEIGHT - y);
    NativeRenderer_MarkVRAMDirty(x, y, firstWidth, firstHeight);
    if (firstWidth < w)
        NativeRenderer_MarkVRAMDirty(0, y, w - firstWidth, firstHeight);
    if (firstHeight < h)
    {
        NativeRenderer_MarkVRAMDirty(x, 0, firstWidth, h - firstHeight);
        if (firstWidth < w)
            NativeRenderer_MarkVRAMDirty(0, 0, w - firstWidth, h - firstHeight);
    }
}

void VitaCtrNativeRenderer_ReadGuestVRAM(u16 *dst, int x, int y, int w, int h)
{
    if (dst == NULL || w <= 0 || h <= 0)
        return;

    for (int row = 0; row < h; row++)
    {
        const int yy = (y + row) & (VRAM_HEIGHT - 1);
        int xx = x & (VRAM_WIDTH - 1);
        int remaining = w;
        while (remaining > 0)
        {
            const int chunk = SDL_min(remaining, VRAM_WIDTH - xx);
            NativeRenderer_ResolveVRAMRead(xx, yy, chunk, 1);
            SDL_memcpy(dst + yy * VRAM_WIDTH + xx,
                       s_vram.cpuPixels + yy * VRAM_WIDTH + xx,
                       (size_t)chunk * sizeof(u16));
            remaining -= chunk;
            xx = 0;
        }
    }
}

/* "vram4 PASS_STP BLEND_AVERAGE": the source's role and its PSX_* defines,
 * the draw name in GPU profiler captures. */
internal void NativeRenderer_ProgramLabel(char *name, size_t size, const char *source, const char *fragmentDefines)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	const char *role = source == gte_shader_4             ? "vram4"
	                   : source == gte_shader_8           ? "vram8"
	                   : source == gte_shader_16          ? "vram16"
	                   : source == gte_shader_32_rgba     ? "rgba32"
	                   : source == gte_shader_cached_p4   ? "p4"
	                   : source == gte_shader_untextured  ? "untextured"
	                                                      : "other";
	size_t used = (size_t)SDL_snprintf(name, size, "%s", role);
	for (const char *d = fragmentDefines; d && (d = SDL_strstr(d, "#define PSX_")) != NULL && used + 2 < size;)
	{
		d += 12;
		name[used++] = ' ';
		while (*d && *d != '\n' && used + 1 < size)
			name[used++] = *d++;
		name[used] = 0;
	}
#else
	(void)source;
	(void)fragmentDefines;
	if (size)
		name[0] = 0;
#endif
}
