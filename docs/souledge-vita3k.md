# Soul Edge on Vita3K (2026-10-13)

This investigation is restricted to the audited `souledge` System 11 COH110
profile. All runs use the MIPS interpreter; native ARMv7 DRC support for this
board is not implemented. Prepared media, inputs, reports and screenshots are
local generated artifacts, not committed ROMs.

## Reproduction and isolation

The latest standalone VitaGL validation package produced a black screen in
Vita3K on the M1 Pro. Both the default 725x544 render target and `--gl-1x`
reproduced it. The guest was not using `--capture-ppm`: all GL images came
from Vita3K's native screenshot facility. There is no added GL readback.

Vita3K's local `set_backend_renderer` forces Vulkan under `__APPLE__`, even
when the YAML says `backend-renderer: OpenGL`. The attempted OpenGL comparison
therefore also exercised Vulkan and is not a separate backend validation.
The user application, firmware tree, global configuration and source checkout
were not modified; each guest ran in a private copied application/filesystem.

The NEON package with `--cpu-present` completed the same 2100-frame, 106-event
replay, reached Siegfried versus Seung Mina, and captured a fight with health
loss. C76 illegal/unsupported/fault counters were zero, C352 output had 3,095,747
stereo frames with peak 5058, and audio errors were zero. This shows the ROM,
board startup and input sequence progress through the software-rendered path.

## Vulkan wait

A one-second native process sample shows the renderer blocked in
`VKTextureCache::prepare_staging_buffer -> vkWaitForFences -> MoltenVK`.
The guest main thread waits for vblank. A bounded texture-staging trace locates
the stall after PSX frame 240 and before frame 300:

```
index=1 bytes=16384 used=2097152 capacity=2097152
frame=1/3 scene=2/9 context_scene=9 last_waited=0
same_fence=false fence_status=NotReady
```

The optional private-host trace is enabled by `--trace-texture-staging` in
`tools/vita-harness/rebuild_vita3k_fixed.py`, logs at most 64 waits and records
its patch hash in the build manifest. It does not modify the guest VPK.

## Correction and GL validation

`--fix-texture-staging-fences` patches only the private Vita3K host. After
`new_frame` waits for completed submissions, it invalidates staging-buffer
references to those fences before resetting them. Uploads that are still in
flight retain their waits. This prevents a later upload from waiting on a
completed fence that has been returned to the unsignaled state.

With the exact same VitaGL VPK and replay, the fixed, instrumented host
completed 2100 frames, all 106 input events, the board report and VitaGL
shutdown. Native screenshots show Siegfried attacking Seung Mina and knocking
her down. The board report and the five audio statistics are identical to the
NEON run. This validates the bounded play sequence, not pixel-identical GPU
output or source-hardware timing.

The private instrumented host SHA-256 is
`5a19f35d8288ce2722d7d17e7f07d666c3f30afb946e0a977b0ba51f163829f5`.

A second host build without the texture-staging trace passed a longer
3600-frame run with the same VPK and replay. It consumed all 106 events,
rendered the fight and a knockout, produced 5,307,154 C352 stereo frames with
peak 6004 and zero audio errors, and completed VitaGL shutdown. C76
illegal/unsupported/fault counters stayed zero. All 127 forwarded host memory
faults had matching recoveries, with none unresolved. This host SHA-256 is
`18b9bf5b232747a238e7b6d7a5d4ad8d390a93aefacc06a57d5215b09b4af87f`.

Build the private host with all its previous required patches plus the fence
fix (local source/build prerequisites are in [vita-harness.md](vita-harness.md)):

```sh
python3 tools/vita-harness/rebuild_vita3k_fixed.py \
  --source ../vita3k --app-template ../vita3k/build/macos/bin/Vita3K.app \
  --output build/vita-arcade-validation/host-souledge \
  --dynarmic-archive build/vita-arcade-validation/dynarmic/libdynarmic.a \
  --fix-host-page-protection --trace-protection-recovery \
  --fix-stale-protection-faults --fix-unmapped-readback \
  --fix-texture-staging-fences
```

Add `--trace-texture-staging` for the bounded diagnostic host, using a fresh
output path. The build manifest records source and patch hashes. The original
`/Applications/Vita3K.app` remains unchanged, so it does not contain this fix.

Run only the prepared Soul Edge profile (choose a fresh output directory).
`VITA_FIRMWARE_FS` names the local tree containing `vs0`, `os0` and `sa0`;
`VITA_CONFIG` names the original YAML; `VITA_SHACC` names the existing
`ur0/data/libshacccg.suprx`. All three are read and copied by the runner.

```sh
python3 tools/vita-harness/run_system12_vita3k.py \
  --emulator build/vita-arcade-validation/host-souledge/emulator/Vita3K-fixed.app \
  --vpk artifacts/vita3k-arcade-20261013/validation-vitagl-final.vpk \
  --firmware-fs "$VITA_FIRMWARE_FS" --config-template "$VITA_CONFIG" \
  --shader-compiler "$VITA_SHACC" \
  --profile artifacts/system11-mac-20261013/souledge \
  --inputs artifacts/vita3k-arcade-20261013/souledge-staging-fix/inputs.csv \
  --frames 3600 --timeout 300 --screenshots 5,15,30,60,90,120,135,150,165,180,195 \
  --output-dir artifacts/souledge-vita3k-repeat
```

## Evidence

Under `artifacts/vita3k-arcade-20261013/`:

- `souledge-final`, `souledge-1x`, `souledge-opengl`: failed black-screen runs.
- `souledge-staging-trace`: failed instrumented run, last milestone 240;
  native stack sample is retained separately.
- `souledge-neon-cpu`: passed 2100 frames, board report, CPU PPM and native PNGs.
- `souledge-staging-fix`: passed 2100 frames with VitaGL, native PNGs and shutdown.
- `souledge-confirmation`: passed 3600 frames with VitaGL and no texture-staging
  trace; nine native PNGs, report, audio/input checks and shutdown passed.

VitaGL package SHA-256:
`937feba9a0635ecbef9434ac03c899939f9fa356bb0b82e697ea4ec3e6c233bb`.
NEON package SHA-256:
`732420a12677f6b08f67de97d7c2933313d8012f973a21f0fbc6a6659fcdd680`.
Replay SHA-256:
`7c1e696de81624fcce7456fdc25e6df883d73747575b8c4a522fe52870ab2d7f`.
Baseline host SHA-256:
`704bf3d3a019aa3b36737343c284ff6223a78cc6f40036c6eef35804d4990547`.

These are bounded emulator runs, not physical Vita validation, full-game
compatibility or a claim of MAME timing equivalence. The shelf/menu production
VPK has not been refreshed by this test.
