# System12 performance and clock diagnostics

`SYSTEM12_PROFILE=1` enables diagnostic scopes and emulated-cycle counters. It
does not change CPU multipliers, device clocks, instruction execution, or board
scheduling. The profiler supports the synchronous System12 core; the libretro
makefile rejects configurations with async GPU or SPU enabled.

For the Vita frontend:

```sh
make -f Makefile.vita VITA_GPU=neon ARCADE_SYSTEM12=1 SYSTEM12_PROFILE=1
```

The default VPK, core archive and standalone object directory gain a profile
suffix. Keep a separate source checkout/copy for performance experiments: core
objects in the generic makefile still share paths, and `Makefile.vita` forces a
complete core rebuild. Preserve validated VPKs and their evidence before building.

Run the guarded gameplay checker with that VPK and the same private Vita3K host,
firmware, profile and replay length used by the control. Do not run another guest,
native harness or compiler during either timed replay. The checker bounds guest
lifetime, preserves native screenshots and waits for shutdown.

## Measurements

The core emits cumulative `PERF` snapshots every 600 `retro_run()` calls. The
reported frequencies for the source-timing Soul Calibur profile are:

| Domain | Frequency / unit |
| --- | --- |
| Main CPU cycle counter | 50,000,000 cycles/s; source CPU input clock 100 MHz |
| H8/3002 | 16,934,400 cycles/s |
| C352 | 88,200 samples/s; source 25.4016 MHz / 288 |

Compare each device's counter delta divided by its frequency with main CPU cycle
deltas divided by `main_cycle_hz`. Main cycles accumulate with unsigned 32-bit
subtraction so normal rollover is included. The profiler starts a new `epoch`
with cleared totals at reset. The newer System12 reset implementation catches
up before clearing the CPU counter and preserves emulated absolute device time;
its exported profile counts remain relative to the new epoch. The frozen Vita
measurements below predate that change. This checks
frequency consistency, not exact MAME instruction costs or bus-event ordering.
GPU `modes` counts the endpoint status bits 19 and 20 of each frame; it is not a
trace of mode changes within a frame.

| Scope | What its elapsed interval includes |
| --- | --- |
| `cpu_inclusive` | `psxCpu->Execute`, including board devices and GPU work called within it |
| `h8` | Each H8 execution slice |
| `c352` | C352 sample generation |
| `board_audio` | Delivery to the board audio callback, including its queue wait |
| `gpu_list` | GPU command list parsing and any rasterization/flushes it invokes; sampled every 31 calls |
| `gpu_flush` | Explicit `renderer_flush_queues`; internal parser flushes are included in `gpu_list` instead |
| `video` | External frontend video callback |
| `present_copy` | Capture copy, scale map update and RGB565-to-display conversion inside `video` |
| `present_wait` | Frontend display vblank wait inside `video` |
| `audio_wait` | Audio queue backpressure inside the audio callback |

These intervals are nested. Do not sum the table or label subtraction of sampled
GPU time from CPU time an exact CPU residual. `estimated_us` extrapolates the
sample mean to the section's total call count; periodic GPU sampling can be
biased. Other sections use stride 1. Calls made before the first profiled frame
are excluded.

On Vita, timestamps use `sceKernelGetProcessTimeWide`. The tested Vita3K host
implements this using its host `high_resolution_clock` RTC in microseconds; the
values measure elapsed time, including blocking, rather than host CPU usage.
The checker also writes `<log>.perf.jsonl` with independent host monotonic
timestamps when it receives frame summaries, CPU-driver start and completed
replay messages. Reception is slightly later than guest emission. Compare the
differences between milestones to calibrate the guest clock. Logging and scope
timers have overhead, so use a same-source profiler-off control for throughput.

## Frontend architecture

The broader board/frontend coverage and existing optional workers are recorded
in [pcsx-arcade-architecture.md](pcsx-arcade-architecture.md).

The validated NEON VPK uses ARI64 for PSX MIPS-to-ARMv7 translation. Vita3K uses
Dynarmic outside it to execute the Vita ARMv7 application on the Mac. H8 remains
a separate interpreter. The GPU uses the C SIMD implementation with actual ARM
NEON instructions (`GPU_NEON_USE_C=1`); this setting does not select a scalar
renderer.

GPU, SPU, CD-ROM and DRC compiler workers are disabled in this VPK. The core runs
sequentially in the standalone loop. SDL audio playback has a queue and can block
the producer, but its effective backend thread count and core affinity have not
been measured. PCSX contains optional async infrastructure; there is no validated
Vita worker/core allocation here yet.

The Mac headless harness additionally calls `system12_sync` after every PSX
`ExecuteBlock`. Deltas below 256 main cycles are carried until a later call. The
libretro/Vita path calls it at both vblank edges, about twice per frame. The two
paths therefore have different H8/shared-RAM event order and C352 chunk sizes.
Matching aggregate clocks does not establish frontend equivalence. Neither bus
handler currently forces H8 synchronization on shared-RAM reads/writes. The new
timestamped C352 boundary catches up before H8 sound-register reads/writes;
key-on/off and BUSY-read fixtures now pass independently of fine/coarse MIPS
blocks. Complete MAME scheduler and frontend equivalence remain unverified.

Parallelizing H8 freely with PSX would introduce observable RAM and IRQ races.
Keep board time and IRQ delivery on an ordered emulation schedule. GPU async can
be evaluated through existing infrastructure once VRAM readbacks and barriers
are covered. CPU/GPU threading alone cannot produce 60 fps if H8 still consumes
more than 16.7 ms of work per emulated frame. Optimize measured H8 work first,
then reassess the benefit of distributing isolated work across Vita cores.

## First controlled diagnostic, 2026-10-05

The private profiler copy was based on `build/vita-system12/src`, with instrumentation
only in the four core/frontend paths above plus the standalone callbacks. The
original frozen VPK (`55258ebe…e1e8aa81`) and original core archive were preserved.
The matching original standalone object had been overwritten, so no bit-for-bit
frontend rebuild claim is made. The same-source profiler-off control rebuilds
all instrumented translation units without the macro and reuses unchanged core
objects from the full profiler build.

Profiler VPK SHA-256:
`490d3bdf3dce8b4d66402bd6ce70235515fd4f3f017d355134f31097731c944b`.
Host SHA-256:
`1f8f370352c0d344f0c9c0ada9551d0739c92ae693d87fc149ea3ded34d4120d`.
Build manifests are in `build/vita-system12-profile-20261005/build-evidence.json`
and `build/vita-system12-profile-off-20261005/build-evidence.json`.

The guarded 6,000-frame replay completed with 75/75 inputs, report, nonblack
framebuffer capture, no recorded host execution issues, and normal process exit.
Evidence: `/tmp/pcsx-neon-profile-6000-20261005/`. This remains a partial System12
validation, with `board_execution_verified=false`.

| Counter | Observed delta | Emulated duration |
| --- | ---: | ---: |
| Main | 5,014,405,004 cycles | 100.28810008 s |
| H8 | 1,698,318,802 cycles | 100.2881000803 s |
| C352 | 8,845,410 samples | 100.2880952381 s |

H8 differs from the main-derived expectation by about 0.005 cycles and C352 by
less than half a sample. Guest and independent host elapsed deltas over each
600-frame interval differed by at most 144 microseconds.

| Timed scope | Cumulative elapsed | Share of total diagnostic elapsed |
| --- | ---: | ---: |
| Total diagnostic elapsed | 361.365 s | 100% |
| CPU inclusive | 310.372 s | 85.9% |
| H8 (inside CPU) | 273.680 s | 75.7% |
| C352 (inside CPU) | 6.557 s | 1.8% |
| GPU lists, sampled (inside CPU) | estimated 17.430 s | estimated 4.8% |
| External video callback | 50.765 s | 14.0% |
| Display wait (inside video) | 43.695 s | 12.1% |
| Copy/scale (inside video) | 7.045 s | 1.9% |
| Audio queue wait (inside board audio) | 0.035 s | 0.01% |

H8 retired 297,843,065 instructions and ran in 11,999 slices. This is the measured
dominant section in this diagnostic, including gameplay.

The same-source profiler-off control also passed 6,000 frames and normal shutdown:
`/tmp/pcsx-neon-profile-off-6000-20261005/`, VPK SHA-256
`d0c751368c32e5c566a55ef9b64749c4d55743a55338a8a591cabbfac1d3d326`.
Host monotonic reception timestamps from CPU-driver start to replay completion
span 361.434 seconds, or 16.60 fps. The profiler's 361.365 seconds is within
0.02% of that interval. This single pair does not establish a statistical
instrumentation overhead bound; it does show consistent throughput for this run.
The final PPM, full board report and replay input CSV are byte-identical between
on and off. PPM SHA-256:
`21648aed86d979da130e4da29a36873f47b5f5bbe5863797ee24ac20f81356c0`.
No physical Vita performance claim follows from these Vita3K measurements.

## H8 IRQ selection optimization

`fdfe62bd` replaces the unconditional scan of vectors 7–63 with iteration over
the pending bits in ascending order. The six external interrupt-line checks,
priority/filter rules, lowest-vector tie handling, latching, and all calls from
instruction prefetch, tick and execution remain unchanged. No instructions or
device events are skipped. VitaSDK lowers the trailing-zero operation to inline
ARM RBIT/CLZ; the tested Dynarmic A32-to-ARM64 path supports both operations.

The differential test compares against the old scan over empty, one-bit, two-bit,
priority/mask and 100,000 deterministic random states. It and the synthetic H8
instruction/IRQ/timer/SCI/ADC/scheduling suite pass normal and UBSan builds.

The profiler-off optimized VPK SHA-256 is
`b9ed187b9505f1bc3b13eab9d32e7faec542dcd6aa738393f9549cd0f48bab8a`.
The guarded same-input 6,000-frame replay passed at
`/tmp/pcsx-neon-h8-irq-6000-20261005/`:

| Variant, profiler off | Host elapsed | Mean emulated frames/s |
| --- | ---: | ---: |
| Original IRQ scan | 361.434 s | 16.60 |
| Pending-bit iteration | 184.165 s | 32.58 |

This is a 1.96× improvement for this measured replay. Final image, complete board
report and input CSV match byte-for-byte; main/H8/C352 clocks, scheduling and
counter totals are unchanged. Both guests exited normally. This is a single
paired experiment; per-scene throughput varies.

The native Mac 12,000-frame replay also retained the original frame and entire
VRAM hashes, 10,029,060,040 main cycles, 3,396,722,206 H8 cycles, 597,909,593 H8
instructions, and 17,691,261 C352 samples. Evidence:
`/tmp/pcsx-mac-h8-irq-soul-12000-20261005/`. Native emulation elapsed was 67.742 s;
the previous baseline ran concurrently with Vita3K, so it is not used for a
paired native speedup claim.

## Current cost and architecture recommendation

The post-change instrumented VPK
`024b73e4d4679fd14b28d7ef72b1a42e76aeee9565d92f677761ccde30193419`
also passed the same replay and identical output/report at
`/tmp/pcsx-neon-h8-irq-profile-6000-20261005/`. Guest elapsed was 188.922 s;
independent host elapsed was 188.928 s. This is 2.6% above the uninstrumented
optimized run; individual scope times and sampled GPU estimates should be read
with that measurement limitation.

| Post-change scope | Elapsed | Share of instrumented total |
| --- | ---: | ---: |
| CPU inclusive | 135.644 s | 71.8% |
| H8, inside CPU | 89.769 s | 47.5% |
| C352, inside CPU | 6.911 s | 3.7% |
| GPU lists, sampled inside CPU | estimated 21.705 s | estimated 11.5% |
| External video | 53.030 s | 28.1% |
| Vblank wait, inside video | 45.659 s | 24.2% |
| Copy/scale, inside video | 7.342 s | 3.9% |
| Audio queue wait, inside board audio | 7.002 s | 3.7% |

The H8 scope fell from 273.680 to 89.769 seconds. It remains the largest measured
compute section. GPU and main-CPU work still run sequentially with it; frontend
display and audio waits have become more visible. This table is not an exact
exclusive partition because of nested scopes and periodic GPU sampling.

Keep the current synchronous architecture while studying these three points:

1. Continue examining H8 interpreter/bus/peripheral costs while preserving
   retired instructions, cycle budgets and IRQ/event boundaries.
2. Reconcile Mac and libretro board scheduling before using their results as
   evidence of equivalent hardware behavior. A common causal scheduler must
   account for shared RAM, IRQ delivery and C352 register/sample timestamps.
3. Review frontend pacing and buffer ownership together: video vblank waits and
   audio queue backpressure can both block the same emulation thread. Additional
   workers do not automatically remove that blocking.

Threading is an architecture study only. No workers or core affinities were
changed. If parallel execution is later evaluated, the existing GPU worker is a
candidate with explicit VRAM/readback barriers. Keep PSX/H8 shared-memory and IRQ
ordering defined; do not run H8 independently without a causal synchronization
design. SDL already has an audio playback path, whose actual threads and
affinities still need measurement. There is no validated allocation across Vita
cores 0/1/2 yet.

