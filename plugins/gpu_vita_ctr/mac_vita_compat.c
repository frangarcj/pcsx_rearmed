#define PCSX_MAC_VITA_COMPAT_IMPL
#include "platform/mac_vita_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern SDL_Window *g_window;

typedef struct ScratchAllocation {
    void *pointer;
    size_t size;
} ScratchAllocation;

static ScratchAllocation s_scratch[4];
static unsigned char *s_capture_rgba;
static int s_capture_width;
static int s_capture_height;
static GLenum s_capture_read_buffer = GL_BACK;

void *vglMemalign(size_t alignment, size_t size)
{
    void *pointer = NULL;
    if (posix_memalign(&pointer, alignment, size) != 0)
        return NULL;
    return pointer;
}

void vglFree(void *pointer)
{
    free(pointer);
}

void *vglAllocFromScratch(size_t size)
{
    void *pointer = malloc(size);
    unsigned i;
    if (pointer == NULL)
        return NULL;
    for (i = 0; i < sizeof(s_scratch) / sizeof(s_scratch[0]); i++) {
        if (s_scratch[i].pointer == NULL) {
            s_scratch[i].pointer = pointer;
            s_scratch[i].size = size;
            return pointer;
        }
    }
    free(pointer);
    return NULL;
}

void vglBufferData(GLenum target, const void *data)
{
	size_t size = 0;
	unsigned i;
	void *allocation = NULL;
	for (i = 0; i < sizeof(s_scratch) / sizeof(s_scratch[0]); i++) {
		if (s_scratch[i].pointer == data && data != NULL) {
			size = s_scratch[i].size;
			allocation = s_scratch[i].pointer;
			s_scratch[i].pointer = NULL;
			s_scratch[i].size = 0;
			break;
		}
	}
	glBufferData(target, (GLsizeiptr)size, data, GL_STREAM_DRAW);
	free(allocation);
}

int vglReadPixelsChecked(GLint x, GLint y, GLsizei width, GLsizei height,
                         GLenum format, GLenum type, void *pixels)
{
    glReadPixels(x, y, width, height, format, type, pixels);
    return glGetError() == GL_NO_ERROR;
}

void vglReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                   GLenum format, GLenum type, void *pixels)
{
    glReadPixels(x, y, width, height, format, type, pixels);
}

static struct {
    GLuint draw_fbo, scene_fbo;
    void *bind_site; /* caller of the last framebuffer bind (scene trace) */
    int scene_open;
    unsigned long long frames, scenes, draws, vertices, clears, uploads, upload_texels;
} s_stats;

static unsigned long long s_stats_skip, s_stats_seen;

static void stats_report(void)
{
    if (!s_stats.frames)
        return;
    const double f = (double)s_stats.frames;
    fprintf(stderr, "gl-stats frames=%llu scenes/frame=%.2f draws/frame=%.1f verts/frame=%.0f clears/frame=%.2f uploads/frame=%.2f upload_texels/frame=%.0f\n",
            s_stats.frames, s_stats.scenes / f, s_stats.draws / f, s_stats.vertices / f,
            s_stats.clears / f, s_stats.uploads / f, s_stats.upload_texels / f);
}

static int stats_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("PCSX_GL_STATS");
        enabled = v && *v == '1';
        if (enabled) {
            const char *skip = getenv("PCSX_GL_STATS_SKIP");
            s_stats_skip = skip ? strtoull(skip, NULL, 10) : 0;
            atexit(stats_report);
        }
    }
    return enabled;
}

/* PCSX_GL_SCENE_TRACE=N: print each scene of the first N counted frames
 * (fbo and the code address that bound it; symbolize with atos). */
static unsigned long long scene_trace_frames(void)
{
    static long long n = -1;
    if (n < 0) {
        const char *v = getenv("PCSX_GL_SCENE_TRACE");
        n = v ? strtoll(v, NULL, 10) : 0;
        if (n)
            fprintf(stderr, "gl-scene-trace base=%p\n", (void *)&PCSXGLStats_Draw);
    }
    return (unsigned long long)n;
}

static void stats_touch(void)
{
    if (!s_stats.scene_open || s_stats.scene_fbo != s_stats.draw_fbo) {
        if (s_stats_seen > s_stats_skip && s_stats.frames < scene_trace_frames())
            fprintf(stderr, "gl-scene frame=%llu scene=%llu fbo=%u site=%p\n", s_stats.frames,
                    s_stats.scenes, s_stats.draw_fbo, s_stats.bind_site);
        s_stats.scenes++;
        s_stats.scene_open = 1;
        s_stats.scene_fbo = s_stats.draw_fbo;
    }
}

void PCSXGLStats_BindFramebuffer(GLenum target, GLuint framebuffer)
{
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
        s_stats.draw_fbo = framebuffer;
        s_stats.bind_site = __builtin_return_address(0);
    }
    glBindFramebuffer(target, framebuffer);
}

void PCSXGLStats_Draw(GLsizei vertices)
{
    if (!stats_enabled())
        return;
    stats_touch();
    s_stats.draws++;
    s_stats.vertices += (unsigned long long)vertices;
}

void PCSXGLStats_Clear(void)
{
    if (!stats_enabled())
        return;
    stats_touch();
    s_stats.clears++;
}

