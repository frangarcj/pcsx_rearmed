#include "arcade_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

#include <imgui.h>

#include "ui_gl.h"
#include "cabinet3d.h"
#include "../../libpcsxcore/arcade/system12_romset.h"
#include "roboto_medium.inc" /* Roboto_Medium_ttf, from deps/imgui/misc/fonts */

namespace {

/* ---- catalog: games (MAME parents) and their versions (sets) ---- */

struct Version {
	std::string set, label; /* soulclbrac, "Asia, SOC14/VER.C" */
	int year;
};

struct Game {
	std::string key, title, board; /* soulclbr, "Soul Calibur", "Namco System 12" */
	std::vector<Version> versions;
	int selected;                   /* version shown */
};

struct Art {
	std::string set;
	UiTexture snap, marquee;
	UiTexture marquee3d, screen3d; /* the 3D cabinet's lit surfaces */
	bool tried;
	unsigned last_used;
};

struct State {
	ArcadeUiConfig config;
	std::string rompath, data_root;
	std::vector<Game> games;
	int game;          /* selected game */
	float scroll;      /* carousel position, eases toward game */
	unsigned previous; /* buttons last frame */
	float repeat_time;
	unsigned repeat_button;
	std::vector<Art> art;
	std::vector<std::pair<std::string, unsigned> > kick; /* kick panel per board */
	unsigned frame;
	float time;               /* seconds, for the idle animation */
	float yaw, pitch;         /* right stick turn of the selected cabinet */
	int posed_game;           /* the game yaw/pitch belong to */
	ImFont *font_small, *font_body, *font_title, *font_marquee;
};

State *s;

/* Cabinet colours per board family. */
struct BoardStyle {
	const char *board;
	ImU32 body, molding, kick_top, kick_bottom, marquee_a, marquee_b;
};
const BoardStyle kBoardStyles[] = {
	{ "Namco System 12", IM_COL32(64, 72, 165, 255), IM_COL32(232, 62, 52, 255), IM_COL32(44, 48, 110, 255),
	  IM_COL32(22, 24, 60, 255), IM_COL32(240, 70, 80, 255), IM_COL32(130, 50, 200, 255) },
	{ "Namco System 11", IM_COL32(160, 36, 46, 255), IM_COL32(28, 28, 32, 255), IM_COL32(120, 26, 36, 255),
	  IM_COL32(60, 12, 18, 255), IM_COL32(250, 120, 40, 255), IM_COL32(200, 30, 50, 255) },
	{ "Namco System 10", IM_COL32(30, 130, 140, 255), IM_COL32(200, 205, 215, 255), IM_COL32(24, 96, 104, 255),
	  IM_COL32(10, 44, 50, 255), IM_COL32(40, 200, 200, 255), IM_COL32(30, 90, 170, 255) },
	{ "Capcom ZN", IM_COL32(44, 44, 50, 255), IM_COL32(250, 196, 30, 255), IM_COL32(36, 36, 42, 255),
	  IM_COL32(16, 16, 20, 255), IM_COL32(250, 200, 40, 255), IM_COL32(220, 70, 30, 255) },
	{ "Taito FX-1", IM_COL32(40, 96, 190, 255), IM_COL32(235, 235, 240, 255), IM_COL32(30, 72, 150, 255),
	  IM_COL32(12, 30, 70, 255), IM_COL32(60, 160, 255, 255), IM_COL32(30, 60, 180, 255) },
	{ "Konami GV", IM_COL32(34, 120, 76, 255), IM_COL32(245, 130, 30, 255), IM_COL32(26, 92, 58, 255),
	  IM_COL32(10, 40, 24, 255), IM_COL32(250, 150, 40, 255), IM_COL32(40, 160, 90, 255) },
	{ "Taito G-Net", IM_COL32(110, 44, 140, 255), IM_COL32(200, 200, 210, 255), IM_COL32(84, 32, 110, 255),
	  IM_COL32(36, 12, 50, 255), IM_COL32(200, 80, 240, 255), IM_COL32(90, 40, 180, 255) },
};

const BoardStyle &style_for(const std::string &board)
{
	for (const BoardStyle &style : kBoardStyles)
		if (board == style.board)
			return style;
	return kBoardStyles[0];
}

/* "Soul Calibur (Asia, SOC14/VER.C)" -> "Soul Calibur" and "Asia, SOC14/VER.C". */
void split_description(const char *description, std::string *title, std::string *label)
{
	const char *open = strchr(description, '(');
	if (!open) {
		*title = description;
		label->clear();
		return;
	}
	const char *close = strrchr(open, ')');
	std::string head(description, open);
	while (!head.empty() && head.back() == ' ')
		head.pop_back();
	*title = head;
	*label = std::string(open + 1, close ? close : open + strlen(open));
}

std::string config_path()
{
	return s->data_root + "/arcade_ui.cfg";
}

/* arcade_ui.cfg: "game=<key>" for the last game, "<key>=<set>" per game. */
void load_config(std::string *last_game)
{
	FILE *f = fopen(config_path().c_str(), "r");
	char line[256];
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		size_t length = strcspn(line, "\r\n");
		line[length] = 0;
		if (!eq)
			continue;
		*eq = 0;
		if (!strcmp(line, "game")) {
			*last_game = eq + 1;
			continue;
		}
		for (Game &game : s->games)
			if (game.key == line)
				for (size_t v = 0; v < game.versions.size(); v++)
					if (game.versions[v].set == eq + 1)
						game.selected = (int)v;
	}
	fclose(f);
}

