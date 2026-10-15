# Twinkle / IIDX 2nd style Mac validation work

The representative is original beatmania IIDX 2nd style GC985 JAA (`bmiidx2`),
machine/input `twinklei`, parent BIOS `gq863`. Pinned MAME
`deb2f54fe7300e7a192690318beb860159bfd52a`, `konami/twinkle.cpp`, marks it
`MACHINE_IMPERFECT_SOUND`, without the first style's `MACHINE_NOT_WORKING`.
This row remains open: the native interpreter now initializes original backup
RAM, reaches the title and runs the tutorial/attract sequence on GL and NEON.
Timestamped speaker output and a successful timed P1 note are verified; live
controls, presentation and full-song validation remain pending. Original native
execution, component vectors and the independent MAME reference are tracked
separately.

## Original media, 2026-10-15

`scripts/audit_twinkle_media.py` now checks the source pin, unchanged driver,
exact reviewed registration, parent chain, three chip sizes/CRC/SHA1s and all
three original CHDs. Full verification requires source header SHA1s, CD track
metadata, consistent 512-byte HDD geometry and successful `chdman verify` for
every image. It retains source `MACHINE_IMPERFECT_SOUND`, records complete
media verification separately and emits no runnable `board.ini`.

| Original chip | Bytes | CRC32 | SHA1 |
| --- | ---: | --- | --- |
| PSX BIOS `863a03.7b` | 524288 | `81498f73` | `3599b40a5872eab3a00d345287635355fcb25a71` |
| Sound ROM `863a05.2x` | 524288 | `6f42a09e` | `cab5209f90f47b9ee6e721479913ad74e3ba84b1` |
| M24C02 `985a02` | 256 | `059c1f99` | `f094a12c9a56d351667746a765804c94cb3f96bb` |

Both CPU ROMs come from the public split `gq863.zip`, 74484 bytes / file SHA1
`f216c17e3191b1c8acb36568b0a2f983b5b7fea1`. Original security comes from
`bmiidx2.zip`, 147 bytes / file SHA1 `a00365be62ecac32351308207f1a962e84d24d46`.
The existing unrelated `twinkle.zip` does not contain these BIOS chips and is
not used. Audio ROM assembly retains source `ROM_LOAD16_WORD_SWAP`; the
MC68000 bus image has initial SP `00140000`, PC `00000086` and SHA1
`23e0017c501465f50c5ccc0a2ecce9c18f5f712e`. The raw chip and assembled image
remain distinct.

| Original medium | Source CHD SHA1 | Download bytes | File SHA1 |
| --- | --- | ---: | --- |
| Program CD `gc985a01.chd` | `0b783f11317f64552ebf3323459139529e7f315f` | 7036662 | `19951df448959aa49d7fbf365985edec0b9efa31` |
| Video CD `gc985a04.chd` | `4306417f61eb1ea92894d288cdb7c385eb4610f2` | 477649634 | `6923b0cd56287b4d7770b2013ddb386447c419ef` |
| Sample HDD `985hdda01.chd` | `a5b9ec9a5afa38f36af529d3aea5355ea7d022ca` | 1400595246 | `2efb5aa840f80841749bb624115fcfae45a1a18a` |

The CDs are public members of `MAME_0.185_CHDs_Merged`'s `(a-g).zip`; exact
member lengths/ZIP CRCs and full CHD verification pass. Its old archive lacks
this HDD. A public download linked by `mdk.cab/game/bmiidx2` supplies the HDD;
its byte identity and full raw/overall CHD verification pass. Restricted
archive entries were not accessed. Original download URLs and verification
output are retained with the evidence. HDD GDDD is 12495 cylinders, 16 heads,
63 sectors/track, 512 bytes/sector: 12,594,960 sectors / 6,448,619,520 logical
bytes. Runtime writes do not modify the original image.

```sh
python3 scripts/audit_twinkle_media.py --mame-source ../mamedev --rompath roms \
  --output-dir roms/audited/bmiidx2
python3 -m unittest discover -s tests -p test_twinkle_media.py
```

All three original images pass the full audit. The five media regressions
cover actual original chips/word order, a corrupted security chip, missing HDD,
unreviewed first style and a real DDR CD supplied to the IIDX identity check.
Explicit `--roms-only` produces an incomplete chip audit, with every disk
unverified. Neither mode claims native game execution or installs game data.

## Original sound-board-only execution

The [sound-board adapter](twinkle-board.md) now connects the pinned IDE HDD
engine to the 16 MHz MC68000, source CS0 registers, IRQ6/DMARQ/DMACK and banked
32 MiB RF5C400 sample RAM. Synthetic tests verify PIO, DMA, failures and source
timing; the shared 68000 scheduler's callback-safe deadline keeps STOP from
delaying a newly scheduled ATA interrupt until the next 60 Hz boundary.

