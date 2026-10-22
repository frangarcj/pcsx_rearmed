#include "mame_romload.h"

#include <string.h>

int mame_rom_load(uint8_t *region, uint32_t region_size, const uint8_t *file, uint32_t file_size,
	uint32_t source_offset, uint32_t offset, uint32_t length, unsigned group, unsigned stride, int swap)
{
	uint32_t units, u, lane;
	if (!group || !stride || length % group || source_offset > file_size || length > file_size - source_offset)
		return -1;
	units = length / group;
	if (!units)
		return 0;
	if (offset > region_size || (uint64_t)(units - 1) * stride + group > region_size - offset)
		return -1;
	if (group == 1 && stride == 1) {
		memcpy(region + offset, file + source_offset, length);
		return 0;
	}
	for (u = 0; u < units; u++)
		for (lane = 0; lane < group; lane++)
			region[offset + u * stride + lane] =
				file[source_offset + u * group + (swap ? group - 1 - lane : lane)];
	return 0;
}
