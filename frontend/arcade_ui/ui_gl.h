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

/* Sets the viewport (pixels, top-left origin), the view-projection matrix
 * (column-major) and the eye position, and enables depth testing. */
void ui_gl3d_begin(int x, int y, int width, int height, int framebuffer_height,
	const float view_projection[16], const float eye[3]);
enum {
	UI3D_EMISSIVE = 1, /* lit from inside: no diffuse shading (screen, marquee) */
	UI3D_BLEND = 2,    /* alpha blended, no depth writes (shadows, floor) */
	UI3D_REFLECTION = 4 /* mirrored below the floor: fades into the floor colour */
};
/* How a surface takes the light. */
typedef struct {
	unsigned tint;   /* IM_COL32, multiplies the texture and vertex colours */
	float specular;  /* highlight strength, 0 for matte */
	float shininess; /* highlight size: larger is tighter */
	int flags;       /* UI3D_* */
} UiMaterial;
/* Draws triangles with a model matrix, lit per pixel by a key light and a
 * cool rim light; light scales the result (dimmed neighbours). */
void ui_gl3d_draw(const UiVertex3D *vertices, int count, unsigned texture, const float model[16],
	float light, const UiMaterial *material);
/* A texture from RGBA pixels; repeat tiles it. */
bool ui_gl_texture_rgba(UiTexture *texture, int width, int height, const void *rgba, bool repeat);
void ui_gl3d_end(void);
/* A 1x1 white texture for untextured surfaces. */
unsigned ui_gl_white_texture(void);

#endif
