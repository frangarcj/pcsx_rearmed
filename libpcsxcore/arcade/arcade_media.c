#include "arcade_media.h"

#include <string.h>
#include <sys/stat.h>

static const arcade_media_provider *g_provider;
static void *g_context;

void arcade_media_set_provider(const arcade_media_provider *provider, void *context)
{
	g_provider = provider;
	g_context = context;
}

int arcade_media_is_virtual(const char *path)
{
	return !strncmp(path, ARCADE_MEDIA_VIRTUAL_PREFIX, strlen(ARCADE_MEDIA_VIRTUAL_PREFIX));
}

/* "mame-zip:set/name" -> set, its length and name. */
static int split(const char *path, const char **set, size_t *set_length, const char **name)
{
	const char *slash;
	*set = path + strlen(ARCADE_MEDIA_VIRTUAL_PREFIX);
	slash = strchr(*set, '/');
	if (!slash || slash == *set || !slash[1])
		return -1;
	*set_length = (size_t)(slash - *set);
	*name = slash + 1;
	return 0;
}

FILE *arcade_media_open_text(const char *path)
{
	const char *set, *name, *text;
	size_t set_length;
	if (!arcade_media_is_virtual(path))
		return fopen(path, "r");
	if (!g_provider || !g_provider->text || split(path, &set, &set_length, &name) ||
	    !(text = g_provider->text(g_context, set, set_length, name)))
		return NULL;
	/* Read only: the provider keeps the text until it is released. */
	return fmemopen((void *)text, strlen(text), "r");
}

int arcade_media_size(const char *path, size_t *size)
{
	const char *set, *name;
	size_t set_length;
	struct stat st;
	if (arcade_media_is_virtual(path))
		return g_provider && !split(path, &set, &set_length, &name) &&
			!g_provider->size(g_context, set, set_length, name, size) ? 0 : -1;
	if (stat(path, &st) || !S_ISREG(st.st_mode))
		return -1;
	*size = (size_t)st.st_size;
	return 0;
}

int arcade_media_read(const char *path, void *dst, size_t size)
{
	const char *set, *name;
	size_t set_length, have;
	FILE *file;
	int ok;
	if (arcade_media_is_virtual(path))
		return g_provider && !split(path, &set, &set_length, &name) &&
			!g_provider->size(g_context, set, set_length, name, &have) && have == size &&
			!g_provider->read(g_context, set, set_length, name, dst, size) ? 0 : -1;
	if (arcade_media_size(path, &have) || have != size || !(file = fopen(path, "rb")))
		return -1;
	ok = fread(dst, 1, size, file) == size;
	fclose(file);
	return ok ? 0 : -1;
}
