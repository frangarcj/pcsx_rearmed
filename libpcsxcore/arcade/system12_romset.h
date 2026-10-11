/* Namco System 12 games from MAME romset zips (roms/<set>.zip, with the
 * parent's zip for clones). Files are found by CRC32 and size, so merged,
 * split and non-merged sets all work. See docs/system12-romsets.md. */
#ifndef PCSX_SYSTEM12_ROMSET_H
#define PCSX_SYSTEM12_ROMSET_H
#include <stddef.h>

typedef struct {
	const char *name, *parent, *description, *unsupported;
	int year;
} system12_romset_info;

unsigned system12_romset_count(void);
const system12_romset_info *system12_romset_at(unsigned index);
/* The set a zip path names: its file name without ".zip". NULL otherwise. */
const system12_romset_info *system12_romset_for_path(const char *zip_path);
/* Checks that every file of the set is in the zips of rompath, without
 * reading them. 0 if complete; otherwise -1, with the first missing file
 * written to missing. */
int system12_romset_verify(const char *rompath, const char *set, char *missing, size_t missing_size);
/* Builds the board regions from the zips next to zip_path and loads the
 * board (system12_load_regions). Returns what that returns: 0, -1, or -2
 * for a set the adapter does not emulate. */
int system12_romset_load(const char *zip_path);

#endif
