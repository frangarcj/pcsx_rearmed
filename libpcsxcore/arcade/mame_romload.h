/* The placement of a MAME ROM_LOAD in a region. */
#ifndef PCSX_MAME_ROMLOAD_H
#define PCSX_MAME_ROMLOAD_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* For each unit u of `group` bytes and lane l of the file range
 * [source_offset, source_offset + length):
 *   region[offset + u * stride + l] =
 *       file[source_offset + u * group + (swap ? group - 1 - l : l)].
 * 0, or -1 if the range does not fit the file or the region. */
int mame_rom_load(uint8_t *region, uint32_t region_size, const uint8_t *file, uint32_t file_size,
	uint32_t source_offset, uint32_t offset, uint32_t length, unsigned group, unsigned stride, int swap);

#ifdef __cplusplus
}
#endif

#endif
