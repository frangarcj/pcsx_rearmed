#include "gl_present.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include "app_root.h"
#include "shelf_common.h"

#define DISPLAY_W 960
#define DISPLAY_H 544
#define GAME_W 725
#define TEX_W 1024
#define TEX_H 512
/* Three textures: a frame writes the one of three frames before, whose
 * drawing the two pending swaps have finished, as the vita2d path does. */
#define TEX_COUNT 3

extern int (*pcsx_raw_frame_cb)(const void *vram, int ofs, int stride, unsigned mask, int w, int h);

static GLuint s_tex[TEX_COUNT], s_program, s_vbo;
static uint8_t *s_data[TEX_COUNT]; /* the textures' memory, written directly */
static GLint s_src_loc = -1, s_texsize_loc = -1, s_scale_loc = -1;
static int s_next, s_ready = -1, s_last = -1, s_w, s_h, s_aspect;
/* Main-thread time in the present (copy, draw and swap). */
unsigned long long glp_busy_us;

/* The textures hold the PSX's 15-bit pixels as they are (GXM reads that
 * format, as the vita2d path did). Sharp bilinear: whole texels by nearest,
 * the fraction of a screen pixel at each texel edge blended by the
 * texture unit. */
static const char *k_vertex =
    "attribute vec2 a_pos;\n"
    "uniform vec2 u_src;\n"
    "varying vec2 v_texel;\n"
    "void main() {\n"
    "  v_texel = vec2(a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5) * u_src;\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";
static const char *k_fragment =
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_src;\n"
    "uniform vec2 u_texsize;\n"
    "uniform vec2 u_scale;\n"
    "varying vec2 v_texel;\n"
    "void main() {\n"
    "  vec2 i = floor(v_texel);\n"
    "  vec2 d = v_texel - i - 0.5;\n"
    "  vec2 range = 0.5 - 0.5 / u_scale;\n"
    "  vec2 f = (d - clamp(d, -range, range)) * u_scale + 0.5;\n"
    "  vec2 t = clamp(i + f, vec2(0.5), u_src - 0.5);\n"
    "  gl_FragColor = vec4(texture2D(u_tex, t / u_texsize).rgb, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint ok = GL_FALSE;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        sceClibPrintf("pcsx-vita: present shader failed\n");
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* Where the picture goes: the 4:3 middle, or the whole screen for 16:9. */
static void game_rect(int rect[4])
{
    const int w = s_aspect ? DISPLAY_W : GAME_W;
    rect[0] = (DISPLAY_W - w) / 2;
    rect[1] = 0;
    rect[2] = w;
    rect[3] = DISPLAY_H;
}

static void frame_ready(int w, int h, SceGxmTextureFormat format)
{
    glBindTexture(GL_TEXTURE_2D, s_tex[s_next]);
    sceGxmTextureSetFormat(vglGetGxmTexture(GL_TEXTURE_2D), format);
    s_w = w;
    s_h = h;
    s_ready = s_next;
    s_next = (s_next + 1) % TEX_COUNT;
}

/* The displayed VRAM rows, as the core gives them (raw 15-bit). */
static int glp_raw_frame(const void *vram_, int ofs, int stride, unsigned mask, int w, int h)
{
    const uint8_t *vram = vram_;
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    uint8_t *dst = s_data[s_next];
    unsigned r, bytes;
    if (w <= 0 || h <= 0)
        return 0;
    if (w > TEX_W) w = TEX_W;
    if (h > TEX_H) h = TEX_H;
    bytes = (unsigned)w * 2;
    for (r = 0; r < (unsigned)h; r++) {
        const unsigned row = ((unsigned)ofs + r * (unsigned)stride) & mask, col = row & 2047u;
        uint8_t *out = dst + (size_t)r * TEX_W * 2;
        if (col + bytes <= 2048)
            memcpy(out, vram + row, bytes);
        else {
            memcpy(out, vram + row, 2048 - col);
            memcpy(out + 2048 - col, vram + (row - col), bytes - (2048 - col));
        }
    }
    frame_ready(w, h, SCE_GXM_TEXTURE_FORMAT_X1U5U5U5_1BGR);
    glp_busy_us += sceKernelGetProcessTimeWide() - start;
    return 1;
}

