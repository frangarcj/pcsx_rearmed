# Beat the Champ / System GV Sharp flash and dual trackballs on Mac

The native Mac interpreter now accepts the original `btchamp` GV053 UAA01
profile, executes its Sharp LH28F400 interface and two UPD4701A trackballs,
initializes the blank flash through the original firmware's TEST workflow,
and reaches a P1 football event with an actual ball-flight camera in GL and
NEON. The installed 512 KiB flash payload is byte-identical to MAME.

This is bounded player/peripheral validation, not full sports-event coverage.
The pinned source's `MACHINE_NOT_WORKING | MACHINE_IMPERFECT_SOUND` and CD
`BAD_DUMP` annotations are preserved. Native progress does not repair the
original dump or change those source flags.

## Audited media

Source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`,
`src/mame/konami/konamigv.cpp`. The shared 512 KiB BIOS has SHA1
`02a82a2fe1fba0404517c3602324bfa64e23e478`. EEPROM `btchmp.25c` is 128 bytes,
CRC `6d02ea54`, SHA1 `d3babf481fd89db3aec17f589d0d3d999a2aa6e1`.
The public MAME209RomsOnlyMerged ZIP is 151 bytes, SHA1
`45cdcfe5992e58165c7bd172550821ad3218cf7e`.

The CD comes from the public archive member
`MAME_0.185_CHDs_Merged/MAME 0.185 CHDs (merged) (a-g).zip`, inner
`MAME 0.185 CHDs (merged) (a-g)/btchamp/btchamp.chd`. Download size is
211263157 bytes, file SHA1 `c8869245269882c8c381fc98d519f7ca63e2d6b5`.
CHD v4 has source header SHA1 `c9c858e9034826e1a12c3c003dd068a49a3577e1`,
data SHA1 `e1a75a034d83cffa44268eb30653ba334cc6252d`, 400600512 logical
bytes, 9792-byte hunks and twelve tracks (one MODE1 and eleven audio).
The last logical LBA is 163626. The preparer's full CHD verification and
source ROM CRC/SHA1 checks pass; native loading independently verifies the
profile, original BIOS/EEPROM and CHD header identity.

## Native daughterboard

Flash at `1f380000–1f3fffff` uses source 16-bit transactions: 512 KiB storage,
Sharp IDs B0/ED, 64 KiB uniform erase blocks and a one-second erase status
interval. Word programming and stored byte order follow the pinned MAME
intelfsh16 engine. Original ROMs and the CD are never overwritten.

The two UPD4701 channels share `1f680080–1f680087`: player 1 occupies odd
byte lanes and player 2 even lanes, with X low/high followed by Y low/high.
The write at `1f680088` drives shared CS from bit 1 and inverted X/Y reset
from bit 0, including the source ordering of those pin changes. The source
no-op at `1f6800e0` is retained. Both absolute 12-bit input counts are sampled
once per emulated frame. Warm CPU reset retains peripheral state as the
source does; it does not synthesize an installed flash or trackball result.

CSV exposes `p1trackx`, `p1tracky`, `p2trackx`, `p2tracky` (0 through 4095)
and the source common GV buttons/coin/test inputs. In interactive Mac GL,
mouse motion controls P1 with normal X direction, and each of two mapped
gamepads supplies its own trackball. This differs from Simpsons' source
reversed X. Left/right mouse buttons remain buttons 1/2; keyboard coin/start
and buttons use the existing GV mapping.

`flash.nvram` has a 64-byte versioned header and a 512 KiB payload. It binds
the state to this exact set's BIOS/EEPROM/CD identities and stores a payload
SHA1. Missing state starts erased. Wrong binding, size or payload digest
causes an explicit rejection and preserves the invalid file. Saves use a
process-specific temporary file followed by rename. The shared helper retains
the existing Simpsons format and its process-restart/corruption regressions.

## Original installation and play

A cold boot with erased flash reports `FLASH ROM CORRUPT / DATA INVALID /
POWERUP FAILED` in both native and independent MAME. Hold `test` from frame
0 until 2400, then release it. The original game writes and verifies flash;
the 4800-frame native run reaches the title with zero device faults and
3148450 flash command writes. Independent MAME holds Service Mode for forty
virtual seconds, reaches its attract/high-score screens and saves exactly
the same 524288-byte flash payload, SHA1
`f7610eb14afc8ff8c95bdde5162d5d354bcbcc96`.

The first 7200-frame player replay consumes 760/760 events and has nonzero
SPU but ends on a Navy attract message; that image alone is not player
evidence. A later replay repeats Start after the boot sequence and shows P1
in the football event (`KICK 3 / YARD 10 / GOAL 0`, the P2 side invites a
coin). Subsequent positive Y trackball input produces the observed ball-flight
view. The final 5600-frame replay consumes 597/597 events, also exercises P2
raw counts, and ends with `track=708/120,123/456`, zero faults and 3008 CD DMA
requests. Second-player gameplay and a scored goal are not established.

GL and NEON reach the same PC `80030e40`, 3170272006 CPU cycles and
234804902 execution blocks. Their 2 MiB RAM and native SPU WAV match exactly:

| Evidence | SHA256 |
| --- | --- |
| RAM | `1a8ae3b95cbb093cdd0f91baac66389e559f8075cead010b53ad7df7b72a7512` |
| WAV | `a446cf19f8bc5cbd151aae9f024dd70562a7a7b318763a2bd9ecf3901238127d` |

The earlier SPU capture generates 4125216 stereo frames at 44100 Hz, peak
1102. At that stage CDDA position advances without a speaker mix; this does not validate
all music/speech. Existing GV interactive source SPU routing swaps channels
and uses gain 0.75; this particular replay captures the unscaled native SPU.
No new live SDL output comparison is claimed.

## Verification and reproduction

`make -f tests/Makefile.btchamp test sanitize` covers IDs, word program/erase,
busy deadline, byte/dword trackball lane wiring, shared latch/reset, masked
writes and no-op behavior with O2 and ASan/UBSan. The full NOR suite also
passes both configurations. Native common GV, Simpsons persistence and
Tokimeki printer fixtures pass; SDL tests cover unreversed mouse X and an
independent second virtual gamepad. HAVE_CHD=0 still compiles.

Evidence is retained in ignored `artifacts/boards-mac-20261014/btchamp`, with
scratch/reference media and complete RAM/WAVs under
`/tmp/pcsx-btchamp-mac-20261014`. To initialize an erased profile and replay:

```sh
build/mac-harness-primal-gl/pcsx-harness \
  --system-gv roms/prepared/btchamp --frames 4800 \
  --inputs artifacts/boards-mac-20261014/btchamp/operator.csv \
  --capture-gl --output /tmp/btchamp-install.ppm

