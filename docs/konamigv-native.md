# Native Konami GV common-map integration

`libpcsxcore/systemgv.c` connects the standalone common board to the Mac
PCSX CPU, GPU, RAM, reset, input, EEPROM and DMA5 paths. It is built only with
`BOARD=1 HAVE_CHD=1`; the Vita System12 build has no GV dependencies.

The normal `--system-gv REGION-DIR` loader accepts the seven common-map source
profiles: `hyperath`, `lacrazyc`, `nagano98`, `naganoj`, `powyak96`, `susume`
and `weddingr`. It checks the complete preparer's `board.ini`, pinned manifest
metadata, exact BIOS/EEPROM sizes and SHA1s, and the declared CHD header SHA1
before opening the CHD-backed XM-5401 target. Source `BAD_DUMP` labels persist.
The preparer performs full CHD hunk verification; runtime loading checks the
header identity and decoder. Special native profiles now also include
`simpbowl` (flash/trackball; `simpbowl-mac.md`), `btchamp` (Sharp flash/two
trackballs; `btchamp-mac.md`) and `tmoshs`/`tmoshsp`
(sensors/virtual printer; `tokimeki-mac.md`), plus `kdeadeye` (two guns and
authenticated source Sharp flash; `kdeadeye-mac.md`). The BIOS root,
base Tokimeki and the alternate Plus profile remain rejected.
Hyper Athlete has an authenticated Mac player run below.

The board uses 2 MiB main RAM, a 512 KiB BIOS and a type-2 GPU with 1 MiB VRAM.
MMIO goes through the shared board's width/lane decoders; NCR IRQ drives PSX
IRQ10. PCSX cycles are converted to monotonic nanoseconds with retained
fractional remainder and overflow checks. Reports distinguish the 67.7376 MHz
source oscillator, MAME's four oscillator clocks per CPU execution cycle and
the 33.8688 MHz PCSX cycle-counter domain. This is not a timing-equivalence
claim: PCSX instruction timing and its configured cycle multiplier remain
separate from the MAME scheduler.

DMA5 follows the pinned MAME PSX DMA dispatcher: `01000000` uses raw BCR;
`01000200` reads and `01000201` writes use BCR block length/count with a zero
count meaning 65536;
masked CD read mode `(CHCR & ffbffeff) == 11000000` uses
`max(BCR.high, 1) * BCR.low`; `11050100`/`11150100` use raw BCR for writes.
The source dispatcher finishes channel 5 `01000201` immediately, and its
`dma_finished` callback then delivers the write payload. Both source functions
are required; skipping the callback stalls the game's MODE SELECT command.
Completion clears CHCR start/trigger and raises the ordinary DMA interrupt
immediately after registering the request. Actual bytes remain pending until
NCR DRQ permits transfer; no payload or CD command success is fabricated.

The explicit `--system-gv-fixture TEST-CHD` path generates its own BIOS in
memory, without relaxing the authenticated game loader. Its executed MIPS
program reads a pressed input, writes a RAM signature, issues real DMA5
register stores and resets the NCR bus. It checks the masked CD-DMA mode's
block-mode write and 12-word read requests before the zero-block mode's 1 MiB request. The fixture verifies immediate CHCR
completion, a 1 MiB request still pending with DRQ low, the delayed NCR IRQ10
and zero device faults. It checks a real eight-sector CHD made by `chdman`;
this fixture does not issue a game CD read or transfer that pending payload.
A second, destructive synthetic-target check sends a real MODE SELECT CDB and
12-byte block descriptor through NCR MMIO and DMA5 CHCR `01000201`, and requires
the CD target to change its block size from 2048 to 512 with no pending bytes.
This catches a completed CHCR whose write payload was silently discarded.

Both interpreter and native ARM64 DRC fixtures pass. The DRC registers the
arcade memory-table path with the board's fixed code mappings and accepts the
2 MiB page geometry. The test executes through the selected CPU backend and
includes the architectural MIPS load-delay slot. The no-board console smoke
also passes and rejects the GV option with a clear rebuild requirement.

```sh
make -f Makefile.mac-harness BUILD=build/mac-harness-gv-drc \
  BOARD=1 HAVE_CHD=1 DRC=1 systemgv-test
```

Authentic ARM64 DRC still routes Sony serial MMIO through console handlers and
does not pass Hyper Athlete startup. CDDA mixing, source timing comparison,
special game peripherals and Vita integration remain unverified.

## Authentic Mac startup follow-up

Hyper Athlete's verified prepared package exposed four native startup issues:
`disk.cdrom` contains the source filename with `.chd`; both CPU Sony serial
ports must use their source device state rather than console's idle SIO1 flags;
CXD8514Q uses GPU type 2 (only CXD8538Q selects type 1 in pinned `video/psx.cpp`);
and the source `ROM_REGION16_BE` EEPROM dump must be normalized to the 93C46
port's little-endian word storage after its raw SHA1 check. The normalized default
matches independent MAME NVRAM byte for byte. Device selftests cover serial byte,
word and dword routing, GPU info type and the default EEPROM's exported bytes.

The additional DMA write fix now lets the native interpreter/GL enter a P1
running event with exercised inputs, nonzero SPU PCM and zero board faults. Mac `--nvram` uses
128-byte runtime EEPROM images for GV; `--audio` captures SPU. CDDA mixing remains
pending. See [the player evidence](konamigv-mac.md) and
[the ongoing board validation matrix](arcade-mac-validation.md).