/* RGB565 from the core (24-bit modes, the games' movies). */
static void rgb565_frame(const void *data, unsigned width, unsigned height, size_t pitch)
{
    const unsigned w = width > TEX_W ? TEX_W : width, h = height > TEX_H ? TEX_H : height;
    for (unsigned y = 0; y < h; y++)
        memcpy(s_data[s_next] + (size_t)y * TEX_W * 2, (const uint8_t *)data + y * pitch, w * 2);
    frame_ready((int)w, (int)h, SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB);
}

static void draw(int texture)
{
    int rect[4];
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, DISPLAY_W, DISPLAY_H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (texture < 0)
        return;
    game_rect(rect);
    glViewport(rect[0], rect[1], rect[2], rect[3]);
    glUseProgram(s_program);
    glUniform2f(s_src_loc, (float)s_w, (float)s_h);
    glUniform2f(s_texsize_loc, (float)TEX_W, (float)TEX_H);
    glUniform2f(s_scale_loc, (float)rect[2] / s_w, (float)rect[3] / s_h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex[texture]);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

void glp_video(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (s_ready < 0 && data && width && height)
        rgb565_frame(data, width, height, pitch);
    if (s_ready < 0)
        return;
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    draw(s_ready);
    vglSwapBuffers(GL_FALSE);
    glp_busy_us += sceKernelGetProcessTimeWide() - start;
    s_last = s_ready;
    s_ready = -1;
}

/* ---- the menus' presenter ---- */

static void run_here(void (*task)(void *), void *arg) { task(arg); }
static void present_last(void) { draw(s_last); }
static void swap(void) { vglSwapBuffers(GL_FALSE); }
static void set_aspect(int mode) { s_aspect = mode; }

static const VitaShelfPresenter k_presenter = { run_here, present_last, game_rect, swap, set_aspect };

int glp_init(void)
{
    static const float quad[8] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
    GLuint vs, fs;
    GLint linked = GL_FALSE;

    vglSetShaderCachePath(APP_ROOT "/shader_cache");
    vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, GL_TRUE, GL_TRUE, GL_TRUE);
    /* vitaGL takes the free memory but this much: the NEON renderer and the
     * game's RAM live in the newlib heap, set before it. */
    vglInitExtended(0, DISPLAY_W, DISPLAY_H, 32 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    /* The emulator paces the frames (pace_frame): the swap must not wait
     * for a vblank too, as vita2d's did not (gxm_init). */
    vglWaitVblankStart(GL_FALSE);

    glGenTextures(TEX_COUNT, s_tex);
    for (int i = 0; i < TEX_COUNT; i++) {
        s_data[i] = vglAlloc(TEX_W * TEX_H * 2, VGL_MEM_RAM);
        if (!s_data[i])
            return -1;
        memset(s_data[i], 0, TEX_W * TEX_H * 2);
        glBindTexture(GL_TEXTURE_2D, s_tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        vglSetupTexture(GL_TEXTURE_2D, s_data[i], SCE_GXM_TEXTURE_FORMAT_X1U5U5U5_1BGR,
            SCE_GXM_TEXTURE_LINEAR, TEX_W, TEX_H, 1);
    }
    vs = compile(GL_VERTEX_SHADER, k_vertex);
    fs = compile(GL_FRAGMENT_SHADER, k_fragment);
    if (!vs || !fs)
        return -1;
    s_program = glCreateProgram();
    glAttachShader(s_program, vs);
    glAttachShader(s_program, fs);
    glBindAttribLocation(s_program, 0, "a_pos");
    glLinkProgram(s_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(s_program, GL_LINK_STATUS, &linked);
    if (!linked)
        return -1;
    s_src_loc = glGetUniformLocation(s_program, "u_src");
    s_texsize_loc = glGetUniformLocation(s_program, "u_texsize");
    s_scale_loc = glGetUniformLocation(s_program, "u_scale");
    glUseProgram(s_program);
    glUniform1i(glGetUniformLocation(s_program, "u_tex"), 0);
    glUseProgram(0);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    pcsx_raw_frame_cb = glp_raw_frame;
    VitaShelf_SetPresenter(&k_presenter);
    return 0;
}

void glp_finish(void)
{
    pcsx_raw_frame_cb = NULL;
    glFinish();
    glDeleteTextures(TEX_COUNT, s_tex);
    glDeleteBuffers(1, &s_vbo);
    glDeleteProgram(s_program);
    /* The textures' memory went with them. */
    memset(s_data, 0, sizeof(s_data));
    s_ready = s_last = -1;
    vglShutdown();
}
