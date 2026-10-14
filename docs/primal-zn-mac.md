# Primal Rage II / Time Warner ZN-1 on Mac

Primal Rage II 0.36a (`primrag2`) passes bounded native Mac interpreter player
combat in GL and NEON. The 3600-frame replay consumes 82/82 input events and
enters Arik-versus-Tor combat, with the round clock at 51. A separate 3300-frame
warm-NVRAM run shows Arik's sword attack and damage to Tor at clock 56. Both
renderers show the same active player scene. HDD errors, VIA diagnostics and board
faults remain zero; the authentic run has no watchdog resets. Live SPU sound
and the exact source-gain SDL stream are nonzero and have no audio error.

This is bounded player coverage. Physical Vita, authentic ARM64 DRC, whole-game
timing/visual/audio fidelity and authentic HDD sector writes remain unverified.
The observed player sessions read the original HDD and do not write sectors.

## Source and original media

Source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`, `sony/zn.cpp`,
`primrag2_state::coh1000w`, its fixed-ROM/VIA maps, DMA delegates, watchdog,
`primrag2` six-button inputs and `ROM_START(primrag2)`. VIA and IDE32 controller
logic follows the same commit's `vt83c461.cpp`, `idectrl.cpp` and the ATA
wired-AND bus behavior. The existing ATA HDD core follows source
`atahle`/`atastorage`/`ide_hdd_device`, with its real busy/DRQ/IRQ timing.

The user-linked TeknoParrot collection supplies this game's entire package:
550,403,265 bytes, outer SHA1 `7985060299e4edef370c160964e3ade24d3f9a3a`.
Only its exact `coh1000w.zip`, `primrag2.zip` and `primrag2/primrag2.chd` entries
are extracted. Preparation audits all eight original ROM/EEPROM/key files by
source length, CRC and SHA1, preserving four program-chip byte interleaves.
It verifies CHD data and metadata with `chdman verify`, checks source overall
SHA1 `bc615068ddf4fd967f770ee01c02f285c052c4c5`, and validates HDD geometry.

The CHD is version 5, logical size 1,081,737,216 bytes, 2,112,768 sectors of
512 bytes, cylinders/heads/sectors 2096/16/63, with 4096-byte hunks. It has GDDD
geometry and no IDNT override; native uses the source IDE HDD identify profile
with that geometry. The source CHD is opened read-only; sector writes use a
separate `primrag2.overlay` and never modify the original image. Existing CHD
overlay persistence tests cover that backing; no authentic sector write was
observed in these player runs.

```sh
python3 scripts/prepare_primal_zn_board.py --set primrag2 \
  --mame-source ../mamedev \
  --rompath /tmp/pcsx-boards-mac-20261014/primrag2-tp \
  --output-dir roms/prepared/primrag2
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 BOARD=1 HAVE_CHD=1 -j8
```

Native independently checks the exact 14-key profile, all source ROM CRC/layout
and HDD SHA1/geometry. Authentic ROMs with a different otherwise valid HDD are
rejected. The source AT28C16 dump is retained as an immutable 2048-byte default;
runtime `--nvram` is a separate 2080-byte data/ID image. The exact unused
uPD78081 NO_DUMP declaration is omitted because source configures behavioral
`znmcu_device`; firmware and keys are not replaced. The BIOS root is not a game.
An explicit synthetic fixture bypasses media checks only for test-generated
guest code and disk; normal loading rejects that package. Without CHD support,
this board is explicitly rejected while other ROM-only ZN families still run.

## Native board wiring

- COH1000W uses ZN-1 CXD8530CQ input clock 67.7376 MHz, **8 MiB RAM**, type-2
  CXD8561Q GPU and 2 MiB VRAM, common SIO/CAT702/MCU/AT28C16 and PSX SPU.
  Motherboard configuration reads 6A. Fixed ROM maps read-only at
  `1f000000..1f1fffff`, including physical/KSEG aliases. There is no TPS ROM bank.
- `1f7e4000..1f7e4fff` uses the source 16-bit VIA wrapper. Config window `+30`
  has byte-sized unknown/index/data registers; writes replace their entire
  source byte, even on high/partial lanes, rather than COMBINE_DATA. Task file
  `+1f0` and control `+3f0` preserve exact IDE32 branch priority and masks.
  A full multi-lane access does not iterate through all task-file registers.
- `1f7f4000..1f7f4fff` is a separate paired-data map. Reading halfword `+1f0`
  consumes two ATA words, returns the low one and saves the high one. Reading
  `+1f2` returns that latch without another device read. A dword observes those
  two operations in source order. Source writes here only log and deliver no
  HDD payload. VIA config and both board latches survive soft reset.
- `1f7e8000..1f7e8003` reads/combines the 16-bit IRQ control latch. ATA IRQ10
  is enabled by bit 0 and follows the source inverted callback expression.
  Changing control alone does not reevaluate an already-high ATA IRQ. Main
  status acknowledges device IRQ; alternate status preserves it. Native I_STAT
  retains its ordinary PSX acknowledgement behavior.
- DMA5 uses the source guest CHCR/BCR rules and RAM-size mask on the initial
  address. Reads copy successive ATA halfwords into 8 MiB RAM and finish
  immediately, retaining MADR. Writes invoke the source log-only delegate;
  they do not invent an ATA write transfer. Out-of-range host memory is rejected.
- The DS1232 watchdog is exactly 600 ms. Writes at `1f000000..1f000003` service
  its source halfword handlers. Expiry requests a reset at a safe native CPU
  boundary. RAM, EEPROM and source latches persist; ATA resets and the watchdog
  rearms on the existing absolute clock. A real guest boot-counter vector proves
  upper-RAM preservation across an intentional watchdog restart.
- Native IDE preserves the transfer width selected by ATA SET FEATURES. PC
  Card adapters retain their existing explicit lane-based width overrides.
- P1/P2 buttons 1–3 use their main input byte. Buttons 4–6 use the source P4
  byte, at bits 0/1/2 and 4/5/6 respectively. Coin GPIO counts only source bit 0.
- SPU DMA retains its 24000-tick source completion boundary. The shared fixture
  now chooses the actual RAM-size boundary, covering both 4 and 8 MiB boards.
  Live audio uses native SPU at 44.1 kHz and source speaker gain 0.35.

## Player evidence and reproduction

Evidence is under `artifacts/boards-mac-20261014`; logs/reference runs are under
`/tmp/pcsx-boards-mac-20261014`. At 1200 frames the initial disk loading has no
capturable GL frame; at 3000 native reaches a combat demo. Independent MAME
0.287 reaches the same demo from the same media. Neither establishes player
compatibility by itself. The first one-coin replay remains in demo with credit
1/2: the source EEPROM requires two coins. The validated `player2` replay adds
the second coin, then Start, character confirmation, movement and all six buttons.

The final run has 120,505 HDD sector reads and 117,770 DMA5 reads totaling
15,074,560 words, with zero media errors, VIA diagnostics or watchdog resets.
There are 3584 watchdog kicks. SPU and SDL each record 2,653,688 stereo frames
at 44.1 kHz, raw/SDL peaks 21,518 / 7531. Every SDL sample matches production
source gain 0.35. NVRAM is exactly 2080 bytes; the warm copy remains separate
and re-enters actual combat, consuming the 50 events due by frame 3300.

```sh
build/mac-harness-gnet-gl/pcsx-harness \
  --primal-zn roms/prepared/primrag2 --interactive \
  --frames 3600 --max-blocks 800000000 --capture-gl \
  --inputs artifacts/boards-mac-20261014/primrag2-player2-inputs.csv \
  --nvram artifacts/boards-mac-20261014/primrag2-player2-nvram.bin \
  --audio artifacts/boards-mac-20261014/primrag2-player2-spu.wav \
  --mixed-audio artifacts/boards-mac-20261014/primrag2-player2-sdl.wav \
  --output artifacts/boards-mac-20261014/primrag2-player2-gl.ppm
