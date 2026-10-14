# System 573 representatives on Mac

System 573 native board execution remains pending. The first authenticated
media package is Hyper Bishi Bashi Champ EAA (`hyperbbc`), covering the base
flash-only hardware. Independent MAME 0.287 reaches a P1 minigame sequence
with the supplied flash/timekeeper dumps. These reference captures do not
establish PCSX compatibility.

The full family validation still requires three distinct configurations:

| Representative | Coverage | Current evidence |
| --- | --- | --- |
| Hyper Bishi Bashi Champ EAA (`hyperbbc`) | Base board, eight onboard flashes, timekeeper, cassette Y lamp outputs and three-player buttons | Eleven dumped chips audited; MAME attract and P1 minigames observed. Native integration pending |
| Dance Dance Revolution 2nd Mix (`ddr2m`) | Analog daughterboard, CD-ROM, cassette X, dance input multiplexing | Source configuration reviewed; cassette/CD carry source BAD_DUMP annotations. Media preparation and native runtime pending |
| Percussion Freaks 4th Mix KAA (`pcnfrk4mk`) | Digital daughterboard, CD-ROM, cassette ZI/serial ID, instrument inputs and multi-session unit | Source working representative selected; ID/CD carry source BAD_DUMP annotations. Media preparation, ZS01 security, digital sound devices and native runtime pending |

## Hyper Bishi Bashi media

Source is pinned MAME `deb2f54fe7300e7a192690318beb860159bfd52a`,
`src/mame/konami/ksys573.cpp`, `SYS573_BIOS_A`, `ROM_START(hyperbbc)`,
`ksys573_state::hyperbbc` and its three-player inputs. The selected standard
BIOS is `700a01.22g`, SHA1 `e1284add4aaddd5337bd7f4e27614460d52b5b48`.

The package verifies BIOS, the original 64-byte `h8a01.bin` response dump,
eight 2 MiB flash dumps and the original 8 KiB `876ea.22h` M48T58 seed.
No game CD or PC-card is declared by this configuration. Source selects cassette
Y and explicitly notes that this game's program does not check its security
chip; there is no EEPROM dump in the set. No fabricated cassette image is
emitted. The H8 firmware `hd6473644h.18e` remains NO_DUMP. MAME uses the separate
dumped response bytes at its H8 startup interface, rather than executing H8
firmware. Both facts remain explicit in the preparation manifest.

Downloaded archive identities are:

* `hyperbbc.zip`: 8,228,616 bytes, SHA1 `b12d39c993cd5d4b656d60ef91991b37456b4d3f`.
* `sys573.zip`: 225,992 bytes, SHA1 `25b7d9a938c92f10ae7c935e5aef9d76aefac744`.

`prepare_system573_board.py` requires the pinned clean driver, reviewed EAA
machine/input/init, exact flash geometry and standard BIOS. It rejects other
sets and BIOS choices until they receive their own source review. Every dumped
chip passes source size/CRC/SHA1 before any output is published. A one-bit
change in `876ea.31m` is rejected and produces no prepared directory.

```sh
python3 scripts/prepare_system573_board.py --set hyperbbc \
  --mame-source ../mamedev --rompath roms --output-dir roms/prepared/hyperbbc
python3 -m unittest discover -s tests -p test_system573_profiles.py
```

Three profile tests pass. `board.ini` and `manifest.json` state
`board_execution_verified=false`; no native loader currently accepts this
format. Prepared files retain the eight separate chip images, not a replacement
PS-X EXE. ROM/media files remain outside Git.

## Reference and native work

Evidence is under `artifacts/boards-mac-20261014/system573`. A cold 95-second
MAME run shows three COMPUTER players and CREDIT 0 in attract. A separate
125-second cold run inserts three coins, presses P1 Start and exercises all
three P1 colors. Reviewed 40/75/120-second captures show PINKROCKER versus two
COMPUTER players across ship alignment, a colored grid and a castle minigame.
This reference uses original ROMs and timekeeper; no patched game or synthetic
installation is supplied. MAME warns about the source-undumped H8 as expected.

Native integration must retain CXD8530CQ at 67.7376 MHz, 4 MiB RAM,
CXD8561Q type-2 2 MiB VRAM and source SPU gain 1. The 4 MiB flash aperture at
`1f000000` selects a bank with the low six control bits. Four populated banks
pair low/high Fujitsu MBM29F016A x8 chips; the existing flash engine has the
correct 2 MiB/04h/ADh profile. The M48T58 maps its byte devices over the
`00ff00ff` lanes at `1f620000–1f623fff` and requires real RTC/latch behavior.
Control/security readback, H8 response clocking, ADC0834, source input ports,
cassette/lamp lines, empty ATA slots and empty JVS/PC-card state need wiring.
None of those board requirements is proved by a successful media audit.

The analog and digital configurations remain separate open work; this base
flash representative cannot replace their CD transport, security or sound
validation. Physical Vita is also outside the present Mac evidence.
