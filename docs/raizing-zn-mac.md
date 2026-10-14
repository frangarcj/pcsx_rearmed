# Eighting/Raizing ZN on Mac

Bloody Roar 2 World (`bldyror2`) and Brave Blade World (`brvblade`) have bounded
native interpreter/GL player validation. Bloody Roar 2 reaches Stun selection,
the Stage 1 versus screen and combat, including its fourth button on the P3
connector. Brave Blade reaches 1P gameplay with 6,120 points, movement and firing
after its long explanation sequence (the final 6700-frame check reaches 6,700). Both produce live SPU/YMF271 audio, retain
2080-byte AT28C16 images and finish with zero board/YMF faults and diagnostics.
This is Mac coverage; physical Vita and authentic ARM64 DRC remain separate.

## Source and media

The implementation is reviewed against MAME
`deb2f54fe7300e7a192690318beb860159bfd52a`, `src/mame/sony/zn.cpp`,
`raizing_zn_state::coh1002e`, its main/sound maps and the two World ROM declarations.
The preparer checks every original ROM's length, CRC and SHA1 before assembly:
13 source files for Bloody Roar 2 and 10 for Brave Blade. It preserves the
68000 program's big-endian byte interleave, the 24 MiB main region's source zero
padding, the 4 MiB sample ROM and the original 2048-byte EEPROM default.
Normal native loading independently checks source CRCs, layouts and padding in
all seven assembled regions. Generated media requires the explicit fixture flag.

Brave Blade uses Tecmo BIOS/key 0 despite selecting the Raizing machine:
`m534002c-61.ic353`, `mg01.ic652`, and game key `mg11`. Bloody Roar 2 uses
`m27c402cz-54.ic353`, `et01.ic652`, and `et03`. These are checked against their
own declarations. No keys or firmware are substituted. The exact unused
`upd78081` NO_DUMP declaration is omitted because the source configuration uses
behavioral `znmcu_device`, as in the other reviewed ZN adapters.

```sh
python3 scripts/prepare_raizing_zn_board.py --set bldyror2 \
  --mame-source ../mamedev --rompath roms --output-dir roms/prepared/bldyror2
python3 scripts/prepare_raizing_zn_board.py --set brvblade \
  --mame-source ../mamedev --rompath roms --output-dir roms/prepared/brvblade
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 BOARD=1 HAVE_CHD=1 -j8
```

These packages remain immutable; `--nvram` is a separate mutable 2080-byte
AT28C16 data/ID file. The independent MAME 0.287 run reaches Brave Blade's player
selection using its own NVRAM directory. Its resulting EEPROM differs from the
native player run at four bytes; equal whole-machine timing or NVRAM is not
claimed.

## Native wiring

- Source ZN-1 CXD8530CQ input clock 67.7376 MHz, 4 MiB RAM and type-2 GPU with
  2 MiB VRAM; common CAT702, MCU, SIO0/1 and AT28C16 remain active.
- The 8 MiB main ROM window is selected by `ZNSECSEL` at `1fa10300`, using its
  low two bits while preserving the common security mux writes. Physical/KSEG
  mappings execute the selected ROM; writes do not modify it. Tecmo's
  `1fb00006` selector does not select a Raizing bank. A source-configured fourth
  entry lies beyond these 24 MiB ROM regions; native bounds checks reject it
  rather than reading beyond the allocation. Both observed games use valid entries.
- Main writes at `1fb00000` set the 8-bit sound latch; `1fb00004` asserts
  MC68000 IRQ2 with HOLD semantics. CPU autovector acknowledgement clears IRQ2;
  the separate latch read at `180009` clears pending. A command alone does not IRQ.
- The real MC68000 runs at 12 MHz, reading ROM `000000..07ffff`, RAM
  `080000..0fffff` and YMF271 `100000..10001f` on the low byte of its 16-bit bus.
  Source ROM/nop writes and unmapped open-bus reads are retained. Reset preserves
  sound RAM, latch value and absolute clock time, while clearing pending/IRQ.
