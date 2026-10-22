/* Every zip of a MAME rompath, searched by CRC32 and size: the file of a set
 * may be in its own zip, its parent's or any other (merged, split and
 * non-merged sets, whatever the zips are called). */
#ifndef PCSX_MAME_ROMINDEX_H
#define PCSX_MAME_ROMINDEX_H
#include <stddef.h>
#include <stdint.h>

#include "mame_zip.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mame_romindex mame_romindex;

/* NULL if the directory cannot be read. Reads the directory of every zip,
 * unless the last index of this rompath is still valid (same zip names,
 * sizes and times): then that one is shared. Not for use from several threads. */
mame_romindex *mame_romindex_open(const char *rompath);
void mame_romindex_close(mame_romindex *index);
/* Frees the kept index, if nobody has it open. */
void mame_romindex_flush(void);
unsigned mame_romindex_count(const mame_romindex *index);
mame_zip *mame_romindex_zip(const mame_romindex *index, unsigned i);
/* The zip's file name without ".zip". */
const char *mame_romindex_stem(const mame_romindex *index, unsigned i);
/* The zip named name, or -1. */
int mame_romindex_named(const mame_romindex *index, const char *name, size_t length);
/* The member with this CRC32 and size: in the zip named set, then in the zip
 * named parent (either may be ""), then in the others in directory order.
 * *owner is the zip that holds it. NULL if no zip does. */
const mame_zip_entry *mame_romindex_find(const mame_romindex *index, const char *set,
	const char *parent, uint32_t crc32, uint32_t size, mame_zip **owner);

/* The file name of a path, for "/", "\" and "device:" separators. */
const char *mame_path_base(const char *path);
/* The directory of a path ("ux0:data/x/roms/a.zip" -> "ux0:data/x/roms"). */
void mame_path_directory(const char *path, char *out, size_t size);
/* 1 if the name ends in ".zip"; *stem_length is the length without it. */
int mame_path_is_zip(const char *name, size_t *stem_length);

#ifdef __cplusplus
}
#endif

#endif
