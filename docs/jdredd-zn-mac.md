# Judge Dredd / Acclaim ZN-1 work in progress

Judge Dredd Rev C (`jdredd`) now has source-audited original ROM/HDD media and
native Mac daughterboard wiring for ATA, IRQ10, multiplexed pistols and GPU
coordinate queries. Analytical and native MIPS/SDL tests pass, but the authentic
native game **does not yet pass boot or player validation**. The first HDD
read is requested but its IRQ-driven firmware path does not consume the sector;
the loader reaches `HDLOAD vB1.7 Error 001 / Can't find _GAME.EXE`. Independent
MAME 0.287 instead reaches the expected cold-NVRAM restore prompt.

## Source and media

Source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`, `sony/zn.cpp`,
`jdredd_state`, `ROM_START(jdredd)`, `jdredd` inputs, `psx/irq.cpp`, GPU
`video/psx.cpp`, and ATA `bus/ata/ataintf.cpp` / `hdd.h` with its preserved
`atahle` / `atastorage` implementation. Rev B has its own HDD and is not claimed
by this profile.

The user's TeknoParrot package is 423454130 bytes, SHA1
`892638b1410c4c9ca8b6d8b13479cfbf20b036dd`. Exact contents are `coh1000a.zip`,
`jdredd.zip` and `jdredd/jdreddc.chd`. All five original chips pass pinned size,
CRC and SHA1. The unused uPD78081 NO_DUMP declaration is omitted only because
source uses behavioral `znmcu_device`; no replacement firmware is supplied.

Original CHD v5: 424257837 compressed bytes, 1082253312 logical bytes, geometry
2097 cylinders / 16 heads / 63 sectors, 2113776 sectors of 512 bytes. Source
SHA1 is `eee205f83e5f590f8baf36452c873d7063156bd0`; data SHA1 is
`83ed8df25d100b1c060f4dde2f162ba31803db7d`. `chdman verify` checks full data
and metadata before preparation. Source media stay immutable; optional runtime
sector writes use `jdreddc.overlay` separately. Source has no EEPROM dump;
common AT28C16 defaults to FF and exports the existing 2080-byte data/ID image.

```sh
python3 scripts/prepare_jdredd_zn_board.py --set jdredd \
  --mame-source ../mamedev \
  --rompath /tmp/pcsx-boards-mac-20261014/jdredd-tp \
  --output-dir roms/prepared/jdredd
python3 scripts/generate_jdredd_zn_profiles.py --check
```

## Native source wiring

COH1000A retains common ZN/MCU/CAT702/SIO/AT28C16, 4 MiB RAM and 2 MiB type-2
VRAM. The 2 MiB original ROM window is fixed and read-only. Parent bank and
auxiliary writes are source log-only operations. No invented ROM switching,
HDD DMA5 handler, watchdog or RAX daughterboard is added.

ATA CS1 is 1fbfff80–1fbfff8f; CS0 is 1fbfff90–1fbfff9f. Source 16-bit delegates
on the little-endian 32-bit bus target distinct halfword registers. A 32-bit
read at DATA consumes one data word and reads ERROR in the upper halfword,
unlike Primal's two-data-word VIA bridge. The abstract ATA interface starts
its wired-AND result at mem_mask; source HDD task-file writes ignore mem_mask
when storing their data. High-byte sector-count writes with a zero low byte
therefore select 256 sectors. Guest SET FEATURES transfer width is retained.
Status acknowledges ATA IRQ; alternate status preserves it. The native host
routes the real line to PSX IRQ10. Judge's I_STAT acknowledgement follows the
pinned PSX IRQ controller's `pending & enabled & written_data` contract.

Each rising VBlank toggles the source gun mux, starting with P2. The raw selected
X/Y latch is accepted only with `x > 0393 && x < 0cb2` and
`y > 002d && y < 0217`; offscreen positions retain previous coordinates until
software clears them. SERVICE bit 08 reports the active-low mux. Trigger is
SERVICE 40/80, second gun button is P1/P2 bit 10; unrelated directions, third
buttons and P3/P4 Starts remain unsupported as source declares.

GPU GP1(10h,08h) exposes raw X/Y; GP1(0dh) clears them. Repeated identical clear
commands must bypass the ordinary register cache after a new VBlank sample.
Normal GPU reset preserves the lightgun latch, matching source. Cold GPU init
starts with zero coordinates. These are hardware-facing GPU fields; no GPU
readback is used to synthesize shots.

SDL maps the rendered viewport to the source valid raw range. Left mouse is
trigger and right mouse is the second button. Offscreen clicks still deliver
trigger with invalid raw coordinates; focus loss releases buttons. A mouse
movement takes aim ownership from the P1 stick until another stick-axis event;
P2 has its own stick coordinates. Replay accepts raw 16-bit `p1gunx/p1guny` /
`p2gunx/p2guny`, separate from digital pressed values. Source SPU gain 0.35 is
routed to ordinary 44.1 kHz SDL playback. Live game sound remains unproven.

## Component evidence and unresolved boot

```sh
make -f tests/Makefile.jdredd-zn-bus test sanitize
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 HAVE_CHD=1 jdredd-zn-test gnet-frontend-test
make -f Makefile.mac-harness BUILD=build/mac-harness-primal-neon \
  GPU=neon DRC=0 HAVE_CHD=1 jdredd-zn-test
```

O2 and ASan/UBSan pass direct ATA16 lanes, real HDD identify/sector read/write,
status/alternate IRQ handling, guest byte-transfer mode, ROM protection,
VBlank multiplexing and exact offscreen boundaries. GL/NEON native MIPS guests
verify IRQ10, a CHD-backed 512-byte sector, GPU raw coordinate query, repeated
GP1 clear, preserved latch across GPU reset, active-low mux, source I_STAT mask,
NVRAM roundtrip and the original SPU DMA timing. Ordinary loading rejects
synthetic fixture chips. Production SDL tests cover mouse/gamepad aim, viewport,
offscreen trigger and focus release; existing frontend families still pass.

These tests do not establish the authentic firmware's full IRQ callback path.
Its first 256 IDENTIFY words match MAME byte for byte. Both then issue SET
MULTIPLE (8), SET FEATURES (PIO mode 4) and READ MULTIPLE for sector zero.
Native asserts IRQ10 and reads status 58, but never reads the sector's data
port. MAME consumes that data and continues to subsequent sectors. Native
reports no device/backing errors, so a zero diagnostic flag is not accepted
as game compatibility. This is the next investigation, before restoring NVRAM,
calibrating guns and testing a live shooting stage.

The former native black run stalled in BIOS dispatch; source I_STAT semantics
now produce a visible loader error, still not a passing run. Temporary ATA/CPU
traces are preserved under `/tmp/pcsx-boards-mac-20261014`; successful analytical
and fixture logs and independent MAME snapshots are under
`artifacts/boards-mac-20261014`. Normal reproduction is:

```sh
build/mac-harness-primal-neon/pcsx-harness --jdredd-zn roms/prepared/jdredd \
  --frames 600 --max-blocks 800000000 \
  --output artifacts/boards-mac-20261014/jdredd-boot-neon.ppm
```

The overall Mac board-validation goal remains open. Judge player gameplay,
nonzero authentic sound, warm-game persistence, physical Vita, authentic DRC
and whole-machine timing remain unverified.
