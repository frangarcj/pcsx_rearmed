# Standalone Konami GV common board

The standalone konamigv adapter ports the shared map and common hardware
setup/reset path from MAME src/mame/konami/konamigv.cpp at revision
deb2f54fe7300e7a192690318beb860159bfd52a. It owns an NCR 53CF96 at 16 MHz
and SCSI ID 7, Toshiba XM-5401 CD target ID 4, 4 MHz MB89371 dual UART, and a
16-bit 93C46 EEPROM. The caller supplies CD media operations, two-megabyte host
RAM byte callbacks, and raw active-low P1/P2/P3-P4 port values.

The common map includes the 32-byte NCR window with its low-byte 16-bit lanes,
three 32-bit input ports at 0x1f100000, EEPROM output at 0x1f180000, the
two DUART windows at 0x1f680000, and the watchdog write no-op at
0x1f780000. P1 bit 0x2000 is replaced by EEPROM DO. Accesses support 8-,
16-, and 32-bit widths and honor byte write masks. No game-specific flash,
gun, trackball, printer, or sensor behavior is implied by this common adapter.

Inactive delegate byte lanes read zero, matching the pinned PSX program map.
NCR clock, test and FIFO-align registers (9, 10, 15) have write-only delegates
and also read zero at this board aperture. The standalone NCR register API
retains its distinct unmapped-register behavior. Independent MAME CPU-space
probes and component regressions cover these bus contracts.

DMA channel 5 stores the requested buffer direction, byte address, and
n_size * 4 remaining count. It transfers bytes only while NCR DRQ is asserted;
a request can remain pending until a later DRQ edge and can be advanced in
small virtual-time slices. RAM ranges are checked against the board's 2 MiB
span. Bus flushes happen after controller calls, outside NCR callbacks. No DMA
status, payload, IRQ, or media response is fabricated.

konamigv_advance_to uses absolute monotonic virtual nanoseconds and advances
the earliest NCR cycle deadline, CD target timer, or MB89371 baud-generator
edge. NCR cycles map to nanoseconds with ceiling conversion for deadlines and
floor conversion for the sampled cycle count. Simultaneous deadlines use the
fixed update order NCR, CD target, then DUART; bus notifications flush after
that batch. The integer conversion uses checked quotient/remainder arithmetic
for ARM32 compatibility. Device callbacks observe the timestamp of the batch
that caused them. This avoids host waits and per-cycle polling.

The shared map's 0x1f780000 write range is an upstream `nopw()` marked
`// watchdog?`; this adapter preserves that no-op mapping and does not infer a
watchdog or reset behavior from the address comment.

The board reset cancels pending DMA and resets the NCR, CD target, both UART
children, and EEPROM device lifecycle. EEPROM contents persist across reset;
its serial pins/protocol state follow the standalone 93C46 device reset rules.
The MB89371 wrapper's mode and baud registers persist, as in MAME, while its
8251 children reset; channel 0 CTS is driven low as the GV machine reset does.
The optional initial EEPROM image is exactly 128 bytes and can be exported or
imported for caller-owned persistence.

The common board does not claim game boot. A boot path also needs the matching
PSX BIOS, game firmware/media, and any game-specific peripheral hardware.
