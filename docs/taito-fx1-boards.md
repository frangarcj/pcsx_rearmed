# Taito FX-1 board profiles

`scripts/taito_fx1_boards.json` inventories all 22 Taito FX-1 `GAME` entries in the pinned MAME source `deb2f54fe7300e7a192690318beb860159bfd52a`, driver `src/mame/sony/zn.cpp`. The generator also asks the generic ROM parser to classify every one of the driver's 116 `ROM_START` declarations before writing the FX-1 registry. Profiles keep upstream parent, config, input, state class, flags, ROM-region geometry/media digests, and board facts.

| MAME configuration/class | Board | Entries | PSX/RAM geometry | Sound hardware | Scope |
|---|---|---:|---|---|---|
| `coh1000ta`, `taito_fx1s_state` | FX-1S / COH1000T | 10, including BIOS root | CXD8530CQ 67,737,600 Hz; CXD8561Q 53,693,175 Hz; 4 MiB RAM; 1 MiB VRAM | 4 MHz Z80, 8 MHz YM2610B, TC0140SYT, MB3773 | 9 game sets packageable by this preparer; BIOS root is recorded but rejected |
| `coh1000tb`, `taito_fx1z_state` | FX-1 ZROM / COH1000TB | 8 | Same ZN-1 CPU/GPU and RAM; 1 MiB VRAM | MN10200, ZSG-2, TMS57002, M66220 mailbox, FM1208S | Source-admitted FX1Z media; RayStorm has bounded Mac player evidence |
| `coh1002tb`, `taito_fx1z_state` and `gdarius_state` | FX-1 ZROM / COH1002TB | 4 | Same ZN-1 CPU/GPU and RAM; 2 MiB VRAM | ZROM sound subsystem; `gdarius_state` adds its own coin handling | Source-admitted FX1Z media; G-Darius Ver.2 has bounded Mac player evidence |

The exact groups are:

| Class/config | MAME short names |
|---|---|
| FX-1S / `taito_fx1s_state` | `coh1000t` (BIOS root), `sfchamp`, `sfchampo`, `sfchampu`, `sfchampj`, `psyforce`, `psyforcej`, `psyforcex`, `mgcldate`, `mgcldtex` |
| FX-1 ZROM / `taito_fx1z_state` | `raystorm`, `raystormo`, `raystormu`, `raystormj`, `ftimpact`, `ftimpactu`, `ftimpactj`, `ftimpcta` |
| FX-1 ZROM / `gdarius_state` | `gdarius`, `gdariusu`, `gdariusj`, `gdarius2` |

All ten FX-1S entries declare input profile `znt2p`; the exact machine, input, and GAME metadata are retained per set in JSON. FX1S ROM region tags are `maincpu:rom`, `cat702_1`, `cat702_2`, `upd78081`, `bankedroms`, `audiocpu`, and `ym2610b:adpcma`. Sizes are respectively 0x80000, 8, 8, 0x2000, 0x1000000, 0x20000 for game sets (0x80000 in the BIOS root), and 0x200000 or 0x400000 for ADPCM-A samples. MAME provides the Japanese main BIOS and two CAT702 key regions in each set's declarations; the ROM resolver also checks parent archives and verifies every supplied file by exact size, CRC32, and SHA-1.

The common FX-1 BIOS macro declares `upd78081` firmware (`78081g503.ic655`, 0x2000 bytes, `NO_DUMP`). In the selected `taito_fx1s_state`/`coh1000ta` setup, the FX1S class configures the CAT702, Z80, YM2610B, MB3773 and TC0140SYT. It does not instantiate an `upd78081`; `zn_state::zn_base` separately instantiates the ZNMCU. The preparer therefore omits this one exact region without synthesizing firmware. The registry and manifest retain its tag, filename, size, source proof, and the unresolved dump limitation. The same source review now covers `taito_fx1z_state`/`gdarius_state` with `coh1000tb`/`coh1002tb`: they use the separate ZNMCU and Zoom devices, and do not instantiate the undumped MCU. Changed or unreviewed declarations remain rejected.

Generate or check the full profile file from local pinned MAME sources:

```sh
python3 scripts/taito_fx1_boards.py
```

Prepare a set from local ROM archives:

```sh
python3 scripts/prepare_taito_fx1_board.py \
  --set psyforce --mame-source ../mamedev --rompath /path/to/roms \
  --output-dir artifacts/taito-fx1/psyforce
```

The package contains `main.bin`, `banked.bin`, `audiocpu.bin`, `adpcma.bin`, both CAT702 key files, `board.ini`, and a digest/provenance manifest. Output replacement preserves unrelated files in the destination. Disk media, NO_DUMP files outside the reviewed unused MCU region, unexpected ROM tags/sizes, stale profiles, BIOS roots and unsupported subclasses fail closed. No ROMs are fetched. Every manifest records `board_execution_verified=false`: assembled ROM regions do not demonstrate complete board or game execution.

Run tests with `python3 -m unittest tests.test_taito_fx1_boards -v`.


FX1Z packages use `taito_zoom:mn10200` (512 KiB program ROM) and
`taito_zoom:zsg2` (4 or 6 MiB sample ROM), stored in the six-region adapter's
legacy `audiocpu.bin`/`adpcma.bin` slots. The generated 25-key profile records
MN10200 input 12.5 MHz (6.25 MHz internal cycles), ZSG-2 at 25 MHz, TMS57002 at
12.5 MHz, M66220FP and MB3773. All 21 game sets are now source candidates; that
classification is separate from runtime validation. See [FX1Z Mac evidence](taito-fx1z-mac.md).
