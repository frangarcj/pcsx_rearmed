# System 573 representatives on Mac

Hyper Bishi Bashi Champ EAA (`hyperbbc`) passes a bounded native Mac interpreter
P1 run on GL and NEON with original source-verified media. Both show PINKROCKER
versus two COMPUTER players in a ship alignment minigame, consume 1902/1902
input rows and produce identical RAM/SPU WAV with zero device faults. This
covers the base flash-only board. DDR 2nd Mix now also passes a bounded analog/CD
P1 run and source-gain SPU/CDDA speaker mix. The digital configuration now has
original-media cold installation and bounded scored P1 gameplay with live SDL
playback. Rhythm judgement alignment against MAME remains open. Independent
MAME 0.287 captures are separate reference evidence.

Raw audio/RAM/VRAM/frame intermediates were pruned during the October 15 disk
cleanup. Run results below are historical; retained observations can be checked
with `check_system573_mpeg_observation.py` without recreating those captures.

The full family validation still requires three distinct configurations:

| Representative | Coverage | Current evidence |
| --- | --- | --- |
| Hyper Bishi Bashi Champ EAA (`hyperbbc`) | Base board, eight onboard flashes, timekeeper, cassette Y lamp outputs and three-player buttons | Native interpreter GL/NEON P1 minigame, live source-gain SPU/SDL audio and complete flash/NVRAM persistence verified; full P2/P3 gameplay/operator checks pending |
| Dance Dance Revolution 2nd Mix (`ddr2m`) | Analog daughterboard, CD-ROM, cassette X, dance input multiplexing | Original cassette/CD verified; source CR-589 transport and DDR analog multiplexor integrated. Native GL/NEON cold flash initialization completes and all eight programmed chips match MAME byte for byte. Native warm GL/NEON 1st Stage/PERFECT, 92/92 inputs and original nonzero SPU/CDDA decoding pass. Source gain-1 SPU/CDDA mix passes headless GL/NEON and live SDL. Full song/P2/operator coverage remains separate |
| Percussion Freaks 4th Mix KAA (`pcnfrk4mk`) | Digital daughterboard, CD-ROM, cassette ZI/serial ID, instrument inputs and multi-session unit | Seven original game/device chips audited, retaining source BAD_DUMP serial/CD status. ZS01/ZI encrypted protocol, original serial readback and FPGA-to-MAS components pass. Native loader/MMIO/drum controls/NV3 pass generated CPU tests on GL/NEON with strict original chip/CD rejection and cold/warm persistence. CPU-clocked digital audio and gain-1 SPU/CDDA/digital mixing now pass synthetic MPEG tests, including identical GL/NEON/live SDL WAVs. Original KAA CD now passes full audit, and native GL/NEON cold installation matches all eight MAME flash chips, mutable ZS01 and the CR-589 update buffer. Original GL/NEON and live SDL six-pad replays now reach 1st Stage with 1,150 points; full-song results and judgement alignment against MAME remain open |

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
game/BIOS profiles are rejected in tests. The earlier archive search found only
private entries or older public archives without this disc. A subsequent public
MDK download supplies the exact KAA image; full data verification and the
original cold-install comparison are recorded below. No other-region disc or
extracted executable supplies this result. BAD_DUMP annotations remain explicit.

The standalone [ZS01 component](konami-zs01.md) now passes encrypted pin
commands, CRC/retry erasure, data/configuration writes, key replacement,
persistence and serial reads with an independent host codec, both optimized
and under ASan/UBSan. The original audited KAA cassette/serial also pass reset,
all fourteen data pages, configuration and serial readback. Existing X76F041,
X76F100 and DS2401 tests still pass. This does not prove game acceptance of
the cassette. ZI security/control/IN1 wiring now passes the same complete
protocol and original-image readback through the board bus, including timed
DS2401, masked lanes and reset persistence, under optimized and ASan/UBSan
builds. Original boot/installation now has separate evidence below; these
component checks alone do not establish game acceptance.

