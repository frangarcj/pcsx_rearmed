/* Runtime PSX GPU VRAM geometry shared by GPULIB and GPU_NEON. */
#ifndef PCSX_GPU_VRAM_H
#define PCSX_GPU_VRAM_H

#include <stdint.h>

#define GPU_VRAM_ROWS_CONSOLE 512u
#define GPU_VRAM_ROWS_SYSTEM12 1024u

extern unsigned gpu_vram_height;

/* Call before GPUinit. Returns 0 on success, -1 for invalid geometry or if
 * VRAM has already been allocated. Console builds accept 512; PCSX_SYSTEM12 builds also accept 1024. */
int gpu_set_vram_height(unsigned rows);
unsigned gpu_get_vram_height(void);
/* Source GPU lightgun latch, raw pixel-clock/scanline values. GP1(10h,08h)
 * returns it; GP1(0dh) clears it. A normal GPU reset preserves the latch. */
void gpu_set_lightgun_position(uint16_t x,uint16_t y);

/* Select GPU command/TPAGE semantics before GPU initialization. Model 2 is
 * the default. Model 1 is available only in PCSX_SYSTEM12 builds and selects
 * a 1024-row diagnostic geometry; board profiles should call
 * gpu_set_vram_height() with the physical VRAM geometry they declare. */
int gpu_set_model(int model);
/* Inline: the renderer asks for every primitive and state command. */
#ifdef PCSX_SYSTEM12
extern int gpu_model;
static inline int gpu_get_model(void) { return gpu_model; }
#else
static inline int gpu_get_model(void) { return 2; }
#endif

/* Map TPAGE fields to GPU_NEON's type-2/console renderer convention. */
static inline uint32_t gpu_tpage_to_renderer(uint32_t tpage)
{
#ifdef PCSX_SYSTEM12
  if (gpu_get_model() == 1) {
    return (tpage & 0x000f)
        | ((tpage & 0x0020) >> 1)       /* type 1 Y=256 -> renderer bit 4 */
        | ((tpage & 0x0040) << 5)       /* type 1 Y=512 -> renderer bit 11 */
        | ((tpage & 0x0180) >> 2)       /* blend bits 7-8 -> 5-6 */
        | ((tpage & 0x0600) >> 2)       /* depth bits 9-10 -> 7-8 */
        | (tpage & 0x2000);             /* type 1 interleaved texture */
  }
#endif
  return tpage & 0x1fff;
}

/* Restore type-1 TPAGE fields after GPU_NEON has updated renderer state. */
static inline uint32_t gpu_tpage_from_renderer(uint32_t tpage)
{
#ifdef PCSX_SYSTEM12
  if (gpu_get_model() == 1) {
    return (tpage & 0x000f)
        | ((tpage & 0x0010) << 1)
        | ((tpage & 0x0800) >> 5)
        | ((tpage & 0x0060) << 2)
        | ((tpage & 0x0180) << 2)
        | (tpage & 0x2000);
  }
#endif
  return tpage & 0x1fff;
}

#ifdef PCSX_SYSTEM12
#define GPU_VRAM_HEIGHT (gpu_vram_height)
#else
#define GPU_VRAM_HEIGHT GPU_VRAM_ROWS_CONSOLE
#endif
#define GPU_VRAM_Y_MASK (GPU_VRAM_HEIGHT - 1u)

#endif
