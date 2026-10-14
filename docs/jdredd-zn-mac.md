# Judge Dredd / Acclaim ZN-1 on Mac

Judge Dredd Rev C (`jdredd`) passes a bounded native interpreter player run
with GL and NEON using the original source-verified ROM and HDD. The 4800-frame
replay restores cold NVRAM through the original START prompt, inserts coins,
starts P1, aims, shoots and reloads offscreen. Both final captures show score
100, health 100%, one credit remaining and refilled ammunition. All 238 input
rows are consumed; device/backing errors are zero.

The boot failure was a common ZN no-op read returning FF instead of the pinned
program map's default zero. That made the original IRQ dispatcher enter an
asynchronous transfer path during a synchronous HDD read. Correcting the source
bus value lets unmodified firmware consume the first sector and load the game.
Independent MAME 0.287 read taps confirm zero at `1fa30000` and `1fa40000`.

## Source and media

Source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`, `sony/zn.cpp`,
`jdredd_state`, `ROM_START(jdredd)`, `jdredd` inputs, `psx/irq.cpp`, GPU
`video/psx.cpp`, and ATA `bus/ata/ataintf.cpp` / `hdd.h` with its preserved
`atahle` / `atastorage` implementation. Rev B has its own HDD and is not claimed
by this profile.

The user's TeknoParrot package is 423454130 bytes, SHA1
`892638b1410c4c9ca8b6d8b13479cfbf20b036dd`. Exact contents are `coh1000a.zip`,
`jdredd.zip` and `jdredd/jdreddc.chd`. All five original chips pass pinned size,
CRC and SHA1. The unused uPD78081 NO_DUMP declaration is omitted only because
source uses behavioral `znmcu_device`; no replacement firmware is supplied.

Original CHD v5: 424257837 compressed bytes, 1082253312 logical bytes, geometry
2097 cylinders / 16 heads / 63 sectors, 2113776 sectors of 512 bytes. Source
SHA1 is `eee205f83e5f590f8baf36452c873d7063156bd0`; data SHA1 is
`83ed8df25d100b1c060f4dde2f162ba31803db7d`. `chdman verify` checks full data
and metadata before preparation. Source media stay immutable; optional runtime
sector writes use `jdreddc.overlay` separately. Source has no EEPROM dump;
common AT28C16 defaults to FF and exports the existing 2080-byte data/ID image.

```sh
python3 scripts/prepare_jdredd_zn_board.py --set jdredd \
  --mame-source ../mamedev \
  --rompath /tmp/pcsx-boards-mac-20261014/jdredd-tp \
  --output-dir roms/prepared/jdredd
python3 scripts/generate_jdredd_zn_profiles.py --check
```

## Native source wiring

COH1000A retains common ZN/MCU/CAT702/SIO/AT28C16, 4 MiB RAM and 2 MiB type-2
VRAM. The 2 MiB original ROM window is fixed and read-only. Parent bank and
auxiliary writes are source log-only operations. No invented ROM switching,
HDD DMA5 handler, watchdog or RAX daughterboard is added.

ATA CS1 is 1fbfff80–1fbfff8f; CS0 is 1fbfff90–1fbfff9f. Source 16-bit delegates
on the little-endian 32-bit bus target distinct halfword registers. A 32-bit
read at DATA consumes one data word and reads ERROR in the upper halfword,
unlike Primal's two-data-word VIA bridge. The abstract ATA interface starts
its wired-AND result at mem_mask; source HDD task-file writes ignore mem_mask
when storing their data. High-byte sector-count writes with a zero low byte
therefore select 256 sectors. Guest SET FEATURES transfer width is retained.
Status acknowledges ATA IRQ; alternate status preserves it. The native host
routes the real line to PSX IRQ10. Judge's I_STAT acknowledgement follows the
pinned PSX IRQ controller's `pending & enabled & written_data` contract.

Each rising VBlank toggles the source gun mux, starting with P2. The raw selected
X/Y latch is accepted only with `x > 0393 && x < 0cb2` and
`y > 002d && y < 0217`; offscreen positions retain previous coordinates until
software clears them. SERVICE bit 08 reports the active-low mux. Trigger is
SERVICE 40/80, second gun button is P1/P2 bit 10; unrelated directions, third
buttons and P3/P4 Starts remain unsupported as source declares.

GPU GP1(10h,08h) exposes raw X/Y; GP1(0dh) clears them. Repeated identical clear
commands must bypass the ordinary register cache after a new VBlank sample.
Normal GPU reset preserves the lightgun latch, matching source. Cold GPU init
starts with zero coordinates. These are hardware-facing GPU fields; no GPU
readback is used to synthesize shots.

SDL maps the rendered viewport to the source valid raw range. Left mouse is
trigger and right mouse is the second button. Offscreen clicks still deliver
trigger with invalid raw coordinates; focus loss releases buttons. A mouse
movement takes aim ownership from the P1 stick until another stick-axis event;
P2 has its own stick coordinates. Replay accepts raw 16-bit `p1gunx/p1guny` /
`p2gunx/p2guny`, separate from digital pressed values. Source SPU gain 0.35 is
routed to ordinary 44.1 kHz SDL playback. Live source-gain SPU/SDL game sound is validated below.

## Component and authentic player evidence

```sh
make -f tests/Makefile.zn-board test
make -f tests/Makefile.jdredd-zn-bus test sanitize
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 HAVE_CHD=1 jdredd-zn-test gnet-frontend-test
make -f Makefile.mac-harness BUILD=build/mac-harness-primal-neon \
  GPU=neon DRC=0 HAVE_CHD=1 jdredd-zn-test
