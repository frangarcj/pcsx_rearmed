/* Derived from REDRIVER2/PsyCross MIT source: externals/PsyCross/src/gpu/PsyX_GPU.cpp See THIRD_PARTY_NOTICES.md for copyright and license details. */

#include <SDL3/SDL.h>
#include <macros.h>
#include "platform/native_vita_path.h"
#include "platform/native_gpu.h"

#include <platform.h>

#include "platform/native_log.h"
#include "platform/native_renderer.h"

#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__vita__) && defined(VITA_CTR_GPU_TRACE)
#include "../../libpcsxcore/system.h"
#endif
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/lw_cond.h>
#include <psp2/kernel/threadmgr/lw_mutex.h>
#include <vitaGL.h>
#endif

void Platform_PollHostEvents(void);
extern int g_cfg_bilinearFiltering;
extern int g_dbg_emulatorPaused;
extern int g_dbg_polygonSelected;

#define NATIVE_GPU_LOG(fmt, ...)   Platform_Log("[CTR GPU] " fmt, __VA_ARGS__)
#define NATIVE_GPU_ERROR(fmt, ...) Platform_LogError("[CTR GPU] [%s] - " fmt, __func__, __VA_ARGS__)

// NOTE(aalhendi): Little-endian tag `CTRG` = CTR native GPU snapshot.
#define NATIVE_GPU_STATE_MAGIC     0x47525443
#define NATIVE_GPU_STATE_VERSION   1

#define GET_TPAGE_BLEND(tpage)     ((BlendMode)(((tpage >> 5) & 3) + 1))

#define GET_TPAGE_DITHER(tpage)    ((tpage >> 9) & 0x1)

#define GET_CLUT_X(clut)           ((clut & 0x3F) << 4)
#define GET_CLUT_Y(clut)           (clut >> 6)
/* 0x8000 is a High Octane private TPAGE marker, not part of generic PS1 GP0. Real games may leave that high bit set, so never interpret it as a tint here. */
#define NATIVE_GPU_TPAGE_SUPER_TURBO_TINT 0u

internal TexFormat GetTPageFormat(int tpage)
{
	const int mode = (tpage >> 7) & 0x3;

	// NOTE(aalhendi): ctr-native local divergence from upstream PsyCross. PS1 mode 3 is reserved; TF_32_BIT_RGBA is only for explicit native override textures, not raw retail TPAGE mode bits.
	return mode == 3 ? TF_16_BIT : (TexFormat)mode;
}

internal short GetTPageBase(int tpage)
{
	const u16 page = (u16)tpage;

	// NOTE(aalhendi): ctr-native local divergence for CTR retail emitters. The shader wants xPage + yPage * 16, not raw draw-mode bits.
	return (s16)((page & 0x1f) | ((VRAM_HEIGHT > 512) ? ((page >> 6) & 0x20) : 0));
}

internal s16 NativeGpu_SignExtend11(u32 value)
{
	value &= 0x7ff;
	return (s16)((value ^ 0x400) - 0x400);
}

DISPENV activeDispEnv;
DRAWENV activeDrawEnv;
int g_GPUDisabledState = 0;

typedef struct
{
	DRAWENV drawenv;
	DISPENV dispenv;

	BlendMode blendMode;

	TexFormat texFormat;
	TextureID textureId;

	int drawPrimMode;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	RECT16 clearRect;
	u8 clearR;
	u8 clearG;
	u8 clearB;
	/* A CPU upload of clearRect queued with the frame (a drawPrimMode 2
	 * split): its texels, packed, in the packet's upload storage. */
	const u16 *uploadPixels;
	bool uploadApplied;
#endif
	bool psxTexturedSemiTrans;
	bool psxTextureOutputSTP;
	bool psxDrawMaskSet;
	bool superTurboTint;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	bool psxTextureFullyOpaque;
	bool p4CacheEligible;
	bool p4SuperTurboTint;
	s16 p4Page;
	u16 p4Clut;
	/* Bounds of the split's first boundsVerts vertices (frontend only):
	 * vertices are only appended, so NativeGpu_SplitWriteRect extends them
	 * instead of rescanning the split at every query. */
	u16 boundsVerts;
	s16 boundsMinX, boundsMaxX, boundsMinY, boundsMaxY;
#endif
	s16 overrideTextureWidth;
	s16 overrideTextureHeight;
	u8 psxSemiTransPassMask;

	u16 startVertex;
	u16 numVerts;

	const char *debugText;
} GPUDrawSplit;

#define MAX_DRAW_SPLITS 4096

#define NATIVE_GPU_PENDING_TILE_SHIFT 6
#define NATIVE_GPU_PENDING_TILE_COLS (1024 >> NATIVE_GPU_PENDING_TILE_SHIFT)
#define NATIVE_GPU_PENDING_TILE_ROWS (1024 >> NATIVE_GPU_PENDING_TILE_SHIFT)
#define NATIVE_GPU_PENDING_TILE_WORDS (NATIVE_GPU_PENDING_TILE_COLS * NATIVE_GPU_PENDING_TILE_ROWS / 32)

typedef struct
{
	const char *currentSplitDebugText;
	TextureID overrideTexture;
	int overrideTextureWidth;
	int overrideTextureHeight;
	bool overrideTexturePsxStp;

	int drawPrimMode;
	bool psxDrawMaskSet;
	bool framebufferFeedbackRunActive;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	u32 primitiveOrder;
	u16 currentPrimitiveOrder;
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	GrVertex *vertexBuffer;
	GPUDrawSplit *splits;
#else
	GrVertex vertexBuffer[MAX_VERTEX_BUFFER_SIZE];
	GPUDrawSplit splits[MAX_DRAW_SPLITS];
#endif
	int vertexIndex;
	int splitIndex;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	u16 *uploadStorage;
	int uploadTexels;
	/* Set once the packet holds anything but queued uploads. */
	bool packetHasDraws;
	/* 64x64 VRAM tiles the pending splits read (texture pages, CLUTs) or
	 * write (draw areas, fills), and those of the queued uploads. */
	u32 pendingTiles[NATIVE_GPU_PENDING_TILE_WORDS];
	u32 uploadTiles[NATIVE_GPU_PENDING_TILE_WORDS];
	/* The draw area and texture sources AddSplit last marked in
	 * pendingTiles: a run of primitives with the same state marks them
	 * once (marks only accumulate until ClearSplits). */
	RECT16 markedClip;
	bool markedSources;
	int markedTpage;
	int markedClut;
#endif
} NativeGpuState;

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
/* Texels of CPU VRAM uploads a frame packet can carry (512 KB). */
#define NATIVE_GPU_UPLOAD_TEXELS (256 * 1024)

typedef struct
{
	GrVertex vertices[MAX_VERTEX_BUFFER_SIZE];
	GPUDrawSplit splits[MAX_DRAW_SPLITS];
	u16 uploads[NATIVE_GPU_UPLOAD_TEXELS];
	DRAWENV drawEnv;
	DISPENV dispEnv;
	int vertexCount;
	int splitCount;
	int blankDisplay;
	/* A frame is one packet, or several when an upload had to be ordered
	 * between its draws: only the first begins the scene and only the last
	 * ends (presents) it. */
	int beginScene;
	int endScene;
} NativeGpuFramePacket;

/* Three, so the frontend can queue a frame's first packet while the
 * renderer still draws the previous frame's last one (the split at an
 * upload a pending draw uses): with two it waited for it. */
#define NATIVE_GPU_FRAME_PACKET_COUNT 3

global_variable NativeGpuFramePacket s_gpuFramePackets[NATIVE_GPU_FRAME_PACKET_COUNT] __attribute__((aligned(64)));
global_variable int s_gpuFrontendPacketIndex;
global_variable int s_gpuFrontendFramePrepared;
global_variable int s_gpuSynchronousFrame;
/* The frame being built already had a packet submitted: its scene began. */
global_variable int s_gpuFrameBegun;
/* 64x64 VRAM tiles where splits handed to the backend may have drawn since
 * the guest VRAM was last read back: elsewhere the guest copy is current,
 * as only the CPU wrote there. */
global_variable u32 s_gpuGuestStaleTiles[NATIVE_GPU_PENDING_TILE_WORDS];
#ifdef __vita__
/* The packet the renderer thread is drawing (that thread only). */
global_variable NativeGpuFramePacket *s_gpuBackendPacket;
/* Submitted packets the renderer has not finished, oldest first from
 * s_gpuBackendPacketIndex: the frontend's packet is the next slot. */
global_variable int s_gpuBackendPacketIndex;
global_variable int s_gpuPacketsQueued;
global_variable NativeGpuBackendTaskFn s_gpuBackendTask;
global_variable void *s_gpuBackendTaskArg;
global_variable DRAWENV s_gpuBackendDrawEnv;
global_variable DISPENV s_gpuBackendDispEnv;
global_variable DRAWENV s_gpuBackendTaskDrawEnv;
global_variable DISPENV s_gpuBackendTaskDispEnv;
global_variable SceKernelLwMutexWork s_gpuBackendMutex;
global_variable SceKernelLwCondWork s_gpuBackendCond;
global_variable SceUID s_gpuBackendThread = -1;
/* A synchronous task posted and not finished yet. */
global_variable int s_gpuBackendTaskPending;
global_variable int s_gpuBackendInitComplete;
global_variable volatile int s_gpuBackendExit;
global_variable int s_gpuBackendEnabled;
global_variable int s_gpuBackendInitWidth;
global_variable int s_gpuBackendInitHeight;
global_variable int s_gpuBackendInitSucceeded;
#endif
#endif

global_variable GrVertex *s_gpuDrawVertices;
global_variable GPUDrawSplit *s_gpuDrawSplits;
global_variable int s_gpuDrawVertexCount;
global_variable int s_gpuDrawSplitCount;
global_variable int s_pcsxRenderHadGeometry;
global_variable NativeGpuState s_gpu;

internal void NativeGpu_DrawPreparedFrame(GrVertex *vertices, GPUDrawSplit *splits, int vertexCount, int splitCount);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
internal void NativeGpu_ApplyQueuedUploads(GPUDrawSplit *splits, int splitCount);
internal void NativeGpu_MarkPacketWrites(void);
internal bool NativeGpu_RectOverlaps(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh);
#endif

void NativeGpu_ResetOrderDepth(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	s_gpu.primitiveOrder = 0;
	s_gpu.currentPrimitiveOrder = 0;
#endif
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
internal void NativeGpu_BindFrontendPacket(void)
{
	NativeGpuFramePacket *packet = &s_gpuFramePackets[s_gpuFrontendPacketIndex];
	s_gpu.vertexBuffer = packet->vertices;
	s_gpu.splits = packet->splits;
	s_gpu.uploadStorage = packet->uploads;
}
#endif

#ifdef __vita__
internal void NativeGpu_BackendLock(void)
{
	sceKernelLockLwMutex(&s_gpuBackendMutex, 1, NULL);
}

internal void NativeGpu_BackendUnlock(void)
{
	sceKernelUnlockLwMutex(&s_gpuBackendMutex, 1);
}

/* Microseconds and count of the times one thread blocked on the other,
 * read by the frontend's progress lines (NATIVE_GPU_WAIT_*). */
unsigned long long pcsx_vita_ctr_wait_us[NATIVE_GPU_WAIT_COUNT];
unsigned pcsx_vita_ctr_wait_n[NATIVE_GPU_WAIT_COUNT];

internal void NativeGpu_WaitAdd(int slot, SceUInt64 start)
{
	pcsx_vita_ctr_wait_us[slot] += sceKernelGetProcessTimeWide() - start;
	pcsx_vita_ctr_wait_n[slot]++;
}

internal int NativeGpu_BackendIdleLocked(void)
{
	return s_gpuPacketsQueued == 0 && !s_gpuBackendTaskPending;
}

internal void NativeGpu_BackendWaitIdleLocked(void)
{
	while (!NativeGpu_BackendIdleLocked())
	{
		sceKernelWaitLwCond(&s_gpuBackendCond, NULL);
	}
}

internal void NativeGpu_BackendWaitIdleTimed(int slot)
{
	if (NativeGpu_BackendIdleLocked())
		return;
	const SceUInt64 start = sceKernelGetProcessTimeWide();
	NativeGpu_BackendWaitIdleLocked();
	NativeGpu_WaitAdd(slot, start);
}

/* Queues the frontend's packet for the renderer and moves the frontend to
 * the next slot, once the renderer has finished the packet that used it. */
internal void NativeGpu_BackendQueuePacketLocked(int waitSlot)
{
	s_gpuPacketsQueued++;
	s_gpuFrontendPacketIndex = (s_gpuFrontendPacketIndex + 1) % NATIVE_GPU_FRAME_PACKET_COUNT;
	sceKernelSignalLwCond(&s_gpuBackendCond);
	if (s_gpuPacketsQueued >= NATIVE_GPU_FRAME_PACKET_COUNT)
	{
		const SceUInt64 start = sceKernelGetProcessTimeWide();
		while (s_gpuPacketsQueued >= NATIVE_GPU_FRAME_PACKET_COUNT)
		{
			sceKernelWaitLwCond(&s_gpuBackendCond, NULL);
		}
		NativeGpu_WaitAdd(waitSlot, start);
	}
	NativeGpu_BindFrontendPacket();
}

internal void NativeGpu_BackendApplyEnv(const DRAWENV *drawEnv, const DISPENV *dispEnv)
{
	s_gpuBackendDrawEnv = *drawEnv;
	s_gpuBackendDispEnv = *dispEnv;
}

typedef struct
{
	GrVertex *vertices;
	GPUDrawSplit *splits;
	int vertexCount;
	int splitCount;
} NativeGpuBackendDrawTask;

internal void NativeGpu_BackendBeginSceneTask(void *arg)
{
	(void)arg;
	Platform_BeginScene();
}

internal void NativeGpu_BackendEndSceneTask(void *arg)
{
	(void)arg;
	Platform_EndScene();
}

internal void NativeGpu_BackendDrawTaskMain(void *arg)
{
	NativeGpuBackendDrawTask *task = (NativeGpuBackendDrawTask *)arg;
	NativeGpu_DrawPreparedFrame(task->vertices, task->splits, task->vertexCount, task->splitCount);
}

typedef struct
{
	int x;
	int y;
	int w;
	int h;
	u8 r;
	u8 g;
	u8 b;
} NativeGpuBackendRectTask;

internal void NativeGpu_BackendStoreFrameBufferTask(void *arg)
{
	(void)arg;
	/* The current GP0 environment need not describe the last rendered split.
	 * Feedback must commit both owned targets before sampling VRAM. */
	NativeRenderer_EndScene();
}

internal void NativeGpu_BackendClearTask(void *arg)
{
	NativeGpuBackendRectTask *task = (NativeGpuBackendRectTask *)arg;
    NativeRenderer_ClearVRAM(task->x, task->y, task->w, task->h, task->r, task->g, task->b);
    NativeRenderer_UpdateVRAM();
}

internal void NativeGpu_BackendSyncVRAMToCPUTask(void *arg)
{
	NativeGpuBackendRectTask *task = (NativeGpuBackendRectTask *)arg;
	NativeRenderer_SyncVRAMToCPU(task->x, task->y, task->w, task->h);
}

/* vitaGL's multisampling: 4, 2 or 0 (the standalone frontend's --gl-msaa). */
int pcsx_vita_gl_msaa = 4;

internal int NativeGpu_BackendThreadMain(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	vglSetupRenderTargetScenesNum(2, 2);
	vglSetShaderCachePath("ux0:data/pcsx-rearmed/shader_cache");
	vglUseLowPrecision(GL_TRUE);
	vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, GL_TRUE, GL_TRUE, GL_TRUE);
	/* vitaGL's init result reports whether it had to fall back to a smaller
	 * display resolution. GL_FALSE is the normal exact-resolution result; it
	 * does not mean initialization failed. This API has no failure result once
	 * initialization has run to completion. */
	/* vitaGL takes all free user memory but the threshold for its pool. GXM
	 * allocates render targets' driver memory outside it, on the fly as draw
	 * areas change: with 4 MB, HD 4x MSAA targets ran out of physical pages
	 * on hardware and sceGxmBeginScene failed. */
	extern int pcsx_vita_gl_msaa;
	const GLboolean resolutionFallback = vglInitExtended(0, s_gpuBackendInitWidth, s_gpuBackendInitHeight,
	                                                    32 * 1024 * 1024,
	                                                    pcsx_vita_gl_msaa >= 4 ? SCE_GXM_MULTISAMPLE_4X
	                                                    : pcsx_vita_gl_msaa >= 2 ? SCE_GXM_MULTISAMPLE_2X
	                                                    : SCE_GXM_MULTISAMPLE_NONE);
	if (resolutionFallback)
	{
		NATIVE_GPU_LOG("%s\n", "vitaGL reduced display resolution to its maximum supported size");
	}
	NativeGpu_BackendLock();
	s_gpuBackendInitSucceeded = 1;
	s_gpuBackendInitComplete = 1;
	sceKernelSignalLwCondAll(&s_gpuBackendCond);
	NativeGpu_BackendUnlock();
	NativeGpu_BackendLock();
	for (;;)
	{
		if (NativeGpu_BackendIdleLocked() && !s_gpuBackendExit)
		{
			const SceUInt64 idleStart = sceKernelGetProcessTimeWide();
			while (NativeGpu_BackendIdleLocked() && !s_gpuBackendExit)
			{
				sceKernelWaitLwCond(&s_gpuBackendCond, NULL);
			}
			NativeGpu_WaitAdd(NATIVE_GPU_WAIT_RENDER_IDLE, idleStart);
		}
		if (s_gpuBackendExit)
		{
			break;
		}
		/* Tasks are posted only once the queue is empty. */
		NativeGpuFramePacket *packet = NULL;
		NativeGpuBackendTaskFn task = NULL;
		void *taskArg = NULL;
		if (s_gpuPacketsQueued > 0)
		{
			packet = &s_gpuFramePackets[s_gpuBackendPacketIndex];
			NativeGpu_BackendApplyEnv(&packet->drawEnv, &packet->dispEnv);
		}
		else
		{
			task = s_gpuBackendTask;
			taskArg = s_gpuBackendTaskArg;
			NativeGpu_BackendApplyEnv(&s_gpuBackendTaskDrawEnv, &s_gpuBackendTaskDispEnv);
		}
		s_gpuBackendPacket = packet;
		NativeGpu_BackendUnlock();
		const SceUInt64 workStart = sceKernelGetProcessTimeWide();
		if (packet != NULL)
		{
			if (packet->beginScene)
				s_pcsxRenderHadGeometry = 0;
			s_pcsxRenderHadGeometry |= (packet->vertexCount > 0 || packet->splitCount > 0) ? 1 : 0;
			NativeGpu_ApplyQueuedUploads(packet->splits, packet->splitCount);
			if (packet->beginScene)
				Platform_BeginScene();
			NativeGpu_DrawPreparedFrame(packet->vertices, packet->splits, packet->vertexCount, packet->splitCount);
			if (packet->endScene)
				Platform_EndScene();
		}
		else if (task != NULL)
		{
			task(taskArg);
		}
		NativeGpu_WaitAdd(packet != NULL ? NATIVE_GPU_WAIT_RENDER_PACKET : NATIVE_GPU_WAIT_RENDER_TASK, workStart);
		NativeGpu_BackendLock();
		s_gpuBackendPacket = NULL;
		if (packet != NULL)
		{
			s_gpuBackendPacketIndex = (s_gpuBackendPacketIndex + 1) % NATIVE_GPU_FRAME_PACKET_COUNT;
			s_gpuPacketsQueued--;
		}
		else
		{
			s_gpuBackendTask = NULL;
			s_gpuBackendTaskArg = NULL;
			s_gpuBackendTaskPending = 0;
		}
		sceKernelSignalLwCondAll(&s_gpuBackendCond);
	}
	NativeGpu_BackendUnlock();
	sceKernelExitThread(0);
	return 0;
}