`tests/probe_twinkle_sound.cpp` checks the original assembled sound-ROM SHA1
and HDD identity/geometry before running this component for ten emulated
seconds with real CHD sector callbacks. It injects no PSX mailbox commands.
The original ROM finishes the probe at PC `0053c4`, SR `2008`, ATA status
`50`, reads one actual HDD sector, rejects no writes and reports zero failed
reads, bus errors, callback faults or board faults. DMA count is zero and the
mailbox remains zero: this is sound-CPU initialization awaiting the main
system, not sample installation, original music or game acceptance.

```sh
make -f tests/Makefile.twinkle test sanitize
make -f tests/Makefile.m68000 test sanitize
make -f tests/Makefile.ata-storage test sanitize
make -f tests/Makefile.twinkle-hdd -j8
build/twinkle-hdd/probe-twinkle-sound roms/audited/bmiidx2/soundcpu.bin \
  roms/bmiidx2/985hdda01.chd
```

## Independent MAME reference

MAME 0.287 accepts the complete original set and reaches the GC985JA device
check. SCSI, CD-ROM, DVD, security and sound report OK. Initial backup RAM is
BAD and the game reports `E150 BACKUP DATA ERROR`, inviting 1P Start to clear
backup or Test mode. A 120-second run with a one-second Start pulse and a
separate 90-second run holding Start for ten seconds remain at E150. The
latter records actual IN0 `fe` while pressed and `ff` after release; input
delivery is verified, but successful backup initialization is not. The first
snapshot script's unsupported uppercase PSX `PC` lookup was corrected in the
subsequent runs; their logs and captured screens are retained separately.

The initial E150 attempts above happened before the sound checks had finished.
A subsequent Test pulse reaches the real operator menu. Waiting until 65–70
seconds before pressing Start clears backup through the original program;
the 120-second run reaches the IIDX 2nd style title / INSERT COIN screen and
persists 3264 nonzero XRAM bytes. This is independent MAME initialization,
not a native IIDX player result. No RTC/security bytes were supplied as an
invented initialized NVRAM seed. The original ROM-only sound-board probe
likewise injects no main-CPU commands.
A separate 80-second MAME process restores this runtime backup and reaches
the title again without any input, confirming that the original program's
initialization survives a restart.

## RTC component follow-up

The [source RTC65271 port](rtc65271-port.md) now supplies indirect registers,
all 4 KiB backup RAM, exact update/divider scheduling, alarms/IRQ and source
NVRAM serialization. O2/ASan/UBSan vectors preserve the pinned source's
calendar and status quirks. Replaying the actual original-game backup-clear
trace matches all 14,240 read responses, 35,618 writes, final registers/XRAM
and the complete 4152-byte MAME NVRAM exactly. This component evidence is
separate from the native PSX execution described below.

## Native main-board bring-up

`--twinkle REGION-DIR` accepts only the exact reviewed IIDX2 configuration,
three original chip identities and three original CHD identities. The separate
`--twinkle-fixture` selector accepts generated chips for regression coverage.
Both require BOARD=1/HAVE_CHD=1, ARCADE_TIMING=1 and interpreter execution.
The shared source clock profile selects 67.7376 MHz input, 33.8688 MHz core
cycles, multiplier 200 and the existing source-derived GPU cadence. Source 4 MiB RAM,
2 MiB VRAM/type-2 GPU, low-byte mailbox/RTC lanes, panel/turntable/volume
inputs, SIO0/1, original HDD-backed sound board, and NCR53CF96/XM-5401 CD
transport are now connected. The [source serial adapter](twinkle-serial.md)
connects the FDC37C665GT configuration/decoder, two NS16550s and the original
JVC XV-D701 control state machine. DMA5 completes its PSX channel immediately as in
source while the payload remains gated by actual NCR DRQ. Original media is
opened read-only. The video CD is audited/opened but DVD presentation is not
implemented. LED and source video-mixer serial shift latches are retained.

The M24C02-W pin state machine is extracted from the pinned `i2cmem.cpp`,
with its original 16-byte page buffer, STOP commit, device select, sequential
read and write protection. Port fields update SDA before SCL, following the
source ioport field-mask order. No security response or initialized backup is
invented. An independently observed original MAME boot supplies 3,709 port
transactions (2,422 reads); replay matches every read and all 256 final EEPROM
bytes under O2 and ASan/UBSan. The generator verifies both source SHA256s.

