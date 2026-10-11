/* PCSX's settings as arcade-shelf options: the options of the PCSX AwO
 * ROM selector (frontend/vita/rom_selector.cpp) and the arcade controls,
 * by core option key; and
 * PCSX ReArcade's credits for the About panel. */
#ifndef PCSX_SHELF_OPTIONS_H
#define PCSX_SHELF_OPTIONS_H

#include "arcade_shelf.h"

/* The options for the selector, or the pause menu of a disc or an arcade
 * game; *count gets their number. */
enum { PCSX_OPTIONS_SELECTOR, PCSX_OPTIONS_DISC, PCSX_OPTIONS_ARCADE };
const ShelfOption *pcsx_shelf_options(int context, int *count);

/* The About text, "# " headings; version: the git revision, or "". */
#ifdef __cplusplus
#include <string>
std::string pcsx_shelf_about(const char *version);
#endif

#endif
