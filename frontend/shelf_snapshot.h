/* A game's frame as its arcade-shelf cabinet art (snap/<set>.png). */
#ifndef PCSX_SHELF_SNAPSHOT_H
#define PCSX_SHELF_SNAPSHOT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Saves the rect x, y, w, h (GL pixels, bottom-left origin) of the bound
 * framebuffer as a 320x240 PNG at path, creating its folder. 0 on success. */
int shelf_snapshot_save(const char *path, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif
