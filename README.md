PCSX-ReARMed - yet another PCSX fork
====================================

![CI (Linux)](https://github.com/notaz/pcsx_rearmed/workflows/CI%20(Linux)/badge.svg)

PCSX ReARMed is yet another PCSX fork based on the PCSX-Reloaded project,
which itself contains code from PCSX, PCSX-df and PCSX-Revolution. This
version was originally ARM architecture oriented (hence the name) with
its MIPS->ARM dynamic recompilation and assembly optimizations, but more
recently it targets other architectures too. A fork of this emulator was
used in [PS Classic](https://en.wikipedia.org/wiki/PlayStation_Classic)
(without any coordination or even notification).

## PCSX ReArcade (PS Vita)

PCSX ReArcade is this fork's PS Vita app: Namco System 10, System 11 and
System 12 and Capcom ZN arcade games (Soul Calibur, Tekken 3, Street Fighter
EX...) from MAME romsets, and PlayStation discs, on one 3D game shelf
([arcade-shelf](deps/arcade-shelf)).

```sh
make -f Makefile.vita VITA_GPU=vitagl ARCADE_SYSTEM12=1   # pcsx-rearcade.vpk
```

It installs as `PCRA00001` and keeps its files in `ux0:data/pcsx-rearcade`:
MAME zips and discs in `roms/`, BIOS files in `bios/`, PlayStation covers
in `covers/`, cabinet art in `snap/` and `marquees/` (by MAME set name).
On the shelf: left/right game, up/down version, Cross play, Square
options, Select credits, Triangle exit. Start + Select pauses a game.
`ARCADE_SHELF=0` builds the same core as the validation app (`PCSXG0012`,
`ux0:data/pcsx-rearmed`) that the measurement tools use.

Copy the MAME zips of the four families into `roms/`, whether the sets are
merged, split or non-merged: files are found by CRC32 and size, so zip names do
not matter, and the BIOS and sound sets that a game needs (`namcoc76.zip`,
`coh1000c.zip`, `qsound.zip`...) are found next to it
([System 10/11/Capcom ZN](docs/arcade-romsets.md),
[System 12](docs/system12-romsets.md)). The shelf lists the complete sets and
groups clones under their parent. Prepared folders still work: prepare local
media with `scripts/prepare_system10_board.py`,
`scripts/prepare_system11_board.py` or `scripts/prepare_capcom_zn_board.py`
(see each script's `--help`) and place the folder directly inside `roms/`,
keeping `board.ini` and all its region files together. Prepared System 12
folders with `board.cfg` are also listed, including CDXA profiles when their
required media is supplied.

System 10, System 11, System 12 and Capcom ZN support the native ARMv7 dynarec.
Capcom ZN invalidates translated code when its 4 MiB ROM bank changes and
routes both SIO blocks through the actual ZN devices. System 10 DRC
handles MEM(M) banks/flash, MEM(N) shared code and NAND DMA invalidation.
Live stream-encrypted opcode fetches use an interpreter trampoline without
speculatively consuming the stream. The arcade button assignments
apply to all four families. System 12/11 and Capcom EEPROM images are saved
separately from the ROM files in the frontend save directory as
`<set>.arcade-nvram`. What a System 10 game writes to its flash or NAND is saved
as `<set>.arcade-flash`, only the 16 KiB chunks that differ from the ROM media.
The pause menu offers save states only when the active board supports them.

System 10 offloads SPU synthesis to a worker; System 11 uses a board worker
for its C76 CPU and C352 audio, and Capcom ZN for its Z80 and QSound.
In the ReArcade NEON app, the default Vita
layout is main MIPS/DRC on core 0, GPU rendering on core 1, and SPU (System 10),
C76/C352 (System 11), H8/C352 (System 12) or Z80/QSound (Capcom ZN) on core 2.
System 10's bus devices, SPU registers and IRQ scheduling stay on main;
Capcom ZN's SIO, MCU, EEPROM and main IRQs also stay on main. The shared
`pcsx_rearmed_system12_board_thread` option controls all four boards; the
standalone measurement app accepts `--board-thread`.


Credits:
* [PCSX-ReARMed](https://github.com/notaz/pcsx_rearmed) by notaz and
  contributors, and the PCSX projects it comes from (see AUTHORS)
* [PCSX AwO](https://github.com/Rinnegatamante/PCSX_AwO) by Rinnegatamante:
  the PS Vita frontend, ROM selector, options, save states, the disc of the
  game shelf, and [vitaGL](https://github.com/Rinnegatamante/vitaGL)
* the native PS1 renderer by aalhendi, derived from
  [PsyCross](https://github.com/OpenDriver2/PsyCross) (REDRIVER2 Project)
* [MAME](https://www.mamedev.org/) and its developers: the Namco System 12
  driver, devices and romset definitions this port follows
  (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md))
* VitaSDK, SDL, Dear ImGui, libpng, zlib, libchdr and Roboto

Not affiliated with Namco, Bandai Namco or Sony; no game data is included.

## Features
* ARM/ARM64 dynamic recompiler by Ari64
* [lightrec](https://github.com/pcercuei/lightrec/) dynamic recompiler for other architectures
* NEON GPU by Exophase for ARM NEON and x86 SSE2+
* PCSX4ALL GPU by Una-i/senquack for other architectures
* heavily modified P.E.Op.S. SPU
* BIOS HLE emulation (most games run without proprietary BIOS)
* libretro support

## Native macOS harness and arcade board ports

An Apple Silicon headless harness runs the interpreter with the existing NEON
GPU. `make mac-smoke` builds and verifies a generated MIPS graphics program.
See [the harness instructions](tools/mac-harness/README.md) and
[Soul Calibur port status](docs/system12.md) for ROM verification, board tests,
and current limits. The verified Soul Calibur Asia C set reaches attract mode
and a match through scripted coin, start and character-selection inputs, with
C352 audio. Other regional sets and complete gameplay fidelity remain unverified.

The full MAME PSX-derived scope is recorded in
[the machine inventory](docs/mame-psx-systems.md): 491 registrations across ten
driver families. The inventory distinguishes declared hardware from verified
board support. Native generated guests also exercise System 10, System 11,
Capcom ZN and Taito FX1S. Standalone device checks are available with
`make -f Makefile.mac-harness devices-test`; each board's native test target
is documented in the harness instructions.