Combined manifests and comparisons:
`build/vita-system12-h8-irq-20261005/comparison-evidence.json`.

## Presentation and interpreter follow-up: source review only

The final post-change log distinguishes 6,000 video callback entries from
**4,761 valid image conversions/display waits**. `video_cb` returns immediately
for null data or zero dimensions. The measured averages are:

| Scope | Calls | Mean per call | Mean per emulated frame |
| --- | ---: | ---: | ---: |
| H8 slice | 11,999 | 7.481 ms | 14.962 ms |
| Display wait | 4,761 | 9.590 ms | 7.610 ms |
| Capture/copy/scale | 4,761 | 1.542 ms | 1.224 ms |
| Audio queue wait scope | 35,999 | 0.194 ms | 1.167 ms |

Audio wait scope calls include checks that do not actually sleep. Copy time
includes the enabled final-frame capture copy; it is not solely RGB conversion.
The averaging denominator matters: neither display wait nor H8 slice occurs
exactly once per emulated frame. Evidence is the final `PERF` block in
`/tmp/pcsx-neon-h8-irq-profile-6000-20261005/vita3k.log`.

The private host source used by this replay is
`build/vita-validation-mspace-stats-fix2/host-fix-src`. Its display wait and
vblank sync implementation matches the corresponding functions in `../vita3k`;
the files as a whole differ because of the private lifetime/teardown fixes and
display logging. `SceDisplay.cpp::display_wait` chooses at least
`vblank_count + 1` for `sceDisplayWaitVblankStart`. `display.cpp` increments that
counter on a host thread with a nominal 60 Hz cadence and suspends the waiting
guest thread until the target count. Thus the frontend still incurs a next-edge
wait when emulation is already slower than real time. This explains the
blocking mechanism, not a statistical prediction of each wait's duration or
physical Vita behavior.

The frontend fills the alternate framebuffer, submits it with `NEXTFRAME`,
waits, and then swaps its front index. Removing that wait without a replacement
buffer-lifetime contract could let the producer overwrite a buffer still being
displayed. The appropriate pacing study is to distinguish buffer availability
from real-time throttling, including the host's predicted-frame/display access
and teardown lifetime. Keep the emulated vblank/IRQ timeline independent of this
presentation decision. No wait or buffer implementation has been changed.

Audio uses an SDL stream with source 88.2 kHz and device/output 48 kHz, confirmed
by the replay log after the board AV update. The frontend's queue threshold is
8,192 bytes (2,048 stereo S16 source frames, nominally 23.22 ms at 88.2 kHz).
It delays the producer in 1 ms increments while the queue exceeds that limit.
Removing display waiting can move backpressure to this queue instead of removing
it. Source sample cadence, device resampling and queue occupancy must be assessed
together before selecting one pacing policy.

Before changing pacing, a future diagnostic should record presentation return
codes, buffer identity and vblank count before/after each wait, plus audio queue
occupancy, wait iterations and maximum blocked interval. Aggregate these into
periodic summaries rather than per-sample logs. Current gameplay presentation
return codes are ignored, and an audio queue that stops draining can keep this
callback waiting without a local timeout or further input polling. Neither
case should be diagnosed from the FPS number alone. Preserve the existing
external watchdog and verify clocks/replay signatures in any diagnostic build.

The source review identifies these H8 candidates; their individual cost has
**not** been measured within the 47.5% H8 scope. That scope also includes bus and
peripheral callbacks invoked by H8 execution, not only instruction arithmetic:

- `h8_run_cycles`, `prefetch_done` and `tick` all select pending IRQs. The recent
  optimization made each selection cheaper. Further caching would need to track
  every pending/enable/priority/CCR change and preserve the original recognition
  point, latching and acknowledgement; dropping calls is not justified.
- `tick` checks five timers, two SCI deadlines and ADC after each execution
  state, including intermediate prefix-decoder states as well as completed
  instructions and exception states.
  Active internally clocked timers iterate each timer tick. A future deadline or
  arithmetic implementation must preserve compare-before-increment semantics,
  overflow, simultaneous A/B matches, clear modes, fractional divider phase,
  status/enable transitions and interrupt observation. Timer-specific profiling
  and differential state/event checks should precede implementation.
- Instruction dispatch uses the generated decoder, and fetch/data accesses use
  bus callbacks with side effects and cycle charges. Decode caching or a new H8
  DRC would need fetch/SMC, atomic C352 word access and precise IRQ/cycle guards.
  No benefit estimate or implementation is established here.
- The board's `sub_read16` resolves two byte reads for ROM/shared accesses.
  A direct word path is another candidate if it retains big-endian assembly,
  boundary behavior and access counters. C352 atomic word side effects must
  remain on their existing distinct path.

First investigate timer and IRQ cost while retaining synchronous execution;
then choose between a faithful interpreter improvement and a larger H8 backend
change from measurements. A worker would relocate the same interpreter cost and
introduce shared-memory ordering requirements. Presentation changes, H8 backend
work and parallel execution remain proposals for review, not runtime changes.

## Mac C352 timing fidelity, 2026-10-05

The priority is a faithful Mac reference before selecting further speedups.
The first concrete mismatch was C352 register/sample ordering: the old adapter
executed the whole H8 interval, then generated all its audio using the resulting
register state. A key-on could affect a sample preceding its bus access; a read
could inspect BUSY before the samples that should clear it had been generated.

Commits `3124e90a` and `b5a670d4` expose the H8 access cycle and catch up C352
before each byte/word read or write. The timestamp includes costs charged inside
the current execution state. SCI/ADC callbacks after `tick` use committed cycles
without charging that state again. Word writes remain atomic. One monotonic
stream cursor combines register catch-up and the existing board catch-up.

The H8 phase uses `floor(1e18 / 16934400)` attoseconds per cycle, matching the
pinned MAME scheduler's accumulated normal subsecond slices. At 88200 Hz the
reduced conversion is `26041666666617 / 5000000000000000`. Bounded 64-bit chunks
preserve that phase without requiring 128-bit arithmetic in the Vita core.
Host tests compare it with an independent wide-integer reference over sample
edges, split updates, rewind rejection and 24 hours. Time zero emits nothing;
at positive time the stream includes its current sample as MAME does.

`c2e1f5f7` adds a real MAME oracle. The installed executable reports **0.287
(unknown)**; its precise source commit is unavailable. Source reasoning remains
against pinned `deb2f54fe7300e7a192690318beb860159bfd52a`. The oracle transiently
replaces H8 instructions and the first wave bytes in memory after loading
`soulclbrac` from the local `soulclbr.zip` archive. It captures the C352 sound
hook before speaker resampling, uses private config/NVRAM directories, and
leaves the archive unchanged. Every run has a 45-second kill timeout.

The actual MAME capture and native adapter produce these identical front-channel
raw sample prefixes (left and right are equal):

| Key-on H8 cycle | Samples 0..4 | First nonzero | First silent after key-off |
| --- | --- | --- | --- |
| 190 | 0, 8, 16, 24, 0 | 1 | 4 |
| 192 | 0, 8, 16, 24, 0 | 1 | 4 |
| 194 | 0, 0, 8, 16, 0 | 2 | 4 |

MAME exposes normalized floats; multiplication by 32768 yields these integers
exactly. Native integration tests run the actual H8 firmware through the board
with both MIPS interpreter and DRC, fine and coarse main CPU blocks, and an
additional one-shot BUSY/key-off read fixture. All pass. The prior binary fails
the 190-cycle test by starting at sample zero, so the test detects the original
regression rather than merely accepting both implementations.

Reproduction:

```sh
make -f Makefile.mac-harness BUILD=build/mac-system12-audio-time-20261005 DRC=1 SYSTEM12_TIMING=1 -j4
make -f tests/Makefile.h8 test irq-test
make -f tests/Makefile.c352-timing test
python3 tests/check_system12_audio_timing.py --harness build/mac-system12-audio-time-20261005/pcsx-harness
python3 tests/check_system12_audio_timing.py --harness build/mac-system12-audio-time-20261005/pcsx-harness --drc
python3 tests/check_mame_c352_timing.py
```

Normal and UBSan C352/clock tests pass; existing synthetic board and native DRC
bank/RAM/SMC checks pass. Evidence logs are
`/tmp/pcsx-mac-audio-time-{interpreter,drc,board-test,drc-test}-20261005.log`,
`/tmp/pcsx-c352-timing-{root,ubsan}-20261005.log` and
`/tmp/pcsx-mame-c352-oracle-20261005.log`. The older failing run is retained at
`/tmp/pcsx-audio-timing-old-regression-20261005.log`.

An isolated native NEON/ARI64 Soul Calibur replay of **12000 frames** completed
with the same input script as the earlier IRQ-optimization replay. Main cycles
remain `10029060040`; H8 cycles remain `3396722206`, with `597909593`
instructions, no illegal/unsupported instruction and no unmapped board access.
PCM frames are now `17691262`, one more than the old stream, consistent with
including the current sample. The final framebuffer and synchronized VRAM are
byte-identical to the earlier replay. This is regression evidence, not a claim
of pixel equivalence to MAME or of complete game compatibility.

The harness reported 65.965 seconds (66.516 seconds including startup/teardown).
One replay does not establish a speed improvement. Audio generation from H8 bus
callbacks now falls inside the inclusive H8 profiling scope, so future scope
comparisons must account for that nesting. Frozen evidence is under
`/tmp/pcsx-mac-c352-timed-soul-12000-20261005/`, including the binary, log,
framebuffer, VRAM and `evidence.json`. Binary SHA-256:
`cd3377f6597b71e25a3586e8224fd78baf746956e0c8b39dadda444cf4123580`.

At the C352-access milestone, the next fidelity gates were actual
scheduler/shared-mailbox and vblank traces, H8 peripheral deadlines/partial
execution, and soft-reset phase. The reset mismatch is addressed in the next
section; register-edge tests alone did not cover it. No new Vita,
vitaGL, worker or H8 DRC validation is claimed for these commits. Optimization
should preserve the established bus/state/sample traces and then demonstrate a
measured reduction in cost; a worker or DRC is still an architectural candidate.

## Soft-reset retention and phase, 2026-10-05

Commits `056f7832`, `eca1460d` and `2dcecc79` separate initial machine creation
from soft reset. Source review found that `namcos12_state::driver_start` sets
banks and DMA offsets, whereas `driver_reset` only repeats the optional boot
copy. CPU main RAM and shared RAM are not erased by reset. H8 reset preserves
ER/CCR, internal RAM and the mapped GPIO registers. MAME preserves CPU local
time and the sound stream cursor while resetting the C352 voices/control.

The former adapter cleared all those storage/phase domains. It now retains main
and shared RAM, the bank selector and offsets, held input levels, modeled H8
storage/GPIO, clock conversion remainders and the absolute PCM cursor. COH-716
still repeats its source boot copy of exactly 12 bytes. `system12_pre_reset`
catches up even a pending interval shorter than 256 main cycles before
`psxReset` clears the main counter.

H8 reset is explicit about instruction debt: MAME's H8 engine clears its carried
overshoot at reset, and its scheduler preserves budget-based local time rather
than advancing by the overshot instruction tail. The board accumulates requested
H8 clocks separately from its executed-state diagnostic count and supplies that
absolute budget to `h8_soft_reset`. The CPU resets execution/peripheral state,
retains ER/CCR/RAM/GPIO, and starts a new diagnostic epoch. Old SCI/ADC deadlines
are cancelled; future deadlines use the new absolute domain. The stream clock
rejects a temporary backwards access timestamp without modifying its cursor,
then resumes once the CPU reaches the last rendered access time. Audio callbacks
are synchronous and must not reset or re-enter the core.

The independent real MAME reset oracle requests reset at
`0:16715113233669674` attoseconds from `frame_done`. The reset notifier observes
the same time. The raw C352 capture contains a first burst at sample 1
(`8,16,24,0,0`) and a second at absolute sample **1476** (`8,16,0,0,0`), proving
that the output timeline does not restart at zero. The second onset and key-off
phase match the timestamp prediction exactly, without a one-sample tolerance.
Both status reads are `0006`. As before, this installed MAME reports 0.287 with
unknown build commit; pinned-source reasoning and runtime evidence are distinct.

Native integration tests execute a MIPS boot counter and initialize RAM/shared
markers and bank 2 only on the first boot. After the harness invokes `psxReset`,
the guest reads and stores the retained state rather than recreating it. Five
cases per backend cover four COH-700 reset times, including the same PCM phase
bin as the MAME capture, and a COH-716/altbank reset. They verify two boots, main
and shared RAM, banked reads, held coin/start, timed H8 audio, the 12-byte boot
copy, its untouched neighboring word and retained RAM near the top of the 16 MiB
configuration. Interpreter
and DRC produce identical observed reset times/onsets for these cases.

