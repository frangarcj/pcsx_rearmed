# Tokimeki Seal Version sensors and virtual printer on Mac

The authentic `tmoshs` BIOS, EEPROM and CD now execute in the native Mac GV
interpreter with GL and NEON. A bounded player replay reaches heroine selection
and a conversation/date scene; the game displays its heartbeat value. The
original operator PRINTER CHECK / TEST PRINT also produces an actual 800x600
RGB page through the game's own timed serial commands. This establishes an
operator test print, not a completed romantic-game result or a physical printer.
The original Plus (`tmoshsp`) now also reaches a bounded GL player scene with
nonzero SPU/CDDA sound and produces its 16-cell operator sheet on GL and NEON.
A separate extended player replay also generates the game's own sticker sheet
on both renderers, without test/service inputs.

## Audited media

Source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`,
`src/mame/konami/konamigv.cpp`. BIOS `999a01.7e` is 512 KiB, SHA1
`02a82a2fe1fba0404517c3602324bfa64e23e478`; EEPROM `tmoshs.25c` is 128 bytes,
SHA1 `f18a0974a6be69dc179706643aab837ff61c2738`. CD `755jaa01.chd` has source
CHD SHA1 `fc742a0b763ba38350ba7eb5d775948632aafd9d`, data SHA1
`4a02e0990bef2051e2403467a1228ad7dc1ce103`, twelve tracks and 127984 last LBA.
Full `chdman verify` and the preparer's ROM CRC/SHA1 checks pass. Native loading
independently checks the pinned profile, BIOS/EEPROM hashes and CHD identity.
The original media and mutable EEPROM remain separate.

## Native peripheral path

The sensor/printer port at `1f680080` / `1f680090`, GPIO identity rotation,
heartbeat latch and GSR ADC follow the source protocol in `tokimeki-io.md`.
Default simulated heartbeat input 31 represents 80 bpm; input zero selects
Direct mode. GSR takes a raw value from 0 through 128. Inputs supply measurements
to the game; the emulator does not invent its assessment or score.

The game's memory-in command captures the actual renderer output at the next
rising vblank. GL reads the presented framebuffer only for that command;
NEON uses its presented RGB buffer. The logical image is copied to source-sized
800x600 paper. A validated print command marks the printer busy for one virtual
second and optionally writes `print-0001.ppm`, then successive numbered pages.
Unknown printer OSD commands remain unmodeled, as in the reference driver.

The optional output directory must already exist:

```sh
python3 scripts/check_validation_storage.py --reserve-mib 128
make -f Makefile.mac-harness GPU=vitagl-like -j8
mkdir -p /tmp/tokimeki-prints
build/mac-harness-vitagl-like/pcsx-harness \
  --system-gv roms/prepared/tmoshs --interactive \
  --printer-dir /tmp/tokimeki-prints --frames 20000
