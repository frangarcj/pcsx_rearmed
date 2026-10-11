/* The arcade selector on macOS, to work on the UI without a Vita.
 *
 *   build/arcade-ui/arcade-ui [--rompath DIR] [--data DIR] [--demo]
 *       [--script STEPS] [--step-frames N] [--shot FILE.png] [--shots DIR] [--headless]
 *
 * Keys: arrows; Z or Enter = cross (play); X or Esc = circle (exit);
 * S = triangle; A = square; Q/W = L/R. A game controller works too.
 * --script runs a comma-separated button list (U D L R X O T S LB RB ST SE,
 * or "-" for no button), one press every --step-frames frames, then exits.
 * --shots saves a PNG after each step; --shot saves the last frame.
 * --headless draws into an offscreen framebuffer instead of a window.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#define GL_SILENCE_DEPRECATION
#define GL_GLEXT_PROTOTYPES
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <png.h>

#include "../../frontend/arcade_ui/arcade_ui.h"
extern "C" {
#include "../../libpcsxcore/system12.h"
}

enum { WIDTH = 960, HEIGHT = 544 };

/* The romset module is linked without the core: it logs and never loads. */
extern "C" void SysPrintf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

extern "C" int system12_load_regions(const system12_desc *, u8 *main, u8 *banked, u8 *sub, u8 *wave)
{
	free(main); free(banked); free(sub); free(wave);
	return -1;
}

static unsigned script_button(const std::string &name)
{
	static const struct { const char *name; unsigned button; } names[] = {
		{"U", UI_UP}, {"D", UI_DOWN}, {"L", UI_LEFT}, {"R", UI_RIGHT}, {"X", UI_CROSS},
		{"O", UI_CIRCLE}, {"T", UI_TRIANGLE}, {"S", UI_SQUARE}, {"LB", UI_L}, {"RB", UI_R},
		{"ST", UI_START}, {"SE", UI_SELECT}, {"-", 0},
	};
	for (const auto &entry : names)
		if (name == entry.name)
			return entry.button;
	fprintf(stderr, "arcade-ui: unknown script button '%s'\n", name.c_str());
	exit(2);
}

static bool save_png(const char *path)
{
	std::vector<unsigned char> pixels(WIDTH * HEIGHT * 4), flipped(WIDTH * HEIGHT * 4);
	png_image image;
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	for (int y = 0; y < HEIGHT; y++)
		memcpy(&flipped[y * WIDTH * 4], &pixels[(HEIGHT - 1 - y) * WIDTH * 4], WIDTH * 4);
	memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	image.width = WIDTH;
	image.height = HEIGHT;
	image.format = PNG_FORMAT_RGBA;
	if (!png_image_write_to_file(&image, path, 0, flipped.data(), 0, NULL)) {
		fprintf(stderr, "arcade-ui: cannot write %s\n", path);
		return false;
	}
	return true;
}

static unsigned keyboard_buttons(void)
{
	const bool *keys = SDL_GetKeyboardState(NULL);
	unsigned buttons = 0;
	if (keys[SDL_SCANCODE_UP]) buttons |= UI_UP;
	if (keys[SDL_SCANCODE_DOWN]) buttons |= UI_DOWN;
	if (keys[SDL_SCANCODE_LEFT]) buttons |= UI_LEFT;
	if (keys[SDL_SCANCODE_RIGHT]) buttons |= UI_RIGHT;
	if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_RETURN]) buttons |= UI_CROSS;
	if (keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_ESCAPE]) buttons |= UI_CIRCLE;
	if (keys[SDL_SCANCODE_S]) buttons |= UI_TRIANGLE;
	if (keys[SDL_SCANCODE_A]) buttons |= UI_SQUARE;
	if (keys[SDL_SCANCODE_Q]) buttons |= UI_L;
	if (keys[SDL_SCANCODE_W]) buttons |= UI_R;
	return buttons;
}

static unsigned gamepad_buttons(SDL_Gamepad *pad)
{
	static const struct { SDL_GamepadButton button; unsigned ui; } map[] = {
		{SDL_GAMEPAD_BUTTON_DPAD_UP, UI_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, UI_DOWN},
		{SDL_GAMEPAD_BUTTON_DPAD_LEFT, UI_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, UI_RIGHT},
		{SDL_GAMEPAD_BUTTON_SOUTH, UI_CROSS}, {SDL_GAMEPAD_BUTTON_EAST, UI_CIRCLE},
		{SDL_GAMEPAD_BUTTON_NORTH, UI_TRIANGLE}, {SDL_GAMEPAD_BUTTON_WEST, UI_SQUARE},
		{SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, UI_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, UI_R},
		{SDL_GAMEPAD_BUTTON_START, UI_START}, {SDL_GAMEPAD_BUTTON_BACK, UI_SELECT},
	};
	unsigned buttons = 0;
	if (!pad)
		return 0;
	for (const auto &entry : map)
		if (SDL_GetGamepadButton(pad, entry.button))
			buttons |= entry.ui;
	return buttons;
}

