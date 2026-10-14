# Atlus ZN-1 / Heaven's Gate on Mac

Heaven's Gate (`hvnsgate`) has bounded native interpreter/GL player validation.
The 2400-frame interactive run consumes 198/198 input events and reaches Stage 1
combat: P1 Jin performs a two-hit strike, both health bars change, and the round
clock advances to 43. The authentic 68000 program services 82 command IRQs and
71 YMZ voice-end IRQs, producing live mixed SPU/YMZ280B stereo with zero board
faults, chip diagnostics, mixer clipping or audio errors. A separate warm-NVRAM
run re-enters combat, including an observed kick against a different opponent.
Physical Vita and authentic ARM64 DRC are outside this Mac result.

## Source and media

The reviewed source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`,
`src/mame/sony/zn.cpp`, `atlus_zn_state::coh1001l`, its main/sound maps, the
`zn2p` input profile and `ROM_START(hvnsgate)`. Chip synthesis follows the same
commit's `src/devices/sound/ymz280b.cpp` (BSD-3-Clause, Aaron Giles), with its
PCM8, little-endian PCM16, ADPCM, interpolation, voice loops and register logic.

The user's Internet Archive TeknoParrot collection contains Heaven's Gate.
Its outer ZIP size/SHA1 is checked before extraction, then all 14 original chip
files in its `hvnsgate.zip` / `coh1001l.zip` are checked against source length,
CRC and SHA1. Neither the outer checksum nor a successful parse establishes
runtime compatibility; the actual player run above provides that evidence.

```sh
python3 scripts/prepare_atlus_zn_board.py --set hvnsgate \
  --mame-source ../mamedev \
  --rompath /tmp/pcsx-boards-mac-20261014/hvnsgate-tp \
  --output-dir roms/prepared/hvnsgate
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 BOARD=1 HAVE_CHD=1 -j8
```

The package preserves source MIPS and big-endian 68000 ROM byte interleaves, the
32 MiB banked region's zero padding, and the complete 8 MiB YMZ sample region.
Its first 4 MiB is the explicit source `ROM_FILL(..., 0xff)`; the two original
2 MiB sample chips occupy the upper half. Native loading independently checks
source CRC/layout, unused zero padding and the reviewed FF fill. A mutation in
the first sample-region byte is rejected as a ROM_FILL mismatch.

BIOS `m534002c-17.ic353` is CRC `6721146b`; original CAT702 keys `at01.ic652`
and `at02` are `181668b3` and `55fe469e`. No firmware or keys are substituted.
The exact unused `upd78081` NO_DUMP declaration is omitted because the source
configuration uses behavioral `znmcu_device`. There is no source EEPROM dump:
the AT28C16 starts erased and the game initializes it. Runtime `--nvram` is a
separate 2080-byte data/ID image, with cold/warm copies kept outside ROM media.
The BIOS root cannot impersonate a game; synthetic media uses only the explicit
fixture path.

## Native wiring

- COH1001L uses source ZN-1 CXD8530CQ input clock 67.7376 MHz, 4 MiB RAM,
  type-2 CXD8561Q GPU and 2 MiB VRAM, common SIO0/1, CAT702, MCU and AT28C16.
- Main ROM maps at `1f000000..1f7fffff`, in four 8 MiB entries. The low byte at
  `1fb00002` selects `data & 3`. Physical/KSEG mappings execute that bank and
  remain read-only. Tecmo's `+6` selector and Raizing's ZNSECSEL do not select
  Atlus ROM banks; reset selects entry zero.
- `1fb00000..1fb00001` writes the source 16-bit generic latch. Its write
  callback has no mem_mask/COMBINE_DATA: partial byte writes replace the whole
  word with their shifted bus value. A dword at `1fb00000` writes both the
  low-word latch and the bank selector in byte lane 2.
- Latch pending asserts the MC68000's level IRQ3. Interrupt acknowledgement
  keeps the level asserted; reading the latch at `100000..100001` clears it.
  The source `sound_unk_w` write at that address only logs and is not an ack.
- The real MC68000 runs at 10 MHz, reads its original 256 KiB ROM at
  `000000..03ffff`, and has 64 KiB RAM at `700000..70ffff`. The YMZ address/data
  pair at `200000..200003` uses the low byte of the 16-bit bus. Chip status
  reads clear its independent level IRQ2. Audio is serviced at the next YMZ
  sample's CPU boundary, rather than waiting for a video frame.
- YMZ280B runs at 16.9344 MHz / 192 = 88.2 kHz. Its read-only 8 MiB source
  region mirrors over the 24-bit ROM bus. Source zero-time voice-end callbacks
  update IRQ/status before the next CPU access. Timestamped bus operations
  retain partial instruction state and refuse time rewinds/callback reentry.
- Source coin GPIO counts four counter inputs in their original ordering and
  exposes active-high lockout bits. The two-player input profile uses three
  buttons per player; additional buttons remain disconnected.
- SDL mixes original SPU gain 0.175 with YMZ gain 0.35 into 44.1 kHz stereo,
  preserving each oscillator/divider ratio. The chip's source synthesis scale
  is retained before speaker gain. `--audio` records YMZ alone at 88.2 kHz;
  `--mixed-audio` records the exact SDL stereo stream at 44.1 kHz.

## Player evidence and reproduction

Local files are under `artifacts/boards-mac-20261014`; scratch logs are under
`/tmp/pcsx-boards-mac-20261014`. The 600-frame capture shows the Atlus logo and
is only boot evidence. At 1200 frames the capture shows the start of Round 1.
The 2400-frame player capture shows active Jin-versus-Jin combat and a two-hit
strike. The 1700-frame warm-NVRAM capture shows an active kick against Gen.

The live run has 3,538,250 YMZ frames, peak 29,874; SDL records 1,768,960 mixed
frames, peak 10,463. Both source lanes are nonzero, source IRQ3 acknowledgements
match all 82 commands, and IRQ2 acknowledgements match all 71 voice ends. Mixer
residual queues are 166/165 frames, below its 256-frame callback batch. NVRAM is
exactly 2080 bytes; the original player image remains separate from the warm copy.

```sh
build/mac-harness-gnet-gl/pcsx-harness \
  --atlus-zn roms/prepared/hvnsgate --interactive \
  --frames 2400 --max-blocks 300000000 --capture-gl \
  --inputs artifacts/boards-mac-20261014/hvnsgate-player-inputs.csv \
  --nvram artifacts/boards-mac-20261014/hvnsgate-replay-nvram.bin \
  --audio artifacts/boards-mac-20261014/hvnsgate-replay-ymz.wav \
  --mixed-audio artifacts/boards-mac-20261014/hvnsgate-replay-sdl.wav \
  --output artifacts/boards-mac-20261014/hvnsgate-replay-gl.ppm
