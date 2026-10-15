# System 573 representatives on Mac

Hyper Bishi Bashi Champ EAA (`hyperbbc`) passes a bounded native Mac interpreter
P1 run on GL and NEON with original source-verified media. Both show PINKROCKER
versus two COMPUTER players in a ship alignment minigame, consume 1902/1902
input rows and produce identical RAM/SPU WAV with zero device faults. This
covers the base flash-only board. DDR 2nd Mix now also passes a bounded analog/CD
P1 run and source-gain SPU/CDDA speaker mix; the digital configuration remains open. Independent MAME 0.287 captures are separate reference evidence.

The full family validation still requires three distinct configurations:

| Representative | Coverage | Current evidence |
| --- | --- | --- |
| Hyper Bishi Bashi Champ EAA (`hyperbbc`) | Base board, eight onboard flashes, timekeeper, cassette Y lamp outputs and three-player buttons | Native interpreter GL/NEON P1 minigame, live source-gain SPU/SDL audio and complete flash/NVRAM persistence verified; full P2/P3 gameplay/operator checks pending |
| Dance Dance Revolution 2nd Mix (`ddr2m`) | Analog daughterboard, CD-ROM, cassette X, dance input multiplexing | Original cassette/CD verified; source CR-589 transport and DDR analog multiplexor integrated. Native GL/NEON cold flash initialization completes and all eight programmed chips match MAME byte for byte. Native warm GL/NEON 1st Stage/PERFECT, 92/92 inputs and original nonzero SPU/CDDA decoding pass. Source gain-1 SPU/CDDA mix passes headless GL/NEON and live SDL. Full song/P2/operator coverage remains separate |
| Percussion Freaks 4th Mix KAA (`pcnfrk4mk`) | Digital daughterboard, CD-ROM, cassette ZI/serial ID, instrument inputs and multi-session unit | Seven original game/device chips audited, retaining source BAD_DUMP serial/CD status. ZS01 encrypted pin protocol passes optimized/ASan tests and original-cassette readback; ZI bus also passes complete encrypted protocol/original readback and timed DS2401 checks. Native digital loader, original CD, FPGA/MAS3507D sound and game runtime remain pending |

## Digital KAA media and cassette component

On 2026-10-15 the original `pcnfrk4mk` game and daughterboard ROMs pass a
reproducible source-pinned audit. The complete configuration includes more
than the game ZIP: the digital I/O board requires its own eight-byte serial,
and the multi-session unit declares a flash and timekeeper. MAME `-listroms
pcnfrk4mk` independently lists these device ROMs.

| Chip | Bytes | Source CRC32 | Source status |
| --- | ---: | --- | --- |
| Standard BIOS `700a01.22g` | 524288 | `11812ef8` | Good |
| H8 response `h8a01.bin` | 64 | `131e0359` | Good; separate H8 firmware remains NO_DUMP |
| ZS01 `gka25kaa.u1` | 140 | `2b0efc90` | Good |
| Cassette serial `gka25kaa.u6` | 8 | `ce84419e` | BAD_DUMP |
| Digital I/O `digital-id.bin` | 8 | `2b977f4d` | Good |
| Multi-session `flash.20t` | 524288 | `b70c65b0` | Good |
| Multi-session `m48t58y.6t` | 8192 | `609ef020` | Good |

The auditor also checks source SHA1 for every chip. The historical parent
archive stores the cassette serial as `gea25aaa.u6`; it is accepted only
because its eight bytes match the current KAA serial's size, CRC and SHA1
exactly. Both the declared and actual member names remain in the audit.
The old parent ZIP's 2 MiB flash dumps are not part of the pinned KAA set
and are never used to supply an installed game.

Public split archives `k573_dio.zip` (158 bytes, archive SHA1
`9695831910742a7d6e3b17f5e3f811320e8b4125`) and `k573msu.zip` (136526 bytes,
archive SHA1 `f026619781ba414e581c2e3c152a4ef053dc57a5`) supply the three
device chips. Original archive URLs, hashes and audit/test output are recorded
under ignored `artifacts/boards-mac-20261015/system573/pcnfrk4mk`.

```sh
python3 scripts/audit_system573_digital_media.py --mame-source ../mamedev \
  --rompath roms --roms-only --output-dir roms/audited/pcnfrk4mk
python3 -m unittest discover -s tests -p test_system573_digital_media.py
```

