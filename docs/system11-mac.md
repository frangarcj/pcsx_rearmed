# System 11 native Mac validation

Validated on Apple M1 Pro, macOS ARM64, 2026-10-13, using the native PCSX
ReArcade harness with the interpreter. These are short gameplay checks with
scripted coin, start, character selection, movement and attack inputs.

| Set | Board | NEON | Vita-path GL on Mac |
| --- | --- | --- | --- |
| `tekken` | COH100, GPU type 1, C406 | Stage 1: Paul vs Kazuya | Stage 1: Paul vs Kazuya (GPU type 1 implemented) |
| `tekken2` | COH110, GPU type 2, C406 | Stage 1: Yoshimitsu vs Jack-2 | Same fight rendered successfully |
| `souledge` | COH110, GPU type 2, C409 | Stage 1: Siegfried vs Seung Mina | Same fight rendered successfully |

Each set generated nonzero stereo C352 audio at 88.2 kHz. All final runs
reported zero illegal C76 opcodes, unsupported peripheral accesses and board
faults. Tekken and Tekken 2 ran for 1800 frames; Soul Edge for 2100. The runs
include boot, selection and the beginning of combat, not a complete game.
Tekken also passed 1800 frames with GL after GPU type-1 support was added.
The inspected capture shows Paul vs Kazuya in stage 1. Its successful log is
`tekken-gl-fight.log`; the original rejected run is kept as
`tekken-gl-rejected.log`. Tekken 2 and Soul Edge GL captures were byte-identical
to their pre-change captures.
Audio generation is verified; auditory fidelity against MAME is not measured.
DRC, interactive harness controls, physical Vita and exact source clock timing
are not validated by these runs. The shared GL source also compiled and linked
into `pcsx-rearcade.velf`; the VPK has not been refreshed by this GPU task.

## Fixes found with real firmware

The C76 BIOS writes watchdog reload (`0x60`) and one-shot start (`0x42`)
during boot. Previously both writes raised a fatal unsupported-peripheral
diagnostic. The adapter now follows pinned MAME: watchdog reload has no side
effects and one-shot start is latched. This does not implement one-shot timer
execution, which is also absent from pinned MAME. Unknown peripherals still
diagnose faults. Fatal C76 diagnostics now include kind, PC and address/detail.

The standalone C352 test build also excludes System 12-specific profiling
counters unless `PCSX_SYSTEM12` is compiled, avoiding a linker dependency on
the System 12 board.

## GL GPU type 1

The native renderer now accepts CXD8538Q commands. GP0 E1 and polygon TPAGE
fields are converted to the renderer's type-2 positions; E3/E4 drawing-area Y
and signed E5 offset Y move from bit 12 to their native-parser positions.
The conversion operates on a packet copy. GPULIB retains the hardware fields
for GP1 register reads and type-1 GPUSTAT, including E6 mask bits.

Interleaved 4/8-bit pages use the address equations from pinned MAME
`src/devices/video/psx.cpp`. A per-vertex page flag selects the shader path;
physical page/CLUT coordinates and VRAM dependency tracking retain their normal
meaning. Interleaved pages bypass the cache designed for linear indexed pages.
16-bit textures remain linear, matching MAME/NEON. Console/type-2 pages keep
their existing cache path. VRAM uploads, copies and reads retain their guest ABI.

The shared GPU test now reads back GL-rendered pixels before checking them.
Coverage includes 512/1024-row VRAM, 4/8/16-bit pixels, odd indexed pages,
all four texture Y banks, polygon TPAGE changes, raw guest registers, signed
offsets and clipping. The same vectors pass under NEON.

## Local media and reproduction

The packages came from the S–Z volume (`tp__roms_2`) of the Internet Archive
TeknoParrot collection linked by the user. Download SHA1s were checked against
item metadata. Inner `tekken.zip`, `tekken2.zip`, `souledge.zip` and
`namcoc76.zip` are in `roms/`. MAME 0.287 audited all three sets successfully.
Preparation additionally checks every ROM size/CRC/SHA1 against the pinned
MAME source `deb2f54fe7300e7a192690318beb860159bfd52a`.

```sh
python3 scripts/prepare_system11_board.py --set tekken \
  --mame-source ../mamedev --rompath roms \
  --output-dir artifacts/system11-mac-20261013/tekken
make -f Makefile.mac-harness BUILD=build/mac-harness-system11 GPU=neon DRC=0 -j8
build/mac-harness-system11/pcsx-harness \
  --system11 artifacts/system11-mac-20261013/tekken \
  --frames 1800 --max-blocks 300000000 \
  --inputs artifacts/system11-mac-20261013/fight-four-coins.csv \
  --output artifacts/system11-mac-20261013/tekken-final.ppm \
  --audio artifacts/system11-mac-20261013/tekken-final.wav
```

For Tekken 2 and Soul Edge, prepare their respective set names and use their
directories. Soul Edge requires three credits to start with the default
configuration; the replay inserts four. For GL use
`BUILD=build/mac-harness-system11-gl GPU=vitagl-like DRC=0` and add
`--capture-gl` to the harness invocation. Use 2100 frames for Soul Edge.

Evidence is under `artifacts/system11-mac-20261013`: inspected PNG/PPM frames,
final WAV files, replay CSVs, logs, media manifests and `validation.json`.
Media and generated artifacts are not committed.

## Regression checks

Passed M37710 instruction/peripheral suite (including firmware startup register
writes), nine KEYCUS/ROM-bank/C76/C352 board vectors, synthetic native System
11 MIPS/C76 harness fixtures, 12 Python profile/input tests, and C352/MAME
stream timing plus System 12 audio-clock tests. After the GL implementation,
512/1024-row GPU vectors pass in both renderers, and synthetic System 11 and
System 12 harness suites pass under GL. Real Tekken 2/Soul Edge GL captures
remain byte-identical to the pre-change captures.

## Vita3K follow-up

The isolated Soul Edge black-screen investigation and native NEON/GL runtime
evidence are recorded in [souledge-vita3k.md](souledge-vita3k.md). The Vulkan
fence fix belongs to the private Vita3K host; the game VPK is unchanged.
