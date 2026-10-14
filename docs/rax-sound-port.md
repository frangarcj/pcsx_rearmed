# Acclaim RAX component on Mac

The standalone RAX board executes NBA Jam Extreme's original ADSP2181 firmware
with native Harvard buses, BDMA, host latches and stereo DMA-driven DACs. This
establishes the sound-board component, **not native PSX gameplay validation**.
Main-CPU SRAM/ROM banking, live inputs, NVRAM and SPU/RAX mixing now have
separate [native NBA player evidence](nbajam-zn-mac.md).

## Source behavior

The port follows MAME `deb2f54fe7300e7a192690318beb860159bfd52a`,
`src/mame/shared/rax.cpp/.h` (Phil Bennett),
`src/devices/sound/dmadac.cpp/.h` (Aaron Giles), and the separate complete
[ADSP2181 source core](adsp2181-port.md). Both components include their upstream
BSD-3-Clause attribution and license. No firmware substitution or command HLE
is used.

The ADSP runs at 16,670,000 Hz. Reset loads exactly the first 32 big-endian PM
words from ROM, preserving the rest of PM and DM RAM. DM maps five low RAM
banks selected by DMOVLAY and the two-bit RAM bank latch, shared internal RAM,
and the source control register window. Unsupported overlays expose a fault.
ROM bank writes retain the full value. Source unmapped I/O returns FFFF.

Host writes replace the 16-bit input latch and assert level IRQL0. Guest I/O
read clears it; guest reply replaces the output latch and clears PF0. Host
output read sets PF0. Soft reset preserves both latched values and the DAC
queues/enables/rate. Generic source timers without a configured period also
retain their pending deadlines; a BDMA CPU reset does not reset the entire RAX
board. Reset does not erase elapsed time or RAM.

BDMA supports 24-bit PM, 16-bit DM, high-byte DM and low-byte DM reads. Payload
copy is immediate, with source address increments, while completion occurs
after the word count in DSP clocks. Completion updates external address/page,
clears the count, and resets only the CPU or pulses BDMA. ROM spans can cross
the 4 MiB bank boundary within the actual region; no invented mask wraps them.
ROM bounds, reverse DMA and recursive transfers expose faults rather than
unsafe accesses or fabricated completion. Source reverse DMA is fatal/TODO.

SPORT0 uses the guest I/M/L selection, source pointer rewind, serial-clock
divisor and half-buffer timer. Source SPORT1 writes have no transfer effect.
Two 32768-entry DAC rings accept interleaved signed 16-bit samples with gain 1
and the source float normalization `sample * (1.0f / 32767.0f)`. Disabling a DAC
clears that channel's queue. Empty streams produce silence and full queues
report drops. Oversized autobuffers are rejected instead of reproducing the
source's fixed stack-buffer overflow; companded SPORT1 data is unsupported,
as in source. Other control writes retain their source stored values.

## Timing and output limits

The current board executes one nominal DSP clock per instruction, so a guest
write can schedule even a one-clock BDMA completion without a batch overshoot.
Fractional autobuffer deadlines are delivered at the next DSP clock boundary;
zero-period generic timers fire once. This bounds event quantization to one
DSP clock and retains all events within that boundary. CPU diagnostics stop
the board with an explicit fault.

The output callback supplies raw stereo floats at the integer source DAC rate,
including its default 48 kHz startup and firmware-selected rate changes.
Each frame also supplies its absolute DSP clock for frontend interpolation.
MAME's `set_sample_rate(u32)` truncates the RAX-derived frequency; NBA's divider
32 gives 31,571 Hz. A rate change begins a new sample phase after flushing
previous output. This component does **not** reproduce MAME's sound-stream
history resampling or claim host output at 44.1 kHz. Native frontend interpolation now uses the absolute sample clocks; MAME
resampler-history matching and whole-machine timing remain unverified. Run/reset/host/debug
mutation and destruction are refused from output callbacks.

## Authentic firmware and independent reference

All original NBA chips are separately source-audited by
`prepare_nbajam_zn_board.py`; the borrowed RAX region is 8 MiB including original
zero padding. A two-second cold component run executes 33,340,000 nominal DSP
clocks without CPU/board faults. Bootstrap loads 9,245 words in 49 transfers,
performs one BDMA CPU reset, replies A000, then configures SPORT:
`AUTO=0f82`, `DIV=0020`, `SYS=1c00`, `IMASK=00c0`, size 1024, increment 1.

Independent Homebrew MAME 0.287 reports the same reply and control state.
Its reply time is 0.000903299 seconds; the native first-reply clock is 15,058
(0.00090329934 seconds). This is a bounded bootstrap comparison, not a claim
of cycle-exact whole-system equivalence.

A 40-second MAME run supplies 127 actual main-CPU latch writes. Replaying their
captured order/data/time (rounded up by at most one DSP clock) into the native
component consumes 127/127, with 896 interrupts, 28,473 completed BDMA transfers,
2,547,775 transferred words, 9,866 autobuffer callbacks and **697,374 nonzero
stereo output frames**. It reports zero board/CPU faults and zero DAC overruns.
The 254 channel underruns are initial silence before the first half-buffer;
that count stays fixed. This exercises authentic sound paths, not a PSX player
run in PCSX Rearcade. O2 and ASan/UBSan produce identical diagnostic logs.

```sh
make -f tests/Makefile.rax-sound test sanitize
build/rax-sound-tests/test-rax-sound --firmware roms/prepared/nbajamex/rax.bin
build/rax-sound-tests/test-rax-sound --firmware roms/prepared/nbajamex/rax.bin \
  --commands artifacts/boards-mac-20261014/nbajamex-rax-commands-40s.csv \
  --seconds 40
```

The optional command CSV is `elapsed_DSP_clock,hex_word`, ordered by clock.
Analytical tests cover cold/soft reset, all five RAM banks, PM/DM endpoints,
unmapped I/O, latch replacement and real level-IRQ acknowledgement/reply,
four BDMA types, bank crossing, count/deadlines, edge IRQ/RTI and CPU-only reset,
pending timers across soft reset, actual SPORT-driven stereo sample output and
source unsupported-operation faults. All pass O2 and ASan/UBSan, and the Mac GL
harness builds the component. The native NBA game profile now selects the component, with complete-board
player evidence recorded separately.
