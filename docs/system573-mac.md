# System 573 representatives on Mac

Hyper Bishi Bashi Champ EAA (`hyperbbc`) passes a bounded native Mac interpreter
P1 run on GL and NEON with original source-verified media. Both show PINKROCKER
versus two COMPUTER players in a ship alignment minigame, consume 1902/1902
input rows and produce identical RAM/SPU WAV with zero device faults. This
covers the base flash-only board; analog/digital sound and CD configurations
remain open. Independent MAME 0.287 captures are separate reference evidence.

The full family validation still requires three distinct configurations:

| Representative | Coverage | Current evidence |
| --- | --- | --- |
| Hyper Bishi Bashi Champ EAA (`hyperbbc`) | Base board, eight onboard flashes, timekeeper, cassette Y lamp outputs and three-player buttons | Native interpreter GL/NEON P1 minigame, live source-gain SPU/SDL audio and complete flash/NVRAM persistence verified; full P2/P3 gameplay/operator checks pending |
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

Four profile tests pass, including the native per-chip CRC table against the
selected pinned source chips. Preparation keeps `board_execution_verified=false`
because a media audit is separate from runtime evidence. The normal native
loader requires this exact reviewed profile and all eleven size/CRC checks.
Prepared files retain the eight separate chip images, not a replacement PS-X
EXE. ROM/media files remain outside Git.

## Reference and native work

Evidence is under `artifacts/boards-mac-20261014/system573`. A cold 95-second
MAME run shows three COMPUTER players and CREDIT 0 in attract. A separate
125-second cold run inserts three coins, presses P1 Start and exercises all
three P1 colors. Reviewed 40/75/120-second captures show PINKROCKER versus two
COMPUTER players across ship alignment, a colored grid and a castle minigame.
This reference uses original ROMs and timekeeper; no patched game or synthetic
installation is supplied. MAME warns about the source-undumped H8 as expected.

Native integration retains CXD8530CQ at 67.7376 MHz, 4 MiB RAM,
CXD8561Q type-2 2 MiB VRAM and source SPU gain 1. The 4 MiB flash aperture at
`1f000000` selects a bank with the low six control bits. Four populated banks
pair low/high Fujitsu MBM29F016A x8 chips; the existing flash engine has the
correct 2 MiB/04h/ADh profile. The M48T58 maps its byte devices over the
`00ff00ff` lanes at `1f620000–1f623fff`, with elapsed BCD clock, R/W/ST latches,
leap/year carry and source century-enable behavior. SRAM starts from the original
dump; the RTC counters start from local machine time as the source RTC interface
does. The pinned weekday mask's 7-to-0 wrap is retained. Control/security reads,
H8 rising-clock/saturated response indexing, ADC0834 serial pins, source input
ports, cassette/lamp lines and empty ATA/JVS/PC-card state are wired.

Cassette Y uses the existing X76F100 state machine with the exact source factory
default: reset response `19 00 aa 55`, other memory zero. This source-default
device state is explicitly reported and does not become an authenticated EEPROM
dump. No game security check is bypassed. This exception is confined to this
reviewed profile, whose source declares no cassette image and whose program
does not check the chip. Future cassette-protected profiles require their own
original media and protocol integration.

SIO0/SIO1 retain source timers and pin state; cassette Y DSR is low. Source clock
selection, memory/reset hooks, I_STAT masks and delayed SPU DMA are connected
through the real core. CPU data reads use the flash command engine; instruction
fetches use an interleaved bank view with all three CPU aliases, read-only LUTs
and code invalidation when banks or flash backing change. Empty ATA reads retain
requested bits high. Unmapped MCU/control upper lanes and RTC disabled byte lanes
read zero, confirmed by independent MAME memory reads.

## Native player, sound and persistence evidence

The 7500-frame GL/NEON replay finishes at PC `80108eec`, 4,245,900,000 cycles
and 355,726,025 interpreter blocks. It exercises original firmware boot,
three coin pulses, P1 Start and all three colors. It does not attempt perfect
minigame timing or a complete tournament. Both runs have 23,643,199 flash byte
reads, four H8 clock edges, 404 RTC reads, three empty-JVS packets and no faults.
The game does not write flashes in these runs; source component/guest tests
separately exercise Fujitsu commands and persistence of mutable flash data.

* Final RAM SHA256: `792bd6a1677573696cb4d2c5567e15d282b7a7e203d60267e050a2c9a5ffd29c`.
* Raw SPU WAV SHA256: `87e55a783ab117cd258826113148b3505319ec404e5fab81765e38f945bae1df`.
* SPU: 5,525,776 stereo frames at 44.1 kHz, peak 10,606.

A separate 5000-frame interactive GL run consumes the 1102 events scheduled
within its frame bound and shows an active PINKROCKER round. SDL receives
3,682,938 stereo frames with source gain 1, peak 10,606 and no audio errors.
The raw and SDL WAV files are byte-identical (SHA256
`d12e2d1786609ef83a062d68e60e7efed1c6caa26cf26c2cb044873fc41d6eb0`).
The logged nonzero count is sample values, not stereo frames.

`--nvram` saves a 16,785,572-byte versioned/checksummed snapshot: 32-byte
profile header, all eight 2 MiB flash images, 8 KiB timekeeper and 132 bytes
of cassette memory. Original prepared files stay immutable. A separate cold/warm
restart retains flash/SRAM/cassette bytes; RTC registers refresh from machine
time on load as source does. Header, profile, length, reserved fields and CRC
are checked before mutating device backing. Protocol timers/flash command mode
are transient chip state and are not represented as NVRAM.

A complete 7500-frame original GL player replay after restoring the interactive
snapshot consumes 1902/1902 events, again shows PINKROCKER versus two COMPUTER
players and retains zero faults. Its RAM and SPU WAV match both original cold
GL/NEON replays byte for byte.

Generated MIPS fixtures test bank switching, Fujitsu ID paths, RTC byte
lanes, H8 pins, P3 Green, empty ATA, source SPU DMA and mutable flash/NVRAM
restore. Separate component traces exercise actual x8 flash programming and
preservation on soft reset. Fixtures are explicitly labeled synthetic; the normal original loader
rejects their BIOS/chips. Component O2/ASan/UBSan, GL/NEON native fixtures,
three-player SDL/focus tests, shared Judge/NBA/Metro regressions and a no-CHD
native fixture build pass. Game evidence is recorded separately from fixtures.

```sh
make -f tests/Makefile.system573 test sanitize
make -f Makefile.mac-harness GPU=neon system573-test
make -f Makefile.mac-harness GPU=vitagl-like system573-test gnet-frontend-test
build/mac-harness-vitagl-like/pcsx-harness --system573 roms/prepared/hyperbbc \
  --interactive --nvram /tmp/hyperbbc-state.bin
```

Interactive P1 colors are A/S/D; P2 Z/X/C; P3 keypad 1/3/7. Starts are 1/2/3,
coins 5/6, T Test, 9 Service and Escape pause. Three mapped gamepads have
independent face-button/Start routes. Directions and fourth buttons are rejected
by source API/CSV. P2/P3 native game rounds and operator lamp tests are still
pending even though their bus/SDL routing is checked.

The analog and digital configurations remain separate open work; this base
flash representative cannot replace their CD transport, security or sound
validation. Physical Vita is also outside the present Mac evidence.
Authentic DRC, full-tournament progression and operator checks remain unverified.