The native CPU/MMIO/input/NVRAM profile is now present, with original seven-chip
checks and a separate `pcsx-aow-system573-digital-v1` configuration. The normal
preparer requires the full original CD audit before publishing any native
directory; the ROM-only auditor still emits no runnable profile. Its
`native_profile_available=false` describes that audit-only artifact. The subsequent
complete-media package and original boot below use the actual KAA CD.

```sh
python3 scripts/prepare_system573_digital_board.py --mame-source ../mamedev \
  --rompath roms --output-dir roms/prepared/pcnfrk4mk
```

The normal loader verifies all seven source CRC/size pairs and the original
KAA CD SHA. Tests retain a deliberately CD-free temporary ROM directory for
missing-CD rejection, verify full preparation with the now-available original
CD, reject a one-bit change in each individual chip, and reject a real DDR
CD supplied under the KAA filename. This is loader evidence, not execution of
the original game. Source defaults remain eight erased FF Fujitsu flashes and
FF main-board timekeeper SRAM with live RTC counters; old parent flash dumps
and extracted game executables are not used.

Digital I/O maps at `1f640000–1f6400ff`. CPU reads/writes split into the source
16-bit handlers, including high-byte accesses, independent auto-incrementing
RAM pointers and source-zero unmapped neighbours. ZI SDA retains its separate
active-low control pin. Six drum buttons map to source IN2 masks
`0100/0200/0400/0800/1000/4000`; left/right select use `0001/0002`, and Start
uses `8000`. Coin 2 and P2 controls are rejected as unused by this source
configuration. Source drum/spot/neon/start/select outputs are retained, with
their actual inversion and duplicate offset cases. The Mac frontend reports
one gamepad for this profile.

`tests/check_system573_digital_harness.py` executes generated MIPS BIOS code
through the real Mac CPU, in both GL and NEON. It verifies 8/16/32-bit register
access, a high-byte RAM-address write followed by `5566` readback, unassigned
lanes returning zero, ZS01 reset word `5a530001`, drum/select/start/DIP input
and source output lamps. Both runs consume 20/20 input events with zero faults.
The fixture uses an actual source CR589 model with an explicitly empty media
backend: no CD sectors/tracks/decoded audio are supplied. Its guest delay lets
the original ATA power-on diagnostic timer finish before checking signature
`14/eb`; early reads correctly return BSY rather than an invented ready state.
Every log labels `SYNTHETIC_FIXTURE` and `EMPTY_FIXTURE_DRIVE`. The ordinary
loader rejects its generated BIOS, and the base fixture selector rejects the
digital configuration. A no-CHD build rejects this CD profile explicitly.

Digital NVRAM uses its own 16-byte identity `S573NV3\0PF4KAA\0\0` and a
16,851,116-byte checksummed snapshot: 32-byte header, eight flashes, 8 KiB
timekeeper, complete mutable 140-byte ZS01 image and 64 KiB source CR589 buffer.
The immutable cassette/digital IDs stay in their verified source media. The
32 MiB digital RAM, device protocols and timers are transient, not NVRAM.
Native cold/warm tests preserve separately mutated flash, EEPROM and drive
buffer bytes, while invalid size, identity, reserved header bytes, CRC and
unreviewed/mixed profiles fail before importing state. Hyperbbc NV1 and DDR
NV2 layouts and loader/persistence regressions continue passing on GL/NEON.

```sh
make -f Makefile.mac-harness GPU=neon system573-test
make -f Makefile.mac-harness GPU=vitagl-like system573-test
python3 tests/check_system573_digital_harness.py \
  --harness build/mac-harness/pcsx-harness \
  --original-media roms/audited/pcnfrk4mk --wrong-cd roms/ddr2m/895jaa02.chd
```

