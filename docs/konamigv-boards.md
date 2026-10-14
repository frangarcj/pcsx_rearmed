# Konami System GV media profiles

The source-audited inventory covers all 15 `GAME` entries from `konami/konamigv.cpp` at MAME commit `deb2f54fe7300e7a192690318beb860159bfd52a`. `scripts/konamigv_boards.py` extracts the declarations and ROM regions, then checks the set names and listxml metadata against `scripts/mame_psx_systems.json`. The checked-in result is `scripts/konamigv_boards.json`.

The BIOS root is `konamigv`. Its 512 KiB `maincpu:rom` declaration is the shared system BIOS; each game declares a 128-byte 93C46 EEPROM image and one read-only CD image. `kdeadeye` also declares a 512 KiB flash ROM. The common machine config declares a CXD8530BQ at 67.7376 MHz, 2 MiB main RAM, CXD8514Q at 53.693175 MHz with 1 MiB VRAM, NCR53CF96 SCSI at 16 MHz, XM-5401 CD-ROM, MB89371 DUART at 4 MHz, a 16-bit 93C46, and the PSX SPU at 33.8688 MHz. The BIOS root itself is deliberately not accepted as a game package.

Run the preparer with local ROM media and a CHD:

```sh
python3 scripts/prepare_konamigv_board.py \
  --set hyperath \
  --mame-source ../mamedev \
  --rompath /path/to/mame/roms \
  --output-dir artifacts/system-gv/hyperath
```

The ROM path follows MAME conventions: ROM files may be in the set ZIP or extracted set directory, and the disk image may be named `<set>/<disk>.chd` or `<disk>.chd`. `--archive` may provide an additional local ZIP. The program never downloads media. It checks ROM CRC/SHA1 through the shared MAME media reader and requires `chdman` to parse the CHD header, verify all hunks, check the MAME-declared CHD SHA1, and confirm recognized MAME CD track metadata. It accepts the legacy `CHTR` and current `CHT2` layouts and the track types recognized by MAME, without requiring a particular data/audio track arrangement. Use `--chdman` when the executable is not on `PATH`.

The output includes `bios.bin`, `eeprom.bin`, any source-declared flash image, the verified `.chd`, `board.ini`, and `manifest.json`. A package records its source regions and all hashes; it does not claim board or game execution. Existing unrelated files in the output directory are retained. Files listed by an existing manifest are replaced as package-owned media.

MAME marks seven CD declarations `BAD_DUMP`: `btchamp`, `lacrazyc`, `nagano98`, `powyak96`, `susume`, `tmoshspa`, and `weddingr`. The source profile preserves those labels. A locally supplied CHD still must match the declared SHA1 and pass structural verification; validation does not change MAME's known quality status. No System GV `NO_DUMP` ROM is currently declared.

## Source classes and remaining board I/O

The common `konamigv_state` class supplies the shared SCSI/DMA and common memory map. The extra source classes and machine maps are distinct:

| MAME set(s) | State/config | Source-declared extra I/O | Native adapter status |
| --- | --- | --- | --- |
| `hyperath`, `lacrazyc`, `naganoj`, `nagano98`, `powyak96`, `susume`, `weddingr` | `konamigv_state` / `konamigv` | Common P1/P2/P3_P4 ports | Native loader/common-board integration and generated MIPS fixtures. Hyper Athlete has a bounded original player run (`konamigv-mac.md`); the other common profiles remain unverified |
| `btchamp` | `konamigv_state` / `btchamp` | Two UPD4701A trackballs, serial reads/reset, LH28F400 flash | Native Mac flash/dual-trackball path. TEST initialization matches MAME flash byte for byte; GL/NEON P1 football/ball flight, 597/597 events and zero faults (`btchamp-mac.md`). Source NOT_WORKING/BAD_DUMP flags remain |
| `kdeadeye` | `konamigv_state` / `kdeadeye` | Two lightguns, buttons, LH28F400 flash | Native source flash/guns and Mac mouse/two-gamepad inputs. Original GL/NEON P1 shooting, offscreen reload and score 15 pass; CDDA/calibration/P2 remain open (`kdeadeye-mac.md`). MAME not-working flags retained |
| `tmosh`, `tmoshs`, `tmoshsp`, `tmoshspa` | `tokimeki_state` / `tmosh` | Sensor serial protocol, heartbeat/GSR inputs, printer state, printer timing/video | Native `tmoshs`/`tmoshsp` sensor and virtual-printer integration. Original `tmoshs` player and operator test print exercised; Plus game, base/alternate profiles and final report remain pending (`tokimeki-mac.md`). `tmoshspa` is marked not working |
| `simpbowl` | `simpbowl_state` / `simpbowl` | Four Fujitsu flash devices and trackball | Native Mac flash/trackball integration, original installation matches MAME; bounded GL/NEON player bowling with pins down verified (`simpbowl-mac.md`) |

The GAME macros also mark all sets `MACHINE_IMPERFECT_SOUND`; graphics are imperfect for the listed exceptions, and `btchamp`, `kdeadeye`, and `tmoshspa` are marked not working. These MAME flags are recorded per set in the registry.

## Common board and remaining game validation

The [native common-board adapter](konamigv-native.md) connects the seven shared
profiles to the Mac harness. Its generated MIPS interpreter/DRC fixtures
validate input, DMA completion/pending payload and NCR IRQ10 against a real
synthetic CHD. They do not establish authentic BIOS/CD boot or game execution.

The common source map places the NCR53CF96 registers at `0x1f000000–0x1f00001f` with an 8-bit lane, inputs at `0x1f100000`, EEPROM output at `0x1f180000`, the two DUART channels at `0x1f680000` and `0x1f680008`, and a watchdog/no-op write range at `0x1f780000`. SCSI DMA channel 5 transfers between the controller and PSX RAM. A board implementation needs the NCR53CF96, SCSI bus/CD-ROM sector and redbook audio behavior, correct byte-lane and DMA semantics, 93C46 persistence, and MB89371 behavior before it can make a boot claim.

The CD data and EEPROM must remain paired. The upstream driver comments that using the wrong or blank 25C EEPROM with a CD can produce `25C MBAD`. The media package therefore carries each set's EEPROM beside its own disk image; it does not generate or substitute EEPROM contents. The `tokimeki_state`, `simpbowl_state`, and machine-map variants also need their actual peripheral behavior before those games can be considered executable.

The upstream file is BSD-3-Clause licensed and credits R. Belmont and smf. No MAME implementation code is copied into the preparer or registry generator.