void save_config()
{
	FILE *f = fopen(config_path().c_str(), "w");
	if (!f)
		return;
	if (!s->games.empty())
		fprintf(f, "game=%s\n", s->games[s->game].key.c_str());
	for (const Game &game : s->games)
		fprintf(f, "%s=%s\n", game.key.c_str(), game.versions[game.selected].set.c_str());
	fclose(f);
}

void build_catalog()
{
	const unsigned count = system12_romset_count();
	std::vector<unsigned char> available(count, 0);
	std::string last_game;
	s->games.clear();
	if (s->config.demo) {
		for (unsigned i = 0; i < count; i++)
			available[i] = 1;
	} else if (system12_romset_scan(s->rompath.c_str(), available.data()) < 0) {
		return;
	}
	for (unsigned i = 0; i < count; i++) {
		const system12_romset_info *info = system12_romset_at(i);
		if (!available[i] || strcmp(info->unsupported, "none"))
			continue;
		const std::string key = info->parent[0] ? info->parent : info->name;
		std::string title, label;
		split_description(info->description, &title, &label);
		auto game = std::find_if(s->games.begin(), s->games.end(),
			[&](const Game &g) { return g.key == key; });
		if (game == s->games.end()) {
			s->games.push_back(Game{key, title, "Namco System 12", {}, 0});
			game = s->games.end() - 1;
		}
		Version version{info->name, label, info->year};
		/* The parent first: its title names the game. */
		if (!info->parent[0]) {
			game->title = title;
			game->versions.insert(game->versions.begin(), version);
		} else {
			game->versions.push_back(version);
		}
	}
	std::sort(s->games.begin(), s->games.end(),
		[](const Game &a, const Game &b) { return a.title < b.title; });
	/* Palette preview: every board family in turn (demo 2 only). */
	if (s->config.demo == 2)
		for (size_t i = 0; i < s->games.size(); i++)
			s->games[i].board = kBoardStyles[i % (sizeof(kBoardStyles) / sizeof(kBoardStyles[0]))].board;
	s->game = 0;
	load_config(&last_game);
	for (size_t i = 0; i < s->games.size(); i++)
		if (s->games[i].key == last_game)
			s->game = (int)i;
	s->scroll = (float)s->game;
}

/* ---- art: snap/, titles/ and marquees/<set>.png, else the parent's ---- */

bool load_art_file(const char *dir, const std::string &set, const std::string &parent, UiTexture *texture)
{
	const std::string names[2] = { set, parent };
	for (const std::string &name : names) {
		if (name.empty())
			continue;
		const std::string path = s->data_root + "/" + dir + "/" + name + ".png";
		if (ui_gl_load_png(path.c_str(), texture))
			return true;
	}
	return false;
}

Art *art_for(const Game &game)
{
	const std::string &set = game.versions[game.selected].set;
	for (Art &art : s->art)
		if (art.set == set) {
			art.last_used = s->frame;
			return &art;
		}
	if (s->art.size() >= 12) {
		auto oldest = std::min_element(s->art.begin(), s->art.end(),
			[](const Art &a, const Art &b) { return a.last_used < b.last_used; });
		ui_gl_free_texture(&oldest->snap);
		ui_gl_free_texture(&oldest->marquee);
		ui_gl_free_texture(&oldest->marquee3d);
		ui_gl_free_texture(&oldest->screen3d);
		s->art.erase(oldest);
	}
	Art art{};
	art.set = set;
	art.last_used = s->frame;
	if (!load_art_file("snap", set, game.key, &art.snap))
		load_art_file("titles", set, game.key, &art.snap);
	load_art_file("marquees", set, game.key, &art.marquee);
	s->art.push_back(art);
	return &s->art.back();
}

/* ---- drawing ---- */

ImU32 rgba(int r, int g, int b, float a = 1.0f)
{
	return IM_COL32(r, g, b, (int)(a * 255.0f));
}

/* Moves a colour toward the background: light 1 keeps it, 0 is the background. */
ImU32 fade(ImU32 color, float light)
{
	static const int background[3] = { 15, 18, 36 };
	int channel[3];
	for (int i = 0; i < 3; i++) {
		const int value = (color >> (8 * i)) & 0xff;
		channel[i] = background[i] + (int)((value - background[i]) * light);
	}
	return IM_COL32(channel[0], channel[1], channel[2], (color >> IM_COL32_A_SHIFT) & 0xff);
}

