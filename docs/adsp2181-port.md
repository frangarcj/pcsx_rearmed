# Standalone ADSP2181 source core

The native core now contains the complete pinned MAME ADSP2181 instruction
dispatcher and arithmetic/addressing helpers, behind a C API. This is a component
port for Acclaim RAX, not evidence that NBA Jam Extreme runs or produces sound.
RAX board wiring, BDMA, autobuffer timers/DACs and native PSX integration remain
pending in the [Mac board matrix](arcade-mac-validation.md).

Source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`,
`src/devices/cpu/adsp2100/adsp2100.cpp`, `adsp2100.h` and `2100ops.hxx`,
BSD-3-Clause, Aaron Giles. `tools/port_mame_adsp2181.py` checks the pinned
revision and local source changes before extracting the core, register layouts,
lookup tables and debugger-register bindings. `--check` verifies all generated
files. Public support is ADSP2181 only; retained upstream chip-type constants
do not establish coverage of other ADSP-21xx CPUs.

## Host interface and timing

`adsp2181.h` exposes 24-bit PM words, 16-bit DM words and an 11-bit I/O word
bus through board-owned callbacks. PM/DM addresses mask to 14 bits. SPORT
transmit/receive, DMOVLAY and timer-enable callbacks retain their source hooks;
the caller supplies actual memory, overlay banks and external timer behavior.
The core does not replace a sound firmware or synthesize command responses.

The source interpreter executes one nominal clock per instruction. Its IDLE
instruction and self-jump optimization terminate the current execution budget;
the host accounts for the full elapsed budget. Source interrupt polling occurs
at execution entry and relevant register/RTI operations. Board scheduling must
therefore deliver external events at appropriate boundaries; whole-machine
timing and physical IDLE behavior have not been independently validated.

Level IRQL0/IRQL1 and edge-latched BDMA/SPORT/IRQE/timer inputs preserve source
priority, vectors, nesting masks and PC/status stack handling. IDMA preserves
its paired upper-16/lower-8 PM protocol, data mode and address increment rules.
Reset clears source control/interrupt/stack state and restores DMOVLAY, while
preserving arithmetic/DAG register values and the host's elapsed clock.

Run/reset/IDMA reentry and destruction during callbacks are refused. SPORT's
source callback can read registers and adjust I registers. Register reads return
raw values; M registers retain their signed 14-bit representation in the returned
32-bit pattern. Invalid register/depth requests are rejected. Source invalid
register operations inside guest execution expose diagnostics and retain source
continuation behavior.

## Defined arithmetic and checks

Signed left shifts and overflowing signed products in the source are expressed
through unsigned bit patterns before narrowing, retaining MAME's existing
int32 intermediate-product behavior. These transformations cover negative
fractional products, unsigned products, PM writes and DIVQ remainder shifts.
They do not claim independently verified hardware behavior for every MAC corner
case. NORM high-word shifts use a bounded arithmetic shift after inserting the
carry bit, consistent with the [ADSP-218x instruction reference's NORM rules](https://www.analog.com/media/en/dsp-documentation/processor-manuals/5876423468xinset.pdf)
(pages 4-101/4-102). C++20 provides the source exponent-count operations and
defined arithmetic right shifts.

```sh
make -f tests/Makefile.adsp2181 test sanitize check-generated
```

Both O2 and ASan/UBSan pass. Executed vectors cover PM/DM/I/O, SPORT and
callback guards, overlay/reset, ALU carry/overflow/saturation and 218x constants,
16,000 source MAC product cases, shift limits, exponent and divide primitives,
circular/negative/bit-reverse DAG, PX packing, counted loops/calls, register
banks, level/edge IRQ acknowledgement and RTI, IDMA and HALT. A normal 256-cycle
memory program has identical registers/memory with one batch or single-cycle
slices. This does not turn the source's batch-ending optimizations into a claim
of universal instruction-count equivalence.

The Mac harness builds the component with C++20. No existing board uses it
until its real daughterboard wiring is added; no-board console behavior remains
separate. Authentic RAX bootstrap/commands/audio and gameplay still require
their own evidence.
