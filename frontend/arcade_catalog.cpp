#include "arcade_catalog.h"

#include <ctype.h>
#include <string.h>
#include <algorithm>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "arcade_profile.h"

#include "../libpcsxcore/arcade/arcade_romset.h"
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
	/* Discs: only the generated label uses it. */
	{ "PlayStation", COLOUR(60, 60, 66), COLOUR(170, 170, 178), COLOUR(40, 40, 46), COLOUR(18, 18, 22),
	  COLOUR(70, 80, 110), COLOUR(30, 34, 48) },
};
const int arcade_catalog_style_count = sizeof(arcade_catalog_styles) / sizeof(arcade_catalog_styles[0]);

struct PreparedProfile {
	ArcadeProfileKind kind;
	const char *system, *set, *parent, *description, *parent_description, *files;
	int year;
};
static const PreparedProfile prepared_profiles[] = {
#include "arcade_catalog_profiles.inc"
};

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
	const unsigned count = system12_romset_count(), other_count = arcade_romset_count();
	std::vector<unsigned char> available(count, 0), other_available(other_count, 0);
	entries_.clear();
	games_.clear();
	if (demo) {
		std::fill(available.begin(), available.end(), 1);
		std::fill(other_available.begin(), other_available.end(), 1);
	} else if (mame_romindex *index = mame_romindex_open(rompath.c_str())) {
		/* One pass over the zips serves every family. */
		system12_romset_scan_index(index, available.data());
		arcade_romset_scan_index(index, other_available.data());
		mame_romindex_close(index);
	}
	for (unsigned i = 0; i < count; i++) {
		const system12_romset_info *info = system12_romset_at(i);
		if (!available[i] || strcmp(info->unsupported, "none"))
			continue;
		add_arcade("Namco System 12", info->name, info->parent, info->description,
			info->description, info->year, rompath + "/" + info->name + ".zip");
	}
	/* System 10, System 11 and Capcom ZN zips: the core builds their board package. */
	for (unsigned i = 0; i < other_count; i++) {
		const arcade_romset_info *info = arcade_romset_at(i);
		if (other_available[i])
			add_arcade(info->system, info->name, info->parent, info->description,
				info->description, info->year, rompath + "/" + info->name + ".zip");
	}
	if (demo) {
		for (const PreparedProfile &p : prepared_profiles)
			if (p.kind != ARCADE_PROFILE_SYSTEM12)
				add_arcade(p.system, p.set, p.parent, p.description, p.parent_description, p.year, rompath + "/" + p.set);
	} else
		scan_profiles(rompath);
	std::sort(entries_.begin(), entries_.end(), [](const Game &a, const Game &b) { return a.title < b.title; });
	link();
}

void ArcadeCatalog::add_arcade(const char *system, const char *set, const char *parent,
	const char *description, const char *parent_description, int year, const std::string &path)
{
	const std::string key = parent[0] ? parent : set;
	std::string title, label, parent_label;
	split_description(description, &title, &label);
	auto game = std::find_if(entries_.begin(), entries_.end(),
		[&](const Game &g) { return g.kind == SHELF_CABINET && g.system == system && g.key == key; });
	if (game == entries_.end()) {
		std::string group_title;
		split_description(parent[0] ? parent_description : description, &group_title, &parent_label);
		entries_.push_back(Game{ key, group_title, system, SHELF_CABINET, {}, {}, {}, {}, {}, {} });
		game = entries_.end() - 1;
	}
	/* Prefer a prepared profile to its zip, without listing the set twice. */
	auto existing = std::find(game->ids.begin(), game->ids.end(), set);
	if (existing != game->ids.end()) {
		game->launches[existing - game->ids.begin()] = path;
		return;
	}
	const size_t at = parent[0] ? game->ids.size() : 0;
	if (!parent[0]) game->title = title;
	game->ids.insert(game->ids.begin() + at, set);
	game->labels.insert(game->labels.begin() + at, label);
	game->launches.insert(game->launches.begin() + at, path);
	game->arts.insert(game->arts.begin() + at, "");
	game->years.insert(game->years.begin() + at, year);
}

