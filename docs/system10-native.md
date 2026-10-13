# Native Namco System 10 adapter

`system10.c` connects the source-derived MEM(M)/MEM(N) board devices to the
PCSX host. The embedded profile table comes from all 45 `GAME` entries in the
pinned `scripts/system10_boards.json`; `scripts/generate_system10_profiles.py`
checks the pinned MAME checkout and extracts per-game ROM-region sizes and
MEM(N) NAND models before regenerating `libpcsxcore/arcade/system10_profiles.inc`.

The loader accepts only source-matching MEM(M) or MEM(N) packages whose
registry row has no unsupported extensions, NO_DUMP media,
`MACHINE_NOT_WORKING`, or other unsupported requirements. It requires the
exact eleven metadata keys from `prepare_system10_board.py`, plus exactly one
`region.<tag>` filename and `region.<tag>.size` value for every source ROM
region. Unknown or duplicate keys, wrong metadata, missing or extra regions,
unexpected files in the metadata, and any file whose byte length differs from
the profile cause loading to fail before the device starts. At this revision
the eligible base-board profiles are Chocovader, Gamshara, Gamshara original,
GJ Space, Kuru Kuru Ninpuzzle, Kono Tako, Mr. Driller 2 variants,
Mr. Driller G, PaniKuru, and Star Trigon. The registry still records the other sets with their unsupported
requirements so the loader does not silently approximate their daughterboards.

MEM(M) loads the 4 MiB main ROM, 8 MiB raw flash region, and 32 MiB data ROM.
It keeps the immutable raw region separate from a mutable 8 MiB Intel flash
image. MEM(N) loads the 4 MiB main region and each exact small-page NAND image
including its 16-byte OOB area. MAME's source-selected K9F2808U0B geometry
(512+16 bytes/page, 32 pages/block, IDs `ec 73`) is passed to the NAND device.
For MEM(N), the native board copies the MAME boot pages into the writable main
ROM buffer. The main PSX RAM and ROM layouts are both 4 MiB, and the GPU is
configured as type 2 with the standard 2 MiB VRAM geometry.

The base I/O device reads the MAME active-low SYSTEM and IN1 ports. IN1 exposes
the two players' six buttons, directions, start buttons, coins, service, and
service 1 at their source bit positions. `system10_set_input` takes names such
as `p1up`, `p2button3`, `coin1`, `service`, `dip1`, and `jvs_sense`. The 100 Hz
MAME timer starts with the source's immediate reset-time tick and raises PSX
INT10 through I_STAT bit 10. Elapsed PSX cycles are converted to microseconds
with a retained rational remainder before advancing the board timers and flash.

The adapter exposes dynamic MEM(M) bank-window reads through
`system10_read`, since those decryptable bank reads cannot be represented by a
static ROM pointer. The native interpreter uses that callback for executed
instruction fetches in the dynamic window, including its optional instruction
cache line fill. Debugger peeks do not call it because the MAME read path
has device side effects; the trace's opcode field is zero where an instruction
has no direct memory pointer. This does not affect the executed instruction.
Normal boot code is copied into the usual PSX ROM map.
MEM(N) DMA channel 5 reads caller-provided PSX RAM spans from the native NAND
pipeline. ATA/CD-ROM and EXIO/MGEXIO/MEM(P3)/MEM/IO daughterboards remain
unsupported and the loader rejects profiles that require them. No game keys,
ROM substitutions, or boot patches are added. DMA5 supports the source's
`0x01000000` word transfer and `0x01000200` block transfer; a zero block count
means 65,536 blocks. A transfer exceeding the destination RAM span fails before
copying, and unsupported modes or sources report a board fault. Completion
clears CHCR and raises the existing PSX DMA interrupt only after a successful
transfer. Completion is synchronous; detailed DMA bus timing remains pending.

Build with `make -f Makefile.mac-harness`, then pass a verified media package
using `--system10 REGION-DIR`. `--inputs` and `--board-test` are available.
`--audio` now captures the native SPU stereo output in the Mac harness.
`--nvram` export is still unavailable; Intel flash and optional writable NAND
remain mutable in-memory images. See [Mac gameplay validation](system10-mac.md). The input adapter uses the
base SYSTEM/IN1 layout and rejects controls disabled by the source's
`PORT_MODIFY` masks for Gamshara, GJ Space, Star Trigon and Kono Tako. DIP fields
are raw switch levels; compound region DIP encodings remain a caller choice.

The runtime adapter follows the already-tested `namcos10_board`,
`namcos10_crypto`, `namcos10_io`, and `nor_flash` modules. The MAME source is
`src/mame/namco/namcos10.cpp` at the repository's pinned revision. The adapter
does not claim full game support; CPU instruction execution and board-specific
guest software require per-title validation. Mr. Driller 2, Mr. Driller G and
Gamshara passed short scripted Mac gameplay checks on 2026-10-13.
