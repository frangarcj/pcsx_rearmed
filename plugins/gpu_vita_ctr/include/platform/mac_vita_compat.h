#ifndef PCSX_GPU_MAC_VITA_COMPAT_H
#define PCSX_GPU_MAC_VITA_COMPAT_H

#include <stddef.h>
#include <SDL3/SDL.h>
#include <OpenGL/gl3.h>

#ifndef GL_PALETTE4_RGBA8_OES
#define GL_PALETTE4_RGBA8_OES 0x8B90
#endif

void *vglMemalign(size_t alignment, size_t size);
void *vglAllocFromScratch(size_t size);
void vglFree(void *ptr);
void vglBufferData(GLenum target, const void *data);
int vglReadPixelsChecked(GLint x, GLint y, GLsizei width, GLsizei height,
                         GLenum format, GLenum type, void *pixels);
void vglReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                   GLenum format, GLenum type, void *pixels);
void vglSwapBuffers(GLboolean wait_vblank);
int vglShutdown(void);

int PCSXMacVitaGL_IsCurrent(void);
int PCSXMacVitaGL_CaptureCurrentFrame(void);
int PCSXMacVitaGL_CapturePresentedFrame(void);
int PCSXMacVitaGL_WriteCapturePpm(const char *path);
void PCSXMacVitaGL_UploadP4Texture(GLsizei width, GLsizei height,
                                   const void *vita_palette4_rgba8_payload);


/* PCSX_GL_STATS=1 (harness only) counts, per presented frame, what costs a
 * GXM scene on the Vita: a draw or clear into another framebuffer than the
 * previous one ends the scene. Printed at exit. */
void PCSXGLStats_BindFramebuffer(GLenum target, GLuint framebuffer);
void PCSXGLStats_Draw(GLsizei vertices);
void PCSXGLStats_Clear(void);
void PCSXGLStats_Upload(GLsizei width, GLsizei height);
#ifndef PCSX_MAC_VITA_COMPAT_IMPL
#define glBindFramebuffer(t, f) PCSXGLStats_BindFramebuffer((t), (f))
#define glDrawArrays(m, first, n) (PCSXGLStats_Draw(n), glDrawArrays((m), (first), (n)))
#define glClear(mask) (PCSXGLStats_Clear(), glClear(mask))
#define glTexImage2D(t, l, i, w, h, b, f, ty, p) (PCSXGLStats_Upload((p) ? (w) : 0, (p) ? (h) : 0), glTexImage2D((t), (l), (i), (w), (h), (b), (f), (ty), (p)))
#define glTexSubImage2D(t, l, x, y, w, h, f, ty, p) (PCSXGLStats_Upload((w), (h)), glTexSubImage2D((t), (l), (x), (y), (w), (h), (f), (ty), (p)))
#endif

#endif
