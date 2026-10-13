# System 12 special cabinets on Mac

Validation host: Apple M1 Pro, macOS ARM64, 2026-10-13. Native PCSX
ReArcade harness with ARM64 DRC, the Vita-path GL renderer and source System
12 timing. MAME 0.287 supplied the comparison runs; device behavior and ROM
profiles are pinned to MAME `deb2f54fe7300e7a192690318beb860159bfd52a`.

Run a downloaded set from this checkout:

```sh
./scripts/run_system12_special_mac.sh ptblank2
./scripts/run_system12_special_mac.sh technodr
./scripts/run_system12_special_mac.sh kartduel
./scripts/run_system12_special_mac.sh truckk
./scripts/run_system12_special_mac.sh ujlnow
```

The launcher builds `build/mac-harness-special/pcsx-harness` and runs the
local `roms/<set>.zip`. CDXA also needs `roms/truckk/tkk2-a.chd` or
`roms/ujlnow/ul1-a.chd`; the runtime verifies the disc header SHA1 against
the pinned game declaration. ROMs and game media are not committed.

| Set | Cabinet support | Observed native result |
| --- | --- | --- |
| `ptblank2` | Gun counters, DMA-wait workaround, recoil/lamp registers | Stage selection and shooting missions |
| `ghlpanic` | Gun counters and outputs | In-game shooting/tutorial |
| `golgo13` | Gun-to-ADC conversion, trigger, sensor and Enter | Mission scenes with input |
| `g13knd` | Same Golgo hardware | Mission selection/story and input |
| `technodr` | JVS EMIO and printer endpoints, eight analog inputs | Driving trial, 80 km/h |
| `kartduel` | Default calibration EEPROM, pedals/wheel, link CPU map | Active race, lap 1/6 |
| `aplarail` | JVS coin/input endpoint and handle ADC | Active train run, 71 km/h |
| `truckk` | CDXA SH7014, ATAPI, DMA, SCI audio and pedals/wheel | Active driving, 35 km/h, then results |
| `ujlnow` | CDXA and guitar button mapping | Active song and scored notes (24 points) |

These are short gameplay checks, not full-game completion or multiplayer
validation. Kart Duel network play is not implemented (also unemulated in
the pinned MAME driver). The Technodrive printer endpoint answers its protocol
but does not print physical receipts. Truck's auxiliary cabinet I/O CPU has
no connected outputs in the pinned MAME map; physical cabinet mechanisms are
not validated. CDXA is currently enabled in the Mac harness with `HAVE_CHD=1`;
other builds explicitly reject CDXA when the adapter is not compiled. CDXA
runs on the main thread because its IRQ10 callback updates the main CPU queue.
The validated audio clock is 37.8 kHz; the programmable PLL and per-channel
volume chip are not fully modeled.

## Controls

`5` inserts a coin; `1` starts; Escape opens the pause menu. `T` and `9`
operate test and service. Default buttons are `A`, `S`, `D`, Space, with
`Q`, `E`, and `F` providing extra inputs.

* Guns: move the mouse in the 4:3 game area and left-click to fire. Point
  Blank and Ghoul Panic use their own raw counter ranges; Golgo uses a
  0..511 reversed vertical coordinate, converted through the H8 ADC. In
  Golgo, `D` is Enter and holding `F` or the right mouse button activates the sensor.
  Mouse clicks update aim immediately, including on Retina displays; clicks
  in the black borders do not fire. Losing focus releases the trigger.
* Techno Drive: arrows steer/accelerate/brake; `D` selects.
* Kart Duel: Up operates the accelerator, Down the brake; Left/Right steer.
  Release and press the accelerator to leave the title. `A` is Enter.
* Attack PlaRail: Up/Down move the controller; `1` is OK/Horn, `A`/`S`
  select left/right.
* Truck: arrows operate wheel/pedals, `1` is Horn/Start, `A` is Enter,
  and Space changes the view.
* Lammy: `1` is Start; `A`/`S`/`D`/Space are Circle/Cross/Triangle/Square;
  `Q`/`E` are Chin/Joe.

On Vita, the **front touchscreen** aims and fires player 1: touch the game
image to pull the trigger and lift the finger to release it. The touch panel's
reported physical bounds are converted into the displayed game viewport,
including its black borders and the renderer's aspect setting. Physical
buttons still work with touch, including a remapped trigger. Touch is ignored
in input replays and paused menus; resuming waits for the previous touch to
be released. This is included in both vitaGL and NEON executables in
`pcsx-rearcade.vpk`. Physical touchscreen validation remains pending.

Analog CSV inputs accept `adc0`..`adc7` (0..1023), `jvsanalog0`..`jvsanalog7`
(0..65535) and raw `p1gunx/y`, `p2gunx/y`. Digital controls remain 0/1.

## Verification

Local captures, WAVs, input CSVs and commands are retained in
`artifacts/system12-special-20261013`; the complete development and MAME
comparison logs are in `/tmp/pcsx-system12-special-20261013`.

The checks cover real ROM launches, synthetic board mappings and invalid
profiles, JVS framing/escaped bytes/115200-baud UART bits/coin edges/the real
Techno EMIO capability response and printer chain addressing, SH-2 reset and
branch delays/interrupt priority and pending requests, and real CHD fixtures
with Mode 2 Form 1/Form 2 field selection and ATAPI completion/seek/error/state
behavior. H8 CPU and SCI tests passed. CD/ATAPI fixtures also passed ASan and
UBSan. The Python suite passed 112 tests.

Mr. Driller, Libero Grande and Paca Paca Passion were rerun with their earlier
input scripts. Their WAV files are byte-for-byte identical to the recordings
before these changes. A native Lammy state saved at frame 3600 was restored and run to frame 5001;
the final capture and CDXA CPU/device diagnostics match the uninterrupted run.
Saved states use System 12 state version 3 to include
the new device/input state; older version-2 states are rejected.

Pointer validation (2026-10-13): the Mac SDL handler passed click-without-motion,
keyboard/mouse trigger merging, sensor, offscreen clicks and focus-loss tests
using a private hidden SDL window. The shared mapping passed viewport-edge,
letterboxing, Retina/widescreen and normalized-coordinate roundtrip checks.
The actual libretro core loaded Point Blank 2 and confirmed gun MMIO and
JAMMA trigger transitions for touch/release with both default and remapped
pad controls. The Vita package built successfully and contains both executables.

```sh
make -f Makefile.mac-harness BUILD=build/mac-harness-special \
  GPU=vitagl-like DRC=1 SYSTEM12_TIMING=1 gun-pointer-test
# With the native libretro core already built:
xcrun clang -arch arm64 -O2 -std=gnu99 -Ideps/libretro-common/include \
  tests/test_system12_pointer_libretro.c -o build/mac-libretro-gun-pointer/test-pointer
build/mac-libretro-gun-pointer/test-pointer \
  build/mac-libretro-gun-pointer/src/pcsx_rearmed_libretro.dylib roms/ptblank2.zip
# Append `mapped` to check the custom button layout.
```
