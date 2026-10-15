# Konami ZS01 cassette protocol

`libpcsxcore/arcade/konami_zs01.cpp` exposes the pinned MAME ZS01 pin protocol
through a standalone C API. This is MAME's high-level model of the security
cassette PIC, not execution of its firmware. It is a component for the System
573 digital configuration; it is not yet connected to a native game profile.

The constructor requires both a complete 140-byte ZS01 image and an eight-byte
DS2401 serial image. The ZS01 layout is reset response 4, command key 8, data
key 8, configuration registers 8 and data 112. Both images are copied; no
default passwords, serial repair, guessed keys or successful authentication
fallback are supplied. The serial image uses MAME region order. Serial reads
inside ZS01 packets return image[7] through image[0]. The external one-wire
line will use the separate existing DS2401 device when cassette ZI is wired.

CS is active low. Rising RST while selected starts the supplied reset response,
MSB first on falling SCL. Commands and responses use twelve encrypted bytes,
MSB first with an additional ACK clock per byte. Read requests install the
host-supplied response key. Bit 2 selects the data-key decrypt stage. CRC,
page reads/writes, configuration access, data-key replacement and erase follow
the source protocol. Failed CRC increments the stored retry counter and erases
data/data key when the configured threshold is reached. A successful request
resets that counter. Reset clears protocol state but preserves the mutable
image, which can be exported and used to recreate a chip.

The port deliberately preserves source quirks: rising-edge SDA changes,
unknown serial address FC using the same DS2401 bytes as FD, release of SDA
on the last reset-response bit, and retention of already-encrypted response
buffer bytes when the source does not overwrite them. Response byte 1 supplies
the next data-decrypt chaining byte; it cannot be assumed to remain zero after
a read. Unknown addresses do not become invented readable sectors. Host NACK
repeats the current response byte; deselecting CS freezes command processing.

`konami_zs01_generate.py` checks SHA256 of `zs01.cpp/.h` from MAME
`deb2f54fe7300e7a192690318beb860159bfd52a`. It extracts the actual reset,
crypto and pin methods, removes framework logging, redirects direct DS2401
reads to the supplied immutable serial bytes, and trims trailing whitespace.
There are no command/state-machine corrections or authentication shortcuts.
The source is BSD-3-Clause, copyright smf; see `konami_security_LICENSE.txt`.

```sh
python3 libpcsxcore/arcade/konami_zs01_generate.py ../mamedev --check
make -f tests/Makefile.konami-security test
make -f tests/Makefile.konami-security BUILD=build/konami-security-asan \
  CXXFLAGS='-std=c++11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' test
```

Synthetic host traces use an independent byte/rotation cipher and CRC codec,
driving only the public pins. They cover all fourteen data pages, serial and
configuration reads, encrypted data operations, first-write response key,
replacement keys, boundary/unknown addresses, EEPROM recreation, explicit
erase, wrong command key, CRC failures and retry erasure, CS interruption,
NACK, repeated clocks, partial commands and reset. X76/DS2401 regressions pass
in the same target. Both optimized and ASan/UBSan builds pass.

The original `pcnfrk4mk` ZS01 image and source-identical parent serial are also
verified by the media auditor and exercised by the same pin readback test:

```sh
build/konami-security-tests/test-konami-zs01 \
  roms/audited/pcnfrk4mk/cassette.bin roms/audited/pcnfrk4mk/serial.bin
```

This original-image check reads the supplied reset response, every page,
configuration and both source serial addresses. It is component evidence,
not proof that the game has accepted the cassette or entered gameplay.
