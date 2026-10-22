#include "arcade_romset.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "arcade_media.h"
#include "mame_romload.h"
#include "../system.h"

enum { A_LOAD, A_FILL };

/* bios: 0 for a file every choice of BIOS needs, else the 1-based choice. */
typedef struct {
	const char *name;
	uint32_t size, crc32;
	uint8_t bios;
} arcade_rom_file;

/* A MAME ROM_LOAD (see mame_rom_load) or a ROM_FILL of `value`. `file` is
 * relative to the first file of the set. */
typedef struct {
	uint8_t kind, group, stride, swap, value;
	uint16_t file;
	uint32_t source_offset, offset, length;
} arcade_rom_op;

/* A file of the board package: text kept in memory (board.ini), or a region
 * of `size` bytes erased to `erase` and built by the ops. */
typedef struct {
	const char *name, *text;
	uint32_t size;
	uint8_t erase;
	uint32_t first_op, op_count;
	uint8_t bios;
} arcade_vfile;

/* bios_names: the choices of BIOS, comma separated ("j,e"), or "" if none. */
typedef struct {
	arcade_romset_info info;
	uint16_t first_file, file_count, first_vfile, vfile_count;
	const char *bios_names;
	uint8_t default_bios;
} arcade_romset;

#include "arcade_romsets.inc"

#define SET_COUNT (sizeof(arcade_sets) / sizeof(arcade_sets[0]))

unsigned arcade_romset_count(void)
{
	return SET_COUNT;
}

const arcade_romset_info *arcade_romset_at(unsigned index)
{
	return index < SET_COUNT ? &arcade_sets[index].info : NULL;
}

static const arcade_romset *find_set(const char *name, size_t length)
{
	unsigned i;
	for (i = 0; length && i < SET_COUNT; i++)
		if (strlen(arcade_sets[i].info.name) == length && !strncasecmp(arcade_sets[i].info.name, name, length))
			return &arcade_sets[i];
	return NULL;
}

static const arcade_rom_file *set_file(const arcade_romset *set, unsigned i)
{
	return &arcade_files[set->first_file + i];
}

static const mame_zip_entry *index_find(const mame_romindex *index, const arcade_romset *set,
	const arcade_rom_file *file, mame_zip **owner)
{
	return mame_romindex_find(index, set->info.name, set->info.parent, file->crc32, file->size, owner);
}

/* The first file of the set, for this choice of BIOS (0: none), that no zip
 * holds, or NULL if the set is complete. */
static const arcade_rom_file *first_missing_for(const mame_romindex *index, const arcade_romset *set, unsigned bios)
{
	unsigned i;
	for (i = 0; i < set->file_count; i++) {
		mame_zip *owner;
		const arcade_rom_file *file = set_file(set, i);
		if (file->bios && file->bios != bios)
			continue;
		if (!index_find(index, set, file, &owner))
			return file;
	}
	return NULL;
}

static unsigned bios_count(const arcade_romset *set)
{
	unsigned count = 0;
	const char *p = set->bios_names;
	if (!*p)
		return 0;
	for (count = 1; *p; p++)
		count += *p == ',';
	return count;
}

/* The name of choice n (1-based) in out. */
static void bios_name(const arcade_romset *set, unsigned n, char *out, size_t size)
{
	const char *p = set->bios_names;
	size_t length;
	while (--n && (p = strchr(p, ',')))
		p++;
	length = p ? strcspn(p, ",") : 0;
	if (length >= size)
		length = size - 1;
	if (p)
		memcpy(out, p, length);
	out[length] = 0;
}

static char g_bios_preference[16];

void arcade_romset_set_bios(const char *name)
{
	snprintf(g_bios_preference, sizeof(g_bios_preference), "%s", name ? name : "");
}

/* The choice of BIOS (1-based, 0 for a set without one) to load the set with:
 * the preferred one if the zips hold its files, else the set's default, else
 * any that they hold. -1 if none; *missing is then the first file the default
 * needs. */
static int choose_bios(const mame_romindex *index, const arcade_romset *set, const arcade_rom_file **missing)
{
	unsigned count = bios_count(set), n, order[8], tries = 0;
	char name[16];
	if (!count)
		return (*missing = first_missing_for(index, set, 0)) ? -1 : 0;
	for (n = 1; n <= count && tries < 8; n++) {
		bios_name(set, n, name, sizeof(name));
		if (g_bios_preference[0] && !strcasecmp(name, g_bios_preference))
			order[tries++] = n;
	}
	if (tries < 8)
		order[tries++] = set->default_bios;
	for (n = 1; n <= count && tries < 8; n++)
		order[tries++] = n;
	for (n = 0; n < tries; n++)
		if (!first_missing_for(index, set, order[n]))
			return (int)order[n];
	*missing = first_missing_for(index, set, set->default_bios);
	return -1;
}