#endif

int NativeGpu_InitBackend(int width, int height)
{
#ifdef __vita__
	if (s_gpuBackendEnabled)
	{
		return 1;
	}
	s_gpuFrontendPacketIndex = 0;
	s_gpuFrontendFramePrepared = 0;
	s_gpuSynchronousFrame = 0;
	s_gpuBackendPacket = NULL;
	s_gpuBackendPacketIndex = 0;
	s_gpuPacketsQueued = 0;
	s_gpuBackendTask = NULL;
	s_gpuBackendTaskArg = NULL;
	s_gpuBackendTaskPending = 0;
	s_gpuBackendInitComplete = 0;
	s_gpuBackendExit = 0;
	s_gpuBackendInitWidth = width;
	s_gpuBackendInitHeight = height;
	s_gpuBackendInitSucceeded = 0;
	NativeGpu_BindFrontendPacket();
	if (sceKernelCreateLwMutex(&s_gpuBackendMutex, "CTR Render Lock", 0, 0, NULL) < 0)
	{
		return 0;
	}
	if (sceKernelCreateLwCond(&s_gpuBackendCond, "CTR Render Cond", 0, &s_gpuBackendMutex, NULL) < 0)
	{
		sceKernelDeleteLwMutex(&s_gpuBackendMutex);
		return 0;
	}
	/* Core 1: the emulation runs on core 0 and the System 12 board thread on
	 * core 2 (frontend/pcsxr-threads.c); left to the scheduler, the renderer
	 * also ran on core 2 and the main thread waited for the board. */
	s_gpuBackendThread = sceKernelCreateThread("CTR Renderer", NativeGpu_BackendThreadMain, 0x10000100, 0x40000, 0,
	                                           SCE_KERNEL_CPU_MASK_USER_1, NULL);
	if (s_gpuBackendThread < 0)
	{
		sceKernelDeleteLwCond(&s_gpuBackendCond);
		sceKernelDeleteLwMutex(&s_gpuBackendMutex);
		return 0;
	}
	if (sceKernelStartThread(s_gpuBackendThread, 0, NULL) < 0)
	{
		sceKernelDeleteThread(s_gpuBackendThread);
		s_gpuBackendThread = -1;
		sceKernelDeleteLwCond(&s_gpuBackendCond);
		sceKernelDeleteLwMutex(&s_gpuBackendMutex);
		return 0;
	}
	NativeGpu_BackendLock();
	while (!s_gpuBackendInitComplete)
	{
		sceKernelWaitLwCond(&s_gpuBackendCond, NULL);
	}
	const int initSucceeded = s_gpuBackendInitSucceeded;
	NativeGpu_BackendUnlock();
	if (!initSucceeded)
	{
		sceKernelWaitThreadEnd(s_gpuBackendThread, NULL, NULL);
		sceKernelDeleteThread(s_gpuBackendThread);
		s_gpuBackendThread = -1;
		sceKernelDeleteLwCond(&s_gpuBackendCond);
		sceKernelDeleteLwMutex(&s_gpuBackendMutex);
		return 0;
	}
	s_gpuBackendEnabled = 1;
	return 1;
#elif defined(PCSX_NATIVE_VITA_PATH)
	(void)width;
	(void)height;
	s_gpuFrontendPacketIndex = 0;
	s_gpuFrontendFramePrepared = 0;
	s_gpuSynchronousFrame = 0;
	NativeGpu_BindFrontendPacket();
	return 1;
#else
	(void)width;
	(void)height;
	return 1;
#endif
}

void NativeGpu_SyncBackend(void)
{
#ifdef __vita__
	if (!s_gpuBackendEnabled)
	{
		return;
	}
	NativeGpu_BackendLock();
	NativeGpu_BackendWaitIdleTimed(NATIVE_GPU_WAIT_SYNC);
	NativeGpu_BackendUnlock();
#endif
}

void NativeGpu_RunBackendTaskSync(NativeGpuBackendTaskFn task, void *arg)
{
#ifdef __vita__
	if (task == NULL)
	{
		return;
	}
	if (!s_gpuBackendEnabled)
	{
		task(arg);
		return;
	}
	NativeGpu_BackendLock();
	NativeGpu_BackendWaitIdleTimed(NATIVE_GPU_WAIT_TASK_PREVIOUS);
	s_gpuBackendTask = task;
	s_gpuBackendTaskArg = arg;
	s_gpuBackendTaskDrawEnv = activeDrawEnv;
	s_gpuBackendTaskDispEnv = activeDispEnv;
	s_gpuBackendTaskPending = 1;
	sceKernelSignalLwCond(&s_gpuBackendCond);
	NativeGpu_BackendWaitIdleTimed(NATIVE_GPU_WAIT_TASK);
	NativeGpu_BackendUnlock();
#else
	if (task != NULL)
	{
		task(arg);
	}
#endif
}

void NativeGpu_SyncVRAMToCPU(int x, int y, int w, int h)
{
#ifdef __vita__
	NativeGpuBackendRectTask task = {x, y, w, h, 0, 0, 0};
	NativeGpu_RunBackendTaskSync(NativeGpu_BackendSyncVRAMToCPUTask, &task);
#else
	NativeRenderer_SyncVRAMToCPU(x, y, w, h);
#endif
}

void NativeGpu_ShutdownBackend(void)
{
#ifdef __vita__
	if (!s_gpuBackendEnabled)
	{
		return;
	}
	NativeGpu_BackendLock();
	NativeGpu_BackendWaitIdleLocked();
	s_gpuBackendExit = 1;
	sceKernelSignalLwCond(&s_gpuBackendCond);
	NativeGpu_BackendUnlock();
	sceKernelWaitThreadEnd(s_gpuBackendThread, NULL, NULL);
	sceKernelDeleteThread(s_gpuBackendThread);
	s_gpuBackendThread = -1;
	sceKernelDeleteLwCond(&s_gpuBackendCond);
	sceKernelDeleteLwMutex(&s_gpuBackendMutex);
	s_gpuBackendEnabled = 0;
#endif
}

void NativeGpu_BeginFrontendFrame(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	NativeGpu_BindFrontendPacket();
	NativeGpuFramePacket *packet = &s_gpuFramePackets[s_gpuFrontendPacketIndex];
	packet->drawEnv = activeDrawEnv;
	packet->dispEnv = activeDispEnv;
	packet->blankDisplay = 0;
	s_gpuFrontendFramePrepared = 1;
	s_gpuSynchronousFrame = 0;
#endif
	NativeGpu_ResetOrderDepth();
	ClearSplits();
}

void NativeGpu_SetFrontendDrawEnv(const DRAWENV *env)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (s_gpuFrontendFramePrepared && env != NULL)
	{
		s_gpuFramePackets[s_gpuFrontendPacketIndex].drawEnv = *env;
	}
#else
	(void)env;
#endif
}

void NativeGpu_SetFrontendDispEnv(const DISPENV *env)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (s_gpuFrontendFramePrepared && env != NULL)
	{
		s_gpuFramePackets[s_gpuFrontendPacketIndex].dispEnv = *env;
	}
#else
	(void)env;
#endif
}
void NativeGpu_SetFrontendDisplayBlank(int blank)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (s_gpuFrontendFramePrepared)
	{
		s_gpuFramePackets[s_gpuFrontendPacketIndex].blankDisplay = blank != 0;
	}
#else
	(void)blank;
#endif
}

int NativeGpu_GetRenderDisplayBlank(void)
{
#ifdef __vita__
	return (s_gpuBackendPacket != NULL) ? s_gpuBackendPacket->blankDisplay : 0;
#else
	return 0;
#endif
}

const DRAWENV *NativeGpu_GetRenderDrawEnv(void)
{
#ifdef __vita__
	return s_gpuBackendEnabled ? &s_gpuBackendDrawEnv : &activeDrawEnv;
#else
	return &activeDrawEnv;
#endif
}

const DISPENV *NativeGpu_GetRenderDispEnv(void)
{
#ifdef __vita__
	return s_gpuBackendEnabled ? &s_gpuBackendDispEnv : &activeDispEnv;
#else
	return &activeDispEnv;
#endif
}


void NativeGpu_ForceSynchronousFrame(void)
{
#ifdef __vita__
    if (!s_gpuSynchronousFrame)
    {
        if (s_gpuFrameBegun)
            NativeGpu_SyncBackend();
        else
            NativeGpu_RunBackendTaskSync(NativeGpu_BackendBeginSceneTask, NULL);
        s_gpuSynchronousFrame = 1;
    }
#elif defined(PCSX_NATIVE_VITA_PATH)
	if (!s_gpuSynchronousFrame)
	{
		if (!s_gpuFrameBegun)
			Platform_BeginScene();
		s_gpuSynchronousFrame = 1;
	}
#endif
}

void NativeGpu_FlushFrontendSplitsSync(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	NativeGpu_MarkPacketWrites();
#endif
#ifdef __vita__
	if (!NativeGpu_HasPendingSplits())
	{
		return;
	}
	NativeGpuBackendDrawTask task;
	task.vertices = s_gpu.vertexBuffer;
	task.splits = s_gpu.splits;
	task.vertexCount = s_gpu.vertexIndex;
	task.splitCount = s_gpu.splitIndex;
	NativeGpu_RunBackendTaskSync(NativeGpu_BackendDrawTaskMain, &task);
	ClearSplits();
	#elif defined(PCSX_NATIVE_VITA_PATH)
	DrawAllSplits();
#else
	DrawAllSplits();
#endif
}

void NativeGpu_FinishSynchronousFrame(void)
{
#ifdef __vita__
	if (!s_gpuSynchronousFrame)
	{
		return;
	}
	NativeGpu_FlushFrontendSplitsSync();
	NativeGpu_RunBackendTaskSync(NativeGpu_BackendEndSceneTask, NULL);
	s_gpuSynchronousFrame = 0;
	s_gpuFrontendFramePrepared = 0;
	s_gpuFrameBegun = 0;
#elif defined(PCSX_NATIVE_VITA_PATH)
	if (s_gpuSynchronousFrame)
	{
		NativeGpu_FlushFrontendSplitsSync();
		Platform_EndScene();
		s_gpuSynchronousFrame = 0;
		s_gpuFrontendFramePrepared = 0;
		s_gpuFrameBegun = 0;
	}
#endif
}

int NativeGpu_IsSynchronousFrame(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	return s_gpuSynchronousFrame;
#else
	return 1;
#endif
}

int NativeGpu_IsFrontendFrameActive(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	return s_gpuFrontendFramePrepared;
#else
	return 0;
#endif
}

/* Hands the splits built so far to the backend without ending the frame,
 * and continues it in the other packet. An upload that pending splits read
 * or write is then queued at the start of the new packet, instead of making
 * the rest of the frame synchronous. Returns 0 when it cannot. */
int NativeGpu_SubmitPartialFrame(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (!s_gpuFrontendFramePrepared || s_gpuSynchronousFrame || s_gpu.splitIndex <= 0)
	{
		return 0;
	}
	NativeGpu_MarkPacketWrites();
	NativeGpuFramePacket *packet = &s_gpuFramePackets[s_gpuFrontendPacketIndex];
	packet->vertexCount = s_gpu.vertexIndex;
	packet->splitCount = s_gpu.splitIndex;
	GPUDrawSplit *lastSplit = &s_gpu.splits[s_gpu.splitIndex];
	lastSplit->numVerts = s_gpu.vertexIndex - lastSplit->startVertex;
#ifdef __vita__
	if (!s_gpuBackendEnabled)
	{
		return 0;
	}
	const int blankDisplay = packet->blankDisplay;
	packet->beginScene = !s_gpuFrameBegun;
	packet->endScene = 0;
	NativeGpu_BackendLock();
	NativeGpu_BackendQueuePacketLocked(NATIVE_GPU_WAIT_PARTIAL);
	NativeGpu_BackendUnlock();
#else
	const int blankDisplay = packet->blankDisplay;
	NativeGpu_ApplyQueuedUploads(s_gpu.splits, s_gpu.splitIndex);
	if (!s_gpuFrameBegun)
		Platform_BeginScene();
	DrawAllSplits();
#endif
	ClearSplits();
	NativeGpuFramePacket *next = &s_gpuFramePackets[s_gpuFrontendPacketIndex];
	next->drawEnv = activeDrawEnv;
	next->dispEnv = activeDispEnv;
	next->blankDisplay = blankDisplay;
	s_gpuFrameBegun = 1;
	return 1;
#else
	return 0;
#endif
}

int NativeGpu_SubmitFrontendFrame(void)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (!s_gpuFrontendFramePrepared)
	{
		return 0;
	}
	NativeGpu_MarkPacketWrites();

	NativeGpuFramePacket *packet = &s_gpuFramePackets[s_gpuFrontendPacketIndex];
	packet->vertexCount = s_gpu.vertexIndex;
	packet->splitCount = s_gpu.splitIndex;

#ifdef __vita__
	if (!s_gpuBackendEnabled)
	{
		NativeGpu_ApplyQueuedUploads(s_gpu.splits, s_gpu.splitIndex);
		Platform_BeginScene();
		if (s_gpu.splitIndex > 0)
		{
			DrawAllSplits();
		}
		else
		{
			ClearSplits();
		}
		Platform_EndScene();
		s_gpuFrontendFramePrepared = 0;
		s_gpuSynchronousFrame = 0;
		return 1;
	}

        if (s_gpuSynchronousFrame)
        {
            NativeGpu_FinishSynchronousFrame();
            return 1;
        }

	packet->beginScene = !s_gpuFrameBegun;
	packet->endScene = 1;
	NativeGpu_BackendLock();
	NativeGpu_BackendQueuePacketLocked(NATIVE_GPU_WAIT_SUBMIT);
	NativeGpu_BackendUnlock();
	ClearSplits();
	s_gpuFrontendFramePrepared = 0;
	s_gpuSynchronousFrame = 0;
	s_gpuFrameBegun = 0;
	return 1;
#else
	if (s_gpuSynchronousFrame)
	{
		NativeGpu_FinishSynchronousFrame();
		return 1;
	}
	NativeGpu_ApplyQueuedUploads(s_gpu.splits, s_gpu.splitIndex);
	if (!s_gpuFrameBegun)
		Platform_BeginScene();
	if (s_gpu.splitIndex > 0)
		DrawAllSplits();
	else
		ClearSplits();
	Platform_EndScene();
	s_gpuFrontendFramePrepared = 0;
	s_gpuSynchronousFrame = 0;
	s_gpuFrameBegun = 0;
	return 1;
#endif
#else
	return 0;
#endif
}
struct NativeGpuSnapshot
{
	u32 magic;
	u32 version;
	u32 size;
	DRAWENV drawEnv;
	DISPENV dispEnv;
	s32 gpuDisabledState;
	s32 psxDrawMaskSet;
	u16 vram[];
};

#ifdef __vita__
typedef struct
{
	void *data;
	int size;
	int result;
} NativeGpuBackendVRAMStateTask;

internal void NativeGpu_BackendCaptureVRAMStateTask(void *arg)
{
	NativeGpuBackendVRAMStateTask *task = (NativeGpuBackendVRAMStateTask *)arg;
	task->result = NativeRenderer_CaptureVRAMState(task->data, task->size);
}

internal void NativeGpu_BackendRestoreVRAMStateTask(void *arg)
{
	NativeGpuBackendVRAMStateTask *task = (NativeGpuBackendVRAMStateTask *)arg;
	task->result = NativeRenderer_RestoreVRAMState(task->data, task->size);
}
#endif

