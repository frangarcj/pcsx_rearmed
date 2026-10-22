# System 10, System 11 and Capcom ZN from MAME romset zips

The core loads Namco System 10, Namco System 11 and Capcom ZN games straight
from MAME romset zips, as it already does for System 12
([system12-romsets.md](system12-romsets.md)). The console needs no Python
preparation step. The prepared board folders (`scripts/prepare_system10_board.py`,
`prepare_system11_board.py`, `prepare_capcom_zn_board.py`) still work and stay
the format of the validation harnesses; the shelf prefers a prepared folder to
a zip of the same set.

## Use

Put the zips in a rompath, for example `ux0:data/pcsx-rearcade/roms/`. The
shelf lists every complete set of the four families, and a game launches as
`<rompath>/<set>.zip`. The loader indexes every zip in that directory and finds
each ROM file by CRC32 and size, not by name. It searches the set's own zip
first, then its parent's, then the other zips. Merged, split and non-merged sets
all work, the zip names do not matter, and the files that other MAME sets
declare are found wherever they are:

- System 11: the C76 BIOS (`c76.bin` of `namcoc76.zip`);
- Capcom ZN: the COH1000C/COH1002C/COH3002C BIOS and the QSound DSP firmware
  (`dl-1425.bin` of `qsound.zip`). A set with a choice of BIOS (`sfex`,
  `starglad`, `ts2`...) uses the one of the **Capcom ZN BIOS** option (Japan by
  default, or English (debug)); if the zips hold only the other one, the game
  loads with it and the log says so.

The zip path selects the set like this:

1. If the zip's name is a set and that set is complete, it is that set. The
   zip itself does not have to exist: `roms/tekken2.zip` loads from
   `roms/tekken.zip` when that holds the clone's files.
2. Otherwise it is the complete set with the most of its own files in that
   zip, and the log says so.

If a file is missing, the log names it and its CRC32. Every member is checked
against its CRC32 while it is read. SHA1 is not checked at load time (the
preparers do check it).

## What is built

For each set the loader builds, in memory, exactly the package that the
preparer writes: `board.ini` and the region files (`main.bin`, `banked.bin`,
`c76.bin`, `c76bios.bin`, `wave.bin`; `rawnand0.bin`...; `country.bin`,
`audiocpu.bin`, `qsound_samples.bin`, `qsound_dsp.bin`, `cat702_key0.bin`...).
The board loaders (`system10_load`, `system11_load`, `systemzn_load`) read them
as they read a folder: `libpcsxcore/arcade/arcade_media.c` serves a virtual
directory `mame-zip:<set>` through three calls (`arcade_media_open_text`,
`arcade_media_size`, `arcade_media_read`), and `arcade_media_read` builds a
region straight into the loader's buffer. Peak memory is one region plus the
ROM file being placed, not two copies of the game. `arcade_romset_stage()` makes
the directory and `arcade_romset_release()` removes it once the loader returns;
libretro and the Mac harness do both.

Files:

| File | Role |
| --- | --- |
| `libpcsxcore/arcade/mame_zip.c` | zip reader; the file is open only while a directory or a member is read, so an index of hundreds of zips holds no handles |
| `libpcsxcore/arcade/mame_romindex.c` | index of the zips of a rompath, search by CRC32 and size |
| `libpcsxcore/arcade/mame_romload.c` | placement of a ROM_LOAD in a region (shared with System 12) |
| `libpcsxcore/arcade/arcade_romset.c` | set table, identify/scan/stage |
| `libpcsxcore/arcade/arcade_media.c` | virtual package directory for the board loaders |
| `libpcsxcore/arcade/arcade_romsets.inc` | generated tables |

## Table

`arcade_romsets.inc` holds, for the sets the preparers accept (12 System 10,
35 System 11, 43 Capcom ZN):

- the ROM files (size, CRC32) and the `board.ini` text (one for each BIOS of a
  set that has a choice; its files are tagged with the choice);
- the files of the package, each with the ROM_LOAD/ROM_FILL operations that
  build it (the C76 BIOS and QSound DSP firmware are part of the package of the
  game that needs them).

It is generated from the MAME source at the pinned commit:

```sh
python3 scripts/generate_arcade_romsets.py --mame-source ../mamedev
python3 scripts/generate_arcade_romsets.py --mame-source ../mamedev --check
```

The generator reuses the preparers' registries, parser and `board.ini` writers.
It stops if the MAME commit differs from the pinned one, and it leaves out a set
that the preparers cannot package (it checks each choice of BIOS). A ROM_CONTINUE
may write over bytes already loaded, as MAME does (`dunkmnia`, `dunkmniajc` and
`primglex` declare "first & second half identical"): the later write wins, in
the preparer and in the loader. Any other overlap is still an error.

## BIOS choice

The Capcom ZN-1 games with two BIOS files (Japan, English (debug)) take the
choice from the option `pcsx_rearmed_arcade_zn_bios` ("Capcom ZN BIOS" in the
arcade options of the selector and the pause menu: Auto, Japan, English
(debug); Auto is Japan, MAME's default). The libretro core reads it when it
loads a game, and `arcade_romset_set_bios()` sets it for other callers. It
applies to the next game started.

## Flash and NAND of System 10

A System 10 game can write its MEM(M) flash or MEM(N) NAND. The loader hashes
the media in 16 KiB chunks when it loads them; when the game is unloaded
`system10_persist_export()` saves the chunks that differ to
`<save dir>/<set>.arcade-flash` (nothing if none does, and an old file is
removed if they match again), and the next load applies them
(`system10_persist_import()`). A file for other media, or a damaged one, is
refused and left as it is. It works for prepared folders and for zips, and it
does not need a second copy of the media in memory.

## Index of the zips

`mame_romindex_open()` keeps the last index of a rompath while the zip names,
sizes and times are the same, so launching a game after the shelf scan does not
read every zip's directory again; it needs only a `stat` of each zip. A change
to any zip builds a new index.

## Limits

- Only what the preparers accept: System 10 MEM(M)/MEM(N) sets without extra
  hardware, System 11 sets without CHD, Capcom ZN sets that are not BIOS roots.

## Validation (2026-10-22)

There are no game ROMs on the development machine, so the zips hold synthetic
files: each has the size and CRC32 that MAME declares (the last four bytes are
computed) and random content, which catches a wrong interleave, byte swap or
offset.

| Check | Result |
| --- | --- |
| `ARCADE_ROMSET_FULL=1 python3 -m unittest tests.test_arcade_romset` | all 90 sets (12 System 10, 35 System 11, 43 Capcom ZN): `board.ini` and every region file identical to the package that the matching `prepare_*_board.py` writes from the same data (58 s) |
| `python3 -m unittest tests.test_arcade_romset` | the smallest sets of each family, merged, split, renamed and deflated zips, identify and scan, a missing file (log names it), a corrupt member (CRC32), each BIOS choice and the fallback, a ROM_CONTINUE overwrite (`primglex`), the kept index |
| `python3 -m unittest tests.test_system12_romset` | System 12 unchanged on the shared zip index |
| `make -f tests/Makefile.arcade-catalog test` | shelf catalogue |