The [native digital audio clock and speaker bridge](system573-digital-audio-clock.md)
now bind FPGA/MAS rendering to actual PSX cycles, resample the decoder's rate
to 44.1 kHz and sum SPU/CDDA/digital at source gain 1 before final saturation.
`--mixed-audio` and live SDL use the complete speaker route; raw `--audio`
remains SPU-only. Generated MIPS code loads encrypted synthetic MPEG through
the real MMIO bus. Its 120-frame cold/warm runs pass at 22.05, 44.1 and 48 kHz;
44.1 kHz output is byte-identical between GL, NEON and live SDL. Component tests
cover all nine MPEG rates, a midstream rate/channel change, continuous reset
epochs, callback rejection and coarse/fine CPU advance equivalence. These
checks establish synthetic CPU/audio integration, not original game music.

The pinned multi-session device declares its ROMs but has an empty
`device_start`, no configured processor and no runtime address map. Auditing
its original media does not establish emulation of a working external
multi-session box. The following original runs establish native cold
installation and bounded scored drum gameplay separately. Full-song judgement
alignment and MSU operation remain open in the overall Mac matrix.

The standalone [MAS3507D decoder/I2C port](mas3507d-port.md) now retains the
actual pinned MAME methods and identical minimp3 decoder. Explicit synthetic
44.1 kHz stereo and 22.05 kHz mono MPEG streams decode with verified frequencies,
frame-sync/counter agreement, deterministic restart, mute and independent
channel gain. Optimized/ASan/UBSan checks pass with identical baseline PCM
hashes. Empty/invalid input produces no successful frame sync; callbacks cannot
reenter mutation. The [digital I/O/FPGA component](system573-digital-io.md) now connects encrypted
32 MiB RAM, source cipher/timer/register methods and this decoder. Both FPGA
variants pass delivered-byte/PCM oracle checks, enable/counter/reset/mute tests
and wrong-key silence; original digital-board serial readback passes too. The
source last-pending-byte/guard-padding behaviour and its final-frame effect are
recorded separately. CPU/sample-clock binding and speaker output now have the
synthetic evidence above; original game music remains pending.

## Original KAA CD and cold installation

