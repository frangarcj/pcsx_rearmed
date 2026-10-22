#include "mame_romindex.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

struct mame_romindex {
	mame_zip **zip;
	char **stem;
	unsigned count;
	/* The last index of a rompath is kept while the zips are as they were
	 * (names, sizes, times), so a launch after a scan does not read every
	 * zip's directory again. */
	unsigned refs;
	int cached;
	uint64_t signature;
	char *rompath;
};

static mame_romindex *g_cache;

const char *mame_path_base(const char *path)
{
	const char *slash = strrchr(path, '/'), *backslash = strrchr(path, '\\'), *colon = strrchr(path, ':');
	const char *base = path;
	if (slash && slash + 1 > base) base = slash + 1;
	if (backslash && backslash + 1 > base) base = backslash + 1;
	if (colon && colon + 1 > base) base = colon + 1;
	return base;
}

void mame_path_directory(const char *path, char *out, size_t size)
{
	const char *base = mame_path_base(path);
	size_t length = (size_t)(base - path);
	if (length && (path[length - 1] == '/' || path[length - 1] == '\\'))
		length--;
	if (length >= size)
		length = size - 1;
	memcpy(out, path, length);
	out[length] = 0;
	if (!length && size > 1)
		strcpy(out, ".");
}

int mame_path_is_zip(const char *name, size_t *stem_length)
{
	size_t length = strlen(name);
	if (length <= 4 || strcasecmp(name + length - 4, ".zip"))
		return 0;
	*stem_length = length - 4;
	return 1;
}

static void index_free(mame_romindex *index)
{
	unsigned i;
	for (i = 0; i < index->count; i++) {
		mame_zip_close(index->zip[i]);
		free(index->stem[i]);
	}
	free(index->zip);
	free(index->stem);
	free(index->rompath);
	free(index);
}

void mame_romindex_close(mame_romindex *index)
{
	if (!index)
		return;
	if (--index->refs == 0 && !index->cached)
		index_free(index);
}

void mame_romindex_flush(void)
{
	if (g_cache && !g_cache->refs) {
		index_free(g_cache);
		g_cache = NULL;
	}
}

/* Names, sizes and times of the zips of a directory. 0 if it cannot be read. */
static uint64_t directory_signature(const char *rompath)
{
	char path[4096];
	struct dirent *ent;
	struct stat st;
	uint64_t hash = 0xcbf29ce484222325ull, sum = 0;
	DIR *dir = opendir(rompath);
	if (!dir)
		return 0;
	while ((ent = readdir(dir))) {
		size_t stem_length;
		const unsigned char *c;
		uint64_t entry = 0xcbf29ce484222325ull;
		if (!mame_path_is_zip(ent->d_name, &stem_length) ||
		    snprintf(path, sizeof(path), "%s/%s", rompath, ent->d_name) >= (int)sizeof(path) || stat(path, &st))
			continue;
		for (c = (const unsigned char *)ent->d_name; *c; c++)
			entry = (entry ^ *c) * 0x100000001b3ull;
		entry = (entry ^ (uint64_t)st.st_size) * 0x100000001b3ull;
		entry = (entry ^ (uint64_t)st.st_mtime) * 0x100000001b3ull;
		sum += entry; /* the order of the directory does not matter */
	}
	closedir(dir);
	return (hash ^ sum) | 1;
}

static mame_romindex *index_build(const char *rompath)
{
	char path[4096];
	struct dirent *ent;
	unsigned capacity = 0;
	mame_romindex *index = calloc(1, sizeof(*index));
	DIR *dir = index ? opendir(rompath) : NULL;
	if (!dir) {
		free(index);
		return NULL;
	}
	while ((ent = readdir(dir))) {
		size_t stem_length;
		mame_zip *zip;
		if (!mame_path_is_zip(ent->d_name, &stem_length) ||
		    snprintf(path, sizeof(path), "%s/%s", rompath, ent->d_name) >= (int)sizeof(path) ||
		    !(zip = mame_zip_open(path)))
			continue;
		if (index->count == capacity) {
			unsigned grow = capacity ? capacity * 2 : 16;
			mame_zip **zips = realloc(index->zip, grow * sizeof(*zips));
			char **stems = zips ? realloc(index->stem, grow * sizeof(*stems)) : NULL;
			if (zips) index->zip = zips;
			if (stems) index->stem = stems;
			if (!zips || !stems) {
				mame_zip_close(zip);
				break;
			}
			capacity = grow;
		}
		index->zip[index->count] = zip;
		index->stem[index->count] = malloc(stem_length + 1);
		if (!index->stem[index->count]) {
			mame_zip_close(zip);
			break;
		}
		memcpy(index->stem[index->count], ent->d_name, stem_length);
		index->stem[index->count][stem_length] = 0;
		index->count++;
	}
	closedir(dir);
	return index;
}

mame_romindex *mame_romindex_open(const char *rompath)
{
	const uint64_t signature = directory_signature(rompath);
	mame_romindex *index;
	if (!signature)
		return NULL;
	if (g_cache && g_cache->signature == signature && !strcmp(g_cache->rompath, rompath)) {
		g_cache->refs++;
		return g_cache;
	}
	if (!(index = index_build(rompath)))
		return NULL;
	index->signature = signature;
	index->refs = 1;
	index->rompath = strdup(rompath);
	if (index->rompath && (!g_cache || !g_cache->refs)) {
		mame_romindex_flush();
		g_cache = index;
		index->cached = 1;
	}
	return index;
}

unsigned mame_romindex_count(const mame_romindex *index)
{
	return index->count;
}

mame_zip *mame_romindex_zip(const mame_romindex *index, unsigned i)
{
	return index->zip[i];
}

const char *mame_romindex_stem(const mame_romindex *index, unsigned i)
{
	return index->stem[i];
}

int mame_romindex_named(const mame_romindex *index, const char *name, size_t length)
{
	unsigned i;
	for (i = 0; length && i < index->count; i++)
		if (strlen(index->stem[i]) == length && !strncasecmp(index->stem[i], name, length))
			return (int)i;
	return -1;
}

const mame_zip_entry *mame_romindex_find(const mame_romindex *index, const char *set,
	const char *parent, uint32_t crc32, uint32_t size, mame_zip **owner)
{
	const int own = mame_romindex_named(index, set, strlen(set));
	const int up = mame_romindex_named(index, parent, strlen(parent));
	const mame_zip_entry *entry;
	unsigned i;
	if (own >= 0 && (entry = mame_zip_find(index->zip[own], crc32, size))) {
		*owner = index->zip[own];
		return entry;
	}
	if (up >= 0 && (entry = mame_zip_find(index->zip[up], crc32, size))) {
		*owner = index->zip[up];
		return entry;
	}
	for (i = 0; i < index->count; i++)
		if ((int)i != own && (int)i != up && (entry = mame_zip_find(index->zip[i], crc32, size))) {
			*owner = index->zip[i];
			return entry;
		}
	return NULL;
}