int main(int argc, char **argv)
{
	const char *rompath = "roms", *data = "build/arcade-ui/data", *shot = NULL, *shots = NULL;
	std::vector<unsigned> script;
	int step_frames = 15, demo = 0, headless = 0, result = 0;
	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		const bool has_value = i + 1 < argc;
		if (arg == "--rompath" && has_value) rompath = argv[++i];
		else if (arg == "--data" && has_value) data = argv[++i];
		else if (arg == "--demo") demo = 1;
		else if (arg == "--headless") headless = 1;
		else if (arg == "--shot" && has_value) shot = argv[++i];
		else if (arg == "--shots" && has_value) shots = argv[++i];
		else if (arg == "--step-frames" && has_value) step_frames = atoi(argv[++i]);
		else if (arg == "--script" && has_value) {
			std::string list = argv[++i];
			size_t start = 0;
			while (start <= list.size()) {
				size_t comma = list.find(',', start);
				script.push_back(script_button(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
				if (comma == std::string::npos)
					break;
				start = comma + 1;
			}
		} else {
			fprintf(stderr, "usage: see the comment at the top of tools/arcade-ui/arcade_ui_mac.cpp\n");
			return 2;
		}
	}
	const bool scripted = !script.empty() || shot;

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
		fprintf(stderr, "arcade-ui: SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	SDL_Window *window = SDL_CreateWindow("Arcade selector", WIDTH, HEIGHT,
		SDL_WINDOW_OPENGL | (headless ? SDL_WINDOW_HIDDEN : 0));
	SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
	if (!context) {
		fprintf(stderr, "arcade-ui: no GL context: %s\n", SDL_GetError());
		return 1;
	}
	SDL_GL_SetSwapInterval(scripted ? 0 : 1);

	GLuint framebuffer = 0, color = 0;
	if (headless) {
		glGenFramebuffersEXT(1, &framebuffer);
		glGenRenderbuffersEXT(1, &color);
		glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, color);
		glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, WIDTH, HEIGHT);
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, framebuffer);
		glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, color);
	}

	SDL_CreateDirectory(data);
	ArcadeUiConfig config = { rompath, data, WIDTH, HEIGHT, demo };
	if (!arcade_ui_init(&config)) {
		fprintf(stderr, "arcade-ui: init failed\n");
		return 1;
	}

	SDL_Gamepad *pad = NULL;
	Uint64 last = SDL_GetTicksNS();
	size_t step = 0;
	int frame = 0;
	for (bool running = true; running;) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_EVENT_QUIT)
				running = false;
			else if (event.type == SDL_EVENT_GAMEPAD_ADDED && !pad)
				pad = SDL_OpenGamepad(event.gdevice.which);
		}
		unsigned buttons;
		float dt;
		if (scripted) {
			/* Fixed time step: the same script always draws the same frames. */
			dt = 1.0f / 60.0f;
			buttons = step < script.size() && frame % step_frames == 0 ? script[step] : 0;
		} else {
			const Uint64 now = SDL_GetTicksNS();
			dt = (float)((now - last) / 1e9);
			last = now;
			buttons = keyboard_buttons() | gamepad_buttons(pad);
		}
		char launch[1024];
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		const int action = arcade_ui_frame(buttons, dt, launch, sizeof(launch));
		frame++;
		if (scripted && frame % step_frames == 0) {
			if (shots) {
				char path[1024];
				snprintf(path, sizeof(path), "%s/step-%02zu.png", shots, step);
				save_png(path);
			}
			if (++step >= script.size()) {
				if (shot && !save_png(shot))
					result = 1;
				running = false;
			}
		}
		if (action == ARCADE_UI_LAUNCH) {
			printf("launch %s\n", launch);
			if (!scripted)
				running = false;
		} else if (action == ARCADE_UI_QUIT && !scripted) {
			running = false;
		}
		if (!headless)
			SDL_GL_SwapWindow(window);
	}
	arcade_ui_shutdown();
	if (framebuffer) {
		glDeleteFramebuffersEXT(1, &framebuffer);
		glDeleteRenderbuffersEXT(1, &color);
	}
	SDL_GL_DestroyContext(context);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return result;
}
