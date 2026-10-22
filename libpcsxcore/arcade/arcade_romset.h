/* Namco System 10, Namco System 11 and Capcom ZN games from MAME romset zips.
 * Files are found by CRC32 and size in any zip of the rompath (the set's own
 * and its parent's first), so merged, split and non-merged sets work whatever
 * the zips are called. The board package that scripts/prepare_system10_board.py,
 * prepare_system11_board.py and prepare_capcom_zn_board.py write is built in
 * memory, byte for byte, as a virtual directory for the board loaders
 * (arcade_media.h). See docs/arcade-romsets.md. */
#ifndef PCSX_ARCADE_ROMSET_H
#define PCSX_ARCADE_ROMSET_H
#include <stddef.h>

#include "mame_romindex.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	const char *system, *name, *parent, *description;
	int year;
} arcade_romset_info;

/* The sets the board loaders accept, from the MAME declarations at the
 * pinned commit. */
unsigned arcade_romset_count(void);
const arcade_romset_info *arcade_romset_at(unsigned index);
/* available[i] (one byte per set, arcade_romset_count()) is 1 when every
 * file of set i is in some zip of the rompath. Returns how many are, or -1. */
int arcade_romset_scan(const char *rompath, unsigned char *available);
int arcade_romset_scan_index(const mame_romindex *index, unsigned char *available);
/* The set a zip is for: the set it names when that set is complete in the
 * zips of its directory; otherwise the complete set with the most of its own
 * files in that zip. NULL if none. */
const arcade_romset_info *arcade_romset_identify(const char *zip_path);
/* The BIOS of a set that has a choice (Capcom ZN-1: "j", "e"); "" or an
 * unknown name for the set's default. A set loads with another BIOS if the
 * zips do not hold the preferred one's files. */
void arcade_romset_set_bios(const char *name);
/* Makes the set of zip_path available as a virtual board directory and
 * returns its path, for system10_load, system11_load or systemzn_load. It
 * stays valid until arcade_romset_release(), which the caller must call once
 * the loader has returned. NULL if the zip is not a complete supported set;
 * the log names the first missing file. */
const char *arcade_romset_stage(const char *zip_path, const arcade_romset_info **info);
void arcade_romset_release(void);

#ifdef __cplusplus
}
#endif

#endif
