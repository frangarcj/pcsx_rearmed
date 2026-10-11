/* PCSX ReArcade's selector on macOS: arcade-shelf with the System 12 sets
 * of a rompath (frontend/arcade_catalog.cpp), to preview it without a Vita.
 *
 *   build/arcade-ui/arcade-ui [--rompath DIR] [--data DIR] [--demo] [--disc FILE]... [--pause]
 *       [--script STEPS] [--step-frames N] [--shot FILE.png] [--shots DIR] [--headless]
 *
 * --demo lists every supported set, with or without its zips. --disc adds a
 * PlayStation disc, as the Vita lists them: its cover is
 * <data>/covers/<file name without extension>.png. --pause shows the pause
 * menu over a stand-in game frame. The options are PCSX's
 * (frontend/shelf_options.cpp), kept in memory. Keys,
 * scripts and screenshots work as in arcade-shelf's viewer
 * (deps/arcade-shelf/examples/viewer/viewer.cpp).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

#include <SDL3/SDL.h>
#define GL_SILENCE_DEPRECATION
#define GL_GLEXT_PROTOTYPES
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <png.h>

#include "arcade_shelf.h"
#include "../../frontend/arcade_catalog.h"
#include "../../frontend/shelf_options.h"
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

/* Script steps that hold the right stick for the whole step. */
static bool script_stick(const std::string &name, float *x, float *y)
{
	if (name == "SL") { *x = -1; *y = 0; return true; }
	if (name == "SR") { *x = 1; *y = 0; return true; }
	if (name == "SU") { *x = 0; *y = -1; return true; }
	if (name == "SD") { *x = 0; *y = 1; return true; }
	return false;
}

static unsigned script_button(const std::string &name)
{
	static const struct { const char *name; unsigned button; } names[] = {
		{"U", SHELF_UP}, {"D", SHELF_DOWN}, {"L", SHELF_LEFT}, {"R", SHELF_RIGHT}, {"X", SHELF_CROSS},
		{"O", SHELF_CIRCLE}, {"T", SHELF_TRIANGLE}, {"S", SHELF_SQUARE}, {"LB", SHELF_L}, {"RB", SHELF_R},
		{"ST", SHELF_START}, {"SE", SHELF_SELECT}, {"-", 0},
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
	if (keys[SDL_SCANCODE_UP]) buttons |= SHELF_UP;
	if (keys[SDL_SCANCODE_DOWN]) buttons |= SHELF_DOWN;
	if (keys[SDL_SCANCODE_LEFT]) buttons |= SHELF_LEFT;
	if (keys[SDL_SCANCODE_RIGHT]) buttons |= SHELF_RIGHT;
	if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_RETURN]) buttons |= SHELF_CROSS;
	if (keys[SDL_SCANCODE_X]) buttons |= SHELF_CIRCLE;
	if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_ESCAPE]) buttons |= SHELF_TRIANGLE;
	if (keys[SDL_SCANCODE_A]) buttons |= SHELF_SQUARE;
	if (keys[SDL_SCANCODE_TAB]) buttons |= SHELF_SELECT;
	if (keys[SDL_SCANCODE_SPACE]) buttons |= SHELF_START;
	if (keys[SDL_SCANCODE_Q]) buttons |= SHELF_L;
	if (keys[SDL_SCANCODE_W]) buttons |= SHELF_R;
	return buttons;
}

static unsigned gamepad_buttons(SDL_Gamepad *pad)
{
	static const struct { SDL_GamepadButton button; unsigned shelf; } map[] = {
		{SDL_GAMEPAD_BUTTON_DPAD_UP, SHELF_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, SHELF_DOWN},
		{SDL_GAMEPAD_BUTTON_DPAD_LEFT, SHELF_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SHELF_RIGHT},
		{SDL_GAMEPAD_BUTTON_SOUTH, SHELF_CROSS}, {SDL_GAMEPAD_BUTTON_EAST, SHELF_CIRCLE},
		{SDL_GAMEPAD_BUTTON_NORTH, SHELF_TRIANGLE}, {SDL_GAMEPAD_BUTTON_WEST, SHELF_SQUARE},
		{SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SHELF_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SHELF_R},
		{SDL_GAMEPAD_BUTTON_START, SHELF_START}, {SDL_GAMEPAD_BUTTON_BACK, SHELF_SELECT},
	};
	unsigned buttons = 0;
	if (!pad)
		return 0;
	for (const auto &entry : map)
		if (SDL_GetGamepadButton(pad, entry.button))
			buttons |= entry.shelf;
	return buttons;
}

