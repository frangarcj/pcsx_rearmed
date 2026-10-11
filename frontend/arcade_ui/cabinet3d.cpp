#include "cabinet3d.h"

#include <math.h>
#include <string.h>
#include <vector>

#include <imgui.h>

#include "ui_gl.h"

/* ---- matrices ---- */

void mat4_identity(float m[16])
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat4_multiply(float out[16], const float a[16], const float b[16])
{
	float r[16];
	for (int column = 0; column < 4; column++)
		for (int row = 0; row < 4; row++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++)
				sum += a[k * 4 + row] * b[column * 4 + k];
			r[column * 4 + row] = sum;
		}
	memcpy(out, r, sizeof(r));
}

void mat4_perspective(float m[16], float fov_y_degrees, float aspect, float z_near, float z_far)
{
	const float f = 1.0f / tanf(fov_y_degrees * (float)M_PI / 360.0f);
	memset(m, 0, 16 * sizeof(float));
	m[0] = f / aspect;
	m[5] = f;
	m[10] = (z_far + z_near) / (z_near - z_far);
	m[11] = -1.0f;
	m[14] = 2.0f * z_far * z_near / (z_near - z_far);
}

static void normalize3(float v[3])
{
	const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (length > 0.0f)
		for (int i = 0; i < 3; i++)
			v[i] /= length;
}

