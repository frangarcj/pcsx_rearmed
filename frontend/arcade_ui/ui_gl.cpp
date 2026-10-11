#include "ui_gl.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>
#include <imgui.h>

#ifdef __vita__
#include <vitaGL.h>
#define FB_GEN glGenFramebuffers
#define FB_DELETE glDeleteFramebuffers
#define FB_BIND glBindFramebuffer
#define FB_TEXTURE glFramebufferTexture2D
#define FB_TARGET GL_FRAMEBUFFER
#define FB_COLOR0 GL_COLOR_ATTACHMENT0
#define FB_BINDING GL_FRAMEBUFFER_BINDING
#else
#define GL_SILENCE_DEPRECATION
#define GL_GLEXT_PROTOTYPES
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
/* The macOS OpenGL 2.1 context has framebuffer objects as EXT functions. */
#define FB_GEN glGenFramebuffersEXT
#define FB_DELETE glDeleteFramebuffersEXT
#define FB_BIND glBindFramebufferEXT
#define FB_TEXTURE glFramebufferTexture2DEXT
#define FB_TARGET GL_FRAMEBUFFER_EXT
#define FB_COLOR0 GL_COLOR_ATTACHMENT0_EXT
#define FB_BINDING GL_FRAMEBUFFER_BINDING_EXT
#endif

static GLuint s_program, s_vbo, s_ibo, s_font_texture, s_white_texture;
static GLint s_screen_loc = -1;
static GLuint s_program3d;
static GLint s_vp_loc = -1, s_model_loc = -1, s_params_loc = -1, s_tint_loc = -1;

static GLuint compile_shader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint ok = GL_FALSE;
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		GLsizei length = 0;
		glGetShaderInfoLog(shader, sizeof(log), &length, log);
		fprintf(stderr, "arcade-ui: shader compile failed: %s\n", length ? log : "(no log)");
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint make_texture(int width, int height, const void *rgba)
{
	GLuint id = 0;
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	return id;
}

