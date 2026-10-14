# Gallop Racer 2 Link hardware on Mac

Gallop Racer 2 Link HW (`glpracr2l`, Japan) passes bounded local player racing
in the native Mac interpreter with GL and NEON. The 4500-frame interactive
replay consumes 138/138 input events, selects Sunny Brian and reaches the race:
elapsed time 36.9 seconds, position 12/12, changed stamina and an observed steer
off the course. A separate warm-NVRAM run re-enters the race at time 5.2 seconds.
Source SPU audio and the exact SDL stream are nonzero, with zero board/Z80 faults.

Cabinet-to-cabinet link is **not implemented or validated**. The pinned MAME
source itself leaves this Z80's port map empty and exposes no shared memory,
interrupt or main-CPU transport. Native execution preserves that map rather than
fabricating a partner. This establishes local gameplay with the source-defined
extra CPU; it does not close the unknown physical link protocol. Vita, authentic
DRC and whole-machine timing/sound fidelity remain outside this result.

## Source and media

Source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`, `sony/zn.cpp`,
`glpracr2l_state::coh1002ml`, inherited `tecmo_zn_state`, `glpracr2` inputs and
`ROM_START(glpracr2l)`. The base board retains COH1002M, 4 MiB main RAM,
2 MiB type-2 VRAM, CAT702, behavioral MCU, SIO, AT28C16 and ordinary PSX SPU.

The user-linked TeknoParrot collection contains the Export common-map game,
but no Link HW archive. The split Link archive comes from
`mame-0.260-roms-split_202310`: 670,567 bytes, SHA1
`8104448e5c8d8b661a6e645c4a00a722c3fe0f83`. Parent `glpracr2.zip` and
`coh1002m.zip` already came from the user's collection. Preparation audits all
14 original chip files by source length, CRC and SHA1, including the complete
256 KiB `link3118.bin` (CRC `a4d4761e`). Native loading independently checks
CRC/layout/padding; a mutation at offset `20000` in the link ROM is rejected
even though it lies outside the Z80's mapped first 32 KiB.

```sh
python3 scripts/prepare_tecmo_tps_board.py --set glpracr2l \
  --mame-source ../mamedev --rompath roms --output-dir roms/prepared/glpracr2l
```

The two original Japanese program chips retain their byte interleave and the
40 MiB banked region retains source zero padding. Source has no EEPROM dump;
runtime NVRAM is a separate 2080-byte data/ID image. The exact unused uPD78081
NO_DUMP declaration is omitted because source uses `znmcu_device`; no firmware
substitute is produced. Common sets do not instantiate the link CPU, and Link
HW cannot silently load without its separate original ROM.

## Extra CPU and source limits

The real Z80 runs at the source's 4 MHz; source marks that clock uncertain.
ROM maps at `0000..7fff`, RAM at `8000..ffff`; ROM writes are ignored and
soft reset preserves RAM and elapsed scheduler time. Eight-bit port decoding
matches `map.global_mask(0xff)`. Unmapped reads return **00**, the source
address-map default (`unmap_value_low`), while writes have no device effect.
No IRQ or NMI is connected. The native report exposes access counts and states
`map=SOURCE_UNMAPPED transport=UNIMPLEMENTED`.

Independent MAME 0.287 reaches the same records screen and reports Z80
`PC=0229`, `SP=ff00`, port `80=00`; native's final player run reports the same
PC/SP after 300,871,598 nominal Z80 cycles. It makes ten port reads and sixteen
writes, with zero unsupported operations, IRQs, NMIs or cycle debt. This is
bounded firmware-path evidence, not a claim of exact cycle alignment with MAME.
Treating unmapped ports as FF instead produced millions of reads at port 80;
the independent reference caught that error before validation was recorded.

The main map remains the source raw-byte TPS bank selector at `1fb00006`,
8 MiB window and five populated entries. Tecmo's four rising-edge coin GPIO
inputs now have diagnostics with no lockout lines; fixture writes verify
repeated highs do not count twice. `glpracr2` controls still disable coin 2
and third buttons. Keyboard/gamepad handling, focus release and replay isolation
use the existing TPS frontend.

## Player evidence and reproduction

Evidence is in `artifacts/boards-mac-20261014`; logs/reference scripts are in
`/tmp/pcsx-boards-mac-20261014`. Use the `glpracr2l-fixed-*` files for the
validated unmapped-low run. Earlier captures made before that correction are
intermediate evidence only. GL and NEON show the same player race; the warm
3300-frame replay consumes the 36 events due by that frame and also has zero faults.

```sh
build/mac-harness-gnet-gl/pcsx-harness \
  --tecmo-tps roms/prepared/glpracr2l --interactive \
  --frames 4500 --max-blocks 800000000 --capture-gl \
  --inputs artifacts/boards-mac-20261014/glpracr2l-player-inputs.csv \
  --nvram artifacts/boards-mac-20261014/glpracr2l-fixed-nvram.bin \
  --audio artifacts/boards-mac-20261014/glpracr2l-fixed-spu.wav \
  --mixed-audio artifacts/boards-mac-20261014/glpracr2l-fixed-sdl.wav \
  --output artifacts/boards-mac-20261014/glpracr2l-fixed-gl.ppm
```

For live play omit replay/frame/capture flags. 5 coin, 1 Start, arrows and A/S
for P1, Escape pause. SPU/SDL each contain 3,317,110 stereo frames at 44.1 kHz;
every SDL sample matches the production SPU gain 0.35 route. Raw peak is 32,768,
SDL peak 11,468, with no audio error. Source SPU DMA retains its 24000-tick
payload boundary and original MADR/read-wrap behavior.

## Verification

```sh
make -f tests/Makefile.tecmo-link test sanitize
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 HAVE_CHD=1 tecmo-tps-test cbaj-tps-test \
  atlus-zn-test visco-zn-test raizing-zn-test capcom-zn-test gnet-frontend-test
```

Standalone vectors execute Z80 ROM/RAM endpoints, ROM protection, masked
unmapped-low I/O, no interrupts, HALT, single-cycle/chunk equivalence and reset.
They pass both O2 and ASan/UBSan.
Native MIPS fixtures execute the original TPS bank/alias/SPU vectors with and
without the extra Z80, require its ROM, observe its RAM writes and expose the
unimplemented transport. Both GL and no-CHD NEON pass. Source chip CRC rejection,
four coin counter edges and existing daughterboard/frontend regressions pass.

Remaining hardware families stay tracked in
[the complete Mac matrix](arcade-mac-validation.md); the next distinct board is
Time Warner/Atari Primal Rage II with HDD, DMA5, 8 MiB RAM and watchdog.
