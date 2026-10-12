# H8/3002 execution port

The Namco System 12 adapter executes the actual H8/300H sub CPU ROM through
`libpcsxcore/arcade/h8.cpp`. ISA bodies and arithmetic flags derive from
Olivier Galibert's BSD-3-Clause MAME H8 implementation. The local generator
includes every opcode definition enabled for H8/300H, and illegal encodings
halt with a diagnostic. No shared-memory boot responses are synthesized.

`h8.h` exposes an opaque C interface: create/destroy/reset, run a cycle budget,
set external IRQ/NMI levels, drive ADTRG and SCI RX/CLK pins, and obtain
CPU diagnostics. Config callbacks provide logical big-endian byte accesses,
optional atomic word accesses, GPIO input/output, serial TX/CLK output, and raw
10-bit analog input. Prepared `sub.bin` already has logical H8 byte order;
its reset vector is 0x100. Word callbacks are needed for C352 word operations.

The board must independently drive IRQ1 and `h8_adc_trigger` from vblank.
H8 ADC conversion starts on the falling ADTRG edge when ADCR bit7 enables
external triggering. This is required by the real Soul Calibur routine at
0x990, which waits for the ADC completion flag. Soul Calibur's unused analog
inputs default to zero in MAME. The System 12 sub CPU clock is 16,934,400 Hz.

Implemented internal devices include RAM, GPIO, IRQ priority/latching, five
internal-clock timers, two pin-level SCI channels and ADC software/external/scan
conversion. Instruction costs follow MAME bus/internal cycle accounting.
`h8_run_cycles` completes execution states and carries budget overshoot;
`h8_run_until` resumes MAME-generated partial states at external absolute
deadlines. Bus checkpoints occur after an access, so a crossing access sees
the pre-edge value and its remaining charge is carried forward. Peripherals
establish internal deadlines as well as external deadlines. Register mutations
use the charged bus cycle, in source ADC/SCI0/SCI1/timer order. After a soft
reset, device dividers use the restarted H8 cycle domain, while callbacks
retain the board absolute clock. The legacy whole-state budget API retains
coarser execution and is not the timing-fidelity entry point.

DMA and watchdog execution remain unimplemented and accesses set diagnostic
bits. External timer clock pins, timer output/synchronization, ADC power/standby and save-state
serialization also remain incomplete. RTC/settings/JVS endpoint behavior is
the board adapter's responsibility. No complete-peripheral claim is made.

SCI now executes the source shift-register/clock state machine, including
internal/external clocks, synchronous/asynchronous framing, TDRE/TEND timing,
RX errors and read-before-clear status latches. System12 routes SCI1 TX to
settings and CLK to inverted RTC CLK before direct settings CLK; RTC DATA
feeds RX1. SCI0 uses the source external period `542534722222` attoseconds.
Byte receive/transmit helpers remain diagnostic compatibility paths.
The independent MAME probe covers register polling, including synchronous
receive without TX. RX start now converts accumulated CPU local attotime to
ticks, independently of TX's charged cycle count, and retains its fractional
phase across soft reset. Five programs and three native slice sizes match
1440 observation stores exactly. The added RX-only program detected an overrun
one poll late before this correction. A separate production-board RTC firmware
probe also matches eight bytes/address/cycle observations across eight Mac CPU
and scheduling variants. This is not a complete RTC/JVS wire trace;
MAME's timer-callback time context and warm reset remain separate comparison work.
No JVS endpoint or standby implementation is claimed.

`make -f tests/Makefile.h8 test` verifies instructions/addressing/flags,
subroutines/IRQ return, atomic word access, timers, SCI, ADC trigger/scan,
illegal diagnostics, scheduler slicing and partial deadline continuation.
`make -f tests/Makefile.h8 deadline-ubsan-test` covers access timestamps, reset,
IRQ selection and external IRQ/ADTRG deadlines under UBSan. Synthetic tests and 120 frames of
the actual sub ROM passed ASan/UBSan with no illegal opcode or unsupported
DMA/watchdog access. The sub ROM completes RTC/settings initialization and
reaches its 0x16ca..0x16d2 idle routine.

The native System12 port-B polling fixture now matches four actual MAME
old/new read pairs with zero cycle difference on interpreter and ARI64,
independent of short/long MIPS block sizes. See
[screen timing evidence and its limits](system12-performance.md#mac-source-screen-correction-2026-10-05).

`python3 tests/check_mame_h8.py` captures an independent installed-MAME oracle
at 0.1/1/2/10/20 seconds. MAME observes the same idle routine and shared H8
identification words at 0x080400: `5130 324e 3136 3132`; the PSX sees those words
at 0x1f080400. CPU samples vary with scheduling and main-CPU progress. The
oracle records registers, SCI/IRQ/timers, stack, and shared 0x3078/0x3140 regions
in `artifacts/mame/h8-oracle.log`. Early captures found and verified the SCI
status-clear requirement and external ADC-trigger wiring.

See `libpcsxcore/arcade/h8_README.md` for regeneration and details, and
`libpcsxcore/arcade/h8_LICENSE.txt` for attribution and license terms.

The new peripheral comparison is reproducible with:

```sh
make -f tests/Makefile.h8-adc test ubsan
make -f tests/Makefile.h8_timer16 test ubsan-test
make -f tests/Makefile.h8-sci test ubsan
make -f tests/Makefile.h8 cycle-clock-test cycle-clock-ubsan-test
python3 tests/check_mame_h8_peripherals.py --include-sci --output artifacts/validation/h8-peripherals-20261005/mame-peripherals.json
```

It runs the same injected H8 firmware in installed MAME and native code,
compares external observation stores with zero tolerance in value/address/cycle,
and repeats native execution with 1/1024/10000-cycle slices. Parent ROMs are
unchanged; private NVRAM/config paths and a termination watchdog bound the run.
The historical baseline is fixed at `dde07db7`, not the moving HEAD. Installed
MAME reports 0.287; its binary identity with the pinned source is unverified.

The production SCI1/RTC serial gate is reproducible with:

```sh
make -f Makefile.mac-harness DRC=1 SYSTEM12_TIMING=1 BUILD=build/mac-harness-drc-timing
python3 tests/check_mame_system12_rtc.py --output artifacts/validation/h8-rx-clock-20261005/mame-rtc.json
python3 tests/check_system12_rtc.py --harness build/mac-harness-drc-timing/pcsx-harness --mame artifacts/validation/h8-rx-clock-20261005/mame-rtc.json --output artifacts/validation/h8-rx-clock-20261005/native-rtc.json
```

It drives port A.0/B.5, reads SCI1 with the actual board's inverted RTC CLK and
RX1 DATA wiring, and captures H8 stores through a passive shared-RAM observer.
The RTC is seeded at the native deterministic epoch (Sunday 2026-10-04 12:00:00,
raw BCD `00 00 12 07 04 10 26`). Its 52 valid bits omit the weekday high nibble;
the remaining clocks retain the final DATA level. This covers that transfer,
not complete timed mailboxes, RTC calendar advancement, JVS or global scheduler
equivalence. Observer on/off retains the board report and framebuffer.
