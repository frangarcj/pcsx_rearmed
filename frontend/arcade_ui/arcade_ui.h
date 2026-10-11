/* Arcade game selector: one cabinet per game (MAME parent), its versions
 * (MAME sets) listed beside it. Portable: the platform owns the window, the
 * GL context and the pad, and calls arcade_ui_frame once per frame. */
#ifndef PCSX_ARCADE_UI_H
#define PCSX_ARCADE_UI_H
#include <stddef.h>

enum {
	UI_UP = 1 << 0, UI_DOWN = 1 << 1, UI_LEFT = 1 << 2, UI_RIGHT = 1 << 3,
	UI_CROSS = 1 << 4, UI_CIRCLE = 1 << 5, UI_TRIANGLE = 1 << 6, UI_SQUARE = 1 << 7,
	UI_L = 1 << 8, UI_R = 1 << 9, UI_START = 1 << 10, UI_SELECT = 1 << 11
};

enum { ARCADE_UI_NONE, ARCADE_UI_LAUNCH, ARCADE_UI_QUIT };

typedef struct {
	const char *rompath;   /* MAME zips */
	const char *data_root; /* snap/, titles/, marquees/ and arcade_ui.cfg */
	int width, height;     /* framebuffer size in pixels */
	int demo;              /* 1: list every supported set as present (UI work);
	                        * 2: also give games every board's colours in turn */
	int flat;              /* 2D cabinet fronts instead of the 3D carousel */
} ArcadeUiConfig;

/* Call with the GL context current. */
bool arcade_ui_init(const ArcadeUiConfig *config);
void arcade_ui_shutdown(void);
/* buttons: the UI_* buttons held now; stick_x/y: the right stick, -1..1
 * (it turns the selected cabinet); dt: seconds since the last frame.
 * Draws one frame into the bound framebuffer. On ARCADE_UI_LAUNCH,
 * launch_path holds the zip path to give the core. */
int arcade_ui_frame(unsigned buttons, float stick_x, float stick_y, float dt,
	char *launch_path, size_t launch_size);

#endif
