# Arcade selector

`frontend/arcade_ui/` is the game selector of the arcade build. It shows one
cabinet per game (a MAME parent) and lists the game's versions (its MAME
sets) beside it:

- Left and right change the game. Up and down change the version.
- Cross (or Start) plays the selected set. Circle exits.
- The last game and the version chosen for each game are kept in
  `<data>/arcade_ui.cfg`.

Only complete sets that the board adapter emulates are listed
(`system12_romset_scan`). For a selected set, the launch path is
`<rompath>/<set>.zip`, which the core resolves by name or by content
(docs/system12-romsets.md).

## Art

The art is optional and the user supplies it, with MAME names:

- `<data>/snap/<set>.png`, or else `<data>/titles/<set>.png`, for the screen;
- `<data>/marquees/<set>.png` for the marquee.

A clone without its own art uses its parent's. A snap fills the 4:3 screen
whatever its pixel size. Without art, the marquee shows the game title and
the screen shows the set name. The control panel, the coin door and the
cabinet are drawn by the UI. The build ships no game art.

## Code

- `arcade_ui.cpp` holds the catalog, the input handling and the drawing,
  with Dear ImGui draw lists.
- `ui_gl.cpp` renders ImGui and loads PNG textures with libpng. It uses only
  GLES2-style calls and no VAOs, so the same code runs on vitaGL and on the
  OpenGL 2.1 context of macOS.
- `deps/imgui` is pinned to v1.61, the version that VitaSDK's `libimgui`
  has.
- The font is Roboto Medium (Apache License 2.0, from
  `deps/imgui/misc/fonts`). It is compiled in.

## On macOS

```sh
make -f Makefile.arcade-ui
build/arcade-ui/arcade-ui --rompath roms --data build/arcade-ui/data
build/arcade-ui/arcade-ui --demo        # every supported set, no zips needed
```

Keys:

| Key | Button |
| --- | --- |
| Arrows | D-pad |
| Z or Enter | Cross |
| X or Esc | Circle |
| S | Triangle |
| A | Square |
| Q, W | L, R |

A game controller also works.

A scripted run draws with a fixed time step and writes PNG screenshots, so
the UI can be checked without a display:

```sh
build/arcade-ui/arcade-ui --headless --demo --script "R,R,D,-" \
  --step-frames 20 --shots /tmp/ui-steps --shot /tmp/ui-last.png
```

`--script` takes `U D L R X O T S LB RB ST SE` or `-` (no button), one
press every `--step-frames` frames.
