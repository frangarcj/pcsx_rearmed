#include "arcade_catalog.h"

#include <string.h>
#include <algorithm>

#include "../libpcsxcore/arcade/system12_romset.h"

#define COLOUR(r, g, b) (0xff000000u | ((unsigned)(b) << 16) | ((unsigned)(g) << 8) | (unsigned)(r))

/* system, body, trim, kick panel top and bottom, marquee gradient. */
const ShelfSystemStyle arcade_catalog_styles[] = {
	{ "Namco System 12", COLOUR(64, 72, 165), COLOUR(232, 62, 52), COLOUR(44, 48, 110), COLOUR(22, 24, 60),
	  COLOUR(240, 70, 80), COLOUR(130, 50, 200) },
	{ "Namco System 11", COLOUR(160, 36, 46), COLOUR(28, 28, 32), COLOUR(120, 26, 36), COLOUR(60, 12, 18),
	  COLOUR(250, 120, 40), COLOUR(200, 30, 50) },
	{ "Namco System 10", COLOUR(30, 130, 140), COLOUR(200, 205, 215), COLOUR(24, 96, 104), COLOUR(10, 44, 50),
	  COLOUR(40, 200, 200), COLOUR(30, 90, 170) },
	{ "Capcom ZN", COLOUR(44, 44, 50), COLOUR(250, 196, 30), COLOUR(36, 36, 42), COLOUR(16, 16, 20),
	  COLOUR(250, 200, 40), COLOUR(220, 70, 30) },
	{ "Taito FX-1", COLOUR(40, 96, 190), COLOUR(235, 235, 240), COLOUR(30, 72, 150), COLOUR(12, 30, 70),
	  COLOUR(60, 160, 255), COLOUR(30, 60, 180) },
	{ "Konami GV", COLOUR(34, 120, 76), COLOUR(245, 130, 30), COLOUR(26, 92, 58), COLOUR(10, 40, 24),
	  COLOUR(250, 150, 40), COLOUR(40, 160, 90) },
	{ "Taito G-Net", COLOUR(110, 44, 140), COLOUR(200, 200, 210), COLOUR(84, 32, 110), COLOUR(36, 12, 50),
	  COLOUR(200, 80, 240), COLOUR(90, 40, 180) },
};
const int arcade_catalog_style_count = sizeof(arcade_catalog_styles) / sizeof(arcade_catalog_styles[0]);

/* "Soul Calibur (Asia, SOC14/VER.C)" -> "Soul Calibur" and "Asia, SOC14/VER.C". */
static void split_description(const char *description, std::string *title, std::string *label)
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

void ArcadeCatalog::build(const std::string &rompath, bool demo)
{
	const unsigned count = system12_romset_count();
	std::vector<unsigned char> available(count, 0);
	entries_.clear();
	games_.clear();
	if (demo)
		std::fill(available.begin(), available.end(), 1);
	else if (system12_romset_scan(rompath.c_str(), available.data()) < 0)
		return;
	for (unsigned i = 0; i < count; i++) {
		const system12_romset_info *info = system12_romset_at(i);
		if (!available[i] || strcmp(info->unsupported, "none"))
			continue;
		const std::string key = info->parent[0] ? info->parent : info->name;
		std::string title, label;
		split_description(info->description, &title, &label);
		auto game = std::find_if(entries_.begin(), entries_.end(), [&](const Game &g) { return g.key == key; });
		if (game == entries_.end()) {
			entries_.push_back(Game{ key, title, {}, {}, {}, {}, {} });
			game = entries_.end() - 1;
		}
		/* The parent first: its title names the game. */
		const size_t at = info->parent[0] ? game->ids.size() : 0;
		if (!info->parent[0])
			game->title = title;
		game->ids.insert(game->ids.begin() + at, info->name);
		game->labels.insert(game->labels.begin() + at, label);
		game->launches.insert(game->launches.begin() + at, rompath + "/" + info->name + ".zip");
		game->years.insert(game->years.begin() + at, info->year);
	}
	std::sort(entries_.begin(), entries_.end(), [](const Game &a, const Game &b) { return a.title < b.title; });
	/* The strings are final now: point the shelf records at them. */
	for (Game &game : entries_) {
		for (size_t v = 0; v < game.ids.size(); v++)
			game.versions.push_back(ShelfVersion{ game.ids[v].c_str(), game.labels[v].c_str(),
				game.launches[v].c_str(), game.years[v] });
		games_.push_back(ShelfGame{ game.key.c_str(), game.title.c_str(), "Namco System 12", SHELF_CABINET,
			game.versions.data(), (int)game.versions.size() });
	}
}