This explicit ROM-only mode produces audited chip files and `audit.json`,
with `complete_media_verified=false`, `native_profile_available=false` and
`board_execution_verified=false`. It creates no native `board.ini`. Without
`--roms-only`, the audit requires the original KAA CD `a25kaa02.chd`, source
SHA1 `6c9084773c6964e794bd096822da4413aa7305ef`, matching CD metadata and a full
`chdman verify`. Missing CD, a CD from DDR, corrupt serial bytes and unreviewed
game/BIOS profiles are rejected in tests. The original KAA CD has not yet been
acquired: queried modern archive entries mark it private, while inspected
public older archives do not contain that image. No other-region disc or
extracted executable substitutes for it. BAD_DUMP annotations remain explicit.

The standalone [ZS01 component](konami-zs01.md) now passes encrypted pin
commands, CRC/retry erasure, data/configuration writes, key replacement,
persistence and serial reads with an independent host codec, both optimized
and under ASan/UBSan. The original audited KAA cassette/serial also pass reset,
all fourteen data pages, configuration and serial readback. Existing X76F041,
X76F100 and DS2401 tests still pass. This does not prove game acceptance of
the cassette. ZI security/control/IN1 wiring now passes the same complete
protocol and original-image readback through the board bus, including timed
DS2401, masked lanes and reset persistence, under optimized and ASan/UBSan
builds. Native digital loader and execution remain pending.

The pinned multi-session device declares its ROMs but has an empty
`device_start`, no configured processor and no runtime address map. Auditing
its original media does not establish emulation of a working external
multi-session box. The digital I/O FPGA/MAS3507D audio path, instrument inputs,
native cold installation and player validation still need integration and
actual game evidence. This row remains open in the overall Mac matrix.

The standalone [MAS3507D decoder/I2C port](mas3507d-port.md) now retains the
actual pinned MAME methods and identical minimp3 decoder. Explicit synthetic
44.1 kHz stereo and 22.05 kHz mono MPEG streams decode with verified frequencies,
frame-sync/counter agreement, deterministic restart, mute and independent
channel gain. Optimized/ASan/UBSan checks pass with identical baseline PCM
hashes. Empty/invalid input produces no successful frame sync; callbacks cannot
reenter mutation. This is not yet connected to the FPGA's encrypted RAM/timers
or native speaker output, and does not provide original game music evidence.

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
* Raw SPU WAV SHA256: `ec0f32a92fd44384d92949c315b664a10c15349bd2351a87bdf8cad30389073f`.
* SPU: 5,528,516 stereo frames at 44.1 kHz, peak 10,606.

A separate pre-clock-fix 5000-frame interactive GL run consumes the 1102 events scheduled
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

This base flash representative cannot replace analog/digital CD transport,
security or sound validation. The analog/CD representative is validated below;
the digital configuration remains open. Physical Vita is also outside the present Mac evidence.
Authentic DRC, full-tournament progression and operator checks remain unverified.


## DDR 2nd Mix analog/CD integration

The reviewed original `ddr2m` JAA profile uses the same standard BIOS and H8
response as the base machine, a 548-byte X76F041 cassette X (`gn895jaa.u1`,
CRC `5d906be5`, SHA1 `4ea9c5506aaaf1726f2a39d0a37a8df35a6aad47`) and read-only
CD `895jaa02` (source SHA1 `cfe3a6f3ed62ba388b07045e29e22472d17dcfe4`).
Both cassette and CD retain the source BAD_DUMP status. H8 firmware remains
NO_DUMP; the actual separate response dump is used at the source response pins.
The preparer verifies all three original chip size/CRC/SHA1 values and the full
CHD data/metadata hashes before publication. No flash or timekeeper dump is
declared for this set. Its source defaults are eight erased FF Fujitsu flashes
and FF M48T58 SRAM with the actual host RTC counters. No pre-extracted executable
or installed flash image substitutes for native cold installation.

`--system573 roms/prepared/ddr2m` accepts the exact separate CD profile; a build
without CHD support rejects it while retaining Hyperbbc support. Cassette X uses
the original 548-byte image and source X76F041 pins, without cassette Y lamp
callbacks or its unrelated factory-default exception. The analog output aperture
at `1f640000` retains the source bit permutation, output latches and two-player
GN845 stage shift/mask handshake. Directions drive the stage port; the source
panel buttons and Start use IN2/IN3. `boot_cd` in an ordered input CSV controls
the source boot-device DIP (pressed=CD, released=Flash), independently of Test.
Normal cold board defaults select Flash as in source; an empty installation
still reaches the CD initialization prompt.

