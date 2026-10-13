# G-Net interactive Mac validation

The native Mac GL harness now accepts G-Net in `--interactive` mode. The
four representative media profiles from [the first Mac validation](gnet-mac.md)
retain the same audited BIOS, keys and CHDs. This follow-up covers presentation,
SDL controls and live audio, with the interpreter. It does not validate Vita or
libretro's separate input/audio frontend.

## Presentation and input

The selected MAME profile supplies clockwise rotation (Psyvariar: ROT270).
Mac rotates the completed scanout, including its HD overlay and picture borders,
with a GPU texture copy and presentation pass. PSX VRAM, display registers and
internal render targets keep their original geometry. No CPU pixel readback was
added. Menu backdrops are captured after rotation; the pause menu stays upright.
Raw software-renderer/VRAM captures remain unrotated.

| Control | Keyboard | Mouse | SDL gamepad |
| --- | --- | --- | --- |
| Coin / start | 5 / 1; P2 6 / 2 | — | Back or L3 / Start |
| Standard G-Net directions/buttons | Arrows, A/S/D; P2 I/K/J/L, Z/X/C | — | D-pad, south/east/west |
| Go By RC wheel | Left/right; released = 128 | Horizontal window position | Left stick X, with deadzone |
| Go By RC trigger | A = 255, S = 0; released = 128 | Left = 255, right = 0 | Right minus left trigger |
| Mawasunda handle | Left/right; P2 J/L | Relative horizontal movement, one video frame | Left stick X on each player's pad |

Go By RC's low trigger endpoint confirms menu choices and applies reverse/brake;
the high endpoint accelerates. Mawasunda preserves its source minimum of 1.
Focus loss and pause release live controls. A CSV replay disables live controls
and keeps its own held state across focus changes and pause. Device connection
and removal are supported; physical force feedback is not implemented.

Test and volume in the pause menu target the selected board. `--gnet-jp1 on|off`
sets the actual BIOS-flash strap before CPU execution. G-Darius G requires an
installed flash state and `--gnet-jp1 off` for gameplay; the source's JP1-on
installation screen remains available.

## Live audio

The SDL stream receives stereo PCM16 at 44100 Hz. The mixer linearly resamples
SPU and Zoom independently using their exact source clock/768 rates, and follows
the pinned `taitogn.cpp` routes: SPU at 0.3, Zoom at unity. It saturates the sum,
keeps bounded queues, and rejects nonfinite input/producer overflow. Resampling
and playback do not change board clocks. Chip resets and main resets are separate;
a main reset clears both resampler epochs. Pausing clears SDL's queued audio.

The 50 MHz board initially lost silent SPU samples: a whole video interval
exceeds dfsound's 914-sample work buffer. The interactive frontend now services
SPU generation at bounded intervals while the CPU runs, in addition to the
existing MMIO/IRQ paths. Both sources then remain on the same timeline. This
change is confined to native Mac interactive audio; it is not a timing-equivalence
claim for other frontends.

`--mixed-audio FILE.wav` captures the exact mixed samples submitted to SDL.
The existing `--audio FILE.wav` still captures the independent Zoom stream.

## Checks and evidence

Native SDL replays reach Psyvariar's player/continue screen (2400 frames),
Go By RC's player race with changed position (8300), and Mawasunda's running
minigame (7500), and G-Darius G's player Zone A gameplay (3600, JP1 off).
Both sound sources produce nonzero samples. The complete runs
have zero board faults, mixer overflows, clipping and SDL submission errors.
Psyvariar's 2000-frame pause/resume replay also completes with all inputs consumed.

The production SDL event-handler tests cover keyboard/mouse merging, raw analog
endpoints, a virtual mapped gamepad, focus release and replay isolation. Existing
System 12 mouse-gun tests still pass. Mixer tests check exact output counts at
both board clocks, stereo sum/cancellation, clipping, reset history, overflow
and NaN rejection, including ASan/UBSan. G-Net device/storage/DMA fixtures and
the no-board console smoke remain passing.

Evidence is in `artifacts/gnet-mac-interactive-20261014`: input replay, original
and final source-gain WAVs, raw/rotated captures, logs and `validation.json`.
Only final `*-source-*` audio uses the source SPU gain of 0.3; earlier exploratory
mix captures used unity SPU gain. Generated media and captures are not committed.

```sh
make -f Makefile.mac-harness BUILD=build/mac-harness-gnet-gl GPU=vitagl-like DRC=0 -j8
build/mac-harness-gnet-gl/pcsx-harness \
  --system-gnet artifacts/gnet-mac-20261013/psyvarij --interactive
build/mac-harness-gnet-gl/pcsx-harness \
  --system-gnet artifacts/gnet-mac-20261013/gobyrc --interactive \
  --nvram artifacts/gnet-mac-20261013/gobyrc/eeprom.nvram
build/mac-harness-gnet-gl/pcsx-harness \
  --system-gnet artifacts/gnet-mac-20261013/gdariusg --gnet-jp1 off --interactive
```