int NativeGpu_HasPendingSplits(void)
{
	return s_gpu.splitIndex > 0;
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
/* Visits the 64x64 tiles of a VRAM rect, wrapping at the VRAM edges:
 * marks them in, or tests them against, the given tile set. */
internal int NativeGpu_VisitTiles(u32 *tiles, int x, int y, int w, int h, int mark)
{
	if (w <= 0 || h <= 0)
		return 0;
	if (w > VRAM_WIDTH)
		w = VRAM_WIDTH;
	if (h > VRAM_HEIGHT)
		h = VRAM_HEIGHT;
	x &= VRAM_WIDTH - 1;
	y &= VRAM_HEIGHT - 1;
	for (int yy = y; yy < y + h; yy = (yy | 63) + 1)
	{
		const int row = (yy & (VRAM_HEIGHT - 1)) >> NATIVE_GPU_PENDING_TILE_SHIFT;
		for (int xx = x; xx < x + w; xx = (xx | 63) + 1)
		{
			const int col = (xx & (VRAM_WIDTH - 1)) >> NATIVE_GPU_PENDING_TILE_SHIFT;
			const int tile = row * NATIVE_GPU_PENDING_TILE_COLS + col;
			if (mark)
				tiles[tile >> 5] |= 1u << (tile & 31);
			else if (tiles[tile >> 5] & (1u << (tile & 31)))
				return 1;
		}
	}
	return 0;
}

internal int NativeGpu_PendingTiles(int x, int y, int w, int h, int mark)
{
	return NativeGpu_VisitTiles(s_gpu.pendingTiles, x, y, w, h, mark);
}

/* Whether the splits not yet drawn read or write any of this VRAM rect,
 * or a queued upload writes it. */
internal int NativeGpu_PendingSplitsAccess(int x, int y, int w, int h, int uploads);

int NativeGpu_PendingSplitsTouch(int x, int y, int w, int h)
{
	return s_gpu.splitIndex > 0 &&
	       (NativeGpu_PendingTiles(x, y, w, h, 0) || NativeGpu_VisitTiles(s_gpu.uploadTiles, x, y, w, h, 0)) &&
	       NativeGpu_PendingSplitsAccess(x, y, w, h, 1);
}

/* Extends a draw split's cached vertex bounds to its current vertices. */
internal void NativeGpu_ExtendSplitBounds(GPUDrawSplit *split, int count)
{
	const GrVertex *v = &s_gpu.vertexBuffer[split->startVertex];
	int k = split->boundsVerts;
	if (k <= 0 || k > count)
	{
		k = 1;
		split->boundsMinX = split->boundsMaxX = v->x;
		split->boundsMinY = split->boundsMaxY = v->y;
	}
	int minX = split->boundsMinX, maxX = split->boundsMaxX, minY = split->boundsMinY, maxY = split->boundsMaxY;
	for (; k < count; k++)
	{
		minX = v[k].x < minX ? v[k].x : minX;
		maxX = v[k].x > maxX ? v[k].x : maxX;
		minY = v[k].y < minY ? v[k].y : minY;
		maxY = v[k].y > maxY ? v[k].y : maxY;
	}
	split->boundsVerts = (u16)count;
	split->boundsMinX = (s16)minX;
	split->boundsMaxX = (s16)maxX;
	split->boundsMinY = (s16)minY;
	split->boundsMaxY = (s16)maxY;
}

/* The VRAM rect a frontend split writes: the bounds of its vertices within
 * its draw area, or its fill. Returns 0 when it writes nothing (queued
 * uploads write what the guest VRAM already holds). */
internal int NativeGpu_SplitWriteRect(int i, RECT16 *rect)
{
	GPUDrawSplit *split = &s_gpu.splits[i];
	if (split->drawPrimMode == 2)
	{
		if (split->uploadPixels != NULL)
			return 0;
		*rect = split->clearRect;
		return rect->w > 0 && rect->h > 0;
	}
	{
		const int count = i == s_gpu.splitIndex ? s_gpu.vertexIndex - split->startVertex : split->numVerts;
		if (count <= 0 || split->drawenv.clip.w <= 0 || split->drawenv.clip.h <= 0)
			return 0;
		NativeGpu_ExtendSplitBounds(split, count);
		int minX = split->boundsMinX, maxX = split->boundsMaxX, minY = split->boundsMinY, maxY = split->boundsMaxY;
		/* Vertices are relative to the draw area; keep a pixel of margin. */
		minX = minX - 1 < 0 ? 0 : minX - 1;
		minY = minY - 1 < 0 ? 0 : minY - 1;
		maxX = maxX + 1 > split->drawenv.clip.w ? split->drawenv.clip.w : maxX + 1;
		maxY = maxY + 1 > split->drawenv.clip.h ? split->drawenv.clip.h : maxY + 1;
		if (maxX <= minX || maxY <= minY)
			return 0;
		rect->x = (s16)(split->drawenv.clip.x + minX);
		rect->y = (s16)(split->drawenv.clip.y + minY);
		rect->w = (s16)(maxX - minX);
		rect->h = (s16)(maxY - minY);
		return 1;
	}
}

/* Whether a pending split reads or writes this VRAM rect, to the texel: the
 * pending tiles are 64x64 and take a draw's whole texture page and its
 * CLUT's tile, so most uploads they flag touch nothing a split uses (Soul
 * Calibur: 9 in 10), and each ended the packet early (a scene of VRAM
 * uploads and the main target's again). Reads are the texels inside each
 * page's UV bounds (the whole page under a texture window) and the CLUT;
 * writes are NativeGpu_SplitWriteRect. With uploads, queued uploads
 * overlapping the rect count too. */
internal int NativeGpu_PendingSplitsAccess(int x, int y, int w, int h, int uploads)
{
	RECT16 rect;
	for (int i = 1; i <= s_gpu.splitIndex; i++)
	{
		const GPUDrawSplit *split = &s_gpu.splits[i];
		if (split->drawPrimMode == 2 && split->uploadPixels != NULL)
		{
			if (uploads && NativeGpu_RectOverlaps(split->clearRect.x, split->clearRect.y, split->clearRect.w, split->clearRect.h, x, y, w, h))
				return 1;
			continue;
		}
		if (NativeGpu_SplitWriteRect(i, &rect) && NativeGpu_RectOverlaps(rect.x, rect.y, rect.w, rect.h, x, y, w, h))
			return 1;
		if (split->drawPrimMode == 2 || split->textureId == NativeRenderer_GetWhiteTexture())
			continue;
		const int count = i == s_gpu.splitIndex ? s_gpu.vertexIndex - split->startVertex : split->numVerts;
		const int div = split->texFormat == TF_4_BIT ? 4 : split->texFormat == TF_8_BIT ? 2 : 1;
		const int window = split->drawenv.tw.w != 0 || split->drawenv.tw.h != 0;
		const GrVertex *v = &s_gpu.vertexBuffer[split->startVertex];
		for (int k = 0; k < count;)
		{
			/* A run of vertices sharing a page and CLUT. */
			const int page = v[k].page, clut = v[k].clut;
			int u0 = v[k].u, u1 = v[k].u, v0 = v[k].v, v1 = v[k].v;
			for (k++; k < count && v[k].page == page && v[k].clut == clut; k++)
			{
				u0 = v[k].u < u0 ? v[k].u : u0;
				u1 = v[k].u > u1 ? v[k].u : u1;
				v0 = v[k].v < v0 ? v[k].v : v0;
				v1 = v[k].v > v1 ? v[k].v : v1;
			}
			const int pageX = (page & 15) * 64, pageY = (page >> 4) * 256;
			if (window)
				u0 = 0, u1 = 255, v0 = 0, v1 = 255;
			if (NativeGpu_RectOverlaps(pageX + u0 / div, pageY + v0, u1 / div - u0 / div + 1, v1 - v0 + 1, x, y, w, h))
				return 1;
			if (div > 1 && NativeGpu_RectOverlaps((clut & 0x3f) << 4, (clut >> 6) & 0x3ff, div == 4 ? 16 : 256, 1, x, y, w, h))
				return 1;
		}
	}
	return 0;
}

/* Marks what the frontend packet's splits write as stale in the guest VRAM. */
internal void NativeGpu_MarkPacketWrites(void)
{
	RECT16 rect;
	for (int i = 1; i <= s_gpu.splitIndex; i++)
		if (NativeGpu_SplitWriteRect(i, &rect))
			NativeGpu_VisitTiles(s_gpuGuestStaleTiles, rect.x, rect.y, rect.w, rect.h, 1);
}

/* Whether a pending frontend split writes into this VRAM rect. */
internal int NativeGpu_PendingWritesTouch(int x, int y, int w, int h)
{
	RECT16 rect;
	/* Every split writes inside the tiles AddSplit or a fill marked. */
	if (!NativeGpu_PendingTiles(x, y, w, h, 0))
		return 0;
	for (int i = 1; i <= s_gpu.splitIndex; i++)
		if (NativeGpu_SplitWriteRect(i, &rect) && NativeGpu_RectOverlaps(rect.x, rect.y, rect.w, rect.h, x, y, w, h))
			return 1;
	return 0;
}

/* The renderer wrote this rect outside a packet (synchronous fills and
 * image copies). */
void NativeGpu_GuestVRAMWritten(int x, int y, int w, int h)
{
	NativeGpu_VisitTiles(s_gpuGuestStaleTiles, x, y, w, h, 1);
}

/* Whether a CPU read of this rect needs the renderer's VRAM: something the
 * renderer drew (or will draw from the pending splits) may be there. */
int NativeGpu_GuestVRAMStale(int x, int y, int w, int h)
{
	if (x < 0 || y < 0 || x + w > VRAM_WIDTH || y + h > VRAM_HEIGHT)
		return 1;
	return NativeGpu_VisitTiles(s_gpuGuestStaleTiles, x, y, w, h, 0) || NativeGpu_PendingWritesTouch(x, y, w, h);
}

/* After a read-back the guest VRAM is current in the tiles the rect covers. */
void NativeGpu_GuestVRAMSynced(int x, int y, int w, int h)
{
	if (x < 0 || y < 0 || x + w > VRAM_WIDTH || y + h > VRAM_HEIGHT)
		return;
	for (int row = (y + 63) >> 6; (row + 1) << 6 <= y + h; row++)
		for (int col = (x + 63) >> 6; (col + 1) << 6 <= x + w; col++)
		{
			const int tile = row * NATIVE_GPU_PENDING_TILE_COLS + col;
			s_gpuGuestStaleTiles[tile >> 5] &= ~(1u << (tile & 31));
		}
}

internal void NativeGpu_MarkPendingSources(int textured, int tpage, int clut)
{
	if (!textured)
		return;
	const TexFormat format = GetTPageFormat(tpage);
	const int pageWidth = format == TF_4_BIT ? 64 : format == TF_8_BIT ? 128 : 256;
	NativeGpu_PendingTiles((tpage & 0xf) << 6, (GetTPageBase(tpage) >> 4) * 256, pageWidth, 256, 1);
	if (format == TF_4_BIT || format == TF_8_BIT)
		NativeGpu_PendingTiles((clut & 0x3f) << 4, (clut >> 6) & 0x3ff, format == TF_4_BIT ? 16 : 256, 1, 1);
}
#endif

internal void NativeGpu_SetVertexOrderDepth(GrVertex *vertex, int count)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	for (int i = 0; i < count; i++)
	{
		vertex[i].orderDepth = s_gpu.currentPrimitiveOrder;
	}
#else
	(void)vertex;
	(void)count;
#endif
}

void ClearSplits(void)
{
	s_gpu.currentSplitDebugText = NULL;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (s_gpu.vertexBuffer == NULL || s_gpu.splits == NULL)
	{
		NativeGpu_BindFrontendPacket();
	}
#endif
	s_gpu.vertexIndex = 0;
	s_gpu.splitIndex = 0;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	memset(s_gpu.pendingTiles, 0, sizeof(s_gpu.pendingTiles));
	memset(s_gpu.uploadTiles, 0, sizeof(s_gpu.uploadTiles));
	memset(&s_gpu.markedClip, 0, sizeof(s_gpu.markedClip));
	s_gpu.markedSources = false;
	s_gpu.uploadTexels = 0;
	s_gpu.packetHasDraws = false;
#endif
	s_gpu.splits[0].texFormat = (TexFormat)0xFFFF;
	s_gpu.splits[0].psxTexturedSemiTrans = false;
	s_gpu.splits[0].psxTextureOutputSTP = false;
	s_gpu.splits[0].psxDrawMaskSet = false;
	s_gpu.splits[0].superTurboTint = false;
	#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	s_gpu.splits[0].psxTextureFullyOpaque = false;
	s_gpu.splits[0].p4CacheEligible = false;
	s_gpu.splits[0].p4SuperTurboTint = false;
	s_gpu.splits[0].p4Page = -1;
	s_gpu.splits[0].p4Clut = 0;
#endif
	s_gpu.splits[0].psxSemiTransPassMask = 0;
	s_gpu.framebufferFeedbackRunActive = false;
}

int NativeGpu_GetStateSize(void)
{
	return (int)offsetof(struct NativeGpuSnapshot, vram) + NativeRenderer_GetVRAMStateSize();
}

int NativeGpu_CaptureState(void *dst, int dstSize)
{
	struct NativeGpuSnapshot *snapshot = (struct NativeGpuSnapshot *)dst;

	if ((dst == NULL) || (dstSize < NativeGpu_GetStateSize()))
	{
		return 0;
	}

	memset(snapshot, 0, NativeGpu_GetStateSize());
	snapshot->magic = NATIVE_GPU_STATE_MAGIC;
	snapshot->version = NATIVE_GPU_STATE_VERSION;
	snapshot->size = NativeGpu_GetStateSize();
	snapshot->drawEnv = activeDrawEnv;
	snapshot->dispEnv = activeDispEnv;
	snapshot->gpuDisabledState = g_GPUDisabledState;
	snapshot->psxDrawMaskSet = s_gpu.psxDrawMaskSet;

#ifdef __vita__
	NativeGpuBackendVRAMStateTask task = {snapshot->vram, NativeRenderer_GetVRAMStateSize(), 0};
	NativeGpu_RunBackendTaskSync(NativeGpu_BackendCaptureVRAMStateTask, &task);
	return task.result;
#else
	return NativeRenderer_CaptureVRAMState(snapshot->vram, NativeRenderer_GetVRAMStateSize());
#endif
}

int NativeGpu_RestoreState(const void *src, int srcSize)
{
	const struct NativeGpuSnapshot *snapshot = (const struct NativeGpuSnapshot *)src;

	if ((src == NULL) || (srcSize < NativeGpu_GetStateSize()))
	{
		return 0;
	}
	if ((snapshot->magic != NATIVE_GPU_STATE_MAGIC) || (snapshot->version != NATIVE_GPU_STATE_VERSION) || (snapshot->size != (u32)NativeGpu_GetStateSize()))
	{
		return 0;
	}
	if ((snapshot->gpuDisabledState < 0) || (snapshot->gpuDisabledState > 1) || (snapshot->psxDrawMaskSet < 0) || (snapshot->psxDrawMaskSet > 1))
	{
		return 0;
	}
#ifdef __vita__
	NativeGpuBackendVRAMStateTask task = {(void *)snapshot->vram, NativeRenderer_GetVRAMStateSize(), 0};
	NativeGpu_RunBackendTaskSync(NativeGpu_BackendRestoreVRAMStateTask, &task);
	if (!task.result)
	{
		return 0;
	}
#else
	if (!NativeRenderer_RestoreVRAMState(snapshot->vram, NativeRenderer_GetVRAMStateSize()))
	{
		return 0;
	}
#endif

	activeDrawEnv = snapshot->drawEnv;
	activeDispEnv = snapshot->dispEnv;
	g_GPUDisabledState = snapshot->gpuDisabledState;
	s_gpu.psxDrawMaskSet = snapshot->psxDrawMaskSet;
	ClearSplits();
	return 1;
}

internal void DrawEnvDimensionsInt(int *width, int *height)
{
	if (activeDrawEnv.dfe)
	{
		*width = activeDispEnv.disp.w;
		*height = activeDispEnv.disp.h;
	}
	else
	{
		*width = activeDrawEnv.clip.w;
		*height = activeDrawEnv.clip.h;
	}
}

void DrawEnvOffset(float *ofsX, float *ofsY)
{
	if (activeDrawEnv.dfe)
	{
		int w, h;
		DrawEnvDimensionsInt(&w, &h);

		if (w <= 0)
		{
			w = 1;
		}

		// Convert PS1 VRAM-page draw offsets into draw-buffer-relative host coordinates. DISPENV is the other page during normal PS1 double buffering.
		*ofsX = activeDrawEnv.ofs[0] - activeDrawEnv.clip.x;
		*ofsY = activeDrawEnv.ofs[1] - activeDrawEnv.clip.y;
	}
	else
	{
		*ofsX = activeDrawEnv.ofs[0] - activeDrawEnv.clip.x;
		*ofsY = activeDrawEnv.ofs[1] - activeDrawEnv.clip.y;
	}
}

void LineSwapSourceVerts(VERTTYPE **p0, VERTTYPE **p1, u8 **c0, u8 **c1)
{
	// swap line coordinates for left-to-right and up-to-bottom direction
	if (((*p0)[0] > (*p1)[0]) || ((*p0)[1] > (*p1)[1] && (*p0)[0] == (*p1)[0]))
	{
		VERTTYPE *tmp = *p0;
		*p0 = *p1;
		*p1 = tmp;

		u8 *tmpCol = *c0;
		*c0 = *c1;
		*c1 = tmpCol;
	}
}

void MakeLineArray(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1)
{
	const VERTTYPE dx = p1[0] - p0[0];
	const VERTTYPE dy = p1[1] - p0[1];

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);
	NativeGpu_SetVertexOrderDepth(vertex, 4);

	if (dx > abs((s16)dy))
	{ // horizontal
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX + 1;
		vertex[1].y = p1[1] + ofsY;

		vertex[2].x = vertex[1].x;
		vertex[2].y = vertex[1].y + 1;

		vertex[3].x = vertex[0].x;
		vertex[3].y = vertex[0].y + 1;
	}
	else
	{ // vertical
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX;
		vertex[1].y = p1[1] + ofsY + 1;

		vertex[2].x = vertex[1].x + 1;
		vertex[2].y = vertex[1].y;

		vertex[3].x = vertex[0].x + 1;
		vertex[3].y = vertex[0].y;
	} // TODO diagonal line alignment
}

void MakeVertexTriangle(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1, VERTTYPE *p2)
{
	assert(p0);
	assert(p1);
	assert(p2);

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 3);
	NativeGpu_SetVertexOrderDepth(vertex, 3);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;
}

void MakeVertexQuad(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1, VERTTYPE *p2, VERTTYPE *p3)
{
	assert(p0);
	assert(p1);
	assert(p2);
	assert(p3);

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);
	NativeGpu_SetVertexOrderDepth(vertex, 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	vertex[3].x = p3[0] + ofsX;
	vertex[3].y = p3[1] + ofsY;
}

void MakeVertexRect(GrVertex *vertex, VERTTYPE *p0, s16 w, s16 h)
{
	assert(p0);

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);
	NativeGpu_SetVertexOrderDepth(vertex, 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = vertex[0].x;
	vertex[1].y = vertex[0].y + h;

	vertex[2].x = vertex[0].x + w;
	vertex[2].y = vertex[0].y + h;

	vertex[3].x = vertex[0].x + w;
	vertex[3].y = vertex[0].y;
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
/* A polygon whose vertices share a U (or V) stretches one texel column (or
 * row) over it: every pixel samples that texel. Its coordinate interpolates
 * to the texel's left (top) edge, an exact integer the shaders floor(), and
 * the Vita's half-precision varyings (vglUseLowPrecision) can bring it one
 * step below: the previous texel was drawn (Soul Calibur's life bars, a
 * stretched column, lost their green to a transparent neighbour). Sample
 * the texel's centre instead: tcx/tcy are half-texel offsets. */
internal void CentreConstantTexcoords(GrVertex *vertex, int count)
{
	bool sameU = true;
	bool sameV = true;
	for (int i = 1; i < count; i++)
	{
		sameU = sameU && vertex[i].u == vertex[0].u;
		sameV = sameV && vertex[i].v == vertex[0].v;
	}
	for (int i = 0; i < count; i++)
	{
		vertex[i].tcx = sameU ? 1 : 0;
		vertex[i].tcy = sameV ? 1 : 0;
	}
}
#endif

void MakeTexcoordQuad(GrVertex *vertex, u8 *uv0, u8 *uv1, u8 *uv2, u8 *uv3, s16 page, s16 clut, u8 dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);
	assert(uv3);

	const u8 bright = 2;
	const short texPage = GetTPageBase(page);

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = texPage;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = texPage;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = texPage;
	vertex[2].clut = clut;

	vertex[3].u = uv3[0];
	vertex[3].v = uv3[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = texPage;
	vertex[3].clut = clut;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	CentreConstantTexcoords(vertex, 4);
#endif
	/* if (g_cfg_bilinearFiltering) { vertex[0].tcx = -1; vertex[0].tcy = -1; vertex[1].tcx = -1; vertex[1].tcy = -1; vertex[2].tcx = -1; vertex[2].tcy = -1; vertex[3].tcx = -1; vertex[3].tcy = -1; } */
}

void MakeTexcoordTriangle(GrVertex *vertex, u8 *uv0, u8 *uv1, u8 *uv2, s16 page, s16 clut, u8 dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);

	const u8 bright = 2;
	const short texPage = GetTPageBase(page);

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = texPage;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = texPage;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = texPage;
	vertex[2].clut = clut;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	CentreConstantTexcoords(vertex, 3);
#endif
	/* if (g_cfg_bilinearFiltering) { vertex[0].tcx = -1; vertex[0].tcy = -1; vertex[1].tcx = -1; vertex[1].tcy = -1; vertex[2].tcx = -1; vertex[2].tcy = -1; vertex[3].tcx = -1; vertex[3].tcy = -1; } */
}