The previous System12 reset implementation was separately compiled with the
new harness diagnostic switch and a no-op pre-reset shim. The same test fails:
the second burst includes an extra nonzero sample at index 7 rather than going
silent there. This baseline binary/source/build log is retained under
`/tmp/pcsx-system12-pre-reset-baseline-20261005/`; its failure is logged in
`/tmp/pcsx-system12-reset-old-regression-20261005.log`.

Reproduction after building the source-timing Mac DRC harness:

```sh
make -f tests/Makefile.h8 test irq-test
python3 tests/check_system12_reset.py --harness build/mac-system12-audio-time-20261005/pcsx-harness
python3 tests/check_system12_reset.py --harness build/mac-system12-audio-time-20261005/pcsx-harness --drc
python3 tests/check_mame_c352_reset.py
```

H8 retention/debt tests pass at O2 and with UBSan, including 64-bit/zero epochs,
repeated reset, prefix/illegal-state disposal, and rejection during bus/SCI/ADC
callbacks. Existing H8, IRQ, C352-access, board/profile and native DRC bank/RAM/SMC
fixtures also pass. The new harness switch `--reset-at-cycle N` requests one
System12 reset at the first safe block endpoint reaching that cumulative main
cycle; it preserves replay frame progress and fails if the request is not reached.
It is a validation control, not an exact-instruction scheduling guarantee.

Evidence logs:
`/tmp/pcsx-h8-soft-reset-root-20261005.log`,
`/tmp/pcsx-h8-soft-reset-ubsan-root-20261005.log`,
`/tmp/pcsx-system12-reset-{interpreter,drc,board,existing-drc}-20261005.log`,
`/tmp/pcsx-system12-reset-existing-audio-{interpreter,drc}-20261005.log`,
`/tmp/pcsx-mame-c352-reset-oracle-20261005.log` and
`/tmp/pcsx-mame-c352-oracle-busy-20261005.log`.

A second isolated **12000-frame no-reset** Soul Calibur replay verifies creation
and normal execution after this lifetime change: final framebuffer, VRAM, main
cycles `10029060040`, H8 cycles `3396722206`, H8 instructions `597909593` and PCM
frames `17691262` match the preceding C352-timing replay. It completes without
illegal/unsupported H8 instructions or unmapped board access. This does not
itself validate a real-game reset. Frozen binary/log/evidence are under
`/tmp/pcsx-mac-reset-fidelity-soul-12000-20261005/`; binary SHA-256 is
`d39c2e233c911d419f335537f0b5e735500d1a9cdfce1a24cf145f468b0a2a33`.

An authentic **3600-frame warm-reset** replay also completes: the harness
resets at the safe block endpoint reaching main cycle `835756`, then continues
the existing input script. The captured image shows Stage 1, Ivy versus Kilik,
with timer 24 and credit 0. It reports no illegal/unsupported H8 instruction or
unmapped board access. Frozen output and the PNG preview are under
`/tmp/pcsx-mac-reset-fidelity-soul-warm-3600-20261005/`. This demonstrates resumed
game execution; no identical real-game MAME reset replay was captured, so it
does not establish gameplay-state or pixel equivalence across that reset.

MAME's immediate H8 debug PC/IR/temporary snapshots before reset prefetch also
retain old values that this standalone API reconstructs. Source reset execution
overwrites its active vector/prefetch fields before executing guest instructions;
the current gates make no claim of identical prefetch-time debug snapshots.

Full scheduler/shared-mailbox/vblank and H8 partial-state/peripheral equivalence
remain open. No new Vita or vitaGL execution is claimed, and no worker or H8 DRC
has been introduced. The scope remains all ten PSX-derived driver families;
these are System12 fidelity gates, not completion of a whole family.

## Measured Mac vblank discrepancy, 2026-10-05

`2d953d84` adds passive pin diagnostics, without changing device clocks or the
scheduler. `--system12-edge-trace FILE` records vblank delivery and H8 port-B
input observations before GPIO masking. It retains the last old and first new
poll around each transition. Rows include the 32-bit main counter, its last
synchronized value, absolute H8 access cycles and accumulated H8 budget. The
main counter is not an instruction-exact PC/time trace under every DRC path.
Reset/unload ownership and output truncation are explicit; overflow or file
errors fail the harness run.

The native and MAME fixtures use identical H8 firmware, words
`6a08 ffd6 40fa`: read port B at `0xffffd6`, then branch back. Reset prefetch
places the first read at H8 cycle 12; subsequent reads are ten cycles apart.
Both fixtures replace MIPS startup with a jump/delay-slot-NOP loop. The native
coarse case inserts 1024 NOPs before that jump to expose block-dependent event
delivery. No authentic ROM archive is modified.

The MAME oracle taps actual CPU program-space reads, normalizes the high byte
of the 16-bit big-endian bus, and retains integer attosecond timestamps. Its
reset notifier establishes epoch zero. Screen API targets bracket each last-old
and first-new read; the initial start target is reconstructed from consecutive
exact start targets, rather than treating `frame_done` as a vblank timestamp.
The first read and every reported read gap are checked against the truncated
H8 attosecond period. The installed MAME still reports **0.287 (unknown)**;
source reasoning uses the pinned tree, not a verified matching executable.

The first four transition observations are:

| Port-B bit 7 becomes | MAME first new H8 read | Mac fine-block first new read | Mac coarse-block first new read |
| --- | ---: | ---: | ---: |
| 1 | 283062 | 240702 | 241172 |
| 0 | 325402 | 283012 | 283562 |
| 1 | 566122 | 523792 | 524032 |
| 0 | 608462 | 566102 | 566422 |

Native MIPS interpreter and ARI64 produce the same table. The observer-on/off
controls have identical board reports and framebuffers in each native case.
Those controls validate passive capture, not MAME fidelity: the strict
`compare_system12_vblank.py` gate **fails** for both backends and both block
sizes, with no timing tolerance.

MAME's first scheduled start is `0:16715113233669674` attoseconds, followed by
its end exactly 2.5 ms later. The fine native case instead delivers start at
main cycle 710756 (14.21512 ms) and end at 835756 (16.71512 ms). This reveals a
phase discrepancy of about 2.5 ms, in addition to synchronization granularity.
The fine case leaves main-cycle tails `[100, 172, 16, 88, 188, 4]` pending when
delivering its six edges. Coarse block delivery removes that tail but is late
relative to the native counter deadlines; its first start is cycle 712044.

Source inspection explains the phase discrepancy. Our source-timing
`scheduleRcntBase` places the rising edge at `frameCycles - PSXCLK/400` and the
falling edge at `frameCycles`. Pinned `screen.cpp` starts its vblank timeline at
zero and schedules the next start a full period later, then the end after its
configured blank duration. `psx.cpp` configures a 2500 us blank duration, which
the source explicitly labels approximate. Fidelity to this MAME model requires
its phase as well as its average refresh rate; this capture does not establish
physical arcade timing accuracy.

The next correction must cover source screen phase/deadlines, fractional frame
periods, mode changes and reset continuity, then order H8 execution and external
edges at those deadlines. Merely forcing the sub-256 tail or shifting one
callback does not establish partial-instruction/bus ordering. Timed shared-RAM
protocol and IRQ/ADC fixtures remain separate requirements. No scheduler fix,
worker, H8 DRC, Vita or graphics validation is claimed by this measurement.

Reproduction:

```sh
python3 tests/check_mame_system12_vblank.py --output /tmp/mame-vblank.json
python3 tests/check_system12_vblank.py --harness build/mac-system12-audio-time-20261005/pcsx-harness --output /tmp/mac-vblank-interpreter.json
python3 tests/check_system12_vblank.py --harness build/mac-system12-audio-time-20261005/pcsx-harness --drc --output /tmp/mac-vblank-drc.json
python3 tests/compare_system12_vblank.py --mame /tmp/mame-vblank.json --native /tmp/mac-vblank-interpreter.json /tmp/mac-vblank-drc.json
```

At pre-correction revision `42e64dc2`, the capture fixtures pass and the last
command returns 1 for the measured fidelity failure. Frozen native binary/evidence are under
`/tmp/pcsx-mac-vblank-fidelity-20261005/`, binary SHA-256
`320f9386de8e9ca4d71294df0dccb5dd421bb746f10c9726e447f27bb5e70feb`.
Root MAME evidence is `/tmp/pcsx-mame-system12-vblank-root-20261005.json`
with adjacent stdout log. Comparison evidence is
`/tmp/pcsx-system12-vblank-comparison-20261005.{json,log}`.

## Mac source screen correction, 2026-10-05

Mac fidelity to MAME remains the first target. Optimization work must preserve
the compared device observations; neither a frame-rate improvement nor this
narrow gate establishes complete board fidelity.

`a1b20e0c` adds a standalone screen clock modeled on pinned MAME
`screen_device::configure/vblank_begin`, with integer-attosecond periods and
the source's approximate 2500 us blank duration. It retains fractional frame
phase, the previous start origin on refresh changes and an already armed end
timer. The cold end time exists for `screen.vblank()` queries but its timer is
unarmed until the first actual start. The component preserves phase across
GPU and machine reset rather than restarting a frame at reset time.

`bff1141c` generates both full and partial H8 instruction states using MAME's
bus checkpoints. `h8_run_until` advances the committed H8 clock to an absolute
external deadline and resumes the partial state after the caller delivers the
edge. A bus operation can cross the deadline: its callback observes the charged
cycle before the edge, and the remaining charge is carried forward without
repeating the access. This follows the post-access checkpoint contract, rather
than claiming that every callback is bounded by the external deadline.

`cc02244d` schedules source screen events independently of
MIPS block endpoints. It runs the H8 to the floor of the event's absolute time
on the truncated H8 clock, then delivers IRQ1, ADTRG and port-B bit 7 in source
order. GP1 reset/display-mode writes update the source screen configuration;
warm reset restores GPU defaults and retains absolute screen time. Compatibility
timing and other board families keep their existing counter path.

`cafcc034` also preserves catch-up for frontends that rely on core vblank
events. RTC/EEPROM/PCM advance after all overdue screen edges have been
delivered, so H8 execution cannot pass a later queued edge while catching up
to a late MIPS endpoint. The native diagnostic option
`--system12-vblank-sync-only` omits the harness's extra block synchronization.
Before this fix its silent-ROM fixture left C352 at zero generated frames;
after the fix PCM advances and the same strict H8 polling gate passes on both
CPU backends and block sizes. This simulates the cadence in Mac, without
claiming an actual libretro/Vita run.

The strict polling comparison now passes with **zero cycle tolerance**:

| Port-B bit 7 becomes | MAME old → new H8 read | Mac interpreter, short/long blocks | Mac ARI64, short/long blocks |
| --- | ---: | ---: | ---: |
| 1 | 283052 → 283062 | 283052 → 283062 | 283052 → 283062 |
| 0 | 325392 → 325402 | 325392 → 325402 | 325392 → 325402 |
| 1 | 566112 → 566122 | 566112 → 566122 | 566112 → 566122 |
| 0 | 608452 → 608462 | 608452 → 608462 | 608452 → 608462 |

Observer-on/off controls retain identical native board reports and images.
The recorded main-counter synchronization tail can still be nonzero when an
edge is traced: the H8 is now advanced separately to the scheduled edge time,
so that field describes the last ordinary bridge synchronization, not the
delivered H8 deadline. MIPS interrupt delivery remains subject to the core's
block boundary granularity; no instruction-exact MIPS interrupt claim follows
from the H8 comparison.

Seven real installed-MAME captures cover initial phase, NTSC480, PAL, a change
during blank, shortening a frame past its elapsed age, GP1 reset and machine
soft reset. `compare_psx_screen_clock.py` compiles the actual C component and
replays their exact integer write/reset/sample timestamps. All queried start
and end targets match with zero tolerance; rounded floating-point period
diagnostics are not used for comparison. The installed executable's source
revision is still unknown. Evidence is `/tmp/mame-psx-screen-clock.json`.

`f0de5e78` executes GP1 mode writes from synthetic native MIPS guests on both
interpreter and ARI64. It checks exact absolute H8 edge clocks for NTSC480,
PAL and PAL-to-default GPU reset. The during-blank write retains the old end
and uses the previous start as the new period's origin; a main-CPU trace
confirms the write occurs inside that window. Warm reset at actual main cycle
850002 retains the active end and schedules the next default start from the
last NTSC480 start, rather than from reset time.

Standalone screen normal/UBSan tests and H8 normal, IRQ and deadline UBSan
tests pass. Native H8/C352 key-on/off, timed reads and warm-reset retention/PCM
phase gates pass on both interpreter and ARI64. Comparison evidence is
`/tmp/pcsx-system12-source-screen-final-comparison-20261005.json`; native
captures are `/tmp/pcsx-system12-source-screen-{vblank,vblank-only}-{interpreter,drc}-20261005.json`.
Console smoke and the existing synthetic System11, System10, Capcom ZN,
Taito FX1, System GV and G-Net gates also pass; they remain component/integration
regressions, not authentic-game validation of those families.

