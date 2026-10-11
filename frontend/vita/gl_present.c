#include "gl_present.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/clib.h>
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
static GLint s_src_loc = -1, s_texsize_loc = -1, s_scale_loc = -1;
static uint8_t *s_staging;
static int s_next, s_ready = -1, s_last = -1, s_w, s_h, s_aspect;

/* The PSX's 15-bit pixels arrive as their two bytes (GL_RG8, red = low
 * byte); the shader decodes them and scales sharply: whole texels by
 * nearest, the fraction of a screen pixel at each texel edge blended. */
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
    "vec3 psx(vec2 texel) {\n"
    "  texel = min(texel, u_src - 1.0);\n"
    "  vec2 rg = floor(texture2D(u_tex, (texel + 0.5) / u_texsize).rg * 255.0 + 0.5);\n"
    "  float v = rg.r + rg.g * 256.0;\n"
    "  return vec3(mod(v, 32.0), mod(floor(v / 32.0), 32.0), mod(floor(v / 1024.0), 32.0)) / 31.0;\n"
    "}\n"
    "void main() {\n"
    "  vec2 p = v_texel - 0.5;\n"
    "  vec2 i = floor(p);\n"
    "  vec2 f = clamp((p - i - 0.5) * u_scale + 0.5, 0.0, 1.0);\n"
    "  vec3 top = mix(psx(i), psx(i + vec2(1.0, 0.0)), f.x);\n"
    "  vec3 bottom = mix(psx(i + vec2(0.0, 1.0)), psx(i + vec2(1.0, 1.0)), f.x);\n"
    "  gl_FragColor = vec4(mix(top, bottom, f.y), 1.0);\n"
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

static void upload(int w, int h)
{
    glBindTexture(GL_TEXTURE_2D, s_tex[s_next]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RG, GL_UNSIGNED_BYTE, s_staging);
    s_w = w;
    s_h = h;
    s_ready = s_next;
    s_next = (s_next + 1) % TEX_COUNT;
}

/* The displayed VRAM rows, as the core gives them (raw 15-bit). */
static int glp_raw_frame(const void *vram_, int ofs, int stride, unsigned mask, int w, int h)
{
    const uint8_t *vram = vram_;
    unsigned r, bytes;
    if (w <= 0 || h <= 0)
        return 0;
    if (w > TEX_W) w = TEX_W;
    if (h > TEX_H) h = TEX_H;
    bytes = (unsigned)w * 2;
    for (r = 0; r < (unsigned)h; r++) {
        const unsigned row = ((unsigned)ofs + r * (unsigned)stride) & mask, col = row & 2047u;
        uint8_t *out = s_staging + (size_t)r * bytes;
        if (col + bytes <= 2048)
            memcpy(out, vram + row, bytes);
        else {
            memcpy(out, vram + row, 2048 - col);
            memcpy(out + 2048 - col, vram + (row - col), bytes - (2048 - col));
        }
    }
    upload(w, h);
    return 1;
}

/* RGB565 from the core (24-bit modes): back to the PSX's 15 bits. */
static void rgb565_frame(const void *data, unsigned width, unsigned height, size_t pitch)
{
    const unsigned w = width > TEX_W ? TEX_W : width, h = height > TEX_H ? TEX_H : height;
    for (unsigned y = 0; y < h; y++) {
        const uint16_t *in = (const uint16_t *)((const uint8_t *)data + y * pitch);
        uint16_t *out = (uint16_t *)(s_staging + (size_t)y * w * 2);
        for (unsigned x = 0; x < w; x++) {
            const uint16_t p = in[x];
            out[x] = (uint16_t)(((p >> 11) & 31) | (((p >> 6) & 31) << 5) | ((p & 31) << 10));
        }
    }
    upload((int)w, (int)h);
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
    draw(s_ready);
    vglSwapBuffers(GL_FALSE);
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

    s_staging = malloc((size_t)TEX_W * TEX_H * 2);
    if (!s_staging)
        return -1;
    glGenTextures(TEX_COUNT, s_tex);
    for (int i = 0; i < TEX_COUNT; i++) {
        glBindTexture(GL_TEXTURE_2D, s_tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, TEX_W, TEX_H, 0, GL_RG, GL_UNSIGNED_BYTE, NULL);
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
    free(s_staging);
    s_staging = NULL;
    s_ready = s_last = -1;
    vglShutdown();
}
