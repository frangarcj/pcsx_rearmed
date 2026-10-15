# MAS3507D MPEG audio component

`libpcsxcore/arcade/mas3507d.cpp` ports the pinned MAME high-level MAS3507D
model and its `mp3_audio` wrapper, including the identical minimp3 header.
It provides serial MPEG data input, I2C configuration, decoder demand and
MPEG frame-sync notifications, and normalized float stereo output. This is
a component for System 573 digital, not a working digital game audio path.
The [digital I/O/FPGA component](system573-digital-io.md) now feeds this decoder
from source encrypted RAM/timer methods. The [native CPU clock and speaker
bridge](system573-digital-audio-clock.md) now pass synthetic MIPS/MPEG and live
SDL checks. The DSP's undumped internal program is not executed.

`mas3507d_generate.py` verifies SHA256 of all five source files from MAME
`deb2f54fe7300e7a192690318beb860159bfd52a`; generated provenance records their
identities. Framework construction/save registration and logging are removed,
while decoder, FIFO, I2C commands, reset and sample math retain the source
bodies. A bounded BIT helper returns zero for an out-of-range shift. The
minimp3 implementation is copied without changes, with its upstream CC0
dedication. MAME-derived code is BSD-3-Clause, Olivier Galibert/windyfairy;
see `mas3507d_LICENSE.txt`.

The source's 3584-byte FIFO requests data through callbacks. A callback must
schedule input after the current device call; mutation or destruction during
callbacks is rejected and latched as a host-interface fault. Notifications
retain repeated demand=1 calls, which the FPGA uses to schedule another byte.
When callers overfill the FIFO, the source discards its oldest byte. Empty or
undecodable data requests more bytes and does not fabricate decoded frames
or successful sync notifications. The original decoder requires two matching
subsequent frame headers before accepting a stream.

Rendering retains source float PCM scale 1/32768, mono duplication, independent
LL/RR gains, mute and decoded-frame counters. The current MPEG sample rate is
reported in status; this API does not silently convert it to 44.1 kHz. Full
device reset restores 44.1 kHz/default gains, while playback reset retains
I2C/gains/mute and clears decoder reservoir, overlap, FIFO and frame state.
An audio bridge must still handle rate changes and source scheduling when the
component is integrated. Source I2C limitations, including its wrong-address
electrical ACK TODO and partial command/read support, are retained.

```sh
python3 libpcsxcore/arcade/mas3507d_generate.py ../mamedev --check
make -f tests/Makefile.system573 test sanitize
```

Default tests drive real I2C pins to write mute/gain registers and read the
frame counter. They check wrong-device writes, bounded FIFO overflow,
invalid-data silence/demand, reset distinctions and rejected callback reentry.
An additional test run supplies explicitly synthesized MPEG sine fixtures:

* 44.1 kHz stereo: 40 decoded frames/sync notifications; independently measured
  left 999.999970 Hz and right 1500.000095 Hz; peak 0.238251.
* 22.05 kHz mono: 41 decoded frames/sync notifications; measured 1000.000072 Hz
  on both duplicated channels; peak 0.238770.

Both fixture runs also verify bit-identical PCM after playback reset, continued
decode counters with mute enabled, left-channel zero gain with unchanged right
PCM, restoration to unity and exact source float multiplication for right
attenuation. Optimized and ASan/UBSan builds pass. These fixtures are not game
media. Baseline PCM hashes also match between optimized and sanitized builds.
Their generation command, raw float output and waveform checks live
under ignored `artifacts/boards-mac-20261015/system573/pcnfrk4mk`.

The test executable accepts one or more MPEG fixture filenames and writes
each baseline PCM to `<filename>.f32`:

```sh
build/system573-tests/mas /tmp/mas-stereo-44100.mp3 /tmp/mas-mono-22050.mp3
```

Original KAA cassette/CD acceptance, native installation, actual game music,
instrument gameplay and digital board validation remain separate requirements
in `system573-mac.md`. Synthetic live SDL playback is now verified separately.
