/* PCSX ReArcade's NEON executable: the NEON renderer's frames shown through
 * vitaGL, so that the arcade-shelf menus can draw over them. */
#ifndef PCSX_VITA_GL_PRESENT_H
#define PCSX_VITA_GL_PRESENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts vitaGL, takes the core's raw VRAM frames (pcsx_raw_frame_cb) and
 * becomes the menus' presenter. 0 on success. */
int glp_init(void);
void glp_finish(void);
/* video_cb: a converted frame (the 24-bit modes), if the raw one did not
 * come, then the picture goes to the screen. */
void glp_video(const void *data, unsigned width, unsigned height, size_t pitch);

#ifdef __cplusplus
}
#endif

#endif