/* Draws text centred in [x0, x1] at y, shrunk to fit. */
void centred_text(ImDrawList *draw, ImFont *font, float size, float x0, float x1, float y, ImU32 color, const char *text)
{
	ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
	if (extent.x > x1 - x0) {
		size *= (x1 - x0) / extent.x;
		extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
	}
	draw->AddText(font, size, ImVec2(floorf((x0 + x1 - extent.x) * 0.5f), floorf(y - extent.y * 0.5f)), color, text);
}

void image_fit(ImDrawList *draw, const UiTexture &texture, ImVec2 a, ImVec2 b, ImU32 tint)
{
	const float box_w = b.x - a.x, box_h = b.y - a.y;
	const float scale = std::min(box_w / texture.width, box_h / texture.height);
	const float w = texture.width * scale, h = texture.height * scale;
	const ImVec2 p(a.x + (box_w - w) * 0.5f, a.y + (box_h - h) * 0.5f);
	draw->AddImage((ImTextureID)(uintptr_t)texture.id, p, ImVec2(p.x + w, p.y + h), ImVec2(0, 0), ImVec2(1, 1), tint);
}

/* A cabinet front, centred at cx, with its top at y; s scales it. */
/* light: 1 for the selected cabinet, lower for its neighbours. */
void draw_cabinet(ImDrawList *draw, const Game &game, float cx, float y, float scale, float alpha, bool focused)
{
	const float w = 236.0f * scale, h = 392.0f * scale;
	const float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
	const Version &version = game.versions[game.selected];
	Art *art = art_for(game);

	/* Body and side panels. */
	draw->AddRectFilled(ImVec2(x0 - 6 * scale, y + 4 * scale), ImVec2(x1 + 6 * scale, y + h),
		fade(rgba(22, 24, 52), alpha), 6.0f * scale);
	draw->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(x1, y + h),
		fade(rgba(52, 58, 128), alpha), fade(rgba(52, 58, 128), alpha),
		fade(rgba(26, 29, 72), alpha), fade(rgba(26, 29, 72), alpha));

	/* Marquee. */
	const ImVec2 m0(x0 + 8 * scale, y + 10 * scale), m1(x1 - 8 * scale, y + 66 * scale);
	if (art && art->marquee.id) {
		draw->AddRectFilled(m0, m1, fade(rgba(0, 0, 0), alpha));
		image_fit(draw, art->marquee, m0, m1, fade(rgba(255, 255, 255), alpha));
	} else {
		draw->AddRectFilledMultiColor(m0, m1, fade(rgba(230, 57, 70), alpha), fade(rgba(123, 44, 191), alpha),
			fade(rgba(123, 44, 191), alpha), fade(rgba(230, 57, 70), alpha));
		centred_text(draw, s->font_marquee, 30.0f * scale, m0.x + 8 * scale, m1.x - 8 * scale,
			(m0.y + m1.y) * 0.5f, fade(rgba(255, 246, 220), alpha), game.title.c_str());
	}
	draw->AddRect(m0, m1, fade(rgba(255, 255, 255, 0.25f), alpha));

	/* Screen: bezel, then a 4:3 picture. */
	const ImVec2 b0(x0 + 14 * scale, y + 78 * scale), b1(x1 - 14 * scale, y + 246 * scale);
	draw->AddRectFilled(b0, b1, fade(rgba(6, 6, 10), alpha), 10.0f * scale);
	const float sw = (b1.x - b0.x) - 20 * scale, sh = sw * 0.75f;
	const ImVec2 s0(cx - sw * 0.5f, (b0.y + b1.y - sh) * 0.5f), s1(cx + sw * 0.5f, (b0.y + b1.y + sh) * 0.5f);
	if (art && art->snap.id) {
		/* MAME snaps keep the game's pixel size (512x240...); the monitor is 4:3. */
		draw->AddImage((ImTextureID)(uintptr_t)art->snap.id, s0, s1, ImVec2(0, 0), ImVec2(1, 1),
			fade(rgba(255, 255, 255), alpha));
	} else {
		draw->AddRectFilledMultiColor(s0, s1, fade(rgba(20, 34, 44), alpha), fade(rgba(20, 34, 44), alpha),
			fade(rgba(8, 14, 20), alpha), fade(rgba(8, 14, 20), alpha));
		for (float line = s0.y; line < s1.y; line += 3.0f * scale)
			draw->AddLine(ImVec2(s0.x, line), ImVec2(s1.x, line), fade(rgba(0, 0, 0, 0.35f), alpha));
		centred_text(draw, s->font_title, 22.0f * scale, s0.x + 8, s1.x - 8, (s0.y + s1.y) * 0.5f - 8 * scale,
			fade(rgba(120, 230, 200), alpha), version.set.c_str());
		centred_text(draw, s->font_small, 13.0f * scale, s0.x + 8, s1.x - 8, (s0.y + s1.y) * 0.5f + 16 * scale,
			fade(rgba(120, 230, 200, 0.6f), alpha), "no snapshot");
	}
	if (focused)
		draw->AddRect(ImVec2(s0.x - 2, s0.y - 2), ImVec2(s1.x + 2, s1.y + 2), fade(rgba(255, 180, 0, 0.8f), alpha), 0, 0, 2.0f);

	/* Control panel: a slanted shelf with a stick and four buttons. */
	const float py = y + 262 * scale, ph = 54 * scale;
	const ImVec2 shelf[4] = { ImVec2(x0 + 6 * scale, py), ImVec2(x1 - 6 * scale, py),
		ImVec2(x1 + 6 * scale, py + ph), ImVec2(x0 - 6 * scale, py + ph) };
	draw->AddConvexPolyFilled(shelf, 4, fade(rgba(30, 30, 36), alpha));
	draw->AddLine(shelf[0], shelf[1], fade(rgba(255, 255, 255, 0.18f), alpha), 1.5f);
	const float stick_x = x0 + w * 0.28f, row = py + ph * 0.55f;
	draw->AddLine(ImVec2(stick_x, row + 6 * scale), ImVec2(stick_x, row - 6 * scale), fade(rgba(150, 150, 160), alpha), 3.0f * scale);
	draw->AddCircleFilled(ImVec2(stick_x, row - 9 * scale), 8.0f * scale, fade(rgba(220, 40, 50), alpha), 20);
	static const ImU32 button_colors[4] = { IM_COL32(255, 196, 0, 255), IM_COL32(40, 120, 255, 255),
		IM_COL32(40, 200, 90, 255), IM_COL32(240, 60, 70, 255) };
	for (int i = 0; i < 4; i++) {
		const float bx = x0 + w * (0.52f + 0.11f * (i % 2)) + (i / 2) * 6.0f * scale;
		const float by = row - 8 * scale + (i / 2) * 15 * scale + (i % 2) * -3.0f * scale;
		draw->AddCircleFilled(ImVec2(bx, by), 5.5f * scale, fade(button_colors[i], alpha), 16);
	}

	/* Coin door with two lit slots. */
	const ImVec2 d0(cx - 34 * scale, y + 330 * scale), d1(cx + 34 * scale, y + 378 * scale);
	draw->AddRectFilled(d0, d1, fade(rgba(14, 14, 18), alpha), 3.0f * scale);
	for (int i = 0; i < 2; i++) {
		const float slot = cx + (i ? 12 : -12) * scale;
		draw->AddRectFilled(ImVec2(slot - 5 * scale, d0.y + 10 * scale), ImVec2(slot + 5 * scale, d0.y + 24 * scale),
			fade(rgba(255, 140, 30), alpha), 2.0f * scale);
	}
}

