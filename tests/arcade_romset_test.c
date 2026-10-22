/* Driver for tests/test_arcade_romset.py: the native System 10, System 11 and
 * Capcom ZN romset loader on zips of synthetic data.
 *
 *   scan ROMPATH                  one line per complete set
 *   identify ZIP                  the set the zip is for
 *   dump ZIP OUTDIR FILE...       stage ZIP and write each file of its board
 *                                 package (board.ini included) to OUTDIR
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../libpcsxcore/arcade/arcade_media.h"
#include "../libpcsxcore/arcade/arcade_romset.h"

void SysPrintf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

static int dump_file(const char *directory, const char *outdir, const char *name)
{
	char path[4096], out[4096];
	size_t size;
	void *data;
	FILE *file;
	int ok;
	snprintf(path, sizeof(path), "%s/%s", directory, name);
	snprintf(out, sizeof(out), "%s/%s", outdir, name);
	if (arcade_media_size(path, &size)) {
		fprintf(stderr, "no file %s\n", name);
		return -1;
	}
	if (!(data = malloc(size ? size : 1)))
		return -1;
	if (!strcmp(name, "board.ini")) {
		/* The loaders read the text through the same call. */
		FILE *text = arcade_media_open_text(path);
		ok = text && fread(data, 1, size, text) == size && fgetc(text) == EOF;
		if (text)
			fclose(text);
	} else
		ok = !arcade_media_read(path, data, size);
	if (!ok) {
		fprintf(stderr, "cannot read %s\n", name);
		free(data);
		return -1;
	}
	file = fopen(out, "wb");
	ok = file && fwrite(data, 1, size, file) == size;
	if (file)
		fclose(file);
	free(data);
	return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
	/* ARCADE_BIOS: the preferred BIOS of a set with a choice. */
	arcade_romset_set_bios(getenv("ARCADE_BIOS"));
	if (argc == 3 && !strcmp(argv[1], "scan")) {
		unsigned char *available = calloc(arcade_romset_count(), 1);
		unsigned i;
		if (arcade_romset_scan(argv[2], available) < 0)
			return 1;
		for (i = 0; i < arcade_romset_count(); i++)
			if (available[i])
				printf("%s\n", arcade_romset_at(i)->name);
		free(available);
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "cache")) {
		/* The index of an unchanged rompath is shared; a changed zip makes a new one. */
		mame_romindex *a = mame_romindex_open(argv[2]), *b, *c;
		FILE *file;
		if (!a)
			return 1;
		mame_romindex_close(a);
		b = mame_romindex_open(argv[2]);
		printf("same=%d\n", a == b);
		mame_romindex_close(b);
		if (!(file = fopen(argv[3], "ab")))
			return 1;
		fputc(0, file);
		fclose(file);
		c = mame_romindex_open(argv[2]);
		printf("changed=%d\n", c != b);
		mame_romindex_close(c);
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "identify")) {
		const arcade_romset_info *info = arcade_romset_identify(argv[2]);
		if (!info)
			return 1;
		printf("%s\n", info->name);
		return 0;
	}
	if (argc >= 5 && !strcmp(argv[1], "dump")) {
		const arcade_romset_info *info;
		const char *directory = arcade_romset_stage(argv[2], &info);
		int result = directory ? 0 : 1, i;
		for (i = 4; !result && i < argc; i++)
			result = dump_file(directory, argv[3], argv[i]) ? 1 : 0;
		if (directory)
			printf("%s\n", info->name);
		/* A file that is not in the package is not served either. */
		if (!result && directory) {
			char path[4096];
			size_t size;
			snprintf(path, sizeof(path), "%s/%s", directory, "no-such-file.bin");
			if (!arcade_media_size(path, &size))
				result = 1;
		}
		arcade_romset_release();
		return result;
	}
	fprintf(stderr, "usage: %s scan|identify|dump ...\n", argv[0]);
	return 2;
}
