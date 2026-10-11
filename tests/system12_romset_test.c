/* Driver for tests/test_system12_romset.py: exercises the zip reader and the
 * ROM_LOAD placement of the System 12 romset loader on synthetic data.
 *
 *   zip PATH CRC32 SIZE OUT    read the member with that CRC32/size to OUT
 *   load SRC REGION_SIZE SOURCE_OFFSET OFFSET LENGTH GROUP STRIDE SWAP OUT
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../libpcsxcore/arcade/system12_romset.c"

void SysPrintf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

int system12_load_regions(const system12_desc *d, u8 *main, u8 *banked, u8 *sub, u8 *wave)
{
	(void)d;
	free(main); free(banked); free(sub); free(wave);
	return -1;
}

static int write_file(const char *path, const uint8_t *data, size_t size)
{
	FILE *f = fopen(path, "wb");
	int ok = f && fwrite(data, 1, size, f) == size;
	if (f) fclose(f);
	return ok ? 0 : -1;
}

static uint8_t *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *data = NULL;
	long length;
	if (!f) return NULL;
	if (!fseek(f, 0, SEEK_END) && (length = ftell(f)) >= 0 && !fseek(f, 0, SEEK_SET) &&
	    (data = malloc(length ? (size_t)length : 1)) && fread(data, 1, (size_t)length, f) == (size_t)length)
		*size = (size_t)length;
	else {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

int main(int argc, char **argv)
{
	if (argc == 6 && !strcmp(argv[1], "zip")) {
		mame_zip *zip = mame_zip_open(argv[2]);
		const mame_zip_entry *entry = zip ? mame_zip_find(zip, (uint32_t)strtoul(argv[3], NULL, 16),
			(uint32_t)strtoul(argv[4], NULL, 0)) : NULL;
		uint8_t *data = entry ? malloc(entry->size ? entry->size : 1) : NULL;
		int result = !data || mame_zip_read(zip, entry, data) || write_file(argv[5], data, entry->size);
		free(data);
		mame_zip_close(zip);
		puts(result ? "fail" : "ok");
		return result;
	}
	if (argc == 11 && !strcmp(argv[1], "load")) {
		size_t source_size = 0, region_size = strtoul(argv[3], NULL, 0);
		uint8_t *source = read_file(argv[2], &source_size);
		uint8_t *region = calloc(1, region_size);
		s12_rom_op op = {0};
		int result;
		op.source_offset = (uint32_t)strtoul(argv[4], NULL, 0);
		op.offset = (uint32_t)strtoul(argv[5], NULL, 0);
		op.length = (uint32_t)strtoul(argv[6], NULL, 0);
		op.group = (uint8_t)atoi(argv[7]);
		op.stride = (uint8_t)atoi(argv[8]);
		op.swap = (uint8_t)atoi(argv[9]);
		result = !source || !region || apply_load(region, (uint32_t)region_size, &op, source, (uint32_t)source_size) ||
			write_file(argv[10], region, region_size);
		free(source);
		free(region);
		puts(result ? "fail" : "ok");
		return result;
	}
	fprintf(stderr, "usage: see the comment at the top of %s\n", __FILE__);
	return 2;
}