```

For live play omit replay, frame and capture flags. 5/6 coin, 1/2 start; P1 arrows
and A/S/D; P2 I/K/J/L and Z/X/C. 9 service, T test, Escape pause. Mapped pads use
D-pad, three face buttons, Start and Back/left-stick for coin. Focus loss releases
live controls, while replay is isolated from live events.

## Regression coverage and limits

```sh
make -f tests/Makefile.ymz280b test sanitize
make -f tests/Makefile.atlus-sound test sanitize
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 HAVE_CHD=1 atlus-zn-test gnet-frontend-test \
  visco-zn-test tecmo-tps-test raizing-zn-test capcom-zn-test \
  taito-fx1z-test systemgnet-test
```

YMZ analytical vectors cover PCM scale/pan, little-endian PCM16, ADPCM high-first
nibbles, loop/end IRQ, masks, status acknowledgement, ROM read latch/mirroring,
read-only memory, reset and cycle slicing. CPU vectors execute the real MC68000
through RAM/YMZ lanes, independent IRQ2/IRQ3, latch-read ack and the source no-op
write. Both pass ASan/UBSan. Unsupported DSP writes are exposed as diagnostics;
the source's DSP placeholder and ROM-handling TODO remain explicit.

Native MIPS vectors execute selected banks, cover endpoints/KSEG aliases,
byte/word/dword selector lanes, 16-bit latch replacement, the combined dword
latch/bank write, four coin GPIO lines, source SPU DMA, synthetic-media rejection
and BIOS-root rejection. GL/NEON pass, including without CHD. SDL tests cover
exact controls and production SPU/YMZ gain/rate routing; existing Visco, TPS,
Raizing, Capcom, FX1Z/G-Net and mixer regressions and no-board console smoke pass.

This establishes bounded Heaven's Gate player coverage. Whole-machine timing
and full sound fidelity versus MAME/hardware remain unverified. The next distinct
configuration, Cool Boarders Arcade Jam (Tecmo TPS Z80/YMZ280B with two FIFOs),
now has [its own bounded player validation](cbaj-tps-mac.md).
All remaining boards stay tracked in [the full Mac matrix](arcade-mac-validation.md).