```

For live play omit replay/frame/capture flags. P1 arrows and A/S/D/Space/F/H;
P2 I/K/J/L and Z/X/C/V/B/N. 5/6 coins, 1/2 Start, 9 service, T test, Escape
pause. Mapped pads use D-pad, four face buttons and both shoulders, Start and
Back/left-stick for coin. Focus loss releases all controls; replay is isolated.

## Tests and remaining boards

```sh
make -f tests/Makefile.vt83c461 test sanitize
make -f tests/Makefile.primal-zn-bus test sanitize
make -f tests/Makefile.ata-storage test sanitize
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 HAVE_CHD=1 primal-zn-test tecmo-tps-test \
  cbaj-tps-test taito-fx1z-test systemgnet-test gnet-frontend-test
```

VIA and board vectors pass O2 and ASan/UBSan with the real HDD backend: exact
byte/halfword/dword masks, paired data, config replacement/persistence, PIO
reads/writes, IRQ/status/control behavior, transfer width, DMA into upper RAM,
600 ms timeout/rearm and source log-only writes. Existing ATA vectors still pass.

Executed native MIPS vectors pass in GL and NEON: IDE32 latch, IRQ10, exact
512-byte DMA5 payload at upper RAM, initial 8 MiB mask, retained MADR, fixed ROM
endpoints/protection, six-button source GPIO, NVRAM round-trip and SPU DMA.
A 50-frame guest intentionally omits watchdog service and boots twice with
its upper-RAM counter preserved. The normal loader rejects synthetic ROMs and
the wrong HDD. Existing TPS, CBaj, FX1Z/G-Net and frontend regressions, earlier
Atlus/Visco/Raizing/Capcom regressions and no-board console smoke pass. ROM-only
TPS/CBaj also pass without CHD; Primal fails explicitly in that configuration.

The next distinct configuration is Acclaim ZN-1: NBA Jam Extreme's ROM/SRAM/RAX
sound, then Judge Dredd's disk map. The remaining scope stays in
[the full Mac matrix](arcade-mac-validation.md).
