# Twinkle source UART and JVC control

`arcade/twinkle_serial.cpp` hosts pinned MAME `diserial`, `ins8250`,
`fdc37c665gt` and `bus/rs232/xvd701` methods. The generator verifies the SHA256s
of all eight original source/header files at revision
`deb2f54fe7300e7a192690318beb860159bfd52a`. Source protocol/register bodies are
extracted, with framework allocation/logging adapted and explicit failure
before an invalid configuration-array index, an unconfigured serial TX shift
or a zero-divisor FIFO timeout. Retained BSD notices name the original authors.

The shared single-owner scheduler retains integer attosecond periods, RX/TX
half-bit timers, cancellation/reconfiguration during callbacks and stable
same-deadline ordering. This replaces MAME device/timer plumbing; it does not
replace bit transfers with immediate bytes or ready/ack values. The NS16550s
use the source integer 24 MHz / 13 clock, line framing, divisor latches,
register side effects, modem inversion, interrupt priority, loopback and FIFO
state. The source's short FIFO timeout formula and overrun behavior are kept.
The FDC37C665GT keeps source default logical enables/address selection,
configuration unlock/lock, alternate COM ports and clock changes. Its floppy
and parallel subdevices fail explicitly if accessed; unmapped source reads
still return zero. Configuration is preserved within the source decoder; full
machine-reset integration is tracked separately.

The selected `twinklei` wiring connects UART2 TX/RX pins to the JVC XV-D701.
The player starts with the source 9600/8O1 frame, asserted source modem pins,
JLIP ID 33, powered off and stopped. Original checksum parsing, power/name/
identity requests, chapter/play/pause state and 100 ms response-byte scheduling
are retained. The original source has no video decoder or attached movie
playback implementation; this is faithful control behavior, not decoded DVD
video. The real video CHD remains separately audited by the native profile.

```sh
make -f tests/Makefile.twinkle-serial test sanitize
python3 libpcsxcore/arcade/twinkle_serial_generate.py ../mamedev --check
```

Vectors run actual UART bit timers, FIFO TX/RX, loopback, overrun/IRQ priority,
source FIFO timeout, JVC checksum rejection, power on/already-on results,
name and chapter/pause commands, first-byte delay, configuration relocation/
lock and defined unsupported/invalid access faults. O2 and ASan/UBSan pass.

`tests/capture_twinkle_serial.lua` independently observes the original IIDX2
Super-I/O bus at all three PSX aliases. It never replaces data or writes device
state; a Start pulse at 65–70 seconds is the explicit operator action. A fresh
120-second MAME run records 125 transactions / 78 reads. Native replay matches
every read and eight final UART control/status/divisor values, executes three
JVC commands and sends all 33 response bytes, under O2 and ASan/UBSan.

For reproduction, create a fresh evidence directory and set
`PCSX_TWINKLE_SERIAL_TRACE_DIR` to its absolute path. Run the complete original
`bmiidx2` set in MAME with `-seconds_to_run 120 -nothrottle -video none -sound
none`, the capture script via `-autoboot_script`, fresh NVRAM/config locations
and an explicit snapshot directory. Replay with:

```sh
build/twinkle-serial-tests/twinkle-serial --source-trace /path/to/transactions.csv
build/twinkle-serial-tests/twinkle-serial-sanitize --source-trace /path/to/transactions.csv
```

The trace's `final.csv` belongs beside the transaction file. Ignored evidence
is under `artifacts/boards-mac-20261015/twinkle/bmiidx2/serial/source`. The
[native Mac report](twinkle-mac.md) separately verifies original title, backup
initialization, no-input warm restart and visible tutorial on GL and NEON.
Speaker mixing and player-scored song validation remain open.