void ArcadeCatalog::scan_profiles(const std::string &rompath)
{
	DIR *dir = opendir(rompath.c_str());
	if (!dir) return;
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.') continue;
		const std::string path = rompath + "/" + entry->d_name;
		ArcadeProfile profile;
		if (!arcade_profile_read(path.c_str(), &profile)) continue;
		for (const PreparedProfile &p : prepared_profiles) {
			if (p.kind != profile.kind || strcmp(p.set, profile.set)) continue;
			bool complete = true;
			std::string files = p.files;
			size_t begin = 0;
			while (begin < files.size()) {
				size_t end = files.find('|', begin);
				if (end == std::string::npos) end = files.size();
				struct stat st;
				if (stat((path + "/" + files.substr(begin, end - begin)).c_str(), &st) || !S_ISREG(st.st_mode)) {
					complete = false; break;
				}
				begin = end + 1;
			}
			if (complete)
				add_arcade(p.system, p.set, p.parent, p.description, p.parent_description, p.year, path);
			break;
		}
	}
	closedir(dir);
}

/* "Ridge Racer Type 4 (Europe) (Disc 1)" -> "Ridge Racer Type 4" and
 * "Europe, Disc 1". */
static void split_file_title(const std::string &file_title, std::string *title, std::string *label)
{
	const size_t tags = file_title.find(" (");
	*title = file_title.substr(0, tags);
	/* "R4 - Ridge Racer Type 4": the short name goes, as in the covers. */
	const size_t dash = title->find(" - ");
	if (dash != std::string::npos && dash <= 5 && dash + 3 < title->size() &&
		std::all_of(title->begin(), title->begin() + dash, [](char c) { return isupper(c) || isdigit(c); }))
		title->erase(0, dash + 3);
	label->clear();
	if (tags == std::string::npos)
		return;
	for (char c : file_title.substr(tags + 1)) {
		if (c == '(')
			continue;
		if (c == ')')
			c = ',';
		label->push_back(c);
	}
	while (!label->empty() && (label->back() == ',' || label->back() == ' '))
		label->pop_back();
}

void ArcadeCatalog::add_disc(const std::string &path, const std::string &file_title, const std::string &cover_art)
{
	std::string title, label;
	split_file_title(file_title, &title, &label);
	/* Discs come after the arcade games, sorted by title. */
	auto game = std::find_if(entries_.begin(), entries_.end(),
		[&](const Game &g) { return g.kind == SHELF_DISC && g.key == title; });
	if (game == entries_.end()) {
		auto at = std::find_if(entries_.begin(), entries_.end(),
			[&](const Game &g) { return g.kind == SHELF_DISC && title < g.title; });
		game = entries_.insert(at, Game{ title, title, "PlayStation", SHELF_DISC, {}, {}, {}, {}, {}, {} });
	}
	const size_t slash = path.find_last_of("/\\");
	game->ids.push_back(slash == std::string::npos ? path : path.substr(slash + 1));
	game->labels.push_back(label);
	game->launches.push_back(path);
	game->arts.push_back(cover_art);
	game->years.push_back(0);
	link();
}

void ArcadeCatalog::link()
{
	games_.clear();
	for (Game &game : entries_) {
		game.versions.clear();
		for (size_t v = 0; v < game.ids.size(); v++)
			game.versions.push_back(ShelfVersion{ game.ids[v].c_str(), game.labels[v].c_str(),
				game.launches[v].c_str(), game.years[v], game.arts[v].c_str() });
		games_.push_back(ShelfGame{ game.key.c_str(), game.title.c_str(),
			game.system.c_str(), game.kind,
			game.versions.data(), (int)game.versions.size() });
	}
}
