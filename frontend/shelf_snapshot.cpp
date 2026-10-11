#include "shelf_snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include <png.h>
#ifdef __vita__
#include <psp2/io/stat.h>
#include <vitaGL.h>
#else
#include <sys/stat.h>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#endif

enum { SNAP_W = 320, SNAP_H = 240 };

static void make_folder(const std::string &path)
{
	const size_t slash = path.find_last_of('/');
	if (slash == std::string::npos)
		return;
#ifdef __vita__
	sceIoMkdir(path.substr(0, slash).c_str(), 0777);
#else
	mkdir(path.substr(0, slash).c_str(), 0777);
#endif
}

extern "C" int shelf_snapshot_save(const char *path, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return -1;
	std::vector<unsigned char> frame((size_t)w * h * 4), snap((size_t)SNAP_W * SNAP_H * 4);
	/* vitaGL reads back without waiting for the GPU (READBACKS_SPEEDHACK). */
	glFinish();
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, frame.data());
	/* Average each block into 320x240, top row first (GL's is the bottom). */
	for (int sy = 0; sy < SNAP_H; sy++) {
		const int y0 = sy * h / SNAP_H, y1 = (sy + 1) * h / SNAP_H > y0 ? (sy + 1) * h / SNAP_H : y0 + 1;
		for (int sx = 0; sx < SNAP_W; sx++) {
			const int x0 = sx * w / SNAP_W, x1 = (sx + 1) * w / SNAP_W > x0 ? (sx + 1) * w / SNAP_W : x0 + 1;
			unsigned sum[3] = { 0, 0, 0 };
			for (int fy = y0; fy < y1; fy++)
				for (int fx = x0; fx < x1; fx++)
					for (int c = 0; c < 3; c++)
						sum[c] += frame[((size_t)(h - 1 - fy) * w + fx) * 4 + c];
			const unsigned n = (unsigned)((y1 - y0) * (x1 - x0));
			unsigned char *out = &snap[((size_t)sy * SNAP_W + sx) * 4];
			for (int c = 0; c < 3; c++)
				out[c] = (unsigned char)(sum[c] / n);
			out[3] = 255;
		}
	}
	make_folder(path);
	png_image image;
	memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	image.width = SNAP_W;
	image.height = SNAP_H;
	image.format = PNG_FORMAT_RGBA;
	return png_image_write_to_file(&image, path, 0, snap.data(), 0, NULL) ? 0 : -1;
}
