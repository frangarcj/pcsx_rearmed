#include "system12_romset.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

#include "mame_zip.h"
#include "../system12.h"

enum { S12_LOAD, S12_FILL };
enum { S12_MAIN, S12_BANKED, S12_SUB, S12_WAVE, S12_EEPROM, S12_REGIONS };

typedef struct {
	const char *name;
	uint32_t size, crc32;
} s12_rom_file;

/* A MAME ROM_LOAD: for each unit u of `group` bytes and lane l,
 * region[offset + u * stride + l] = file[source_offset + u * group +
 * (swap ? group - 1 - l : l)]. A ROM_FILL writes `value` instead. */
typedef struct {
	uint8_t region, kind;
	uint16_t file;
	uint32_t source_offset, offset, length;
	uint8_t group, stride, swap, value;
} s12_rom_op;

typedef struct {
	system12_romset_info info;
	const char *machine, *state;
	uint32_t ram_size;
	uint8_t boot_hack, alt_bank, tektagt_dma;
	uint32_t region_size[S12_REGIONS];
	uint16_t first_file, file_count, first_op, op_count;
} s12_romset;

#include "system12_romsets.inc"

#define SET_COUNT (sizeof(s12_sets) / sizeof(s12_sets[0]))

unsigned system12_romset_count(void)
{
	return SET_COUNT;
}

const system12_romset_info *system12_romset_at(unsigned index)
{
	return index < SET_COUNT ? &s12_sets[index].info : NULL;
}

static const s12_romset *find_set(const char *name, size_t length)
{
	unsigned i;
	for (i = 0; i < SET_COUNT; i++)
		if (strlen(s12_sets[i].info.name) == length && !strncasecmp(s12_sets[i].info.name, name, length))
			return &s12_sets[i];
	return NULL;
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/'), *backslash = strrchr(path, '\\'), *colon = strrchr(path, ':');
	const char *base = path;
	if (slash && slash + 1 > base) base = slash + 1;
	if (backslash && backslash + 1 > base) base = backslash + 1;
	if (colon && colon + 1 > base) base = colon + 1;
	return base;
}