```

Use `5` for a coin, arrows and `A` to select. `+`/`-` adjusts the simulated
heartbeat raw input by ten; `H` switches to Direct and sends a pulse.
PageUp/PageDown adjusts GSR by two. CSV controls are `heartbeat`, `gsr`,
`heartbeat_pulse`, `printer_empty` and `printer_fault`, in addition to the
source buttons, coin, service and test. Controls unused by this cabinet are
rejected rather than translated into additional buttons.

## Original-game evidence

Scratch evidence is under `/tmp/pcsx-boards-mac-20261014`. The 7200-frame player
replay consumes 98/98 events, reaches Shiori selection and displays heartbeat
080. The longer GL replay reaches the date/conversation scene. Both renderer
paths report zero peripheral and board faults. These runs do not finish a game
or exercise its final printed report.

For the operator print, press test at frame 3200 and release at 3216. At frame
4260 press Up, at 4320 Up again, then select at 4380 to enter PRINTER CHECK.
Select TEST PRINT at 5160; each direction/button is released twelve frames
later. Both 6000-frame GL and NEON replays consume 10/10 events, decode 44 printer
packets with zero rejected packets, capture twice and print once. Each
800x600 page contains the actual colorful Konami test artwork on white paper;
41 unknown OSD commands are recorded separately from device faults.

Both runs end at the same CPU cycle, block count and PC with zero board faults.
The exported pages show the same test artwork but are not pixel-identical:
GL includes scaling/raster differences and a black strip on the right. This
is a remaining printer-image fidelity issue; it does not count as byte-exact
reference output. NEON provides the cleaner logical image in this replay.

A separate original I/O CHECK replay enters that menu with the same test
sequence, then selects the first item at 4260. At 4500 it supplies GSR 64 and
heartbeat input 51; the 5100-frame screenshot displays `GSR SENSOR:40`, the
expected hexadecimal value. All 6/6 events are consumed with zero faults.

## Verification and limits

The component tests cover identity rotation, heartbeat deadlines, GSR serial
readout, packet rejection, one-second busy timing and all sixteen Plus cells,
under O2 and ASan/UBSan. `make -f Makefile.mac-harness tokimeki-test` uses an
explicit generated MIPS guest to exercise native GPIO, CPU-timed printer
pulses and capture of a real red GPU frame. The fixture cannot pass as authentic
media. SDL tests exercise the production heartbeat/GSR keyboard controls;
builds with BOARD=0 or HAVE_CHD=0 also compile and reject unavailable GV paths.

The `tmoshsp` Plus profile now has original-media player, mixed sound and
operator-page evidence below. Base `tmosh` and `tmoshspa` remain outside the
accepted native profiles. Existing preparer
manifests retain their documentary unsupported-I/O label; the loader accepts
that exact legacy label for the two supported profiles.

The shared GV source speaker mix is now integrated. Original Plus execution
below exercises nonzero SPU and CDDA together; complete base/Seal speech/music
coverage is still separate. Physical printing, the base/Seal final game report, authentic
DRC, physical Vita and whole-machine save states remain unvalidated.


## Original Seal Version Plus, 2026-10-15

The distinct original `tmoshsp` GE756 JAB set uses the same audited GV BIOS,
but EEPROM `tmoshsp.25c` has CRC `af4cdd87`, SHA1
`97041e287e4c80066043967450779b81b62b2b8e`. Its `756jab01.chd` was obtained
from [MDK](https://mdk.cab/game/tmoshsp): 157,560,800 compressed bytes, source
CHD SHA1 `b2c59b9801debccbbd986728152f314535c67e53`, file SHA256
`2a9be2517dd7fcd943dc682d3e7a14caacd0a7905be010fe37a76d7d2f32bec4`.
Full `chdman verify` and the preparer's source ROM/EEPROM/CHD checks pass;
MAME 0.287 `-verifyroms tmoshsp` reports the set good. The CD has one data
track plus eleven audio tracks, with last LBA 146,033.

Preparation retains a **single original CD file**. The prepared profile links
to it, while its small BIOS/EEPROM files and complete audit manifest remain in
the package. The loader still independently checks the original profile/media;
`game_execution_verified=false` is not rewritten into an automatic gameplay claim.

```sh
python3 scripts/check_validation_storage.py --reserve-mib 180
python3 scripts/prepare_konamigv_board.py --set tmoshsp \
  --mame-source ../mamedev --rompath roms \
  --output-dir roms/prepared/tmoshsp --link-chd
