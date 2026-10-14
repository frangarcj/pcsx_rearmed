# NBA Jam Extreme / Acclaim ZN-1 work in progress

NBA Jam Extreme 1.10I (`nbajamex`) is the next representative for Acclaim's
ROM/SRAM/RAX configuration. All original media are prepared. The complete
ADSP2181 core and [RAX board component](rax-sound-port.md) pass component vectors
and authentic firmware/command/audio checks. Native PSX board runtime,
player validation and final SPU/RAX mixing are **not yet established**.

The user's TeknoParrot collection supplies the package: 27,627,999 bytes,
outer SHA1 `97177c97d1b240840cb978efca77b7443b16ded4`, with exact
`coh1000a.zip` and `nbajamex.zip` entries. All 22 original chip files pass pinned
MAME length, CRC and SHA1 checks. Source is
`deb2f54fe7300e7a192690318beb860159bfd52a`, `sony/zn.cpp`,
`nbajamex_state`, `ROM_START(nbajamex)` and `shared/rax.cpp`.

```sh
python3 scripts/prepare_nbajam_zn_board.py --set nbajamex \
  --mame-source ../mamedev \
  --rompath /tmp/pcsx-boards-mac-20261014/nbajamex-tp \
  --output-dir roms/prepared/nbajamex
```

Preparation preserves original BIOS/keys, 32 MiB interleaved banked ROM and
complete 8 MiB RAX region, including source zero padding. The exact unused
uPD78081 NO_DUMP declaration is omitted because source configures behavioral
`znmcu_device`; no replacement firmware is produced. Source has no EEPROM/SRAM
dump for this set. The eventual native board must use source erased AT28C16
and 32768-byte 71256 SRAM defaulting to FF, with mutable runtime storage kept
separate. Preparation does not mark execution verified.

## Remaining native work

- COH1000A common ZN motherboard, 4 MiB RAM and 2 MiB type-2 VRAM.
- Two source 2 MiB ROM bank selectors, the 8 MiB address-map-bank page switch,
  byte SRAM window and its asymmetric read/write mirror behavior. Source raw
  bank latches are preserved across soft reset even when the live map resets.
- Integrate the verified RAX component with the PSX event scheduler and frontend
  output. Its original DSP, bootstrap, RAM banks, overlay, latches/PF0, four BDMA
  word types, completion/reset/IRQ and SPORT0 stereo DAC paths are implemented
  and checked separately; that does not establish whole-system timing fidelity.
- Main sound status/read constants and full 16-bit command writes exactly as
  source, with actual DSP execution rather than invented acknowledgements.
- Four-player source inputs, four coin GPIO counters, live keyboard/gamepads,
  source-gain SPU/RAX mixing, cold/warm NVRAM and real player evidence.

The [ADSP2181 port](adsp2181-port.md) and RAX are prerequisites for the native
NBA profile. Independent MAME confirms bootstrap reply A000 and SPORT controls;
127 captured host writes replay with nonzero original-firmware sound and zero
faults under O2 and ASan/UBSan. Native PSX gameplay, frontend resampling and
whole-system timing/audio fidelity remain pending. Judge Dredd has separate
HDD/gun/VBlank wiring and remains its own representative.
