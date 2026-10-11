/* PCSX's settings as arcade-shelf options: the options of the PCSX AwO
 * ROM selector (frontend/vita/rom_selector.cpp), by core option key; and
 * PCSX ReArcade's credits for the About panel. */
#ifndef PCSX_SHELF_OPTIONS_H
#define PCSX_SHELF_OPTIONS_H

#include "arcade_shelf.h"

extern const ShelfOption pcsx_shelf_options[];
extern const int pcsx_shelf_option_count;

/* The About text, "# " headings; version: the git revision, or "". */
#ifdef __cplusplus
#include <string>
std::string pcsx_shelf_about(const char *version);
#endif

#endif