The exact original KAA disc is now available from
[MDK](https://mdk.cab/game/pcnfrk4mk), downloaded from its
`/download/chd/pcnfrk4m/a25kaa02.chd` endpoint. Its 287,173,674 compressed bytes
have file SHA1 `2c7d85598097d058989618f7e591768609f4ab96`; the independent CHD
header/data+metadata SHA1 is the source-pinned
`6c9084773c6964e794bd096822da4413aa7305ef`. Full `chdman verify`, all seven
original chip audits and normal native preparation pass. MAME 0.287 reports
the complete set as best available, retaining the source cassette-serial/CD
BAD_DUMP annotations and undumped H8. No missing firmware is substituted.

Cold native startup selects the CD DIP at frame zero, with all eight flashes
and board timekeeper at their actual source erased defaults. The original
CDUPDATE program displays NOTE 604 and requests Test. A real Test pulse at
frames 2,400–2,460 starts its update; the CR-589's source buffer changes from
its `GS0N` seed to the original update's `gscb` inquiry identity. The game then
automatically starts `INSTALL G*A25KAA`, programs/verifies the flash chips and
reaches INFO 508 / Installation completed by frame 14,400. Both GL and NEON
show machine number `1891-5346` and the original request to power-cycle.

Independent MAME starts with its own erased/source device defaults and uses
a corresponding CD DIP/Test sequence. Its own installation also reaches INFO
508. Each of the **eight 2 MiB native flash images matches MAME byte for byte**;
the J pair (`31j`/`27j`) correctly remains erased in both implementations. The
complete mutable 140-byte ZS01 image and updated 65,536-byte CR-589 buffer also
match the independent reference exactly. The drive buffer SHA256 is
`319602942bfce7b39fe2e806ff4e772dbf8c49d033c97db9f18b7d922a63b92f`.
MAME runtime NVRAM is never imported to create either native installation.

Both native runs consume 3/3 input events, read 8,426 original CD sectors,
execute 547 packets / 8,426 DMA transfers / 4,314,112 DMA words, cross the
32-bit CPU-cycle wrap and report zero board/CD/digital/clock faults. The
captured 4 MiB RAM and whole 16,851,116-byte NV3 images also match each other:

| Native GL/NEON cold artifact | SHA256 |
| --- | --- |
| Main RAM | `85e558b64f7a4ddfad540593cea5d7f7684f5dbfc1bc57149d38a20cce432012` |
| Complete checksummed NV3 | `46f03fd07e3f8ce694c3f4a1d59b1ad1d38f97f2e291d51f51781d8cd3f6ee90` |

The source chip/cassette/drive comparisons are required by
`tests/check_system573_digital_original_harness.py`. Whole RAM/NV3 equality is
reported separately because independent future cold starts may receive
different live host calendar fields; bytes are never masked to manufacture
equality. The static completion scene uses the existing GL front-buffer
capture when it has not presented recently. Both native captures and the
reference completion capture have been visually inspected.

```sh
python3 -m unittest discover -s tests -p test_system573_digital_media.py
python3 scripts/prepare_system573_digital_board.py --mame-source ../mamedev \
  --rompath roms --output-dir roms/prepared/pcnfrk4mk
python3 tests/check_system573_digital_original_harness.py \
  --evidence-dir artifacts/boards-mac-20261015/system573/pcnfrk4mk
```

The existing ROM-only audit artifact remains explicitly incomplete; preparing
and executing the new full profile does not rewrite the older audit-only
manifest into a gameplay claim. Normal preparation continues to set
`board_execution_verified=false`.

Warm Flash boot from copies of native-installed NV3 reaches the original
NOTE 605 / Cleared RTC-RAM screen, matching an independent warm MAME process.
A Test pulse at frames 1,800–1,860 acknowledges it and launches the actual
`GCA25KAA Percussion Freaks 4th Mix` program. By frame 7,200 it is loading its
original device/data files into digital sound RAM, with zero faults. A longer
original boot then synchronizes MPEG, produces nonzero original digital PCM
and reaches the animated attract flow. That first long probe stops at the
harness's default one-billion-instruction-block bound at frame 9,867, so it
is retained as intermediate evidence, not a completed 18,000-frame run.
Player and complete sound replays use an explicit larger harness bound.

Cold/warm/reference evidence, exact chip comparisons, CD origin and full-media
test output are under ignored
`artifacts/boards-mac-20261015/system573/pcnfrk4mk`.

## Original KAA scored drums and live playback

Separate GL and NEON interpreter runs restore copies of the native installation,
select Flash boot, acknowledge NOTE 605, insert two coins and enter STANDARD.
The exact 1,495-event replay pulses all six drum inputs for four frames every
eight frames from 12,800 through 13,776. Both complete 13,800 video frames with
`--max-blocks 3000000000` and show **1ST STAGE / 1,150 points**, falling notes
and an active gauge. A matched NEON control retains all 19 boot/coin/start
events and removes only the pad pulses; all four original score fields remain
zero at the same 13,800-frame endpoint. This is a bounded input/score check,
not a full-song clear or rhythm-accuracy benchmark.

The unchanged original score getter at `8008a04c` loads `80349538`; that field
and `803493fc`, `80349400`, `80349410` all contain 1,150 in each positive run.
No executable, chart, score or installed game data is patched. Inputs retain
the pinned source's six active-low masks: `0100`, `0200`, `0400`, `0800`,
`1000`, `4000` (High Tom, Low Tom, Hi-Hat, Snare, Cymbal, Bass Drum).

Both runs consume 1,495/1,495 events and read 13,260 CD sectors through the real
CR-589/DMA path. The original encrypted RAM-to-MPEG path reports FPGA `e000`,
MPEG `4000`, 599 decoded frames, 2,628,289 nonzero digital frames and no device,
clock or mixer faults. Their 10,158,686-frame speaker captures mix SPU, CDDA
and digital output at source gain 1. This data disc emits no decoded CDDA;
the SPU and MPEG sources are both nonzero. One channel sample saturates at
full scale in the source-gain sum.

Headless GL/NEON WAV files match byte for byte:

| Original replay artifact | SHA256 |
| --- | --- |
| Raw SPU WAV | `74fe81f4b975c475be994355c89491cbd4b4b6b328f8a9693f3e9aed7988ba27` |
| Complete speaker WAV | `e62d9a2427f9c860cdeabc7d9100d8ce7203b593d0b230ec821a8164be252782` |

Whole RAM does **not** match: the retained dumps differ at 92 bytes, including
calendar/debug copies and challenge-like fields whose provenance is not fully
traced. The two full NV3 files differ only at three live RTC clock bytes and
the resulting CRC. Flash, mutable cassette and drive buffer remain identical.
These differences are reported; no bytes are removed to claim equality.

A separate original live GL/SDL replay consumes the same 1,495 events, shows
1,150 points in all four score fields, plays the complete source speaker mix
and reports zero SDL/device errors. Source digital/CD counters match the
headless runs. Live WAVs first differ at 215.129660 seconds, during drum SPU
effects: 96,038 raw SPU and 96,037 mixed channel samples differ. Every mixed
change occurs where raw SPU also changes; subtracting the raw-SPU delta from
the mixed delta leaves at most one signed PCM LSB. This measured relationship
does not establish bit-identical live digital-only output. Live raw SPU SHA256
is `1d576731020891a22589ef67f72138d1cae69e91409155ddbf259870904cac05`;
live speaker SHA256 is
`551c0630d5a7be20effbb614d22b221abb2c86019b80bab912eed142d2c9f0f3`.
A separate manual keyboard run also enters STANDARD with coin/start and plays
nonzero original audio; manual operation of all six pads is not yet recorded.

Independent MAME enters the same original 1st Stage and shows **120,150**
points under an approximate time-converted replay. That Lua replay converts
native frame numbers using a single cold-boot average, so its input events
are not aligned to native VBlank transitions or the same song phase. The score
difference initially remained an open comparison. The song-clock follow-up
below confirms a remaining mismatch after aligning the song phase; neither
comparison is a passing judgement oracle or a proven oscillator defect. The pinned `konami573`/`drmn4m` source and MAME
0.287 `-listxml pcnfrk4mk` both declare CPU 67,737,600 Hz, SPU 33,868,800 Hz
and CDDA 44,100 Hz, matching the native profile/core clock. No clock is changed
to improve a score.

The extended evidence verifier requires the matched zero-score control,
original getter/score fields, full event consumption, device diagnostics,
headless WAV equality and live audio-delta relationship:

```sh
python3 tests/check_system573_digital_original_harness.py \
  --evidence-dir artifacts/boards-mac-20261015/system573/pcnfrk4mk --live
```

This verifies collected original-media artifacts; it does not launch MAME or
substitute its installed NVRAM. For another native run, start from a **copy**
of `install-neon.nv` and the retained `player-pads.csv`, then use either native
harness with `--system573 roms/prepared/pcnfrk4mk --frames 13800
--max-blocks 3000000000 --inputs INPUT.csv --nvram COPY.nv --audio SPU.wav
--mixed-audio SPEAKER.wav --dump-ram RAM.bin --output FRAME.ppm`; the GL run
also uses `--capture-gl`, and the SDL replay adds `--interactive`.

## Original KAA song-clock comparison

The previous comparison now has a stronger follow-up. Optional
`--snapshot-ram ADDRESS` arguments append up to 16 selected little-endian RAM
words to the native cycle-snapshot CSV. Reads use the backing main RAM directly,
reject MMIO, unaligned/invalid segment addresses and offsets beyond the selected
board's actual RAM size. They neither call mapped devices nor write guest state.
The original 4 MiB profile passes alias, boundary, dump-agreement and invalid
address checks on both Mac renderers:

```sh
python3 tests/check_mac_snapshot_ram.py --harness build/mac-harness/pcsx-harness \
  --system573 roms/prepared/pcnfrk4mk
python3 tests/check_mac_snapshot_ram.py \
  --harness build/mac-harness-vitagl-like/pcsx-harness \
  --system573 roms/prepared/pcnfrk4mk
```

A fresh complete 13,800-frame native replay records the original song counter
at `803493f8`, score fields and chart parameters from core cycle 6,773,760,000,
every 564,480 cycles. It reports zero device/audio errors, scores 1,150 and
retains both previous headless WAVs **byte for byte**. The first nonzero song
tick occurs at native frame 12,758 / about 212.950 seconds. Independent MAME
reaches that tick at 207.201084 seconds: approximately 5.749 seconds earlier.
This proves that total-boot-time conversion did not align song input phase.

The committed `tests/mame_system573_song_oracle.lua` keeps the recorded 19
boot/menu events, then schedules the 1,476 pad transitions against this original
song tick. Native frame 12,800 precedes game tick 43's update; the corresponding
MAME frame-done callback sets inputs at tick 42. Both traces finish at song tick
1,042 and retain the same four original chart parameters. The oracle consumes
all events and exits cleanly using its own independently installed MAME state.
**It scores 25,950, versus native 1,150.** A separate tick-43 boundary probe
scored 24,300, so shifting the first pulse by one tick also does not establish
judgement equality. These are retained differences, not a compatibility pass.
Further input-sampling and MPEG playback-clock alignment is still required.

For an independent reference run, copy `mame-install/nvram/pcnfrk4mk` into a
fresh MAME NVRAM directory; never substitute that data into a native run. Set
`PCSX_PF4_INPUTS` to the absolute path of `player-pads.csv` and
`PCSX_PF4_TRACE` to a fresh output CSV, then launch MAME `pcnfrk4mk` with
`-noreadconfig -autoboot_delay 0 -autoboot_script
/absolute/path/to/tests/mame_system573_song_oracle.lua`, that separate
`-nvram_directory`, `-video none -sound none -nothrottle -seconds_to_run 226`
and the audited original `-rompath`. The script reads memory and delivers real
cabinet inputs; it does not modify original program or chart data.

```sh
python3 tests/check_system573_digital_original_harness.py \
  --evidence-dir artifacts/boards-mac-20261015/system573/pcnfrk4mk --timing
```

This verifier checks collected observations and reports the score comparison
as open. The authoritative new traces are `timing-neon-ok.*` and
`mame-song-oracle/*`. The earlier `timing-neon.*` attempt completed its CPU
bound but failed WAV writes when the volume filled; those truncated WAVs are
not successful audio evidence. Two temporary CHD copies were removed only
after full SHA256 equality with the preserved prepared originals was confirmed;
`verified-temporary-cleanup.json` records the paths, sizes and hashes. Neither
original media nor prior unique validation evidence was discarded.

## Retained original MPEG observations

The original GL/NEON replays recorded before the disk cleanup now have a pure
status observer. `system573_get_digital_status` copies the existing FPGA/MAS
status and current input latch without advancing time, rendering audio or
calling mapped-register reads. System 573 cycle-snapshot CSVs append eight
fields: decoded MPEG frame counter, guest-latched sample timer, stream
start/end/current addresses, raw IN2 bits and FPGA/MPEG control states. Analog
and base profiles leave those cells empty.

The MAME oracle observes the same saved device fields rather than reading
clock registers. In MAME 0.287, the human device names contain `I/O`, while
Lua's `device.items` code splits save names at their first slash. Those two
devices therefore lack named Lua entries. The oracle finds their unique
source-sorted size/count layouts, validates the digital oscillator and reads
those items; it neither assumes fixed global indices nor writes save items.

The retained `mpeg-neon.csv` and `mpeg-gl.csv` contain 1,822 rows apiece and
are byte-identical, SHA256
`7cf3a31d94828db604da0aeb29dfdb91afe6caa8a21532072b06b5f1caaf8da0`.
Their original logs complete 13,800 frames / 1,495 events with zero faults and
1,150 points. Across the **599 common decoded MPEG frame numbers**, native and
MAME have exactly the same encrypted-source byte cursor for every frame.
This checks stream consumption boundaries, not original-game PCM equality
against MAME or complete rhythm-judgement accuracy.

For the selected song stream (`00000000..00187014`), the first sampled MPEG
frame 1 occurs at native song tick 108 and MAME tick 104. Among 745 later
same-song-tick observations, the MAME-minus-native guest-latched timer has two
ranges: 2,424–2,426 samples (about 55 ms) and 1,687–1,688 samples (about 38 ms).
The samples come from different CPU/video callback phases; grouping by the
native frame/song-tick offset alone does not explain every timer observation.
No samples are adjusted or discarded to manufacture clock equality, and these
records do not prove a running-clock drift or its absence.

Two measured-offset reference probes retain the original boot/menu sequence
and all 1,476 pad transitions. Advancing pads four song ticks (the sampled
MPEG onset difference) gives MAME 6,900 points. Advancing them three ticks
(the nearest whole tick to the dominant 55 ms timer difference) gives 350.
Both end at tick 1,042; native remains 1,150. Neither probe is selected as a
passing judgement match. Exact input-poll/audio-phase comparison remains open;
no emulated clock, chart or program timing is changed for these probes.

The original status-observed GL/NEON WAVs matched the earlier headless captures
before removal. Their SHA256 values remain in the cleanup manifest; their raw
files were deliberately discarded under [the storage policy](build-storage.md).
The new verifier reads only retained logs/CSVs and those historical hashes:

```sh
python3 scripts/check_validation_storage.py --reserve-mib 1
python3 tests/check_system573_mpeg_observation.py \
  --evidence-dir artifacts/boards-mac-20261015/system573/pcnfrk4mk
```

It checks all 599 cursor pairs, equal renderer status rows, event consumption,
chart identity, score results and unmodified timer distributions. Output
explicitly says `fresh_audio_verification=false` and
`judgement_equivalence=OPEN`. The earlier raw-artifact verifiers require their
removed captures and must not be rerun merely to recreate those files.

## Isolated navigation and original-game pad reads

The retained song-tick reference has an extra navigation confound: four late
Start edges intended as native menu retries arrive after MAME has entered its
song. Forty reference snapshot rows therefore hold Start during gameplay;
the native snapshots have it released. This must be isolated before using
the reference as an input-poll oracle.

`PCSX_PF4_STOP_BOOT_AT_SONG=1` adds a control to
`tests/mame_system573_song_oracle.lua`: once the original song tick becomes
positive in the established gameplay observation window (after 180 seconds),
release Start and cancel only the remaining Start retries. Other navigation
events cannot be cancelled, and all 1,476 pad transitions remain unchanged.
The time gate prevents treating the boot RAM test's temporary contents as a
running song. Cancellations are logged separately from applied inputs.

The fresh independent MAME control applies 15 navigation rows and explicitly
cancels four. It still scores **25,950** at tick 1,042. All 2,672 callback
observations have the same times, score/chart RAM and MPEG state as the old
reference. Only the navigation counts and forty Start-bit observations change.
Thus the unwanted Start pulses do not explain native's 1,150-point result.

Optional `PCSX_PF4_READ_TRACE=/absolute/path/reads.csv` installs a pure MAME
read tap on `1f400008..1f40000b`. It returns no replacement value, reads no
additional device registers and observes only song ticks 1 through 1,042 in
the same gameplay window. It records the original returned data/mask, CPU PC,
song tick, score and already-latched MPEG status; it is bounded to 100,000
rows. The separate observation completes with **2,082 real reads**, exactly
two per recorded tick 1 through 1,041, all 16-bit reads (`0000ffff`) from
PC `8001876c`. Start is released in every read. Its complete callback CSV is
identical to the navigation control, and its score remains 25,950.

Evidence is under `mame-menu-control/` and `mame-pad-reads/` in the original
KAA evidence directory. Both runs use independently installed MAME state,
preserve the native install, and request no WAVs or memory dumps. The read
CSV is about 144 KiB. The retained checker verifies the isolation and passive
tap invariants:

```sh
python3 scripts/check_validation_storage.py --reserve-mib 1
python3 tests/check_system573_mpeg_observation.py \
  --evidence-dir artifacts/boards-mac-20261015/system573/pcnfrk4mk --pad-reads
```

The native follow-up below records the corresponding instruction. These
reference-only observations do not establish a native timing fix.

## Actual native pad polls and corrected reference boundary

`--system573-pad-reads FILE` now observes completed native IN2 bus reads
through a callback without replacing the returned data or advancing any extra
device clocks. It requires a System 573 interpreter replay, rejects output
path collisions and can append the same validated backing-RAM words used by
`--snapshot-ram`. `--trace-start CYCLES` limits its observation window. A
100,000-row bound reports failure if exceeded; the trace is detached before
closing and on board unload. The source compiles for NEON and the GL C objects;
the original-game runtime observation here is NEON.

The new original 13,800-frame replay starts tracing at cycle 6,773,760,000
and watches song tick `803493f8` and score `80349538`. It consumes 1,495/1,495
events with zero faults. Of 3,636 retained input reads, 2,082 occur during song
ticks 1 through 1,041: exactly two 16-bit reads per tick, separated by 162
native cycles. The reported native PC is `80018770`; the interpreter advances
PC by four before executing the load, corresponding to MAME's instruction
PC `8001876c`.

This reveals a callback-boundary error in the old reference: 492 actual pad
reads differ. `PCSX_PF4_PAD_TICK_OFFSET=1` corrects the reference by the measured
one-tick difference. With the menu control still enabled, all **2,082 actual
returned pad values now match native**. All 1,476 pad transitions are retained.
The corrected reference scores **24,300**, while native remains **1,150**;
matching actual input values does not establish matching rhythm judgement.

Among the 1,484 paired reads at ticks 300 through 1,041, the reference-minus-
native already-latched MPEG timer difference is exclusively 2,424 through
2,426 samples (54.966–55.011 ms). The earlier 38-ms band is absent from this
comparison at the actual read sites. This narrows the remaining comparison
to MPEG/game sampling phase; it does not by itself prove an oscillator defect
or complete-song clock equality. No guest clocks, chart data or scores change.

A fresh native run without the observer also scores 1,150 and has the identical
final PNG. Its full RAM hash differs from the observed run, so full-state
equivalence is **not established**. The persisted NV3 images differ only in
live RTC seconds/minutes and their CRC; the source loader initializes RTC from
host local time even when importing NVRAM. The runs therefore did not hold RTC
conditions equal. Neither a RAM match nor a proven cause of the RAM difference
is claimed. Both temporary 4 MiB RAM captures and raw frames were hashed and
removed immediately; no new WAV files were requested.

Native evidence is under `native-pad-reads/` and `native-pad-control/`; the
corrected source observation is `mame-poll-aligned/`. The retained checker
checks actual read pairs/PC semantics, all matching pad values, the timer
distribution, scores, PNGs and the limited NV differences:

```sh
python3 scripts/check_validation_storage.py --reserve-mib 1 && \
python3 tests/check_system573_mpeg_observation.py \
  --evidence-dir artifacts/boards-mac-20261015/system573/pcnfrk4mk --native-pad-reads
```

It explicitly reports `native_full_ram_equivalence_verified=false`,
`fresh_audio_verification=false` and `judgement_equivalence=OPEN`.

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
full digital judgement/song and MSU coverage remain open. Physical Vita is
also outside the present Mac evidence.
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
