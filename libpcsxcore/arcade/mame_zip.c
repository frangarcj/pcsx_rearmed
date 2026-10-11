#include "mame_zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zlib_wrapper.h"

enum {
	EOCD_SIGNATURE = 0x06054b50, EOCD_SIZE = 22, EOCD_MAX_COMMENT = 0xffff,
	CENTRAL_SIGNATURE = 0x02014b50, CENTRAL_SIZE = 46,
	LOCAL_SIGNATURE = 0x04034b50, LOCAL_SIZE = 30,
	METHOD_STORED = 0, METHOD_DEFLATED = 8,
	CHUNK = 64 * 1024
};

struct mame_zip {
	FILE *file;
	unsigned count;
	mame_zip_entry *entries;
};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int read_at(FILE *file, long offset, void *dst, size_t size)
{
	return fseek(file, offset, SEEK_SET) == 0 && fread(dst, 1, size, file) == size ? 0 : -1;
}

/* The end-of-central-directory record is in the last 22 + 65535 bytes. */
static long find_eocd(FILE *file, uint8_t *eocd)
{
	long file_size, start, i;
	size_t span;
	uint8_t *tail;
	if (fseek(file, 0, SEEK_END) || (file_size = ftell(file)) < EOCD_SIZE)
		return -1;
	start = file_size > EOCD_SIZE + EOCD_MAX_COMMENT ? file_size - (EOCD_SIZE + EOCD_MAX_COMMENT) : 0;
	span = (size_t)(file_size - start);
	tail = malloc(span);
	if (!tail || read_at(file, start, tail, span)) {
		free(tail);
		return -1;
	}
	for (i = (long)span - EOCD_SIZE; i >= 0; i--)
		if (le32(tail + i) == EOCD_SIGNATURE) {
			memcpy(eocd, tail + i, EOCD_SIZE);
			free(tail);
			return start + i;
		}
	free(tail);
	return -1;
}

mame_zip *mame_zip_open(const char *path)
{
	uint8_t eocd[EOCD_SIZE], *directory = NULL, *p;
	uint32_t directory_size, directory_offset;
	unsigned count, i;
	mame_zip *zip = calloc(1, sizeof(*zip));
	if (!zip || !(zip->file = fopen(path, "rb")))
		goto fail;
	if (find_eocd(zip->file, eocd) < 0)
		goto fail;
	count = le16(eocd + 10);
	directory_size = le32(eocd + 12);
	directory_offset = le32(eocd + 16);
	/* Single disk only; 0xffff/0xffffffff mark zip64. */
	if (le16(eocd + 4) || le16(eocd + 6) || count != le16(eocd + 8) || count == 0xffff ||
	    directory_offset == 0xffffffffu || directory_size == 0xffffffffu)
		goto fail;
	directory = malloc(directory_size ? directory_size : 1);
	zip->entries = calloc(count ? count : 1, sizeof(*zip->entries));
	if (!directory || !zip->entries || read_at(zip->file, (long)directory_offset, directory, directory_size))
		goto fail;
	for (i = 0, p = directory; i < count; i++) {
		mame_zip_entry *entry = &zip->entries[i];
		uint16_t name_length, extra_length, comment_length;
		if ((size_t)(p - directory) + CENTRAL_SIZE > directory_size || le32(p) != CENTRAL_SIGNATURE)
			goto fail;
		name_length = le16(p + 28); extra_length = le16(p + 30); comment_length = le16(p + 32);
		if ((size_t)(p - directory) + CENTRAL_SIZE + name_length + extra_length + comment_length > directory_size)
			goto fail;
		entry->method = le16(p + 10);
		entry->crc32 = le32(p + 16);
		entry->compressed_size = le32(p + 20);
		entry->size = le32(p + 24);
		entry->local_offset = le32(p + 42);
		if (entry->compressed_size == 0xffffffffu || entry->size == 0xffffffffu ||
		    entry->local_offset == 0xffffffffu || (le16(p + 8) & 1))
			goto fail;
		memcpy(entry->name, p + CENTRAL_SIZE, name_length < sizeof(entry->name) - 1 ? name_length : sizeof(entry->name) - 1);
		p += CENTRAL_SIZE + name_length + extra_length + comment_length;
	}
	zip->count = count;
	free(directory);
	return zip;
fail:
	free(directory);
	mame_zip_close(zip);
	return NULL;
}

void mame_zip_close(mame_zip *zip)
{
	if (!zip)
		return;
	if (zip->file)
		fclose(zip->file);
	free(zip->entries);
	free(zip);
}

unsigned mame_zip_count(const mame_zip *zip)
{
	return zip ? zip->count : 0;
}

const mame_zip_entry *mame_zip_entry_at(const mame_zip *zip, unsigned index)
{
	return zip && index < zip->count ? &zip->entries[index] : NULL;
}

const mame_zip_entry *mame_zip_find(const mame_zip *zip, uint32_t crc32, uint32_t size)
{
	unsigned i;
	for (i = 0; zip && i < zip->count; i++)
		if (zip->entries[i].crc32 == crc32 && zip->entries[i].size == size)
			return &zip->entries[i];
	return NULL;
}

static int inflate_member(FILE *file, uint32_t compressed_size, uint8_t *dst, uint32_t size)
{
	z_stream z;
	uint8_t *chunk = malloc(CHUNK);
	int ret = Z_OK;
	if (!chunk)
		return -1;
	memset(&z, 0, sizeof(z));
	if (inflateInit2(&z, -15) != Z_OK) {
		free(chunk);
		return -1;
	}
	z.next_out = dst;
	z.avail_out = size;
	while (ret == Z_OK) {
		if (z.avail_in == 0) {
			uInt want = compressed_size < CHUNK ? compressed_size : CHUNK;
			if (want == 0 || fread(chunk, 1, want, file) != want)
				break;
			compressed_size -= want;
			z.next_in = chunk;
			z.avail_in = want;
		}
		ret = inflate(&z, Z_NO_FLUSH);
	}
	inflateEnd(&z);
	free(chunk);
	return ret == Z_STREAM_END && z.avail_out == 0 ? 0 : -1;
}

int mame_zip_read(mame_zip *zip, const mame_zip_entry *entry, uint8_t *dst)
{
	uint8_t local[LOCAL_SIZE];
	long data_offset;
	if (!zip || !entry || read_at(zip->file, (long)entry->local_offset, local, sizeof(local)) ||
	    le32(local) != LOCAL_SIGNATURE)
		return -1;
	data_offset = (long)entry->local_offset + LOCAL_SIZE + le16(local + 26) + le16(local + 28);
	if (fseek(zip->file, data_offset, SEEK_SET))
		return -1;
	if (entry->method == METHOD_STORED) {
		if (entry->compressed_size != entry->size ||
		    fread(dst, 1, entry->size, zip->file) != entry->size)
			return -1;
	} else if (entry->method == METHOD_DEFLATED) {
		if (inflate_member(zip->file, entry->compressed_size, dst, entry->size))
			return -1;
	} else {
		return -1;
	}
	return (uint32_t)crc32(0, dst, entry->size) == entry->crc32 ? 0 : -1;
}