Additional reproduction commands, after the build used above:

```sh
make -f tests/Makefile.psx-screen-clock test sanitize
make -f tests/Makefile.h8 test irq-test deadline-ubsan-test
python3 tests/check_mame_psx_screen_clock.py --output /tmp/mame-screen-clock.json
python3 tests/compare_psx_screen_clock.py /tmp/mame-screen-clock.json
python3 tests/check_system12_screen_modes.py --harness build/mac-system12-audio-time-20261005/pcsx-harness --drc
python3 tests/check_system12_vblank.py --harness build/mac-system12-audio-time-20261005/pcsx-harness --drc --vblank-sync-only --output /tmp/mac-vblank-cadence.json
python3 tests/compare_system12_vblank.py --mame /tmp/mame-vblank.json --native /tmp/mac-vblank-cadence.json
```

An authentic NEON/ARI64 Soul Calibur replay completes **12000 frames** with
no illegal/unsupported H8 instruction or unmapped board access. Its final
image shows the continue prompt, credit 0. Main cycles are `10029192943`, H8
cycles `3396767298`, H8 instructions `598911789`, and PCM frames `17691496`.
The screen correction changes the gameplay timeline, so the previous replay's
framebuffer and counters are not an equivalence baseline for this run. Its
80.432 s elapsed time is a single smoke measurement, not a controlled speed
comparison. Frozen binary/log/image are under
`/tmp/pcsx-mac-screen-cadence-soul-12000-20261005/`, binary SHA-256
`f26faf50fcc9ed5644bc521e99ea8be45aec7ae4b4b7c42ce1017e57ed0c70d8`.
The framebuffer and all final board report lines equal the source-screen replay
before the frontend-cadence fix; `cadence-regression.json` records that control.

The following internal-device correction supersedes the earlier coarse
H8 timer/SCI/ADC scheduling. Timed shared-mailbox protocols,
MIPS IRQ ordering, real-game state/pixel/audio comparison and native/libretro
scheduling equivalence remain open. No worker, H8 DRC, new Vita or vitaGL
validation is claimed. Zero whole driver families are complete.

### Mac H8 internal-device correction (2026-10-05)

The ADC3337, H83002 timer16 and SCI source models now replace the coarse
register-array implementations. `h8_run_until` bounds partial CPU execution by
the next internal event as well as the external board deadline. ADC starts
from the charged write cycle; timers settle elapsed time before register
mutations and use source compare/divider arithmetic; SCI runs physical TX/CLK
transitions rather than delivering grouped bytes. System12 wires inverted
RTC CLK first, then settings CLK, and routes RTC DATA to RX1. SCI0's fixed
external clock period is `542534722222` attoseconds. Soft reset restarts local
peripheral cycles while keeping the board's absolute callback time and SCI
status-read/RX latches.

The independent installed-MAME probe injects four H8 programs: ADC start/poll,
timer0 counter polling, internal asynchronous SCI, and external-rate SCI.
Each emits 96 external observation stores. Native runs with 1/1024/10000-cycle
slices match **1152 value/address/cycle comparisons with zero differences**.
The fixed pre-port baseline `dde07db7` differs in 101 observations. For example,
the ADC read at cycle 280 still sees ADST in MAME and the new port, whereas
the old port has completed early; its observable store differs at cycle 288.
Internal SCI SSR polls bracket TEND between cycles 356 and 374; the source
external-rate case brackets it between 1508 and 1526. Parent ROM/template
hashes remain unchanged and all MAME runs exit under the watchdog.
Installed MAME is 0.287; matching that binary to the pinned source remains
unverified. These are register traces, not full RTC/JVS waveform captures.

Component normal/UBSan tests and H8 instruction/IRQ/deadline/reset tests pass,
including an odd soft-reset epoch above 32 bits. Native interpreter and ARI64
GP1 modes/reset, C352 key-on/off phase and warm-reset retention gates pass.
A fresh MAME vblank capture also matches the four initial old/new port-B read
pairs exactly for both CPU engines, both MIPS block sizes and both frontend
sync cadences.

An authentic Mac NEON/ARI64 **12000-frame** run uses the existing
`artifacts/system12/match-3600.csv` input script. It completes at main cycles
`10029193005`, H8 cycles `3396767298`, H8 instructions `590887759`, and PCM
frames `17691497`, without H8 illegal/unsupported or unmapped accesses. The
final image shows the VS-BATTLE ranking screen. This run has a different input
script from the former temporary replay and is not its state/image equivalence
baseline. The measured 57.912 s emulation elapsed is a smoke observation, not
a controlled optimization benchmark. The frozen executable SHA-256 is
`c991acc204feeffd8338b78a44f2f8013482a61232b09fcdd6e336602c4715b4`.

Evidence is retained in ignored `artifacts/validation/h8-peripherals-20261005/`:
`summary.json`, `mame-peripherals.json`, `vblank-comparison.json`,
`core-final.log`, `mac-gates.log`, and the frozen executable/input/log/PPM/PNG/WAV.
Older `/tmp` evidence referenced above is historical and was lost when the
execution environment restarted; current claims use these fresh captures.

Reproduction uses the Mac source-timing build command above, the component
gates in [H8 port details](h8-port.md), and:

```sh
python3 tests/check_mame_h8_peripherals.py --include-sci --output artifacts/validation/h8-peripherals-20261005/mame-peripherals.json
```

At this milestone, open fidelity work included RX-start scheduler-attotime
phase (addressed for CPU MMIO in the next section),
standby/DMA/watchdog, timer external pins, complete RTC/JVS wire traces, timed
shared mailboxes and MIPS IRQ ordering, real-game MAME pixels/audio, and
native/libretro scheduler equivalence. The legacy whole-state `h8_run_cycles`
API retains coarse checkpoints. Worker/H8 DRC and new Vita/vitaGL validation
remain outside this milestone. No whole driver family is complete.

## Mac SCI receive clock correction, 2026-10-05

Mac fidelity to MAME is the first gate. Performance changes must preserve the
reproducible device/board comparisons and measure a reduction in cost. Worker
and H8 DRC proposals remain architectural studies.

MAME starts TX from `total_cycles()` but RX from `now_as_cycles()`. During CPU
MMIO, `machine().time()` uses that CPU's charged local attotime. The port now
accumulates the source integer attotime conversions separately from executed
cycles, retaining fractional phase over soft reset. No floating point or 128-bit
arithmetic is required. Timer expiration contexts and full warm-reset timing
are not covered by this cold-start comparison.

A fifth injected program starts synchronous RX without TX. Before the change,
the SSR read at cycle 96 returned `c4` in Mac and `e4` in MAME: Mac exposed
overrun one poll late. The observable store at cycle 104 differed at all three
native slice sizes. After correction, all five programs' first 96 stores at
1/1024/10000-cycle slices match **1440 addresses, values and cycles exactly**.
The preceding committed implementation failed three observations; the older
fixed baseline `dde07db7` differs in 195 observations over the expanded suite.

Normal/UBSan clock and H8 instruction/IRQ/deadline/reset gates pass. Mac
interpreter and ARI64 screen-mode, C352 audio and warm-reset integration gates
pass. The existing MAME vblank capture still matches all 32 old/new read pairs
across engines, MIPS block sizes and frontend catch-up cadences with zero cycle
difference.

The authentic 12000-frame NEON/ARI64 replay retains main cycles `10029193005`,
H8 cycles `3396767298` and PCM frames `17691497`, with no H8 illegal/unsupported
or unmapped access. H8 retires `590883828` instructions. The final VS-BATTLE
ranking image matches the preceding capture byte-for-byte; the WAV differs,
so no whole-replay audio equivalence or MAME audio fidelity is claimed. Emulation
elapsed 58.545 s is a smoke observation, not an optimization benchmark. Frozen
binary SHA-256 is `ee36dc857bc088f829325dba60f8047c16527050d9d7edbb9d92904e4466e93b`.

Evidence lives in ignored `artifacts/validation/h8-rx-clock-20261005/`:
`before.json`, `after.json`, `core-final.log`, `mac-gates.json`,
`vblank-comparison.json`, and the frozen binary/input/log/PPM/PNG/WAV.
Installed MAME 0.287 has not been tied to the pinned source binary identity.

## Production System12 SCI1/RTC transfer gate, 2026-10-05

`--system12-trace-shared-writes` opts into passive H8 shared-store observations
inside `--system12-edge-trace FILE`. CSV rows include the charged H8 access cycle,
address and width; word stores emit one 16-bit event while preserving the former
two-byte diagnostic write count. Both tracing and nontracing native executions
retain identical board reports and final images. The gate separately exercises
an atomic `beef` word store and the RTC firmware's byte stores.

MAME's real H8 executes the same firmware as the Mac production board: select
RTC with port A.0/B.5, set SCI1 synchronous RX-only with BRR=7, poll RDRF, read
RDR, and clear RDRF through SSR after each byte. Both use the native fixed epoch
Sunday 2026-10-04 12:00:00, raw registers `00 00 12 07 04 10 26`. MAME seeds the
actual RTC save item, with readback, while retaining its real SCI/RTC pin wiring.
The parent ROM archive is unchanged and the capture exits after eight stores
under the existing watchdog, before a one-second RTC tick.

The SCI receives `00 00 12 47 00 61 02 00`: the RTC emits 52 LSB-first bits,
omits the weekday high nibble, and holds final DATA for the remaining clocks.
MAME shared-store cycles are `328, 590, 840, 1102, 1352, 1614, 1864, 2126`.
All eight address/value/cycle observations match the Mac interpreter and ARI64
with short/long MIPS blocks and both frontend catch-up cadences: **64 exact
comparisons, zero differences**. MAME also records RTC bit/data/clock/CE state
at each store; native comparison currently covers stores, not those extra
states or every wire transition.

Evidence is `mame-rtc.json/log` and `native-rtc.json/log` in
`artifacts/validation/h8-rx-clock-20261005/`. Reproduction commands are in
[H8 port details](h8-port.md). Remaining work includes timed PSX/H8 shared
mailboxes and IRQ ordering, RTC calendar progression, JVS, global timer/reset
contexts, real-game MAME pixels/audio and native/libretro scheduler equivalence.
No new Vita run, worker or H8 DRC implementation is claimed; all ten driver
families still retain incomplete fidelity gates.

## Mac PSX/H8 mailbox boundary, 2026-10-05

The priority remains a Mac reference faithful to MAME before performance work.
The shared-RAM map has no source synchronization handler or H8-to-PSX mailbox
IRQ. The injected PSX firmware publishes `1122`, polls H8's response `3344`,
acknowledges it and parks. The real MAME H8 executes the identical poll/response/
sleep firmware used by the production Mac board. Both reset at time zero.

`--system12-trace-shared-accesses` adds passive reads and writes from both CPUs
to the edge CSV. H8 words produce one atomic 16-bit observation, preserving the
two-byte access counters. Repeated polls retain their first and last observation
around a change; sequence numbers preserve CPU execution order rather than
sorting overlapping CPU local timestamps. Both engines and catch-up policies
produce identical complete board reports and framebuffers with tracing on/off.

MAME's 100 MHz input becomes 25 MHz execution cycles, with one cycle charged
after each instruction. Native's 50 MHz counter and multiplier 200 charge two
counts per instruction: both execute at 40 ns/instruction. The interpreter
charges before its bus access and reports a 40 ns later initial bus phase;
ARI64's initial stores/loads match the source phase. No diagnostic timestamps
were shifted and no clock/multiplier was changed to disguise the difference.

| Bus observation, reset-relative | MAME | Mac interpreter, block | Mac ARI64, block | Mac interpreter, vblank only | Mac ARI64, vblank only |
| --- | ---: | ---: | ---: | ---: | ---: |
| PSX command store | 0.080 µs | 0.120 µs | 0.080 µs | 0.120 µs | 0.080 µs |
| H8 command read, cycle 14 | 0.826719576718 µs | same | same | same | same |
| H8 response store, cycle 34 | 2.007747543458 µs | same | same | same | same |
| PSX first response read | 16666.680 µs | 5.280 µs | 5.240 µs | 16715.200 µs | 16715.160 µs |
| PSX acknowledgement store | 16666.960 µs | 5.560 µs | 5.520 µs | 16715.480 µs | 16715.440 µs |

The fixture and observer controls pass, but the strict comparison **fails**:
interpreter 5/7 observations differ per policy, ARI64 3/7. Address/value/width
match; the differences are timestamps. The test exits nonzero and retains exact
attosecond deltas. This is a reproducible scheduler gap, not a passing whole-board
validation or a reason to halve the native CPU clock.

Source `schedule.cpp` runs PSX before H8 toward a common target bounded by the
maximum quantum and next global timer. In this cold no-SCI fixture, PSX runs
through its first quantum before H8 publishes the reply, so the PSX poll sees
the reply in its next slice. The base maximum is 1/60 s; JVS slots can override
it. CPU local-time flooring, cycle debt and timers adjusted while a CPU runs
also affect targets. SCI's `m_sync_timer` is a global timer, including after
internally clocked events, so a fixed 1/60 catch-up would not establish faithful
game scheduling. PSX root-counter timers also need their source contract:
counts use `owner.total_cycles()*2`, but scheduling uses the source's fixed
33.8688 MHz timer period even on the 100 MHz board. No scheduler change is
included in this milestone.

