# Arcade selector

`frontend/arcade_ui/` is the game selector of the arcade build. It shows one
3D cabinet per game (a MAME parent) in a carousel, and lists the game's
versions (its MAME sets) beside it:

- Left and right change the game. Up and down change the version.
- The right stick turns the selected cabinet and tilts it toward the viewer.
  Another game starts from the rest pose.
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

## Cabinets

`cabinet3d.cpp` builds an upright cabinet from its side profile. The side
panels are that profile, ear-clipped into triangles, with a red T-molding
along their edges. The front surfaces run between the panels, one per
profile segment:

- kick panel with a coin door;
- control panel with two players' controls, which are geometry: each has
  a stick (dust washer, metal shaft, red ball) and four buttons (black ring,
  coloured plunger);
- screen, which leans back;
- marquee, under an overhang.

Each surface has a material, lit per pixel by a key light with a
Blinn-Phong highlight and a cool rim light from behind:

- laminate sides with a procedural grain;
- glossy T-molding;
- a brushed control panel;
- semi-matte black plastic;
- glossy balls and buttons;
- metal shafts.

The marquee (plexiglass) and the screen (glass) are lit from inside, with
a sharp highlight. Per game, both are drawn once with ImGui into textures:

- the marquee art, or the title on a gradient;
- the snap inside a bezel, under a glass reflection.

Each board family has its own colours: the body, the T-molding, the kick
panel and the generated marquee. System 12 is indigo with a red molding;
System 11, System 10, Capcom ZN, Taito FX-1, Konami GV and Taito G-Net have
their own palettes for when those boards are listed.

The selected cabinet sways a few degrees, and its marquee pulses gently, as
a lit tube does. Neighbours step back, turn toward the middle and darken. A
soft shadow and a pool of light mark the floor. `--flat` keeps the earlier
2D fronts.

## Code

- `arcade_ui.cpp` holds the catalog, the input handling and the drawing,
  with Dear ImGui draw lists.
- `ui_gl.cpp` renders ImGui and loads PNG textures with libpng. It also
  renders draw lists into textures (framebuffer objects) and draws the lit
  3D triangles. It uses only GLES2-style calls and no VAOs, so the same code
  runs on vitaGL and on the OpenGL 2.1 context of macOS.
- `deps/imgui` is pinned to v1.61, the version that VitaSDK's `libimgui`
  has.
- The font is Roboto Medium (Apache License 2.0, from
  `deps/imgui/misc/fonts`). It is compiled in.

## On macOS

```sh
make -f Makefile.arcade-ui
build/arcade-ui/arcade-ui --rompath roms --data build/arcade-ui/data
build/arcade-ui/arcade-ui --demo        # every supported set, no zips needed
build/arcade-ui/arcade-ui --demo-boards # and every board palette in turn
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
| J, L, I, K | Right stick |

A game controller also works.

A scripted run draws with a fixed time step and writes PNG screenshots, so
the UI can be checked without a display:

```sh
build/arcade-ui/arcade-ui --headless --demo --script "R,R,D,-" \
  --step-frames 20 --shots /tmp/ui-steps --shot /tmp/ui-last.png
```

`--script` takes `U D L R X O T S LB RB ST SE` or `-` (no button), one
press every `--step-frames` frames. `SL SR SU SD` hold the right stick for
a whole step.
