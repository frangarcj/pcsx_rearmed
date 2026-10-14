# Tokimeki GV sensor and printer controller

`tokimeki_io.c` follows MAME `konami/konamigv.cpp` at
`deb2f54fe7300e7a192690318beb860159bfd52a`. The protocol module has no game
firmware, media, GPU or fabricated measurement results. Native Mac integration
and original-game evidence are documented separately in `tokimeki-mac.md`.

The device-check word rotates on a rising output line, starting at F073 for
Seal Version and F0BA for Seal Plus. The GSR interface shifts a five-bit sensor
ID, skips the source dummy clock, then exposes bit 8 of the shifting ADC word.
ID 1A samples the supplied GSR port; 18/19 and unknown IDs return the source
zero response. Serial reads acknowledge the heartbeat latch. The heartbeat
notifier uses integer milliseconds `60000/(input+49)` for a nonzero simulated
input and polls every 100 ms when the hand is off. An explicit pulse input is
also available, following the source Direct configuration.

Printer bits use actual virtual time at 67737600/16 ticks per second. A pulse
of at least 9000 ticks starts a byte; 4000–5600 ticks represents one. Seven command
bits and eight trailer bits form a pair. Three identical pairs with trailer F9
are required; mismatches are rejected. Command 10 schedules video capture at
the next rising vblank, 11 prints and sets busy for exactly one virtual second,
and 17 clears the page and resets the image cursor. Unknown OSD commands remain
unmodeled, matching the reference's logged behavior.

The page is 800x600 RGB. The base/Seal layout centers the actual video frame on
white paper. Plus uses the source crop (1 left, 40 top, 19 right, 16 bottom),
fixed-point nearest-neighbor scaling and 4x4 positioning. Source memory-in
captures are provided by the renderer callback; the module does not synthesize
images. Reset clears source protocol/page state while retaining the scheduled
heartbeat clock and output line state. Callbacks must not destroy the controller.

`make -f tests/Makefile.tokimeki test sanitize` verifies independent expected
identity bits, repeated line writes, heartbeat deadlines, GSR serial bits,
rejected printer packets, a full-page capture, exact busy timeout, and all 16
Plus cells under O2 and ASan/UBSan. Synthetic captures are not game/printer
compatibility evidence. Original `tmoshs` CD/EEPROM, bounded player scenes and
the operator test print have separate native evidence in `tokimeki-mac.md`.