```

The original operator replay uses the same ten Test/Up/Select edges listed
above and completes 6,000 native interpreter frames on both renderers. It
enters PRINTER CHECK / TEST PRINT and sends 60 protocol packets, with zero
rejections, 18 real video captures and one printed page. Forty unknown OSD
commands remain explicitly counted, matching the source's unmodeled OSD path;
they are not silently treated as implemented commands. Both runs have zero
board/peripheral/mixer faults and identical main RAM, EEPROM and raw/mixed
WAVs before the raw captures are removed:

| Original GL/NEON operator artifact | SHA256 |
| --- | --- |
| 2 MiB RAM | `e0da2b6665909d697a3490925df7c1104ce17b0eaf6745c3c6bc5553be4a0066` |
| Raw SPU WAV | `bc20fd57d108c9b51b8d2c68eaefe65d039d8e740c49a6c0384d4c316003c8ac` |
| Speaker WAV | `0527333957ee9d2859cfc418a7c6044448b981327784510fbcbd3e6318982aff` |
| Persisted 128-byte EEPROM | `52ee4672ec9b5c19911cf3a1c76b7df5af9e07a530875d92089d2b48064f285c` |

The 800x600 original Plus sheet fills all sixteen source positions: the
Konami logo, fourteen repeated test scenes and the Plus title logo. Both
native outputs and the independently run MAME operator page were visually
inspected and retain that layout. PNG RGB pixels are **not identical** across
the renderers/reference, so these are layout/protocol observations, not
pixel-exact printer emulation.

A separate 9,000-frame GL player replay consumes all 98 coin/button/sensor
input events and reaches the original autumn 1996 date scene, with heartbeat
080 and credit zero. It has zero board/peripheral/mixer faults and plays
6,634,218 stereo speaker frames. The original SPU is nonzero; the CD path
decodes 431,179 CDDA frames, including 203,982 nonzero frames with peak 5,345.
The source route is SPU gain 0.75 with swapped channels plus CDDA gain 1;
the final speaker peak is 7,635, with no clipping or pending SPU samples.
Its recorded speaker WAV SHA256 is
`b10f6488d15e15df389d8ae37075ddf3d8668145a55fdc6f6b39c4d39f6cafcd`.
This does not complete the game or produce its final assessment/report.

Evidence is under ignored `artifacts/boards-mac-20261015/gv/tmoshsp`: exact
operator/player CSVs, media origin, preparation/unit-test output, runtime logs,
PNG scenes/pages, hashes and persisted EEPROM. Completed batches immediately
remove WAV/RAM/PPM intermediates under the storage policy. The retained
observation checker uses only those small artifacts, with Pillow for PNGs:

```sh
python3 tests/check_tokimeki_plus_observations.py \
  --evidence-dir artifacts/boards-mac-20261015/gv/tmoshsp
```

It checks all sixteen populated cells and fourteen repeated scenes in each
renderer/MAME page, original profile diagnostics and full event consumption,
recorded equal operator hashes, retained EEPROM and the player screenshot.
It explicitly labels audio verification as historical after cleanup; no raw
captures are recreated. The linked-media preparer suite passes 16 tests,
including reprepare, changed media, self-owned links and temporary-extraction
rejection. The separate extended player print is described below; live SDL
sound for Plus and physical printing remain open.

### Original Plus player sticker sheet

Extending the original player CSV to 20,000 frames consumes 318/318 inputs on
GL and NEON. The first 98 events are the bounded replay above; the extension
only adds P1 button presses, held for sixteen frames every 120 frames through
frame 19,800. The only controls are coins, P1 button 1, GSR and heartbeat.
There are no test/service edges, ROM patches or injected game results.

Both original-game executions send 58 printer packets with zero rejections,
capture sixteen video images and issue one print. Their 800x600 sheets contain
the Konami logo, fourteen identical Shiori festival images and the Plus logo.
The heroine artwork differs from the operator test artwork. The retained final
screen shows two heroines holding sticker sheets, collection dialogue and
CREDIT 0. This establishes the original player's virtual sticker output;
it does not establish every ending or a physical printer.

Both native runs end at cycle 11,173,961,591, block 1,045,900,507 and PC
`800da374`, with zero board/peripheral faults and equal persisted EEPROM.
The final RAM SHA256 matches across renderers:
`fdbc8e2dc0b0d820982112570b4f83ad3b18f390838994b2f65a06b515553b4a`.
GL source speaker diagnostics record 14,549,429 stereo frames, 203,982
nonzero CDDA frames, peak 8,492 and no clipping/mixer faults. The GL raw WAV
was pruned with its hash retained; the NEON extension deliberately writes no
audio capture, so this is not a new GL/NEON audio comparison.

The NEON follow-up retains only small PNGs, logs, CSVs, hashes and EEPROM.
Its temporary RAM/page/frame captures are hashed and removed in the runner's
cleanup block.

An independent MAME 0.287 replay consumes the same 318 inputs, using native
CSV frame numbers divided by 60 for its input times. At 334 emulated seconds
it also reaches the sticker-collection scene and has a populated 16-cell
player sheet. Its awarded artwork differs: native GL/NEON show card 635-C,
while this MAME replay shows card 748. This establishes reference player
progression and page layout, **not matching gameplay timing or result**.
Those remain open; the original operator comparison alone cannot prove them.
The MAME reference uses `-video none` to bypass the interactive startup warning
and writes only two PNGs, diagnostics and its own small EEPROM/configuration.

The observation check has an explicit extended-player option:

```sh
python3 tests/check_tokimeki_plus_observations.py \
  --evidence-dir artifacts/boards-mac-20261015/gv/tmoshsp \
  --with-player-sheet
```