void mat4_look_at(float m[16], const float eye[3], const float target[3], const float up[3])
{
	float f[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
	float s[3], u[3];
	normalize3(f);
	s[0] = f[1] * up[2] - f[2] * up[1];
	s[1] = f[2] * up[0] - f[0] * up[2];
	s[2] = f[0] * up[1] - f[1] * up[0];
	normalize3(s);
	u[0] = s[1] * f[2] - s[2] * f[1];
	u[1] = s[2] * f[0] - s[0] * f[2];
	u[2] = s[0] * f[1] - s[1] * f[0];
	mat4_identity(m);
	m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
	m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
	m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
	m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
	m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
	m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
}

void mat4_place_tilted(float m[16], float x, float y, float z, float yaw_degrees,
	float pitch_degrees, float pivot_y)
{
	/* The tilt is about the world X axis (toward the viewer), after the turn:
	 * T(x, y + pivot, z) Rx(pitch) T(0, -pivot, 0) Ry(yaw). */
	const float a = pitch_degrees * (float)M_PI / 180.0f, c = cosf(a), s = sinf(a);
	float place[16], tilt[16], down[16], turn[16];
	mat4_place(place, x, y + pivot_y, z, 0.0f);
	mat4_identity(tilt);
	tilt[5] = c; tilt[6] = s;
	tilt[9] = -s; tilt[10] = c;
	mat4_place(down, 0.0f, -pivot_y, 0.0f, 0.0f);
	mat4_place(turn, 0.0f, 0.0f, 0.0f, yaw_degrees);
	mat4_multiply(m, place, tilt);
	mat4_multiply(m, m, down);
	mat4_multiply(m, m, turn);
}

void mat4_place(float m[16], float x, float y, float z, float yaw_degrees)
{
	const float a = yaw_degrees * (float)M_PI / 180.0f, c = cosf(a), s = sinf(a);
	mat4_identity(m);
	m[0] = c; m[2] = -s;
	m[8] = s; m[10] = c;
	m[12] = x; m[13] = y; m[14] = z;
}

bool mat4_project(const float vp[16], const float p[3], float vx, float vy, float vw, float vh, float *x, float *y)
{
	const float cx = vp[0] * p[0] + vp[4] * p[1] + vp[8] * p[2] + vp[12];
	const float cy = vp[1] * p[0] + vp[5] * p[1] + vp[9] * p[2] + vp[13];
	const float cw = vp[3] * p[0] + vp[7] * p[1] + vp[11] * p[2] + vp[15];
	if (cw <= 0.0f)
		return false;
	*x = vx + (cx / cw * 0.5f + 0.5f) * vw;
	*y = vy + (0.5f - cy / cw * 0.5f) * vh;
	return true;
}

/* ---- mesh ---- */

namespace {

/* Side profile, (z toward the player, y up), from the floor at the front up
 * the front and down the back. */
struct ProfilePoint { float z, y; };
const ProfilePoint kProfile[] = {
	{ 0.50f, 0.00f }, /* A floor, front */
	{ 0.50f, 0.92f }, /* B kick panel top */
	{ 0.80f, 0.98f }, /* C control panel underside, front */
	{ 0.80f, 1.06f }, /* D control panel front edge */
	{ 0.40f, 1.15f }, /* E control panel back */
	{ 0.36f, 1.17f }, /* F screen bottom */
	{ 0.15f, 1.66f }, /* G screen top (leaning back) */
	{ 0.34f, 1.73f }, /* H marquee bottom (overhang; the face is 3.5:1, as MAME marquees) */
	{ 0.34f, 1.92f }, /* I marquee top */
	{ -0.42f, 1.92f }, /* J top, back */
	{ -0.42f, 0.00f }, /* K floor, back */
};
enum { PROFILE_POINTS = sizeof(kProfile) / sizeof(kProfile[0]) };
/* What each front segment (point i to i + 1) shows. */
enum Surface { BODY, DARK, TRIM, PANEL, KICK, SCREEN, MARQUEE, SURFACES };
const Surface kSegment[PROFILE_POINTS - 1] = { KICK, DARK, TRIM, PANEL, DARK, SCREEN, DARK, MARQUEE, BODY, BODY };
const float kSide = 0.035f; /* side panel thickness */

std::vector<UiVertex3D> s_surface[SURFACES];
std::vector<UiVertex3D> s_sides, s_molding, s_shadow;
/* Controls on the panel: glossy plastic (balls, buttons), black parts,
 * metal shafts. */
std::vector<UiVertex3D> s_glossy, s_black, s_metal;
UiTexture s_panel, s_shadow_texture, s_grain;

void vertex(std::vector<UiVertex3D> &out, float x, float y, float z, const float n[3], float u, float v, ImU32 color)
{
	UiVertex3D vtx = { x, y, z, n[0], n[1], n[2], u, v, color };
	out.push_back(vtx);
}

void quad(std::vector<UiVertex3D> &out, const float p[4][3], const float n[3], ImU32 color, const float uv[4][2])
{
	static const int order[6] = { 0, 1, 2, 0, 2, 3 };
	for (int i : order)
		vertex(out, p[i][0], p[i][1], p[i][2], n, uv[i][0], uv[i][1], color);
}

/* Ear clipping of the side profile (a simple polygon, counter-clockwise in
 * the (z, y) plane when seen from +x). */
std::vector<int> triangulate_profile()
{
	std::vector<int> remaining, triangles;
	for (int i = 0; i < PROFILE_POINTS; i++)
		remaining.push_back(i);
	auto cross = [](const ProfilePoint &a, const ProfilePoint &b, const ProfilePoint &c) {
		return (b.z - a.z) * (c.y - a.y) - (b.y - a.y) * (c.z - a.z);
	};
	float area = 0.0f;
	for (int i = 0; i < PROFILE_POINTS; i++) {
		const ProfilePoint &a = kProfile[i], &b = kProfile[(i + 1) % PROFILE_POINTS];
		area += a.z * b.y - b.z * a.y;
	}
	const float sign = area > 0.0f ? 1.0f : -1.0f;
	while (remaining.size() > 3) {
		bool clipped = false;
		for (size_t i = 0; i < remaining.size() && !clipped; i++) {
			const int ia = remaining[(i + remaining.size() - 1) % remaining.size()];
			const int ib = remaining[i], ic = remaining[(i + 1) % remaining.size()];
			const ProfilePoint &a = kProfile[ia], &b = kProfile[ib], &c = kProfile[ic];
			if (cross(a, b, c) * sign <= 0.0f)
				continue;
			bool inside = false;
			for (int j : remaining) {
				if (j == ia || j == ib || j == ic)
					continue;
				const ProfilePoint &p = kProfile[j];
				if (cross(a, b, p) * sign > 0 && cross(b, c, p) * sign > 0 && cross(c, a, p) * sign > 0)
					inside = true;
			}
			if (inside)
				continue;
			triangles.insert(triangles.end(), { ia, ib, ic });
			remaining.erase(remaining.begin() + i);
			clipped = true;
		}
		if (!clipped)
			break;
	}
	if (remaining.size() == 3)
		triangles.insert(triangles.end(), { remaining[0], remaining[1], remaining[2] });
	return triangles;
}

ImU32 surface_color(Surface surface)
{
	switch (surface) {
	case DARK: return IM_COL32(18, 18, 24, 255);
	case TRIM: return IM_COL32(40, 40, 52, 255);
	case BODY: return IM_COL32(150, 150, 150, 255); /* tinted by the body colour */
	default: return IM_COL32(255, 255, 255, 255);
	}
}

/* A point on the control panel: x across, t from its front edge (0) to its
 * back (1), h above its surface. */
struct PanelFrame {
	float origin[3], along[3], normal[3];
	void at(float x, float t, float h, float out[3]) const
	{
		for (int i = 0; i < 3; i++)
			out[i] = origin[i] + along[i] * t + normal[i] * h;
		out[0] += x;
	}
};

PanelFrame panel_frame()
{
	/* Segment D -> E of the profile is the panel top. */
	const ProfilePoint &d = kProfile[3], &e = kProfile[4];
	PanelFrame f;
	f.origin[0] = 0.0f; f.origin[1] = d.y; f.origin[2] = d.z;
	f.along[0] = 0.0f; f.along[1] = e.y - d.y; f.along[2] = e.z - d.z;
	f.normal[0] = 0.0f; f.normal[1] = -(e.z - d.z); f.normal[2] = e.y - d.y;
	normalize3(f.normal);
	return f;
}

/* A cylinder standing on the panel at base, along the panel normal, with
 * its top cap; caps can also be a dome (sphere_top). */
void cylinder(std::vector<UiVertex3D> &out, const PanelFrame &f, const float base[3], float radius,
	float height, ImU32 side_color, ImU32 top_color)
{
	enum { SEGMENTS = 18 };
	/* A frame around the panel normal: x and the panel's "along" direction. */
	float u[3] = { 1.0f, 0.0f, 0.0f }, w[3];
	w[0] = f.normal[1] * u[2] - f.normal[2] * u[1];
	w[1] = f.normal[2] * u[0] - f.normal[0] * u[2];
	w[2] = f.normal[0] * u[1] - f.normal[1] * u[0];
	normalize3(w);
	for (int i = 0; i < SEGMENTS; i++) {
		const float a0 = 2.0f * (float)M_PI * i / SEGMENTS, a1 = 2.0f * (float)M_PI * (i + 1) / SEGMENTS;
		float r0[3], r1[3], p[4][3], top[3], t0[3], t1[3];
		for (int k = 0; k < 3; k++) {
			r0[k] = u[k] * cosf(a0) + w[k] * sinf(a0);
			r1[k] = u[k] * cosf(a1) + w[k] * sinf(a1);
			p[0][k] = base[k] + r0[k] * radius;
			p[1][k] = base[k] + r1[k] * radius;
			p[2][k] = p[1][k] + f.normal[k] * height;
			p[3][k] = p[0][k] + f.normal[k] * height;
			top[k] = base[k] + f.normal[k] * height;
			t0[k] = p[3][k];
			t1[k] = p[2][k];
		}
		float n0[3] = { r0[0], r0[1], r0[2] }, n1[3] = { r1[0], r1[1], r1[2] };
		vertex(out, p[0][0], p[0][1], p[0][2], n0, 0, 0, side_color);
		vertex(out, p[1][0], p[1][1], p[1][2], n1, 0, 0, side_color);
		vertex(out, p[2][0], p[2][1], p[2][2], n1, 0, 0, side_color);
		vertex(out, p[0][0], p[0][1], p[0][2], n0, 0, 0, side_color);
		vertex(out, p[2][0], p[2][1], p[2][2], n1, 0, 0, side_color);
		vertex(out, p[3][0], p[3][1], p[3][2], n0, 0, 0, side_color);
		vertex(out, top[0], top[1], top[2], f.normal, 0, 0, top_color);
		vertex(out, t0[0], t0[1], t0[2], f.normal, 0, 0, top_color);
		vertex(out, t1[0], t1[1], t1[2], f.normal, 0, 0, top_color);
	}
}

void sphere(std::vector<UiVertex3D> &out, const float center[3], float radius, ImU32 color)
{
	enum { RINGS = 10, SEGMENTS = 18 };
	for (int r = 0; r < RINGS; r++)
		for (int i = 0; i < SEGMENTS; i++) {
			float n[4][3], p[4][3];
			const int corners[4][2] = { { r, i }, { r, i + 1 }, { r + 1, i + 1 }, { r + 1, i } };
			for (int c = 0; c < 4; c++) {
				const float lat = (float)M_PI * corners[c][0] / RINGS - (float)M_PI * 0.5f;
				const float lon = 2.0f * (float)M_PI * corners[c][1] / SEGMENTS;
				n[c][0] = cosf(lat) * cosf(lon);
				n[c][1] = sinf(lat);
				n[c][2] = cosf(lat) * sinf(lon);
				for (int k = 0; k < 3; k++)
					p[c][k] = center[k] + n[c][k] * radius;
			}
			static const int order[6] = { 0, 1, 2, 0, 2, 3 };
			for (int k : order)
				vertex(out, p[k][0], p[k][1], p[k][2], n[k], 0, 0, color);
		}
}

/* Two players: a stick and four buttons each, Namco style. */
void build_controls()
{
	static const ImU32 buttons[4] = { IM_COL32(240, 200, 30, 255), IM_COL32(40, 110, 240, 255),
		IM_COL32(40, 190, 80, 255), IM_COL32(230, 50, 60, 255) };
	static const float layout[4][2] = { { 0.030f, 0.64f }, { 0.074f, 0.68f }, { 0.036f, 0.34f }, { 0.080f, 0.38f } };
	const PanelFrame f = panel_frame();
	for (int player = 0; player < 2; player++) {
		const float x0 = player ? 0.155f : -0.155f;
		float base[3], top[3];
		/* Stick: a dust washer, a metal shaft and the ball. */
		f.at(x0 - 0.075f, 0.5f, 0.0f, base);
		cylinder(s_black, f, base, 0.030f, 0.005f, IM_COL32(14, 14, 16, 255), IM_COL32(24, 24, 28, 255));
		cylinder(s_metal, f, base, 0.0065f, 0.066f, IM_COL32(170, 172, 180, 255), IM_COL32(170, 172, 180, 255));
		f.at(x0 - 0.075f, 0.5f, 0.088f, top);
		sphere(s_glossy, top, 0.027f, IM_COL32(205, 28, 36, 255));
		/* Buttons: a black ring and a coloured plunger. */
		for (int i = 0; i < 4; i++) {
			f.at(x0 + layout[i][0] - 0.02f, layout[i][1], 0.0f, base);
			cylinder(s_black, f, base, 0.0215f, 0.006f, IM_COL32(12, 12, 14, 255), IM_COL32(20, 20, 24, 255));
			cylinder(s_glossy, f, base, 0.0170f, 0.016f, buttons[i], buttons[i]);
		}
	}
}

/* Laminate grain: soft streaks along the panel with fine speckle. */
void make_grain(UiTexture *texture)
{
	enum { SIZE = 128 };
	std::vector<unsigned char> pixels(SIZE * SIZE * 4);
	unsigned seed = 12345u;
	float row_noise[SIZE];
	for (int y = 0; y < SIZE; y++) {
		seed = seed * 1103515245u + 12345u;
		row_noise[y] = (float)((seed >> 16) & 0xff) / 255.0f;
	}
	for (int y = 0; y < SIZE; y++)
		for (int x = 0; x < SIZE; x++) {
			seed = seed * 1103515245u + 12345u;
			const float speckle = (float)((seed >> 16) & 0xff) / 255.0f;
			const float streak = 0.5f + 0.5f * sinf((float)x * 0.21f + row_noise[(y / 3) % SIZE] * 6.0f);
			const float value = 0.84f + 0.07f * streak + 0.06f * speckle;
			const unsigned char v = (unsigned char)(value > 1.0f ? 255 : value * 255.0f);
			unsigned char *p = &pixels[(y * SIZE + x) * 4];
			p[0] = p[1] = p[2] = v;
			p[3] = 255;
		}
	ui_gl_texture_rgba(texture, SIZE, SIZE, pixels.data(), true);
}

void build_mesh()
{
	const float half = CABINET3D_WIDTH * 0.5f, inner = half - kSide;
	for (int i = 0; i + 1 < PROFILE_POINTS; i++) {
		const ProfilePoint &a = kProfile[i], &b = kProfile[i + 1];
		/* The contour runs up the front and down the back: (dy, -dz) points out. */
		float n[3] = { 0.0f, -(b.z - a.z), b.y - a.y };
		normalize3(n);
		const float p[4][3] = { { -inner, a.y, a.z }, { inner, a.y, a.z }, { inner, b.y, b.z }, { -inner, b.y, b.z } };
		const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
		quad(s_surface[kSegment[i]], p, n, surface_color(kSegment[i]), uv);
		/* T-molding: the edge of each side panel along the contour. */
		for (int side = -1; side <= 1; side += 2) {
			const float x0 = side * inner, x1 = side * half;
			const float e[4][3] = { { x0, a.y, a.z }, { x1, a.y, a.z }, { x1, b.y, b.z }, { x0, b.y, b.z } };
			quad(s_molding, e, n, IM_COL32(255, 255, 255, 255), uv);
		}
	}
	/* Side panels: the profile at x = +-half, darker toward the floor; the
	 * body colour tints them. */
	const std::vector<int> triangles = triangulate_profile();
	for (int side = -1; side <= 1; side += 2) {
		const float n[3] = { (float)side, 0.0f, 0.0f };
		for (size_t t = 0; t < triangles.size(); t += 3)
			for (int k = 0; k < 3; k++) {
				const int index = triangles[t + (side > 0 ? k : 2 - k)];
				const ProfilePoint &p = kProfile[index];
				const int shade = (int)(255.0f * (0.55f + 0.45f * (p.y / CABINET3D_HEIGHT)));
				/* Grain runs along the panel: tile the texture over it. */
				vertex(s_sides, side * half, p.y, p.z, n, (p.z + 0.42f) * 1.6f, p.y * 1.1f,
					IM_COL32(shade, shade, shade, 255));
			}
	}
	build_controls();
	/* Floor shadow: a quad under the cabinet with a radial texture. */
	const float up[3] = { 0, 1, 0 };
	const float shadow[4][3] = { { -0.75f, 0.002f, 1.05f }, { 0.75f, 0.002f, 1.05f },
		{ 0.75f, 0.002f, -0.75f }, { -0.75f, 0.002f, -0.75f } };
	const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
	quad(s_shadow, shadow, up, IM_COL32(255, 255, 255, 255), uv);
}

/* Control panel top: a dark brushed finish with a player divider; the
 * sticks and buttons are geometry. */
void draw_panel(ImDrawList *draw, void *)
{
	const float W = 512, H = 160;
	draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), IM_COL32(64, 64, 72, 255), IM_COL32(64, 64, 72, 255),
		IM_COL32(36, 36, 42, 255), IM_COL32(36, 36, 42, 255));
	unsigned seed = 99u;
	for (int i = 0; i < 260; i++) {
		seed = seed * 1103515245u + 12345u;
		const float y = (float)((seed >> 8) % 160);
		const int a = 6 + (int)((seed >> 20) % 14);
		draw->AddLine(ImVec2(0, y), ImVec2(W, y), IM_COL32(255, 255, 255, a), 1.0f);
	}
	draw->AddLine(ImVec2(0, 2), ImVec2(W, 2), IM_COL32(255, 255, 255, 60), 3.0f);
	draw->AddLine(ImVec2(W * 0.5f, 12), ImVec2(W * 0.5f, H - 12), IM_COL32(0, 0, 0, 90), 3.0f);
	/* Outside an ImGui frame there is no current font: name one. */
	ImFont *font = ImGui::GetIO().Fonts->Fonts[0];
	draw->AddText(font, 18.0f, ImVec2(20, H - 28), IM_COL32(255, 255, 255, 70), "1P");
	draw->AddText(font, 18.0f, ImVec2(W - 44, H - 28), IM_COL32(255, 255, 255, 70), "2P");
}