A separate RTC correction advances the weekday by all days consumed when
crossing a month, following `dirtc.cpp::advance_seconds`. Previously Jan 30 +
three days reached the right date with the wrong weekday. The new boundary
fixture fails against the preceding source and passes normally and under UBSan;
it includes leap day, two-digit year rollover, multiple months, and split versus
continuous advance. Existing production RTC transfer comparisons still pass
64/64, and the vblank observer still matches all 32 transition pairs.

Evidence is retained in ignored `artifacts/validation/system12-mailbox-20261005/`:
`mame-mailbox.json/log`, `native-mailbox.json/log`, `calendar.log`,
`native-rtc.json/log`, `vblank-comparison.json/log`, and device/audio gate logs.
The MAME capture identifies 0.287 with unknown build commit; its binary identity
has still not been tied to the pinned source revision. Reproduce with:

```sh
python3 tests/check_mame_system12_mailbox.py --output mame-mailbox.json --log mame-mailbox.log
python3 tests/check_system12_mailbox.py --harness build/mac-system12-mailbox-20261005/pcsx-harness --mame mame-mailbox.json --output native-mailbox.json
```

Next work is the shared CPU scheduler and global timer boundaries, retaining
the passing peripheral gates, then real-game MAME state/pixel/audio comparison.
Workers and H8 DRC remain studies. Vita NEON/DRC and vitaGL validation follow
the Mac reference; no additional Vita run or completed driver family is claimed.

## SCI global timers and PSX instruction costs (2026-10-05)

MAME's SCI `sync_tick` callback is empty: its purpose is to create a global
scheduling boundary. The timer is independent of the H8 internal `clock_event`.
Stopping SCI or resetting its device clears the internal clock state but does
not cancel an already armed global one-shot. Internal-clock and simulated
external-rate modes arm/rearm it; physical external clock edges do not create
that timer.

The H8 port now retains each SCI global timer's absolute attotime expiry and
generation, with query/earliest-event/fire APIs. A stale generation cannot
consume a replacement timer. Firing consumes only the empty callback and sets
expiry to MAME's exact `never` value, without advancing the CPU or SCI. Soft
reset retains pending timers; cold reset invalidates them. Explicitly timestamped
RX/CLK pin entry points provide a scoped, nestable scheduler callback context;
an executing H8 still uses its charged CPU local time.

The actual MAME capture injects/readbacks identical H8 firmware into SCI0 and
SCI1, with either running or explicitly stopped synchronous receive clocks.
It discovers global timer save-item groups using the source layout and a unique
firmware-induced finite one-shot; no fixed global item index is used. Named
SCI `m_clock_event` observations remain separate. All four cases reset once at
time zero and leave the parent ROM archive unchanged. Each compares 96 stores:
**384/384 exact index/cycle/armed/expiry/internal-deadline states**, including
inactive `never` expiry. MAME timer start time is retained as diagnostic evidence
but is not part of the native comparison.

This gate uses an isolated H8 host that consumes due empty callbacks in
one-cycle steps. It does not implement production PSX/H8 arbitration, CPU slice
aborts when the global head changes, or complete machine-reset scheduling.
Stop behavior has actual MAME evidence; pending soft-reset and nested external
callback contexts have normal/UBSan contract tests. The strict production
mailbox comparison remains red with its previous differences.

A second firmware probe runs the real MAME PSX with CU2 enabled and an inert
H8, observing eight 16-bit shared-RAM stores around MULT/MFLO, DIV/MFHI/MFLO,
RTPS and a taken BEQ with a delay slot. The branch skips the fallthrough opcode.
Main ROM words are read back, reset is at time zero, and both native engines
preserve the full board report and PPM with the passive observer enabled or
disabled. Intervals include the separating marker store, so these are measured
firmware intervals rather than standalone instruction latency claims:

| Marker interval | MAME | Mac interpreter | Mac ARI64 |
| --- | ---: | ---: | ---: |
| MULT + MFLO | 120 ns | 220 ns | 300 ns |
| DIV + MFHI + MFLO | 160 ns | 860 ns | 860 ns |
| COP2 RTPS, CU2 enabled | 80 ns | 80 ns | 80 ns |
| Taken BEQ + delay slot | 120 ns | 120 ns | 120 ns |

The exact reset-relative comparison fails for **8/8 interpreter markers and
7/8 ARI64 markers**. ARI64's first marker matches at 600 ns; the interpreter's
first marker is 640 ns. Later matching interval lengths retain earlier timing
displacement. This confirms that the main clock frequency alone is not the
problem. The source-compatible path must reconcile instruction costs and bus
phase as well as global scheduling, while preserving the regular console path.

Current PSX `ExecuteBlock` is not a deadline primitive: interpreter branches
execute their delay slot before returning, and ARI64 can perform MMIO beyond a
requested `next_interupt`. The next implementation should first provide a
source-mode interpreter instruction step with resumable branch/delay-slot
state and access time at instruction start. Its context must avoid changing
the assembly/savestate layout of `psxRegisters`. Deadline checks in ARI64 and
the shared frontend coordinator remain separate required work. No worker,
affinity or H8 DRC change is included.

Evidence: ignored `artifacts/validation/h8-sync-timer-20261005/`, including
`mame-timers.json/log`, `native-timers.json/log`, `mame-psx-timing.json/log`,
`native-psx-timing.json/log`, and regression logs. MAME executable hashes are
recorded; installed 0.287 still has unverified correspondence to the pinned
source commit. The new stage retains all 1440 peripheral comparisons, 64
production RTC transfers, 32 vblank transition pairs, timed C352 tests in both
engines, and the broad device/tool suite (108 Python tests). The Vita3K timeout
messages in that suite come from mocked watchdog tests, not a new emulator run.
The full target remains 491 registrations in ten families, with no family yet
validated complete.

```sh
python3 tests/check_mame_h8_sync_timers.py --output mame-timers.json --log mame-timers.log
make -f tests/Makefile.h8 sci-sync-timer-fixture
python3 tests/check_h8_sync_timers.py --mame mame-timers.json --native build/h8-tests/h8-sync-timer-fixture --output native-timers.json
python3 tests/check_mame_psx_instruction_timing.py --output mame-psx-timing.json --log mame-psx-timing.log
python3 tests/check_system12_psx_instruction_timing.py --harness build/mac-system12-mailbox-20261005/pcsx-harness --mame mame-psx-timing.json --output native-psx-timing.json --log native-psx-timing.log
```

The last command deliberately exits nonzero while exact timing differs.

## PSX source instruction step and MAME timeslice order (2026-10-09)

`intExecuteSourceStep()` is an opt-in interpreter step that follows the MAME
`psxcpu_device` model: exactly one instruction per call, one 40 ns MAME cycle
(two 50 MHz native cycles) per instruction, and memory accesses timestamped at
instruction start (the cost is charged after execution). A branch and its delay
slot are separate steps; the pending branch is kept in interpreter-private
state, so `psxRegisters`' assembly/savestate layout is unchanged, and reset and
state load clear it. A branch in a taken delay slot is relative to the outer
target, links write through the load-delay cancel path, and a misaligned jump
target faults at its fetch with EPC equal to that address, as in MAME. Events
and interrupts are tested after every instruction except between a branch and
its slot. The harness flag `--system12-source-step` selects it with precise
load/store exceptions and without MULT/DIV/GTE stalls, which MAME does not
charge. Every step yields to the board coordinator. Regular interpreter and
ARI64 paths are unchanged. Interrupt entry is not yet compared: MAME executes
one more instruction before taking `EXC_INT` from `update_cop0`.

| Gate | Result |
| --- | --- |
| Instruction timing markers (MULT, DIV, RTPS, BEQ) | **8/8 exact** reset-relative times (was 8/8 interpreter, 7/8 ARI64 different) |
| MAME step cases: taken/not-taken branch, load delay, SYSCALL in taken/not-taken slot, branch in slot | **6/6 exact**, 18 stores including Cause `c0000020`/`80000020` and EPC |
| Default interpreter/ARI64 timing and mailbox observations | unchanged from 2026-10-05 |
| PSX/H8 mailbox, source step | **4/7** (was 5/7 interpreter, 3/7 ARI64 different) |

The step oracle template previously substituted its placeholders into Lua
identifiers and its reset parser failed; both are fixed. Its exception vector
now places a NOP after each MFC0, since MFC0 has a load delay and the original
stores captured stale registers instead of Cause/EPC.

The three remaining mailbox differences are not instruction timing. In MAME
the PSX polls shared RAM continuously until 16 666 520 ns, before the H8 runs
at all, although the H8 response is stamped at 2 007.748 ns. MAME's scheduler
(`device_scheduler::timeslice`) runs each CPU in order to a common target equal
to `min(basetime + quantum, next global timer)`. The default quantum is 1/60 s
and no System 12 interleave is configured, so the PSX sees the response only in
its next slice, at 16 666 680 ns. Per-instruction catch-up is therefore
*more* interleaved than MAME. Diagnostic confirmation: source step with
`--system12-vblank-sync-only` reproduces the same one-slice shape, offset by
the native vblank boundary (16 715 160 ns versus 16 666 680 ns).

Next work: a coordinator that reproduces MAME timeslice targets, the
"move target up" rule when a CPU stops short, and the global timer list (PSX
root counters, screen, DMA, SCI sync timers, etc.) as slice boundaries. That
also defines how large native slices may be, which bounds performance work.

Soul Calibur (`artifacts/system12`, 300 frames, Mac): ARI64 5.04 s, interpreter
4.99 s, source step 10.28 s, with identical H8 instruction and shared-access
counts. Evidence: ignored `artifacts/validation/psx-source-step-20261009/`.

```sh
make -f Makefile.mac-harness BUILD=build/mac-psx-source-step-20261009 DRC=1 SYSTEM12_TIMING=1 -j8
python3 tests/check_system12_psx_instruction_timing.py --harness build/mac-psx-source-step-20261009/pcsx-harness --mame mame-psx-timing.json --output native-source.json --log native-source.log --source-step
python3 tests/check_mame_psx_step.py --output mame-step.json --log mame-step.log
python3 tests/check_system12_psx_step.py --harness build/mac-psx-source-step-20261009/pcsx-harness --mame mame-step.json --output native-step.json
python3 tests/check_system12_mailbox.py --harness build/mac-psx-source-step-20261009/pcsx-harness --mame mame-mailbox.json --output native-mailbox.json --source-step
```

The mailbox command exited nonzero until the slice coordinator below.

### MAME timeslice coordinator: mailbox exact (2026-10-09)

The source step profile now drives the board through
`system12_slice_begin()`/`system12_slice_end()` instead of catching the H8 up
after every instruction. Each slice targets `min(base + 1/60 s, next global
timer)`, with the quantum equal to MAME `attotime::from_hz(60)`
(16 666 666 666 666 666 as). Global timers currently are the source screen
VBLANK start/end deadlines and the H8 SCI sync timers. The main CPU runs
`floor((target - now) / 40 ns)` instructions; when it stops short, the target
moves to its local time but not before the base, as in
`device_scheduler::timeslice`. The H8 then runs to the target through the
existing truncated-period attotime conversion, RTC/EEPROM/C352 catch up, the
base advances, and due SCI sync timers retire (their callbacks are empty).
Other System12 profiles are unchanged.

**The PSX/H8 mailbox now matches MAME 7/7**, including the main CPU's last
zero poll at 16 666 520 ns and the response at 16 666 680 ns. Instruction
timing 8/8, step cases 6/6, production RTC 8/8, timed C352, vblank fixtures
and 108 Python tests pass; default interpreter/ARI64 mailbox observations are
identical to 2026-10-05.

Known gaps: PSX-side MAME timers (root counters, DMA, SIO, CD) are not yet
slice boundaries. A screen timer between two 40 ns main instruction starts is
delivered by the core after the next main instruction, while MAME fires it
first; this affects PSX interrupt phase, not the H8 deadline.

Soul Calibur, 300 frames: source step with slices 3.00 s (per-instruction
H8 catch-up was 10.28 s), with the same H8 instruction and shared-access
counts.

```sh
python3 tests/check_system12_mailbox.py --harness build/mac-psx-source-step-20261009/pcsx-harness --mame mame-mailbox.json --output native-mailbox.json --source-step
```

## Real libretro core on macOS and exact H8/C352 work reductions (2026-10-09)