/* A PlayStation face button glyph, then its caption; returns the next x. */
float hint(ImDrawList *draw, float x, float y, unsigned button, const char *caption)
{
	const ImVec2 c(x + 9, y);
	const ImU32 white = rgba(235, 235, 245);
	draw->AddCircleFilled(c, 9.0f, rgba(40, 44, 64), 20);
	switch (button) {
	case UI_CROSS:
		draw->AddLine(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), rgba(120, 170, 255), 2.0f);
		draw->AddLine(ImVec2(c.x - 4, c.y + 4), ImVec2(c.x + 4, c.y - 4), rgba(120, 170, 255), 2.0f);
		break;
	case UI_CIRCLE:
		draw->AddCircle(c, 4.5f, rgba(255, 110, 110), 16, 2.0f);
		break;
	case UI_TRIANGLE: {
		const ImVec2 t[3] = { ImVec2(c.x, c.y - 5), ImVec2(c.x + 5, c.y + 4), ImVec2(c.x - 5, c.y + 4) };
		draw->AddPolyline(t, 3, rgba(100, 220, 160), true, 2.0f);
		break;
	}
	default:
		break;
	}
	const ImVec2 extent = s->font_small->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, caption);
	draw->AddText(s->font_small, 15.0f, ImVec2(x + 24, y - extent.y * 0.5f), white, caption);
	return x + 24 + extent.x + 22;
}

float text_hint(ImDrawList *draw, float x, float y, const char *key, const char *caption)
{
	const ImVec2 key_size = s->font_small->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, key);
	draw->AddRectFilled(ImVec2(x, y - 9), ImVec2(x + key_size.x + 12, y + 9), rgba(40, 44, 64), 4.0f);
	draw->AddText(s->font_small, 13.0f, ImVec2(x + 6, y - key_size.y * 0.5f), rgba(200, 205, 230), key);
	const ImVec2 extent = s->font_small->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, caption);
	draw->AddText(s->font_small, 15.0f, ImVec2(x + key_size.x + 18, y - extent.y * 0.5f), rgba(235, 235, 245), caption);
	return x + key_size.x + 18 + extent.x + 22;
}

