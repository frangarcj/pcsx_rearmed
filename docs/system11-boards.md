# Namco System 11 ROM preparation and profiles

`scripts/system11_boards.json` records all 36 `GAME` entries from MAME
`src/mame/namco/namcos11.cpp`, pinned to
`deb2f54fe7300e7a192690318beb860159bfd52a`. The profile stores the source
machine/input/parent/flags plus the COH board type, ROM banking map, KEYCUS
device configured by that machine configuration, and lightgun map. The loader
checks those source fields before accepting a set.

| MAME set(s) | Board | Banking | KEYCUS | Lightgun / MAME note |
| --- | --- | --- | --- | --- |
| `tekken`, `tekkenb`, `tekkenac`, `tekkenjb` | COH-100 | ROM8 | C406 | MAME machine config installs C406; board notes list Tekken as no physical KEYCUS |
| `tekken2` | COH-110 | ROM8 | C406 | — |
| `tekken2b`, `tekken2ub`, `tekken2jc`, `tekken2jb`, `tekken2a`, `tekken2ua` | COH-100 | ROM8 | C406 | — |
| `tekken2ud` | COH-110 | ROM8 | C406 | — |
| `souledge`, `souledgeuc`, `souledgea`, `souledgeua`, `souledgeja` | COH-110 | ROM8 | C409 | — |
| `dunkmnia`, `dunkmniajc` | COH-110 | ROM8 | C410 | — |
| `xevi3dg`, `xevi3dga`, `xevi3dgj` | COH-110 | ROM8 | C430 | — |
| `primglex` | COH-110 | ROM8 | C411 | — |
| `danceyes`, `danceyesu`, `danceyesj` | COH-110 | ROM8 | C431 | — |
| `pocketrc` | COH-110 | ROM8 | C432 | `MACHINE_NODEVICE_LAN` |
| `fambowl` | COH-110 | NONE | C432 | Rejected: upstream marks `MACHINE_NOT_WORKING`; MAME notes no physical KEYCUS |
| `starswep`, `starswepj` | COH-110 | NONE | C442 | — |
| `myangel3` | COH-110 | ROM8_64 | C443 | — |
| `ptblank2a`, `ptblank2b`, `ptblank2c`, `ptblank2ua`, `gunbarla` | COH-110 | ROM8_64 | C443 | Lightgun map |

The profile reflects MAME's instantiated devices and map selection. It is not a
claim about the populated silicon on every physical PCB. In particular, the
source has TODO comments for Tekken and Family Bowl KEYCUS presence.

Run the local preparer with a pinned MAME checkout and user-supplied media:

```sh
python3 scripts/prepare_system11_board.py \
  --set tekken --mame-source ../mamedev --rompath /path/to/roms \
  --output-dir artifacts/system11/tekken
```

It writes `main.bin`, `banked.bin`, `c76.bin`, `c76bios.bin`, `wave.bin`,
`board.ini`, and `manifest.json`. Regions are assembled by the shared
`prepare_mame_psx.py` parser using each set's source declarations, local ZIP
members/loose files, and exact MAME size/CRC/SHA1 validation. No ROMs are
downloaded. `banked.bin` is an empty file only when the source set has no such
region and the selected map has no banking.

The C76's 16 KiB internal program ROM is separate from the game's `c76` sound
program region. MAME declares it in `src/mame/namco/namcomcu.cpp` as
`c76.bin`, CRC `399faac7`, SHA1
`ceb184ef0486caf715dd997101999785f67a40b8`. It is mandatory; a missing or
mismatched BIOS fails preparation. The manifest records its provenance. The
loader also refuses any required `NO_DUMP`, CHD/disk media, source/profile
mismatch, or upstream `MACHINE_NOT_WORKING` game. `fambowl` is listed for scope
completeness but cannot be prepared by this command.

This produces verified raw memory regions and a declarative board profile.
The native harness accepts the directory with `--system11`; its adapter
connects the CPU, bank windows, C76, GPU and audio interfaces. Tekken, Tekken 2
and Soul Edge passed short native Mac gameplay checks on 2026-10-13; see
[System 11 Mac validation](system11-mac.md). Other sets remain unverified.
Tests use synthetic ROM regions for deterministic layout checks:

```sh
python3 -m unittest tests/test_system11_boards.py
```