void MakeTexcoordRect(GrVertex *vertex, u8 *uv, s16 page, s16 clut, s16 w, s16 h)
{
	assert(uv);

	const u8 bright = 2;
	const u8 dither = 0;
	const short texPage = GetTPageBase(page);
	const int endU = (int)uv[0] + w;
	const int endV = (int)uv[1] + h;
	const bool wrapsU = endU == 256;
	const bool wrapsV = endV == 256;
	const s8 texelCenterOffset = g_cfg_bilinearFiltering ? -1 : 0;
	const u8 encodedEndU = wrapsU ? 255 : (u8)endU;
	const u8 encodedEndV = wrapsV ? 255 : (u8)endV;

	vertex[0].u = uv[0];
	vertex[0].v = uv[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = texPage;
	vertex[0].clut = clut;
	vertex[0].tcx = texelCenterOffset;
	vertex[0].tcy = texelCenterOffset;

	vertex[1].u = uv[0];
	vertex[1].v = encodedEndV;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = texPage;
	vertex[1].clut = clut;
	vertex[1].tcx = texelCenterOffset;
	vertex[1].tcy = texelCenterOffset + (wrapsV ? 2 : 0);

	vertex[2].u = encodedEndU;
	vertex[2].v = encodedEndV;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = texPage;
	vertex[2].clut = clut;
	vertex[2].tcx = texelCenterOffset + (wrapsU ? 2 : 0);
	vertex[2].tcy = texelCenterOffset + (wrapsV ? 2 : 0);

	vertex[3].u = encodedEndU;
	vertex[3].v = uv[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = texPage;
	vertex[3].clut = clut;
	vertex[3].tcx = texelCenterOffset + (wrapsU ? 2 : 0);
	vertex[3].tcy = texelCenterOffset;
}

void MakeTexcoordLineZero(GrVertex *vertex, u8 dither)
{
	const u8 bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = 0;
	vertex[3].clut = 0;
}

void MakeTexcoordTriangleZero(GrVertex *vertex, u8 dither)
{
	const u8 bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;
}

void MakeTexcoordQuadZero(GrVertex *vertex, u8 dither)
{
	const u8 bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = 0;
	vertex[3].clut = 0;
}

void MakeColourNoShade(GrVertex *vertex, int n)
{
	--n;
	while (n >= 0)
	{
		vertex[n].r = 128;
		vertex[n].g = 128;
		vertex[n].b = 128;
		vertex[n].a = 255;
		--n;
	}
}

void MakeColourSuperTurboTint(GrVertex *vertex, int n)
{
	--n;
	while (n >= 0)
	{
		vertex[n].r = 96;
		vertex[n].g = 144;
		vertex[n].b = 160;
		vertex[n].a = 255;
		--n;
	}
}

void MakeColourLine(GrVertex *vertex, bool shadeTexOn, u8 *col0, u8 *col1)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}
	assert(col0);
	assert(col1);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col1[0];
	vertex[2].g = col1[1];
	vertex[2].b = col1[2];
	vertex[2].a = 255;

	vertex[3].r = col0[0];
	vertex[3].g = col0[1];
	vertex[3].b = col0[2];
	vertex[3].a = 255;
}

void MakeColourTriangle(GrVertex *vertex, bool shadeTexOn, u8 *col0, u8 *col1, u8 *col2)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 3);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;
}

void MakeColourQuad(GrVertex *vertex, bool shadeTexOn, u8 *col0, u8 *col1, u8 *col2, u8 *col3)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);
	assert(col3);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;

	vertex[3].r = col3[0];
	vertex[3].g = col3[1];
	vertex[3].b = col3[2];
	vertex[3].a = 255;
}

internal void TriangulateQuadVertices(GrVertex *vertex)
{
	/* Triangulate like this: v0--v1 | / | | / | v2--v3 NOTE: v2 swapped with v3 during primitive parsing but it not shown here */

	vertex[4] = vertex[3];
	vertex[5] = vertex[2];
	vertex[2] = vertex[3];
	vertex[3] = vertex[1];
}

void TriangulateQuad()
{
	TriangulateQuadVertices(&s_gpu.vertexBuffer[s_gpu.vertexIndex]);
}

internal int NativeGpu_EmitTexturedSprite(VERTTYPE *position, u8 *uv, s16 page, s16 clut, s16 width, s16 height, bool shadeTexOn, u8 *color)
{
	int emittedVertices = 0;
	int yOffset = 0;

	while (yOffset < height)
	{
		const int texV = ((int)uv[1] + yOffset) & 255;
		const int remainingH = height - yOffset;
		const int partH = remainingH < 256 - texV ? remainingH : 256 - texV;
		int xOffset = 0;

		while (xOffset < width)
		{
			const int texU = ((int)uv[0] + xOffset) & 255;
			const int remainingW = width - xOffset;
			const int partW = remainingW < 256 - texU ? remainingW : 256 - texU;
			VERTTYPE partPosition[2] = {(VERTTYPE)(position[0] + xOffset), (VERTTYPE)(position[1] + yOffset)};
			u8 partUv[2] = {(u8)texU, (u8)texV};
			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex + emittedVertices];

			MakeVertexRect(firstVertex, partPosition, (s16)partW, (s16)partH);
			MakeTexcoordRect(firstVertex, partUv, page, clut, (s16)partW, (s16)partH);
			MakeColourQuad(firstVertex, shadeTexOn, color, color, color, color);
			TriangulateQuadVertices(firstVertex);
			emittedVertices += 6;
			xOffset += partW;
		}

		yOffset += partH;
	}

	return emittedVertices;
}

// ------------------------------------------------------------------------------------------------------------------------

internal bool NativeGpu_RectOverlaps(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
	return (aw > 0) && (ah > 0) && (bw > 0) && (bh > 0) && (ax < bx + bw) && (bx < ax + aw) && (ay < by + bh) && (by < ay + ah);
}

internal bool NativeGpu_TPageOverlapsActiveDrawPage(int tpage)
{
	const int pageX = (tpage & 0xf) << 6;
	const int pageY = (GetTPageBase(tpage) >> 4) * 256;
	const int pageW = 0x100;
	const int pageH = 0x100;

	if (GetTPageFormat(tpage) != TF_16_BIT)
	{
		return false;
	}

	return NativeGpu_RectOverlaps(pageX, pageY, pageW, pageH, activeDrawEnv.clip.x, activeDrawEnv.clip.y, activeDrawEnv.clip.w, activeDrawEnv.clip.h);
}