The source CR-589 engine maps CS0 at `1f480000`, CS1 at `1f4c0000`, reset at
`1f560000` and IRQ10. DMA5 follows the source's board-write callback dispatch,
which is RAM-bound despite its name, with two actual DATA-register reads per
32-bit word. DMA channel completion is immediate; payload delivery belongs to
the board timer. The original 30,000 MAME CPU cycles per sector use the PSX
input/4 domain, equivalent to 60,000 cycles in this core's input/2 domain. The
source's remaining-DRQ retransfer is preserved. Host bounds reject RAM overflow.
Instruction fetches retain actual flash bytes; dirty spans are collected over
all byte writes in a CPU word, and only those spans need copying/invalidation.
Bank control changes update the whole fetch view only when the bank changes.

Independent MAME cold initialization progresses through erase/programming and
finishes at FLASH-ROM INITIALIZE COMPLETE. A separate Flash-DIP warm boot reaches
HOW TO PLAY / INSERT COIN(S). Native GL and NEON each run 18,000 frames from
source erased/default state, pulse Test at frames 1800–1860 and finish at the
same initialization-complete screen with zero faults. Each reads 8,719 original
sectors, transfers 4,464,128 DMA words and generates identical final RAM. All
eight programmed flash chips match each other and the independent MAME
installation byte for byte. MAME NVRAM is never imported to create native
installation evidence. A further current-code GL cold installation also completes and matches all
eight MAME chips after the timer/dirty-span bookkeeping refinements. These
installation results remain separate from player-gameplay evidence.

DDR snapshots use a separate `S573NV2\0ddr2m\0\0\0` 16-byte identity within the
32-byte length/CRC header. Its 16,851,524-byte payload layout includes the eight
2 MiB flashes, 8 KiB timekeeper, 548-byte cassette and original 64 KiB mutable
CR-589 buffer. Hyperbbc's NV1 shape remains unchanged. Original prepared media
remain immutable. Original-media loader tests exercise both players' stage and
panel controls, DIP changes, complete NVRAM persistence and rejection of
corrupted chips, malformed headers/payloads and mixed/unreviewed profiles.
Hyperbbc guest regressions pass on GL/NEON and with CHD support disabled.

Warm native Flash boot reaches flash verification (31% at 1800 frames). A
separate replay then inserts coins and reaches SELECT PLAY MODE at frame 7800.
The 9600-frame GL/NEON replay enters actual 1st Stage, exercises all four P1
stage directions and displays PERFECT!! with the dance arrows and score HUD.
Both consume 92/92 events with zero faults, 70 original sector transfers and
1948 stage clock edges. Their raw SPU WAVs are byte-identical: SHA256
`516192c876d06aec8b8498c5047b32ac3505ca63b1a3cb1a47e70544ef843484`,
7,073,760 stereo frames at 44.1 kHz, peak 26,546. Final 4 MiB RAM differs in
one byte at `92854` (BCD `25` versus `26`), consistent with source host-clock
startup one second apart. The mismatch is preserved in the evidence rather
than discarded to claim an identical RAM hash.

The initial guest integration decoded 2,322,708 stereo CDDA frames, 2,268,288
nonzero frames and peak 32,639 in both runs while its speaker output still
contained SPU only. That intermediate result is superseded by the complete
source-clock speaker and live output checks below. The separate original-disc
component is recorded in `atapi-cr589-port.md`. A longer provisional 11,000-frame replay returns to
the warning/title flow and schedules its final release outside the bound
(279/280 events); it is not substituted for the fully consumed 9600-frame player
run. Full song completion, P2/operator checks and physical Vita remain open.

Evidence is under `artifacts/boards-mac-20261014/system573/ddr2m`.

```sh
python3 scripts/prepare_system573_cd_board.py --mame-source ../mamedev \
  --rompath roms --output-dir roms/prepared/ddr2m
python3 tests/test_system573_cd_profiles.py
python3 tests/check_system573_cd_harness.py --harness build/mac-harness/pcsx-harness \
  --media roms/prepared/ddr2m --installed-nvram /path/to/native-ddr2m.nv
```


