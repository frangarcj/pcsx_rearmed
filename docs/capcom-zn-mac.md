# Capcom ZN native Mac validation

Validated on Apple M1 Pro, macOS ARM64, 2026-10-13, with the native PCSX
ReArcade interpreter and Vita-path GL renderer. Scripted coin, start,
selection, movement and attack inputs reach actual player games.

| Set | Board | VRAM | Frames | Inspected result | QSound peak |
| --- | --- | --- | ---: | --- | ---: |
| `sfex` | COH1002C, ZN-1 | 2 MiB | 3600 | P1 Ryu selection, Ryu vs Hokuto, round ends in a loss | 6241 |
| `starglad` | COH1000C, ZN-1 | 1 MiB | 2400 | Hayato vs June, active combat and health loss | 15595 |
| `rvschool` | COH3002C, ZN-2 | 2 MiB | 3600 | Batsu vs Roy, P1 K.O. with 25,700 points | 7721 |

All final runs report `faults=0`, execute the real Z80 program and produce
nonzero stereo QSound PCM at 24,038 Hz. Captures are from the OpenGL back
buffer at 1920x1088. The WAVs contain 1,446,470, 964,313 and 1,443,633 stereo
frames respectively. These are short gameplay checks, not complete games.
Neither visual/audio equivalence to MAME nor auditory fidelity is established.
The mainboard SPU is not mixed into this capture. DRC, interactive harness
controls, other Capcom titles/daughterboards and physical Vita remain untested.

## ZN-2 clock correction

Generic arcade source timing selects main CPU input clock / 2: ZN-1 runs
its core cycle counter at 33.8688 MHz, ZN-2 at 50 MHz. The adapter previously
interpreted every cycle as 1/33.8688 MHz, advancing ZN-2 sound, serial and
EEPROM/MCU time about 47.6% too quickly. In the initial 600-frame Rival
Schools run, 501,149,516 CPU cycles advanced the peripherals by 14.796789
seconds instead of about 10.023 seconds.

The adapter now converts elapsed CPU cycles separately to the 8 MHz Z80
budget and fixed 33.8688 MHz SIO time, retaining fractional remainders.
SIO pin callbacks and MCU/EEPROM deadlines share the latter absolute time
domain. Pinned MAME `src/devices/cpu/psx/sio.cpp` explicitly uses 33.8688 MHz
for its bit timer even though the device constructor receives a derived clock.
Serial/MCU time and phase remain preserved across soft reset as before.

The final Rival Schools run advances 3,002,814,926 main CPU cycles to
480,450,388 Z80 cycles and 2,034,034,763 SIO cycles, about 60.056298 seconds.
The regression fixture independently checks these ratios for all three board
geometries. It fails on the previous binary's ZN-2 timing and passes with the
fix. ZN-1's time ratio remains unchanged.

## Media and reproduction

Downloads came from the user-linked Internet Archive TeknoParrot collection:
`sfex` and `starglad` from `tp__roms_2`, `rvschool` from `tp-roms_1`.
Outer package SHA1s match item metadata. Inner `sfex.zip`, `starglad.zip`,
`rvschool.zip`, `coh1000c.zip`, `coh3002c.zip` and `qsound.zip` are in `roms/`.
MAME 0.287 audits all three as OK/best available. Its only missing-ROM
diagnostic is the declared `NO_DUMP` `78081g503.ic655`; this unused device
region is omitted by the preparer's existing source-reviewed rule.
Every required ROM size, CRC and SHA1 is checked against pinned MAME
`deb2f54fe7300e7a192690318beb860159bfd52a`.

```sh
python3 scripts/prepare_capcom_zn_board.py --set sfex \
  --mame-source ../mamedev --rompath roms \
  --output-dir artifacts/capcom-zn-mac-20261013/sfex
make -f Makefile.mac-harness BUILD=build/mac-harness-system10 \
  GPU=vitagl-like DRC=0 -j8
build/mac-harness-system10/pcsx-harness \
  --capcom-zn artifacts/capcom-zn-mac-20261013/sfex \
  --frames 3600 --capture-gl \
  --inputs artifacts/capcom-zn-mac-20261013/sfex-play.csv \
  --output artifacts/capcom-zn-mac-20261013/sfex-long.ppm \
  --audio artifacts/capcom-zn-mac-20261013/sfex-long.wav
```

The existing `build/mac-harness-system10` directory contains the generic
multi-board harness; its name does not select a board. For the other games,
prepare their set names and select the corresponding directory. Use
`play.csv`, 2400 frames for Star Gladiator, 3600 for Rival Schools.
`--nvram FILE` imports/exports the 2080-byte AT28C16 image; the initial
2400-frame replays also exercised writing these files.

Local evidence is in `artifacts/capcom-zn-mac-20261013`: inspected PNG/PPM
frames, WAVs, replay CSVs, NVRAM images, audited region manifests, download
provenance, logs and `validation.json`. Media and generated artifacts are
ignored and not committed.

## Regression checks

Passed the synthetic native MIPS/Z80 guests for all three board geometries,
including banks, GPU readback, serial security, IRQ, frame-indexed inputs,
EEPROM persistence and QSound output. Also passed the Capcom daughterboard,
common ZN board and ZN MCU component suites, eight Python media/profile tests,
and generated profile consistency checks. The new timing assertion rejects
the old ZN-2 binary, preventing a successful boot from concealing this clock
error.
