#include "mac_pause.h"

#include <string.h>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>

#include "arcade_shelf.h"
#include "shelf_options.h"

extern "C" SDL_Window *g_window;
extern "C" void VitaCtrNativeRenderer_PresentCurrentFrame(void);
extern "C" void VitaCtrNativeRenderer_SetAspectMode(int mode);
extern "C" void NativeRenderer_SwapWindow(void);
extern "C" int PCSXMacVitaGL_CaptureCurrentFrame(void);
extern "C" int PCSXMacVitaGL_WriteCapturePpm(const char *path);

/* The options, kept for the session; they start at the Vita's defaults
 * (frontend/vita/settings.c). Only the aspect ratio acts on the harness. */
static std::vector<std::pair<std::string, std::string> > settings = {
	{ "pcsx_rearmed_region", "auto" }, { "pcsx_rearmed_bios", "auto" },
	{ "pcsx_rearmed_show_bios_bootlogo", "disabled" }, { "pcsx_rearmed_psxclock", "auto" },
	{ "pcsx_rearmed_cd_turbo", "disabled" }, { "pcsx_rearmed_cd_readahead", "64" },
	{ "vita_aspect_ratio", "4:3" }, { "pcsx_rearmed_screen_centering", "borderless" },
	{ "pcsx_rearmed_fractional_framerate", "auto" }, { "pcsx_rearmed_alt_flip", "auto" },
	{ "pcsx_rearmed_spu_reverb", "enabled" }, { "pcsx_rearmed_spu_interpolation", "gaussian" },
	{ "pcsx_rearmed_nocdaudio", "enabled" }, { "pcsx_rearmed_noxadecoding", "enabled" },
};

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
	if (!strcmp(key, "vita_aspect_ratio"))
		VitaCtrNativeRenderer_SetAspectMode(!strcmp(value, "16:9_stretched") ? 1 :
			!strcmp(value, "16:9_unstretched") ? 2 : 0);
}

/* Arrows; Enter or Z = cross; Escape, X or Backspace = circle (resume,
 * back). */
static unsigned keyboard_buttons(void)
{
	static const struct { SDL_Scancode key; unsigned button; } map[] = {
		{ SDL_SCANCODE_UP, SHELF_UP }, { SDL_SCANCODE_DOWN, SHELF_DOWN },
		{ SDL_SCANCODE_LEFT, SHELF_LEFT }, { SDL_SCANCODE_RIGHT, SHELF_RIGHT },
		{ SDL_SCANCODE_RETURN, SHELF_CROSS }, { SDL_SCANCODE_Z, SHELF_CROSS },
		{ SDL_SCANCODE_ESCAPE, SHELF_CIRCLE }, { SDL_SCANCODE_X, SHELF_CIRCLE },
		{ SDL_SCANCODE_BACKSPACE, SHELF_CIRCLE },
	};
	const bool *keys = SDL_GetKeyboardState(NULL);
	unsigned buttons = 0;
	for (const auto &entry : map)
		if (keys[entry.key])
			buttons |= entry.button;
	return buttons;
}

/* "D,D,X": the buttons of a script, in order. */
static std::vector<unsigned> parse_script(const char *script)
{
	std::vector<unsigned> steps;
	std::string text = script ? script : "";
	size_t start = 0;
	while (start < text.size()) {
		const size_t comma = text.find(',', start);
		const std::string token = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		static const struct { const char *name; unsigned button; } names[] = {
			{ "U", SHELF_UP }, { "D", SHELF_DOWN }, { "L", SHELF_LEFT }, { "R", SHELF_RIGHT },
			{ "X", SHELF_CROSS }, { "O", SHELF_CIRCLE }, { "-", 0 },
		};
		for (const auto &entry : names)
			if (token == entry.name)
				steps.push_back(entry.button);
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	return steps;
}

extern "C" int mac_pause_run(const char *game, const char *script, const char *shot)
{
	const std::vector<unsigned> steps = parse_script(script);
	const bool scripted = shot != NULL;
	unsigned frame = 0;
	const ShelfMenus menus = { pcsx_shelf_options, pcsx_shelf_option_count, NULL, get_option, set_option, 0, NULL };
	int fb_width = 960, fb_height = 544;
	SDL_GetWindowSizeInPixels(g_window, &fb_width, &fb_height);
	const ShelfConfig config = { "PCSX", "ReArcade", ".", "", 960, 544, NULL, 0, &menus, NULL, fb_width, fb_height };
	int result = MAC_PAUSE_RESUME;

	if (!shelf_init(&config)) {
		fprintf(stderr, "Pause menu: arcade-shelf init failed\n");
		shelf_shutdown();
		return result;
	}
	shelf_pause_open(game, 0, 0);
	/* The key that paused counts once released. */
	SDL_PumpEvents();
	unsigned ignored = keyboard_buttons();
	Uint64 last = SDL_GetTicksNS();
	for (;;) {
		SDL_Event event;
		bool quit = false;
		while (SDL_PollEvent(&event))
			if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
				quit = true;
		if (quit) {
			result = MAC_PAUSE_CLOSE;
			break;
		}
		const unsigned held = keyboard_buttons();
		ignored &= held;
		const Uint64 now = SDL_GetTicksNS();
		ShelfInput input = { held & ~ignored, 0.0f, 0.0f, (float)((now - last) / 1e9) };
		last = now;
		if (scripted) {
			/* From frame 10: the menu takes no press on the frame it opens. */
			const size_t step = frame / 10 - 1;
			input.buttons = frame >= 10 && step < steps.size() && frame % 10 == 0 ? steps[step] : 0;
			input.dt = 1.0f / 60.0f;
		}

		int slot = 0;
		GLint framebuffer = 0;
		VitaCtrNativeRenderer_PresentCurrentFrame();
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		const int action = shelf_pause_frame(&input, &slot);
		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)framebuffer);
		if (scripted && ++frame >= (steps.size() + 2) * 10) {
			if (!PCSXMacVitaGL_CaptureCurrentFrame() || PCSXMacVitaGL_WriteCapturePpm(shot) <= 0)
				fprintf(stderr, "Pause menu: cannot write %s\n", shot);
			result = MAC_PAUSE_CLOSE;
			break;
		}
		NativeRenderer_SwapWindow();
		if (action == SHELF_PAUSE_RESUME)
			break;
		if (action == SHELF_PAUSE_RESTART || action == SHELF_PAUSE_CLOSE) {
			result = action == SHELF_PAUSE_RESTART ? MAC_PAUSE_RESTART : MAC_PAUSE_CLOSE;
			break;
		}
		SDL_DelayNS(16000000);
	}
	glFinish();
	shelf_shutdown();
	return result;
}
