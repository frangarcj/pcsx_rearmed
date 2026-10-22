/* How a board loader reads its package (board.ini and the region files): from
 * a directory on disk, or from a virtual directory that a provider builds,
 * such as the MAME zips of arcade_romset.h. A virtual directory is named
 * "mame-zip:<set>"; the loaders use it like any directory path. */
#ifndef PCSX_ARCADE_MEDIA_H
#define PCSX_ARCADE_MEDIA_H
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARCADE_MEDIA_VIRTUAL_PREFIX "mame-zip:"

typedef struct {
	/* The size of file `name` of set `set` (set_length bytes), or -1. */
	int (*size)(void *context, const char *set, size_t set_length, const char *name, size_t *size);
	/* Writes the whole file; size is what size() returned. 0, or -1. */
	int (*read)(void *context, const char *set, size_t set_length, const char *name, void *dst, size_t size);
	/* The file's bytes if it is text kept in memory, else NULL. */
	const char *(*text)(void *context, const char *set, size_t set_length, const char *name);
} arcade_media_provider;

/* NULL provider removes it. There is one provider at a time. */
void arcade_media_set_provider(const arcade_media_provider *provider, void *context);

/* 1 if the path is inside a virtual directory. */
int arcade_media_is_virtual(const char *path);
/* A text file (board.ini) for reading: fopen(path, "r") on disk. */
FILE *arcade_media_open_text(const char *path);
/* The size of a file in bytes; 0, or -1 if it is missing. */
int arcade_media_size(const char *path, size_t *size);
/* Reads exactly `size` bytes of a file into dst, straight into the caller's
 * buffer. 0, or -1 if the file is missing, differs in size or cannot be read. */
int arcade_media_read(const char *path, void *dst, size_t size);

#ifdef __cplusplus
}
#endif

#endif