internal void NativeGpu_PrepareFramebufferFeedback(int tpage)
{
	if (!NativeGpu_TPageOverlapsActiveDrawPage(tpage))
	{
		return;
	}

	if (s_gpu.framebufferFeedbackRunActive)
	{
		return;
	}

	// NOTE(aalhendi): PS1 can draw into VRAM and immediately texture from that same draw page. Native batches primitives, so screen-feedback effects like heat warp need an explicit barrier before their framebuffer-sampling polygons consume the VRAM texture.
#ifdef __vita__
	NativeGpu_ForceSynchronousFrame();
	NativeGpu_FlushFrontendSplitsSync();
#else
	if (NativeGpu_HasPendingSplits())
	{
		DrawAllSplits();
	}
#endif

#ifdef __vita__
	NativeGpuBackendRectTask task = {activeDrawEnv.clip.x, activeDrawEnv.clip.y, activeDrawEnv.clip.w, activeDrawEnv.clip.h, 0, 0, 0};
	NativeGpu_RunBackendTaskSync(NativeGpu_BackendStoreFrameBufferTask, &task);
#else
	NativeRenderer_StoreFrameBuffer(activeDrawEnv.clip.x, activeDrawEnv.clip.y, activeDrawEnv.clip.w, activeDrawEnv.clip.h);
#endif
	s_gpu.framebufferFeedbackRunActive = true;
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
void VitaCtrNativeRenderer_CopyPackedVRAM(const u16 *src, int x, int y, int w, int h);
internal bool NativeGpu_AppendClearSplit(const RECT16 *rect, u8 r, u8 g, u8 b)
{
	GPUDrawSplit *current = &s_gpu.splits[s_gpu.splitIndex];
	current->numVerts = s_gpu.vertexIndex - current->startVertex;
	if (s_gpu.splitIndex + 1 >= MAX_DRAW_SPLITS)
	{
		return false;
	}
	GPUDrawSplit *split = &s_gpu.splits[++s_gpu.splitIndex];
	memset(split, 0, sizeof(*split));
	s_gpu.packetHasDraws = true;
	NativeGpu_PendingTiles(rect->x, rect->y, rect->w, rect->h, 1);
	split->drawPrimMode = 2;
	split->drawenv = activeDrawEnv;
	split->dispenv = activeDispEnv;
	split->clearRect = *rect;
	split->clearR = r;
	split->clearG = g;
	split->clearB = b;
	split->startVertex = s_gpu.vertexIndex;
	return true;
}

/* Queues a CPU upload into this frame packet instead of copying it on the
 * backend at once, which waited for the backend to finish the previous
 * frame, several times a frame. The backend applies the queued uploads, in
 * order, when it starts the packet, before it reads any palette: so only an
 * upload no pending split reads or writes can be queued (the earlier splits
 * then draw the same pixels with it applied first). Returns 0 when the
 * upload must take the synchronous path. */
int NativeGpu_QueueUpload(const u16 *vram, int x, int y, int w, int h)
{
	if (!s_gpuFrontendFramePrepared || s_gpuSynchronousFrame || vram == NULL ||
	    (s_gpu.packetHasDraws && NativeGpu_PendingTiles(x, y, w, h, 0) && NativeGpu_PendingSplitsAccess(x, y, w, h, 0)) ||
	    w <= 0 || h <= 0 || w > VRAM_WIDTH || h > VRAM_HEIGHT ||
	    s_gpu.splitIndex + 1 >= MAX_DRAW_SPLITS || w * h > NATIVE_GPU_UPLOAD_TEXELS - s_gpu.uploadTexels)
	{
		return 0;
	}
	u16 *dst = s_gpu.uploadStorage + s_gpu.uploadTexels;
	for (int row = 0; row < h; row++)
	{
		const int yy = (y + row) & (VRAM_HEIGHT - 1);
		int xx = x & (VRAM_WIDTH - 1);
		int remaining = w;
		while (remaining > 0)
		{
			const int chunk = remaining < VRAM_WIDTH - xx ? remaining : VRAM_WIDTH - xx;
			memcpy(dst, vram + yy * VRAM_WIDTH + xx, (size_t)chunk * sizeof(u16));
			dst += chunk;
			remaining -= chunk;
			xx = 0;
		}
	}
	GPUDrawSplit *current = &s_gpu.splits[s_gpu.splitIndex];
	current->numVerts = s_gpu.vertexIndex - current->startVertex;
	GPUDrawSplit *split = &s_gpu.splits[++s_gpu.splitIndex];
	memset(split, 0, sizeof(*split));
	split->drawPrimMode = 2;
	split->drawenv = activeDrawEnv;
	split->dispenv = activeDispEnv;
	split->clearRect.x = (s16)x;
	split->clearRect.y = (s16)y;
	split->clearRect.w = (s16)w;
	split->clearRect.h = (s16)h;
	split->uploadPixels = s_gpu.uploadStorage + s_gpu.uploadTexels;
	split->startVertex = s_gpu.vertexIndex;
	s_gpu.uploadTexels += w * h;
	NativeGpu_VisitTiles(s_gpu.uploadTiles, x, y, w, h, 1);
	return 1;
}

/* Applies a packet's queued uploads; DrawSplit then skips them. Called
 * before the packet's Platform_BeginScene, so the main target is loaded
 * with them, and again (a no-op) when the packet is drawn. */
internal void NativeGpu_ApplyQueuedUploads(GPUDrawSplit *splits, int splitCount)
{
	int applied = 0;
	for (int i = 1; i <= splitCount; i++)
	{
		GPUDrawSplit *split = &splits[i];
		if (split->drawPrimMode != 2 || split->uploadPixels == NULL || split->uploadApplied)
			continue;
		VitaCtrNativeRenderer_CopyPackedVRAM(split->uploadPixels, split->clearRect.x, split->clearRect.y,
		                                     split->clearRect.w, split->clearRect.h);
		split->uploadApplied = true;
		applied = 1;
	}
	if (applied)
	{
		NativeRenderer_UpdateVRAM();
	}
}
#endif

internal void AddSplit(bool semiTrans, bool textured, bool framebufferFeedback, s16 clut)
{
	int tpage = activeDrawEnv.tpage;

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (s_gpu.primitiveOrder < 0xffffu)
	{
		s_gpu.primitiveOrder++;
	}
	s_gpu.currentPrimitiveOrder = (u16)s_gpu.primitiveOrder;
#endif

	if (framebufferFeedback)
	{
		NativeGpu_PrepareFramebufferFeedback(tpage);
	}
	else
	{
		s_gpu.framebufferFeedbackRunActive = false;
	}

	GPUDrawSplit *curSplit = &s_gpu.splits[s_gpu.splitIndex];
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	/* The previous primitives' vertices are still in the cache. */
	if (s_gpu.splitIndex > 0 && curSplit->drawPrimMode != 2 && s_gpu.vertexIndex > curSplit->startVertex)
		NativeGpu_ExtendSplitBounds(curSplit, s_gpu.vertexIndex - curSplit->startVertex);
#endif

	BlendMode blendMode = semiTrans ? GET_TPAGE_BLEND(tpage) : BM_NONE;
	TexFormat texFormat = GetTPageFormat(tpage);
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	s_gpu.packetHasDraws = true;
	if (memcmp(&activeDrawEnv.clip, &s_gpu.markedClip, sizeof(s_gpu.markedClip)) != 0)
	{
		NativeGpu_PendingTiles(activeDrawEnv.clip.x, activeDrawEnv.clip.y, activeDrawEnv.clip.w, activeDrawEnv.clip.h, 1);
		s_gpu.markedClip = activeDrawEnv.clip;
	}
	if (textured && (!s_gpu.markedSources || tpage != s_gpu.markedTpage || clut != s_gpu.markedClut))
	{
		NativeGpu_MarkPendingSources(textured, tpage, clut);
		s_gpu.markedSources = true;
		s_gpu.markedTpage = tpage;
		s_gpu.markedClut = clut;
	}
#endif
	TextureID textureId = textured ? NativeRenderer_GetVRAMTexture() : NativeRenderer_GetWhiteTexture();
	bool psxTexturedSemiTrans = semiTrans && textured && (s_gpu.overrideTexture == 0 || s_gpu.overrideTexturePsxStp);
	// NOTE(aalhendi): PS1 framebuffer bit 15 follows sampled texture STP for textured draws unless E6 forces it. Recursive screen-copy effects depend on this bit surviving after the blended textured pass.
	bool psxTextureOutputSTP = textured && s_gpu.overrideTexture == 0;
	const bool superTurboTint = textured && (((u32)tpage & NATIVE_GPU_TPAGE_SUPER_TURBO_TINT) != 0);

	if (textured && s_gpu.overrideTexture != 0)
	{
		// override texture format, zero tpage
		texFormat = TF_32_BIT_RGBA;
		textureId = s_gpu.overrideTexture;
	}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	const bool p4CacheEligible = textured && s_gpu.overrideTexture == 0 && texFormat == TF_4_BIT && !framebufferFeedback;
	const bool p4SuperTurboTint = p4CacheEligible && superTurboTint;
	const s16 p4Page = p4CacheEligible ? GetTPageBase(tpage) : -1;
	const u16 p4Clut = p4CacheEligible ? (u16)clut : 0;
#endif

	// FIXME: compare drawing environment too?
	if (!psxTexturedSemiTrans && curSplit->blendMode == blendMode && curSplit->texFormat == texFormat && curSplit->textureId == textureId &&
	    curSplit->drawPrimMode == s_gpu.drawPrimMode && curSplit->psxTexturedSemiTrans == psxTexturedSemiTrans &&
	    curSplit->psxTextureOutputSTP == psxTextureOutputSTP && curSplit->psxDrawMaskSet == s_gpu.psxDrawMaskSet &&
	    curSplit->superTurboTint == superTurboTint &&
	    curSplit->drawenv.tw.x == activeDrawEnv.tw.x && curSplit->drawenv.tw.y == activeDrawEnv.tw.y &&
	    curSplit->drawenv.tw.w == activeDrawEnv.tw.w && curSplit->drawenv.tw.h == activeDrawEnv.tw.h &&
	    curSplit->overrideTextureWidth == s_gpu.overrideTextureWidth &&
	    curSplit->overrideTextureHeight == s_gpu.overrideTextureHeight &&
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	    curSplit->p4CacheEligible == p4CacheEligible &&
	    (!p4CacheEligible || (curSplit->p4Page == p4Page && curSplit->p4Clut == p4Clut && curSplit->p4SuperTurboTint == p4SuperTurboTint)) &&
#endif
	    curSplit->drawenv.clip.x == activeDrawEnv.clip.x && curSplit->drawenv.clip.y == activeDrawEnv.clip.y &&
	    curSplit->drawenv.clip.w == activeDrawEnv.clip.w && curSplit->drawenv.clip.h == activeDrawEnv.clip.h && curSplit->drawenv.dfe == activeDrawEnv.dfe &&
	    curSplit->debugText == s_gpu.currentSplitDebugText)
	{
		return;
	}

	curSplit->numVerts = s_gpu.vertexIndex - curSplit->startVertex;

	if (s_gpu.splitIndex + 1 >= MAX_DRAW_SPLITS)
	{
		NATIVE_GPU_ERROR("%s\n", "MAX_DRAW_SPLITS reached (too many blend modes, texture formats, drawEnv clip rects, dfe switches), expect rendering errors");
		return;
	}

	GPUDrawSplit *split = &s_gpu.splits[++s_gpu.splitIndex];
	split->blendMode = blendMode;
	split->texFormat = texFormat;
	split->textureId = textureId;
	split->drawPrimMode = s_gpu.drawPrimMode;
	split->psxTexturedSemiTrans = psxTexturedSemiTrans;
	split->psxTextureOutputSTP = psxTextureOutputSTP;
	split->psxDrawMaskSet = s_gpu.psxDrawMaskSet;
	split->superTurboTint = superTurboTint;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	split->psxTextureFullyOpaque = false;
	split->p4CacheEligible = p4CacheEligible;
	split->p4SuperTurboTint = p4SuperTurboTint;
	split->p4Page = p4Page;
	split->p4Clut = p4Clut;
#endif
	split->psxSemiTransPassMask = psxTexturedSemiTrans ? 3 : 0;
	split->drawenv = activeDrawEnv;
	split->dispenv = activeDispEnv;
	split->debugText = s_gpu.currentSplitDebugText;

	split->overrideTextureWidth = (s16)s_gpu.overrideTextureWidth;
	split->overrideTextureHeight = (s16)s_gpu.overrideTextureHeight;

	split->startVertex = s_gpu.vertexIndex;
	split->numVerts = 0;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	split->boundsVerts = 0;
#endif
}

internal void NativeGpu_SetSplitShaderState(const GPUDrawSplit *split, int semiTransPass, BlendMode blendMode, bool offscreen)
{
	TextureID texture = split->textureId;
	int cachedP4 = 0;
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (split->p4CacheEligible)
	{
		const TextureID cachedTexture = NativeRenderer_GetCachedP4Texture(split->p4Page, split->p4Clut, split->p4SuperTurboTint);
		if (cachedTexture != 0)
		{
			texture = cachedTexture;
			cachedP4 = 1;
		}
	}
#endif
	if (semiTransPass == 3)
	{
		NativeRenderer_SetMixedSTPBlendMode(blendMode);
	}
	else
	{
		NativeRenderer_SetBlendMode(blendMode);
	}
	NativeRenderer_SetTexture(texture, split->texFormat, semiTransPass, blendMode,
	                          texture != NativeRenderer_GetWhiteTexture(), split->superTurboTint,
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	                          split->psxTextureFullyOpaque,
#else
	                          false,
#endif
		                          cachedP4, &split->drawenv.tw);
	if (split->texFormat == TF_32_BIT_RGBA)
	{
		NativeRenderer_SetOverrideTextureSize(split->overrideTextureWidth, split->overrideTextureHeight);
	}
	NativeRenderer_SetPSXDrawMaskSet(split->psxDrawMaskSet);
	NativeRenderer_SetPSXTextureOutputSTP(split->psxTextureOutputSTP);
	NativeRenderer_SetProjection(&split->drawenv.clip, &split->dispenv, offscreen);
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
typedef enum
{
	NATIVE_GPU_PASS_OPAQUE,
	NATIVE_GPU_PASS_DISCARD,
	NATIVE_GPU_PASS_BLEND,
	NATIVE_GPU_PASS_BLEND_DISCARD,
	NATIVE_GPU_PASS_COUNT,
} NativeGpuPassCategory;

struct NativeGpuSplitBounds
{
	s32 minX;
	s32 minY;
	s32 maxX;
	s32 maxY;
};

internal bool NativeGpu_GetSplitBounds(const GPUDrawSplit *split, struct NativeGpuSplitBounds *bounds);

typedef struct
{
	int domainFirstSplit;
	int representativeSplit;
	int lastMemberSplit;
	int semiTransPass;
	NativeGpuPassCategory category;
	/* The members' vertices copied together after the frame's own, so the
	 * batch is one draw; drawCount < 0 when they were not. */
	int drawStart;
	int drawCount;
} NativeGpuDepthBatch;

typedef struct
{
	u32 generation;
	u32 hash;
	int batchIndex;
} NativeGpuDepthHashEntry;

#define NATIVE_GPU_DEPTH_HASH_CAPACITY 8192

typedef struct
{
	bool valid;
	NativeGpuPassCategory category;
	int semiTransPass;
} NativeGpuDepthPassInfo;

typedef struct
{
	int splitIndex;
	int indegree;
	bool scheduled;
	NativeGpuPassCategory category;
	struct NativeGpuSplitBounds bounds;
} NativeGpuBlendNode;

typedef struct
{
	int representativeSplit;
	int startVertex;
	int numVerts;
	int semiTransPass;
	bool active;
} NativeGpuBlendBatch;

global_variable NativeGpuDepthBatch s_gpuDepthBatches[MAX_DRAW_SPLITS];
global_variable int s_gpuDepthBatchCount;
global_variable NativeGpuDepthPassInfo s_gpuDepthPassInfo[MAX_DRAW_SPLITS];
global_variable int s_gpuDepthNextSplit[MAX_DRAW_SPLITS];
global_variable NativeGpuDepthHashEntry s_gpuDepthHashTable[NATIVE_GPU_DEPTH_HASH_CAPACITY];
global_variable u32 s_gpuDepthHashGeneration = 1;
global_variable NativeGpuBlendNode s_gpuBlendNodes[MAX_DRAW_SPLITS];

internal NativeGpuPassCategory NativeGpu_GetPassCategory(const GPUDrawSplit *split, BlendMode blendMode)
{
	const bool mayDiscard = split->textureId != NativeRenderer_GetWhiteTexture() &&
	                        (split->texFormat != TF_32_BIT_RGBA || split->psxTexturedSemiTrans) && !split->psxTextureFullyOpaque;
	if (blendMode == BM_NONE)
	{
		return mayDiscard ? NATIVE_GPU_PASS_DISCARD : NATIVE_GPU_PASS_OPAQUE;
	}
	return mayDiscard ? NATIVE_GPU_PASS_BLEND_DISCARD : NATIVE_GPU_PASS_BLEND;
}

internal bool NativeGpu_CanUseMixedSTPPass(const GPUDrawSplit *split)
{
	return split->psxTexturedSemiTrans && split->psxSemiTransPassMask == 3 && !split->drawPrimMode && !split->psxDrawMaskSet &&
	       split->blendMode != BM_SUBTRACT && split->texFormat != TF_32_BIT_RGBA;
}

internal bool NativeGpu_GetDepthPassInfo(const GPUDrawSplit *split, NativeGpuPassCategory *category, int *semiTransPass)
{
	if ((split->drawenv.clip.w <= 0) || (split->drawenv.clip.h <= 0))
	{
		return false;
	}

	if (split->psxTexturedSemiTrans)
	{
		if (NativeGpu_CanUseMixedSTPPass(split))
		{
			return false;
		}
		if ((split->psxSemiTransPassMask & 1) == 0)
		{
			return false;
		}
		*category = NativeGpu_GetPassCategory(split, BM_NONE);
		*semiTransPass = 1;
		return true;
	}

	if (split->blendMode != BM_NONE)
	{
		return false;
	}

	*category = NativeGpu_GetPassCategory(split, BM_NONE);
	*semiTransPass = 0;
	return true;
}

internal bool NativeGpu_DepthPassStateCompatible(const GPUDrawSplit *first, int firstSemiTransPass, const GPUDrawSplit *second,
                                                  int secondSemiTransPass)
{
	if (firstSemiTransPass != secondSemiTransPass || first->textureId != second->textureId || first->texFormat != second->texFormat ||
	    first->drawPrimMode != second->drawPrimMode || first->psxTextureOutputSTP != second->psxTextureOutputSTP ||
	    first->psxDrawMaskSet != second->psxDrawMaskSet || first->psxTextureFullyOpaque != second->psxTextureFullyOpaque ||
	    first->p4CacheEligible != second->p4CacheEligible ||
	    (first->p4CacheEligible &&
	     (first->p4Page != second->p4Page || first->p4Clut != second->p4Clut || first->p4SuperTurboTint != second->p4SuperTurboTint)) ||
	    first->debugText != second->debugText || first->drawenv.dfe != second->drawenv.dfe)
	{
		return false;
	}

	if (first->drawenv.clip.x != second->drawenv.clip.x || first->drawenv.clip.y != second->drawenv.clip.y ||
	    first->drawenv.clip.w != second->drawenv.clip.w || first->drawenv.clip.h != second->drawenv.clip.h ||
	    first->dispenv.disp.x != second->dispenv.disp.x || first->dispenv.disp.y != second->dispenv.disp.y ||
	    first->dispenv.disp.w != second->dispenv.disp.w || first->dispenv.disp.h != second->dispenv.disp.h ||
	    first->dispenv.isinter != second->dispenv.isinter)
	{
		return false;
	}

	if (first->drawenv.tw.x != second->drawenv.tw.x || first->drawenv.tw.y != second->drawenv.tw.y ||
	    first->drawenv.tw.w != second->drawenv.tw.w || first->drawenv.tw.h != second->drawenv.tw.h ||
	    first->overrideTextureWidth != second->overrideTextureWidth ||
	    first->overrideTextureHeight != second->overrideTextureHeight)
	{
		return false;
	}

	return true;
}

#endif

internal void NativeGpu_DrawSplitRangePass(const GPUDrawSplit *split, int semiTransPass, BlendMode blendMode, bool depthWrite, int startVertex,
                                           int numVerts)
{
	if (split->debugText)
	{
		NativeRenderer_PushDebugLabel(split->debugText);
	}

	const bool drawOnScreen = split->drawenv.dfe;
	if ((split->drawenv.clip.w <= 0) || (split->drawenv.clip.h <= 0))
	{
		/* An empty GP0 draw area writes nothing. Preserve the previous segment
		 * until a valid split or a VRAM barrier commits it; the next draw sets
		 * both its target and scissor before submitting geometry. */
		if (split->debugText)
		{
			NativeRenderer_PopDebugLabel();
		}
		return;
	}

	NativeRenderer_SetStencilMode(split->drawPrimMode); // draw with mask 0x16

	NativeRenderer_SetOffscreenState(&split->drawenv.clip, !drawOnScreen);
	NativeRenderer_SetupClipMode(&split->drawenv.clip, &split->dispenv, drawOnScreen);
	NativeGpu_SetSplitShaderState(split, semiTransPass, blendMode, !drawOnScreen);
	NativeRenderer_SetDepthState(1, depthWrite);
	NativeRenderer_DrawTriangles(startVertex, numVerts / 3);

	if (split->debugText)
	{
		NativeRenderer_PopDebugLabel();
	}
}

internal void NativeGpu_DrawSplitPass(const GPUDrawSplit *split, int semiTransPass, BlendMode blendMode, bool depthWrite)
{
	NativeGpu_DrawSplitRangePass(split, semiTransPass, blendMode, depthWrite, split->startVertex, split->numVerts);
}

void DrawSplit(const GPUDrawSplit *split)
{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	if (split->drawPrimMode == 2 && split->uploadPixels != NULL)
	{
		return;
	}
	if (split->drawPrimMode == 2)
	{
#ifdef __vita__
		const DRAWENV savedDrawEnv = s_gpuBackendDrawEnv;
		const DISPENV savedDispEnv = s_gpuBackendDispEnv;
		NativeGpu_BackendApplyEnv(&split->drawenv, &split->dispenv);
#endif
		if (!NativeRenderer_FillMainTarget(split->clearRect.x, split->clearRect.y, split->clearRect.w, split->clearRect.h,
		                                   split->clearR, split->clearG, split->clearB))
		{
			NativeRenderer_ClearVRAM(split->clearRect.x, split->clearRect.y, split->clearRect.w, split->clearRect.h, split->clearR,
			                         split->clearG, split->clearB);
			NativeRenderer_UpdateVRAM();
		}
#ifdef __vita__
		NativeGpu_BackendApplyEnv(&savedDrawEnv, &savedDispEnv);
#endif
		return;
	}
#endif

	if (split->psxTexturedSemiTrans)
	{
#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
		if (NativeGpu_CanUseMixedSTPPass(split))
		{
			NativeGpu_DrawSplitPass(split, 3, split->blendMode, true);
			return;
		}
#endif
		// NOTE(aalhendi): CTR native renderer divergence from upstream PsyCross. PS1 textured ABE only blends texels whose sampled 16-bit color has STP set; non-STP texels remain opaque. Native split state is per draw, so draw this primitive-sized split twice with shader-side STP masks.
		if ((split->psxSemiTransPassMask & 1) != 0)
		{
			NativeGpu_DrawSplitPass(split, 1, BM_NONE, true);
		}

		if ((split->psxSemiTransPassMask & 2) != 0)
		{
			NativeGpu_DrawSplitPass(split, 2, split->blendMode, false);
		}
	}
	else
	{
		NativeGpu_DrawSplitPass(split, 0, split->blendMode, split->blendMode == BM_NONE);
	}
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
internal bool NativeGpu_SplitsShareReorderDomain(const GPUDrawSplit *first, const GPUDrawSplit *second)
{
	if (first->drawPrimMode || second->drawPrimMode || first->drawenv.dfe != second->drawenv.dfe)
	{
		return false;
	}

	/* Rect/page switches are VRAM write barriers for both target modes. */
	return first->drawenv.clip.x == second->drawenv.clip.x && first->drawenv.clip.y == second->drawenv.clip.y &&
	       first->drawenv.clip.w == second->drawenv.clip.w && first->drawenv.clip.h == second->drawenv.clip.h;
}

internal u32 NativeGpu_DepthHashMix(u32 hash, u32 value)
{
	hash ^= value;
	hash *= 16777619u;
	return hash;
}

internal u32 NativeGpu_GetDepthStateHash(const GPUDrawSplit *split, int semiTransPass)
{
	u32 hash = 2166136261u;
	hash = NativeGpu_DepthHashMix(hash, (u32)semiTransPass);
	hash = NativeGpu_DepthHashMix(hash, (u32)split->textureId);
	hash = NativeGpu_DepthHashMix(hash, (u32)split->texFormat);
	hash = NativeGpu_DepthHashMix(hash, (u32)split->drawPrimMode);
	hash = NativeGpu_DepthHashMix(hash, (u32)split->psxTextureOutputSTP);
	hash = NativeGpu_DepthHashMix(hash, (u32)split->psxDrawMaskSet);
	hash = NativeGpu_DepthHashMix(hash, (u32)split->psxTextureFullyOpaque);
	hash = NativeGpu_DepthHashMix(hash, (u32)split->p4CacheEligible);
	if (split->p4CacheEligible)
	{
		hash = NativeGpu_DepthHashMix(hash, (u32)split->p4Page);
		hash = NativeGpu_DepthHashMix(hash, (u32)split->p4Clut);
		hash = NativeGpu_DepthHashMix(hash, (u32)split->p4SuperTurboTint);
	}
	hash = NativeGpu_DepthHashMix(hash, (u32)(uintptr_t)split->debugText);
	hash = NativeGpu_DepthHashMix(hash, (u32)split->drawenv.dfe);
	hash = NativeGpu_DepthHashMix(hash, (u32)(u16)split->drawenv.clip.x | ((u32)(u16)split->drawenv.clip.y << 16));
	hash = NativeGpu_DepthHashMix(hash, (u32)(u16)split->drawenv.clip.w | ((u32)(u16)split->drawenv.clip.h << 16));
	hash = NativeGpu_DepthHashMix(hash, (u32)(u16)split->dispenv.disp.x | ((u32)(u16)split->dispenv.disp.y << 16));
	hash = NativeGpu_DepthHashMix(hash, (u32)(u16)split->dispenv.disp.w | ((u32)(u16)split->dispenv.disp.h << 16));
	hash = NativeGpu_DepthHashMix(hash, (u32)split->dispenv.isinter);
	hash = NativeGpu_DepthHashMix(hash, (u32)(u16)split->drawenv.tw.x | ((u32)(u16)split->drawenv.tw.y << 16));
	hash = NativeGpu_DepthHashMix(hash, (u32)(u16)split->drawenv.tw.w | ((u32)(u16)split->drawenv.tw.h << 16));
	hash = NativeGpu_DepthHashMix(hash, (u32)(u16)split->overrideTextureWidth | ((u32)(u16)split->overrideTextureHeight << 16));
	return hash;
}

internal u32 NativeGpu_BeginDepthHashGeneration(void)
{
	s_gpuDepthHashGeneration++;
	if (s_gpuDepthHashGeneration == 0)
	{
		memset(s_gpuDepthHashTable, 0, sizeof(s_gpuDepthHashTable));
		s_gpuDepthHashGeneration = 1;
	}
	return s_gpuDepthHashGeneration;
}

internal bool NativeGpu_BuildDepthBatches(void)
{
	s_gpuDepthBatchCount = 0;
	if (s_gpuDrawSplitCount <= 0)
	{
		return true;
	}

	memset(s_gpuDepthNextSplit, 0xff, sizeof(s_gpuDepthNextSplit));
	for (int i = 1; i <= s_gpuDrawSplitCount; i++)
	{
		NativeGpuDepthPassInfo *info = &s_gpuDepthPassInfo[i];
		info->valid = NativeGpu_GetDepthPassInfo(&s_gpuDrawSplits[i], &info->category, &info->semiTransPass);
	}

	int splitIndex = 1;
	while (splitIndex <= s_gpuDrawSplitCount)
	{
		GPUDrawSplit *first = &s_gpuDrawSplits[splitIndex];
		const int firstSplit = splitIndex;
		int lastSplit = firstSplit;
		if (!first->drawPrimMode)
		{
			while (lastSplit + 1 <= s_gpuDrawSplitCount && NativeGpu_SplitsShareReorderDomain(first, &s_gpuDrawSplits[lastSplit + 1]))
			{
				lastSplit++;
			}
		}

		if (!first->drawPrimMode)
		{
			for (int categoryIndex = NATIVE_GPU_PASS_OPAQUE; categoryIndex <= NATIVE_GPU_PASS_DISCARD; categoryIndex++)
			{
				const NativeGpuPassCategory wantedCategory = (NativeGpuPassCategory)categoryIndex;
				const u32 generation = NativeGpu_BeginDepthHashGeneration();
				for (int i = firstSplit; i <= lastSplit; i++)
				{
					const NativeGpuDepthPassInfo *info = &s_gpuDepthPassInfo[i];
					if (!info->valid || info->category != wantedCategory)
					{
						continue;
					}

					GPUDrawSplit *split = &s_gpuDrawSplits[i];
					const u32 hash = NativeGpu_GetDepthStateHash(split, info->semiTransPass);
					u32 slot = hash & (NATIVE_GPU_DEPTH_HASH_CAPACITY - 1);
					for (u32 probe = 0; probe < NATIVE_GPU_DEPTH_HASH_CAPACITY; probe++)
					{
						NativeGpuDepthHashEntry *entry = &s_gpuDepthHashTable[slot];
						if (entry->generation != generation)
						{
							if (s_gpuDepthBatchCount >= MAX_DRAW_SPLITS)
							{
								return false;
							}
							const int batchIndex = s_gpuDepthBatchCount++;
							NativeGpuDepthBatch *batch = &s_gpuDepthBatches[batchIndex];
							batch->domainFirstSplit = firstSplit;
							batch->representativeSplit = i;
							batch->lastMemberSplit = i;
							batch->semiTransPass = info->semiTransPass;
							batch->category = info->category;
							entry->generation = generation;
							entry->hash = hash;
							entry->batchIndex = batchIndex;
							break;
						}

						if (entry->hash == hash)
						{
							NativeGpuDepthBatch *batch = &s_gpuDepthBatches[entry->batchIndex];
							const GPUDrawSplit *representative = &s_gpuDrawSplits[batch->representativeSplit];
							if (NativeGpu_DepthPassStateCompatible(representative, batch->semiTransPass, split, info->semiTransPass))
							{
								s_gpuDepthNextSplit[batch->lastMemberSplit] = i;
								batch->lastMemberSplit = i;
								break;
							}
						}
						slot = (slot + 1) & (NATIVE_GPU_DEPTH_HASH_CAPACITY - 1);
					}
				}
			}
		}

		splitIndex = lastSplit + 1;
	}
	return true;
}

internal void NativeGpu_DrawDepthBatch(const NativeGpuDepthBatch *batch)
{
	const GPUDrawSplit *split = &s_gpuDrawSplits[batch->representativeSplit];
	if (split->debugText)
	{
		NativeRenderer_PushDebugLabel(split->debugText);
	}

	const bool drawOnScreen = split->drawenv.dfe;
	NativeRenderer_SetStencilMode(split->drawPrimMode);
	NativeRenderer_SetOffscreenState(&split->drawenv.clip, !drawOnScreen);
	NativeRenderer_SetupClipMode(&split->drawenv.clip, &split->dispenv, drawOnScreen);
	NativeGpu_SetSplitShaderState(split, batch->semiTransPass, BM_NONE, !drawOnScreen);
	NativeRenderer_SetDepthState(1, true);

	if (batch->drawCount >= 0)
	{
		if (batch->drawCount > 0)
			NativeRenderer_DrawTriangles(batch->drawStart, batch->drawCount / 3);
		if (split->debugText)
		{
			NativeRenderer_PopDebugLabel();
		}
		return;
	}

	/* Members are in submission order; one draw covers each run of them
	 * whose vertices are adjacent. */
	int runStart = 0, runCount = 0;
	for (int memberIndex = batch->representativeSplit; memberIndex > 0; memberIndex = s_gpuDepthNextSplit[memberIndex])
	{
		const GPUDrawSplit *member = &s_gpuDrawSplits[memberIndex];
		const int count = member->numVerts / 3 * 3;
		if (runCount > 0 && member->startVertex == runStart + runCount)
		{
			runCount += count;
			continue;
		}
		if (runCount > 0)
			NativeRenderer_DrawTriangles(runStart, runCount / 3);
		runStart = member->startVertex;
		runCount = count;
	}
	if (runCount > 0)
		NativeRenderer_DrawTriangles(runStart, runCount / 3);

	if (split->debugText)
	{
		NativeRenderer_PopDebugLabel();
	}
}

internal void NativeGpu_DrawBlendPass(const GPUDrawSplit *split)
{
	if (split->psxTexturedSemiTrans)
	{
		if (NativeGpu_CanUseMixedSTPPass(split))
		{
			NativeGpu_DrawSplitPass(split, 3, split->blendMode, true);
			return;
		}
		if ((split->psxSemiTransPassMask & 2) != 0)
		{
			NativeGpu_DrawSplitPass(split, 2, split->blendMode, false);
		}
		return;
	}

	if (split->blendMode != BM_NONE)
	{
		NativeGpu_DrawSplitPass(split, 0, split->blendMode, false);
	}
}

internal bool NativeGpu_GetBlendPassInfo(const GPUDrawSplit *split, NativeGpuPassCategory *category, int *semiTransPass)
{
	if ((split->drawenv.clip.w <= 0) || (split->drawenv.clip.h <= 0))
	{
		return false;
	}

	if (split->psxTexturedSemiTrans)
	{
		if (NativeGpu_CanUseMixedSTPPass(split))
		{
			*category = NativeGpu_GetPassCategory(split, split->blendMode);
			*semiTransPass = 3;
			return true;
		}
		if ((split->psxSemiTransPassMask & 2) == 0)
		{
			return false;
		}
		*category = NativeGpu_GetPassCategory(split, split->blendMode);
		*semiTransPass = 2;
		return true;
	}

	if (split->blendMode == BM_NONE)
	{
		return false;
	}

	*category = NativeGpu_GetPassCategory(split, split->blendMode);
	*semiTransPass = 0;
	return true;
}

internal bool NativeGpu_BoundsOverlap(const struct NativeGpuSplitBounds *a, const struct NativeGpuSplitBounds *b)
{
	return !(a->maxX < b->minX || b->maxX < a->minX || a->maxY < b->minY || b->maxY < a->minY);
}

internal bool NativeGpu_BlendScissorStateCompatible(const GPUDrawSplit *first, const GPUDrawSplit *second)
{
	return first->drawenv.clip.x == second->drawenv.clip.x && first->drawenv.clip.y == second->drawenv.clip.y &&
	       first->drawenv.clip.w == second->drawenv.clip.w && first->drawenv.clip.h == second->drawenv.clip.h &&
	       first->dispenv.disp.x == second->dispenv.disp.x && first->dispenv.disp.y == second->dispenv.disp.y &&
	       first->dispenv.disp.w == second->dispenv.disp.w && first->dispenv.disp.h == second->dispenv.disp.h &&
	       first->dispenv.isinter == second->dispenv.isinter;
}

internal bool NativeGpu_BlendPassStateCompatible(const GPUDrawSplit *first, int firstSemiTransPass, const GPUDrawSplit *second,
                                                  int secondSemiTransPass)
{
	if (firstSemiTransPass != secondSemiTransPass || first->blendMode != second->blendMode || first->textureId != second->textureId ||
	    first->texFormat != second->texFormat || first->psxTextureOutputSTP != second->psxTextureOutputSTP ||
	    first->psxDrawMaskSet != second->psxDrawMaskSet || first->psxTextureFullyOpaque != second->psxTextureFullyOpaque ||
	    first->p4CacheEligible != second->p4CacheEligible ||
	    (first->p4CacheEligible &&
	     (first->p4Page != second->p4Page || first->p4Clut != second->p4Clut || first->p4SuperTurboTint != second->p4SuperTurboTint)) ||
	    first->debugText != second->debugText || first->drawenv.dfe != second->drawenv.dfe)
	{
		return false;
	}

	if (!NativeGpu_BlendScissorStateCompatible(first, second))
	{
		return false;
	}

	if (first->drawenv.tw.x != second->drawenv.tw.x || first->drawenv.tw.y != second->drawenv.tw.y ||
	    first->drawenv.tw.w != second->drawenv.tw.w || first->drawenv.tw.h != second->drawenv.tw.h ||
	    first->overrideTextureWidth != second->overrideTextureWidth ||
	    first->overrideTextureHeight != second->overrideTextureHeight)
	{
		return false;
	}

	return true;
}

internal void NativeGpu_FlushBlendBatch(NativeGpuBlendBatch *batch)
{
	if (!batch->active)
	{
		return;
	}

	const GPUDrawSplit *split = &s_gpuDrawSplits[batch->representativeSplit];
	NativeGpu_DrawSplitRangePass(split, batch->semiTransPass, split->blendMode, batch->semiTransPass == 3, batch->startVertex, batch->numVerts);
	batch->active = false;
}

internal void NativeGpu_AppendBlendBatch(NativeGpuBlendBatch *batch, int splitIndex, int semiTransPass)
{
	const GPUDrawSplit *split = &s_gpuDrawSplits[splitIndex];
	if (batch->active)
	{
		const GPUDrawSplit *representative = &s_gpuDrawSplits[batch->representativeSplit];
		if (batch->startVertex + batch->numVerts == split->startVertex &&
		    NativeGpu_BlendPassStateCompatible(representative, batch->semiTransPass, split, semiTransPass))
		{
			batch->numVerts += split->numVerts;
			return;
		}

		NativeGpu_FlushBlendBatch(batch);
	}

	batch->representativeSplit = splitIndex;
	batch->startVertex = split->startVertex;
	batch->numVerts = split->numVerts;
	batch->semiTransPass = semiTransPass;
	batch->active = true;
}

internal int NativeGpu_BlendCandidateScore(const NativeGpuBlendNode *candidate, const NativeGpuBlendNode *previous)
{
	if (previous == NULL)
	{
		return 0;
	}

	const GPUDrawSplit *candidateSplit = &s_gpuDrawSplits[candidate->splitIndex];
	const GPUDrawSplit *previousSplit = &s_gpuDrawSplits[previous->splitIndex];
	int score = 0;

	if (candidate->category == previous->category)
	{
		score += 1 << 24;
	}
	if (NativeGpu_BlendScissorStateCompatible(candidateSplit, previousSplit))
	{
		score += 1 << 20;
	}
	if (candidateSplit->p4CacheEligible && previousSplit->p4CacheEligible && candidateSplit->p4Page == previousSplit->p4Page &&
	    candidateSplit->p4Clut == previousSplit->p4Clut && candidateSplit->p4SuperTurboTint == previousSplit->p4SuperTurboTint)
	{
		score += 1 << 18;
	}
	if (candidateSplit->blendMode == previousSplit->blendMode)
	{
		score += 1 << 16;
	}
	if (candidateSplit->psxTexturedSemiTrans == previousSplit->psxTexturedSemiTrans)
	{
		score += 1 << 14;
	}
	if (candidateSplit->textureId == previousSplit->textureId)
	{
		score += 1 << 12;
	}
	if (candidateSplit->texFormat == previousSplit->texFormat)
	{
		score += 1 << 10;
	}
	if (candidateSplit->psxTextureOutputSTP == previousSplit->psxTextureOutputSTP &&
	    candidateSplit->psxDrawMaskSet == previousSplit->psxDrawMaskSet)
	{
		score += 1 << 8;
	}

	return score;
}

internal void NativeGpu_DrawScheduledBlendPasses(int firstSplit, int lastSplit)
{
	int nodeCount = 0;
	for (int splitIndex = firstSplit; splitIndex <= lastSplit; splitIndex++)
	{
		NativeGpuPassCategory category;
		int semiTransPass;
		if (!NativeGpu_GetBlendPassInfo(&s_gpuDrawSplits[splitIndex], &category, &semiTransPass))
		{
			continue;
		}

		NativeGpuBlendNode *node = &s_gpuBlendNodes[nodeCount];
		if (!NativeGpu_GetSplitBounds(&s_gpuDrawSplits[splitIndex], &node->bounds))
		{
			continue;
		}

		node->splitIndex = splitIndex;
		node->indegree = 0;
		node->scheduled = false;
		node->category = category;
		nodeCount++;
	}

	NativeGpuBlendBatch blendBatch;
	memset(&blendBatch, 0, sizeof(blendBatch));
	if (nodeCount > 256)
	{
		for (int nodeIndex = 0; nodeIndex < nodeCount; nodeIndex++)
		{
			NativeGpuPassCategory ignoredCategory;
			int semiTransPass;
			const int splitIndex = s_gpuBlendNodes[nodeIndex].splitIndex;
			if (NativeGpu_GetBlendPassInfo(&s_gpuDrawSplits[splitIndex], &ignoredCategory, &semiTransPass))
			{
				NativeGpu_AppendBlendBatch(&blendBatch, splitIndex, semiTransPass);
			}
		}
		NativeGpu_FlushBlendBatch(&blendBatch);
		return;
	}

	for (int firstNode = 0; firstNode < nodeCount; firstNode++)
	{
		for (int secondNode = firstNode + 1; secondNode < nodeCount; secondNode++)
		{
			if (NativeGpu_BoundsOverlap(&s_gpuBlendNodes[firstNode].bounds, &s_gpuBlendNodes[secondNode].bounds))
			{
				s_gpuBlendNodes[secondNode].indegree++;
			}
		}
	}
	NativeGpuBlendNode *previous = NULL;
	for (int outputIndex = 0; outputIndex < nodeCount; outputIndex++)
	{
		int bestNode = -1;
		int bestScore = -1;
		for (int nodeIndex = 0; nodeIndex < nodeCount; nodeIndex++)
		{
			NativeGpuBlendNode *candidate = &s_gpuBlendNodes[nodeIndex];
			if (candidate->scheduled || candidate->indegree != 0)
			{
				continue;
			}

			const int score = NativeGpu_BlendCandidateScore(candidate, previous);
			if (bestNode < 0 || score > bestScore ||
			    (score == bestScore && candidate->splitIndex < s_gpuBlendNodes[bestNode].splitIndex))
			{
				bestNode = nodeIndex;
				bestScore = score;
			}
		}

		if (bestNode < 0)
		{
			NativeGpu_FlushBlendBatch(&blendBatch);
			NATIVE_GPU_ERROR("%s\n", "blend dependency scheduler reached an invalid cycle");
			for (int nodeIndex = 0; nodeIndex < nodeCount; nodeIndex++)
			{
				if (!s_gpuBlendNodes[nodeIndex].scheduled)
				{
					NativeGpu_DrawBlendPass(&s_gpuDrawSplits[s_gpuBlendNodes[nodeIndex].splitIndex]);
					s_gpuBlendNodes[nodeIndex].scheduled = true;
				}
			}
			return;
		}

		NativeGpuBlendNode *selected = &s_gpuBlendNodes[bestNode];
		NativeGpuPassCategory ignoredCategory;
		int semiTransPass;
		if (!NativeGpu_GetBlendPassInfo(&s_gpuDrawSplits[selected->splitIndex], &ignoredCategory, &semiTransPass))
		{
			NativeGpu_FlushBlendBatch(&blendBatch);
			NATIVE_GPU_ERROR("%s\n", "scheduled blend node lost its pass metadata");
			return;
		}
		NativeGpu_AppendBlendBatch(&blendBatch, selected->splitIndex, semiTransPass);
		selected->scheduled = true;

		for (int nodeIndex = bestNode + 1; nodeIndex < nodeCount; nodeIndex++)
		{
			NativeGpuBlendNode *dependent = &s_gpuBlendNodes[nodeIndex];
			if (!dependent->scheduled && NativeGpu_BoundsOverlap(&selected->bounds, &dependent->bounds))
			{
				assert(dependent->indegree > 0);
				dependent->indegree--;
			}
		}

		previous = selected;
	}

	NativeGpu_FlushBlendBatch(&blendBatch);
}

internal void NativeGpu_DrawReorderDomain(int firstSplit, int lastSplit, int *depthBatchIndex)
{
	while (*depthBatchIndex < s_gpuDepthBatchCount && s_gpuDepthBatches[*depthBatchIndex].domainFirstSplit == firstSplit)
	{
		NativeGpu_DrawDepthBatch(&s_gpuDepthBatches[*depthBatchIndex]);
		(*depthBatchIndex)++;
	}

	NativeGpu_DrawScheduledBlendPasses(firstSplit, lastSplit);
}

internal void NativeGpu_DrawReorderedSplits(void)
{
	int depthBatchIndex = 0;
	int splitIndex = 1;
	while (splitIndex <= s_gpuDrawSplitCount)
	{
		GPUDrawSplit *first = &s_gpuDrawSplits[splitIndex];
		if (first->drawPrimMode)
		{
			DrawSplit(first);
			splitIndex++;
			continue;
		}

		const int firstSplit = splitIndex;
		int lastSplit = firstSplit;
		while (lastSplit + 1 <= s_gpuDrawSplitCount && NativeGpu_SplitsShareReorderDomain(first, &s_gpuDrawSplits[lastSplit + 1]))
		{
			lastSplit++;
		}

		NativeGpu_DrawReorderDomain(firstSplit, lastSplit, &depthBatchIndex);
		splitIndex = lastSplit + 1;
	}
}
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
internal bool NativeGpu_GetSplitBounds(const GPUDrawSplit *split, struct NativeGpuSplitBounds *bounds)
{
	if (split->numVerts == 0)
	{
		return false;
	}

	const GrVertex *first = &s_gpuDrawVertices[split->startVertex];
	bounds->minX = bounds->maxX = first->x;
	bounds->minY = bounds->maxY = first->y;
	for (u32 i = 1; i < split->numVerts; i++)
	{
		const GrVertex *vertex = &s_gpuDrawVertices[split->startVertex + i];
		if (vertex->x < bounds->minX)
		{
			bounds->minX = vertex->x;
		}
		if (vertex->x > bounds->maxX)
		{
			bounds->maxX = vertex->x;
		}
		if (vertex->y < bounds->minY)
		{
			bounds->minY = vertex->y;
		}
		if (vertex->y > bounds->maxY)
		{
			bounds->maxY = vertex->y;
		}
	}

	return true;
}

internal bool NativeGpu_SemiTransSplitsCanMerge(const GPUDrawSplit *first, const GPUDrawSplit *second)
{
	if (!first->psxTexturedSemiTrans || !second->psxTexturedSemiTrans || first->startVertex + first->numVerts != second->startVertex ||
	    (u32)first->numVerts + second->numVerts > 0xffffu)
	{
		return false;
	}

	if (first->blendMode != second->blendMode || first->texFormat != second->texFormat || first->textureId != second->textureId ||
	    first->drawPrimMode != second->drawPrimMode || first->psxTextureOutputSTP != second->psxTextureOutputSTP ||
	    first->psxDrawMaskSet != second->psxDrawMaskSet || first->p4CacheEligible != second->p4CacheEligible ||
	    (first->p4CacheEligible &&
	     (first->p4Page != second->p4Page || first->p4Clut != second->p4Clut || first->p4SuperTurboTint != second->p4SuperTurboTint)) ||
	    first->debugText != second->debugText ||
	    memcmp(&first->drawenv, &second->drawenv, sizeof(first->drawenv)) != 0 || memcmp(&first->dispenv, &second->dispenv, sizeof(first->dispenv)) != 0)
	{
		return false;
	}

	struct NativeGpuSplitBounds firstBounds;
	struct NativeGpuSplitBounds secondBounds;
	if (!NativeGpu_GetSplitBounds(first, &firstBounds) || !NativeGpu_GetSplitBounds(second, &secondBounds))
	{
		return false;
	}

	// Preserve exact primitive ordering whenever their covered regions can overlap. Disjoint primitives may share the same opaque/STP draw pair.
	return firstBounds.maxX <= secondBounds.minX || secondBounds.maxX <= firstBounds.minX || firstBounds.maxY <= secondBounds.minY ||
	       secondBounds.maxY <= firstBounds.minY;
}

internal void NativeGpu_CoalesceNonOverlappingSemiTransSplits(void)
{
	int outputIndex = 0;
	for (int inputIndex = 1; inputIndex <= s_gpuDrawSplitCount; inputIndex++)
	{
		GPUDrawSplit split = s_gpuDrawSplits[inputIndex];
		if (outputIndex > 0 && NativeGpu_SemiTransSplitsCanMerge(&s_gpuDrawSplits[outputIndex], &split))
		{
			s_gpuDrawSplits[outputIndex].numVerts = (u16)(s_gpuDrawSplits[outputIndex].numVerts + split.numVerts);
			continue;
		}

		outputIndex++;
		if (outputIndex != inputIndex)
		{
			s_gpuDrawSplits[outputIndex] = split;
		}
	}
	s_gpuDrawSplitCount = outputIndex;
}

internal void NativeGpu_ClassifyOpaqueTextureSplits(void)
{
	for (int splitIndex = 1; splitIndex <= s_gpuDrawSplitCount; splitIndex++)
	{
		GPUDrawSplit *split = &s_gpuDrawSplits[splitIndex];
		split->psxTextureFullyOpaque = false;
		split->psxSemiTransPassMask = split->psxTexturedSemiTrans ? 3 : 0;
		if (split->numVerts == 0)
		{
			continue;
		}

		if (split->textureId == NativeRenderer_GetWhiteTexture())
		{
			continue;
		}
		if (!split->psxTextureOutputSTP)
		{
			// RGBA override textures have no PS1 palette to classify here.
			continue;
		}
		if (split->psxTexturedSemiTrans)
		{
			if (split->texFormat == TF_4_BIT || split->texFormat == TF_8_BIT)
			{
				u8 paletteProperties = 0;
				for (u32 vertexIndex = split->startVertex; vertexIndex + 2 < (u32)split->startVertex + split->numVerts; vertexIndex += 3)
				{
					const GrVertex *vertex = &s_gpuDrawVertices[vertexIndex];
					paletteProperties |= NativeRenderer_GetPaletteProperties(split->texFormat, vertex->clut);
					if (paletteProperties == (NATIVE_PALETTE_HAS_TRANSPARENT | NATIVE_PALETTE_HAS_OPAQUE | NATIVE_PALETTE_HAS_STP))
					{
						break;
					}
				}

				if ((paletteProperties & NATIVE_PALETTE_HAS_OPAQUE) == 0)
				{
					split->psxSemiTransPassMask &= ~1;
				}
				if ((paletteProperties & NATIVE_PALETTE_HAS_STP) == 0)
				{
					split->psxSemiTransPassMask &= ~2;
				}
				if ((split->psxSemiTransPassMask == 1 || split->psxSemiTransPassMask == 2) &&
				    (paletteProperties & NATIVE_PALETTE_HAS_TRANSPARENT) == 0)
				{
					split->psxTextureFullyOpaque = true;
				}
			}
			continue;
		}

		if (split->texFormat != TF_4_BIT && split->texFormat != TF_8_BIT)
		{
			continue;
		}

		bool allOpaque = true;
		for (u32 vertexIndex = split->startVertex; vertexIndex + 2 < (u32)split->startVertex + split->numVerts; vertexIndex += 3)
		{
			const GrVertex *vertex = &s_gpuDrawVertices[vertexIndex];
			const u8 paletteProperties = NativeRenderer_GetPaletteProperties(split->texFormat, vertex->clut);
			if ((paletteProperties & NATIVE_PALETTE_HAS_TRANSPARENT) != 0)
			{
				allOpaque = false;
				break;
			}
		}

		split->psxTextureFullyOpaque = allOpaque;
	}
}
#endif

internal void SetPSXMaskState(u32 code)
{
	s_gpu.psxDrawMaskSet = (code & 1) != 0;
}

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
/* Uploads the frame's vertices, followed by each depth batch's members in
 * member order: a batch collects splits of one state from the whole
 * reorder domain, whose vertices are interleaved with other batches', and
 * every vitaGL draw costs CPU time. Copied, each batch is a single draw. */
internal void NativeGpu_UploadFrameVertices(bool depthBatchingReady)
{
	int total = s_gpuDrawVertexCount;
	for (int b = 0; b < s_gpuDepthBatchCount; b++)
	{
		NativeGpuDepthBatch *batch = &s_gpuDepthBatches[b];
		batch->drawCount = -1;
		if (!depthBatchingReady)
			continue;
		int count = 0;
		for (int m = batch->representativeSplit; m > 0; m = s_gpuDepthNextSplit[m])
			count += s_gpuDrawSplits[m].numVerts / 3 * 3;
		if ((u32)(total + count) > MAX_VERTEX_BUFFER_SIZE)
			continue;
		batch->drawStart = total;
		batch->drawCount = count;
		total += count;
	}
	if (s_gpuDrawVertexCount <= 0)
		return;
	GrVertex *dst = NativeRenderer_MapVertexBuffer(total);
	if (dst == NULL)
	{
		for (int b = 0; b < s_gpuDepthBatchCount; b++)
			s_gpuDepthBatches[b].drawCount = -1;
		NativeRenderer_UpdateVertexBuffer(s_gpuDrawVertices, s_gpuDrawVertexCount);
		return;
	}
	memcpy(dst, s_gpuDrawVertices, (size_t)s_gpuDrawVertexCount * sizeof(GrVertex));
	for (int b = 0; b < s_gpuDepthBatchCount; b++)
	{
		const NativeGpuDepthBatch *batch = &s_gpuDepthBatches[b];
		if (batch->drawCount <= 0)
			continue;
		GrVertex *out = dst + batch->drawStart;
		for (int m = batch->representativeSplit; m > 0; m = s_gpuDepthNextSplit[m])
		{
			const GPUDrawSplit *member = &s_gpuDrawSplits[m];
			const int count = member->numVerts / 3 * 3;
			memcpy(out, s_gpuDrawVertices + member->startVertex, (size_t)count * sizeof(GrVertex));
			out += count;
		}
	}
	NativeRenderer_CommitVertexBuffer(dst);
}
#endif

// Draws all polygons after AggregatePTAG
internal void NativeGpu_DrawPreparedFrame(GrVertex *vertices, GPUDrawSplit *splits, int vertexCount, int splitCount)
{
#if defined(__vita__) && defined(VITA_CTR_GPU_TRACE)
	static int traceFrameCount;
	if (traceFrameCount < 8)
	{
		SysPrintf("pcsx-vita: gpu-trace frame[%d] vertices=%d splits=%d target=(%d,%d %dx%d dfe=%d)\n",
		          traceFrameCount, vertexCount, splitCount,
	          NativeGpu_GetRenderDrawEnv()->clip.x, NativeGpu_GetRenderDrawEnv()->clip.y,
	          NativeGpu_GetRenderDrawEnv()->clip.w, NativeGpu_GetRenderDrawEnv()->clip.h,
	          NativeGpu_GetRenderDrawEnv()->dfe);
		for (int i = 1; i <= splitCount && i <= 4; i++)
		{
			const GPUDrawSplit *split = &splits[i];
			SysPrintf("pcsx-vita: gpu-trace split[%d] start=%u count=%u blend=%d tex=%u fmt=%d dfe=%d clip=(%d,%d %dx%d) drawmode=%d\n",
			          i, (unsigned)split->startVertex, (unsigned)split->numVerts, split->blendMode, split->textureId, split->texFormat,
			          split->drawenv.dfe, split->drawenv.clip.x, split->drawenv.clip.y,
			          split->drawenv.clip.w, split->drawenv.clip.h, split->drawPrimMode);
		}
		traceFrameCount++;
	}
#endif
	if (splitCount > 0)
	{
		GPUDrawSplit *lastSplit = &splits[splitCount];
		lastSplit->numVerts = vertexCount - lastSplit->startVertex;
	}
	s_gpuDrawVertices = vertices;
	s_gpuDrawSplits = splits;
	s_gpuDrawVertexCount = vertexCount;
	s_gpuDrawSplitCount = splitCount;
	// CPU-originated LoadImage, MoveImage, and fill commands are GPU-visible before the next draw batch, matching PS1 command ordering.
	NativeRenderer_UpdateVRAM();
#ifdef CTR_INTERNAL
	if (g_dbg_emulatorPaused)
	{
		for (int i = 0; i < 3; i++)
		{
			GrVertex *vert = &s_gpuDrawVertices[g_dbg_polygonSelected + i];
			vert->r = 255;
			vert->g = 0;
			vert->b = 0;

			NATIVE_GPU_LOG("%s\n", "==========================================");
			NATIVE_GPU_LOG("POLYGON: %d\n", g_dbg_polygonSelected);
			NATIVE_GPU_LOG("X: %d Y: %d\n", vert->x, vert->y);
			NATIVE_GPU_LOG("U: %d V: %d\n", vert->u, vert->v);
			NATIVE_GPU_LOG("TP: %d CLT: %d\n", vert->page, vert->clut);

			NATIVE_GPU_LOG("%s\n", "==========================================");
		}

		Platform_PollHostEvents();
	}
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	NativeGpu_ApplyQueuedUploads(s_gpuDrawSplits, s_gpuDrawSplitCount);
	NativeGpu_CoalesceNonOverlappingSemiTransSplits();
	NativeGpu_ClassifyOpaqueTextureSplits();
	const bool depthBatchingReady = NativeGpu_BuildDepthBatches();
	NativeGpu_UploadFrameVertices(depthBatchingReady);
#else
	// next code ideally should be called before EndScene
	NativeRenderer_UpdateVertexBuffer(s_gpuDrawVertices, s_gpuDrawVertexCount);
#endif

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
	for (int i = 1; i <= s_gpuDrawSplitCount; i++)
	{
		const GPUDrawSplit *split = &s_gpuDrawSplits[i];
		struct NativeGpuSplitBounds bounds;
		if (split->p4CacheEligible)
			NativeRenderer_QueueStaleP4Sources(split->p4Page, split->p4Clut);
		if (split->drawPrimMode != 2 && NativeGpu_GetSplitBounds(split, &bounds))
			NativeRenderer_NoteTargetDraw(&split->drawenv.clip, bounds.minX, bounds.minY, bounds.maxX, bounds.maxY);
	}
	NativeRenderer_SyncStaleP4Sources();
	NATIVE_GPU_LABEL(0, "prims", 0);
	if (depthBatchingReady)
	{
		NativeGpu_DrawReorderedSplits();
	}
	else
	{
		for (int i = 1; i <= s_gpuDrawSplitCount; i++)
		{
			DrawSplit(&s_gpuDrawSplits[i]);
		}
	}
	NATIVE_GPU_LABEL(1, NULL, 0);
#else
	for (int i = 1; i <= s_gpuDrawSplitCount; i++)
	{
		DrawSplit(&s_gpuDrawSplits[i]);
	}
#endif

}

void DrawAllSplits(void)
{
	NativeGpu_DrawPreparedFrame(s_gpu.vertexBuffer, s_gpu.splits, s_gpu.vertexIndex, s_gpu.splitIndex);
	ClearSplits();
}

// forward declarations
int ParsePrimitive(P_TAG *polyTag);
int ParseTaglessPrimitive(u32 *command);

internal bool NativeGpu_IsValidOTLink(uintptr_t link)
{
	(void)link;
	return false;
}

internal u32 NativeGpu_ReadPacketWordForLog(uintptr_t packet, int wordIndex)
{
	const uintptr_t word = packet + (uintptr_t)wordIndex * sizeof(u32);
	if ((word & (sizeof(u32) - 1)) != 0)
		return 0xffffffffu;
	return *(const u32 *)word;
}

internal void NativeGpu_FormatPointerRegion(char *dst, size_t dstSize, uintptr_t ptr)
{
	(void)ptr;
	snprintf(dst, dstSize, "psxrecomp-gp0");
}
void ParsePrimitivesLinkedList(u32 *p, int singlePrimitive)
{
	if (!p)
	{
		return;
	}

#ifdef CTR_NATIVE
	if (!singlePrimitive && !NativeGpuLinks_IsRegisteredHostPointer(p) && !isendprim(p))
	{
		char packetRegion[64];
		NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)p);
		NATIVE_GPU_ERROR("unregistered linked DrawOTag packet: packet=%p region=%s addr=%06x len=%d code=%02x words=%08x %08x %08x %08x\n", (void *)p,
		                 packetRegion, getaddr(p), getlen(p), getcode(p), NativeGpu_ReadPacketWordForLog((uintptr_t)p, 0),
		                 NativeGpu_ReadPacketWordForLog((uintptr_t)p, 1), NativeGpu_ReadPacketWordForLog((uintptr_t)p, 2),
		                 NativeGpu_ReadPacketWordForLog((uintptr_t)p, 3));
		return;
	}
#endif

	// setup single primitive flag (needed for AddSplits)
	s_gpu.drawPrimMode = singlePrimitive;

	if (singlePrimitive)
	{
		P_TAG *polyTag = (P_TAG *)p;
		ParsePrimitive(polyTag);

		GPUDrawSplit *lastSplit = &s_gpu.splits[s_gpu.splitIndex];
		lastSplit->numVerts = s_gpu.vertexIndex - lastSplit->startVertex;
	}
	else
	{
		// walk OT_TAG linked list
		u8 *basePacket = (u8 *)p;
		while (true)
		{
			const int tagLength = getlen(basePacket);
			if (tagLength > 0)
			{
				if (tagLength > 32)
				{
					char packetRegion[64];
					NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)basePacket);
					NATIVE_GPU_ERROR("got invalid tag length %d, code %d packet=%p region=%s words=%08x %08x %08x %08x\n", tagLength,
					                 ((P_TAG *)basePacket)->code, (void *)basePacket, packetRegion, NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 0),
					                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 1), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 2),
					                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 3));
					break;
				}

				u8 *currentPacket = basePacket;
				u8 *endPacket = basePacket + (tagLength + P_LEN) * sizeof(u32);
				int primLength = 0;
				if (currentPacket < endPacket)
				{
					primLength = ParsePrimitive((P_TAG *)currentPacket);
					currentPacket += (primLength + P_LEN) * sizeof(u32);
				}

				while (currentPacket < endPacket)
				{
					primLength = ParseTaglessPrimitive((u32 *)currentPacket);
					currentPacket += primLength * sizeof(u32);
				}

				if (currentPacket != endPacket)
				{
					char packetRegion[64];
					NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)basePacket);
					NATIVE_GPU_ERROR("did not output valid primitive or ptag length is not valid (diff=%d packet=%p region=%s words=%08x %08x %08x %08x)\n",
					                 endPacket - currentPacket, (void *)basePacket, packetRegion, NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 0),
					                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 1), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 2),
					                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 3));
				}
			}

			GPUDrawSplit *lastSplit = &s_gpu.splits[s_gpu.splitIndex];
			lastSplit->numVerts = s_gpu.vertexIndex - lastSplit->startVertex;

			if (isendprim(basePacket))
			{
				break;
			}

			u8 *nextPacket = nextPrim(basePacket);
			if (!NativeGpu_IsValidOTLink((uintptr_t)nextPacket))
			{
				char packetRegion[64];
				char nextRegion[64];
				NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)basePacket);
				NativeGpu_FormatPointerRegion(nextRegion, sizeof(nextRegion), (uintptr_t)nextPacket);
				NATIVE_GPU_ERROR("invalid OT link: packet=%p region=%s addr=%06x next=%p nextRegion=%s len=%d code=%02x words=%08x %08x %08x %08x\n",
				                 (void *)basePacket, packetRegion, getaddr(basePacket), (void *)nextPacket, nextRegion, getlen(basePacket), getcode(basePacket),
				                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 0), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 1),
				                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 2), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 3));
				break;
			}

			basePacket = nextPacket;
		}
	}

}

