# Konami cassette security peripherals

`libpcsxcore/arcade/konami_security.cpp` exposes the actual MAME-derived
X76F041, X76F100 and DS2401 pin protocols through a standalone C interface.
It provides reusable devices for System 573 cassette variants and other
boards; no complete System 573 boot is claimed.

The separate [ZS01 component](konami-zs01.md) now provides the encrypted PIC
protocol required by cassette ZI. Original-media board/runtime evidence is
tracked in [System 573 Mac validation](system573-mac.md).

## Secure SerialFlash

`konami_secure_flash_create` requires the chip type and a complete caller
image. Wrong lengths and missing images are rejected. Images are copied to
mutable RAM; no passwords or cassette contents are invented.

| Chip | Image bytes | Layout |
| --- | ---: | --- |
| X76F100 | 132 | Reset response 4, write password 8, read password 8, data 112 |
| X76F041 | 548 | Reset response 4, write password 8, read password 8, configuration password 8, configuration registers 8, data 512 |

Drive CS, RST, SCL and SDA through the separate pin functions. CS is active
low. Rising RST while selected enters reset-response mode, which emits the
four supplied response bytes LSB first on falling SCL. Serial commands,
passwords and data use MSB-first byte clocks plus acknowledgement cycles.
SDA transitions while SCL is high detect START/STOP; repeated START and
host ACK/NACK behavior follow the upstream state machines.
`konami_secure_flash_read_sda` reports the device output, matching MAME;
deselected chips return high. It is not an additional wired-AND calculation
with the host's SDA input.

The X76F100 implements password-controlled sector reads, complete 8-byte
page writes, read/write password replacement and failed-password retry
policy. Eight failed attempts erase passwords/data in RAM, matching MAME's
chip behavior. Verification-byte retries do not repeatedly increment the
counter. Invalid sectors 14 and 15 are bounded and follow MAME's zero-read/
ignored-write behavior.

The X76F041 implements its command/address states, configuration-password
access, block control flags, 128-byte array wrap, retry-counter lockout,
configuration-register reads/writes, normal page writes, arbitrary-length
configuration writes, double-copy password programming and reset/mass
program/erase commands. Password requirements and deliberate bypasses are
controlled by the supplied configuration, rather than forced authentication
success. Incorrect passwords reject verification when authentication is
required. Documented upstream configuration reset/mass operations can omit
passwords; this is inherited device behavior.

Reset clears pin/protocol state but preserves updated image bytes.
`konami_secure_flash_get_image` exports the exact current image for
persistence. It can include changed passwords, retry registers or erased
data; callers decide when and where to save it. Debug status exposes protocol
position and password-match/retry state without exposing password bytes.

Two bounded fixes are applied to the upstream X76 state machines: successful
authentication resets the data-byte cursor even if the host omits repeated
START, preventing a write-buffer overrun; program-only permission checks
`(new & old) == old`, ensuring bit clears are rejected even when the new
byte is numerically greater. Normal cassette sequences remain compatible.

## DS2401 one-wire serial ID

`konami_ds2401_create` requires exactly eight supplied ROM bytes. The image
uses MAME's byte order: wire reads emit image[7] through image[0], LSB first.
Bytes, family code and CRC are returned unchanged. The optional
`konami_ds2401_validate_image` checks Dallas CRC-8 and reports a mismatch;
creation does not repair or substitute the caller's ROM.

Drive/read the pin with absolute emulated microseconds, or advance pending
events with `konami_ds2401_advance_to_us`. Time must be monotonic; rewinds
return an error without changing state. The port follows MAME's timings:
480 microseconds low recognizes reset, release waits 30 microseconds before
the presence pulse, presence remains low for 120 microseconds, command bits
sample 30 microseconds after the falling edge, and read-bit output releases
after 30 microseconds. The line read is device output AND host line level.
ROM commands 0x33 and compatible 0x0f emit all 64 bits. Unsupported commands
return to idle and do not fabricate a response. Reset cancels timers and
restarts the explicit time origin at zero while preserving the supplied ROM.

## Validation and remaining limits

Run `make -f tests/Makefile.konami-security test`. Deterministic synthetic
fixtures drive real pin traces covering reset responses, START/STOP and
repeated-clock edges, failed/correct passwords, retry policy, host NACK,
page commit/persistence, sector and array boundaries, configuration access,
password replacement, program-only protection, retry lockout and mass erase.
DS2401 tests cover exact reset/presence/read windows, both ROM commands,
CRC validation, all 64 bits, unsupported commands and rejected time rewinds.
The complete suite passes ASan/UBSan. These original X76/DS2401 component tests
use synthetic images; later original-cassette board evidence is recorded in
`system573-mac.md`, separately from component protocol checks.

The port retains MAME's X76 limitations: password verification/write latency
is immediate, SDA output updates on the modeled rising edge despite upstream
physical-edge TODOs, and invalid-command validation is incomplete. Hardware
write endurance, power-loss behavior, exact analog timing and complete board
cassette wiring are not modeled. DS2401 implements the two upstream ROM-read
commands, not multidrop search-ROM enumeration. Full in-flight protocol
serialization is not provided; persistent chip images can be exported.

The pin state machines derive from MAME
`src/devices/machine/x76f041.cpp/.h`, `x76f100.cpp/.h`, and `ds2401.cpp/.h`,
BSD-3-Clause, copyright smf. See `konami_security_LICENSE.txt`. To refresh the
X76 source, run:

```
python3 libpcsxcore/arcade/konami_security_generate.py /path/to/mame/src/devices/machine
```
