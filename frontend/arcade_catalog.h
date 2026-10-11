/* The arcade-shelf catalogue of PCSX ReArcade: the System 12 MAME sets
 * that are complete in a rompath, one game per MAME parent, and the
 * cabinet colours of each board family. */
#ifndef PCSX_ARCADE_CATALOG_H
#define PCSX_ARCADE_CATALOG_H

#include <string>
#include <vector>

#include "arcade_shelf.h"

class ArcadeCatalog {
public:
	/* Lists the supported sets complete in rompath (all of them with
	 * demo, for UI work). Versions launch "<rompath>/<set>.zip". */
	void build(const std::string &rompath, bool demo);
	const ShelfGame *games() const { return games_.data(); }
	int count() const { return (int)games_.size(); }

private:
	struct Game {
		std::string key, title;
		std::vector<std::string> ids, labels, launches;
		std::vector<int> years;
		std::vector<ShelfVersion> versions;
	};
	std::vector<Game> entries_;
	std::vector<ShelfGame> games_;
};

/* Cabinet colours per board family; the first is the default. */
extern const ShelfSystemStyle arcade_catalog_styles[];
extern const int arcade_catalog_style_count;

#endif
