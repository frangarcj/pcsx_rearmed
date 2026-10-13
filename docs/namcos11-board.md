# Namco System 11 board bus

`libpcsxcore/arcade/namcos11_board.{c,h}` connects the standalone M37702M2
interpreter and C352 to the System 11 maps from MAME `namcos11.cpp`. The nine
KEYCUS handlers are generated directly from `ns11prot.cpp`; source revision
is `deb2f54fe7300e7a192690318beb860159bfd52a`, BSD-3-Clause, copyright smf.
The C76 BIOS is caller supplied, never substituted with a handshake.

The module supports COH-100 and COH-110 metadata (GPU type 1 and 2 respectively,
both 4 MiB main RAM and 2 MiB VRAM), the eight independent 1 MiB ROM windows,
ROM8 and ROM8_64 selection equations, the latter's address-selected upper bank,
shared RAM, low-byte EEPROM lanes, all nine KEYCUS variants, and Point Blank 2
gun input/output registers. `libpcsxcore/system11.c` now connects the physical
bus, executable bank LUTs, profile-specific inputs, MAME's random generator,
board scheduling and C352 WAV output to the native Apple Silicon harness.
**There is no System 11 game boot claim yet.**

The C76 bus maps C352 at `0x002000..002fff`, input words at
`0x001000..001007`, 32 KiB shared RAM at `0x004000..00bfff`, and three aliases
of its external 512 KiB data ROM. Atomic aligned 16-bit C352 writes preserve
the execute-key register behavior; byte writes cannot accidentally trigger it.
The 16 KiB internal BIOS belongs to the M37702 core. Its ordinary instructions
execute; no game-specific shared RAM answers are installed. The documented
upstream `0x510000..51ffff = 0x80` Family Bowl workaround is retained.

`namcos11_run_cycles` accepts nominal CPU cycles at 8.4672 MHz. It schedules
the two 60 Hz IRQ sources from MAME, including their immediate t=0 events
after reset, and clears their HOLD_LINE state on actual
CPU acknowledge. C352 produces four-channel frames at 88.2 kHz, with the first
two channels connected on the real board. Audio callback scheduling uses
96-cycle slices and the CPU's retained instruction overshoot debt. This is
instruction-boundary scheduling, not a cycle-exact bus implementation.

Unknown KEYCUS reads use an explicit host `random16` callback, matching MAME's
random fallback. Without it, the board faults rather than supplying a made-up
successful protection response. Unsupported CPU peripheral accesses also
fault and are available through the C76 status API.

One upstream map inconsistency is guarded: the PSX shared aperture is declared
as 48 KiB, but the C76 share allocates only 32 KiB, and the 16-bit delegate
indexes that buffer directly. Access to the final 16 KiB faults instead of
reproducing a host buffer overrun. The [AT28C16 device](at28c16-port.md) implements
200 us programming busy polling and ignored busy writes. Its full 2080-byte
image includes ID storage; board reset preserves data and pending writes.
High-voltage pins remain disconnected on this board, as in MAME.

The native adapter selects 4 MiB PSX RAM, 4 MiB program ROM, 2 MiB VRAM and
the correct type-1/type-2 GPU before core initialization. All eight executable
windows follow bank writes through physical, KSEG0 and KSEG1 aliases. CPU
control addresses outside those aliases remain delegated to the PSX core.
The [36-set packager](system11-boards.md) records the MAME board profiles and
verifies actual C76 BIOS digests. Tekken, Tekken 2 and Soul Edge now have
[short native Mac gameplay validation](system11-mac.md). Exact main CPU clock
integration and broader game validation remain pending. `fambowl`, marked not
working upstream, is explicitly refused by the native loader.

Use a directory prepared from your own ROMs:

```sh
build/mac-harness/pcsx-harness --system11 artifacts/system11/tekken \
  --board-test --frames 600 --output artifacts/system11/tekken/frame.ppm \
  --audio artifacts/system11/tekken/sound.wav --nvram artifacts/system11/tekken/eeprom.bin
make -f Makefile.mac-harness system11-test
```

The native fixture executes real generated MIPS subroutines from all eight
ROM banks. Actual C76 instructions poll the input ports, convert extra Tekken
buttons through ADC channels, publish them through shared RAM and program
C352 to produce nonzero stereo PCM. Both COH-100/GPU1 and COH-110/GPU2 fixtures
run. These generated ROMs test integration; they are not game boot evidence.

Run the bus and CPU integration checks:

```sh
make -f tests/Makefile.namcos11-board test
```

The fixtures exercise fixed KEYCUS responses and decimal outputs, color
interpolation, both banking equations, gun overlays, endian and partial shared
accesses, EEPROM persistence, actual C76 instructions writing shared RAM,
C352 key activation and nonzero PCM, IRQ priority/acknowledge and RTI, and
audio/IRQ deadlines. The same suite runs under AddressSanitizer and UBSan.
