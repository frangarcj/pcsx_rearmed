# Ehrgeiz arcade and PlayStation on Mac

Both versions pass a bounded interpreter player-combat check with the native
GL and NEON renderers. Arcade reaches Stage 1 with Godhand against Inoba;
PlayStation reaches the same pairing, shows attacks/damage and later reaches
the player's continue prompt. This covers the fighting mode on Mac, not Quest,
every character, full completion, authentic DRC or physical Vita.

## Media and boot paths

Arcade is the World `ehrgeiz` EG2/VER.A set, pinned to MAME
`deb2f54fe7300e7a192690318beb860159bfd52a`. Its `coh700` configuration has
4 MiB RAM, 4 MiB main ROM, 28 MiB banked ROM, 512 KiB H8 ROM and 16 MiB wave
ROM. The original ZIP is 28516725 bytes, SHA1
`93374d614a71b10583cda53b1e5c4bc0e35f4a24`, from the public
[`mame-0.260-roms-split_202310` collection](https://archive.org/details/mame-0.260-roms-split_202310).
`prepare_mame_psx.py` checks the individual source-declared CRC/SHA1 values,
then `prepare_system12_board.py` packages the exact System 12 profile.
No BIOS, peripheral stub or boot patch was substituted.

The console disc is PAL `SLES-02111`, from the public
[disc archive](https://archive.org/details/SLES-02111). Raw MODE2/2352
`SLES_021.11.BIN` is 697570272 bytes, SHA1
`a9432aec09fd309ef2c66d14986d5e740da2e2be`; its 77-byte cue has SHA1
`c1fb7b18f0a7cf2a2e2142938424af910ec80043`. These hashes verify the archive
files; they do not claim an independent Redump audit. Normal CD identification
returns `SLES02111`, PAL, a single data track and last sector 296736.

The explicit local 512 KiB BIOS has SHA1
`10155d8d6e6e832d6ea66db9bc098321fb5e8ebf`, MD5
`924e392ed05558ffdb115408c263dccf`. The core identifies it as CEX-3000/1001/1002,
19951204, region A. Thus this is a PAL disc with the supplied American BIOS,
not a separately tested PAL BIOS. Boot reports `HLE=0`.

The Mac harness now accepts `--disc IMAGE.cue --bios BIOS.bin`, uses the
existing CD reader, region detection, real BIOS reset/boot and standard SIO
pad path. It does not extract the executable for an EXE-only test. The
bounded disc replay currently requires BOARD=1; interactive mode remains
restricted to the existing arcade profiles. Missing/short BIOSes and mixed
disc/EXE/board selections are rejected. Disc/BIOS output-path collisions
are rejected before emulation.

Console CSV names are `p1`/`p2` followed by `start`, `select`, `up`, `down`,
`left`, `right`, `cross`, `circle`, `square`, `triangle`, `l1`, `r1`, `l2`,
or `r2`. Arcade coin/button names are rejected for a console replay. Inputs
enter the standard pad's active-low SIO response. `--audio` captures native
SPU PCM at 44.1 kHz for console discs; arcade continues to use C352 at 88.2 kHz.

## Player evidence

Replay CSVs, logs, PNGs and comparison JSON are retained in ignored
`artifacts/boards-mac-20261014/ehrgeiz`; scratch data, RAM and WAV files are
under `/tmp/pcsx-ehrgeiz-mac-20261014`.

| Run | Result |
| --- | --- |
| Arcade, GL and NEON, 5500 frames | 250/250 events; coin/start, character selection, directions and three attacks. Stage 1 Godhand/Inoba, visible gunfire and active combat. Zero unmapped reads/writes and zero unsupported H8 opcodes |
| PlayStation, GL and NEON, 6100 frames | 414/414 events; Start, directions, Cross/Circle/Square. Intro video, fighting mode, Godhand/Inoba fight and player CONTINUE prompt. Separate GL frame-4200 image shows health bars, timer 57 and an attack knocking Inoba back |

The arcade run uses the default harness System 12 console timing profile:
33.8688 MHz, cycle multiplier 175. This is a gameplay check, not validation
of the optional 50 MHz System 12 source-timing build. Console region detection
selects PAL timing. Run times are unpaced host throughput, not interactive FPS.

Each pair reaches exactly the same PC, execution-block count and CPU cycle.
GL/NEON RAM and captured native WAV are byte-identical:

| Evidence | SHA256 |
| --- | --- |
| Arcade 4 MiB RAM | `80b336f78ceb408c90c65bd8ab0423fcf41a00567a6c96a67a18563397d6486c` |
| Arcade WAV | `5d9daf7d10eb869518cb0cf562f41fda79a1e832df1efb50bef6ae998b12f5e2` |
| Console 2 MiB RAM | `730c982002a673b03b3ff562f1fe7c35b5112c9625b9e2dfc3523eb15060411d` |
| Console WAV | `c8b06a21998b73b73c5628c51763a52faa0fb59c33fb1a059a0660b8278fdb9c` |

Arcade C352 produces 8083530 stereo frames at 88200 Hz, peak 13243. Console
SPU produces 5378644 stereo frames at 44100 Hz, peak 32768. Both contain
nonzero PCM; this establishes native sound generation, not perceptual or
reference-emulator audio equivalence. GL and NEON screenshots show the same
game state with their respective rendering/scaling; pixel identity is not
asserted.

## Reproduce

```sh
make -f Makefile.mac-harness -j8
make -f Makefile.mac-harness GPU=vitagl-like BUILD=build/mac-harness-primal-gl -j8

build/mac-harness-primal-gl/pcsx-harness \
  --system12 roms/prepared/ehrgeiz --frames 5500 \
  --inputs artifacts/boards-mac-20261014/ehrgeiz/arcade-player.csv \
  --capture-gl --audio /tmp/ehrgeiz-arcade.wav --output /tmp/ehrgeiz-arcade.ppm

build/mac-harness-primal-gl/pcsx-harness \
  --disc roms/retail/ehrgeiz-pal/SLES_021.11.CUE \
  --bios /Users/frangar/Downloads/SCPH1001.BIN --frames 6100 \
  --inputs artifacts/boards-mac-20261014/ehrgeiz/console-player.csv \
  --capture-gl --audio /tmp/ehrgeiz-console.wav --output /tmp/ehrgeiz-console.ppm
```

For NEON use `build/mac-harness/pcsx-harness` and omit `--capture-gl`.

Production pad/replay tests pass alongside the existing arcade SDL tests.
BOARD=0 and HAVE_CHD=0 variants compile; BOARD=0 explicitly rejects this
disc diagnostic. NEON EXE smoke passes. The GL EXE smoke's CPU-VRAM pixel
assertion fails (zero triangle/background, valid RAM sentinel) identically
with the unchanged HEAD frontend and the new frontend; that pre-existing
diagnostic limitation is recorded rather than counted as a passing regression.
Actual GL game frames above have been inspected separately.

Memory-card saves, warm restart, live console SDL audio, Quest/minigames,
PAL BIOS comparisons and long-term disc/protection coverage remain open.
Original media, generated packages, BIOSes, binaries and captures are not
committed.
