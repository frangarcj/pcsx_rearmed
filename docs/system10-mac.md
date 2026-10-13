# System 10 native Mac validation

Validated on Apple M1 Pro, macOS ARM64, 2026-10-13, with the native PCSX
ReArcade harness, interpreter and Vita-path GL renderer. The three sets were
downloaded from the user's TeknoParrot collection link (`tp-roms_1`). Outer
package SHA1s match Internet Archive metadata, inner sets pass MAME 0.287 ROM
audit, and the preparer verifies source size/CRC/SHA1 against MAME commit
`deb2f54fe7300e7a192690318beb860159bfd52a`.

| Set | Media board | Observed scripted gameplay |
| --- | --- | --- |
| `mrdrilr2` | MEM(M), Intel 28F640J5 | America stage, 23 m depth and 2585 points |
| `mrdrilrg` | MEM(N), three K9F2808 NAND chips | America stage, 13 m depth and 2545 points |
| `gamshara` | MEM(N), two K9F2808 NAND chips | Active stage with player shots and three hits |

The Driller runs lasted 3600 frames and Gamshara 2500 frames, including boot,
coin/start, menu choices and gameplay. Final runs reported no unsupported
accesses or board faults. These are short checks, not complete-game coverage.
The other sets, add-on boards, DRC, interactive harness mode, physical Vita,
savestate completeness and persistent settings remain unvalidated.

## Real firmware fixes

* The arcade BIOS initializes SIO1 and polls its error flags. The console
  core's fixed `0x01a5` status kept all three games in the same loop. The
  adapter now connects the existing MAME-derived PSX SIO0/SIO1 devices, with
  IRQ7/IRQ8, byte/halfword/word lanes and the fixed 33.8688 MHz serial timebase.
  Serial storage and clock phase survive soft reset, matching MAME's lack
  of a SIO `device_reset` override.
* Unmapped I/O holes and writes to read-only delegates no longer become
  unsupported-device faults. The drive-presence register returns the pinned
  source's `cdrom_unk_r` value of 1. Actual ATA commands remain unsupported;
  profiles requiring CD/add-on hardware are still rejected.
* MEM(N) can erase 32-page blocks and program buffered 528-byte pages,
  including OOB. Programming clears bits, writes use the source's high-byte-first
  data strobes, and confirm/reset behavior preserves completed settings.
  The standalone NAND API stays read-only unless the caller supplies an
  optional writable backing. Runtime writes affect loaded RAM images only;
  source ZIPs and prepared files remain unchanged. Exporting those changes
  across emulator launches is pending.
* MEM(M) now handles Intel StrataFlash `E8` buffered programming: count,
  data, `D0` confirmation, status and aborts. The commit copies its byte
  buffer and reports its dirty span, following pinned MAME. The game reached
  this path while initializing its configuration flash.

## Audio capture

`--audio` captures the existing SPU output for System 10. The WAV header uses
the native core's sample cadence (`PSXCLK / 768`, 66075 Hz with source CPU
timing), without adding a replacement sound engine or resampling. All three
final WAVs contain nonzero stereo PCM. This verifies audio generation, not
auditory or sample-exact equivalence to MAME's 44.1 kHz output stream. The
capture contains generated chip output and does not pad initial muted time.

## Reproduction

```sh
python3 scripts/prepare_system10_board.py --set mrdrilr2 \
  --mame-source ../mamedev --rompath roms \
  --output-dir artifacts/system10-mac-20261013/mrdrilr2
make -f Makefile.mac-harness BUILD=build/mac-harness-system10 GPU=vitagl-like DRC=0 -j8
build/mac-harness-system10/pcsx-harness \
  --system10 artifacts/system10-mac-20261013/mrdrilr2 \
  --frames 3600 --max-blocks 350000000 \
  --inputs artifacts/system10-mac-20261013/driller-play.csv \
  --capture-gl --output artifacts/system10-mac-20261013/mrdrilr2-final.ppm \
  --audio artifacts/system10-mac-20261013/mrdrilr2-final.wav
```

Use `mrdrilrg` with the same replay and frame count. For `gamshara`, use
`play.csv` and 2500 frames. Evidence is in
`artifacts/system10-mac-20261013`: inspected PNG/PPM frames, WAVs, replay CSVs,
ROM manifests, MAME baseline, test logs and `validation.json`. ROMs and generated
evidence are not committed.

## Regression checks

Passed 441 independent crypto vector streams, existing read-only NAND tests,
new program/OOB/erase/reset/bounds tests, MEM(M)/MEM(N) board and NAND-wire
tests, Intel buffered commit/abort/bounds tests, six Python media-profile tests,
and native synthetic MIPS/SIO/input/bank/NAND-DMA/GPU/rejection fixtures.
Crypto/NAND and all NOR profiles also pass AddressSanitizer/UndefinedBehaviorSanitizer.