/* rompath is the directory of a path ("ux0:data/x/roms/a.zip" -> "...roms"). */
static void directory_of(const char *path, char *out, size_t size)
{
	const char *base = base_name(path);
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

static int is_zip_name(const char *name, size_t *stem_length)
{
	size_t length = strlen(name);
	if (length <= 4 || strcasecmp(name + length - 4, ".zip"))
		return 0;
	*stem_length = length - 4;
	return 1;
}

/* Every zip of a rompath, searched by CRC32 and size. A set's own zip and
 * its parent's come first; the others follow in directory order. */
typedef struct {
	mame_zip **zip;
	char **stem;
	unsigned count;
} s12_index;

static void index_close(s12_index *index)
{
	unsigned i;
	for (i = 0; i < index->count; i++) {
		mame_zip_close(index->zip[i]);
		free(index->stem[i]);
	}
	free(index->zip);
	free(index->stem);
	memset(index, 0, sizeof(*index));
}

static int index_open(s12_index *index, const char *rompath)
{
	char path[4096];
	struct dirent *ent;
	unsigned capacity = 0;
	DIR *dir = opendir(rompath);
	memset(index, 0, sizeof(*index));
	if (!dir)
		return -1;
	while ((ent = readdir(dir))) {
		size_t stem_length;
		mame_zip *zip;
		if (!is_zip_name(ent->d_name, &stem_length) ||
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
	return 0;
}

static int zip_named(const s12_index *index, const char *name)
{
	unsigned i;
	for (i = 0; name[0] && i < index->count; i++)
		if (!strcasecmp(index->stem[i], name))
			return (int)i;
	return -1;
}

static const mame_zip_entry *index_find(const s12_index *index, const s12_romset *set,
	const s12_rom_file *file, mame_zip **owner)
{
	const int own = zip_named(index, set->info.name), parent = zip_named(index, set->info.parent);
	const mame_zip_entry *entry;
	unsigned i;
	if (own >= 0 && (entry = mame_zip_find(index->zip[own], file->crc32, file->size))) {
		*owner = index->zip[own];
		return entry;
	}
	if (parent >= 0 && (entry = mame_zip_find(index->zip[parent], file->crc32, file->size))) {
		*owner = index->zip[parent];
		return entry;
	}
	for (i = 0; i < index->count; i++)
		if ((int)i != own && (int)i != parent &&
		    (entry = mame_zip_find(index->zip[i], file->crc32, file->size))) {
			*owner = index->zip[i];
			return entry;
		}
	return NULL;
}

/* The first file of the set that no zip holds, or NULL if it is complete. */
static const s12_rom_file *first_missing(const s12_index *index, const s12_romset *set)
{
	unsigned i;
	for (i = 0; i < set->file_count; i++) {
		const s12_rom_file *file = &s12_files[set->first_file + i];
		mame_zip *owner;
		if (!index_find(index, set, file, &owner))
			return file;
	}
	return NULL;
}

/* Files of a clone that its parent does not have (all files of a parent). */
static int own_file(const s12_romset *set, const s12_rom_file *file)
{
	const s12_romset *parent = set->info.parent[0] ? find_set(set->info.parent, strlen(set->info.parent)) : NULL;
	unsigned i;
	for (i = 0; parent && i < parent->file_count; i++) {
		const s12_rom_file *other = &s12_files[parent->first_file + i];
		if (other->crc32 == file->crc32 && other->size == file->size)
			return 0;
	}
	return 1;
}

/* The set a zip is for. Its name, as in MAME, when that set is complete;
 * otherwise the complete set with the most own files in that zip (a zip
 * named soulclbr.zip that holds soulclbrac's program ROMs is soulclbrac). */
static const s12_romset *identify(const s12_index *index, const char *zip_path)
{
	const char *base = base_name(zip_path);
	const s12_romset *named, *best = NULL;
	size_t stem_length;
	unsigned i, j, best_score = 0;
	int zip;
	if (!is_zip_name(base, &stem_length))
		return NULL;
	named = find_set(base, stem_length);
	if (named && !first_missing(index, named))
		return named;
	for (zip = 0; zip < (int)index->count; zip++)
		if (strlen(index->stem[zip]) == stem_length && !strncasecmp(index->stem[zip], base, stem_length))
			break;
	if (zip == (int)index->count)
		return NULL;
	for (i = 0; i < SET_COUNT; i++) {
		const s12_romset *set = &s12_sets[i];
		unsigned score = 0;
		for (j = 0; j < set->file_count; j++) {
			const s12_rom_file *file = &s12_files[set->first_file + j];
			if (own_file(set, file) && mame_zip_find(index->zip[zip], file->crc32, file->size))
				score++;
		}
		if (score > best_score && !first_missing(index, set)) {
			best = set;
			best_score = score;
		}
	}
	if (best && named != best)
		SysPrintf("System12: %.*s.zip holds %s (%s)\n", (int)stem_length, base,
			best->info.name, best->info.description);
	return best;
}

const system12_romset_info *system12_romset_identify(const char *zip_path)
{
	char rompath[4096];
	s12_index index;
	const s12_romset *set;
	size_t stem_length;
	if (!is_zip_name(base_name(zip_path), &stem_length))
		return NULL;
	directory_of(zip_path, rompath, sizeof(rompath));
	if (index_open(&index, rompath))
		return NULL;
	set = identify(&index, zip_path);
	index_close(&index);
	return set ? &set->info : NULL;
}

int system12_romset_scan(const char *rompath, unsigned char *available)
{
	s12_index index;
	unsigned i;
	int count = 0;
	if (index_open(&index, rompath))
		return -1;
	for (i = 0; i < SET_COUNT; i++) {
		available[i] = !first_missing(&index, &s12_sets[i]);
		count += available[i];
	}
	index_close(&index);
	return count;
}

int system12_romset_verify(const char *rompath, const char *set_name, char *missing, size_t missing_size)
{
	const s12_romset *set = find_set(set_name, strlen(set_name));
	const s12_rom_file *file;
	s12_index index;
	if (missing_size)
		missing[0] = 0;
	if (!set || index_open(&index, rompath))
		return -1;
	file = first_missing(&index, set);
	if (file && missing_size)
		snprintf(missing, missing_size, "%s (crc %08x)", file->name, (unsigned)file->crc32);
	index_close(&index);
	return file ? -1 : 0;
}

static int apply_load(uint8_t *region, uint32_t region_size, const s12_rom_op *op, const uint8_t *file, uint32_t file_size)
{
	uint32_t units, u, lane;
	if (!op->group || !op->stride || op->length % op->group ||
	    op->source_offset > file_size || op->length > file_size - op->source_offset)
		return -1;
	units = op->length / op->group;
	if (!units)
		return 0;
	if (op->offset > region_size || (uint64_t)(units - 1) * op->stride + op->group > region_size - op->offset)
		return -1;
	if (op->group == 1 && op->stride == 1) {
		memcpy(region + op->offset, file + op->source_offset, op->length);
		return 0;
	}
	for (u = 0; u < units; u++)
		for (lane = 0; lane < op->group; lane++)
			region[op->offset + u * op->stride + lane] =
				file[op->source_offset + u * op->group + (op->swap ? op->group - 1 - lane : lane)];
	return 0;
}

static int build_regions(const s12_romset *set, const s12_index *index, const char *rompath,
	uint8_t *regions[S12_REGIONS])
{
	uint8_t *file_data = NULL;
	int loaded_file = -1;
	unsigned i;
	int result = 0;
	for (i = 0; i < S12_REGIONS; i++)
		if (!(regions[i] = calloc(1, set->region_size[i] ? set->region_size[i] : 1)))
			result = -1;
	for (i = 0; !result && i < set->op_count; i++) {
		const s12_rom_op *op = &s12_ops[set->first_op + i];
		const s12_rom_file *file;
		if (op->kind == S12_FILL) {
			if (op->offset > set->region_size[op->region] ||
			    op->length > set->region_size[op->region] - op->offset)
				result = -1;
			else
				memset(regions[op->region] + op->offset, op->value, op->length);
			continue;
		}
		file = &s12_files[set->first_file + op->file];
		if (loaded_file != op->file) {
			mame_zip *owner;
			const mame_zip_entry *entry = index_find(index, set, file, &owner);
			free(file_data);
			loaded_file = -1;
			if (!entry) {
				SysPrintf("System12: %s needs %s (crc %08x); no zip in %s holds it\n", set->info.name,
					file->name, (unsigned)file->crc32, rompath);
				result = -1;
				break;
			}
			if (!(file_data = malloc(file->size)) || mame_zip_read(owner, entry, file_data)) {
				SysPrintf("System12: cannot read %s from its zip (CRC32 or format error)\n", file->name);
				result = -1;
				break;
			}
			loaded_file = op->file;
		}
		if (apply_load(regions[op->region], set->region_size[op->region], op, file_data, file->size))
			result = -1;
	}
	free(file_data);
	if (result)
		for (i = 0; i < S12_REGIONS; i++) {
			free(regions[i]);
			regions[i] = NULL;
		}
	return result;
}

int system12_romset_is_zip(const char *path)
{
	size_t stem_length;
	return is_zip_name(base_name(path), &stem_length);
}

int system12_romset_load(const char *zip_path)
{
	const s12_romset *set;
	uint8_t *regions[S12_REGIONS] = { NULL };
	char rompath[4096];
	s12_index index;
	system12_desc desc = {0};
	char disk_path[4096];
	int result;
	directory_of(zip_path, rompath, sizeof(rompath));
	if (index_open(&index, rompath))
		return -1;
	if (!(set = identify(&index, zip_path))) {
		const char *base = base_name(zip_path);
		size_t stem_length;
		const s12_romset *named = is_zip_name(base, &stem_length) ? find_set(base, stem_length) : NULL;
		const s12_rom_file *file = named ? first_missing(&index, named) : NULL;
		if (file)
			SysPrintf("System12: %s needs %s (crc %08x); no zip in %s holds it\n",
				named->info.name, file->name, (unsigned)file->crc32, rompath);
		else
			SysPrintf("System12: %s is not a complete System 12 set\n", zip_path);
		index_close(&index);
		return -1;
	}
	if (strcmp(set->info.unsupported, "none")) {
		SysPrintf("System12: %s requires unimplemented board device(s): %s\n",
			set->info.name, set->info.unsupported);
		index_close(&index);
		return -2;
	}
	result = build_regions(set, &index, rompath, regions);
	index_close(&index);
	if (result)
		return -1;
	desc.mame_set = set->info.name;
	desc.machine = set->machine;
	desc.state = set->state;
	desc.unsupported = set->info.unsupported;
	desc.ram_size = set->ram_size;
	desc.main_size = set->region_size[S12_MAIN];
	desc.bank_size = set->region_size[S12_BANKED];
	desc.sub_size = set->region_size[S12_SUB];
	desc.wave_size = set->region_size[S12_WAVE];
	desc.boot_hack = set->boot_hack;
	desc.alt_bank = set->alt_bank;
	desc.tektagt_dma = set->tektagt_dma;
	desc.default_eeprom = regions[S12_EEPROM];
	desc.default_eeprom_size = set->region_size[S12_EEPROM];
	if (!strcmp(set->info.name, "truckk") || !strcmp(set->info.name, "ujlnow")) {
		if (snprintf(disk_path, sizeof(disk_path), "%s/%s/%s.chd", rompath, set->info.name,
			!strcmp(set->info.name, "truckk") ? "tkk2-a" : "ul1-a") >= (int)sizeof(disk_path)) {
			for (unsigned i = 0; i < S12_REGIONS; i++) free(regions[i]);
			return -1;
		}
		desc.disk_path = disk_path;
	}
	result = system12_load_regions(&desc, regions[S12_MAIN], regions[S12_BANKED],
		regions[S12_SUB], regions[S12_WAVE]);
	free(regions[S12_EEPROM]);
	return result;
}