/* The settings and save states, in memory: each option starts on its
 * first value. */
static std::vector<std::pair<std::string, std::string> > settings;
static std::vector<int> saved_slots;

static const char *get_option(void *, const char *key)
{
	for (const auto &entry : settings)
		if (entry.first == key)
			return entry.second.c_str();
	return NULL;
}

static void set_option(void *, const char *key, const char *value)
{
	for (auto &entry : settings)
		if (entry.first == key)
			entry.second = value;
}

static int state_exists(void *, int slot)
{
	return std::find(saved_slots.begin(), saved_slots.end(), slot) != saved_slots.end();
}

/* A stand-in for the game's frame under the pause menu. */
static void draw_game_frame(void)
{
	glEnable(GL_SCISSOR_TEST);
	for (int y = 0; y < HEIGHT; y += 68)
		for (int x = 0; x < WIDTH; x += 80) {
			const int odd = (x / 80 + y / 68) & 1;
			glScissor(x, y, 80, 68);
			glClearColor(odd ? 0.85f : 0.2f, 0.35f + 0.3f * y / HEIGHT, odd ? 0.3f : 0.75f, 1);
			glClear(GL_COLOR_BUFFER_BIT);
		}
	glDisable(GL_SCISSOR_TEST);
}

int main(int argc, char **argv)
{
	const char *rompath = "roms", *data = "build/arcade-ui/data", *shot = NULL, *shots = NULL;
	std::vector<unsigned> script;
	std::vector<std::pair<float, float> > script_sticks;
	std::vector<std::string> discs;
	int pause = 0, slot = 0;
	int step_frames = 15, headless = 0, demo = 0, result = 0;
	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		const bool has_value = i + 1 < argc;
		if (arg == "--rompath" && has_value) rompath = argv[++i];
		else if (arg == "--demo") demo = 1;
		else if (arg == "--disc" && has_value) discs.push_back(argv[++i]);
		else if (arg == "--pause") pause = 1;
		else if (arg == "--data" && has_value) data = argv[++i];
		else if (arg == "--headless") headless = 1;
		else if (arg == "--shot" && has_value) shot = argv[++i];
		else if (arg == "--shots" && has_value) shots = argv[++i];
		else if (arg == "--step-frames" && has_value) step_frames = atoi(argv[++i]);
		else if (arg == "--script" && has_value) {
			std::string list = argv[++i];
			size_t start = 0;
			while (start <= list.size()) {
				size_t comma = list.find(',', start);
				const std::string token = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
				float x = 0, y = 0;
				const bool stick = script_stick(token, &x, &y);
				script.push_back(stick ? 0 : script_button(token));
				script_sticks.push_back(std::make_pair(x, y));
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
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_Window *window = SDL_CreateWindow("PCSX ReArcade", WIDTH, HEIGHT,
		SDL_WINDOW_OPENGL | (headless ? SDL_WINDOW_HIDDEN : 0));
	SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
	if (!context) {
		fprintf(stderr, "arcade-ui: no GL context: %s\n", SDL_GetError());
		return 1;
	}
	SDL_GL_SetSwapInterval(scripted ? 0 : 1);

	GLuint framebuffer = 0, color = 0, depth = 0;
	if (headless) {
		glGenFramebuffersEXT(1, &framebuffer);
		glGenRenderbuffersEXT(1, &color);
		glGenRenderbuffersEXT(1, &depth);
		glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, color);
		glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, WIDTH, HEIGHT);
		glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, depth);
		glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24, WIDTH, HEIGHT);
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, framebuffer);
		glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, color);
		glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, depth);
	}

	SDL_CreateDirectory(data);
	ArcadeCatalog catalogue;
	const std::string hint = std::string("Copy MAME romset zips (soulclbr.zip, tekken3.zip...) to ") + rompath;
	catalogue.build(rompath, demo != 0);
	for (const std::string &disc : discs) {
		const size_t slash = disc.find_last_of('/');
		std::string title = slash == std::string::npos ? disc : disc.substr(slash + 1);
		title = title.substr(0, title.find_last_of('.'));
		catalogue.add_disc(disc, title, title);
	}
	int option_count;
	const ShelfOption *options = pcsx_shelf_options(PCSX_OPTIONS_SELECTOR, &option_count);
	for (int i = 0; i < option_count; i++) {
		const ShelfOption &option = options[i];
		settings.push_back(std::make_pair(option.key, option.choices[0].value));
	}
	const ShelfMenus menus = { options, option_count, NULL, get_option, set_option, 10,
		state_exists };
	const std::string about = pcsx_shelf_about("");
	ShelfConfig config = { "PCSX", "ReArcade", data, hint.c_str(), WIDTH, HEIGHT,
		arcade_catalog_styles, arcade_catalog_style_count, &menus, about.c_str() };
	if (!shelf_init(&config)) {
		fprintf(stderr, "arcade-ui: init failed\n");
		return 1;
	}
	if (pause)
		shelf_pause_open("Soul Calibur (Asia, SOC14/VER.C)", slot, 0);
	else
		shelf_set_games(catalogue.games(), catalogue.count());

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
		float dt, stick_x = 0, stick_y = 0;
		if (scripted) {
			/* Fixed time step: the same script always draws the same frames. */
			dt = 1.0f / 60.0f;
			buttons = step < script.size() && frame % step_frames == 0 ? script[step] : 0;
			if (step < script.size()) {
				stick_x = script_sticks[step].first;
				stick_y = script_sticks[step].second;
			}
		} else {
			const Uint64 now = SDL_GetTicksNS();
			const bool *keys = SDL_GetKeyboardState(NULL);
			dt = (float)((now - last) / 1e9);
			last = now;
			buttons = keyboard_buttons() | gamepad_buttons(pad);
			/* Right stick, or J L I K. */
			if (pad) {
				stick_x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
				stick_y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
			}
			if (keys[SDL_SCANCODE_J]) stick_x = -1;
			if (keys[SDL_SCANCODE_L]) stick_x = 1;
			if (keys[SDL_SCANCODE_I]) stick_y = -1;
			if (keys[SDL_SCANCODE_K]) stick_y = 1;
		}
		char launch[1024];
		const ShelfInput input = { buttons, stick_x, stick_y, dt };
		int action = SHELF_NONE;
		if (pause) {
			draw_game_frame();
			const int result = shelf_pause_frame(&input, &slot);
			if (result != SHELF_PAUSE_NONE) {
				static const char *names[] = { "", "resume", "restart", "close", "save", "load" };
				printf("pause: %s, slot %d\n", names[result], slot);
				/* As the Vita does: save, then back on the save-state page. */
				if (result == SHELF_PAUSE_SAVE_STATE)
					saved_slots.push_back(slot);
				shelf_pause_open("Soul Calibur (Asia, SOC14/VER.C)", slot, result == SHELF_PAUSE_SAVE_STATE);
				if ((result == SHELF_PAUSE_RESUME || result == SHELF_PAUSE_CLOSE) && !scripted)
					running = false;
			}
		} else {
			glClearColor(0, 0, 0, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			action = shelf_frame(&input, launch, sizeof(launch));
		}
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
		if (action == SHELF_LAUNCH) {
			printf("launch %s\n", launch);
			if (!scripted)
				running = false;
		} else if (action == SHELF_RESCAN) {
			/* The rompath again, as the Vita does when its files change. */
			catalogue.build(rompath, demo != 0);
			for (const std::string &disc : discs) {
				const size_t slash = disc.find_last_of('/');
				std::string title = slash == std::string::npos ? disc : disc.substr(slash + 1);
				title = title.substr(0, title.find_last_of('.'));
				catalogue.add_disc(disc, title, title);
			}
			shelf_set_games(catalogue.games(), catalogue.count());
		} else if (action == SHELF_QUIT && !scripted) {
			running = false;
		}
		if (!headless)
			SDL_GL_SwapWindow(window);
	}
	shelf_shutdown();
	if (framebuffer) {
		glDeleteFramebuffersEXT(1, &framebuffer);
		glDeleteRenderbuffersEXT(1, &color);
		glDeleteRenderbuffersEXT(1, &depth);
	}
	SDL_GL_DestroyContext(context);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return result;
}
