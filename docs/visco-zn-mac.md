# Video System / Visco ZN on Mac

Aero Fighters Special USA (`aerofgts`) passes bounded native interpreter/GL
player validation. The 3000-frame interactive run consumes 112/112 input events
and reaches 1P shooting gameplay with 11,900 points. Portrait presentation,
movement, firing, second-button pulses, coins and start are exercised. The
source SPU produces live stereo audio; the SDL recording matches every raw SPU
sample multiplied by the source gain 0.35, with signed integer truncation.
Board and audio diagnostics finish at zero faults/errors. A subsequent warm
NVRAM replay enters 1P gameplay again (score 200 at 1700 frames).

## Source and media

Reviewed source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`,
`src/mame/sony/zn.cpp`, `visco_zn_state`, `coh1002v`, the input profile
`aerofgts`, and its three game ROM declarations. Regional profiles `aerofgtst`
and `sncwgltd` are prepared by the same source-derived tooling; their authentic
runtime has not been tested. The BIOS root is rejected as a game profile.

The user's TeknoParrot collection does not contain this representative. The
public `mame-0.260-roms-split_202310` archive supplies `aerofgts.zip` and
`coh1002v.zip`; wrapper lengths/SHA1 are checked, followed by a separate source
length/CRC/SHA1 audit of all 14 chip files. The assembler preserves the five
512 KiB fixed ROMs and six 4 MiB banked ROMs, without substituting media.

```sh
python3 scripts/prepare_visco_zn_board.py --set aerofgts \
  --mame-source ../mamedev --rompath roms --output-dir roms/prepared/aerofgts
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 BOARD=1 HAVE_CHD=1 -j8
```

The package contains `main.bin`, `fixed.bin`, `banked.bin` and two original
CAT702 key files. BIOS is `coh-1002v.ic353` (source CRC `5ff165f3`); keys are
`kn01.ic652` / `kn02` (`1ce271df` / `0d2001b4`). The normal native loader
independently checks source CRCs, byte layouts and padding of all five regions.
Only the explicit `--visco-zn-fixture` path permits generated test media.

The exact unused `upd78081` NO_DUMP declaration is omitted because this source
configuration instantiates behavioral `znmcu_device`; no firmware substitute is
emitted. There is no EEPROM dump in these game declarations: the source AT28C16
starts erased and the game initializes it. `--nvram` stores the complete
2080-byte data/ID image separately from immutable ROM packages. This file remains
mutable across cold/warm runs; the local verification JSON records its hash
before and after the warm replay.

## Native map and sound

- COH1002V uses source ZN-1 CXD8530CQ input clock 67.7376 MHz, 4 MiB RAM,
  CXD8561Q/type-2 GPU with 2 MiB VRAM, and the common CAT702/MCU/SIO/AT28C16 path.
- Fixed ROM maps at `1f000000..1f27ffff` (2.5 MiB). Banked ROM maps at
  `1fb00000..1fbfffff` (1 MiB), with 24 populated entries. The low byte written
  to `1fb00000` selects the raw bank value. Upper bytes, Tecmo's `+6` selector
  and Raizing's ZNSECSEL do not select this board's ROM bank. Values >=24 fail
  explicitly instead of wrapping or reading outside the allocation.
- Physical, cached and uncached ROM aliases execute the selected bank. Both
  fixed and banked ROM bytes are read-only apart from the selector's side
  effect. Reset selects bank zero; unload clears all borrowed bank LUT pages.
- The source Visco coin handler retains the readable coin byte and counts
  rising edges of bits 0/4, without adding the common coin lockout lines.
  Only two buttons per player are connected; third/fourth buttons are rejected.
- The real PSX SPU runs at 33.8688 MHz / 768 = 44.1 kHz. Source DMA payload and
  completion timing use the shared ZN path. SDL applies source gain 0.35,
  without replacing or resampling the SPU. `--audio` captures raw SPU stereo;
  `--mixed-audio` captures the exact speaker PCM sent to SDL.
- The source 270° presentation reuses the existing GPU rotation path. It adds
  no CPU readback to the live rendering path.

## Player evidence and reproduction

Local evidence is under `artifacts/boards-mac-20261014`; scratch logs are under
`/tmp/pcsx-boards-mac-20261014`. The boot capture is a credits screen and the
1400-frame capture is the initial 1P transition. The 3000-frame capture shows
actual player shooting over the city, 1P score 11,900 and one remaining credit.

The interactive run records 2,211,406 stereo frames at 44.1 kHz in each of the
raw and SDL WAVs. Raw SPU peak is 28,301, speaker peak is 9,905, and all speaker
samples equal raw samples times 0.35 with truncation toward zero. It reports
zero audio errors, zero board faults and two P1 coin counter edges. Runtime
NVRAM is exactly 2080 bytes; warm loading re-enters gameplay without data errors.

```sh
build/mac-harness-gnet-gl/pcsx-harness \
  --visco-zn roms/prepared/aerofgts --interactive \
  --frames 3000 --max-blocks 300000000 --capture-gl \
  --inputs artifacts/boards-mac-20261014/aerofgts-player-inputs.csv \
  --audio artifacts/boards-mac-20261014/aerofgts-replay-spu.wav \
  --mixed-audio artifacts/boards-mac-20261014/aerofgts-replay-sdl.wav \
  --nvram artifacts/boards-mac-20261014/aerofgts-replay-nvram.bin \
  --output artifacts/boards-mac-20261014/aerofgts-replay-gl.ppm
```

For live play omit replay, frame and capture options. Keys: 5/6 coin, 1/2 start,
P1 arrows and A/S; P2 I/K/J/L and Z/X; 9 service, T test, Escape pause. Mapped
pads use D-pad, South/East, Start and Back/left-stick for coin. Focus loss
releases live controls; recorded input is isolated from live events.

## Regression coverage and remaining scope

```sh
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl \
  GPU=vitagl-like DRC=0 HAVE_CHD=1 visco-zn-test gnet-frontend-test \
  tecmo-tps-test raizing-zn-test capcom-zn-test taito-fx1z-test systemgnet-test
```

The generated native MIPS fixture executes fixed/banked helpers, covers ROM
endpoints and KSEG aliases, byte/word/dword selector lanes, high bytes and unused
selectors, read-only ROM, raw bank 23, rejection of bank 24, coin edges,
2080-byte EEPROM API round trips, disconnected buttons and source SPU DMA.
Normal loading rejects the synthetic media. These vectors pass with GL and
NEON, including without CHD. The SDL virtual-gamepad test covers Visco's exact
input names, coin/start/fire/service, focus release and replay isolation.
Existing TPS, Raizing, Capcom, FX1Z/G-Net and mixer regressions pass; the
no-board console smoke passes.

This establishes bounded USA player coverage on Mac. Full machine timing and
sound fidelity versus hardware/MAME, other regional gameplay, physical Vita and
authentic ARM64 DRC remain unverified. Atlus Heaven's Gate now has bounded
68000/YMZ280B player coverage in `atlus-zn-mac.md`. See [the complete Mac matrix](arcade-mac-validation.md).