static GLuint link_program(const char *vs, const char *fs, const char *const *attributes, int count)
{
	GLuint vertex = compile_shader(GL_VERTEX_SHADER, vs);
	GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fs);
	GLint linked = GL_FALSE;
	GLuint program;
	if (!vertex || !fragment)
		return 0;
	program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	for (int i = 0; i < count; i++)
		glBindAttribLocation(program, i, attributes[i]);
	glLinkProgram(program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked) {
		fprintf(stderr, "arcade-ui: shader link failed\n");
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

/* Cabinets: one directional light; emissive surfaces keep their colour. */
static bool init_3d(void)
{
	static const char *vs =
		"attribute vec3 in_pos;\n"
		"attribute vec3 in_normal;\n"
		"attribute vec2 in_uv;\n"
		"attribute vec4 in_color;\n"
		"uniform mat4 u_vp;\n"
		"uniform mat4 u_model;\n"
		"uniform vec4 u_params;\n" /* light, emissive */
		"uniform vec4 u_tint;\n"
		"varying vec2 v_uv;\n"
		"varying vec4 v_color;\n"
		"void main() {\n"
		"  gl_Position = u_vp * (u_model * vec4(in_pos, 1.0));\n"
		"  vec3 n = normalize((u_model * vec4(in_normal, 0.0)).xyz);\n"
		"  float diffuse = 0.34 + 0.66 * max(dot(n, normalize(vec3(-0.45, 0.55, 0.70))), 0.0);\n"
		"  float k = mix(diffuse, 1.0, u_params.y) * u_params.x;\n"
		"  v_color = vec4(in_color.rgb * u_tint.rgb * k, in_color.a * u_tint.a);\n"
		"  v_uv = in_uv;\n"
		"}\n";
	static const char *fs =
		"uniform sampler2D u_texture;\n"
		"varying vec2 v_uv;\n"
		"varying vec4 v_color;\n"
		"void main() {\n"
		"  gl_FragColor = texture2D(u_texture, v_uv) * v_color;\n"
		"}\n";
	static const char *const attributes[] = { "in_pos", "in_normal", "in_uv", "in_color" };
	s_program3d = link_program(vs, fs, attributes, 4);
	if (!s_program3d)
		return false;
	s_vp_loc = glGetUniformLocation(s_program3d, "u_vp");
	s_model_loc = glGetUniformLocation(s_program3d, "u_model");
	s_params_loc = glGetUniformLocation(s_program3d, "u_params");
	s_tint_loc = glGetUniformLocation(s_program3d, "u_tint");
	glUseProgram(s_program3d);
	glUniform1i(glGetUniformLocation(s_program3d, "u_texture"), 0);
	glUseProgram(0);
	return true;
}

bool ui_gl_init(void)
{
	static const char *vs =
		"attribute vec2 in_pos;\n"
		"attribute vec2 in_uv;\n"
		"attribute vec4 in_color;\n"
		"uniform vec4 u_screen;\n" /* width, height, y sign (1, or -1 into a texture) */
		"varying vec2 v_uv;\n"
		"varying vec4 v_color;\n"
		"void main() {\n"
		"  gl_Position = vec4(in_pos.x * (2.0 / u_screen.x) - 1.0,\n"
		"                     (1.0 - in_pos.y * (2.0 / u_screen.y)) * u_screen.z, 0.0, 1.0);\n"
		"  v_uv = in_uv;\n"
		"  v_color = in_color;\n"
		"}\n";
	static const char *fs =
		"uniform sampler2D u_texture;\n"
		"varying vec2 v_uv;\n"
		"varying vec4 v_color;\n"
		"void main() {\n"
		"  gl_FragColor = texture2D(u_texture, v_uv) * v_color;\n"
		"}\n";
	GLuint vertex = compile_shader(GL_VERTEX_SHADER, vs);
	GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fs);
	GLint linked = GL_FALSE;
	unsigned char *pixels;
	int width, height;

	if (!vertex || !fragment)
		return false;
	s_program = glCreateProgram();
	glAttachShader(s_program, vertex);
	glAttachShader(s_program, fragment);
	glBindAttribLocation(s_program, 0, "in_pos");
	glBindAttribLocation(s_program, 1, "in_uv");
	glBindAttribLocation(s_program, 2, "in_color");
	glLinkProgram(s_program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	glGetProgramiv(s_program, GL_LINK_STATUS, &linked);
	if (!linked) {
		fprintf(stderr, "arcade-ui: shader link failed\n");
		return false;
	}
	s_screen_loc = glGetUniformLocation(s_program, "u_screen");
	glUseProgram(s_program);
	glUniform1i(glGetUniformLocation(s_program, "u_texture"), 0);
	glUseProgram(0);
	glGenBuffers(1, &s_vbo);
	glGenBuffers(1, &s_ibo);

	ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	s_font_texture = make_texture(width, height, pixels);
	ImGui::GetIO().Fonts->TexID = (ImTextureID)(uintptr_t)s_font_texture;
	{
		static const unsigned char white[4] = { 255, 255, 255, 255 };
		s_white_texture = make_texture(1, 1, white);
	}
	return init_3d();
}

void ui_gl_shutdown(void)
{
	if (s_font_texture) glDeleteTextures(1, &s_font_texture);
	if (s_white_texture) glDeleteTextures(1, &s_white_texture);
	if (s_program3d) glDeleteProgram(s_program3d);
	s_white_texture = s_program3d = 0;
	if (s_vbo) glDeleteBuffers(1, &s_vbo);
	if (s_ibo) glDeleteBuffers(1, &s_ibo);
	if (s_program) glDeleteProgram(s_program);
	s_font_texture = s_vbo = s_ibo = s_program = 0;
}

static void render(ImDrawData *draw_data, int width, int height, bool into_texture)
{
	if (!draw_data || !s_program)
		return;
	glViewport(0, 0, width, height);
	glUseProgram(s_program);
	glUniform4f(s_screen_loc, (float)width, (float)height, into_texture ? -1.0f : 1.0f, 0.0f);
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glEnable(GL_SCISSOR_TEST);
	glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void *)offsetof(ImDrawVert, pos));
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void *)offsetof(ImDrawVert, uv));
	glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), (void *)offsetof(ImDrawVert, col));
	for (int n = 0; n < draw_data->CmdListsCount; n++) {
		const ImDrawList *list = draw_data->CmdLists[n];
		size_t offset = 0;
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)list->VtxBuffer.Size * sizeof(ImDrawVert),
			list->VtxBuffer.Data, GL_STREAM_DRAW);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)list->IdxBuffer.Size * sizeof(ImDrawIdx),
			list->IdxBuffer.Data, GL_STREAM_DRAW);
		for (int c = 0; c < list->CmdBuffer.Size; c++) {
			const ImDrawCmd &cmd = list->CmdBuffer[c];
			if (cmd.UserCallback) {
				cmd.UserCallback(list, &cmd);
			} else {
				const int w = (int)(cmd.ClipRect.z - cmd.ClipRect.x);
				const int h = (int)(cmd.ClipRect.w - cmd.ClipRect.y);
				if (w > 0 && h > 0) {
					glScissor((int)cmd.ClipRect.x, into_texture ? (int)cmd.ClipRect.y : height - (int)cmd.ClipRect.w, w, h);
					glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)cmd.TextureId);
					glDrawElements(GL_TRIANGLES, (GLsizei)cmd.ElemCount,
						sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
						(void *)(offset * sizeof(ImDrawIdx)));
				}
			}
			offset += cmd.ElemCount;
		}
	}
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDisable(GL_SCISSOR_TEST);
	glUseProgram(0);
}

