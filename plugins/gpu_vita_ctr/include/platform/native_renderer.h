#ifndef NATIVE_RENDERER_H
#define NATIVE_RENDERER_H

#include <platform/native_renderer_types.h>

int NativeRenderer_InitialiseRender(char *windowName, int width, int height, int fullscreen);
int NativeRenderer_InitialisePSX(void);
void NativeRenderer_Shutdown(void);
void NativeRenderer_ResetDevice(void);
void NativeRenderer_BeginScene(void);
void NativeRenderer_EndScene(void);
void NativeRenderer_EndGpuFrame(void);
void NativeRenderer_FinishGpuMeasurements(void);
void NativeRenderer_UpdateSwapIntervalState(int swapInterval);
void NativeRenderer_SwapWindow(void);
/* Vita commits the target's captured VRAM rect; these legacy arguments do
 * not retarget a live texture after GP0 draw-area commands change. */
void NativeRenderer_StoreFrameBuffer(int x, int y, int w, int h);
void NativeRenderer_PresentVRAMDisplay(void);
void NativeRenderer_PresentMainRenderTarget(void);
void NativeRenderer_PresentMainOverlay(int displayX, int displayY, int displayW, int displayH);
void NativeRenderer_CaptureBackdrop(void);
void NativeRenderer_BeginMenuRedraw(void);
int NativeRenderer_PresentBackdrop(void);
void NativeRenderer_InvalidateBackdrop(void);
void NativeRenderer_PresentShownOverlay(int displayX, int displayY, int displayW, int displayH);
void NativeRenderer_PresentVRAMRect(int x, int y, int w, int h);
void NativeRenderer_SaveVRAM(const char *outputFileName, int x, int y, int width, int height, int readFromFramebuffer);
void NativeRenderer_Clear(int x, int y, int w, int h, u8 r, u8 g, u8 b);
void NativeRenderer_ClearVRAM(int x, int y, int w, int h, u8 r, u8 g, u8 b);
int NativeRenderer_FillMainTarget(int x, int y, int w, int h, u8 r, u8 g, u8 b);
void NativeRenderer_QueueStaleP4Sources(int page, int clut);
void NativeRenderer_SyncStaleP4Sources(void);
int NativeRenderer_TargetDrawCovered(const RECT16 *area, int minX, int minY, int maxX, int maxY);
void NativeRenderer_NoteTargetDraw(const RECT16 *area, int minX, int minY, int maxX, int maxY);
void NativeRenderer_CopyVRAM(u16 *src, int x, int y, int w, int h, int dstX, int dstY);
void NativeRenderer_ReadVRAM(u16 *dst, int x, int y, int dstW, int dstH);
void NativeRenderer_UpdateVRAM(void);
int NativeRenderer_GetVRAMStateSize(void);
int NativeRenderer_CaptureVRAMState(void *dst, int dstSize);
int NativeRenderer_RestoreVRAMState(const void *src, int srcSize);
TextureID NativeRenderer_GetVRAMTexture(void);
TextureID NativeRenderer_GetWhiteTexture(void);
TextureID NativeRenderer_GetCachedP4Texture(int page, int clut, int superTurboTint);
int NativeRenderer_GetPaletteProperties(TexFormat format, int clut);
void NativeRenderer_SetBlendMode(BlendMode blendMode);
void NativeRenderer_SetMixedSTPBlendMode(BlendMode blendMode);
void NativeRenderer_SetDepthState(int enable, int write);
void NativeRenderer_SetStencilMode(int drawPrim);
void NativeRenderer_SetOffscreenState(const RECT16 *offscreenRect, int enable);
void NativeRenderer_SetProjection(const RECT16 *drawRect, const DISPENV *displayEnv, int offscreen);
void NativeRenderer_SetupClipMode(const RECT16 *clipRect, const DISPENV *displayEnv, int enable);
void NativeRenderer_SetTexture(TextureID texture, TexFormat texFormat, int semiTransPass,
                               BlendMode blendMode, int textured, int superTurboTint,
                               int textureFullyOpaque, int cachedP4, const RECT16 *textureWindow);
void NativeRenderer_SetOverrideTextureSize(int width, int height);
void NativeRenderer_SetPSXTextureSemiTransPass(int pass);
void NativeRenderer_SetPSXTextureOutputSTP(int enabled);
void NativeRenderer_SetPSXDrawMaskSet(int maskSet);
void NativeRenderer_UpdateVertexBuffer(const GrVertex *vertices, int count);
void NativeRenderer_DrawTriangles(int startVertex, int triangles);
void NativeRenderer_PushDebugLabel(const char *label);
void NativeRenderer_PopDebugLabel(void);
void NativeRenderer_SyncVRAMToCPU(int x, int y, int w, int h);
GrVertex *NativeRenderer_AllocateVertexBuffer(int count);
GrVertex *NativeRenderer_MapVertexBuffer(int count);
void NativeRenderer_CommitVertexBuffer(GrVertex *mapped);

/* GPU profiler labels (see pcsx_vita_gpu_label_cb in native_renderer.c). */
extern void (*pcsx_vita_gpu_label_cb)(int kind, const char *name, unsigned handle);
#define NATIVE_GPU_LABEL(kind, name, handle)                                  \
	do                                                                         \
	{                                                                          \
		if (pcsx_vita_gpu_label_cb)                                            \
			pcsx_vita_gpu_label_cb((kind), (name), (unsigned)(handle));        \
	} while (0)

#endif
