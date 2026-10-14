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

## Targeted High Octane issue checks

[Upstream issue 8](https://github.com/Rinnegatamante/PCSX_AwO/issues/8)
reports a complete Vita crash when selecting a title-screen option in USA
and Japan console releases with High Octane. High Octane is this fork's
VitaGL HD renderer. A passing Mac OpenGL compatibility run cannot establish
that the hardware/GXM failure is fixed.

The additional USA archive is from the public
[`2024-sony-playstation-usa-hearto-1g1r-collection`](https://archive.org/details/2024-sony-playstation-usa-hearto-1g1r-collection).
Its 471506696-byte ZIP has SHA1 `c8d6c1c0c43cb3079ac51662231eb064375519bd`.
The raw 697565568-byte BIN has SHA1 `37c5746e0969d159f6b1111a2904e72857c252d0`,
ZIP CRC `25031f7d`; cue SHA1 is `197d3dafbd9369fee6514c6a5c78306b99375d38`.
The core identifies `SLUS00809`, NTSC, and the same real BIOS (`HLE=0`).

The additional Mac build uses `DRC=1 GPU_THREAD=1 GPU=vitagl-like`, with
default `VITA_GL_HD=1`. At runtime `--gpu-thread` enables the gpulib worker;
the newly shared `--gl-1x` diagnostic disables HD target scaling without
changing CPU, media, input or thread settings. Startup explicitly logs the
resolution setting and requested worker state.

| Configuration | Evidence |
| --- | --- |
| PAL, ARM64 ari64, HD, GPU worker | 6100 frames, 414/414 inputs, nonzero SPU, clean GL shutdown |
| USA, interpreter, HD, GPU worker | 6200 frames, 414/414 inputs, Godhand/Inoba combat, clean shutdown |
| USA, ARM64 ari64, HD, GPU worker | 6200 frames, 414/414 inputs, Godhand/Inoba combat, clean shutdown |
| USA, ARM64 ari64, 1x, GPU worker | Same final PC, block count and cycle as HD; RAM and WAV byte-identical |

USA ari64 HD/1x RAM SHA256 is
`346d27ab1402c55c88980e7e81c6c518f6856e84a861a1b451c3b019affca7c4`;
WAV SHA256 is `c42cb599809d7d9e6eda330c14c2d23370b5c89982062702c408d945e191ae85`.
Both produce 4554860 stereo frames at 44.1 kHz. Interpreter and ari64 do not
reach the same final fight state/cycle; these checks do not assert timing
equivalence between CPU implementations. No crash is reproduced in these
Mac runs, including the reported selection-to-gameplay transition.

Vita uses an additional dedicated VitaGL renderer thread and real GXM
framebuffer/MSAA allocation. Mac uses host OpenGL context handoff; it does not
execute those Vita-specific paths. The Japan disc, original 0.1.0 binary and
physical Vita remain separate untested cases.

## ARMv7/VitaGL diagnostic in Vita3K

The standalone frontend now accepts bounded console pad replays and presented
frame capture in the console build as well as arcade builds. It retains strict
CSV ordering and complete-event checks. Reports remain restricted to arcade
profiles; ordinary game selection without diagnostic arguments is unchanged.

The current arcade-enabled Vita link fails because newly added native arcade
adapters reference sound/storage modules not yet included in that build. This
is a build-integration limitation, not an Ehrgeiz crash. The console diagnostic
therefore uses the normal PlayStation build: ARCADE_SYSTEM12=0, VitaGL HD,
VITA_GPU_THREAD=1, ARMv7 ari64 and the exact audited USA disc/BIOS above.

An isolated Vita3K run reaches 3700/3700 frames and 240/240 replay events with
both pads released, then shuts VitaGL down cleanly. VPK SHA256 is
`609c7fe8be3604dfa7432212f5c80930cb093954f49aa754e2cf2ab961422dc2`;
host SHA256 is `18b9bf5b232747a238e7b6d7a5d4ad8d390a93aefacc06a57d5215b09b4af87f`.
All 1078 forwarded protected accesses have matched recoveries; none remains
unresolved. The host screenshots show player combat. A later HD replay at
4300 frames shows the complete arena and HUD; the earlier picture with absent
elements was taken before that gameplay scene was fully presented. The
960x544 guest PPM is completely black. The first audio statistics incorrectly
show zero because sample counting was gated by an arcade report path, while
console replays had no such path. That diagnostic bug has been corrected;
those earlier zero statistics are not evidence of silent game audio.

After correcting the sample-count gate, the rebuilt console VPK completes a
4300-frame HD replay with 240/240 inputs, clean shutdown and nonzero delivered
audio: 3160500 stereo frames at 44100 Hz, peak 32768, 2695199 nonzero samples,
zero SDL delivery errors. The presented host pictures show actual player
combat with complete HUD and arena. The guest PPM remains black, so the
automated readback-based `visual_pass` stays false. Execution, presentation
screenshots, delivered audio and PPM readback have distinct evidence.

This is **not a passing Vita validation**. `result.json` deliberately reports
`passed=false` even though the replay and shutdown protocol completed. It does
not reproduce the reported whole-system crash; neither does it establish
correct readback or a physical Vita fix. HD and 1x must be compared using
the actual pictures. A second run of the identical VPK/profile with `--gl-1x`
also reaches 3700 frames and 240/240 inputs, with zero unresolved protected
accesses and a clean shutdown. It has the same initially invalid audio statistics and
identical black PPM hash. Its later host screenshot shows a complete arena
and HUD. The initially reviewed HD host picture is earlier in the fight;
that picture alone cannot attribute the absent elements to HD. The later HD
4300-frame run also completes 240/240 inputs with a clean shutdown and zero
unresolved accesses. Disabling HD does not resolve the black readback.
Evidence is retained in ignored
`artifacts/boards-mac-20261014/ehrgeiz/issue8-vita3k`; the isolated host/media
and intermediate files are under `/tmp/pcsx-ehrgeiz-vita3k-20261014`.

## Shared GP0 bridge regression after Dead Eye

A later original Dead Eye scored-hit/reload replay exposed an independent
High Octane SIGBUS: one-word GP0 `01h` cache clear reached the native retail
five-word `DR_MOVE` parser and used nonexistent rectangle words. The gpulib
bridge now consumes GP0 control words separately; it retains VRAM fill and
drawing, with no added framebuffer readback. Its regression fails against
the old bridge and passes after the fix. See `kdeadeye-mac.md` for the actual
crash and scored GL/NEON comparison.

After this shared change, the exact audited USA disc and real BIOS complete
a fresh Mac HD interpreter run: 6200 frames, 414/414 inputs, Godhand/Inoba
combat, 4554860 SPU stereo frames at 44100 Hz, peak 32768 and clean shutdown.
This regression uses GPU_worker=0. It does not replace the earlier GPU worker,
DRC and Vita3K checks, and does not establish that issue 8's physical Vita
crash has the same cause or is fixed. Evidence is retained with the Dead Eye
run under `artifacts/boards-mac-20261014/kdeadeye`.
