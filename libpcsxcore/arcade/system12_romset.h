/* Namco System 12 games from MAME romset zips. Files are found by CRC32 and
 * size in any zip of the directory (the set's own and its parent's first),
 * so merged, split and non-merged sets work whatever the zips are called.
 * See docs/system12-romsets.md. */
#ifndef PCSX_SYSTEM12_ROMSET_H
#define PCSX_SYSTEM12_ROMSET_H
#include <stddef.h>

typedef struct {
	const char *name, *parent, *description, *unsupported;
	int year;
} system12_romset_info;

unsigned system12_romset_count(void);
const system12_romset_info *system12_romset_at(unsigned index);
/* The set a zip is for: the set it names when that set is complete in the
 * zips of its directory; otherwise the complete set with the most of its
 * own files in that zip. NULL if none. Reads the zip directories. */
const system12_romset_info *system12_romset_identify(const char *zip_path);
/* available[i] (one byte per set, system12_romset_count()) is 1 when every
 * file of set i is in some zip of rompath. Returns how many are, or -1. */
int system12_romset_scan(const char *rompath, unsigned char *available);
/* 0 if every file of the set is in the zips of rompath; otherwise -1, with
 * the first missing file written to missing. */
int system12_romset_verify(const char *rompath, const char *set, char *missing, size_t missing_size);
/* 1 if the path names a .zip file. */
int system12_romset_is_zip(const char *path);
/* Builds the board regions of system12_romset_identify(zip_path) from the
 * zips next to it and loads the board (system12_load_regions). Returns
 * what that returns: 0, -1 (the log names a missing file), or -2 for a
 * set the adapter does not emulate. */
int system12_romset_load(const char *zip_path);

#endif