void PCSXGLStats_Upload(GLsizei width, GLsizei height)
{
    if (!stats_enabled() || width <= 0 || height <= 0)
        return;
    s_stats.uploads++;
    s_stats.upload_texels += (unsigned long long)width * (unsigned long long)height;
}

void vglSwapBuffers(GLboolean wait_vblank)
{
    (void)wait_vblank;
    if (stats_enabled()) {
        /* PCSX_GL_STATS_SKIP=N: count from the N+1th presented frame. */
        if (++s_stats_seen <= s_stats_skip)
            memset(&s_stats, 0, sizeof(s_stats));
        else
            s_stats.frames++;
        s_stats.scene_open = 0;
    }
    if (g_window != NULL)
        SDL_GL_SwapWindow(g_window);
}

int vglShutdown(void)
{
    unsigned i;
    for (i = 0; i < sizeof(s_scratch) / sizeof(s_scratch[0]); i++) {
        free(s_scratch[i].pointer);
        s_scratch[i].pointer = NULL;
        s_scratch[i].size = 0;
    }
    return 0;
}

int PCSXMacVitaGL_IsCurrent(void)
{
    return SDL_GL_GetCurrentContext() != NULL;
}

static int capture_framebuffer(GLenum read_buffer)
{
    GLint previous_read_framebuffer = 0;
    GLint previous_read_buffer = GL_BACK;
    GLint previous_pack_alignment = 4;
    int width = 0;
    int height = 0;
    size_t size;
    unsigned char *capture;

    if (g_window == NULL || SDL_GL_GetCurrentContext() == NULL)
        return 0;
    SDL_GetWindowSizeInPixels(g_window, &width, &height);
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return 0;
    size = (size_t)width * (size_t)height * 4u;
    capture = (unsigned char *)realloc(s_capture_rgba, size);
    if (capture == NULL)
        return 0;
    s_capture_rgba = capture;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_framebuffer);
    glGetIntegerv(GL_READ_BUFFER, &previous_read_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(read_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glFinish();
    while (glGetError() != GL_NO_ERROR) {
    }
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, s_capture_rgba);
    if (glGetError() != GL_NO_ERROR) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read_framebuffer);
        glReadBuffer((GLenum)previous_read_buffer);
        glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
        return 0;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read_framebuffer);
    glReadBuffer((GLenum)previous_read_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
    s_capture_width = width;
    s_capture_height = height;
    s_capture_read_buffer = read_buffer;
    return 1;
}

int PCSXMacVitaGL_CaptureCurrentFrame(void)
{
    return capture_framebuffer(GL_BACK);
}

int PCSXMacVitaGL_CapturePresentedFrame(void)
{
    return capture_framebuffer(GL_FRONT);
}

const unsigned char *PCSXMacVitaGL_CapturedPixels(int *width,int *height)
{if(width)*width=s_capture_width;if(height)*height=s_capture_height;return s_capture_rgba;}

int PCSXMacVitaGL_WriteCapturePpm(const char *path)
{
    FILE *file;
    unsigned char *row;
    int y;
    if (s_capture_rgba == NULL || s_capture_width <= 0 || s_capture_height <= 0)
        return 0;
    file = fopen(path, "wb");
    if (file == NULL)
        return -1;
    row = (unsigned char *)malloc((size_t)s_capture_width * 3u);
    if (row == NULL) {
        fclose(file);
        return -1;
    }
    if (fprintf(file, "P6\n%d %d\n255\n", s_capture_width, s_capture_height) < 0) {
        free(row);
        fclose(file);
        return -1;
    }
    for (y = s_capture_height - 1; y >= 0; y--) {
        const unsigned char *src = s_capture_rgba + (size_t)y * s_capture_width * 4u;
        int x;
        for (x = 0; x < s_capture_width; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        if (fwrite(row, 1, (size_t)s_capture_width * 3u, file) != (size_t)s_capture_width * 3u) {
            free(row);
            fclose(file);
            return -1;
        }
    }
    free(row);
    if (fclose(file))
        return -1;
    fprintf(stderr, "capture=%dx%d source=%s\n", s_capture_width, s_capture_height,
        s_capture_read_buffer == GL_FRONT ? "OpenGL-front-buffer-last-presentation" : "OpenGL-back-buffer-before-swap");
    return 1;
}

void PCSXMacVitaGL_UploadP4Texture(GLsizei width, GLsizei height,
                                   const void *vita_palette4_rgba8_payload)
{
    const size_t pixels = (size_t)width * (size_t)height;
    const unsigned char *payload = (const unsigned char *)vita_palette4_rgba8_payload;
    unsigned char *rgba = (unsigned char *)malloc(pixels * 4u);
    const unsigned char *palette;
    const unsigned char *indices;
    size_t i;
    if (rgba == NULL || payload == NULL) {
        free(rgba);
        return;
    }

    palette = payload;
    indices = payload + 16u * 4u;
    for (i = 0; i < pixels; i++) {
        const unsigned char packed = indices[i >> 1];
        const unsigned char palette_index = (i & 1u) ? (packed & 0x0fu) : (packed >> 4);
        rgba[i * 4u + 0] = palette[palette_index * 4u + 0];
        rgba[i * 4u + 1] = palette[palette_index * 4u + 1];
        rgba[i * 4u + 2] = palette[palette_index * 4u + 2];
        rgba[i * 4u + 3] = palette[palette_index * 4u + 3];
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
}