## DDR source-clock speaker output

The bounded analog/CD representative now also passes actual speaker output.
GL/NEON retain the 9600-frame 1st Stage/PERFECT replay, consume 92/92 inputs and
have zero device/sample-clock faults. Independent 44.1 kHz CDDA and SPU samples
are summed at the source gain-1 stereo routes without resampling or swapping
channels. The continuous CD DAC emits silence when inactive, paused or finished;
failed sector reads remain failures. PCM blocks use their actual emulated sample
indexes. If SPU's even-pair rounding produces a future sample, it remains queued
until the CD clock arrives instead of receiving a fabricated sample.

This exposed an actual large-SPU-DMA bug: the console renderer advances by a
size-based speculative delay inside SPUwriteDMAMem. System573 already delivers
that payload at its source DMA timer edge. A 512 KiB transfer could reset the
SPU clock into the future and lose 2740 audio frames. Its native System573 path
now renders at the actual payload cycle, as MAME spu_device::start_dma copies
at that edge. The console/other-board branch is retained. A generated large-DMA
clock check passes with the fix and fails with an independently linked old
speculative path. The speaker mixer also rejects missing/duplicated clocks and
overwritten required history; optimized and ASan/UBSan tests pass.

Both headless renderers produce exactly 7,076,500 stereo frames, matching the
final cycle clock, with no pending samples. They have 751,541 nonzero SPU frames,
2,268,292 nonzero CDDA frames and 270 saturated output samples at the source
unity sum. Saturation is normal signed-PCM16 output, not a device fault.

* Mixed speaker WAV SHA256: `cce2f79b5838ea2820903e69923bc11375adcf7b5e3bcc1a91ae2f86a5d888aa`.
* Raw SPU WAV SHA256: `029104707ba062f881645f7d7710fa6281e06bab7606c333f64f0d13d968b84b`.

The separate live SDL run reaches the same player capture, consumes every input,
queues all 7,076,500 speaker frames at user volume 100%, and reports zero audio
errors. It finishes in 160.495 wall seconds. Its raw and mixed WAVs differ from
headless in the first coin cue between 120.368889–120.700442 seconds: that SPU cue
is shifted two stereo samples (about 45 microseconds). Every mixed-wave
difference equals the corresponding raw-SPU difference; CDDA data and all clock
counters match. This is consistent with the SPU's even-pair sampling and frontend
scheduling granularity, but is preserved as a real difference, not normalized
away to claim a bit-identical live result.

`--audio` still captures raw SPU for diagnosis. `--mixed-audio` captures actual
source speaker output, now available for a bounded noninteractive System573 CD
run as well as the live GL frontend. Hyperbbc retains its existing SPU-only
output and NV1 format. Its generated guest regressions pass with and without CHD;
shared G-Net/GV/GQ/Judge/NBA/Metro input/audio tests also pass. This establishes a
bounded Mac analog/CD representative, not complete songs, every operator/P2
scenario, physical Vita, or the separate digital board.

Evidence is in `artifacts/boards-mac-20261014/system573/ddr2m/audio`.

```sh
build/mac-harness-vitagl-like/pcsx-harness --system573 roms/prepared/ddr2m \
  --frames 9600 --inputs /path/to/ddr-player.csv --nvram /path/to/native-ddr2m.nv \
  --audio /path/to/raw-spu.wav --mixed-audio /path/to/speaker.wav \
  --interactive --capture-gl --output /path/to/player.ppm
```


After the source-DMA clock correction, Hyperbbc's original GL/NEON 7500-frame
P1 regression still shows PINKROCKER versus two COMPUTER players, consumes
1902/1902 inputs and has zero faults. CPU cycles/blocks/PC and final RAM are
unchanged; RAM SHA256 remains `792bd6a1677573696cb4d2c5567e15d282b7a7e203d60267e050a2c9a5ffd29c`.
The current raw SPU captures are identical in both renderers, now with 5,528,516
stereo frames (the 2740 previously skipped boot frames recovered), peak 10,606
and SHA256 `ec0f32a92fd44384d92949c315b664a10c15349bd2351a87bdf8cad30389073f`.
The earlier live Hyperbbc captures remain explicitly pre-clock-fix evidence.