internal inline int IsNull(POLY_FT3 *poly)
{
	return poly->x0 == -1 && poly->y0 == -1 && poly->x1 == -1 && poly->y1 == -1 && poly->x2 == -1 && poly->y2 == -1;
}

internal int ProcessFlatLines(P_TAG *polyTag)
{
	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_F2 *poly = (LINE_F2 *)polyTag;

		AddSplit(semiTrans, false, false, 0);

		VERTTYPE *p0 = &poly->x0;
		VERTTYPE *p1 = &poly->x1;
		u8 *c0 = &poly->r0;
		u8 *c1 = c0;

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		LineSwapSourceVerts(&p0, &p1, &c0, &c1);
		MakeLineArray(firstVertex, p0, p1);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 3;
	}
	case 0x8: // TODO (unused)
	{
		LINE_F3 *poly = (LINE_F3 *)polyTag;

		AddSplit(semiTrans, false, false, 0);

		{
			VERTTYPE *p0 = &poly->x0;
			VERTTYPE *p1 = &poly->x1;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		{
			VERTTYPE *p0 = &poly->x1;
			VERTTYPE *p1 = &poly->x2;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		return 5;
	}
	case 0xc:
	{
		LINE_F4 *poly = (LINE_F4 *)polyTag;

		AddSplit(semiTrans, false, false, 0);

		{
			VERTTYPE *p0 = &poly->x0;
			VERTTYPE *p1 = &poly->x1;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		{
			VERTTYPE *p0 = &poly->x1;
			VERTTYPE *p1 = &poly->x2;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		{
			VERTTYPE *p0 = &poly->x2;
			VERTTYPE *p1 = &poly->x3;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		return 6;
	}
	}
	return 0;
}

internal int ProcessGouraudLines(P_TAG *polyTag)
{
	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_G2 *poly = (LINE_G2 *)polyTag;

		AddSplit(semiTrans, false, false, 0);

		VERTTYPE *p0 = &poly->x0;
		VERTTYPE *p1 = &poly->x1;
		u8 *c0 = &poly->r0;
		u8 *c1 = &poly->r1;

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		LineSwapSourceVerts(&p0, &p1, &c0, &c1);
		MakeLineArray(firstVertex, p0, p1);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 4;
	}
	case 0x8:
	{
		// TODO: LINE_G3
		return 7;
	}
	case 0xC:
	{
		// TODO: LINE_G4
		return 9;
	}
	}
	return 0;
}

internal int ProcessFlatPoly(P_TAG *polyTag)
{
	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;
#if defined(__vita__) && defined(VITA_CTR_GPU_TRACE)
	static int traceF3Count;
#endif

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_F3 *poly = (POLY_F3 *)polyTag;
#if defined(__vita__) && defined(VITA_CTR_GPU_TRACE)
		if (traceF3Count < 8)
		{
			SysPrintf("pcsx-vita: gpu-trace F3[%d] cmd=%08x rgb=%02x%02x%02x xy=(%d,%d),(%d,%d),(%d,%d) draw=(%d,%d %dx%d dfe=%d) disp=(%d,%d %dx%d)\n",
			          traceF3Count, ((u32)poly->code << 24) | ((u32)poly->b0 << 16) | ((u32)poly->g0 << 8) | poly->r0,
			          (unsigned)poly->r0, (unsigned)poly->g0, (unsigned)poly->b0, poly->x0, poly->y0, poly->x1, poly->y1, poly->x2, poly->y2,
			          activeDrawEnv.clip.x, activeDrawEnv.clip.y, activeDrawEnv.clip.w, activeDrawEnv.clip.h, activeDrawEnv.dfe,
			          activeDispEnv.disp.x, activeDispEnv.disp.y, activeDispEnv.disp.w, activeDispEnv.disp.h);
		}
#endif

		AddSplit(semiTrans, false, false, 0);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
		MakeTexcoordTriangleZero(firstVertex, 0);
		/* The raw-texture bit means nothing without a texture: GP0 0x21
		 * draws its colour (Tekken's black shadows). */
		MakeColourTriangle(firstVertex, true, &poly->r0, &poly->r0, &poly->r0);
#if defined(__vita__) && defined(VITA_CTR_GPU_TRACE)
		if (traceF3Count < 8)
		{
			SysPrintf("pcsx-vita: gpu-trace F3 verts=(%d,%d z=%u rgb=%u,%u,%u a=%u),(%d,%d z=%u),(%d,%d z=%u) index=%d\n",
			          firstVertex[0].x, firstVertex[0].y, (unsigned)firstVertex[0].orderDepth, (unsigned)firstVertex[0].r, (unsigned)firstVertex[0].g, (unsigned)firstVertex[0].b, (unsigned)firstVertex[0].a,
			          firstVertex[1].x, firstVertex[1].y, (unsigned)firstVertex[1].orderDepth, firstVertex[2].x, firstVertex[2].y, (unsigned)firstVertex[2].orderDepth,
			          s_gpu.vertexIndex);
			traceF3Count++;
		}
#endif

		s_gpu.vertexIndex += 3;

		return 4;
	}
	case 0x4:
	{
		POLY_FT3 *poly = (POLY_FT3 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		// It is an official hack from SCE devs to not use DR_TPAGE and instead use null polygon
		if (!IsNull(poly))
		{
			AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage), poly->clut);

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
			MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut,
			                     GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
			MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

			s_gpu.vertexIndex += 3;
		}
		return 7;
	}
	case 0x8:
	{
		POLY_F4 *poly = (POLY_F4 *)polyTag;

		AddSplit(semiTrans, false, false, 0);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;
		return 5;
	}
	case 0xC:
	{
		POLY_FT4 *poly = (POLY_FT4 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage), poly->clut);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut,
		                 GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 9;
	}
	}
	return 0;
}

