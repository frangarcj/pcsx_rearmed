# Arcade selector

The selector of PCSX ReArcade is [arcade-shelf](https://github.com/frangarcj/arcade-shelf),
a separate MIT-licensed library in `deps/arcade-shelf`. It draws a 3D
carousel with one cabinet per game, lists the game's versions beside it,
and returns the version the user picks. The arcade-shelf README describes
its controls, the art it reads, the optional Arcade Database download
(`ADB=1`) and its build.

## In PCSX ReArcade

`frontend/arcade_catalog.cpp` builds the catalogue that arcade-shelf shows:

- one game per MAME parent, from the System 12 sets that are complete in
  the rompath (`system12_romset_scan`) and that the board adapter emulates;
- the parent's version first, then its clones, with the region and
  revision from MAME's description;
- a launch path for each version, `<rompath>/<set>.zip`, which the core
  resolves by name or by content (docs/system12-romsets.md);
- the cabinet colours of each board family (`arcade_catalog_styles`).

## Preview on macOS

`tools/arcade-ui/arcade_ui_mac.cpp` runs the selector with that catalogue:

```sh
make -f Makefile.arcade-ui            # also builds deps/arcade-shelf
build/arcade-ui/arcade-ui --rompath roms --data build/arcade-ui/data
build/arcade-ui/arcade-ui --demo      # every supported set, no zips needed
build/arcade-ui/arcade-ui --headless --demo --script "R,R,D,-" --step-frames 20 --shots /tmp/steps
```

Keys, scripts and screenshots work as in arcade-shelf's viewer.
