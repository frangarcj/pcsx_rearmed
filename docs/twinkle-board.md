# Twinkle sound board and source IDE HDD path

This module follows the sound-board map and board wiring in the pinned MAME
`src/mame/konami/twinkle.cpp`. The source identifies the sound CPU as a
16 MHz MC68000, with 512 KiB ROM, 256 KiB RAM, a CY7C131 1 KiB dual-port
mailbox, an RF5C400 at 16.9344 MHz, and 32 MiB of sample RAM. The MC68000's
wave window is 8 MiB; the bank register selects a word offset in 4M-word
increments, as in MAME's handler. RF5C400 external-memory callbacks see the
whole 25-bit byte-addressed RAM.

The board object borrows the exact-size ROM and wave-RAM buffers. Its CPU bus
implements ROM/RAM, mailbox, RF5C400 registers, wave-bank accesses, the LED
latch, and the SPU control register. The SPU control write acknowledges only
the first falling IRQ-enable bit in source order (IRQ1, IRQ2, IRQ4, IRQ6).
The mailbox reproduces the MAME 0x3fe right/main-side interrupt request and
left/sound-side read acknowledge. Only the mapped low byte lane has side
effects; a word read returns the low mailbox byte with a zero high lane, and
a high-lane-only read returns zero without acknowledging IRQ4. These source
address-space defaults replace the foundation's incorrect assumed FF lanes.
The ATA-DMA address registers retain the source's word-index latch.
CPU-to-RF clock conversion
uses the exact 1323/1250 ratio; audio samples are emitted by the existing
RF5C400 callback. The source `nopr` DMA/bank reads and the unmapped read at
write-no-op `0x34000e` return zero, following `emumem_aspace.cpp` / the driver's
map without an `unmap_value_high` override. Soft board reset
leaves board RAM, mailbox, bank/control latches, wave RAM, RF5C400 state, and
absolute timer phase intact; the pinned driver's `machine_reset` resets only
its SCSI DMA state.

Optional HDD backing now connects CS0 at `0x300000` to the existing pinned
MAME ATA HLE engine in IDE HDD mode. Callers provide real sector callbacks,
geometry and original IDENTIFY metadata when present. The board owns the
device and its IRQ/DMARQ lines. With no backing configured, CS0 still fails
as unsupported; no medium, sectors or ready status are supplied. The main CPU's SCSI, panel I/O, RTC,
watchdog, FDC, display/video, DVD, security dongle, and cabinet controls are
outside this sound-board component. The periodic 60 Hz IRQ1 source uses exact
integer-cycle deadlines `floor(n * 16,000,000 / 60)`, with intervals of
266666, 266667, 266667 cycles. Board advance is split at each deadline.

The source DMA handler raises DMACK, consumes words until DMARQ falls, and
stores ATA words without byte swapping at `bank * 0x400000 + dma_word_index`,
incrementing that word index. The adapter defers this drain until the ATA
callback has returned; it does not recursively mutate the ATA device. Bounds
failure stops before consuming an unstoreable word. ATA IRQ raises level 6
only when control bit `0x0400` is set; the existing source falling-bit control
acknowledgement clears it. No extra IRQ2 completion is invented.

Read-only ATA timer deadlines now let the owner split 68000 execution at real
device events. A callback-safe CPU scheduler hook can shorten a running slice
without changing its current timestamp or losing partial-instruction debt.
This handles a guest issuing a command and immediately entering STOP; waiting
until the next 60 Hz boundary would deliver the ATA interrupt too late.
RF audio advances to each actual ATA deadline before DMA changes sample RAM.
Device reset preserves the HDD, wave RAM and continuous clocks, matching the
source machine-reset scope.

The synthetic test runs real MC68000 instructions from a test ROM and checks
RAM, banked wave writes, RF register accesses and nonzero sample callback
data, memory masks, the IRQ4 mailbox handshake, and IRQ1 deadlines under long
and split run slices. Callbacks run synchronously during board advance; do
not reset, mutate, destroy, or otherwise reenter the board from a callback.
Additional tests cover source IDE IDENTIFY, PIO sector data, read failure,
two-sector DMA into a nonzero bank/index, STOP/IRQ6 wakeup before IRQ1,
coarse/17-cycle execution equivalence, reset retention and an end-of-RAM DMA
fault. Disabling the dynamic CPU deadline in an independently built variant
fails the STOP-wakeup assertion even though data transfer completes. O2 and
ASan/UBSan checks cover the CPU scheduler, ATA engine and board separately.
These fixtures do not demonstrate original game boot or complete sound output.
An original sound-ROM/HDD-only probe is documented in `twinkle-mac.md`.
The upstream MAME-derived board wiring is
BSD-3-Clause; full retained terms are in
`libpcsxcore/arcade/twinkle_board_LICENSE.txt`.

The standalone test links the existing M68000 core, which currently requires
C++20 (`std::countr_zero`). An earlier Vita check compiled the foundation
adapter object for ARMv7 against generated M68000 headers; it predates this
HDD binding and was not a linked board or runtime test. RF5C400 register programming uses the board
bus API so it always observes CPU/RF clock synchronization; callers cannot
obtain mutable CPU or RF device handles from this adapter.

Run `make -f tests/Makefile.twinkle test` and `make -f tests/Makefile.twinkle
sanitize` for the standalone normal and ASan/UBSan checks.
