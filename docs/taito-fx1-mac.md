# Taito FX1S representative on Mac

Psychic Force (`psyforce`, Ver.2.4O) passes a bounded native Mac interpreter/GL
player run. The capture at frame 6500 explicitly shows `PLAYER1` and the `1P`
character marker; frame 7500 shows Burn fighting Genma after the replayed start,
buttons and directions. The board consumes all 146 input events, reports zero
watchdog requests and no device faults in the 125.36-second run.

This validates the COH1000T FX1S composition: 4 MiB RAM, 1 MiB type-2 GPU VRAM,
Sony serial/CAT702/ZN MCU, banked ROM, Z80/TC0140SYT/YM2610B, EEPROM and watchdog.
FX1Z has a different Zoom sound daughterboard and remains a separate port.

## Authentic startup and reference

The package is assembled from source-checked local `coh1000t`/`psyforce` ROMs by
`prepare_taito_fx1_board.py`, against MAME commit
`deb2f54fe7300e7a192690318beb860159bfd52a`. The reviewed undumped `upd78081`
region is omitted because this source configuration uses the behavioral ZN MCU;
no substitute ROM is emitted. See [the source/native review](taito-fx1-native.md).

Fresh EEPROM enters the service main menu in both PCSX and independent MAME
0.287. Exit must be selected after the menu is ready. The 2080-byte EEPROM saved
by the native service exit is byte-identical to one independently configured in
MAME: SHA1 `cb0fb6ef75d73e59c097b537ccd570ac1959ffd2`.

The passing replay inserts a coin at frame 5700, starts P1 at 5880, presses
button 1 at 6000, then exercises attacks and directions from 6600 onwards.
Independent MAME runs also reach PLAYER1 selection and the Burn/Genma fight.

```sh
build/mac-harness-gnet-gl/pcsx-harness \
  --taito-fx1 artifacts/boards-mac-20261014/psyforce \
  --frames 7500 --max-blocks 800000000 --capture-gl \
  --inputs artifacts/boards-mac-20261014/psyforce-mame-timing-inputs.csv \
  --nvram artifacts/boards-mac-20261014/psyforce/eeprom.nvram \
  --audio artifacts/boards-mac-20261014/psyforce-mame-timing-ym.wav \
  --output artifacts/boards-mac-20261014/psyforce-mame-timing-gl.ppm
```

Repeat at `--frames 6500` for the PLAYER1 selection capture. The 7500-frame WAV
contains 6,964,550 stereo samples at 55,555 Hz, peak 16,512. Its final five seconds
of player combat have peak 10,393, so the audio evidence includes the game rather
than only startup. This is a YM2610B capture; full SPU/YM live mixing and sound
fidelity are separate checks.

## Remaining early-coin issue

The earlier coin at frame 5000 reproducibly enters a VSync wait inside a DMA/IRQ
callback with interrupts disabled and subsequently expires the active watchdog.
A diagnostic trace records the final IRQ at frame 5030, EPC `800f85ac`, followed
by no interrupt return. A MAME run using the same EEPROM and a coin at 84 seconds
also resets. The later sequence passes in both implementations; the difference
is not an EEPROM checksum or a disabled watchdog. The exact timing/callback
cause remains open and this result does not claim every input timing is safe.

Evidence, logs, reference snapshots and checksums are in
`artifacts/boards-mac-20261014/psyforce-validation.json`. Authentic ARM64 DRC,
physical Vita and FX1Z are still unverified. See [the complete board matrix](arcade-mac-validation.md).
