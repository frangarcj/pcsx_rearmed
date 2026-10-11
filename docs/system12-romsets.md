# System 12 from MAME romset zips

The core loads a Namco System 12 game straight from MAME romset zips. The
console needs no Python preparation step. The prepared region directories
(`scripts/prepare_system12_board.py`) still work and stay the format of the
validation harnesses.

## Use

Give the core the path of a zip in a rompath, for example
`ux0:data/pcsx-rearmed/roms/soulclbr.zip`. The loader indexes every zip in
that directory and finds each ROM file by CRC32 and size, not by name. It
searches the set's own zip first, then its parent's, then the other zips.
So merged, split and non-merged sets all work, and the zip names do not
matter.

The zip path selects the set like this:

1. If the zip's name is a set (as in MAME) and that set is complete, it is
   that set. The zip itself does not have to exist: `roms/soulclbrac.zip`
   loads from `roms/soulclbr.zip`.
2. Otherwise it is the complete set with the most of its own files in that
   zip. Own files are the files that the parent does not have; for a parent,
   all files. A `soulclbr.zip` that holds the program ROMs of `soulclbrac`
   loads `soulclbrac`, and the log says so.

`system12_romset_scan()` lists the complete sets of a rompath, for the
selector. System 12 has no BIOS set. If a file is missing, the log names it
and its CRC32. Every member is checked against its CRC32 while it is read.
SHA1 is not checked at load time.

Sets that the board adapter does not emulate (`unsupported` in
`scripts/system12_boards.json`) are refused before any ROM is read. A set
whose MAME declaration has an undumped file (`tektagtja3`) is refused too.

## Table

`libpcsxcore/arcade/system12_romsets.inc` holds, for the 58 sets of the
registry:

- the board profile;
- the ROM files (size, CRC32);
- the ROM_LOAD and ROM_FILL operations of the four board regions
  (`maincpu:rom`, `bankedroms`, `sub`, `c352`).

It is generated from the MAME source at the pinned commit:

```sh
python3 scripts/generate_system12_romsets.py --mame-source ../mamedev
```

The generator uses the same parser as `prepare_mame_psx.py`. It stops if the
MAME commit or the registry differ from the pinned ones, or if a set uses an
operation that the C loader does not apply.

## Validation (2026-10-11)

| Check | Result |
| --- | --- |
| `tests/system12_romset_fixture.c` with `roms/soulclbr.zip` (holds `soulclbrac`), `roms/soulclbrac.zip` (absent) and a copy named `micopia.zip` | each is `soulclbrac`; main, banked, sub and wave identical to `artifacts/system12-profile` |
| Mac harness, 1,800 frames from the zip and from the directory | PPM, WAV and System 12 report identical |
| `python3 -m unittest tests.test_system12_romset` | ROM_LOAD placement equals `prepare_mame_psx._apply_load` for 7 modes; stored and deflated members; corrupt member fails its CRC32 |
| `make -f Makefile.mac-harness board-test` | PASS |
