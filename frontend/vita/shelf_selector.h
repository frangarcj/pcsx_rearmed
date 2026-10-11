#ifndef PCSX_VITA_SHELF_SELECTOR_H
#define PCSX_VITA_SHELF_SELECTOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PCSX ReArcade's selector (arcade-shelf): the System 12 MAME sets in the
 * ROM folder as cabinets, the PlayStation discs as discs. As
 * VitaRomSelector_Run: 1 and the path chosen, 0 if closed, -1 on error. */
int VitaShelfSelector_Run(char *out_path, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
