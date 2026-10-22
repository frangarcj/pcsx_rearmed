/* The arcade-shelf catalogue of PCSX ReArcade: System 10, 11 and 12 and Capcom ZN
 * MAME zips and prepared board profiles, one game per parent, then the
 * PlayStation discs, and the cabinet colours of each board family. */
#ifndef PCSX_ARCADE_CATALOG_H
#define PCSX_ARCADE_CATALOG_H

#include <string>
#include <vector>

#include "arcade_shelf.h"

class ArcadeCatalog {
public:
	/* Lists the supported sets complete in rompath (all of them with
	 * demo, for UI work). Prepared directories launch directly; the
	 * versions found as MAME zips launch "<rompath>/<set>.zip". */
	void build(const std::string &rompath, bool demo);
	/* Adds a PlayStation disc after the arcade games. Files named
	 * "<title> (<tags>)" share one disc per title, each file a version
	 * labelled with its tags. cover_art: the cover's name in covers/,
	 * without ".png", or "" for none. */
	void add_disc(const std::string &path, const std::string &file_title, const std::string &cover_art);
	const ShelfGame *games() const { return games_.data(); }
	int count() const { return (int)games_.size(); }

private:
	struct Game {
		std::string key, title, system;
		ShelfItemKind kind;
		std::vector<std::string> ids, labels, launches, arts;
		std::vector<int> years;
		std::vector<ShelfVersion> versions;
	};
	/* Points the shelf records at the strings, after any change. */
	void link();
	void add_arcade(const char *system, const char *set, const char *parent,
		const char *description, const char *parent_description, int year, const std::string &path);
	void scan_profiles(const std::string &rompath);
	std::vector<Game> entries_;
	std::vector<ShelfGame> games_;
};

/* Cabinet colours per board family; the first is the default. */
extern const ShelfSystemStyle arcade_catalog_styles[];
extern const int arcade_catalog_style_count;

#endif
