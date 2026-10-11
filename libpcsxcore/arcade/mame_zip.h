/* Minimal reader for MAME romset zips: the central directory, and stored or
 * deflated members read whole and checked against their CRC32. No zip64,
 * encryption or multi-disk archives (MAME sets do not use them). */
#ifndef PCSX_MAME_ZIP_H
#define PCSX_MAME_ZIP_H
#include <stddef.h>
#include <stdint.h>

typedef struct mame_zip mame_zip;
typedef struct {
	char name[256];
	uint32_t crc32, compressed_size, size, local_offset;
	uint16_t method;
} mame_zip_entry;

/* NULL if the file is missing or is not a zip this reader supports. */
mame_zip *mame_zip_open(const char *path);
void mame_zip_close(mame_zip *zip);
unsigned mame_zip_count(const mame_zip *zip);
const mame_zip_entry *mame_zip_entry_at(const mame_zip *zip, unsigned index);
/* The member with this CRC32 and size, whatever its name; NULL if none. */
const mame_zip_entry *mame_zip_find(const mame_zip *zip, uint32_t crc32, uint32_t size);
/* Reads the whole member into dst (entry->size bytes). 0 on success, -1 on
 * an I/O, format or CRC32 error. */
int mame_zip_read(mame_zip *zip, const mame_zip_entry *entry, uint8_t *dst);

#endif