/* ---- 3D carousel ---- */

struct SurfaceJob {
	const Game *game;
	const Art *art;
	const BoardStyle *style;
};

/* The lit marquee: the user's art, or the title on a gradient. */
void draw_marquee3d(ImDrawList *draw, void *opaque)
{
	const SurfaceJob *job = (const SurfaceJob *)opaque;
	const float W = CABINET3D_MARQUEE_W, H = CABINET3D_MARQUEE_H;
	if (job->art->marquee.id) {
		draw->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), rgba(0, 0, 0));
		draw->PushTextureID((ImTextureID)(uintptr_t)job->art->marquee.id);
		image_fit(draw, job->art->marquee, ImVec2(0, 0), ImVec2(W, H), rgba(255, 255, 255));
		draw->PopTextureID();
		return;
	}
	const ImU32 a = job->style->marquee_a, b = job->style->marquee_b;
	draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), a, b, fade(b, 0.7f), fade(a, 0.75f));
	draw->AddRectFilled(ImVec2(0, 0), ImVec2(W, H * 0.45f), rgba(255, 255, 255, 0.10f));
	centred_text(draw, s->font_title, 80.0f, 24, W - 24, H * 0.5f + 2, rgba(0, 0, 0, 0.35f), job->game->title.c_str());
	centred_text(draw, s->font_title, 80.0f, 24, W - 24, H * 0.5f - 2, rgba(255, 248, 225), job->game->title.c_str());
}

/* The screen: a black bezel around the 4:3 picture, and the glass. */
void draw_screen3d(ImDrawList *draw, void *opaque)
{
	const SurfaceJob *job = (const SurfaceJob *)opaque;
	const float W = CABINET3D_SCREEN_W, H = CABINET3D_SCREEN_H;
	const Version &version = job->game->versions[job->game->selected];
	draw->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), rgba(8, 8, 12));
	const float sh = H - 64.0f, sw = sh * 4.0f / 3.0f;
	const ImVec2 s0((W - sw) * 0.5f, 32.0f), s1((W + sw) * 0.5f, 32.0f + sh);
	if (job->art->snap.id) {
		draw->PushTextureID((ImTextureID)(uintptr_t)job->art->snap.id);
		draw->AddImage((ImTextureID)(uintptr_t)job->art->snap.id, s0, s1);
		draw->PopTextureID();
	} else {
		draw->AddRectFilledMultiColor(s0, s1, rgba(20, 40, 52), rgba(20, 40, 52), rgba(6, 14, 20), rgba(6, 14, 20));
		for (float y = s0.y; y < s1.y; y += 4.0f)
			draw->AddLine(ImVec2(s0.x, y), ImVec2(s1.x, y), rgba(0, 0, 0, 0.35f), 1.5f);
		centred_text(draw, s->font_title, 52.0f, s0.x + 20, s1.x - 20, (s0.y + s1.y) * 0.5f - 14, rgba(120, 235, 205),
			version.set.c_str());
		centred_text(draw, s->font_body, 26.0f, s0.x + 20, s1.x - 20, (s0.y + s1.y) * 0.5f + 34, rgba(120, 235, 205, 0.6f),
			"no snapshot");
	}
	/* Glass: a soft diagonal reflection. */
	const ImVec2 glass[4] = { ImVec2(s0.x, s0.y), ImVec2(s0.x + sw * 0.55f, s0.y), ImVec2(s0.x + sw * 0.25f, s1.y),
		ImVec2(s0.x, s1.y) };
	draw->AddConvexPolyFilled(glass, 4, rgba(255, 255, 255, 0.06f));
}

void ensure_surfaces(const Game &game, Art *art)
{
	SurfaceJob job = { &game, art, &style_for(game.board) };
	if (!art->marquee3d.id)
		ui_gl_render_to_texture(&art->marquee3d, CABINET3D_MARQUEE_W, CABINET3D_MARQUEE_H, draw_marquee3d, &job);
	if (!art->screen3d.id)
		ui_gl_render_to_texture(&art->screen3d, CABINET3D_SCREEN_W, CABINET3D_SCREEN_H, draw_screen3d, &job);
}

/* The carousel area of the screen, in pixels. */
const float kStageX = 0.0f, kStageY = 44.0f, kStageW = 584.0f;

void stage_camera(float view_projection[16])
{
	const float stage_h = (float)s->config.height - kStageY - 44.0f;
	float projection[16], view[16];
	const float eye[3] = { 0.0f, 1.22f, 4.25f }, target[3] = { 0.0f, 0.97f, 0.0f }, up[3] = { 0.0f, 1.0f, 0.0f };
	mat4_perspective(projection, 32.0f, kStageW / stage_h, 0.1f, 40.0f);
	mat4_look_at(view, eye, target, up);
	mat4_multiply(view_projection, projection, view);
}