internal int ProcessGouraudPoly(P_TAG *polyTag)
{
	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_G3 *poly = (POLY_G3 *)polyTag;

		AddSplit(semiTrans, false, false, 0);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
		MakeTexcoordTriangleZero(firstVertex, 1);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

		s_gpu.vertexIndex += 3;

		return 6;
	}
	case 0x4:
	{
		POLY_GT3 *poly = (POLY_GT3 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage), poly->clut);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
		MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);
		if (((u32)poly->tpage & NATIVE_GPU_TPAGE_SUPER_TURBO_TINT) != 0)
		{
			MakeColourSuperTurboTint(firstVertex, 3);
		}

		s_gpu.vertexIndex += 3;

		return 9;
	}
	case 0x8:
	{
		POLY_G4 *poly = (POLY_G4 *)polyTag;

		AddSplit(semiTrans, false, false, 0);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuadZero(firstVertex, 1);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 8;
	}
	case 0xC:
	{
		POLY_GT4 *poly = (POLY_GT4 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage), poly->clut);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut,
		                 GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);
		if (((u32)poly->tpage & NATIVE_GPU_TPAGE_SUPER_TURBO_TINT) != 0)
		{
			MakeColourSuperTurboTint(firstVertex, 4);
		}

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 12;
	}
	}
	return 0;
}

