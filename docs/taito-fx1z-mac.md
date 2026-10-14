# Taito FX1Z player validation on Mac

RayStorm (`raystorm`, Ver.2.06A) and G-Darius Ver.2 (`gdarius2`) pass bounded
native Mac interpreter/GL player runs. These cover COH1000TB with 1 MiB VRAM
and COH1002TB with 2 MiB VRAM, both with 4 MiB PSX RAM and ROM-backed Zoom sound.

| Representative | Observed player evidence | Input replay | Live audio | Devices |
| --- | --- | --- | --- | --- |
| RayStorm | PLAYER SELECT / R-GRAY1, then gameplay with movement, shooting and lock-on; final score 6700 | 96/96 events, 4300 frames | 3,169,536 stereo SDL frames at 44.1 kHz; SPU and Zoom nonzero; zero clips/errors | Zero adapter/Zoom faults and watchdog requests |
| G-Darius Ver.2 | PLAYER1 / 1P ship, Zone C, score 5700, shooting, movement and capture button | 112/112 events, 4200 frames | 3,094,528 stereo SDL frames at 44.1 kHz; SPU and Zoom nonzero; zero clips/errors | Zero adapter/Zoom faults and watchdog requests |

The source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`, principally
`sony/zn.cpp` and `sony/taito_zm.cpp`. Evidence, commands, captures, input replays,
raw Zoom/SDL WAVs, checksums and logs are stored under
`artifacts/boards-mac-20261014`, in `fx1z-validation.json`.

## Composition and authenticated media

The existing Taito motherboard adapter now chooses its actual source
daughterboard. FX1S keeps Z80/TC0140SYT/YM2610B; FX1Z creates MN10200, ZSG-2,
TMS57002, MB3773 and 512-byte FM1208S FRAM. No FX1S sound device is instantiated
for FX1Z. Bank selection retains source bits 0/1 at `1fb40000`, and bit 5 clocks
the active watchdog. FRAM and M66220FP shared RAM occupy the source's low byte
of each 16-bit word, with disconnected high-byte lanes left open.

Zoom now admits the source's read-only sample ROM region directly: 4 MiB for
these representatives and 6 MiB for Fighters' Impact profiles. The program ROM
is 512 KiB. ZSG reads beyond the declared ROM return zero as in MAME; the ROM is
not padded into G-Net's three-flash-chip geometry. The MN10200 starts immediately
for this ROM configuration. Its input clock is 12.5 MHz, internal cycle domain
6.25 MHz; ZSG-2 is 25 MHz and TMS57002 is 12.5 MHz. FX1Z deasserts sound reset at
startup and resets the actual sound CPU/devices during host machine reset,
retaining RAM, shared communication and absolute device clocks.

`prepare_taito_fx1_board.py` verifies all supplied ROMs by source size, CRC and
SHA1, with parent archive resolution and the reviewed unused-MCU omission.
The registry admits 21 game profiles across FX1S/FX1Z, but candidacy is separate
from observed game compatibility. The normal FX1Z native loader also audits each
ROM CRC/layout and all untouched padding bytes. Its explicit generated-media
fixture path is separate and does not weaken the normal loader.

```sh
python3 scripts/prepare_taito_fx1_board.py --set raystorm \
  --mame-source ../mamedev --rompath roms \
  --output-dir artifacts/boards-mac-20261014/raystorm
python3 scripts/prepare_taito_fx1_board.py --set gdarius2 \
  --mame-source ../mamedev --rompath roms \
  --output-dir artifacts/boards-mac-20261014/gdarius2
```

## Service, credits and persistent state

A fresh RayStorm memory image enters its service main menu, matching independent
MAME 0.287. Select EXIT (RESET) after the menu is ready; the native replay uses
six down pulses and button 1. The native 2080-byte AT28C16 and 512-byte FRAM both
match independently configured MAME NVRAM byte for byte. The combined native
`--nvram` file contains AT28C16 followed by FRAM and is exactly 2592 bytes;
wrong-size images are rejected. Source ROM packages remain immutable.

The first one-coin RayStorm experiment did not establish player gameplay and
its outputs are labeled `raystorm-one-coin-attract-*`. The passing replay uses
three coin pulses, P1 start and selection confirmations, and has a separate
PLAYER SELECT capture at frame 2750. The final gameplay/audio result uses
`raystorm-player-confirmed-inputs.csv`. MAME also reaches gameplay with sufficient
credit. A rendered demo and consumed input rows alone are not the acceptance gate.

## Native Mac play and repeat commands

```sh
build/mac-harness-gnet-gl/pcsx-harness \
  --taito-fx1z artifacts/boards-mac-20261014/raystorm \
  --interactive --frames 4300 --max-blocks 800000000 --capture-gl \
  --inputs artifacts/boards-mac-20261014/raystorm-player-confirmed-inputs.csv \
  --nvram artifacts/boards-mac-20261014/raystorm/nvram.bin \
  --audio artifacts/boards-mac-20261014/raystorm-player-zoom.wav \
  --mixed-audio artifacts/boards-mac-20261014/raystorm-player-sdl.wav \
  --output artifacts/boards-mac-20261014/raystorm-player-gl.ppm
```

For G-Darius use `--taito-fx1z .../gdarius2`, 4200 frames,
`gdarius2-player-inputs.csv`, and the corresponding `gdarius2-live-*` outputs.
For live play omit `--inputs`, `--frames` and capture flags/files. Keyboard:
5/6 coin, 1/2 start, arrows for P1 movement, A/S/D buttons, Escape pause.
Mapped gamepads support D-pad, face buttons, Start and Back/left-stick for coin.
RayStorm's source third buttons are disconnected and rejected by its API.
Focus loss releases live controls; input replay is isolated from live events.

SDL mixes the original SPU sample clock with the original Zoom sample clock
into 44.1 kHz stereo, using source speaker gains 0.3 and 1.0 respectively.
`--audio` records Zoom alone; `--mixed-audio` records the exact SDL stream.
These verify actual sound CPU/DSP output, cadence and routing. Full sound
fidelity and whole-machine timing equivalence to MAME remain unverified.

## Regression checks and remaining scope

```sh
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 taito-fx1z-test taito-fx1-test gnet-frontend-test
make -f tests/Makefile.zoom-board test sanitize
python3 -m unittest discover -s tests -p test_taito_fx1_boards.py
```

The generated-media native MIPS fixture exercises both VRAM configurations,
FRAM byte lanes/endpoints, shared-RAM lanes, source IRQ read, writes crossing the
IRQ register's open high word, banked ROM execution and 2592-byte NVRAM cold/warm
round trips. Normal loading rejects the synthetic ROMs. Source SPU DMA payload
and completion vectors pass. The same native vectors pass with NEON and GL while CHD
support is disabled. Zoom ROM/flash/reset tests pass under ASan/UBSan. Existing
FX1S, Tecmo TPS, G-Net frontend/mixer and no-board console checks remain passing.

Physical Vita, authentic ARM64 DRC and gameplay of the other regional/Fighters'
Impact profiles are separate checks. The next major board is Eighting/Raizing
ZN with Bloody Roar 2 and Brave Blade. See [the complete matrix](arcade-mac-validation.md).
