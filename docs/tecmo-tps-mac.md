# Tecmo TPS common-map validation on Mac

Gallop Racer 2 (`glpracr2`, Export) passes a bounded native Mac interpreter/GL
player run. Coin/start and menu confirmations enter Road to Victory, then the
first-person race. The final capture shows the controlled horse off the course,
changed stamina, elapsed race time 36.9 seconds and position 12/12 after the
replayed steering, whip and button inputs. Independent MAME 0.287 displays the
same startup/records screen from the authenticated media.

The interactive run completed 4500 frames in 75.175 seconds, consumed 138/138
input events and reported zero board faults. Its evidence is in
`artifacts/boards-mac-20261014/glpracr2-validation.json`, with selection/race
captures, inputs, logs, runtime EEPROM, raw SPU and the exact SDL output WAV.
Whole-machine timing comparison, physical Vita and authentic ARM64 DRC are not
covered by this Mac result.

## Source board and media

The source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`, `sony/zn.cpp`,
`tecmo_zn_state::coh1002m`. This uses ZN-1, 4 MiB main RAM, 2 MiB type-2 VRAM,
512 KiB BIOS, two CAT702 keys, the behavioral ZN MCU, Sony serial ports, AT28C16
and the ordinary PSX SPU. The native adapter shares the existing ZN motherboard
wiring; it does not instantiate the Capcom Z80/QSound daughterboard for TPS.

The ROM window is `1f000000..1f7fffff`, selected by the byte at `1fb00006`.
The source configures nine 8 MiB bank entries without masking the selector.
Gallop Racer 2 declares 40 MiB, providing five complete banks; entries outside
its actual ROM backing raise a native fault rather than reading beyond it.
Bank changes update physical/KSEG0/KSEG1 lookup mappings and invalidate compiled
fetches. Source-declared unpopulated ROM bytes retain zero padding.

`prepare_tecmo_tps_board.py` verifies the 13 source ROMs by size, CRC and SHA1,
preserves parent archive lookup and assembles the interleaved program ROMs.
Only `glpracr2` and `glpracr2j` common-map profiles are currently admitted.
The exact unused `upd78081` NO_DUMP declaration is omitted because this source
machine configures `znmcu_device`; no replacement firmware is written.
The native loader checks the exact 14-key configuration, source-sized regions,
each ROM's CRC/layout and all untouched padding bytes. Corrupting either an
actual program-ROM byte or a padding byte was independently rejected.

```sh
python3 scripts/prepare_tecmo_tps_board.py --set glpracr2 \
  --mame-source ../mamedev --rompath roms \
  --output-dir artifacts/boards-mac-20261014/glpracr2
```

## Native controls and sound

```sh
build/mac-harness-gnet-gl/pcsx-harness \
  --tecmo-tps artifacts/boards-mac-20261014/glpracr2 \
  --interactive --frames 4500 --max-blocks 800000000 --capture-gl \
  --inputs artifacts/boards-mac-20261014/glpracr2-player-inputs.csv \
  --nvram artifacts/boards-mac-20261014/glpracr2/eeprom.nvram \
  --audio artifacts/boards-mac-20261014/glpracr2-player-spu.wav \
  --mixed-audio artifacts/boards-mac-20261014/glpracr2-player-sdl.wav \
  --output artifacts/boards-mac-20261014/glpracr2-player-gl.ppm
```

For live play omit `--inputs`, `--frames`, `--capture-gl` and the capture outputs.
Keyboard controls are 5 for coin, 1/2 for start, arrows for P1, A/S for its two
buttons and Escape for pause. Mapped gamepads support the D-pad, south/east,
Start and Back/left-stick for coin; focus loss releases held controls. Source
`glpracr2` disables coin 2 and each player's third button, and the native control
API rejects them. Replay isolates inputs from live keyboard/gamepad events.

SPU DMA uses the source completion delay of 24000 ticks at 33.8688 MHz. Write
payload arrives at completion, read DMA completes immediately and retains MADR.
The native fixture verifies the payload boundary, read wrap and DMA IRQ.

Live sound uses the native SPU's 44.1 kHz stereo stream and the pinned ZN speaker
route gain of 0.35. The raw and SDL WAVs both contain 3,317,110 stereo frames.
All SDL samples exactly match the raw SPU samples scaled by that gain; SDL peak
is 11,468 with no stream error. Raw SPU hits full scale. This verifies capture,
sample cadence and routing, not full SPU sound fidelity against MAME.

## Regression and remaining configurations

```sh
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 tecmo-tps-test gnet-frontend-test capcom-zn-test
```

The explicit `--tecmo-tps-fixture DIR` path is restricted to board-test use and
rejects live inputs, NVRAM, sound capture and DRC. Its generated media are never
accepted by the normal loader. Executed MIPS reads and executes two different
ROM banks, exercises an aligned 32-bit write reaching selector lane 2, checks
all ROM aliases, the last bank, read-only ROM, source padding and idle SIO.
The normal loader rejects those synthetic ROMs and unsupported link profiles.
GPU upper-VRAM and source SPU DMA vectors also pass. Capcom's existing MIPS,
SIO/MCU/IRQ, EEPROM and QSound tests, G-Net SDL controls/mixer tests and the
no-board console smoke remain passing. A separate `GPU=neon HAVE_CHD=0` build
also passes the executed banking and SPU vectors. G-Net/GV storage integration
is compiled only when CHD support is enabled, so ROM-only TPS can link without
their disk backends.

The Japanese clone is source-admitted but not yet a gameplay representative.
Cool Boarders Arcade Jam adds Z80/YMZ280B and two FIFOs; Gallop Racer 2 Link adds
a separate Z80 link CPU. Those configurations require their own devices and
runtime evidence. This common-map pass does not validate them. The next board
is Taito FX1Z with RayStorm and G-Darius Ver.2. See
[the full board matrix](arcade-mac-validation.md).
