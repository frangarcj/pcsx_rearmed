# Cool Boarders Arcade Jam TPS on Mac

Cool Boarders Arcade Jam (`cbaj`, Export) passes bounded native interpreter
player runs in GL and NEON. The 3100-frame replay consumes 200/200 input events,
selects Normal Round 1 and enters the downhill race. The capture shows P1 on the
course, time 25.443 seconds, countdown 14 and an exercised boost meter. A separate
1700-frame warm-NVRAM run shows the rider descending at 64 km/h. Both source
sound lanes are nonzero, with zero board faults, unsupported Z80 operations,
YMZ diagnostics, FIFO overflows/underflows, mixer clipping or audio errors.
Physical Vita, authentic ARM64 DRC and full hardware timing/fidelity remain open.

## Source and media

The reviewed source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`,
`src/mame/sony/zn.cpp`: `cbaj_state`, its inherited Tecmo map, `zn1p` inputs and
`ROM_START(cbaj)`. The FIFO port follows `src/devices/machine/7200fifo.cpp`
(BSD-3-Clause, hap). The existing native Z80 and YMZ280B execute the original
sound program and sample ROMs; no synthesized command responses are used.

The user-linked TeknoParrot collection does not contain this game. Its split
archive comes from `mame-0.260-roms-split_202310`: 16,031,449 bytes, SHA1
`7ba0fd2326b2d7e6fccf785d6c8288eea6e76d93`. The preparer checks all 23 original
chip files against pinned source length, CRC and SHA1, including the six GAL
dumps. The existing source-audited `coh1002m.zip` supplies the common BIOS/key.
Archive integrity alone does not establish runtime compatibility.

```sh
python3 scripts/prepare_cbaj_tps_board.py --set cbaj \
  --mame-source ../mamedev --rompath roms --output-dir roms/prepared/cbaj
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 BOARD=1 HAVE_CHD=1 -j8
```

The package preserves the two interleaved MIPS program chips, the 40 MiB banked
region's zero padding, complete 256 KiB Z80 ROM, 8 MiB YMZ samples and the six
279-byte GAL dumps at their source offsets in the 3072-byte misc region. Native
loading independently checks chip CRC/layout and unused zero padding. The GALs
are audited media; the source configuration does not instantiate a GAL device.
The exact unused `upd78081` NO_DUMP declaration is omitted because the source
uses behavioral `znmcu_device`. BIOS and CAT702 keys remain original. The bootleg
`cbajbl` is rejected and requires separate review.

There is no source EEPROM dump. AT28C16 starts erased; the game initializes it.
Runtime `--nvram` is a separate 2080-byte data/ID image. Cold and warm copies
remain separate from source ROMs and from each other.

## Native wiring

- COH1002M uses ZN-1 CXD8530CQ input clock 67.7376 MHz, 4 MiB RAM, type-2
  CXD8561Q GPU and 2 MiB VRAM, common SIO, CAT702, MCU and AT28C16.
- The inherited Tecmo ROM window is `1f000000..1f7fffff`; the raw low byte at
  `1fb00006` selects an 8 MiB entry. Source configuration specifies nine entries;
  only five fit this 40 MiB set. Unpopulated selections are rejected. KSEG aliases
  execute the same read-only mapping; reset selects entry zero.
- Two IDT7202 FIFOs each hold 1024 nine-bit words. Main writes at `1fb00000`
  enqueue a low byte to FIFO0; reads dequeue FIFO1. `1fb00003` returns FIFO1 EF
  shifted into bit 1: empty is zero, available is two. Dword reads process the
  data lane before reading status, matching source subunit order. Other lanes
  read FF and writes have no effect. Full writes are dropped and empty reads
  return FF, with diagnostic counts rather than fabricated replies.
- The real Z80 runs at 4 MHz, maps original ROM `0000..7fff` and RAM
  `8000..ffff`. Only the first 32 KiB of the full audited program region is
  mapped, as in source. I/O masks the port to eight bits: `84/85` YMZ, `90`
  FIFO0 read/FIFO1 write, `91` FIFO0 EF shifted into bit 1. IRQ/NMI are unwired.
  Unmapped ports return 00, the source address-map default; an executed Z80
  regression checks this independently of the mapped FIFO/YMZ ports.
- Sound execution chunks stay within the source 1/6000-second quantum. Actual
  Z80 bus timestamps preserve instruction overshoot; later budgets repay cycle
  debt. Soft reset preserves RAM and the absolute audio timeline while resetting
  CPU, chip and FIFOs. Callback reentry and time rewinds are rejected.
- YMZ280B runs at 16.9344 MHz / 192 = 88.2 kHz. Its IRQ is unwired in this board.
  Existing PCM8/PCM16/ADPCM synthesis retains source scaling, ROM mirroring,
  loops, interpolation and voice-end status.
- The source `zn1p` profile connects P1 directions and three buttons; P2 controls
  and a fourth button are rejected. Tecmo coin GPIO counts four source rising
  edges without lockout lines.
- SDL mixes source SPU gain 0.35 and YMZ gain 0.35 into 44.1 kHz stereo with
  the original clock/divider ratios. `--audio` records YMZ alone at 88.2 kHz;
  `--mixed-audio` records the exact SDL output.

## Player evidence and reproduction

Local evidence is under `artifacts/boards-mac-20261014`; scratch logs are under
`/tmp/pcsx-boards-mac-20261014`. Boot at 700 frames and the dark Round 1 intro
at 1300 are intermediate evidence. The intro's patterned fade appears in both
GL and NEON. Both renderers show the same active race at 3100 frames and report
the same sound/device counters; whole-game visual fidelity has not been compared
against hardware or a MAME player run.

The interactive 3100-frame run records 4,567,874 YMZ stereo frames, peak 28,936,
and 2,283,776 SDL stereo frames, peak 11,155. SPU/YMZ nonzero frame counts are
1,382,531 / 1,476,581. FIFO0 consumes all 1979 command bytes; FIFO1 consumes its
one real startup response. There are 5014 YMZ writes and 31 voice ends. Pending
mixer queues are 162/161 frames, below its 256-frame callback batch. Both FIFOs
finish empty, with no overflow/underflow. Warm NVRAM re-enters the race with zero
faults and consumes the 62 input events due before frame 1700.

```sh
build/mac-harness-gnet-gl/pcsx-harness \
  --cbaj-tps roms/prepared/cbaj --interactive \
  --frames 3100 --max-blocks 400000000 --capture-gl \
  --inputs artifacts/boards-mac-20261014/cbaj-player-inputs.csv \
  --nvram artifacts/boards-mac-20261014/cbaj-player-nvram.bin \
  --audio artifacts/boards-mac-20261014/cbaj-player-ymz.wav \
  --mixed-audio artifacts/boards-mac-20261014/cbaj-player-sdl.wav \
  --output artifacts/boards-mac-20261014/cbaj-player-gl.ppm