void ui_gl_render(ImDrawData *draw_data, int width, int height)
{
	render(draw_data, width, height, false);
}

void ui_gl_render_lists(ImDrawList **lists, int count, int width, int height)
{
	ImDrawData data;
	data.Valid = true;
	data.CmdLists = lists;
	data.CmdListsCount = count;
	render(&data, width, height, false);
}

bool ui_gl_render_to_texture(UiTexture *texture, int width, int height,
	void (*draw)(ImDrawList *list, void *opaque), void *opaque)
{
	GLint previous = 0, viewport[4];
	GLuint framebuffer = 0;
	ImDrawList list(ImGui::GetDrawListSharedData());
	ImDrawList *lists[1] = { &list };
	ImDrawData data;

	texture->id = make_texture(width, height, NULL);
	texture->width = width;
	texture->height = height;
	glGetIntegerv(FB_BINDING, &previous);
	glGetIntegerv(GL_VIEWPORT, viewport);
	FB_GEN(1, &framebuffer);
	FB_BIND(FB_TARGET, framebuffer);
	FB_TEXTURE(FB_TARGET, FB_COLOR0, GL_TEXTURE_2D, texture->id, 0);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	list.PushClipRect(ImVec2(0, 0), ImVec2((float)width, (float)height));
	list.PushTextureID(ImGui::GetIO().Fonts->TexID);
	draw(&list, opaque);
	data.Valid = true;
	data.CmdLists = lists;
	data.CmdListsCount = 1;
	render(&data, width, height, true);
	FB_BIND(FB_TARGET, (GLuint)previous);
	FB_DELETE(1, &framebuffer);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	return texture->id != 0;
}

unsigned ui_gl_white_texture(void)
{
	return s_white_texture;
}

void ui_gl3d_begin(int x, int y, int width, int height, int framebuffer_height, const float view_projection[16])
{
	glViewport(x, framebuffer_height - y - height, width, height);
	glEnable(GL_SCISSOR_TEST);
	glScissor(x, framebuffer_height - y - height, width, height);
	glDepthMask(GL_TRUE);
	glClear(GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glUseProgram(s_program3d);
	glUniformMatrix4fv(s_vp_loc, 1, GL_FALSE, view_projection);
	glActiveTexture(GL_TEXTURE0);
	glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
	for (int i = 0; i < 4; i++)
		glEnableVertexAttribArray(i);
}

void ui_gl3d_draw(const UiVertex3D *vertices, int count, unsigned texture, const float model[16],
	float light, unsigned tint, int flags)
{
	if (flags & UI3D_BLEND) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask(GL_FALSE);
	}
	glUniformMatrix4fv(s_model_loc, 1, GL_FALSE, model);
	glUniform4f(s_params_loc, light, (flags & UI3D_EMISSIVE) ? 1.0f : 0.0f, 0.0f, 0.0f);
	glUniform4f(s_tint_loc, (tint & 0xff) / 255.0f, ((tint >> 8) & 0xff) / 255.0f,
		((tint >> 16) & 0xff) / 255.0f, ((tint >> 24) & 0xff) / 255.0f);
	glBindTexture(GL_TEXTURE_2D, texture);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)count * sizeof(UiVertex3D), vertices, GL_STREAM_DRAW);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(UiVertex3D), (void *)offsetof(UiVertex3D, x));
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(UiVertex3D), (void *)offsetof(UiVertex3D, nx));
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex3D), (void *)offsetof(UiVertex3D, u));
	glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(UiVertex3D), (void *)offsetof(UiVertex3D, color));
	glDrawArrays(GL_TRIANGLES, 0, count);
	if (flags & UI3D_BLEND) {
		glDisable(GL_BLEND);
		glDepthMask(GL_TRUE);
	}
}

void ui_gl3d_end(void)
{
	for (int i = 0; i < 4; i++)
		glDisableVertexAttribArray(i);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glUseProgram(0);
}

bool ui_gl_load_png(const char *path, UiTexture *texture)
{
	png_image image;
	png_bytep pixels;
	memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&image, path))
		return false;
	image.format = PNG_FORMAT_RGBA;
	pixels = (png_bytep)malloc(PNG_IMAGE_SIZE(image));
	if (!pixels || !png_image_finish_read(&image, NULL, pixels, 0, NULL)) {
		free(pixels);
		png_image_free(&image);
		return false;
	}
	texture->id = make_texture((int)image.width, (int)image.height, pixels);
	texture->width = (int)image.width;
	texture->height = (int)image.height;
	free(pixels);
	return texture->id != 0;
}

void ui_gl_free_texture(UiTexture *texture)
{
	if (texture->id) {
		GLuint id = texture->id;
		glDeleteTextures(1, &id);
	}
	memset(texture, 0, sizeof(*texture));
}