/* Kick panel: body colour, a coin door with two lit slots. */
void draw_kick(ImDrawList *draw, void *opaque)
{
	const unsigned *body = (const unsigned *)opaque;
	const float W = 512, H = 512;
	draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), body[0], body[0], body[1], body[1]);
	const ImVec2 d0(176, 150), d1(336, 360);
	draw->AddRectFilled(ImVec2(d0.x - 6, d0.y - 6), ImVec2(d1.x + 6, d1.y + 6), IM_COL32(120, 120, 130, 255), 6);
	draw->AddRectFilled(d0, d1, IM_COL32(18, 18, 22, 255), 4);
	for (int i = 0; i < 2; i++) {
		const float x = 222.0f + i * 68.0f;
		draw->AddRectFilled(ImVec2(x - 14, 190), ImVec2(x + 14, 250), IM_COL32(255, 120, 20, 255), 4);
		draw->AddRectFilled(ImVec2(x - 4, 198), ImVec2(x + 4, 242), IM_COL32(60, 20, 0, 255), 2);
		draw->AddRectFilled(ImVec2(x - 14, 280), ImVec2(x + 14, 300), IM_COL32(60, 60, 70, 255), 3);
	}
}

void draw_shadow(ImDrawList *draw, void *)
{
	for (int i = 0; i < 24; i++) {
		const float k = (float)i / 24.0f;
		draw->AddCircleFilled(ImVec2(64, 64), 62.0f * (1.0f - k), IM_COL32(0, 0, 0, (int)(14 + 6 * k)), 48);
	}
}

} // namespace

