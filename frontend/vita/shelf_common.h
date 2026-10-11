/* What the arcade-shelf selector and pause menu share, in both PCSX ReArcade
 * executables: the vitaGL one (selector, vitaGL renderer) and the NEON one
 * (NEON renderer presented through vitaGL, launched from the selector). */
#ifndef PCSX_VITA_SHELF_COMMON_H
#define PCSX_VITA_SHELF_COMMON_H

#include <stddef.h>

#ifdef __cplusplus
#include <string>
#include <psp2/kernel/processmgr.h>
#include "arcade_shelf.h"
extern "C" {
#endif

/* How the menus reach the screen and the game's last frame. */
typedef struct {
	/* Runs task on the thread that owns the GL context, and waits. */
	void (*run_task)(void (*task)(void *), void *arg);
	/* Draws the game's last frame into the window. */
	void (*present_frame)(void);
	/* Where that frame lies in the window: GL pixels, bottom-left origin. */
	void (*frame_rect)(int rect[4]);
	void (*swap)(void);
	/* The aspect option (0 4:3, 1 16:9 stretched, 2 16:9 unstretched). */
	void (*set_aspect)(int mode);
} VitaShelfPresenter;

void VitaShelf_SetPresenter(const VitaShelfPresenter *presenter);
const VitaShelfPresenter *VitaShelf_Presenter(void);
/* The aspect option in settings.cfg, as set_aspect takes it. */
int VitaShelf_AspectMode(void);
/* The game about to run, and its title for the pause menu's header. */
void VitaShelf_SetGame(const char *path, const char *title);
/* That title (or the file's name) into out. */
void VitaShelf_GetGameTitle(const char *path, char *out, size_t size);

#ifdef VITA_DEBUG_SHOT
/* Hardware debugging (VITA_DEBUG_SHOT=1 builds): creating APP_ROOT/shot.req
 * makes a screenshot, APP_ROOT/shot.ppm, and removes the request. The menus
 * call ShotPoll before each swap, on the GL thread; the game loop calls
 * ShotRequested, then ShotGame. Requests are checked every 30th call. */
int VitaShelf_ShotRequested(void);
void VitaShelf_ShotPoll(void);
void VitaShelf_ShotGame(void);
#endif

#ifdef __cplusplus
}

/* For the C++ menus. */
const char *VitaShelf_GetOption(void *user, const char *key);
void VitaShelf_SetOption(void *user, const char *key, const char *value);
ShelfConfig VitaShelf_Config(const ShelfMenus *menus);
/* The pad as the shelf takes it. Buttons held when a menu opens (from the
 * game, or the menu before) count once released. */
ShelfInput VitaShelf_ReadInput(unsigned *ignored, SceUInt64 *previous_time);
/* The title for path: the one given to VitaShelf_SetGame, else the file's name. */
std::string VitaShelf_GameTitle(const char *path);
#endif

#endif