```

O2 and ASan/UBSan cover ATA16 lanes, real HDD identify/sector read/write,
status/alternate IRQ handling, guest transfer width, ROM protection, VBlank
multiplexing and offscreen boundaries. Native MIPS guests exercise IRQ10,
CHD-backed sector data, GPU coordinate query/reset and source I_STAT masks.
Common no-op read tests cover all byte lanes and 8/16/32-bit widths; writes
do not create a latch. Production SDL tests cover mouse/stick aim, viewport,
offscreen trigger, focus release and replay isolation.

These fixtures are distinct from the authentic player evidence. At 4800 frames,
both authentic renderers report 28688 HDD sector reads, 3662 real IRQ10 rising
edges, no disk writes/errors and no board faults. Guns report 4800 rising
VBlanks, 4686 valid coordinate latches and 114 offscreen samples. Repeated
outside-screen shots reload ammunition, then valid aim/fire produces score 100
and leaves health at 100%. A control replay with only the same initial eight
restore/coin/start rows loses without shooting and reaches a movie screen.

A saved 2080-byte AT28C16 data/ID image survives a process restart. A warm
1800-frame run skips the cold restore prompt and reaches attract mode. A
4100-frame warm player replay consumes all 236 rows and reaches another player
shooting scene with score 300 and health 100%. These runs preserve immutable
ROM/CHD inputs separately from runtime EEPROM and the optional sector overlay.

Interactive GL sends 3538250 stereo frames to SDL at 44100 Hz, peak 7522,
3743090 nonzero samples and zero audio errors. Every output sample equals the
same run's captured raw SPU sample multiplied by source gain 35/100, with integer
truncation toward zero: zero mismatches. Raw SPU peak is 21494 in that interactive
run; the NEON run is also nonzero. Raw PCM across different pacing/rendering
runs is not claimed sample-identical.

The shared ZN change passes common O2/ASan/UBSan tests and GL native fixtures
for Judge, NBA, Primal, CBaj, Atlus, Visco, Raizing, Tecmo and Capcom. Authentic
NEON regressions retain NBA's player match (114/114 inputs, 2:43 / score 0–2),
Primal's player combat (80/80) and TPS Link's player race (138/138), all without
board faults. NBA's original RAX command/DMA/nonzero-DAC counters match its
previous validation.

## Reproduction and controls

```sh
build/mac-harness-gnet-gl/pcsx-harness --jdredd-zn roms/prepared/jdredd \
  --frames 4800 --max-blocks 800000000 --capture-gl \
  --inputs artifacts/boards-mac-20261014/jdredd-player-inputs.csv \
  --nvram /tmp/jdredd-cold-player.nv \
  --output /tmp/jdredd-player-gl.ppm
```

Use an absent NVRAM path for this cold replay; it includes the restore START.
For a restart, reuse the saved NVRAM and `jdredd-warm-player-inputs.csv` with
4100 frames. NEON uses the same cold replay without `--capture-gl`.
Interactive Mac: 5/6 insert coins, 1/2 Start, mouse aims, left click/A fires,
right click/S supplies the second gun button. Aim outside the viewport and fire
to reload; mapped gamepad sticks also aim. Escape opens pause.

`--capture-gl` is diagnostic-only. If no scene is presented in the last eight
VBlanks, the harness now captures the last displayed front buffer once on exit.
This fixes the missing final image for a held Judge scene. Gun samples remain
raw board inputs and never use GL framebuffer readback.

Evidence is under `artifacts/boards-mac-20261014`, including
`jdredd-player-validation.json`, GL/NEON captures, passive/warm captures,
input replays, audio gain checks, MAME read probes and regression logs. Prior
loader-failure evidence remains historical, superseded by this player pass.

Physical Vita, authentic DRC, a two-player match, operator calibration menus,
whole-machine save states and full game completion remain unverified. The
representative validation goal stays open for the remaining board families.
