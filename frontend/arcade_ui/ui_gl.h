/* ImGui drawing and PNG textures for the arcade UI, on vitaGL or on the
 * OpenGL 2.1 context of the macOS build: only GLES2-style calls, no VAOs. */
#ifndef PCSX_ARCADE_UI_GL_H
#define PCSX_ARCADE_UI_GL_H

struct ImDrawData;
struct ImDrawList;

typedef struct {
	unsigned id;
	int width, height;
} UiTexture;

/* Builds the shader and the font atlas texture; ImGui must exist. */
bool ui_gl_init(void);
void ui_gl_shutdown(void);
/* Draws a frame of ImGui into the bound framebuffer (width x height). */
void ui_gl_render(ImDrawData *draw_data, int width, int height);
/* Loads an RGBA texture from a PNG file; false if missing or invalid. */
bool ui_gl_load_png(const char *path, UiTexture *texture);
void ui_gl_free_texture(UiTexture *texture);
/* Draws ImGui draw lists that are not part of an ImGui frame. */
void ui_gl_render_lists(ImDrawList **lists, int count, int width, int height);
/* Makes a width x height texture and draws into it: draw() fills an
 * ImDrawList whose coordinates are the texture's pixels. */
bool ui_gl_render_to_texture(UiTexture *texture, int width, int height,
	void (*draw)(ImDrawList *list, void *opaque), void *opaque);

/* 3D: textured, lit triangles for the cabinets. */
typedef struct {
	float x, y, z, nx, ny, nz, u, v;
	unsigned color; /* RGBA, little-endian bytes as ImGui's IM_COL32 */
} UiVertex3D;

/* Sets the viewport (pixels, top-left origin) and the view-projection
 * matrix (column-major), and enables depth testing. */
void ui_gl3d_begin(int x, int y, int width, int height, int framebuffer_height, const float view_projection[16]);
enum {
	UI3D_EMISSIVE = 1, /* not shaded (screen, marquee) */
	UI3D_BLEND = 2     /* alpha blended, no depth writes (shadows) */
};
/* Draws triangles with a model matrix; the vertex colours are multiplied
 * by tint (IM_COL32) and light. */
void ui_gl3d_draw(const UiVertex3D *vertices, int count, unsigned texture, const float model[16],
	float light, unsigned tint, int flags);
void ui_gl3d_end(void);
/* A 1x1 white texture for untextured surfaces. */
unsigned ui_gl_white_texture(void);

#endif
