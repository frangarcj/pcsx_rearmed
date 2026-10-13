# Konami GV representative on Mac

Hyper Athlete (`hyperath`) now reaches a P1 running event with the native Mac
interpreter and Vita-path OpenGL renderer. The capture shows the blue 1P runner,
three COM runners and a measured player speed of 6.08 m/sec. This is a bounded
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

The run consumed 110/110 input events, completed 553 DMA requests with no bytes
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

Authentic ARM64 DRC serial routing, CDDA mixing, physical Vita and special GV
maps remain open. See [native integration](konamigv-native.md) and
[the complete board matrix](arcade-mac-validation.md).