Mac measurements now use the libretro core itself (`frontend/libretro.c`, the
code the Vita runs) through `tools/libretro-host/s12_libretro_host.c`: a
headless host with the Vita standalone core options and replay semantics, no
display, vsync or audio pacing. `tools/libretro-host/build_mac_core.sh` builds
the core in an isolated copy with ari64, GPU NEON, `SYSTEM12_PROFILE=1` and
`NDRC_THREAD=0`, as on the Vita. With the DRC compiler thread enabled, two
identical macOS runs already diverged by 300 frames; without it, runs are
byte-identical. The Mac harness also gained `--frontend-loop` (whole
`psxCpu->Execute` frames, board advanced only by core events); its default
per-block board sync is not what libretro does and inflated measured sync cost.

Workload: the audited 12,000-frame, 177-event gameplay replay on
`artifacts/system12-profile` (Soul Calibur play, final frame "YOU LOSE!!").
Apple M1 Pro, minimum of three runs, profiler scopes in microseconds:

| | Baseline | After |
| --- | ---: | ---: |
| Total elapsed | 26.44 s | 12.79 s |
| H8 | 19.35 s (73%) | 5.89 s |
| C352 | 2.03 s | 1.87 s |
| GPU lists (sampled) | 3.45 s | 3.42 s |

The final PPM, 88.2 kHz WAV and full System12 report are byte-identical to the
baseline, including H8 cycles 3,396,724,964 and instructions 593,033,367. The
Mac harness per-block path (62.2 s to 53.2 s) also keeps its PPM and report.

Changes, all exact:

- **H8 idle-loop skip.** 442.6 M of 592.3 M H8 instructions were the two at
  0x16ca/0x16d0: `mov.b @0x084050,r0l; bne` waiting for a shared-RAM flag the
  H8 set itself. During one `h8_run_until` call the main CPU is stopped, so
  only the H8 can change that byte. `h8_run_until` measures an iteration (two
  equal consecutive deltas) and fast-forwards whole iterations strictly before
  the next internal event and the deadline, with no IRQ pending, and below one
  second of cycles so local attotime advances exactly as per-instruction ticks.
  New optional `H8Config.peek8`/`account_reads` declare side-effect-free memory
  and keep diagnostic read counts; System 12 supplies them for code ROM and
  shared RAM, and disables them while H8 loads are traced.
- **H8 per-instruction bookkeeping.** Timer16 keeps its earliest deadline
  current on every `event_time` change; `update_irq_filter()` recomputes only
  when pending requests, IER/ISR/ICR/SYSCR or CCR I/UI change;
  `H8CycleClock::advance()` adds `cycles * period` for under one second
  without the 64-bit division, which is a library call on the Cortex-A9.
- **C352 idle voices.** Idle voices output zero and are skipped. Play averages
  18.21 busy of 32 voices. VitaSDK `-O3 -mcpu=cortex-a9` code drops from 27
  instructions (5 loads, 4 multiplies) to 7 per idle voice, about 24 M
  instructions per emulated second; on the M1 the scope falls only 7%.

Validation: `tests/h8_spin_skip.cpp` runs a spinning program with IRQ edges, ADC
conversions and runs over one second, with and without the skip, and compares
full CPU state, memory and read counts after each of 4,000 deadlines (also
UBSan). The H8 suite, IRQ filter, deadline and sync-timer UBSan tests, C352 and
C352 timing tests, all MAME gates above (source-step timing, steps, mailbox,
RTC, audio, vblank) and 108 Python tests pass; default mailbox observations
are unchanged.

Vita3K functional run (`check_system12_gameplay_vita3k.py --gameplay-replay`,
NEON VPK `73887bae…`): console smoke PASS, 12,000 frames, 177/177 events, no
unmapped accesses or unsupported H8 instructions, gameplay screenshots. Guest
elapsed 257.5 s (46.6 fps average; the 6,000-frame 2026-10-05 replay measured
32.6 fps). Vita3K translates ARMv7 on the host, so neither figure predicts the
Cortex-A9; a physical Vita measurement is still required.

Vita3K profiler VPK, 6,000-frame replay (same scopes as the 2026-10-05 table):

| Scope | 2026-10-05 | 2026-10-09 |
| --- | ---: | ---: |
| Guest elapsed | 188.9 s | 124.7 s |
| H8, inside CPU | 89.8 s | 37.2 s |
| GPU lists, sampled estimate | 21.7 s | 14.5 s |
| C352, inside CPU | 6.9 s | 5.1 s |
| Vblank wait, inside video | 45.7 s | 36.3 s |
| Audio queue wait, inside board audio | 7.0 s | 36.4 s |

