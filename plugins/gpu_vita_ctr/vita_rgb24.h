#ifndef PSX_VITA_RGB24_H
#define PSX_VITA_RGB24_H

#include <stdint.h>

/* One scanline of a 24-bit display, from its byte offset in the VRAM row.
 * Scanout wraps within the 2048-byte row, as the GPU's X address does. */
static inline void psx_vita_rgb24_row(const uint16_t *vram, uint32_t display_byte_x,
                                    uint32_t display_y, uint32_t row,
                                    uint32_t width, uint32_t vram_height, uint8_t *rgba) {
    const uint32_t byte_x = display_byte_x & 2047u;
    const uint32_t physical_y = (display_y + row) & (vram_height - 1u);
    const uint8_t *source = (const uint8_t *)(vram + physical_y * 1024u);
    for (uint32_t x = 0; x < width; ++x) {
        const uint32_t byte = (byte_x + x * 3u) & 2047u;
        rgba[x * 4u] = source[byte];
        rgba[x * 4u + 1u] = source[(byte + 1u) & 2047u];
        rgba[x * 4u + 2u] = source[(byte + 2u) & 2047u];
        rgba[x * 4u + 3u] = 255;
    }
}

#endif