/* The first file of the set that no zip holds, or NULL if the set is complete
 * (with some choice of BIOS). */
static const arcade_rom_file *first_missing(const mame_romindex *index, const arcade_romset *set)
{
	const arcade_rom_file *missing = NULL;
	return choose_bios(index, set, &missing) < 0 ? missing : NULL;
}

/* Files of a clone that its parent does not have (all files of a parent). */
static int own_file(const arcade_romset *set, const arcade_rom_file *file)
{
	const arcade_romset *parent = find_set(set->info.parent, strlen(set->info.parent));
	unsigned i;
	for (i = 0; parent && i < parent->file_count; i++) {
		const arcade_rom_file *other = set_file(parent, i);
		if (other->crc32 == file->crc32 && other->size == file->size)
			return 0;
	}
	return 1;
}

/* The set a zip is for. Its name, as in MAME, when that set is complete;
 * otherwise the complete set with the most own files in that zip. */
static const arcade_romset *identify(const mame_romindex *index, const char *zip_path)
{
	const char *base = mame_path_base(zip_path);
	const arcade_romset *named, *best = NULL;
	size_t stem_length;
	unsigned i, j, best_score = 0;
	int zip;
	if (!mame_path_is_zip(base, &stem_length))
		return NULL;
	named = find_set(base, stem_length);
	if (named && !first_missing(index, named))
		return named;
	zip = mame_romindex_named(index, base, stem_length);
	if (zip < 0)
		return NULL;
	for (i = 0; i < SET_COUNT; i++) {
		const arcade_romset *set = &arcade_sets[i];
		unsigned score = 0;
		for (j = 0; j < set->file_count; j++) {
			const arcade_rom_file *file = set_file(set, j);
			if (own_file(set, file) && mame_zip_find(mame_romindex_zip(index, (unsigned)zip), file->crc32, file->size))
				score++;
		}
		if (score > best_score && !first_missing(index, set)) {
			best = set;
			best_score = score;
		}
	}
	if (best && named != best)
		SysPrintf("Arcade: %.*s.zip holds %s (%s)\n", (int)stem_length, base,
			best->info.name, best->info.description);
	return best;
}

int arcade_romset_scan_index(const mame_romindex *index, unsigned char *available)
{
	unsigned i;
	int count = 0;
	for (i = 0; i < SET_COUNT; i++) {
		available[i] = !first_missing(index, &arcade_sets[i]);
		count += available[i];
	}
	return count;
}

int arcade_romset_scan(const char *rompath, unsigned char *available)
{
	mame_romindex *index = mame_romindex_open(rompath);
	int count;
	if (!index)
		return -1;
	count = arcade_romset_scan_index(index, available);
	mame_romindex_close(index);
	return count;
}

const arcade_romset_info *arcade_romset_identify(const char *zip_path)
{
	char rompath[4096];
	mame_romindex *index;
	const arcade_romset *set;
	size_t stem_length;
	if (!mame_path_is_zip(mame_path_base(zip_path), &stem_length))
		return NULL;
	mame_path_directory(zip_path, rompath, sizeof(rompath));
	if (!(index = mame_romindex_open(rompath)))
		return NULL;
	set = identify(index, zip_path);
	mame_romindex_close(index);
	return set ? &set->info : NULL;
}

/* The staged set: its zips stay indexed while the loader reads the files. */
static struct {
	const arcade_romset *set;
	mame_romindex *index;
	unsigned bios;
	char rompath[4096], directory[96];
} g_stage;

static const arcade_vfile *find_vfile(const char *set, size_t set_length, const char *name)
{
	unsigned i;
	if (!g_stage.set || strlen(g_stage.set->info.name) != set_length || strncmp(g_stage.set->info.name, set, set_length))
		return NULL;
	for (i = 0; i < g_stage.set->vfile_count; i++) {
		const arcade_vfile *vfile = &arcade_vfiles[g_stage.set->first_vfile + i];
		if (!strcmp(vfile->name, name) && (!vfile->bios || vfile->bios == g_stage.bios))
			return vfile;
	}
	return NULL;
}

static int media_size(void *context, const char *set, size_t set_length, const char *name, size_t *size)
{
	const arcade_vfile *vfile = find_vfile(set, set_length, name);
	(void)context;
	if (!vfile)
		return -1;
	*size = vfile->text ? strlen(vfile->text) : vfile->size;
	return 0;
}