unsigned kick_for(const std::string &board)
{
	for (const auto &entry : s->kick)
		if (entry.first == board)
			return entry.second;
	const BoardStyle &style = style_for(board);
	s->kick.push_back(std::make_pair(board, cabinet3d_kick_texture(style.kick_top, style.kick_bottom)));
	return s->kick.back().second;
}

void cabinet_place(float model[16], float d)
{
	const float distance = fabsf(d);
	/* The selected cabinet turns a little to show its side, sways gently
	 * and follows the right stick; neighbours step back and turn toward the
	 * middle. */
	const float focus = std::max(0.0f, 1.0f - distance * 2.0f);
	const float sway = 4.0f * sinf(s->time * 2.0f * (float)M_PI / 6.0f) * focus;
	mat4_place_tilted(model, d * 1.32f, 0.0f, -std::min(distance, 2.5f) * 1.35f,
		-10.0f - d * 30.0f + sway + s->yaw * focus, s->pitch * focus, 0.95f);
}

void draw_stage_3d()
{
	const int W = s->config.width, H = s->config.height;
	const float stage_h = (float)H - kStageY - 44.0f;
	float view_projection[16], model[16];
	std::vector<int> visible;
	for (int i = 0; i < (int)s->games.size(); i++)
		if (fabsf((float)i - s->scroll) < 2.8f)
			visible.push_back(i);
	for (int i : visible) {
		Game &game = s->games[i];
		ensure_surfaces(game, art_for(game));
	}
	stage_camera(view_projection);
	ui_gl3d_begin((int)kStageX, (int)kStageY, (int)kStageW, (int)stage_h, H, view_projection);
	for (int i : visible) {
		cabinet_place(model, (float)i - s->scroll);
		cabinet3d_shadow(model, std::max(0.3f, 1.0f - 0.3f * fabsf((float)i - s->scroll)));
	}
	for (int i : visible) {
		const float d = (float)i - s->scroll;
		Art *art = art_for(s->games[i]);
		const BoardStyle &style = style_for(s->games[i].board);
		const float focus = std::max(0.0f, 1.0f - fabsf(d) * 2.0f);
		CabinetLook look = { art->marquee3d.id, art->screen3d.id, kick_for(s->games[i].board), style.body,
			style.molding, std::max(0.28f, 1.0f - 0.36f * fabsf(d)),
			/* The selected marquee breathes, as a lit tube does. */
			1.0f - focus * (0.07f + 0.07f * sinf(s->time * 2.0f * (float)M_PI / 2.5f)) };
		cabinet_place(model, d);
		cabinet3d_draw(model, &look);
	}
	ui_gl3d_end();
	(void)W;
}

void draw_background(ImDrawList *draw)
{
	const float W = (float)s->config.width, H = (float)s->config.height;
	draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), rgba(13, 16, 32), rgba(13, 16, 32),
		rgba(26, 31, 58), rgba(26, 31, 58));
	if (s->config.flat || s->games.empty())
		return;
	/* A pool of light on the floor of the stage. */
	float view_projection[16], x, y;
	const float floor_point[3] = { 0.0f, 0.0f, 0.0f };
	stage_camera(view_projection);
	if (mat4_project(view_projection, floor_point, kStageX, kStageY, kStageW,
			(float)s->config.height - kStageY - 44.0f, &x, &y))
		for (int i = 0; i < 48; i++)
			draw->AddCircleFilled(ImVec2(x, y), 260.0f - i * 5.0f, rgba(90, 100, 200, 0.006f), 64);
}

