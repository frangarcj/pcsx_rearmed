# Representative Taito G-Net games on Mac

Validated on the M1 Pro with the native Mac interpreter and the Vita-path GL
compatibility renderer. These four media packages were selected to cover the
main card variants and both CPU board geometries, rather than testing regional
clones. This is bounded runtime evidence, not complete G-Net compatibility.

| Game / MAME set | Coverage | Observed Mac result |
| --- | --- | --- |
| Psyvariar Medium Unit Japan / `psyvarij` | COH3002T, Type 1 card, portrait shooter | NEON player gameplay; GL reaches player gameplay and continue screen, 2400 frames |
| Go By RC / `gobyrc` | COH3002T, Type 2 card, wheel and trigger | Calibration completed with NEON; identical NEON/GL car selection at 5000 frames; GL reaches a player race at 8300 frames |
| Mawasunda!! / `mawasunda` | COH1002T, CompactFlash, handle/trackball and cabinet outputs | NEON reaches presentation; GL replay reaches a running minigame with credits and a countdown, 7500 frames |
| G-Darius G / `gdariusg` | COH3002T, generic ATA card, modified BIOS `mb2011` | Installation, JP1-off data load and player Zone A gameplay with GL, 3600 frames on the warmed flash state |

All final GL runs have zero native board `unsupported` and `fault` counters,
nonzero captured Zoom PCM and successful VitaGL shutdown. The game renderer
outputs are reviewed manually; an exit code or nonzero audio alone is not a
passing graphics test. A black Go By RC capture at 3600 frames was a transition:
the 5000-frame NEON and GL captures show the same textured car selection, and
the complete board reports and audio counters match. Replays use frame-based
inputs, not emulation speed changes. A subsequent 8300-frame GL replay consumes
all 42 inputs and shows player 1 in Beginner / Round 1, lap 1/3, with the race
timer running. The original raw captures of Psyvariar are sideways. The subsequent
[interactive Mac validation](gnet-mac-interactive.md) applies its presentation
rotation and adds live SDL controls and mixed audio. Physical Vita, ARM64 DRC game execution, full-game fidelity and
launcher/menu integration are not validated by these runs.

## Startup fixes found with authentic media

- Flash reads now extract byte lanes and split dwords into x16 transactions.
  The Sony BIOS reads its flash header as bytes. Wider writes to byte-wide
  controls operate on the connected low byte, and the source constant/no-op
  register accepts the byte transactions used by startup firmware.
- Profiles with source JP1-on inputs start in the EPROM bank. `jp1` is also
  available as an explicit replayed input. For the G-Darius conversion,
  installation through the modified BIOS is followed by a JP1-off boot and a
  separate data load; the first install-complete screen is not gameplay.
- Unknown-purpose ZSG-2 control latches follow MAME's store/log behavior;
  they no longer stop authentic sound firmware. Unsupported partial accesses
  and other device diagnostics remain fatal. Sound CPU instruction overruns
  are retained when reset is asserted; a real caller time rewind is still
  rejected separately.
- The MB3773 reset request is deferred to the harness's safe CPU boundary.
  Reset asserts the sound reset line and restores the source initial bank;
  the emulated flash contents survive.
- G-Net channel-4 writes use the pinned MAME completion path: payload delivery
  after 24000 ticks at 33.8688 MHz, followed by DMA completion, with MADR
  retained. The previous console path modified MADR and used a different
  payload/completion schedule, leaving Go By RC/Psyvariar waiting in BIOS
  `TestEvent`. Read DMA is immediate; the native implementation safely wraps
  transfers at the RAM boundary. The console path remains selected whenever
  G-Net is inactive. This does not establish complete DMA timing equivalence.

