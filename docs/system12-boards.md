# Namco System 12 profiles

`scripts/system12_boards.json` records all 58 game aliases declared by the
pinned `namcos12.cpp` at MAME commit
`deb2f54fe7300e7a192690318beb860159bfd52a`. Each entry records its machine
configuration, input profile, C++ state class, init callback, main RAM size,
reset-copy boot hack, alternate ROM bank behavior, Tag DMA behavior, and any
external hardware the native board adapter does not implement.

The current runtime supports the ordinary COH-700 board state for the
`coh700`, `coh700b`, and `coh716` configurations. It handles 4 MiB and 16 MiB
main RAM, the 12-byte reset copy used by `coh700b` and `coh716`, and the
alternate 2 MiB ROM banking used by `namcos12_altbank_state`. Main ROM, banked
ROM, sub CPU ROM, and C352 wave ROM allocations follow their exact MAME region
sizes; the game ROM window stays 2 MiB. Normal Soul Calibur region folders
without `board.cfg` retain the original COH-700 defaults.

To package a directory produced by `prepare_mame_psx.py`, run:

```sh
python3 scripts/prepare_system12_board.py assembled-system12 \
  --output-dir artifacts/system12-board
```

The output contains `main.bin`, `banked.bin`, `sub.bin`, `wave.bin`,
`board.cfg`, and `system12_manifest.json`. The packager verifies the generic
manifest's pinned MAME revision and every required assembled region's length,
CRC32, and SHA1 before copying. It preserves variable banked-ROM lengths and
sets the runtime profile from the registry rather than guessing from file
sizes. Unsupported profiles can be packaged for inventory, but the runtime
rejects their `board.cfg` before guest execution.

The special cabinet paths now implement gun counters and ADC conversion,
JVS input/coin/analog endpoints (including the Techno Drive EMIO/printer chain),
Kart Duel's link CPU map and calibration EEPROM, and the Mac CDXA SH7014/ATAPI
adapter. See [System 12 special cabinets on Mac](system12-special-mac.md) for
actual game coverage, controls and limitations. Tekken Tag's protection/DMA
path is implemented in the runtime; registry acceptance alone is not a claim
that every alias has received gameplay testing.

When the assembled manifest includes `at28c16`, the packager preserves it as
`eeprom.bin`. A CDXA manifest requires `--chd PATH`, which copies the local
disc after checking its v5 header SHA1 against the manifest declaration.
The runtime then checks the pinned game disc identity when opening it.

Validation includes all 58 source aliases and profile declarations, strict
region digest checks, a generic-manifest repack of the local Soul Calibur ROM
set byte-for-byte against the established raw-region output, the default
COH-700 harness, and a synthetic COH-716 16 MiB RAM / alt-bank / reset-copy
profile. This validates board memory and mapping behavior, not complete
gameplay coverage for every profile.
