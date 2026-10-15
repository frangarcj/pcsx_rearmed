# Twinkle / IIDX 2nd style Mac validation work

The representative is original beatmania IIDX 2nd style GC985 JAA (`bmiidx2`),
machine/input `twinklei`, parent BIOS `gq863`. Pinned MAME
`deb2f54fe7300e7a192690318beb860159bfd52a`, `konami/twinkle.cpp`, marks it
`MACHINE_IMPERFECT_SOUND`, without the first style's `MACHINE_NOT_WORKING`.
This row remains open: this project has no native main-PSX Twinkle profile or
player run yet. Original media auditing, the sound-board component and an
independent MAME reference are separate evidence.

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

This reference is an initialization checkpoint, not an IIDX player result.
No RTC/security bytes were supplied as an invented initialized NVRAM seed.
The original ROM-only sound-board probe likewise injects no main-CPU commands.

The remaining native work includes the 4 MiB main PSX profile, 2 MiB VRAM,
NCR/SCSI DMA and program CD, panel/turntable inputs, RTC65271, watchdog,
FDC37C665GT/UART/DVD control, original I2C security and complete speaker
integration. Original boot, installation, warm state, playable notes and music
must then be validated on GL/NEON. The full board matrix remains open.

Evidence is retained under ignored
`artifacts/boards-mac-20261015/twinkle/bmiidx2`; ROMs, CHDs, mutable state,
binaries and captures are not committed.
