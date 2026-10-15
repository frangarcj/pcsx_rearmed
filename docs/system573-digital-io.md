# System 573 digital I/O and FPGA component

`system573_digital.cpp` now connects the source digital I/O registers, owned
32 MiB zeroed volatile RAM, original DS2401 board serial, FPGA cipher and
MAS3507D decoder. It is a standalone native Mac component with float PCM output.
The PSX game profile/MMIO binding, native sample-clock bridge and speaker mix
are not yet integrated. No original digital game execution is claimed.

`system573_digital_generate.py` checks SHA256 of MAME `k573dio.cpp/.h` and
`k573fpga.cpp/.h` from `deb2f54fe7300e7a192690318beb860159bfd52a`. It extracts
the source reset/register/cipher/counter methods and derives dispatch from all
39 source address-map entries. Framework ownership/logging are removed; device
and time callbacks are adapted. Diagnostic word/byte counters and a rolling
FNV64 byte fingerprint do not change emulated state. Two bounds adaptations
are explicit: an actual out-of-RAM FPGA read faults before supplying any byte,
and long-session float counters use a defined 32-bit wrap instead of undefined
float-to-integer overflow. BSD-3-Clause attribution to smf/windyfairy is in
`system573_digital_LICENSE.txt`.

The API accepts source halfword register offsets, reports unmapped operations
separately, and requires a complete caller-supplied digital-board ID. It does
not substitute the game's cassette serial or regenerate its CRC. RAM read and
write pointers are independent, apply the source 25-bit mask and increment by
two. Source reset preserves RAM but clears device registers, output latches,
keys and decoder state; time remains monotonic. Original ID readback through
register EE, with reset/presence/64-bit timings, passes independently of the
cassette ZI tests.

The source common and Solo Bass Mix cipher variants retain pair swaps, XOR
permutations, key rotations and eight-bit key-3 wrap. Key/range writes reset
the stream cursor, decode state and counters as in the source. MP3 enable,
streaming enable and frame-counter enable remain separate. FPGA frame counters
only increment on actual decoder sync notifications when enabled; the common
playback-time counter waits for the first real frame sync. Counter low reads
latch high bits. The source difference-counter quirks are retained: common
successive reads at the same timestamp return zero after the first, whereas
Solo recomputes absolute elapsed time and can return the same difference again.

MAS demand schedules the source zero-delay FPGA byte timer. Events are drained
after decoder callbacks return, retaining repeated demand=1 notifications
without recursive mutation. At most the source FIFO capacity plus an end check
is needed per drain; a runaway event chain faults. FPGA RAM errors, time
rewinds and callback mutation/destruction attempts fail explicitly. The
component preserves source status constants (including the assumed fixed
FPGA firmware), source output permutation, and empty network stubs. It does
not execute an uploaded FPGA bitstream or implement cabinet networking.

## End-of-range evidence

The original `update_stream` increments the byte-addressed cursor by two when
loading a word, then checks the updated cursor again before sending its second
byte. At the end address, the final word's second byte remains pending. Tests
assert this behaviour; the adapter does not silently complete or pad the read.

The synthetic MPEG fixtures are encrypted into RAM with an independent inverse
cipher and an explicit guard word. With this fixture layout, the source emits
the original input plus zero guard padding, withholding the guard word's last
byte. The full emitted-byte fingerprint matches this expected plaintext. PCM
is byte-identical to the MAS3507D adapter given the exact same delivered bytes,
for both FPGA variants. No decoder result is fabricated for a wrong cipher key.

The guard padding changes minimp3's final-frame acceptance compared with the
unpadded standalone test. This is retained and recorded, not normalized:

| Synthetic fixture | Standalone unpadded frames | FPGA/delivered-byte oracle frames | PCM differences versus unpadded baseline |
| --- | ---: | ---: | --- |
| 44.1 kHz stereo, 1000/1500 Hz | 40 | 39 | 1249 samples, stereo-frame indices 44928–45600 |
| 22.05 kHz mono, 1000 Hz | 41 | 40 | 972 samples, stereo-frame indices 23040–23566 |

The decoded tones still measure 999.999970/1500.000095 Hz and 1000.000072 Hz.
This source end/range behaviour requires actual game-media validation; synthetic
decoder acceptance alone cannot establish complete music or song playback.

## Checks and remaining integration

```sh
python3 libpcsxcore/arcade/system573_digital_generate.py ../mamedev --check
make -f tests/Makefile.system573 test sanitize
build/system573-tests/digital --serial roms/audited/pcnfrk4mk/digital_id.bin \
  /tmp/mas-stereo-44100.mp3 /tmp/mas-mono-22050.mp3
```

Tests cover register defaults/holes, full-RAM address wrap, reset persistence,
output permutations, original serial timing, source network stubs, enable
gating, final pending bytes, invalid ranges, bounds faults, monotonic/counter
behaviour and callback reentry. Encrypted MPEG runs cover both cipher variants,
multiple key wraps, exact delivered-byte/PCM oracles, deterministic key/range
restart, muted playback, disabled frame counting and wrong-key silence. Both
optimized and ASan/UBSan builds pass. GL/NEON native builds and their existing
System 573 fixtures continue passing; those fixtures are not digital gameplay.

The decoder runs at its reported source rate, with caller-supplied monotonic
nanosecond time. CPU-cycle scheduling, rate conversion into the native speaker
clock, SPU/CDDA/digital mixing, instrument input/output wiring, original KAA CD
acquisition, installation and real player execution remain open. Evidence and
explicitly synthetic audio files live under ignored
`artifacts/boards-mac-20261015/system573/pcnfrk4mk`.
