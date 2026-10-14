# Metro ZN / Bust a Move 2: Dance Tengoku Mix on Mac

Original `bam2` (HDD) and `bam2a` (CD) ROMs reach a bounded P1 round in the
native Mac interpreter. GL and NEON captures show HEAT with score 662 and
`You Lose`; a separate no-input run stays in `DEMONSTRATION`. This is the dance
game, not Puzzle Bobble. ROM execution, controls and SPU playback have evidence;
the complete storage and music hardware remains unimplemented and unvalidated.

## Source and media

The reference is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`,
`src/mame/sony/zn.cpp`, `bam2_state`, its two machine configurations and inputs.
Both source games carry `MACHINE_NOT_WORKING | MACHINE_IMPERFECT_SOUND`.
The H8 disk controller firmware `hd64f3644.u2` is `NO_DUMP`. Source notes that
it checks the original Quantum Fireball CR4.3AT drive identity. Its MCU status
handlers are provisional and do not transfer ATA data. Native code preserves
that limitation, including error status for both drive checks; no replacement
firmware, invented successful drive authentication or game ROM patch is used.

Preparation audits 15 dumped chips against source size, CRC and SHA1: BIOS,
44 MiB banked ROM and two CAT702 keys. The common uPD78081 declaration is also
`NO_DUMP`; the existing source-derived behavioral ZN MCU handles its interface.
The undumped H8 is separate and has no native implementation.

| Original medium | Source CHD SHA1 | Geometry |
| --- | --- | --- |
| `bam2.chd` | `634d9a745a82c567fc4d7ce48e3570d88326c5f9` | 524 cylinders, 255 heads, 63 sectors; 8,418,060 sectors of 512 bytes |
| `bam2cd.chd` | `cdedc8f9eb67bed267c899d679f986bf1748bd00` | One MODE1_RAW/RW track, 327,795 frames; no CDDA tracks |

Both original CHDs pass full `chdman verify` before preparation. The runtime
opens the expected medium read-only and matches its header SHA1; it does not
repeat the full data verification or consume any disk sectors. Missing HDD,
the original CD under an HDD filename, malformed CHD, synthetic ROMs and
unsupported profile keys are rejected.

```sh
python3 scripts/prepare_metro_zn_board.py --set bam2 \
  --mame-source ../mamedev --rompath roms --output-dir roms/prepared/bam2
python3 scripts/prepare_metro_zn_board.py --set bam2a \
  --mame-source ../mamedev --rompath roms --output-dir roms/prepared/bam2a
```

## Native wiring and controls

COH1002E uses 4 MiB RAM, 2 MiB type-2 VRAM, the common ZN MCU/CAT702/SIO/AT28C16
devices and the original PSX SPU. Fixed ROM is `1f000000–1f3fffff`; a 4 MiB
bank window at `1f400000–1f7fffff` selects pages 0–15 and resets to page 1.
Only pages 0–10 exist in the dump. Invalid backing reads return FF and are
counted instead of reading beyond the allocated image. All three CPU aliases
map ROM read-only and invalidate banked code on selection changes.

Four 16-bit MCU ports occupy `1fb00000–1fb00007`. Offset 0 selects the bank;
offset 1 stores the command, including source nonmerging byte-write semantics.
The implemented status port is offset 2 (`1fb00004`), despite an older source
comment calling it port 1. Commands `007f` and `001c` return provisional error
1; other commands return source provisional status 4. Remaining ports have no
invented watchdog or disk behavior. Warm reset preserves RAM and the command.

Default cabinet is dedicated (`DSW=fe`); `--metro-cabinet generic` selects the
generic mapping (`DSW=ff`). CSV can also set `cabinet_generic` to 0 or 1.
P1 uses arrows, A Rhythm, S Jammar, 1 Start and 5 Coin; P2 uses I/K/J/L,
Z Rhythm, X Jammar, 2 Start and 6 Coin. Dedicated Down/K drive the pedals;
Start and Jammar share a physical line and held keyboard/gamepad aliases are
merged. Keypad 8/2 and Return drive dedicated P3 Up/Down Select and Enter.
Generic Start has separate SYSTEM bits. Test is T, Service1 is 9, and Escape
pauses. Unused controls remain unsupported. Production SDL tests exercise
cabinet routing, mixed keyboard/gamepad aliases and focus release.

## Observed runs and verification

Evidence is saved under `artifacts/boards-mac-20261014/metro/results.json` with
logs, captures, CSV, media manifests and hashes. The original 7000-frame replay
consumes 743/743 input rows on HDD/GL, CD/GL and HDD/NEON. All three finish at
PC `800341a8`, 3,962,840,035 cycles and 269,635,114 blocks with zero device
faults, 684 bank writes and 404 provisional status reads. Final RAM and raw
44.1 kHz SPU WAV are byte-identical:

* RAM SHA256: `8aff5a88b035e382deeb34ee77e03ebf288ff4b625ff2d7cbb39c0a7dc454f85`.
* WAV SHA256: `e90f5eaec8ab24d71c212a79b506fcd7f2802bcf186b08301dd927e06b1b1f75`.
* SPU: 5,159,948 stereo frames, peak 10,615.

A separate 5500-frame interactive GL replay shows the active HEAT round and
consumes 480/480 rows. SDL receives 4,054,244 frames at source gain 0.35,
3,706,625 nonzero frames, peak 3,715 and no audio errors. This validates the
ordinary SPU path; it does not establish disk music playback. Independent
MAME 0.287 HDD/CD attract captures show COMET and the dance instructions with
the same provisional source controller limitation.

```sh
make -f tests/Makefile.metro-zn-bus test sanitize
python3 -m unittest discover -s tests -p test_metro_zn_profiles.py
make -f Makefile.mac-harness GPU=vitagl-like metro-zn-test gnet-frontend-test \
  jdredd-zn-test nbajam-zn-test
make -f Makefile.mac-harness GPU=neon metro-zn-test
build/mac-harness-vitagl-like/pcsx-harness --metro-zn roms/prepared/bam2 \
  --metro-cabinet generic --interactive
```

Optimized, ASan/UBSan, GL/NEON native MIPS and shared Judge/NBA regressions pass.
A `HAVE_CHD=0` build compiles and rejects the Metro loader explicitly. Generated
MIPS fixtures are labeled synthetic and rejected by the original-ROM loader.

Open work: real H8/ATA transport and drive identity, disk music, dedicated/P2
gameplay, operator checks, complete game progression, authentic DRC and physical
Vita. Zero disk accesses in these runs means HDD/CD functional differences have
not been exercised. Metro therefore remains an open full-board validation row.
