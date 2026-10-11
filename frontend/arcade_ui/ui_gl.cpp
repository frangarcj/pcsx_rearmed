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
#else
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#endif

static GLuint s_program, s_vbo, s_ibo, s_font_texture;
static GLint s_screen_loc = -1;

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

bool ui_gl_init(void)
{
	static const char *vs =
		"attribute vec2 in_pos;\n"
		"attribute vec2 in_uv;\n"
		"attribute vec4 in_color;\n"
		"uniform vec4 u_screen;\n"
		"varying vec2 v_uv;\n"
		"varying vec4 v_color;\n"
		"void main() {\n"
		"  gl_Position = vec4(in_pos.x * (2.0 / u_screen.x) - 1.0,\n"
		"                     1.0 - in_pos.y * (2.0 / u_screen.y), 0.0, 1.0);\n"
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
	return true;
}

void ui_gl_shutdown(void)
{
	if (s_font_texture) glDeleteTextures(1, &s_font_texture);
	if (s_vbo) glDeleteBuffers(1, &s_vbo);
	if (s_ibo) glDeleteBuffers(1, &s_ibo);
	if (s_program) glDeleteProgram(s_program);
	s_font_texture = s_vbo = s_ibo = s_program = 0;
}

void ui_gl_render(ImDrawData *draw_data, int width, int height)
{
	if (!draw_data || !s_program)
		return;
	glViewport(0, 0, width, height);
	glUseProgram(s_program);
	glUniform4f(s_screen_loc, (float)width, (float)height, 0.0f, 0.0f);
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
					glScissor((int)cmd.ClipRect.x, height - (int)cmd.ClipRect.w, w, h);
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
