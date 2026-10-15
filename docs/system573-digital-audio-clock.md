# System 573 native digital audio clock

The native digital profile now connects PSX execution to the source
FPGA/MAS3507D audio model and the complete SPU/CDDA/digital speaker route.
Evidence below uses generated MIPS code and synthetic MPEG tones. The original
Percussion Freaks KAA CD is still missing; original installation, music and
instrument gameplay remain unverified. See [the board report](system573-mac.md).

## Clock and filter contract

`system573_digital_clock.c` advances from absolute core cycles at 33,868,800 Hz.
A common rational timebase of 169,344,000 units per second represents five
units per CPU cycle and integer periods for all nine MPEG rates: 8, 11.025,
12, 16, 22.05, 24, 32, 44.1 and 48 kHz. This is host scheduling arithmetic,
not an additional hardware oscillator. Conversion to device nanoseconds uses
quotient/remainder arithmetic to avoid overflow during ordinary long sessions.

Each input tick advances the actual source device, renders one decoder sample
and observes its reported rate. A new rate controls the following interval.
The output epoch stays continuous at 44.1 kHz across rate changes and device
resets. Source frame counters still depend on actual MPEG sync notifications;
silent DAC ticks never manufacture decoded frames or counter progress.

The host bridge uses linear interpolation between actual source samples.
It waits for the next input sample rather than rendering beyond the CPU time.
Exact coincident ticks use the current sample directly, avoiding an additional
floating-point rounding step. This host filter is not claimed bit-identical
to MAME's resampler. At startup the source FIFO requests bytes before the
first decoded frame: the 44.1 kHz fixture has one initial silent sample, then
matches direct MAS output for the same delivered bytes exactly.

The mixer retains digital floats until the final gain-1 sum with integer SPU
and CDDA samples, then saturates/truncates once to signed 16-bit PCM. This
preserves cancellation when the integer subtotal exceeds the PCM range.
Legacy profiles retain their previous integer path. Output waits for every
enabled source clock; fractional-rate interpolation and SPU even-pair rounding
can leave a small pending tail. Flush never fabricates future samples.
Clock rewind, unsupported rate, nonfinite PCM, callback reentry/rejection,
missing timestamps and overwritten history fail explicitly.

The explicitly selected generated digital fixture has an empty source CR589
drive. Its CD DAC clock emits silence while decoded CDDA/sector counters stay
zero. The normal original-media loader requires the original KAA CD and never
uses this fixture backend. Live fixture playback additionally requires the
explicit `--board-test` option. Raw `--audio` captures SPU; `--mixed-audio`
captures the complete speaker route. The live SDL master volume applies once.

## Mac evidence, 2026-10-15

Optimized and ASan/UBSan component suites pass source register/cipher/MAS tests,
continuous clock/reset epochs and three-source float cancellation/saturation
checks. Ten synthetic streams cover all nine MPEG rates plus a real concatenated
22.05 kHz mono to 44.1 kHz stereo header/channel change. Advancing two seconds
in one call or in 997-cycle increments produces identical float PCM. Optimized
and sanitized PCM hashes also match for all ten streams. Independent
zero-crossing measurements at the 44.1 kHz output preserve 1000 Hz within
0.001 Hz; stereo right channels preserve 1500 Hz within 0.001 Hz, including
after the midstream change. Depending on input phase, 88,196–88,200 output
frames are ready at the two-second endpoint; the remaining tail awaits actual
input ticks.

Generated MIPS code encrypts the fixture independently, writes it through
native digital MMIO and enables the source FPGA. Native tests consume 20/20
drum/select/service events, retain the expected output lamps, pass complete
NV3 cold/warm persistence and report zero device/clock/mixer faults.

| Synthetic native run, 120 video frames | Decoded MPEG frames | Speaker frames at 44.1 kHz | Peak | Cold/warm WAV SHA256 |
| --- | ---: | ---: | ---: | --- |
| 44.1 kHz stereo, NEON / GL / live GL SDL | 39 | 88456 | 7807 | `7f2e7b2c8474e0886979fc793980530306224418fa4d24e4d7315b936ce81977` |
| 22.05 kHz mono, NEON | 40 | 88455 | 7824 | `fedaa66b2ff689be93258d6c82aaedea9d860de7bb1496c36fbf1b5f1cda2855` |
| 48 kHz mono, NEON | 42 | 88455 | 7781 | `e76e26534983e87fe2a68794001fa1c62d993d942f89db5b09582f95a1588ba6` |

The first row is byte-identical across both renderers, headless/live output
and cold/warm state. Live SDL reports zero output errors. These programs do
not render a game scene or access game sectors: logs retain `SYNTHETIC_FIXTURE`
and `EMPTY_FIXTURE_DRIVE`, with zero nonzero SPU and zero decoded CDDA frames.
The source end-word/guard-padding effect remains documented in the
[FPGA component report](system573-digital-io.md).

The original DDR 2nd Mix NEON regression separately runs 9600 video frames,
consumes 92/92 events, produces 7,076,500 speaker frames and reports zero faults.
Both WAVs retain their prior exact hashes: SPU
`029104707ba062f881645f7d7710fa6281e06bab7606c333f64f0d13d968b84b`,
speaker `cce2f79b5838ea2820903e69923bc11375adcf7b5e3bcc1a91ae2f86a5d888aa`.
The 15 System 573 Python media/profile tests also pass.

Evidence is saved under ignored
`artifacts/boards-mac-20261015/system573/pcnfrk4mk/audio-clock`.

## Reproduction

Generate explicit synthetic tones, without Xing or ID3 headers:

```sh
ffmpeg -f lavfi -i 'aevalsrc=0.25*sin(2*PI*1000*t)|0.25*sin(2*PI*1500*t):s=44100:d=1' \
  -codec:a libmp3lame -b:a 128k -write_xing 0 -id3v2_version 0 /tmp/mas-stereo-44100.mp3
ffmpeg -f lavfi -i 'aevalsrc=0.25*sin(2*PI*1000*t):s=22050:d=1' \
  -codec:a libmp3lame -b:a 48k -write_xing 0 -id3v2_version 0 /tmp/mas-mono-22050.mp3
make -f tests/Makefile.system573 test sanitize
build/system573-tests/digital /tmp/mas-stereo-44100.mp3 /tmp/mas-mono-22050.mp3
build/system573-tests/sanitize/digital /tmp/mas-stereo-44100.mp3 /tmp/mas-mono-22050.mp3
make -f Makefile.mac-harness GPU=neon system573-test
make -f Makefile.mac-harness GPU=vitagl-like system573-test
python3 tests/check_system573_digital_harness.py \
  --harness build/mac-harness/pcsx-harness --mpeg /tmp/mas-stereo-44100.mp3
python3 tests/check_system573_digital_harness.py \
  --harness build/mac-harness-vitagl-like/pcsx-harness \
  --mpeg /tmp/mas-stereo-44100.mp3 --live
```

Other-rate mono fixtures use the same sine command at the desired sample rate.
Concatenating the 22.05 kHz mono and 44.1 kHz stereo MPEG files supplies the
midstream rate/channel change. Generated files remain outside version control.