Board persistence is an exact 4,440-byte file: a 32-byte tagged, little-endian
length/CRC header, 4,152-byte source RTC image and 256 mutable EEPROM bytes.
It is cold-start NVRAM, not CPU/timer/sample-RAM save state. Malformed or corrupt
images are rejected atomically. The configured 1.2-second watchdog is armed
and rearmed by real writes; expiry stops explicitly until full machine-reset
sequencing is implemented. Unsupported floppy/parallel subdevice accesses still stop explicitly; UART
registers and configuration use the source map, and unmapped upper lanes
remain source zero.

```sh
python3 scripts/prepare_twinkle_board.py --mame-source ../mamedev --rompath roms \
  --output-dir roms/prepared/bmiidx2
make -f Makefile.mac-harness twinkle-test
make -f tests/Makefile.m24c02 test sanitize
python3 libpcsxcore/arcade/m24c02_generate.py ../mamedev --check
build/mac-harness/pcsx-harness --twinkle roms/prepared/bmiidx2 --frames 7200 \
  --output /tmp/iidx2.ppm
```

The original BIOS passes ROM/memory/video checks, loads and executes the CD
program, and now completes UART/JVC control instead of stopping at its first
UART access. In a fresh 9,000-frame run, a real 1P Start pulse at frames
5,400–5,700 clears the original E150 backup error after the sound checks finish.
The program writes 3,264 nonzero RTC XRAM bytes and reaches the original title /
INSERT COIN screen. No initialized MAME NVRAM or invented security bytes are
imported. The 4,440-byte native NVRAM retains the original EEPROM SHA1 and a
valid payload CRC.

Separate 6,600-frame processes restore copies of that runtime NVRAM without
inputs on GL and NEON. Both reach the original `How to play` attract/tutorial
screen with identical PC `8005f9f0`, 3,736,392,002 cycles and final state:

| Native warm artifact | SHA256, equal on GL and NEON |
| --- | --- |
| 4 MiB main RAM | `a242c9c78fa8606b629d69353167dd81002384e5a6dc4d921c07b3bd4d02fdac` |
| 2 MiB VRAM dump including 16-byte header | `db834b6b048bed273a7918671aac2366d55caec031e82e045b36dd3d25687baf` |
| 4,440-byte NVRAM | `3b5de8428f79c2b91e2e13f67b53343fc9407a8ac01a4c46ce35caa8c2a551b2` |

Both warm runs execute three original JVC commands and send 33 serial reply
bytes. NCR/SCSI completes 114 DMA requests / 2,965,688 bytes. The sound CPU
reads 68,473 HDD sectors and transfers 17,383,424 ATA words to sample RAM.
RF5C400 produces 4,865,093 source frames, including 1,031,121 nonzero frames
with peak 20,874. Serial, sound CPU, disk reads, NCR, target and RTC report zero
faults. These counters and visible captures establish original boot, backup
persistence and tutorial/attract progress. Speaker output and the successful
timed-note probe are verified separately below.

## Native speaker WAV

`--mixed-audio FILE.wav` now records the source speaker routes at 44.1 kHz:
SPU gain 0.75 with direct channels, program-CD CDDA gain 1, and RF5C400 direct
outputs 0/1 at gain 1. Effects outputs 2/3 remain unrouted as in the pinned
board. The existing shared timestamped mixer retains wide RF values as float
until the final SPU/CD/RF sum and signed-16 saturation. No independent RF
clipping, volume normalization, channel swapping or resampling is added.

The RF callback checks each completed 384-chip-cycle frame and its exact
sample index. Its input joins continuous CD DAC samples and the SPU's
768-main-cycle sample cadence. Output waits for all three sources; an SPU
sample rounded into the future remains pending. Source CPU/RF clock phases
and real CD/media failures are preserved, with clock/history errors stopping
explicitly. Raw `--audio` capture can run alongside the speaker WAV.

Both original 6,600-frame warm runs produce exactly 4,865,093 speaker frames,
1,031,121 nonzero frames and peak 20,874 without clipping or audio faults.
The actual recorded segment is mono within the two-channel format, matching
this original RF output; the unit vectors separately exercise distinct left
and right routes. SPU and CDDA are silent in this run; the RF board supplies
the sound. One future SPU frame remains pending and is absent from the
speaker file. GL/NEON WAVs are byte-identical, SHA256
`c7b26f704537dda9d19ae50c4a913599f961c6f4f11211884b713537a20e0b73`.
Raw SPU WAVs are also identical, SHA256
`633684828cd3e33d08289160ec8678102415f96da6d81527dd03c07d4eb45d7d`.
RAM, VRAM and NVRAM remain byte-identical to the warm run before adding audio.

