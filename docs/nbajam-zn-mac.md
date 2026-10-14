# NBA Jam Extreme / Acclaim ZN-1 on Mac

NBA Jam Extreme 1.10I (`nbajamex`) passes bounded native player gameplay on Mac
with GL and NEON. The 4800-frame replay inserts four coins, starts P1, advances
selection/tutorial and reaches Dallas–Seattle at 2:43, score 0–2. Both renderers
consume 114/114 inputs and report identical CPU cycles, PC, ROM-bank accesses,
RAX execution/commands and nonzero DAC counts, with zero board/CPU/DAC faults.

A control comparison retains the same coins, Start and selection but withholds
later buttons/movement. At frame 4800 it remains on CPU Help at tip-off, 3:00,
score 0–0. The active replay dismisses that help and enters live play. This
establishes player progression rather than inferring gameplay from attract mode.
The warm-NVRAM run re-enters play at 2:40 with Kidd as P1 and zero faults.

## Source and media

Pinned source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`,
`sony/zn.cpp`, `nbajamex_state`, `ROM_START(nbajamex)` and `shared/rax.cpp`.
The user's TeknoParrot package has 27,627,999 bytes, SHA1
`97177c97d1b240840cb978efca77b7443b16ded4`, and exact `coh1000a.zip` /
`nbajamex.zip` entries. All 22 original chips pass pinned length, CRC and SHA1.

```sh
python3 scripts/prepare_nbajam_zn_board.py --set nbajamex \
  --mame-source ../mamedev \
  --rompath /tmp/pcsx-boards-mac-20261014/nbajamex-tp \
  --output-dir roms/prepared/nbajamex