- The MAME-derived YMF271 uses a 16.9344 MHz oscillator, emits four outputs at
  oscillator/384 = 44.1 kHz, and retains FM/8-bit/12-bit PCM synthesis and source
  timer/status behavior. Its IRQ is not connected to the sound CPU in this map.
  The read-only 4 MiB ROM mirrors through the source 23-bit address space.
- SDL mixes source SPU gain 0.35 with YMF outputs 0/1 at unity into 44.1 kHz
  stereo. `--audio` records YMF alone; `--mixed-audio` records the exact SDL PCM.
  The mixer uses each oscillator/divider ratio explicitly. Brave Blade's 270°
  presentation uses the existing GPU rotation path.

## Observed player runs

Evidence is local under `artifacts/boards-mac-20261014`, with logs also under
`/tmp/pcsx-boards-mac-20261014`. Captures are actual rendered output.

| Run | Player evidence | Audio/diagnostics |
| --- | --- | --- |
| Bloody Roar 2, 2300 frames | Stage 1 Stun versus Busuzima combat; 332 input events before the capture boundary | 1,691,392 mixed frames; SPU/YMF nonzero; YMF peak 10,433, mixed peak 13,924; zero mix clipping/errors, board faults or chip diagnostics |
| Brave Blade, 6600 frames | Portrait 1P gameplay, score 6,120, firing and movement; 483 input events, with the final release scheduled beyond the capture | 4,865,024 mixed frames; SPU/YMF nonzero; YMF peak 7,749, mixed peak 11,468; zero mix clipping/errors, board faults or chip diagnostics |

The earlier Brave Blade captures at 1800/3000/5000 frames show explanation
screens. They are intermediate evidence, not gameplay. Final native checks
also consume 332/332 Bloody Roar 2 rows at 2300 frames and 484/484 Brave Blade
rows at 6700 frames, with zero faults. The latter capture shows 1P score 6,700.

For live play:

```sh
build/mac-harness-gnet-gl/pcsx-harness \
  --raizing-zn roms/prepared/bldyror2 --interactive \
  --nvram artifacts/boards-mac-20261014/bldyror2-live-nvram.bin
# Use roms/prepared/brvblade for the portrait shooter.
```

5/6 insert coins, 1/2 start; P1 arrows and A/S/D/Space; P2 I/K/J/L and Z/X/C/V.
Space/V are Bloody Roar 2's fourth buttons; Brave Blade rejects them. Gamepads
use D-pad, face buttons, Start and Back/left-stick for coin. Focus loss releases
live controls; replay input stays isolated from keyboard/gamepad events.
9 supplies source service1, T test and Escape pause.

## Regression checks and limits

```sh
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl GPU=vitagl-like \
  DRC=0 HAVE_CHD=1 raizing-zn-test gnet-frontend-test \
  tecmo-tps-test capcom-zn-test taito-fx1z-test systemgnet-test
make -f tests/Makefile.raizing-sound test sanitize
make -f tests/Makefile.ymf271 test sanitize
```

The native MIPS fixture executes selected banks and checks ZNSECSEL byte/word/
dword writes, upper selector bits, aliases, region endpoints, read-only ROM,
sound latch/IRQ2, fourth-button connectors, EEPROM round trips, source SPU DMA
and rejection of synthetic media by the normal loader. It passes with GL and
NEON, including a build without CHD. The real MC68000 standalone fixture checks
RAM byte lanes, YMF low-byte mapping, PCM, command/IRQ separation, acknowledgement,
clock slicing, callback guards and reset; it passes ASan/UBSan. YMF tests cover
analytical PCM scale, 12-bit packing, FM output, timers, ROM mirror/latch and
batch invariance. SDL keyboard/gamepad/focus/replay checks and existing TPS,
Capcom, FX1Z/G-Net regressions pass; the no-board console smoke passes.

Full sound fidelity and machine timing equivalence remain unverified. The
upstream YMF271 TODOs, including PFM and alternate loop, are retained; their use
is exposed in diagnostics. Other regional games, the different bootleg sound/
protection board and Metro ATA variants are outside these two reviewed profiles.
The remaining board families are tracked in [the Mac matrix](arcade-mac-validation.md).
