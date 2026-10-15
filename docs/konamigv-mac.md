# Konami GV representative on Mac

Hyper Athlete (`hyperath`) now reaches a P1 running event with the native Mac
interpreter with both Vita-path OpenGL and NEON renderers. The latest capture
shows the blue 1P runner, three COM runners and a player speed of 6.00 m/sec.
The source SPU/CDDA speaker route now passes original-media checks. This is a bounded
common-map GV validation, not coverage of the gun, trackball, flash or printer
variants.

## Prepared authentic media

The pinned MAME source is `deb2f54fe7300e7a192690318beb860159bfd52a`.
`prepare_konamigv_board.py` validates the BIOS, raw EEPROM and complete CHD.
The CHD SHA1 is `579442444025b18da658cd6455c51459fbc3de0e`; BIOS SHA1 is
`02a82a2fe1fba0404517c3602324bfa64e23e478`, raw EEPROM SHA1 is
`a0f203a999757fba68b391c525ac4b9684a57ba9`. The normalized default EEPROM
matches an independent MAME 0.287 run byte for byte. Runtime NVRAM is a separate
128-byte file, never a replacement for the checked raw dump.

## Result and repeat command

Evidence is in `artifacts/boards-mac-20261014`: `hyperath-fixed-player-gl.png`,
`hyperath-fixed-player-gl.ppm`, `hyperath-fixed-spu.wav`, the ordered
`hyperath-play-inputs.csv`, log and `hyperath-validation.json`.

```sh
build/mac-harness-gnet-gl/pcsx-harness \
  --system-gv artifacts/boards-mac-20261014/hyperath \
  --frames 4000 --max-blocks 800000000 --capture-gl \
  --inputs artifacts/boards-mac-20261014/hyperath-play-inputs.csv \
  --nvram artifacts/boards-mac-20261014/hyperath/eeprom.nvram \
  --audio artifacts/boards-mac-20261014/hyperath-fixed-spu.wav \
  --output artifacts/boards-mac-20261014/hyperath-fixed-player-gl.ppm
```

The earlier run consumed 110/110 input events, completed 553 DMA requests with no bytes
remaining and reported zero adapter/device faults. SPU capture contains
2,945,800 stereo frames at 44.1 kHz with peak 9423. The elapsed emulated time is
66.86 seconds; source audio is nonzero, but CDDA samples are not mixed yet.

The player-start blocker was a 28-byte SCSI MODE SELECT data-out command.
The native adapter completed CHCR `01000201` without calling the write delegate.
Pinned `cpu/psx/dma.cpp` invokes that delegate from `dma_finished`, using the
block-mode BCR count. Delivering the payload lets the event load and begin.
The synthetic native regression uses real NCR/core DMA MMIO, sends a 12-byte
MODE SELECT block descriptor and requires a 512-byte target block size. The
previous early-return implementation fails that regression.

Authentic ARM64 DRC serial routing, physical Vita and special GV
maps remain open. See [native integration](konamigv-native.md) and
[the complete board matrix](arcade-mac-validation.md).

## Source-clock speaker mix, 2026-10-15

The native profile now attaches the real CHD backend's continuous 44.1 kHz
DAC clock and mixes it with timestamped SPU output. The pinned
`src/mame/konami/konamigv.cpp` routes SPU channel 1 to left and channel 0 to
right at gain 0.75; CDDA channels retain their order at gain 1. The shared
host buffer in `system573_audio.c` now accepts this source SPU route while
preserving System 573's defaults. Fractional SPU gain survives until the final
sum and 16-bit saturation/truncation; the live master volume applies once.
Unit tests cover channel order, cancellation across fractional sample values,
saturation, clock gaps/history and invalid configuration under O2/ASan/UBSan.

The first original-game mixed run exposed a pre-existing large-SPU-DMA clock
discontinuity during boot at frame 1077. The console renderer's speculative
size-based advance could move its sample clock beyond the CPU timestamp.
GV now uses the source PSX SPU DMA copy timestamp, as System 573 already does.
A native 512 KiB DMA regression passes with at most the existing even-pair
sample rounding. An independently linked previous DMA implementation fails
the same test, reporting PCM end 726528 at CPU cycle 24000 (702528 cycles in
the future). No fabricated PCM or clock discontinuity is accepted by the mixer.

Original Hyper Athlete completes the 4000-frame P1 running replay in both GL
and NEON, consumes 110/110 inputs and completes 553 CD DMA requests with zero
remaining bytes and zero device/mixer faults. Both finish at PC `8002e400`,
2264480028 cycles and 155036070 blocks. Their 2 MiB RAM still matches the
pre-change baseline exactly; the clock correction restores SPU sample time
rather than changing the game result.

| Headless GL and NEON evidence | Result |
| --- | --- |
| Speaker output | 2,948,541 stereo frames at 44.1 kHz, peak 19302, zero clipping |
| Raw SPU | 2,948,542 frames, peak 9423; one actual future rounded sample remains pending |
| Original CDDA | 577002 decoded frames, 484389 nonzero frames, peak 19660 |
| Complete mix versus SPU-only route | 484387 output frames differ, proving original CDDA reaches the speaker |
| RAM SHA256 | `0ab29ae417f6945cbf4f08b03e5178fce1c90e7d0c746f54d22fcc01b3df77a9` |
| Raw SPU WAV SHA256 | `07978e2b170f9032c32cc3616421efa21084b4b5c28e07327e7a22079c1eb4b2` |
| Speaker WAV SHA256 | `425a61163f9eb65a19d983860cab4fe6f6cd44ac813fd2c441cb3e9beb30779b` |

RAM, raw SPU WAV and speaker WAV are byte-identical across both renderers.
The old raw SPU frame count above remains historical evidence before the DMA
correction. `--audio` still captures unscaled SPU; `--mixed-audio` now works
headless as well as live and captures the source speaker route.

A separate 4000-frame live GL run starts from the preceding runtime EEPROM,
retains identical RAM/EEPROM, consumes 110/110 inputs and reaches the same P1
event. SDL outputs 2,948,541 frames, peak 19302 and zero errors; the original
CDDA counts match headless exactly. Its mixed WAV differs in 818 samples over
frames 1744056–1745434, entirely accounted for by the corresponding raw SPU
cue difference after the 0.75/channel-swap route. CDDA introduces no additional
difference. The live WAV is retained separately, SHA256
`51c5bed5e2fc25b392f779092daa0d72c2cb1bd43ed4287a56fcda7936da43c6`;
it is not normalized to the headless result.

```sh
build/mac-harness-vitagl-like/pcsx-harness \
  --system-gv artifacts/boards-mac-20261014/hyperath \
  --frames 4000 --max-blocks 800000000 --capture-gl \
  --inputs artifacts/boards-mac-20261014/hyperath-play-inputs.csv \
  --nvram /tmp/hyperath-audio.nv --audio /tmp/hyperath-spu.wav \
  --mixed-audio /tmp/hyperath-speaker.wav --output /tmp/hyperath-player.ppm
```

Use `build/mac-harness/pcsx-harness` without `--capture-gl` for NEON; add
`--interactive` for live SDL. Use separate runtime EEPROM paths for independent
runs. Evidence is retained under ignored
`artifacts/boards-mac-20261015/konamigv/hyperath-audio`.