build/mac-harness-primal-gl/pcsx-harness \
  --system-gv roms/prepared/btchamp --frames 5600 \
  --inputs artifacts/boards-mac-20261014/btchamp/kick.csv \
  --capture-gl --audio /tmp/btchamp.wav --output /tmp/btchamp-kick.ppm
```

Use `build/mac-harness/pcsx-harness` without `--capture-gl` for NEON. The
loader also accepts the old documentary unsupported-I/O string from already
prepared packages; new registry packages no longer list the implemented
trackball/flash paths as unsupported. `game_execution_verified=false` remains
a preparer provenance field, not a claim updated by executing the game.

Full bowling/baseball/football games, scores, two-player sessions,
authentic DRC, whole-machine save states and physical Vita remain separate
checks. Dead Eye, Metro, System 573 and Twinkle remain open in the full Mac
board matrix. ROMs, CHDs, binaries, NVRAM and captures are not committed.

## Source speaker follow-up, 2026-10-15

The shared GV source-clock speaker route now mixes swapped SPU channels at
gain 0.75 with unswapped CDDA at gain 1. See `konamigv-mac.md` for the clock
contract, the large-DMA correction, fractional-gain tests and separate live
Hyper Athlete validation. The preceding SPU-only paragraph and frame count
describe the earlier implementation; CDDA mixing is no longer pending here.

The original 5600-frame football replay now passes again on both GL and NEON,
using a scratch copy of the original installed flash rather than changing the
audited media directory. Both consume 597/597 inputs, finish at PC `80030e40`
with 3170272006 cycles / 234804902 blocks, retain `track=708/120,123/456`,
complete 3008 CD DMA requests and report zero device/peripheral/mixer faults.
The RAM SHA256 still matches the preceding baseline exactly. GL and NEON
also produce byte-identical raw SPU and complete speaker WAVs:

| Evidence | Result |
| --- | --- |
| Speaker | 4,127,958 frames at 44.1 kHz, peak 18456, zero clipping or pending SPU |
| Raw SPU | 4,127,958 frames, peak 1102 |
| Original CDDA | 2,268,976 decoded frames, 2,251,308 nonzero frames, peak 18456 |
| Raw SPU WAV SHA256 | `772a192647cf9e54a12cceb41e1e570f74dc0057946e086f461089452d1ccb33` |
| Speaker WAV SHA256 | `4112c967099e203df41e7148bc05bcb61bd3b7f9f0628ac1b95701e424a01b33` |

Add `--mixed-audio /tmp/btchamp-speaker.wav` to the player command, using
the current GL or NEON harness. Evidence is retained under ignored
`artifacts/boards-mac-20261015/konamigv/hyperath-audio` alongside the shared
GV checks. This follow-up verifies original CDDA reaching the speaker during
football; a complete event, scored goal, P2 game and the operator's spoken
left/right sound-test procedure remain separate checks. Source BAD_DUMP and
MACHINE_NOT_WORKING annotations are unchanged.
