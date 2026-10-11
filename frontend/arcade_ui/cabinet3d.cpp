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
	{ 0.34f, 1.68f }, /* H marquee bottom (overhang) */
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
std::vector<UiVertex3D> s_sides, s_shadow;
UiTexture s_panel, s_kick, s_shadow_texture;

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
	case BODY: return IM_COL32(30, 32, 70, 255);
	default: return IM_COL32(255, 255, 255, 255);
	}
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
			quad(s_sides, e, n, IM_COL32(230, 70, 60, 255), uv);
		}
	}
	/* Side panels: the profile at x = +-half, shaded darker toward the floor. */
	const std::vector<int> triangles = triangulate_profile();
	for (int side = -1; side <= 1; side += 2) {
		const float n[3] = { (float)side, 0.0f, 0.0f };
		for (size_t t = 0; t < triangles.size(); t += 3)
			for (int k = 0; k < 3; k++) {
				const int index = triangles[t + (side > 0 ? k : 2 - k)];
				const ProfilePoint &p = kProfile[index];
				const float shade = 0.55f + 0.45f * (p.y / CABINET3D_HEIGHT);
				vertex(s_sides, side * half, p.y, p.z, n, 0, 0,
					IM_COL32((int)(52 * shade), (int)(58 * shade), (int)(128 * shade), 255));
			}
	}
	/* Floor shadow: a quad under the cabinet with a radial texture. */
	const float up[3] = { 0, 1, 0 };
	const float shadow[4][3] = { { -0.75f, 0.002f, 1.05f }, { 0.75f, 0.002f, 1.05f },
		{ 0.75f, 0.002f, -0.75f }, { -0.75f, 0.002f, -0.75f } };
	const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
	quad(s_shadow, shadow, up, IM_COL32(255, 255, 255, 255), uv);
}

/* Control panel seen from above: a stick and four buttons, with highlights. */
void draw_panel(ImDrawList *draw, void *)
{
	const float W = 512, H = 160;
	draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), IM_COL32(58, 58, 66, 255), IM_COL32(58, 58, 66, 255),
		IM_COL32(34, 34, 40, 255), IM_COL32(34, 34, 40, 255));
	draw->AddLine(ImVec2(0, 2), ImVec2(W, 2), IM_COL32(255, 255, 255, 50), 3.0f);
	/* Stick: a dust washer, the shaft seen from above, and the ball. */
	const ImVec2 stick(150, 82);
	draw->AddCircleFilled(stick, 30, IM_COL32(20, 20, 24, 255), 32);
	draw->AddCircleFilled(ImVec2(stick.x, stick.y - 4), 24, IM_COL32(200, 30, 40, 255), 32);
	draw->AddCircleFilled(ImVec2(stick.x - 7, stick.y - 12), 8, IM_COL32(255, 140, 140, 200), 16);
	static const ImU32 colors[4] = { IM_COL32(255, 196, 0, 255), IM_COL32(40, 120, 255, 255),
		IM_COL32(40, 200, 90, 255), IM_COL32(240, 60, 70, 255) };
	for (int i = 0; i < 4; i++) {
		const ImVec2 c(290.0f + (i % 2) * 62.0f + (i / 2) * 18.0f, 58.0f + (i / 2) * 50.0f - (i % 2) * 8.0f);
		draw->AddCircleFilled(c, 21, IM_COL32(16, 16, 20, 255), 28);
		draw->AddCircleFilled(c, 17, colors[i], 28);
		draw->AddCircleFilled(ImVec2(c.x - 5, c.y - 6), 6, IM_COL32(255, 255, 255, 110), 16);
	}
}

/* Kick panel: body colour, a coin door with two lit slots. */
void draw_kick(ImDrawList *draw, void *)
{
	const float W = 512, H = 512;
	draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), IM_COL32(44, 48, 104, 255), IM_COL32(44, 48, 104, 255),
		IM_COL32(24, 26, 60, 255), IM_COL32(24, 26, 60, 255));
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
	return ui_gl_render_to_texture(&s_panel, 512, 160, draw_panel, NULL) &&
		ui_gl_render_to_texture(&s_kick, 512, 512, draw_kick, NULL) &&
		ui_gl_render_to_texture(&s_shadow_texture, 128, 128, draw_shadow, NULL);
}

void cabinet3d_shutdown(void)
{
	ui_gl_free_texture(&s_panel);
	ui_gl_free_texture(&s_kick);
	ui_gl_free_texture(&s_shadow_texture);
	for (auto &surface : s_surface)
		surface.clear();
	s_sides.clear();
	s_shadow.clear();
}

void cabinet3d_draw(const float model[16], const CabinetLook *look)
{
	const unsigned white = ui_gl_white_texture();
	ui_gl3d_draw(s_sides.data(), (int)s_sides.size(), white, model, look->light, 0);
	for (int surface = 0; surface < SURFACES; surface++) {
		const std::vector<UiVertex3D> &mesh = s_surface[surface];
		unsigned texture = white;
		int flags = 0;
		if (mesh.empty())
			continue;
		switch (surface) {
		case PANEL: texture = s_panel.id; break;
		case KICK: texture = s_kick.id; break;
		case SCREEN: texture = look->screen; flags = UI3D_EMISSIVE; break;
		case MARQUEE: texture = look->marquee; flags = UI3D_EMISSIVE; break;
		default: break;
		}
		ui_gl3d_draw(mesh.data(), (int)mesh.size(), texture ? texture : white, model, look->light, flags);
	}
}

void cabinet3d_shadow(const float model[16], float strength)
{
	ui_gl3d_draw(s_shadow.data(), (int)s_shadow.size(), s_shadow_texture.id, model, strength,
		UI3D_EMISSIVE | UI3D_BLEND);
}