```

For live play omit replay, frame and capture flags. P1 uses arrows and A/S/D,
5 for coin and 1 for Start; 9 service, T test, Escape pause. A mapped gamepad uses
D-pad, three face buttons, Start and Back/left-stick for coin. Focus loss releases
live controls; replay remains isolated from live events.

## Regression coverage and limits

```sh
make -f tests/Makefile.fifo7200 test sanitize
make -f tests/Makefile.cbaj-sound test sanitize
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 HAVE_CHD=1 cbaj-tps-test gnet-frontend-test \
  atlus-zn-test visco-zn-test tecmo-tps-test raizing-zn-test \
  capcom-zn-test taito-fx1z-test systemgnet-test
```

FIFO vectors cover all three depths, nine-bit masking, wrap, thresholds, pin
levels, full/empty behavior, debugger peeks and reset. Sound vectors run real
Z80 ROM/RAM/I/O, PCM scale, dual FIFO ordering, single-cycle versus chunked
budgets, reset/debt and callback guards. Both pass O2 and ASan/UBSan.

Native MIPS vectors cover FIFO byte/word/dword lanes and post-pop status, source
banks/KSEG execution/endpoints/read-only ROM, original SPU DMA, P1-only inputs,
NVRAM round-trip, bootleg rejection and normal-loader rejection of synthetic
media. GL and NEON pass, including NEON without CHD. SDL verifies keyboard,
virtual gamepad/focus release and exact source SPU/YMZ gains/rates. Existing
Atlus, Visco, TPS, Raizing, Capcom, FX1Z/G-Net and mixer regressions pass; the
no-board console smoke still passes.

This is bounded `cbaj` coverage. TPS Gallop Racer 2 Link has a different Z80
link configuration and remains pending, as do the other rows in
[the full Mac matrix](arcade-mac-validation.md).