static const char *media_text(void *context, const char *set, size_t set_length, const char *name)
{
	const arcade_vfile *vfile = find_vfile(set, set_length, name);
	(void)context;
	return vfile ? vfile->text : NULL;
}

/* Builds the region straight into dst. The file being placed is read once
 * for as long as consecutive operations use it. */
static int media_read(void *context, const char *set_name, size_t set_length, const char *name, void *dst, size_t size)
{
	const arcade_vfile *vfile = find_vfile(set_name, set_length, name);
	const arcade_romset *set = g_stage.set;
	uint8_t *region = dst, *file_data = NULL;
	int loaded = -1, result = 0;
	uint32_t i;
	(void)context;
	if (!vfile || vfile->text || vfile->size != size)
		return -1;
	memset(region, vfile->erase, size);
	for (i = 0; !result && i < vfile->op_count; i++) {
		const arcade_rom_op *op = &arcade_ops[vfile->first_op + i];
		const arcade_rom_file *file;
		if (op->kind == A_FILL) {
			if (op->offset > size || op->length > size - op->offset)
				result = -1;
			else
				memset(region + op->offset, op->value, op->length);
			continue;
		}
		file = set_file(set, op->file);
		if (file->bios && file->bios != g_stage.bios)
			continue;
		if (loaded != op->file) {
			mame_zip *owner;
			const mame_zip_entry *entry = index_find(g_stage.index, set, file, &owner);
			free(file_data);
			file_data = NULL;
			loaded = -1;
			if (!entry) {
				SysPrintf("Arcade: %s needs %s (crc %08x); no zip in %s holds it\n", set->info.name,
					file->name, (unsigned)file->crc32, g_stage.rompath);
				result = -1;
				break;
			}
			if (!(file_data = malloc(file->size)) || mame_zip_read(owner, entry, file_data)) {
				SysPrintf("Arcade: cannot read %s from its zip (CRC32 or format error)\n", file->name);
				result = -1;
				break;
			}
			loaded = op->file;
		}
		if (mame_rom_load(region, (uint32_t)size, file_data, file->size, op->source_offset, op->offset,
				op->length, op->group, op->stride, op->swap))
			result = -1;
	}
	free(file_data);
	return result;
}

static const arcade_media_provider provider = { media_size, media_read, media_text };

void arcade_romset_release(void)
{
	arcade_media_set_provider(NULL, NULL);
	mame_romindex_close(g_stage.index);
	memset(&g_stage, 0, sizeof(g_stage));
}

const char *arcade_romset_stage(const char *zip_path, const arcade_romset_info **info)
{
	const arcade_romset *set;
	const arcade_rom_file *missing;
	mame_romindex *index;
	char rompath[sizeof(g_stage.rompath)];
	arcade_romset_release();
	mame_path_directory(zip_path, rompath, sizeof(rompath));
	if (!(index = mame_romindex_open(rompath))) {
		SysPrintf("Arcade: cannot read the directory %s\n", rompath);
		return NULL;
	}
	if (!(set = identify(index, zip_path))) {
		const char *base = mame_path_base(zip_path);
		size_t stem_length;
		const arcade_romset *named = mame_path_is_zip(base, &stem_length) ? find_set(base, stem_length) : NULL;
		if (named && (missing = first_missing(index, named)))
			SysPrintf("Arcade: %s needs %s (crc %08x); no zip in %s holds it\n",
				named->info.name, missing->name, (unsigned)missing->crc32, rompath);
		mame_romindex_close(index);
		return NULL;
	}
	if (snprintf(g_stage.directory, sizeof(g_stage.directory), ARCADE_MEDIA_VIRTUAL_PREFIX "%s", set->info.name) >=
	    (int)sizeof(g_stage.directory)) {
		mame_romindex_close(index);
		return NULL;
	}
	{
		const arcade_rom_file *unused = NULL;
		g_stage.bios = (unsigned)choose_bios(index, set, &unused);
	}
	if (g_stage.bios) {
		char name[16];
		bios_name(set, g_stage.bios, name, sizeof(name));
		SysPrintf("Arcade: %s uses the %s BIOS\n", set->info.name, name);
	}
	g_stage.set = set;
	g_stage.index = index;
	memcpy(g_stage.rompath, rompath, sizeof(rompath));
	arcade_media_set_provider(&provider, NULL);
	if (info)
		*info = &set->info;
	return g_stage.directory;
}