internal int ProcessTileAndSprt(P_TAG *polyTag)
{
	// NOTE: TILE does not support switching shadeTex on real PSX; only SPRT uses it
	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);

	switch (polyTag->code & 0xFC)
	{
	case 0x60:
	{
		TILE *poly = (TILE *)polyTag;

		AddSplit(semiTrans, false, false, 0);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 3;
	}
	case 0x64:
	{
		SPRT *poly = (SPRT *)polyTag;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(activeDrawEnv.tpage), poly->clut);
		s_gpu.vertexIndex += NativeGpu_EmitTexturedSprite(&poly->x0, &poly->u0, activeDrawEnv.tpage, poly->clut, poly->w, poly->h, shadeTexOn, &poly->r0);

		return 4;
	}
	case 0x68:
	{
		TILE_1 *poly = (TILE_1 *)polyTag;

		AddSplit(semiTrans, false, false, 0);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 1, 1);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 2;
	}
	case 0x70:
	{
		TILE_8 *poly = (TILE_8 *)polyTag;

		AddSplit(semiTrans, false, false, 0);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 2;
	}
	case 0x74:
	{
		SPRT_8 *poly = (SPRT_8 *)polyTag;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(activeDrawEnv.tpage), poly->clut);
		s_gpu.vertexIndex += NativeGpu_EmitTexturedSprite(&poly->x0, &poly->u0, activeDrawEnv.tpage, poly->clut, 8, 8, shadeTexOn, &poly->r0);

		return 3;
	}
	case 0x78:
	{
		TILE_16 *poly = (TILE_16 *)polyTag;

		AddSplit(semiTrans, false, false, 0);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 2;
	}
	case 0x7C:
	{
		SPRT_16 *poly = (SPRT_16 *)polyTag;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(activeDrawEnv.tpage), poly->clut);
		s_gpu.vertexIndex += NativeGpu_EmitTexturedSprite(&poly->x0, &poly->u0, activeDrawEnv.tpage, poly->clut, 16, 16, shadeTexOn, &poly->r0);

		return 3;
	}
	}
	return 0;
}

internal void VitaCtrNativeGpu_UpdateDrawTargetMode(void)
{
	const int displayW = activeDispEnv.disp.w;
	const int displayH = activeDispEnv.disp.h;
	const int drawW = activeDrawEnv.clip.w;
	const int drawH = activeDrawEnv.clip.h;

	/* Generic GP0 has no PsyQ DRAWENV.dfe bit. Treat framebuffer-sized draw areas as the high-resolution onscreen target; smaller areas remain VRAM offscreen targets for effects and render-to-texture paths. */
	activeDrawEnv.dfe = displayW > 0 && displayH > 0 && drawW > 0 && drawH > 0 &&
	                    drawW * 4 >= displayW * 3 && drawH * 4 >= displayH * 3;
}
internal int ProcessDrawEnv(P_TAG *polyTag)
{
	const u32 *codePtr = (u32 *)&polyTag->pad0;
	int processedLongs = 0;
	bool fullDrawEnvPacket = false;
	for (int i = 0; i < polyTag->len; ++i)
	{
		const u32 code = codePtr[i];
		const int primType = code >> 24 & 0xF0;
		const int primSubType = code >> 24 & 0x0F;

		// NOTE(aalhendi): CTR can pack draw-env commands, tagless geometry, and more draw-env commands into one OT entry. Stop at the first non-E command so ParseTaglessPrimitive owns the geometry payload.
		if (primType != 0xE0)
		{
			return processedLongs;
		}

		switch (primSubType)
		{
		case 0x1:
		{
			// DR_TPAGE
			activeDrawEnv.tpage = (code & 0x1FFF);
			activeDrawEnv.dtd = (code >> 9) & 1;
			// NOTE(aalhendi): Standalone DR_TPAGE packets use the same E1 word for blend changes; only full DRAWENV packets retarget native on-screen/offscreen rendering.
			if (fullDrawEnvPacket)
			{
				activeDrawEnv.dfe = (code >> 10) & 1;
			}
			break;
		}
		case 0x2:
		{
			// DR_TWIN
			activeDrawEnv.tw.w = (code & 0x1F);
			activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
			activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
			activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
			break;
		}
		case 0x3:
		{
			// DR_AREA
			activeDrawEnv.clip.x = code & 1023;
			activeDrawEnv.clip.y = (code >> 10) & (VRAM_HEIGHT - 1);
			fullDrawEnvPacket = true;
			break;
		}
		case 0x4:
		{
			// DR_AREA (second part)
			activeDrawEnv.clip.w = code & 1023;
			activeDrawEnv.clip.h = (code >> 10) & (VRAM_HEIGHT - 1);

			activeDrawEnv.clip.w = activeDrawEnv.clip.w - activeDrawEnv.clip.x + 1;
			activeDrawEnv.clip.h = activeDrawEnv.clip.h - activeDrawEnv.clip.y + 1;
			VitaCtrNativeGpu_UpdateDrawTargetMode();
			fullDrawEnvPacket = true;
			break;
		}
		case 0x5:
		{
			// DR_OFFSET
			activeDrawEnv.ofs[0] = NativeGpu_SignExtend11(code);
			activeDrawEnv.ofs[1] = NativeGpu_SignExtend11(code >> 11);
			fullDrawEnvPacket = true;
			break;
		}
		case 0x6:
		{
			SetPSXMaskState(code);
			break;
		}
		case 0:
			// NOTE(aalhendi): ctr-native local divergence for CTR OTs. A zero word can be terminal draw-env padding, or the tag word for the next primitive packed into the same OT entry. return processedLongs;
			if (i + 1 != polyTag->len)
			{
				return processedLongs;
			}
			break;
		}
		++processedLongs;
	}

	return processedLongs;
}

internal void ProcessDrawEnvCommand(u32 code)
{
	const int primSubType = code >> 24 & 0x0F;

	switch (primSubType)
	{
	case 0x1:
		activeDrawEnv.tpage = (code & 0x1FFF);
		activeDrawEnv.dtd = (code >> 9) & 1;
		break;
	case 0x2:
		activeDrawEnv.tw.w = (code & 0x1F);
		activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
		activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
		activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
		break;
	case 0x3:
		activeDrawEnv.clip.x = code & 1023;
		activeDrawEnv.clip.y = (code >> 10) & (VRAM_HEIGHT - 1);
		break;
	case 0x4:
		activeDrawEnv.clip.w = code & 1023;
		activeDrawEnv.clip.h = (code >> 10) & (VRAM_HEIGHT - 1);
		activeDrawEnv.clip.w = activeDrawEnv.clip.w - activeDrawEnv.clip.x + 1;
		activeDrawEnv.clip.h = activeDrawEnv.clip.h - activeDrawEnv.clip.y + 1;
			VitaCtrNativeGpu_UpdateDrawTargetMode();
		break;
	case 0x5:
		activeDrawEnv.ofs[0] = NativeGpu_SignExtend11(code);
		activeDrawEnv.ofs[1] = NativeGpu_SignExtend11(code >> 11);
		break;
	case 0x6:
		SetPSXMaskState(code);
		break;
	}
}

internal int ProcessPsyXPrims(P_TAG *polyTag)
{
	const int primSubType = polyTag->code & 0x0F;

	switch (primSubType)
	{
	case 0x01:
	{
		DR_PSYX_TEX *psytex = (DR_PSYX_TEX *)polyTag;
		s_gpu.overrideTexture = psytex->code[0] & 0xFFFFFF;
		s_gpu.overrideTextureWidth = psytex->code[1] & 0xFFF;
		s_gpu.overrideTextureHeight = psytex->code[1] >> 16 & 0xFFF;
		/* PsyCross currently emits texture ID and dimensions only. Its private
		 * STP extension has no producer in this tree, so do not guess a reserved bit. */
		s_gpu.overrideTexturePsxStp = false;
		return 2;
	}
	case 0x02:
	{
		// [A] Psy-X custom debug marker packet
		DR_PSYX_DBGMARKER *psydbg = (DR_PSYX_DBGMARKER *)polyTag;
		s_gpu.currentSplitDebugText = psydbg->text;
		return 2;
	}
	}

	return 0;
}

// Processes primitive returns processed primitive primLength in longs
int ParsePrimitive(P_TAG *polyTag)
{
	const int primType = polyTag->code & 0xF0;

	int primLength = 0;
	bool handledZeroLength = false;

	switch (primType)
	{
	case 0x00:
	{
		const int primSubType = polyTag->code & 0x0F;
		const u32 *codePtr = (u32 *)&polyTag->pad0;
		// NOTE(aalhendi): ctr-native local divergence. CTR RenderWeather can emit a retail length-2 zero packet when weather is enabled but the level has no fill-mode payload. The PSX consumes it by tag length; the native parser must advance past it too.
		if (polyTag->len == 2 && codePtr[0] == 0 && codePtr[1] == 0)
		{
			primLength = 2;
		}
		else if (polyTag->len == 0 && *(u32 *)polyTag == 0)
		{
			// CTR ghost transparency packets include raw GPU NOP words between draw-mode changes and triangle commands. They consume exactly one command word; ParsePrimitivesLinkedList adds P_LEN to the return.
			handledZeroLength = true;
		}
		else if (primSubType == 0x0)
		{
			primLength = 3;
		}
		else if (primSubType == 0x1)
		{
			DR_MOVE *drmove = (DR_MOVE *)polyTag;
			const u32 rectPos = drmove->code[2];
			const u32 rectSize = drmove->code[4];

			const int y = drmove->code[3] >> 0x10 & 0xFFFF;
			const int x = drmove->code[3] & 0xFFFF;

			RECT16 rect;
			rect.x = (s16)(rectPos & 0xffff);
			rect.y = (s16)(rectPos >> 16);
			rect.w = (s16)(rectSize & 0xffff);
			rect.h = (s16)(rectSize >> 16);

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
			NativeGpu_ForceSynchronousFrame();
			NativeGpu_FlushFrontendSplitsSync();
#else
			if (NativeGpu_HasPendingSplits())
			{
				DrawAllSplits();
			}
#endif
			MoveImage(&rect, x, y);
			primLength = 5;
		}
		else if (primSubType == 0x2)
		{
			// NOTE(aalhendi): ctr-native local divergence. CTR emits retail FILL packets in OTs; the old PsyCross parser did not consume them, which caused zero-length primitive spam.
			TILE *fill = (TILE *)polyTag;
			RECT16 rect;

			rect.x = (u16)fill->x0 & 0x3f0;
			rect.y = (u16)fill->y0 & (VRAM_HEIGHT - 1);
			rect.w = (((u16)fill->w & 0x3ff) + 15) & ~15;
			rect.h = (u16)fill->h & (VRAM_HEIGHT - 1);

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
                if (!NativeGpu_AppendClearSplit(&rect, fill->r0, fill->g0, fill->b0))
                {
                    NativeGpu_GuestVRAMWritten(rect.x, rect.y, rect.w, rect.h);
                    NativeGpu_ForceSynchronousFrame();
                    NativeGpu_FlushFrontendSplitsSync();
#ifdef __vita__
                    NativeGpuBackendRectTask task = {rect.x, rect.y, rect.w, rect.h, fill->r0, fill->g0, fill->b0};
                    NativeGpu_RunBackendTaskSync(NativeGpu_BackendClearTask, &task);
#else
					NativeRenderer_ClearVRAM(rect.x, rect.y, rect.w, rect.h, fill->r0, fill->g0, fill->b0);
					NativeRenderer_UpdateVRAM();
#endif
                }
#else
                const bool fillTargetsFrameBuffer =
                    activeDrawEnv.dfe && rect.x >= activeDrawEnv.clip.x && rect.y >= activeDrawEnv.clip.y &&
                    rect.x + rect.w <= activeDrawEnv.clip.x + activeDrawEnv.clip.w && rect.y + rect.h <= activeDrawEnv.clip.y + activeDrawEnv.clip.h;

                if (fillTargetsFrameBuffer)
                {
                    if (NativeGpu_HasPendingSplits())
                    {
                        DrawAllSplits();
                    }
                    NativeRenderer_Clear(rect.x, rect.y, rect.w, rect.h, fill->r0, fill->g0, fill->b0);
                }
                else
                {
                    if (NativeGpu_HasPendingSplits())
                    {
                        DrawAllSplits();
                    }
                    ClearImage(&rect, fill->r0, fill->g0, fill->b0);
                }
#endif
			primLength = 3;
		}
		break;
	}
	case 0x20:
		// Flat polygons
		primLength = ProcessFlatPoly(polyTag);
		break;
	case 0x30:
		// Gouraud shaded polygons
		primLength = ProcessGouraudPoly(polyTag);
		break;
	case 0x40:
		// Flat (single colour) Lines
		primLength = ProcessFlatLines(polyTag);
		break;
	case 0x50:
		// Gouraud lines
		primLength = ProcessGouraudLines(polyTag);
		break;
	case 0x60:
	case 0x70:
		// TILE and SPRT
		primLength = ProcessTileAndSprt(polyTag);
		break;
	case 0xA0:
		// DR_LOAD
		{
			DR_LOAD *drload = (DR_LOAD *)polyTag;
			const u32 rectPos = drload->code[1];
			const u32 rectSize = drload->code[2];

			RECT16 rect;
			rect.x = (s16)(rectPos & 0xffff);
			rect.y = (s16)(rectPos >> 16);
			rect.w = (s16)(rectSize & 0xffff);
			rect.h = (s16)(rectSize >> 16);

#if defined(__vita__) || defined(PCSX_NATIVE_VITA_PATH)
			if (NativeGpu_HasPendingSplits())
			{
				NativeGpu_ForceSynchronousFrame();
				NativeGpu_FlushFrontendSplitsSync();
			}
#endif
			LoadImage(&rect, (uint32_t *)drload->p);

			// TODO(aalhendi): Audit whether CTR ever appends additional GPU commands after a DR_LOAD payload in the same packet.
		}
		primLength = getlen(polyTag);
		break;
	case 0xB0:
		// [A] Psy-X custom primitives
		primLength = ProcessPsyXPrims(polyTag);
		break;
	case 0xE0:
		// Draw Env setup
		primLength = ProcessDrawEnv(polyTag);
		break;
		// default: NATIVE_GPU_ERROR("got %0x primitive\n", primType);
	}

	if (primLength == 0 && !handledZeroLength)
	{
		NATIVE_GPU_ERROR("Unhandled zero length %0x primitive\n", primType);
	}

	return primLength;
}

int VitaCtrNativeGpu_RenderHadGeometry(void)
{
	return s_pcsxRenderHadGeometry;
}

void VitaCtrNativeGpu_SetDisplayEnv(int x, int y, int w, int h, int depth24)
{
	activeDispEnv.disp.x = (s16)x;
	activeDispEnv.disp.y = (s16)y;
	activeDispEnv.disp.w = (s16)w;
	activeDispEnv.disp.h = (s16)h;
	activeDispEnv.isrgb24 = depth24 != 0;
	activeDispEnv.screen.x = 0;
	activeDispEnv.screen.y = 0;
	activeDispEnv.screen.w = (s16)w;
	activeDispEnv.screen.h = (s16)h;
	VitaCtrNativeGpu_UpdateDrawTargetMode();
	activeDrawEnv.isbg = 0;
	NativeGpu_SetFrontendDrawEnv(&activeDrawEnv);
	NativeGpu_SetFrontendDispEnv(&activeDispEnv);
}

int ParseTaglessPrimitive(u32 *command)
{
	const u32 code = *command;
	const int primType = (code >> 24) & 0xF0;

	if (code == 0)
	{
		return 1;
	}

	if (primType == 0xE0)
	{
		ProcessDrawEnvCommand(code);
		return 1;
	}

	P_TAG *polyTag = (P_TAG *)(command - P_LEN);
	int primLength = ParsePrimitive(polyTag);

	if (primLength == 0)
	{
		NATIVE_GPU_ERROR("Unhandled tagless primitive %08x\n", code);
		return 1;
	}

	return primLength;
}