Excluding both waits, work is about 52 s, 8.7 ms per frame. The 48 fps in
Vita3K is pacing: one thread blocks on the audio queue and again on vblank.
Vita3K's CPU speed is not the Cortex-A9's, so this does not bound the
physical Vita either way, but the double wait is a frontend design issue
independent of emulation cost (see "Current cost and architecture
recommendation", point 3).

### Recorded option: H8 DRC

After the skip, the H8 still executes 150 M instructions per 200 emulated
seconds (0.75 M/s) at about 39 ns each on the M1: generated MAME states with
per-bus-operation deadline checks, peripheral deadline checks and IRQ
filtering on every instruction. No other loop dominates (the hottest remaining
routines run ~64 times per frame). A DRC is the remaining large reduction for
this CPU. Decide on it with a physical Vita profile: if the H8 scope stays
above about a quarter of frame time once video pacing is excluded, a DRC that
keeps `h8_run_until` deadlines, bus-operation checkpoints and IRQ boundaries
is worth its cost; otherwise GPU and C352 come first.

```sh
tools/libretro-host/build_mac_core.sh build/mac-libretro
build/mac-libretro/s12-libretro-host --core build/mac-libretro/src/pcsx_rearmed_libretro.dylib \
  --system12 artifacts/system12-profile --frames 12000 --inputs replay-12000.csv \
  --capture-ppm final.ppm --wav final.wav --report-system12 final.txt
make -f tests/Makefile.h8 test spin-skip-ubsan-test
```

Evidence: ignored `artifacts/validation/system12-perf-20261009/`.

### Vita presentation: one pacing clock (2026-10-09)

The NEON standalone frontend queued each frame with `SETBUF_NEXTFRAME` and then
always blocked in `sceDisplayWaitVblankStart()`, while `audio_batch_cb` blocks
whenever more than 2,048 source frames are queued. With both on one thread, a
frame whose audio wait carried it past a vblank cost two vblanks. The frontend
now calls `sceDisplayWaitSetFrameBuf()` just before writing the back buffer:
it returns at once when a vblank has passed since the previous queue, so it
only waits when two frames would be presented within one vblank. The audio
queue is the only pacing clock. The vitaGL path already swaps without vblank
waits.

Vita3K, profiler VPK, same 6,000-frame replay:

| Scope | Before | After | Diagnostic: no audio backpressure |
| --- | ---: | ---: | ---: |
| Guest elapsed | 124.7 s | 119.1 s | 84.0 s (71 fps) |
| Vblank wait | 36.3 s | 4.1 s | 29.6 s |
| Audio queue wait | 36.4 s | 37.9 s | 0.3 s |

Vita3K drains audio at about 88% of real time: the replay generates 100.3 s of
audio, takes 119 s, and still waits 38 s for the queue. Without backpressure
(diagnostic build only, not committed) the same guest runs faster than real
time. The remaining Vita3K cap is therefore its audio emulation; on hardware,
`sceAudioOutOutput` drains in real time. Vita3K compute scopes varied by about
30% between identical-core runs, so they are not used as costs here.

Normal VPK, 12,000-frame replay: console smoke PASS, 177/177 events, no
unmapped accesses, H8 report identical to the previous Vita3K run (same
instructions 593,505,145), guest elapsed 257.5 s to 215.7 s (46.6 to 55.6 fps).
A physical Vita must confirm pacing and cost; Vita3K establishes behavior only.

### Single-thread budget and H8 interpreter overhead (2026-10-09)

Without a physical Vita, cost is tracked as retired AArch64 instructions on
the M1 (stable, unlike wall time). The gameplay half of the replay (frames
6,000-12,000) cost 16.0 M instructions per frame before this work. A
Cortex-A9 at 444 MHz (dual-issue, limited out-of-order) at 0.8-1.2
instructions per cycle has room for about 6-9 M ARMv7 instructions per frame
at 60 fps; the ARMv7 count of the same work is likely somewhat higher.

An isolated H8 loop (`bench/h8_bench.cpp` in the evidence) cost 485 host
instructions per emulated H8 instruction. Exact changes, each re-verified
against the replay's byte-identical PPM/WAV/report:

| Change | Host instr. per H8 instr. |
| --- | ---: |
| Baseline | 485 |
| Direct code ROM reads (`H8Config.rom`) | 345 |
| Inline timer deadline/tick, IRQ refresh once per run | 289 |
| Cached next event, lazy attotime, IRQ input flag | 252 |
| Inline IRQ check in `prefetch_done`, out-of-line slow fetch | 240 |
| `run_batch`: states back to back, one commit per bound | 190 |
| First-level table dispatch generated from `do_exec_full()` | 169 |
| Forced inline code fetch | 148 |

`run_batch` keeps `m_icount` accumulating from the committed cycle, so
checkpoints, `h8_current_cycle()`, callback times and the event at the bound
are unchanged; the bound is recomputed at a state boundary after peripheral
state changed, a clock cap keeps lazy attotime exact, and possible spin-loop
heads leave the batch for the single-state path. `h8_generate.py` emits
`h8_dispatch.inc` from the same generated switch, so table and switch agree
by construction.

Gameplay half now: **11.39 M instructions per frame** (8.83 M for the first
half). Time shares in that half: GPU lists 38%, H8 31%, C352 21%, rest 10%.
Repeating each C352 block on a copy attributes exactly 1.4 M instructions per
frame to C352 (about 54 per busy voice and sample). A voice-major C352 mixer
produced identical audio but 1% fewer instructions and 3% more time in an
isolated benchmark, so it was not kept.

Validation: H8 unit/UBSan suites, MAME H8 peripherals (1,440 comparisons, 0
differences), SCI sync timers, all System 12 MAME gates, 108 Python tests,
the Mac harness per-block replay (PPM/report identical) and a Vita3K 12,000-
frame replay whose H8 report matches the previous VPK exactly.

The remaining single-thread gap is mostly PSX DRC plus GPU, which is the
same work pcsx_rearmed performs for console games, and the ARMv7/A9 factor,
which only a physical Vita can measure. Further single-thread options: the
ARMv7 NEON assembly GPU renderer (`GPU_NEON_USE_C=0`, needs hardware to
measure), NEON mixing of several C352 voices, and the remaining H8 bodies.

## ARMv7 DRC equal to ARM64: game clock fix (2026-10-09)

In Vita3K the Soul Calibur round clock advanced 6.1-6.4 game seconds per 300
emulated frames; the Mac (DRC or interpreter) and the Vita interpreter build
advanced 5.00. The emulated frame and cycle counts were identical, so the
ARMv7 DRC computed different values, not more cycles.

`tools/libretro-host/build_armv7_core.sh` builds the libretro core for Linux
ARMv7 (Cortex-A9 flags, the ari64 ARM32 backend, `GPU_NEON_USE_C=1` like the
Vita) and runs it in Docker through qemu. It reproduced the fast clock
without Vita3K, so the fault was ARM32 code, not the emulator. The host's new
`--hash-ram FILE` writes one FNV-1a hash of main RAM per frame (the BIOS
kernel's first 64 KiB are excluded: exception handlers save dead registers
that the dynarec leaves stale, and those values differ between backends
without effect). The first differing frame plus traces of `gen_interupt`,
board I/O, GPU DMA and GPUSTAT reads located two causes:

1. ARM32-only root counter readers. With `__arm__`, `pcsxmem.c` maps root
   counter reads to assembly fast paths. `rcnt1_read_count_m1` (hblank mode)
   divides by the NTSC console line period (multiplier `0x1e6cde`, about 2154
   cycles). System 12 lines are `frameCycles() / hsyncTotal()`, about 3178
   cycles at 50 MHz, which the C reader uses. The game measures frame time in
   hblanks, so on ARMv7 it saw 1.475 times more lines (stack counters were
   0x917 against 0x62a) and advanced its clock faster. `rcnt0_read_count_m1`
   approximates /5 and also differs from C. Under arcade source timing both
   modes now map to the exact C readers. Console behavior is unchanged.
2. Register-dependent stall timing. `multdiv_do_stall` and
   `cop2_do_stall_check` skipped the MULT/DIV or GTE stall when no free host
   register remained. ARMv7 has fewer registers: in 12,000 frames it skipped 5
   MULT/DIV and 7 GTE stalls that ARM64 counted (for example 42 cycles in the
   block at 0x800ad0fc). The new `emit_busy_stall()` uses only `HOST_TEMPREG`
   (`d = busy - adj - cc; if (d >= limit) d = 0; cc += d`). That is the same
   arithmetic, now on every backend.

Validation, replay of 12,000 frames:

| Check | Result |
| --- | --- |
| ARMv7 (qemu) and ARM64 RAM hash, per frame | identical in all 12,000 frames |
| ARMv7 and ARM64 final PPM and WAV | byte-identical |
| ARM64 PPM, WAV and System 12 report against the previous reference | byte-identical |
| Vita3K VPK, clock at frames 3000/3300/3600 | 04"34, 09"34, 14"34 (+5.00 s per 300 frames) |
| Vita3K frames 3000/3300/3600 against the Mac ARM64 core | equal at 15-bit color (the hosts' PPM writers expand 5 to 8 bits differently) |

Before the fix, the first differing frame was 385. With only the root counter
fix it was 1741, and that case was the stall difference. Evidence:
`artifacts/validation/armv7-clock-20261009/`.

The ARMv7 GTE (`gte_arm`/`gte_neon`) and the ARMv7 inline readers did not
cause differences in this replay. The per-frame hash comparison with
`build_armv7_core.sh` is the check to repeat after ARM32 DRC or assembly
changes.

## Audio against MAME 0.287, and limits of MAME as reference (2026-10-09)

First whole-game audio comparison. MAME 0.287 (`soulclbrac`, empty NVRAM,
`-samplerate 88200 -wavwrite`) and the libretro core recorded the first
1,800 frames (30.09 s) without input.

| Result | Value |
| --- | --- |
| Before the fix | identical up to 0.507 s, then clicks of 1-3 samples (L/R off by 3008/3136) |
| C352 interpolation fix | identical up to 1.781 s; left channel identical up to 20 s |
| Right channel after 1.78 s | 49,000 samples differ by 1-3 (inaudible) |
| C352 register writes in 400 frames | 10,376 in both, same order and data; MAME often writes 228-240 H8 cycles later |

The fix: MAME interpolates with a `u32` counter product stored in `s16`; the
port used a signed product that overflows for large steps and kept 32 bits,
so samples moved by 65,536 times the volume. In the 200 s replay, audio
changed in 177 seconds and the peak fell from 5398 to 4089. The video is
unchanged.

The remaining differences come from game and H8 timing, not the C352:
the right-channel 1-3 LSB differences follow the later register writes, and
from about 20 s the streams differ because MAME's first-boot sequence is
about 200-400 frames shorter ("FIRST POWER ON" at frame 600 in MAME, about
800 here and longer; see `boot-mame-vs-ours.png`). That backup-memory
initialisation timing (EEPROM/RTC) is open.

MAME is not a reliable reference for two System 12 timings:

- Root counter 1 in hblank mode divides by a fixed 2150 CPU cycles
  (`psx/rcnt.cpp`), the console line period. At 50 MHz, System 12 lines
  last about 3178 cycles (15.73 kHz from the GPU), so MAME counts lines 1.48
  times too fast. This is the same fault as the ARMv7 assembly reader fixed
  above, and it explains MAME's round clock (+6.8 s per 300 frames against
  +5.0 s here and in real time).
- The C352 rate, 25.4016 MHz / 288 = 88.2 kHz, is copied from System 11.
  There, MAME notes that it matches the pitch of a real Tekken PCB although
  the measured C352 clock is 20.0132 MHz. MAME's own System 12 hardware notes
  give a 29.168 MHz C352 clock. The System 12 output rate, and therefore
  pitch, is unverified.

The C352 implementation itself (superctr, also used by libvgm) is the best
available model and now matches bit for bit where the game streams agree. A
direct-feed recording of a real System 12 PCB is the only better reference
for pitch and boot timing. Evidence: `artifacts/validation/c352-mame-20261009/`.

## NEON assembly renderer for console and arcade (2026-10-09)

The ARMv7 assembly renderer (`psx_gpu_arm_neon.S`) was blocked in arcade
builds, so the Vita used the C renderer for everything. One generic build now
runs console games (512 VRAM rows) and System 12 (1024 rows) with the
assembly, and the console-only build is unchanged.

What changed for `PCSX_SYSTEM12`:

- `psx_gpu_offsets_update.c` writes `psx_gpu_offsets_system12.h` for the
  arcade layout (64 texture pages, 64-bit page masks).
- In arcade, the five masks sit after the bitfields. Earlier, they pushed
  16-bit fields beyond the 8-bit offsets of `ldrh`/`ldrsh`. `blocks` stays
  at 0x200 (one immediate, 64-byte aligned).
- After each dirty-page `tst`, `test_texture_mask_high` tests the high words
  when the low words are clean (three conditional instructions; empty on
  console). `update_texture_4bpp_cache` clears both words.
- Model 1 interleaved textures (TPAGE bit 13) go through
  `update_texture_4bpp_cache_type1()`, the C path the C renderer uses.
- `Makefile.vita` uses the assembly by default (`VITA_GPU_NEON_C=1` selects
  C). `build_armv7_core.sh` does the same.

Validation (ARMv7 in Docker/qemu unless noted):

| Check | Result |
| --- | --- |
| Console `psx_gpu_arm_neon.o`, before and after | byte-identical |
| Soul Calibur replay, 12,000 frames: assembly against the ARM64 C renderer | RAM hash per frame, video hash per frame (9,793 distinct images), final PPM and WAV identical |
| Crash Bandicoot, 5,000 frames with input into N. Sanity Beach: generic build against console-only build | RAM and video hash identical in every frame (723 distinct images) |
| Type-1 selftests (512 and 1024 rows), ARMv7 harness with assembly | PASS; with the C helper disabled they FAIL, so the path is exercised |
| ARM64 Mac core after the layout change | PPM, WAV and report identical to the reference |
| Vita3K VPK, replay frames 3000/3300/3600 | passed; equal to the Mac frames at 15-bit color |

Cost for console games in the generic build: three conditional
instructions per texture-cache check, a runtime VRAM height and 4 MiB more
texture cache, which the generic build already had with the C renderer. The
speed gain of the assembly needs a physical Vita to measure; qemu timing is
not representative of NEON cost. Evidence:
`artifacts/validation/neon-asm-arcade-20261009/`.

## Vita hardware: threads and GXM presentation (2026-10-09)

Soul Calibur replay on a physical Vita (192.168.0.25), profile builds,
gameplay frames 3000-4800, NEON assembly renderer unless noted:

| Configuration | fps |
| --- | --- |
| Sequential, CPU present (start of the day) | 15.0 |
| Faster CPU present (NEON row conversion, row dedup) | 16.2 |
| GPU thread + board thread (H8 and C352) | 32.1 |
| + framebuffers in main memory | 35.5 |
| Same, C renderer instead of the assembly | 25.1 |
| GXM presentation (`--gxm`, now the default) | 47.3 |

- Board thread (`SYSTEM12_BOARD_THREAD`, core option
  `pcsx_rearmed_system12_board_thread`): the H8 and C352 run on core 2,
  driven by screen events, with an SPSC audio ring. macOS replays are
  identical with three jitter seeds and ThreadSanitizer clean; ARMv7 and the
  Vita report are identical too.
- GXM presentation: the PSX 15-bit pixel is GXM's `X1U5U5U5_1BGR`, so the
  displayed VRAM rows are copied as they are into an uncached texture and the
  GPU scales them (245 KB per frame instead of 1.6 MB converted by the CPU).
- vita-tracy (PC sampling) before GXM: no core saturated, the threads wait on
  each other; the present was half the main thread; the profiler cost ~19% of
  the board thread; `c352_generate` was 38% of the board thread.

Per-thread cost at 35.5 fps (profile build, ms per frame): main 15.7 (CPU)
+ present, GPU list 21.0, board 19.9 (H8 12.9, C352 6.8). With GXM the
present is ~1 ms, so the frame is bound by the GPU thread and the board
thread together: ~58 ms of work per frame against 50 ms for three cores at
60 fps. Reaching 60 fps needs less work, not only better distribution
(non-profile builds, then C352 and the renderer below).

## vitaGL renderer crash (2026-10-09)

The vitaGL (`gpu_vita_ctr`) build crashed between frames 1700 and 2700 of
the replay: a data abort in the "CTR Renderer" thread inside SceLibKernel's
mspace `malloc`, reached from vitaGL's `glTexImage2D` upload path. The
registers held `0xffefefef`, a PSX colour expanded by the renderer's VRAM
shader (5-bit 29 -> 0xEF), on the heap's metadata.

Cause: vitaGL frees a texture's old store at once when nothing *sampled* it
in the last `FRAME_PURGE_FREQ` frames; rendering into it does not count. The
CTR renderer resizes its render targets with `glTexImage2D` (235 times in
the attract mode), so the GPU kept drawing into freed memory that the next
upload reused. `tools/vita-harness/patches/vitagl-fbo-last-frame.patch`
marks an FBO's texture used when a scene begins on it; the replay then
passes 4800 frames on hardware. The vitaGL build runs ~55 fps in attract
but ~11.5 fps in gameplay (60 VRAM upload rectangles per swap, each a
`glTexImage2D` and a draw); not investigated further.

Also found on hardware: a launch right after another app aborted with
"trampoline banks are full" because the dynarec's VM block came back with
the previous app's data; the trampoline table is now cleared at init.

## GPU command replay and renderer profiling without the Vita (2026-10-09)

`gpulib` built with `-DGPULIB_TRACE` records, for
`GPULIB_TRACE_FILE=path:from:to`, a VRAM/register snapshot and every GPU
entry call of those emulated frames (DMA chains as the packets they walk)
plus the final VRAM hash. `tools/gpu-replay` replays a trace with only
gpulib and the NEON renderer:

- `build.sh mac|armv7`: macOS (SIMD C) or the ARMv7 container (assembly).
- `check.sh`: every `build/gpu-replay/*.gtr` on both, single- and
  two-stage, against a baseline of flip and VRAM hashes.
- `profile.sh TRACE LABEL`: callgrind with a Cortex-A9-like cache model;
  `cg_by_symbol.py` attributes the assembly (its symbols have no `.size`),
  `cg_hot_instr.py` lists one function's hot instructions.

Five Soul Calibur traces (frames 1200, 1800, 2400, 3000, 4200; 60 frames
each) replay to the emulator's exact VRAM. The callgrind ranking matches
vita-tracy's samples of the Vita GPU thread:

| Function | Vita samples | callgrind Ir |
| --- | --- | --- |
| `setup_blocks_shaded_textured_dithered_swizzled_indirect` | 19.6% | 16.6% |
| `texture_blocks_4bpp` | 17.5% | 11.5% |
| `shade_blocks_shaded_textured_modulated_dithered_direct` | 12.5% | 8.3% |
| `setup_blocks_unshaded_textured_dithered_swizzled_indirect` | 6.0% | 6.1% |
| triangle setup (prepare, gradients, spans) | ~12% | ~16% |

`texture_blocks_4bpp` costs more on the Vita than its instruction count
(texture-cache misses, which callgrind also sees). Soul Calibur draws small
gouraud-textured dithered 4bpp triangles: ~4.2 M instructions per frame,
~0.45 instructions per cycle on the Vita.

## NEON renderer: arcade overhead and a two-stage renderer (2026-10-09)

Arcade overhead removed (commit b7449e20, ~4% less estimated GPU thread
time; every replayed frame and the final VRAM unchanged):

- `gpu_get_model()` inline (constant 2 on console); the renderer called it
  9000 times a frame.
- `texture_region_mask` builds the 64-bit page set from 32-bit products;
  it no longer marks pages of the next band when a rectangle wraps in X
  (they are not touched). An exhaustive sweep matches a per-page reference
  and never marks fewer pages than before.
- Writes of up to 1024 halfwords into clean 4bpp pages patch the cached
  texels in place: Soul Calibur's palette animation (16x1 VRAM copies into a
  texture page every frame) made two 64 KB cache rebuilds a frame; now 17
  rebuilds per 60 frames, all first uses.

Two-stage renderer (commits 2a529992, a3a88df0; core option
`pcsx_rearmed_neon_two_stage`, Vita `--gpu2`, off by default): with
threaded rendering, full block buffers are textured, shaded and blended on
another thread while the GPU thread parses and sets up the next primitives.
A ring slot is `psx_gpu_struct` up to the end of `blocks[]`, so stage 2 runs
the handlers on the slot with the state its buffer was set up with. Stage 1
keeps the texture caches (rebuilt before queueing, as `texture_blocks`
would) and waits for stage 2 before anything that reads or writes VRAM the
queued buffers may write (the bounding box of their viewports): cache
rebuilds, textured sprites, lines, fills, simple untextured sprites and
every gpulib VRAM access (all through `renderer_flush_queues`).

Validation: all five traces give the single-threaded flip and VRAM hashes
on macOS and ARMv7, 30 repeats each on macOS, ThreadSanitizer clean; a
4800-frame emulation on macOS with the GPU thread and two stages gives the
same per-frame video, RAM and audio hashes as the sequential run. The first
version raced (lines and simple sprites write VRAM directly); TSan and the
repeats found it.

Expected split: stage 2 is ~32-37% of the GPU thread (callgrind and Vita
samples), so ~13-14 ms of the 21 ms would stay on the GPU thread. Per frame
in gameplay stage 1 queues ~1100 buffers (~23 blocks, ~2 KB each) and waits
for stage 2 about twice for lines/fills, twice at frame end, and ~7 times
because the ring is full. Its value on the Vita depends on a core having
room for stage 2 (see the budget above); not measured on hardware yet.