python3 scripts/generate_nbajam_zn_profiles.py --check
```

Preparation preserves original 512 KiB BIOS, CAT702 keys, 32 MiB interleaved
ROM and complete 8 MiB RAX region including source zero padding. Native loading
independently validates every source chip CRC/layout/padding. Altering a RAX
chip at offset 700000 or its untouched hole at 100000 is rejected. The exact
unused uPD78081 NO_DUMP declaration is omitted because source configures
behavioral `znmcu_device`; no substitute firmware is produced. The preparation
manifest records audited media, with runtime/player evidence kept separately.

## Native map and persistent memory

COH1000A retains the common ZN motherboard, 4 MiB RAM, 2 MiB type-2 VRAM,
CAT702, MCU, SIO and AT28C16. Two 16-entry selectors use 2 MiB strides into
original ROM. Raw 16-bit selector writes replace their latches even for byte
accesses; no COMBINE_DATA is invented. The 8 MiB map page and live selectors
reset to zero while raw latches remain, exactly as source.

Both map pages write the 32768-byte SRAM window at 1f200000–1f207fff. Page zero
reads SRAM; page one reads ROM across 1f200000–1f7fffff, including that same
SRAM write window. The low 2 MiB ROM window uses selector zero in both pages.
Internal bank-map holes return source default 00, while outer motherboard
unmapped/status lanes retain FF. Out-of-range ROM backing exposes a fault
instead of wrapping through an invented mask. ROM fetch mappings remain
read-only and are invalidated when banks change.

Runtime NVRAM is exactly **34848 bytes**: common 2080-byte AT28C16 data/ID image
followed by 32768-byte SRAM. Source has no EEPROM/SRAM dump for this set;
factory memory defaults to FF. Runtime storage is separate from immutable ROM
regions. Both main RAM and SRAM survive soft reset. The warm run imports actual
saved memory, makes 90 SRAM writes versus 128140 in the cold player run, and
reaches the court. Native MIPS reset vectors separately verify a preserved
4 MiB RAM boot counter incrementing to two.

Source status at 1fbfff08 is 0400 and sound read at 1fbfff80 is FFFF. Full
16-bit writes go to the real RAX input latch; guest replies do not replace that
source main-CPU read constant. Native MIPS executes the original-map SRAM/ROM
read asymmetry, both selectors and real DSP IRQ/read/reply while retaining
FFFF on the main sound read.

## Original sound and live controls

The [complete ADSP2181 core](adsp2181-port.md) and [RAX port](rax-sound-port.md)
execute original firmware at 16.67 MHz, including boot PM loading, five DM banks,
DMOVLAY, ROM banking, four BDMA types, deadlines/reset/IRQ, latches/PF0, SPORT0
and the two DMA-driven DAC channels. Source SCLKDIV sets the rate: this firmware
uses divider 32, giving integer DAC rate 31571 Hz. No command HLE is used.

The native main scheduler synchronizes RAX before each command write and
through normal elapsed PSX time. Each raw stereo frame carries its absolute
DSP clock. Frontend interpolation uses that clock through rate transitions,
with source RAX gain 1 and SPU gain 0.35, mixed to the SDL 44.1 kHz stream.
The stream epoch survives soft reset. MAME's sound-stream history resampling
and cycle-exact whole-machine audio remain outside this bounded validation.

The cold player run executes 1,337,474,547 DSP clocks, 351 commands/reads,
64,124 completed BDMA transfers, 3,458,786 transferred words and 19,789
buffer callbacks. It produces 1,750,962 nonzero raw stereo frames with zero
DAC overruns. SDL captures 3,538,176 stereo frames, peak 12728, 4,813,288
nonzero samples, zero clipping and zero errors. The actual game path's SPU
capture is silent; nonzero source-gain SPU/RAX routing is checked separately
with production callback vectors. The source SPU DMA retains its 24000-tick
payload boundary, MADR and RAM wrapping.

All four source players expose directions, four buttons, Start and coin.
Four rising-edge coin counters follow source order 0,2,1,3 without lockouts.
SDL tests use four virtual mapped gamepads and verify focus release. P1 uses
5/1, arrows, A/S/D/Space; P2 uses 6/2, I/K/J/L, Z/X/C/V. P3 uses 7/3 and the
numeric keypad (8/2/4/6, buttons 1/3/7/9). P4 uses 8/4, F1–F4 directions and
F5–F8 buttons. Gamepads map D-pad, face buttons, Start and Back/left-stick coin.
Replay isolates recorded inputs from live devices.

## Reproduction and checks

Artifacts are under `artifacts/boards-mac-20261014`, including player/passive
CSVs, cold GL/NEON and warm screenshots, NVRAM and original/final audio.
Logs and independent MAME 0.287 RAX traces are in
`/tmp/pcsx-boards-mac-20261014`. The passive comparison is a tutorial/control
check, not another passing player run.

```sh
build/mac-harness-gnet-gl/pcsx-harness \
  --nbajam-zn roms/prepared/nbajamex --interactive \
  --frames 4800 --max-blocks 800000000 --capture-gl \
  --inputs artifacts/boards-mac-20261014/nbajamex-player-inputs.csv \
  --nvram artifacts/boards-mac-20261014/nbajamex-player-nvram.bin \
  --audio artifacts/boards-mac-20261014/nbajamex-player-spu.wav \
  --mixed-audio artifacts/boards-mac-20261014/nbajamex-player-sdl.wav \
  --output artifacts/boards-mac-20261014/nbajamex-player-gl.ppm
```

For live play omit the replay/frame/capture flags. Existing NVRAM makes this a
warm run; use a new runtime path for a cold reproduction.

```sh
make -f tests/Makefile.nbajam-zn-bus test sanitize
make -f tests/Makefile.rax-sound test sanitize
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 HAVE_CHD=1 nbajam-zn-test gnet-frontend-test
python3 tests/check_nbajam_zn_harness.py \
  --harness build/mac-harness-primal-neon/pcsx-harness
```

Bus/CPU/DAC vectors and timed resampling pass O2 and ASan/UBSan. Native GL/NEON
MIPS fixtures pass source banks/SRAM, four-player inputs, exact NVRAM roundtrip,
actual DSP reply, SPU DMA and preserved RAM after guest reset. The ordinary
loader rejects the synthetic fixtures; only the explicit fixture CLI accepts
those test chips. Existing CBaj/TPS/Atlus/Visco/Raizing/Capcom native fixtures,
SDL frontend tests and no-board console smoke pass after integration.

Vita, authentic DRC, other NBA revisions and physical audio/timing fidelity
remain unvalidated. Judge Dredd has distinct HDD/IRQ/gun/VBlank wiring and is
the next Acclaim representative.
