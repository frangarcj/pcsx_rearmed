# Capcom ZN media profiles

`scripts/capcom_zn_boards.json` is a source-derived registry for all 47 Capcom-family entries in the pinned MAME revision `deb2f54fe7300e7a192690318beb860159bfd52a` (`src/mame/sony/zn.cpp`). It includes all 45 `capcom_zn_state` games and both `glpracr_state` games. The registry records each game's parent, machine/input config, flags, BIOS parent and options, ROM-region declarations, and the physical board geometry declared by its machine config.

| MAME config/model | ZN family | Games in registry | CPU | GPU | Main RAM | VRAM |
|---|---:|---:|---|---|---:|---:|
| `coh1000c` / COH1000C | ZN-1 | 10 | CXD8530CQ, 67,737,600 Hz | CXD8561Q, 53,693,175 Hz | 4 MiB | 1 MiB |
| `coh1002c` / COH1002C | ZN-1 | 8 | CXD8530CQ, 67,737,600 Hz | CXD8561Q, 53,693,175 Hz | 4 MiB | 2 MiB |
| `coh3002c` / COH3002C | ZN-2 | 29 | CXD8661R, 100 MHz | CXD8654Q, 53,693,175 Hz | 4 MiB | 2 MiB |

The registry's full set list is compactly represented by these groups:

| Machine/class | MAME short names |
|---|---|
| COH1000C | `coh1000c`, `glpracr`, `glpracrj`, `starglad`, `stargladj`, `ts2`, `ts2j`, `ts2ja`, `ts2u`, `ts2ua` |
| COH1002C | `sfex`, `sfexa`, `sfexj`, `sfexp`, `sfexpj`, `sfexpj1`, `sfexpu1`, `sfexu` |
| COH3002C | `coh3002c`, `jgakuen`, `jgakuen1`, `kikaioh`, `plsmaswd`, `plsmaswda`, `rvschool`, `rvschoola`, `rvschoolu`, `sfex2`, `sfex2a`, `sfex2h`, `sfex2j`, `sfex2p`, `sfex2pa`, `sfex2ph`, `sfex2pj`, `sfex2pu`, `sfex2u`, `sfex2u1`, `shiryu2`, `stargld2`, `strider2`, `strider2a`, `strider2u`, `techromn`, `techromna`, `techromnu`, `tgmj` |

The COH1000C group contains one BIOS root and both Gallop Racer subclass entries;
the COH3002C group contains the other BIOS root. The remaining 43 game entries
are `capcom_zn_state` candidates. The JSON records the exact machine/config mapping
for every set. Registry generation fails if the inventory, pinned source commit,
parser coverage, family count, or BIOS roots change unexpectedly.

Street Fighter EX, Star Gladiator and Rival Schools now have
[short Mac interpreter/GL gameplay validation](capcom-zn-mac.md), covering
all three Capcom board geometries. Other sets remain unverified.

## Packageable media scope

The current preparer accepts the 43 game profiles using `capcom_zn_state` and rejects both BIOS roots (which contain no game program) and both `glpracr_state` games (separate subclass/configuration). It also rejects any changed or additional region, disk/CHD requirement, required `NO_DUMP`, or source/profile mismatch. Supported packages contain the MAME regions `maincpu:rom`, `bankedroms`, `countryrom`, `audiocpu`, `qsound`, `cat702_1`, and `cat702_2`, plus the required QSound DSP firmware region.

The BIOS roots are `coh1000c` and `coh3002c`. COH1000C games declare Japanese (`j`) and English debug (`e`) BIOS images; the Japanese image is MAME's default. Use `--bios e` to select the English image. COH3002C has no selectable BIOS. MAME parent/archive lineage is searched, but each chosen file must still match the source-declared size, CRC32, and SHA-1. The QSound HLE firmware `dl-1425.bin` comes from `src/devices/sound/qsoundhle.cpp`: MAME declares a 0x2000-byte load followed by `ROM_IGNORE(0x4000)`. Accordingly the source archive file is verified as 0x6000 bytes using MAME's declared CRC/SHA1; only the loaded 0x2000 bytes are assembled as `qsound_dsp.bin`.

The Capcom BIOS macros also declare `upd78081` firmware (`78081g503.ic655`, 0x2000 bytes, `NO_DUMP`). For the reviewed `capcom_zn_state` machine setup, `capcom_zn()` configures the CAT702 devices, Z80, sound latch and QSound device; it does not instantiate an `upd78081`. The common ZN base creates the distinct `ZNMCU`. Therefore this one exact unused region is omitted from the media package without filling it with fabricated data. The registry and package manifest record its tag, size, source filename, proof scope, and undumped-firmware limitation. The exception is not applied to the Gallop Racer subclass or any changed declaration.

Generate the checked-in registry from the pinned local MAME checkout:

```sh
python3 scripts/capcom_zn_boards.py
```

Prepare a package from local ROM archives only:

```sh
python3 scripts/prepare_capcom_zn_board.py \
  --set ts2 --mame-source ../mamedev --rompath /path/to/roms \
  --output-dir artifacts/capcom-zn/ts2
```

The tool never downloads ROMs, does not fabricate undumped content, preserves unrelated files in the output directory, and writes `board_execution_verified=false`. Files include `main.bin`, `banked.bin`, `country.bin`, `audiocpu.bin`, `qsound_samples.bin`, both CAT702 key files, `qsound_dsp.bin`, `board.ini`, and a digest/provenance manifest. These are assembled board inputs, not proof that the adapter, sound path, protection, or game execution is complete.

Tests: `python3 -m unittest tests.test_capcom_zn_boards -v`.
