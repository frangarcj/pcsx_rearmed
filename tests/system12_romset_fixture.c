/* Builds a System 12 set's regions from MAME zips with the native loader and
 * compares them with regions prepared by scripts/prepare_system12_board.py.
 *
 *   cc -Iinclude -o fixture tests/system12_romset_fixture.c \
 *      libpcsxcore/arcade/mame_zip.c libpcsxcore/arcade/system12_romset.c -lz
 *   ./fixture roms/soulclbr.zip artifacts/system12-profile
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../libpcsxcore/system12.h"
#include "../libpcsxcore/arcade/system12_romset.h"

static const char *g_reference;
static int g_compared;

void SysPrintf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}

static int compare(const char *name, const u8 *data, size_t size)
{
	char path[4096];
	FILE *f;
	u8 *expected;
	int same;
	snprintf(path, sizeof(path), "%s/%s", g_reference, name);
	if (!(f = fopen(path, "rb"))) {
		printf("%s: cannot open reference %s\n", name, path);
		return -1;
	}
	expected = malloc(size + 1);
	same = expected && fread(expected, 1, size + 1, f) == size && !memcmp(expected, data, size);
	printf("%s: %zu bytes %s\n", name, size, same ? "identical" : "DIFFERENT");
	free(expected);
	fclose(f);
	return same ? 0 : -1;
}

int system12_load_regions(const system12_desc *d, u8 *main, u8 *banked, u8 *sub, u8 *wave)
{
	int result = 0;
	printf("profile %s machine=%s state=%s ram=%#x boot_hack=%d alt_bank=%d tektagt_dma=%d\n",
		d->mame_set, d->machine, d->state, (unsigned)d->ram_size, d->boot_hack, d->alt_bank, d->tektagt_dma);
	result |= compare("main.bin", main, d->main_size);
	result |= compare("banked.bin", banked, d->bank_size);
	result |= compare("sub.bin", sub, d->sub_size);
	result |= compare("wave.bin", wave, d->wave_size);
	free(main); free(banked); free(sub); free(wave);
	g_compared = 1;
	return result;
}

int main(int argc, char **argv)
{
	char missing[512], rompath[4096];
	const system12_romset_info *info;
	const char *slash;
	unsigned char *available;
	unsigned i;
	int result;
	if (argc != 3) {
		fprintf(stderr, "usage: %s ROMPATH/FILE.zip REFERENCE_DIR\n", argv[0]);
		return 2;
	}
	g_reference = argv[2];
	slash = strrchr(argv[1], '/');
	snprintf(rompath, sizeof(rompath), "%.*s", slash ? (int)(slash - argv[1]) : 1, slash ? argv[1] : ".");
	available = calloc(system12_romset_count(), 1);
	printf("%u sets, %d complete in %s:", system12_romset_count(),
		system12_romset_scan(rompath, available), rompath);
	for (i = 0; i < system12_romset_count(); i++)
		if (available[i])
			printf(" %s", system12_romset_at(i)->name);
	printf("\n");
	free(available);
	if (!(info = system12_romset_identify(argv[1]))) {
		printf("%s: no complete System 12 set\n", argv[1]);
		return 1;
	}
	printf("%s is %s: %s (%d), parent %s\n", argv[1], info->name, info->description, info->year,
		info->parent[0] ? info->parent : "none");
	result = system12_romset_verify(rompath, info->name, missing, sizeof(missing));
	printf("verify: %s%s\n", result ? "missing " : "complete", result ? missing : "");
	result = system12_romset_load(argv[1]);
	printf("load: %d, regions %s\n", result, g_compared ? "compared" : "not built");
	return result || !g_compared;
}
