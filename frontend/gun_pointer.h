/* Screen pointer coordinates shared by the Mac and Vita frontends.
 * GPL-2.0-or-later. Rectangles use top-left pixel coordinates. */
#ifndef PCSX_GUN_POINTER_H
#define PCSX_GUN_POINTER_H
#include <stdint.h>

static inline int gun_pointer_position(float px, float py, const int rect[4],
    unsigned *x, unsigned *y)
{
    float nx, ny;
    if (rect[2] < 2 || rect[3] < 2) return 0;
    nx = (px - rect[0]) / (rect[2] - 1);
    ny = (py - rect[1]) / (rect[3] - 1);
    if (nx < 0) nx = 0;
    if (nx > 1) nx = 1;
    if (ny < 0) ny = 0;
    if (ny > 1) ny = 1;
    *x = (unsigned)(nx * 65535); *y = (unsigned)(ny * 65535);
    return px >= rect[0] && px < rect[0] + rect[2] &&
        py >= rect[1] && py < rect[1] + rect[3];
}

static inline int16_t gun_pointer_to_retro(unsigned coordinate)
{
    return (int16_t)((int32_t)((uint64_t)coordinate * 65534 / 65535) - 32767);
}
static inline unsigned gun_pointer_from_retro(int16_t coordinate)
{
    int32_t value = (int32_t)coordinate + 32767;
    if (value < 0) value = 0;
    return (unsigned)((uint64_t)value * 65535 / 65534);
}
#endif
