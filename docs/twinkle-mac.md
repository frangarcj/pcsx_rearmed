# Twinkle / IIDX 2nd style Mac validation work

The representative is original beatmania IIDX 2nd style GC985 JAA (`bmiidx2`),
machine/input `twinklei`, parent BIOS `gq863`. Pinned MAME
`deb2f54fe7300e7a192690318beb860159bfd52a`, `konami/twinkle.cpp`, marks it
`MACHINE_IMPERFECT_SOUND`, without the first style's `MACHINE_NOT_WORKING`.
This row remains open: the native interpreter now boots the original program
into its device check, but UART/DVD integration and player validation remain
pending. Original native execution, component vectors and the independent
MAME reference are tracked separately.

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
transport are now connected. DMA5 completes its PSX channel immediately as in
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
sequencing is implemented. FDC37C665GT/NS16550 accesses on mapped byte lanes
also stop explicitly; unmapped upper lanes remain source zero.

```sh
python3 scripts/prepare_twinkle_board.py --mame-source ../mamedev --rompath roms \
  --output-dir roms/prepared/bmiidx2
make -f Makefile.mac-harness twinkle-test
make -f tests/Makefile.m24c02 test sanitize
python3 libpcsxcore/arcade/m24c02_generate.py ../mamedev --check
build/mac-harness/pcsx-harness --twinkle roms/prepared/bmiidx2 --frames 7200 \
  --output /tmp/iidx2.ppm
```

The original BIOS displays `TWINKLE BOOT PROGRAM`, passes ROM/memory/video
checks, loads and executes the original CD program, and reaches `GC985JA
DEVICE CHECK` with SCSI/CD-ROM OK. With the EEPROM connected, the first
unimplemented access is UART2 at physical `1f2105f8`, main PC `8005341c`,
CPU cycle 943,717,072 (about 27.864 emulated seconds). At this stop: 62 real
SCSI DMA requests / 833,720 bytes, 2,901 successful HDD sector reads and
606,208 ATA DMA words in sample RAM; NCR, target, RTC and sound CPU report
zero device faults; 655 real watchdog writes keep the timer armed. GL and
NEON stop at the same CPU cycle/PC, with identical 4 MiB RAM (SHA256
`737a85f1054b4b8173f0cf43cdc7c61fd116bd7b81255936048d9e13f4600e5e`)
and synchronized 2 MiB VRAM dump files (16-byte header plus pixels, SHA256
`ce5fafd18f4d66277905b10038bf89382c7a45e9305489f8d2c53234bb661221`).
Both captures visibly show the device check. RF output is still silent.
This proves partial original native
boot and sample transfer, not complete installation, music, player acceptance,
or a playable board. No MAME NVRAM is imported into the native run.

Generated MIPS tests separately check upper 4 MiB RAM, mapped/unmapped byte
lanes, mailbox, RTC paging, button/turntable/volume values, Sony SIO register
access, a real CHD sector through NCR/SCSI/DMA5, M24C02 pin writes/readback,
checksummed cold/warm NVRAM, corrupt-image retention, original-chip rejection,
DMA bounds, watchdog expiry and explicit unsupported UART failure. The same
vectors pass on GL/NEON; the HAVE_CHD=0 build rejects the profile. Existing
GV SCSI/DMA and the normal MIPS/GPU smoke regression also pass. Shared SPU
DMA timing/large-payload acceptance also runs with this board selected.

Remaining native work is FDC37C665GT/NS16550/JVC DVD control, complete speaker
mixing, watchdog reset sequencing and cabinet presentation, followed by actual
backup initialization/warm boot, playable notes and music on GL/NEON. The
full board matrix remains open.

Evidence is retained under ignored
`artifacts/boards-mac-20261015/twinkle/bmiidx2`; ROMs, CHDs, mutable state,
binaries and captures are not committed.