void draw_frame(ImDrawList *draw)
{
	const float W = (float)s->config.width, H = (float)s->config.height;

	if (s->games.empty()) {
		centred_text(draw, s->font_title, 26.0f, 40, W - 40, H * 0.42f, rgba(235, 235, 245),
			"No arcade games found");
		const std::string where = "Copy MAME romset zips (soulclbr.zip, tekken3.zip...) to " + s->rompath;
		centred_text(draw, s->font_body, 17.0f, 40, W - 40, H * 0.42f + 40, rgba(170, 175, 200), where.c_str());
		hint(draw, 32, H - 24, UI_CIRCLE, "Exit");
		return;
	}

	/* Header. */
	const ImVec2 pcsx = s->font_body->CalcTextSizeA(20.0f, FLT_MAX, 0.0f, "PCSX ");
	draw->AddText(s->font_body, 20.0f, ImVec2(32, 15), rgba(240, 240, 248), "PCSX ");
	draw->AddText(s->font_body, 20.0f, ImVec2(32 + pcsx.x, 15), rgba(255, 180, 0), "ReArcade");
	const ImVec2 brand = s->font_body->CalcTextSizeA(20.0f, FLT_MAX, 0.0f, "PCSX ReArcade");
	char count[64];
	snprintf(count, sizeof(count), "%d / %d", s->game + 1, (int)s->games.size());
	draw->AddText(s->font_small, 15.0f, ImVec2(32 + brand.x + 16, 19), rgba(150, 155, 185), count);
	const ImVec2 chip = s->font_small->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, s->games[s->game].board.c_str());
	draw->AddRectFilled(ImVec2(W - 32 - chip.x - 20, 16), ImVec2(W - 32, 38), rgba(230, 57, 70, 0.22f), 11.0f);
	draw->AddText(s->font_small, 13.0f, ImVec2(W - 32 - chip.x - 10, 27 - chip.y * 0.5f), rgba(255, 160, 165),
		s->games[s->game].board.c_str());

	/* Carousel, far to near: neighbours shrink and darken with distance. */
	const float center_x = 262.0f, area_right = 572.0f;
	std::vector<int> order;
	for (int i = 0; i < (int)s->games.size(); i++)
		if (fabsf((float)i - s->scroll) < 2.6f)
			order.push_back(i);
	std::sort(order.begin(), order.end(), [](int a, int b) {
		return fabsf((float)a - s->scroll) > fabsf((float)b - s->scroll);
	});
	draw->PushClipRect(ImVec2(0, 48), ImVec2(area_right, H - 50));
	for (int i : s->config.flat ? order : std::vector<int>()) {
		const float d = (float)i - s->scroll, distance = std::min(fabsf(d), 2.0f);
		const float scale = 1.0f - 0.24f * distance;
		const float x = center_x + d * 210.0f, top = 62.0f + (1.0f - scale) * 220.0f;
		draw_cabinet(draw, s->games[i], x, top, scale, std::max(0.22f, 1.0f - 0.45f * distance), fabsf(d) < 0.5f);
	}
	draw->PopClipRect();

	/* Info panel: title, versions. */
	const Game &game = s->games[s->game];
	const Version &version = game.versions[game.selected];
	const float px = 604.0f, pw = W - px - 32.0f;
	draw->AddRectFilled(ImVec2(px - 16, 56), ImVec2(W - 16, H - 56), rgba(24, 29, 54), 10.0f);
	draw->AddRect(ImVec2(px - 16, 56), ImVec2(W - 16, H - 56), rgba(255, 255, 255, 0.06f), 10.0f);
	draw->AddText(s->font_title, 30.0f, ImVec2(px, 70), rgba(245, 245, 250), game.title.c_str(), NULL, pw);
	const ImVec2 title_size = s->font_title->CalcTextSizeA(30.0f, FLT_MAX, pw, game.title.c_str());
	char meta[128];
	snprintf(meta, sizeof(meta), "%d  ·  %s", version.year, game.board.c_str());
	draw->AddText(s->font_body, 17.0f, ImVec2(px, 76 + title_size.y), rgba(160, 165, 195), meta);

	float y = 118 + title_size.y;
	draw->AddText(s->font_small, 13.0f, ImVec2(px, y), rgba(255, 180, 0), "VERSIONS");
	y += 22;
	const int rows = std::min((int)game.versions.size(), 6);
	int first = std::max(0, std::min(game.selected - rows / 2, (int)game.versions.size() - rows));
	for (int i = first; i < first + rows; i++) {
		const Version &v = game.versions[i];
		const bool selected = i == game.selected;
		const ImVec2 r0(px - 6, y), r1(W - 26, y + 42);
		if (selected) {
			draw->AddRectFilled(r0, r1, rgba(255, 180, 0, 0.16f), 6.0f);
			draw->AddRectFilled(r0, ImVec2(r0.x + 3, r1.y), rgba(255, 180, 0), 2.0f);
		}
		draw->AddText(s->font_body, 17.0f, ImVec2(px + 8, y + 4), selected ? rgba(255, 255, 255) : rgba(200, 204, 225),
			v.label.empty() ? v.set.c_str() : v.label.c_str());
		draw->AddText(s->font_small, 13.0f, ImVec2(px + 8, y + 23), selected ? rgba(255, 200, 90) : rgba(130, 135, 165),
			v.set.c_str());
		y += 46;
	}
	if ((int)game.versions.size() > rows) {
		char more[32];
		snprintf(more, sizeof(more), "%d / %d", game.selected + 1, (int)game.versions.size());
		draw->AddText(s->font_small, 13.0f, ImVec2(px, y + 2), rgba(130, 135, 165), more);
	}

	/* Footer hints. */
	const float fy = H - 26;
	float x = 32;
	x = hint(draw, x, fy, UI_CROSS, "Play");
	x = text_hint(draw, x, fy, "< >", "Game");
	if (game.versions.size() > 1)
		x = text_hint(draw, x, fy, "^ v", "Version");
	hint(draw, W - 110, fy, UI_CIRCLE, "Exit");
}

bool pressed(unsigned buttons, unsigned button, float dt)
{
	const bool now = (buttons & button) != 0, before = (s->previous & button) != 0;
	if (now && !before) {
		s->repeat_button = button;
		s->repeat_time = 0.32f;
		return true;
	}
	if (now && s->repeat_button == button && (button & (UI_UP | UI_DOWN | UI_LEFT | UI_RIGHT))) {
		s->repeat_time -= dt;
		if (s->repeat_time <= 0.0f) {
			s->repeat_time += 0.085f;
			return true;
		}
	}
	return false;
}

} // namespace