bool cabinet3d_init(void)
{
	build_mesh();
	make_grain(&s_grain);
	return ui_gl_render_to_texture(&s_panel, 512, 160, draw_panel, NULL) &&
		ui_gl_render_to_texture(&s_shadow_texture, 128, 128, draw_shadow, NULL);
}

unsigned cabinet3d_kick_texture(unsigned body_top, unsigned body_bottom)
{
	unsigned body[2] = { body_top, body_bottom };
	UiTexture texture = {};
	ui_gl_render_to_texture(&texture, 512, 512, draw_kick, body);
	return texture.id;
}

void cabinet3d_shutdown(void)
{
	ui_gl_free_texture(&s_panel);
	ui_gl_free_texture(&s_shadow_texture);
	ui_gl_free_texture(&s_grain);
	s_glossy.clear();
	s_black.clear();
	s_metal.clear();
	for (auto &surface : s_surface)
		surface.clear();
	s_sides.clear();
	s_molding.clear();
	s_shadow.clear();
}

void cabinet3d_draw(const float model[16], const CabinetLook *look)
{
	const unsigned white = ui_gl_white_texture();
	const unsigned plain = IM_COL32(255, 255, 255, 255);
	/* Laminate sides, glossy T-molding. */
	UiMaterial laminate = { look->body, 0.18f, 24.0f, 0 };
	UiMaterial molding = { look->molding, 0.30f, 40.0f, 0 };
	ui_gl3d_draw(s_sides.data(), (int)s_sides.size(), s_grain.id, model, look->light, &laminate);
	ui_gl3d_draw(s_molding.data(), (int)s_molding.size(), white, model, look->light, &molding);
	for (int surface = 0; surface < SURFACES; surface++) {
		const std::vector<UiVertex3D> &mesh = s_surface[surface];
		unsigned texture = white;
		float light = look->light;
		UiMaterial material = { plain, 0.25f, 30.0f, 0 };
		if (mesh.empty())
			continue;
		switch (surface) {
		case PANEL: texture = s_panel.id; material.specular = 0.35f; material.shininess = 48.0f; break;
		case KICK: texture = look->kick; material.specular = 0.15f; material.shininess = 20.0f; break;
		case BODY: texture = s_grain.id; material.tint = look->body; material.specular = 0.15f; break;
		/* Glass and plexiglass: lit from inside, with a sharp highlight. */
		case SCREEN: texture = look->screen; material.flags |= UI3D_EMISSIVE;
			material.specular = 0.55f; material.shininess = 90.0f; break;
		case MARQUEE: texture = look->marquee; material.flags |= UI3D_EMISSIVE;
			material.specular = 0.30f; material.shininess = 60.0f; light *= look->marquee_light; break;
		default: break;
		}
		ui_gl3d_draw(mesh.data(), (int)mesh.size(), texture ? texture : white, model, light, &material);
	}
	UiMaterial plastic = { plain, 0.75f, 60.0f, 0 };
	UiMaterial black = { plain, 0.30f, 32.0f, 0 };
	UiMaterial metal = { plain, 0.90f, 90.0f, 0 };
	ui_gl3d_draw(s_black.data(), (int)s_black.size(), white, model, look->light, &black);
	ui_gl3d_draw(s_metal.data(), (int)s_metal.size(), white, model, look->light, &metal);
	ui_gl3d_draw(s_glossy.data(), (int)s_glossy.size(), white, model, look->light, &plastic);
}

void cabinet3d_shadow(const float model[16], float strength)
{
	UiMaterial shadow = { IM_COL32(255, 255, 255, 255), 0.0f, 1.0f, UI3D_EMISSIVE | UI3D_BLEND };
	ui_gl3d_draw(s_shadow.data(), (int)s_shadow.size(), s_shadow_texture.id, model, strength, &shadow);
}
