# Standalone Konami GQ common board

This adapter ports the shared Konami GQ hardware and maps from MAME's
`src/mame/konami/konamigq.cpp` at revision
deb2f54fe7300e7a192690318beb860159bfd52a. It includes the PSX-side NCR53CF96
at 16 MHz (initiator ID 7, PSX IRQ10), SCSI hard disk at ID 0, the 4 MHz
dual-channel MB89371, 93C46 16-bit EEPROM, K056800 host/sound ports, two
18.432 MHz K054539 PCM devices and the 8 MHz sound MC68000 with its 64 KiB RAM.
The TMS57002 runs at 24 MHz with its distinct 256 KiB data RAM. PCM shared RAM
is 0x380000 bytes; the PSX map exposes the source's 3 MiB aperture. PSX RAM is
caller-owned and 4 MiB, matching this driver's `set_default_size("4M")`.

The PSX map follows source lane masks: NCR at `0x1f000000`, K056800 at
`0x1f100000`, EEPROM/reset output at `0x1f180000`, lamp and gun-trigger write
no-ops at `0x1f198000` and `0x1f1a0000`, six raw lightgun axes at
`0x1f200000` through `0x1f228000`, P1/P2 and P3/service inputs at
`0x1f230000` and `0x1f230004`, DSW at `0x1f238000`, shared PCM RAM at
`0x1f300000`, DUART at `0x1f680000`, and watchdog write no-op at `0x1f780000`.
Gun and switch ports retain all 32 input bits, including gun X bit 8 in the
second byte. Inactive byte lanes of the NCR, PCM RAM, MIRAC and DUART delegates
read zero, matching the pinned default-zero program map. Sound-side unused
lanes and unmapped reads also return zero; the 16-bit DSP status is 0005 after
reset. Independent MAME CPU-space probes cover these values. These bus contracts
are tested separately from synthetic sound/DMA guests and authentic game boot.

Input callbacks provide raw port words; the adapter does not generate gun
coordinates or switch states. The DSW callback supplies switch bits; mapped bit
16 is replaced with the EEPROM's DO pin, matching the source custom input.

The sound MC68000 maps caller-supplied CPU-visible 512 KiB firmware, 64 KiB
RAM, both K054539 register banks on the source byte lanes, the TMS57002 data
port and status/control word, and the K056800 sound port. Sound CPU register
accesses carry the core's current emulated-cycle timestamp and advance the PCM
chip before writes that update its stream. K054539 samples are exposed as two
separate raw stereo routes. Source timer IRQ2 is gated by sound-control bit 0
and rising edges; K056800 drives sound IRQ1. TMS sync is scheduled at 48 kHz
and is gated by sound-control bit 5. The board offers an explicit DASP sample
input/output call; it does not invent inputs or a route for the unmodeled
058141 analog module.

The EEPROM image is caller-persisted. Without an image, its 64 words use the
exact GQ default table. EEPROM output bits 0..2 drive DI/CS/CLK and bit 6
releases the sound CPU from its active-low reset. Reset cancels channel-5 DMA,
resets the sound CPU and peripheral children, retains RAM/EEPROM contents,
and applies the machine reset's channel-0 CTS-low output; MB89371 mode/baud
registers retain their wrapper reset behavior.

DMA channel 5 transfers `word_count * 4` bytes only while NCR DRQ is active.
The caller supplies bounded byte access to the board's 4 MiB PSX RAM. NCR and
hard-disk timer deadlines, DUART BRG edges, DSP 48 kHz syncs, and K054539 sample
edges and K054539 timer edges are advanced in timestamp order on a monotonic
integer-nanosecond clock. The 18.432 MHz and 24 MHz clocks are sampled on that
nanosecond grid. No host sleep or per-68000-cycle polling is used. Callbacks
run synchronously; mutating calls from callbacks are rejected, and the board
must not be destroyed from one of its callbacks.

The adapter does not include the PSX BIOS/video device or any game firmware,
CHD image, cabinet sensors, or the 058141 analog sound module. It makes no game
boot or complete-audio claim. Mount a real CHD-backed HDD through the caller's
sector callbacks; WRITE requires a writable backend such as a CHD overlay. An
absent disk cannot be created as a ready target.