bool arcade_ui_init(const ArcadeUiConfig *config)
{
	ImFontConfig font_config;
	s = new State();
	s->config = *config;
	s->rompath = config->rompath;
	s->data_root = config->data_root;
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = NULL;
	io.DisplaySize = ImVec2((float)config->width, (float)config->height);
	font_config.FontDataOwnedByAtlas = false;
	font_config.OversampleH = 2;
	s->font_small = io.Fonts->AddFontFromMemoryTTF(Roboto_Medium_ttf, sizeof(Roboto_Medium_ttf), 15.0f, &font_config);
	s->font_body = io.Fonts->AddFontFromMemoryTTF(Roboto_Medium_ttf, sizeof(Roboto_Medium_ttf), 18.0f, &font_config);
	s->font_title = io.Fonts->AddFontFromMemoryTTF(Roboto_Medium_ttf, sizeof(Roboto_Medium_ttf), 30.0f, &font_config);
	s->font_marquee = s->font_title;
	if (!ui_gl_init() || !cabinet3d_init())
		return false;
	build_catalog();
	return true;
}

void arcade_ui_shutdown(void)
{
	if (!s)
		return;
	save_config();
	for (Art &art : s->art) {
		ui_gl_free_texture(&art.snap);
		ui_gl_free_texture(&art.marquee);
		ui_gl_free_texture(&art.marquee3d);
		ui_gl_free_texture(&art.screen3d);
	}
	for (auto &entry : s->kick) {
		UiTexture texture = { entry.second, 0, 0 };
		ui_gl_free_texture(&texture);
	}
	cabinet3d_shutdown();
	ui_gl_shutdown();
	ImGui::DestroyContext();
	delete s;
	s = NULL;
}

int arcade_ui_frame(unsigned buttons, float stick_x, float stick_y, float dt, char *launch_path, size_t launch_size)
{
	int action = ARCADE_UI_NONE;
	ImGuiIO &io = ImGui::GetIO();
	s->frame++;
	io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;
	s->time += io.DeltaTime;

	if (pressed(buttons, UI_CIRCLE, dt))
		action = ARCADE_UI_QUIT;
	if (!s->games.empty()) {
		Game &game = s->games[s->game];
		if (pressed(buttons, UI_LEFT, dt) && s->game > 0)
			s->game--;
		if (pressed(buttons, UI_RIGHT, dt) && s->game + 1 < (int)s->games.size())
			s->game++;
		if (pressed(buttons, UI_UP, dt) && game.selected > 0)
			game.selected--;
		if (pressed(buttons, UI_DOWN, dt) && game.selected + 1 < (int)game.versions.size())
			game.selected++;
		if (pressed(buttons, UI_CROSS, dt) || pressed(buttons, UI_START, dt)) {
			const Game &chosen = s->games[s->game];
			snprintf(launch_path, launch_size, "%s/%s.zip", s->rompath.c_str(),
				chosen.versions[chosen.selected].set.c_str());
			save_config();
			action = ARCADE_UI_LAUNCH;
		}
	}
	s->previous = buttons;
	/* The right stick turns the selected cabinet; another game starts from
	 * its rest pose. */
	if (s->posed_game != s->game) {
		s->posed_game = s->game;
		s->yaw = s->pitch = 0.0f;
	}
	if (fabsf(stick_x) > 0.2f)
		s->yaw = fmodf(s->yaw + stick_x * 150.0f * io.DeltaTime, 360.0f);
	if (fabsf(stick_y) > 0.2f)
		s->pitch = std::max(-15.0f, std::min(15.0f, s->pitch + stick_y * 45.0f * io.DeltaTime));
	/* Ease the carousel toward the selected game. */
	s->scroll += ((float)s->game - s->scroll) * std::min(1.0f, io.DeltaTime * 12.0f);
	if (fabsf((float)s->game - s->scroll) < 0.001f)
		s->scroll = (float)s->game;

	{
		ImDrawList background(ImGui::GetDrawListSharedData());
		ImDrawList *lists[1] = { &background };
		background.PushClipRect(ImVec2(0, 0), io.DisplaySize);
		background.PushTextureID(io.Fonts->TexID);
		draw_background(&background);
		ui_gl_render_lists(lists, 1, s->config.width, s->config.height);
	}
	if (!s->config.flat && !s->games.empty())
		draw_stage_3d();

	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::SetNextWindowBgAlpha(0.0f);
	ImGui::Begin("##arcade", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
	draw_frame(ImGui::GetWindowDrawList());
	ImGui::End();
	ImGui::PopStyleVar(2);
	ImGui::Render();
	ui_gl_render(ImGui::GetDrawData(), s->config.width, s->config.height);
	return action;
}