The generated MIPS regression now captures both raw and mixed WAVs and checks
all three source clocks, actual frame counts, final one-frame lookahead and
error-free silent fixture output on GL and NEON. Shared O2/ASan/UBSan route
vectors check direct SPU gain 0.75, fractional cancellation against RF,
unsaturated RF values above signed-16 range, source deferral and final clipping.
GV's shared SCSI/SPU harness regression and the HAVE_CHD=0 rejection pass.

A separate original player sequence inserts two coins, presses Start and
selects the first song, reaching `1st Stage / PIANO AMBIENT / 5.1.1`.
A 7,800-frame input stress run produces game `POOR` judgements but does not
establish a successful note hit or positive score. It crosses the 32-bit CPU
cycle wrap at 4,415,736,005 total cycles without changing the audio epoch.
GL/NEON state and speaker WAVs remain identical. RF peak 54,578 is retained
until final mixing; 6,968 channel values saturate at the source output limit,
with zero clock/device/audio faults. The speaker hash is
`57f4beb3015e34543a794e3ca36e2adbfc02660aa4345bd7da58b53843c9557c`.
A following measured timing probe resolves that gate for a P1 note. The same
falling key-7 note moves from y=77 at frame 7,800 to y=97 at frame 7,820.
A real key-7 press at frame 7,883, released at 7,885, produces a visibly
positive score of **1,204** by frame 7,896 on both renderers. Main RAM
`8017e1a8` changes from zero in the stress capture to 1,204, independently
corroborating the on-screen score. No guest code, chart, score or NVRAM is
patched; only ordinary coin, Start, key and turntable input events are sent.
This is a timed-note acceptance check, not a full-song clear or a judgement
accuracy benchmark.

The 7,896-frame GL and NEON runs again match exactly:

| Timed-note artifact | SHA256 |
| --- | --- |
| 4 MiB main RAM | `acc4674fe4b47adfa9441f8dd16d72b23636924ca5db3d3e7fb082db9c38cd58` |
| VRAM dump | `27a5cb08e7f40e6ae59caee94f46569dede0fd7a90ae29ad2e0d286de10ed751` |
| NVRAM | `3b5de8428f79c2b91e2e13f67b53343fc9407a8ac01a4c46ce35caa8c2a551b2` |
| Speaker WAV | `870dd2476be9d1b3cc08c7114782362737b4f20d3c25cec7b443c184ddb51d77` |

`tests/check_twinkle_original_harness.py` generates the exact 6,687 ordered
input events, runs the normal original-media loader on GL and NEON with
separate copies of a valid native NVRAM seed, and checks score, source identity,
cycle wrap, device/audio faults, PCM format/signal and all four equal artifacts.
It requires the original local media; generated fixtures never satisfy it.
Use a new output directory:

```sh
python3 tests/check_twinkle_original_harness.py \
  --neon build/mac-harness/pcsx-harness \
  --gl build/mac-harness-vitagl-like/pcsx-harness \
  --profile roms/prepared/bmiidx2 --nvram-seed /path/to/native-initialized.nv \
  --evidence-dir /tmp/iidx2-original-replay
python3 tests/check_twinkle_original_harness.py \
  --evidence-dir artifacts/boards-mac-20261015/twinkle/bmiidx2/player --verify-only
```

The recorded original runs pass the verifier; both positive-score captures
have been visually inspected. `--verify-only` reads already captured evidence
without launching either emulator.

Generated MIPS tests separately check upper 4 MiB RAM, mapped/unmapped byte
lanes, mailbox, RTC paging, button/turntable/volume values, Sony SIO register
access, a real CHD sector through NCR/SCSI/DMA5, M24C02 pin writes/readback,
UART scratch/modem mapped and unmapped lanes, checksummed cold/warm NVRAM,
corrupt-image retention, original-chip rejection,
DMA bounds, watchdog expiry and explicit unsupported floppy failure. The same
vectors pass on GL/NEON; the HAVE_CHD=0 build rejects the profile. Existing
GV SCSI/DMA and the normal MIPS/GPU smoke regression also pass. Shared SPU
DMA timing/large-payload acceptance also runs with this board selected.

Remaining native work is a full-song clear/result, additional player/mode
coverage, live SDL audio/interactive cabinet controls/presentation and watchdog reset
sequencing. DVD video decoding is absent in the pinned JVC reference too; the
control protocol follows that source and is tracked separately from video.
Unused floppy/parallel controller implementation remains explicit unsupported
scope. The full board matrix remains open.

Evidence is retained under ignored
`artifacts/boards-mac-20261015/twinkle/bmiidx2`, with the serial/title/warm
evidence in its `serial` subdirectory; ROMs, CHDs, mutable state,
binaries and captures are not committed.
