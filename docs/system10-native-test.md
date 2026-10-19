# Native System 10 adapter test

`tests/check_system10_harness.py` runs synthetic MEM(M) and MEM(N) packages
through the native ARM64 headless harness. It downloads no ROMs and makes no
claim that these fixtures boot any game. Each package is assembled with
`prepare_system10_board.package_regions` from the pinned System 10 registry,
using the real region names, sizes, board profiles, and metadata format.

The MEM(M) fixture encodes a small MIPS-I program into the raw region with the
`mrdrilr2` passive XOR/permutation. The guest selects NAND bank zero, reads
the source bank predictor register, and executes a helper from that dynamic
window through JALR, both with and without interpreter instruction caching.
The MEM(N) fixture encodes its reset program in
`chocovdr` NAND0 pages 0x40 onward, then programs the real NAND read command,
column/page address bytes, and PSX DMA channel 5 to copy 16 bytes from page
0x300 into upper RAM. Boot data respects the 512-byte data and 16-byte OOB
stride of each raw NAND page. A second MEM(N) program requests a transfer
past the RAM boundary and must fault before copying any bytes.

Both guests poll the base active-low IN1 port while the harness applies a
frame-indexed button press and release. They read and acknowledge the board's
immediate timer IRQ status, write and read an upper-RAM sentinel, confirm that
the corresponding low-RAM address remains independent, and issue a GPU fill
followed by a VRAM readback. The harness trace checks guest register values;
the final device report checks DMA length and fault counters. Invalid sets,
duplicate metadata, mismatched NAND size metadata, and truncated MEM(M) flash
are also required to fail before the guest executes.

Build the native macOS ARM64 harness, then run:

```sh
python3 tests/check_system10_harness.py --harness build/mac-harness/pcsx-harness
```

The test needs roughly 130 MiB for generated MEM(N) fixture regions plus the
harness's loaded copies. Temporary packages, traces, and frames are removed
when the test ends.

## Production DRC regression

System 10 now supplies the existing ari64 backend with the board memory map,
both SIO devices and stable executable spans. MEM(M) bank selection, predictor
reads, flash commands and crypto control invalidate compiled window code.
MEM(N) shared-RAM writes and NAND DMA invalidate the affected instructions.
Stateful encrypted opcode reads execute live through the interpreter trampoline
instead of advancing the cipher during speculative compilation.

`tests/check_system10_drc.py` generates MEM(M)/MEM(N) programs and runs each
with the interpreter and ari64, before and after reset. It checks RAM results
from executed bank changes, cached/uncached aliases, a branch across a code-page
boundary, direct flash execution, shared-RAM code rewrites, NAND DMA replacing
previously compiled RAM code, independent upper RAM and real SIO register lanes.
The production libretro switching test also checks that System 10 selects DRC,
System 11 also selects DRC, Capcom retains its interpreter selection, and console CPU selection
is restored on unload.

For the production-core regression, reuse the existing Mac build:

```sh
python3 scripts/check_validation_storage.py --reserve-mib 256 && \
PCSX_BUILD_JOBS=1 sh tools/libretro-host/build_mac_core.sh && \
xcrun clang -O2 -std=gnu99 -Ideps/libretro-common/include \
  tests/test_system10_drc_libretro.c -o build/mac-libretro/test-system10-drc && \
python3 tests/check_system10_drc.py \
  --core build/mac-libretro/src/pcsx_rearmed_libretro.dylib \
  --host build/mac-libretro/test-system10-drc
```

The largest generated profile is approximately 169 MiB (five raw NAND chips),
kept one board at a time and removed after comparison. No raw frame/audio
captures are needed. This verifies CPU and bus integration, not compatibility
with every game or flash/NAND persistence across application launches.


## SPU worker on Vita core 2

The arcade board-thread option (`pcsx_rearmed_system12_board_thread`) also
controls System 10's SPU worker. ReArcade enables it by default in menu mode;
finite standalone runs use `--board-thread --gpu-thread`. `VITA_SPU_THREAD`
defaults to `ARCADE_SYSTEM12`, so arcade Vita builds compile the worker. The
NEON layout is MIPS DRC and board bus devices on core 0, GPU NEON on core 1,
and SPU ADPCM decoding, interpolation, mixing and reverb on core 2. SPU register
accesses, DMA, IRQ scheduling and frontend audio delivery remain on main.

The shared SPU worker uses the portable mutex/condition thread backend, with
ordered publication and completion of its bounded queue. Register mutations
and DMA fence old work; disabling the worker, reset and shutdown drain it.
CD reverb and noise used as a frequency-modulation source keep the serial
path where the existing SPU implementation shares scratch/state with main.
The console SPU-thread option is restored when leaving System 10. The other
arcade sound workers retain their independent device models.

`System10: spu_thread=1 worker_jobs=N` in a report verifies initialization
and actual work, not just the option value. The production-core DRC signatures
now run serial/threaded MEM(M) and MEM(N), interpreter/ari64, and reset cases.
`tests/check_system10_spu.py` additionally compares exact sample counts and
hashes across a serial run and two worker runs. It checks main-thread audio
callbacks, four ADPCM voices, volume/pitch changes, noise, pitch modulation,
nonzero reverb settings, DMA replacement/readback, serial fallback, reset with
pending work and worker shutdown. Build its host alongside the existing core:

```sh
python3 scripts/check_validation_storage.py --reserve-mib 64 && \
xcrun clang -O2 -std=gnu99 -Ideps/libretro-common/include \
  tests/test_system10_spu_libretro.c -o build/mac-libretro/test-system10-spu && \
python3 tests/check_system10_spu.py \
  --core build/mac-libretro/src/pcsx_rearmed_libretro.dylib \
  --host build/mac-libretro/test-system10-spu
```

The complete generated checks passed locally on 2026-10-19. A real Mr. Driller
2 run of 1,200 frames produced identical WAV hashes and every-frame video
hashes with serial and threaded synthesis, with ari64 and no board faults.
The worker processed 190 audio jobs. The switching/NVRAM test also enables
workers and verifies that System 10's SPU and System 11's C76 worker stop when
leaving their respective boards, in both legacy and mapped-input modes.
This is Mac correctness evidence, not a physical Vita performance result.
Commands, diagnostics and hashes are in
`artifacts/validation/system10-thread-20261019`; raw WAVs were removed after
comparison. The production VPK remains a separate packaging step.


The rebuilt ARMv7 NEON executable completed 1,200 Mr. Driller 2 frames in
Vita3K with both `--board-thread` and `--gpu-thread`, ari64 active, 190 SPU
worker jobs, no board faults and zero frontend audio errors at 66,075 Hz.
The same executable is installed in the manual Vita3K runtime; its previous
NEON eboot is preserved. The host emitted its known native cleanup errors
after the guest completed its report and returned to Idle, so the guest pass
is recorded separately from a clean host shutdown. No physical Vita speed
claim is made.