The four verified CHDs are kept under `roms/<set>/`; their prepared directories
under `artifacts/gnet-mac-20261013/<set>/` contain audited regions, manifests and
links to that durable media. The packages came from the user-linked
[Internet Archive collection](https://archive.org/download/tp-roms_1/TeknoParrot%20Collection/).
Wrapper size/SHA1, chip hashes and CHD verification were checked; the loader
also checks the CHD's declared MAME SHA1 and card metadata. No source firmware,
key or CHD validation was bypassed.

## Flash NVRAM and controls

`flash.nvram` stores the five mutable flash images separately from the original
hash-checked ROM regions. Its header binds the state to the set, BIOS root,
BIOS option and CHD identity. A payload SHA1 and exact length catch corruption;
saves use a temporary file and rename. Missing state starts from the original
firmware and erased program/sample devices, as in MAME. Invalid, truncated and
wrong-profile states fail before CPU execution and are not overwritten.
This is runtime-created NVRAM, not an alternate firmware/media package or a
savestate of the complete board. It does not include EEPROM or the CHD overlay.
Use the existing `--nvram FILE` option for the 2080-byte AT28C16 state, including
Go By RC's calibration.

The native source-profile controls added here are `p1_wheel` / `p1_trigger`
(0..255, neutral 128) for Go By RC, and `p1_handle` / `p2_handle` (1..255,
neutral 128) for Mawasunda. Handle sampling uses the pinned source's cumulative
trackball equation, including player 2's -193/256 factor; weight and lamp writes
are latched in their source bank-0 registers. The [interactive Mac frontend](gnet-mac-interactive.md) also routes keyboard,
mouse and SDL gamepad inputs. Physical cabinet force feedback is not provided.

The Zoom PCM sink converts the source normalized stereo stream to bounded
PCM16 at the device's integer 25000000/768 rate (32552 Hz), without altering its
emulated clock. Float callback delivery remains available. This first pass captures Zoom separately. The subsequent
[Mac interactive frontend](gnet-mac-interactive.md) mixes SPU and Zoom for SDL;
libretro mixing remains pending.

## MAME comparisons and regression checks

Independent MAME 0.287 runs use private config/NVRAM/snapshot paths. Psyvariar
and Mawasunda reach presentation there; G-Darius JP1-on ends at the source
install-complete/power-off message. The five flash images from native
Psyvariar, Mawasunda and G-Darius installation match MAME byte for byte after
converting MAME's x16 NVRAM from big-endian storage to the native CPU little-endian
image format. This checks the installed data, not complete emulated state or
frame-exact gameplay.

Passed Zoom and G-Net component tests, including ASan/UBSan; source profile
registry tests; native MIPS/storage/PCM fixtures with NEON and GL; the deferred
watchdog fixture; and source-derived SPU DMA vectors. The latter check that
payload is absent before the 35431-cycle COH3002T boundary, delivered at that
boundary, MADR retained, DMA IRQ signaled, and read data wraps safely at 4 MiB.
The original time-rewind rejection regression and the reset-after-overrun
regression both pass.
The native console PS-X EXE smoke also passes after the shared DMA changes.

## Reproduction and evidence

```sh
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl GPU=vitagl-like DRC=0 -j8
build/mac-harness-gnet-gl/pcsx-harness \
  --system-gnet artifacts/gnet-mac-20261013/psyvarij \
  --frames 2400 --max-blocks 400000000 --capture-gl \
  --inputs artifacts/gnet-mac-20261013/inputs/psyvarij.csv \
  --audio artifacts/gnet-mac-20261013/psyvarij-repeat.wav \
  --output artifacts/gnet-mac-20261013/psyvarij-repeat.ppm
```

This reproduction uses the warmed state created by the first installation.
Keep a copy of `flash.nvram` and EEPROM when comparing runs, and give parallel
runs separate prepared directories. Use `GPU=neon` without `--capture-gl` for
the software renderer. The local preparer is `scripts/prepare_systemgnet.py`;
it accepts local media only and is a pre-existing untracked workspace file.

Evidence is under `artifacts/gnet-mac-20261013`: raw PPM/PNG captures, WAVs,
input CSVs, source manifests, `validation.json`, `flash-comparison.json`, state
rejection checks and independent MAME captures. `board.ini` and the generic
adapter report retain their conservative `execution_verified=false` /
`auth_boot=false` metadata; runtime observations are recorded separately here.
Generated media, NVRAM, binaries and captures are not committed.