Assembly (`psx_gpu_arm_neon.S`) review, for hardware measurement:

- `setup_blocks_*_textured`: ~60 instructions per span before ~40 per block;
  Soul Calibur's spans have 1-2 blocks, so the span prologue dominates
  (five per-triangle block-span vectors reloaded per span, `uvrg_dx`
  recomputed). All 16 q registers are in use.
- `shade_blocks_*_direct` read the framebuffer line of every block to merge
  the draw mask (97% of that function's L1 misses in callgrind). Skipping the
  read for full blocks needs the NEON mask in an ARM register (~20-cycle
  stall on Cortex-A9). setup's `pld [fb_ptr]` covers it single-threaded
  (callgrind does not model `pld`); with two stages it ran on the other core,
  so stage 2 now prefetches each buffer's framebuffer lines.
- `texture_blocks_4bpp`: 8 byte loads from the texture cache per block,
  ~0.3 L1 misses per block.

## C352 voice-major mixing (2026-10-10)

`c352_generate` ran all 32 voices for each output frame (88,200 a second),
reloading each voice's state every time; it was 38% of the Vita board
thread. Calls of two frames or more now run one voice over the whole block
with its state in registers, then the next voice. Voices meet only in the
mix, an integer sum, so the order cannot change it. Two exceptions keep
MAME's order: single-frame calls, and blocks with a busy noise voice (the
noise generator is shared and advanced in voice order). Inside the block the
four channel volumes are packed in a word (a settled ramp costs one compare),
`fetch_sample`'s common case is inline, and on ARM the mix is one NEON
multiply by the phase-signed volumes.

`tools/c352-replay` replays a call trace (register writes and reads, resets,
generate calls with their sizes) recorded by `c352.c` built with
`-DC352_TRACE` (`C352_TRACE_FILE=path:frames`) against the same wave ROM, and
checks the output's FNV-1a. Soul Calibur's 4800-frame replay (7.06 M output
frames; 69% of them in 256-frame calls, 380,000 calls of 1-3 frames):

| ARMv7 (callgrind, chip only) | Instructions |
| --- | --- |
| Before | 6.46 G |
| Voice-major, scalar mix | 5.07 G (-22%) |
| Voice-major, NEON mix | 3.66 G (-43%) |

Output identical on macOS and ARMv7; full emulation identical in per-frame
video, RAM and audio, with and without the board thread. A randomized test
(`tests/test_c352.c`) runs the same voice programs frame by frame and in
blocks and compares output and registers; a wrong phase or step direction
makes it fail. Expected on the Vita: about 3 ms less per frame on the board
thread (C352 was 6.8 ms per frame in the profile build); not measured on
hardware yet.

## Vita measurements after the GPU and C352 changes (2026-10-10)

Same replay, frames 3000-4800, `--gpu-thread --board-thread`, GXM present:

| ms per frame | 2026-10-09 (GXM) | Now | + `--gpu2` | `--gpu2-core 2` | `--gpu2-core 0` |
| --- | --- | --- | --- | --- | --- |
| fps (profile build) | 47.3 | 49.3 | 40.0 | 30.3 | 33.3 |
| GPU thread (`gpu_list`) | 23.16 | 19.27 | 20.38 | 21.86 | 27.99 |
| main thread (`cpu_inclusive`) | 20.25 | 19.35 | 24.04 | 32.10 | 29.08 |
| H8 | 12.80 | 11.48 | 11.47 | 11.26 | 11.28 |
| C352 | 6.92 | 4.35 | 4.39 | 4.32 | 4.35 |

The non-profile build runs 50.5 fps in the same frames. The GPU-thread
changes gave 17% on hardware (callgrind estimated 4%: the avoided 64 KB cache
rebuilds cost more on the Vita than in the cache model); the C352 dropped
37%, as estimated. The reports are identical in every configuration.

The two-stage renderer loses on the Vita whatever the core: core 1 (GPU
thread) is the one near 100%, but cores 0 and 2 have too little room for
~7 ms of stage 2, and sharing a core makes stage 1 wait on the scheduler
around its ~1100 hand-offs a frame. It stays off; `--gpu2-core N` pins it
for experiments. To reach 60 fps the GPU thread has to drop below ~16.7 ms.

## Tekken 3 and Tekken Tag Tournament (2026-10-10)

Profiles prepared from the local MAME sets with `prepare_mame_psx.py` and
`prepare_system12_board.py` (pinned MAME source; Tekken Tag's protection is
unchanged in current MAME, only its API names moved). Tekken Tag needed its
protection (commit 4e30b4ce, port of MAME's `tektagt_state`).

| Set | macOS core (attract, no input) | Vita3K, NEON VPK, 5400 frames |
| --- | --- | --- |
| `tekken3` (World, TET2/VER.E1) | boots; attract demo fights from ~60 s | PASS, report and capture; 91 s |
| `tektagt` (World, TEG2/VER.C1) | boots; VS screen, time records, demo fight | PASS, report and capture; 91 s |

Both report no illegal H8 instruction, no unmapped access, and C352 output
(peak 5886 and 7590). `tools/vita-harness/run_system12_vita3k.py` runs any
prepared profile in an isolated Vita3K (it reuses the Soul Calibur runner's
isolation and packaging) and keeps the report, the final frame and Vita3K's
screenshots. Not yet run on the console.

## vitaGL renderer: HD presentation and CPU-side overhead (2026-10-10)

The vitaGL renderer (`gpu_vita_ctr`) drew into a multisampled target but
showed every frame from VRAM, packed to 15-bit PSX pixels at 512x240: no
host resolution and no antialiasing reached the screen. It now presents
the target that drew the displayed page (the previous scene's one for
double-buffered games, swapped, not copied), sized one target pixel per
presented pixel (725x517 for Soul Calibur's 512x228 draw area on the
Vita), with 4x MSAA. `--gl-1x` and `--gl-msaa 0|2|4` switch it back for
measurements. Only draw areas shaped like the display get the HD target:
Soul Calibur draws small areas off screen (its font among them) and reads
them back every frame, and packed from an HD target they changed the
game's course. With that, the Vita3K reports of the vitaGL build equal the
NEON build's for Soul Calibur (gameplay replay), Tekken 3 and Tekken Tag.
Fixed on the way: untextured flat polygons and tiles drew
the raw-texture bit's 128 grey (Tekken's shadows), Tekken Tag's ROMs did
not fit the 128 MB newlib heap next to vitaGL's shader translation, and
`--capture-ppm` now works for vitaGL builds (on hardware; Vita3K reads the
display buffer back black).

Vita3K runs vitaGL builds with `run_system12_vita3k.py --shader-compiler`
(it then switches surface sync on: without it every textured polygon
disappears, since VRAM is a render target the shaders sample). `--inputs`
replays a gameplay CSV and `--app-arg` passes guest options.

The hardware measurement before this work was ~11.5 fps in gameplay. The
Mac vitaGL-like harness now counts what costs on the Vita
(`PCSX_GL_STATS=1`, `PCSX_GL_STATS_SKIP=N`); `PCSX_GL_LOWDPI=1` gives it
the Vita's 960x544 back buffer. Soul Calibur gameplay replay
(`tests/check_system12_gameplay.py` inputs, presented frames after 2600):

| per presented frame | before | now |
| --- | --- | --- |
| GL draw calls | 763.9 | 131.6 |
| texture stores (`glTexImage2D`) | 7.50 | 3.83 |
| GXM scenes (render target switches) | 10.86 | 8.80 |
| frames made synchronous (whole replay, 4883 frames) | 4147 | 52 |
| synchronous renderer tasks (whole replay) | 39533 | 1486 |

- Draws: a depth batch drew each member split separately; members are now
  copied together into the frame's vertex buffer and drawn once.
- Synchronous frames: each one made the emulation thread wait for the
  renderer thread. CPU uploads are queued with the frame packet when no
  pending split uses their VRAM, or start a new packet of the same frame
  when one does; gpulib no longer syncs the renderer at every VRAM write
  DMA; CPU reads of VRAM the renderer never drew into return at once.
- Texture stores: the CPU rects of each VRAM update go up in one RG8
  texture (raw PSX pixels, no conversion) instead of one RGBA8 texture each.

Final captures are unchanged by every step except where the Mac harness
itself was fixed (it began a second scene in synchronous frames, reloading
the HD target at PSX resolution). Soul Calibur (attract and gameplay),
Tekken 3 and Tekken Tag run in Vita3K with the final VPK. Hardware fps not
measured yet (the console was locked).

## vitaGL renderer on hardware: crashes, clocks and render targets (2026-10-10)

The HD vitaGL build crashed on the Vita around frames 1800-2400 of the
gameplay replay, in several ways: physical memory exhausted (vitaGL leaked
each FBO's stencil surface on resize), heap metadata overwritten with a
pixel colour (a texture reallocated at the same address kept its old colour
surface size), and GPU lockups (watchdog reset, no MMU fault; decoded from
the dump's GPU_INFO note with `../vita_gpucrash.py`). All of them happened in
the renderer's render target churn: targets followed the game's draw areas
and were resized in place about 200 times in the replay.

Fixes: two vitaGL patches (stencil free, size-checked re-attach), 32 MB left
outside vitaGL's pool, render targets allocated in steps and swapped from a
pool instead of resized (28 allocations in the replay), and vitaGL pinned to
upstream dca4b9d. The app also never raised the clocks: the GPU ran at
111 MHz. With ARM 444-500 / bus 222 / GPU 222 / xbar 166 MHz:

| Soul Calibur gameplay replay, frames 3000-4800 | fps |
| --- | --- |
| vitaGL HD + 4x MSAA, before (GPU 111 MHz) | ~10 |
| vitaGL HD + 4x MSAA, clocks raised | ~17 |
| vitaGL HD + 4x MSAA, all fixes (3 runs, no crash) | 40.1 |
| NEON renderer, release, vblank pacing | 59.9 |

Every vitaGL run's report equals the NEON build's. Profiled before the fixes
(psp2-cex-gpues4-prof at 111 MHz): GPU-bound, ~105 ms of GPU time a frame, 7
scenes and 138 draws a frame, 67% of fragment time in the cached 4-bit
polygon shader (ALU-bound), which the texture-window and opaque variants
now trim. A profile with the fixes is the next step.

## H8 batch loop with the CPU state in registers (2026-10-12)

Soul Calibur's lake attract stage (frames ~6480-7680) executes ~15,000 H8
instructions a frame. In that window 106 handlers run; 30 cover 88% of the
instructions, 60 cover 99%. Data accesses are ~0.3 a instruction to shared
RAM/C352 (callbacks) and ~0.4 to internal RAM/registers. The interpreter, not
the callbacks, held the time: every field of `H8CPU` was loaded and stored
around each handler call, `prefetch_done()` filtered IRQs on every
instruction and a 32-bit ER took two 16-bit loads.

- `ERn` is one host-order u32 (`m_R[2n]`/`m_R[2n+1]`); 8/16/32-bit register
  accesses are single loads/stores. System 12 savestates move to version 2.
- `h8_generate.py` also writes `h8_fast.inc`: the same MAME full-state bodies,
  with their bus checkpoints and substate numbers, written on a local copy of
  the CPU state (`H8Fast`) inside one function with computed-goto dispatch.
  The copy goes back to `H8CPU` before bus accesses outside code ROM and
  internal RAM, the IRQ choice when its inputs may have changed, handlers it
  does not model (CCR loads, rte, trapa, sleep, eepmov, divides, IRQ/reset
  states, illegal encodings: the original handlers run them) and the exit. A
  checkpoint leaves the state the partial handlers resume.

Exactness: the 12,000-frame `--frontend-loop` attract and a 3,000-frame
sliced run keep PPM, WAV and System 12 report byte-identical; H8 tests pass
under clang, GCC 15 `-O3 -std=gnu++11` and UBSan; the MAME H8 oracle, peripheral
(SCI included), SCI timer and production RTC gates pass. The RTC gate needs a
`DRC=1 SYSTEM12_TIMING=1` harness: console timing shifts its store cycles by
12-24 H8 cycles, which the check now rejects up front.

| H8, attract | Before | After |
| --- | ---: | ---: |
| Mac M1, ns/instruction (frontend loop) | 20.1 | 15.3 |
| Vita, lake stage, b_h8 ms/frame | 7.86 | 7.08 |
| Vita, lake stage, ns/instruction | 518 | 467 |
| Vita, rest of attract, ns/instruction | 386 | 337 |
| Vita, lake stage fps (one run each) | 57.2 | 58.7 |

Executed H8 instructions per frame are equal on the Vita (15,164 in the lake
window). GCC still spills part of the copy to the stack in that large
function (the hottest body is ~75 ARM instructions, from 103 before the
helpers were forced inline); the remaining H8 options are lazy flags or a DRC.
