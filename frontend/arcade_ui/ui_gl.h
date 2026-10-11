/* ImGui drawing and PNG textures for the arcade UI, on vitaGL or on the
 * OpenGL 2.1 context of the macOS build: only GLES2-style calls, no VAOs. */
#ifndef PCSX_ARCADE_UI_GL_H
#define PCSX_ARCADE_UI_GL_H

struct ImDrawData;

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

#endif
