# RTC65271 source port for Twinkle

`libpcsxcore/arcade/rtc65271.cpp` now provides the Epson RTC65271 required by
Twinkle's main board: 64 indirect registers, 50 user bytes, 128 pages of 32
extended RAM bytes, update/alarm/periodic interrupts and source NVRAM format.
It is a standalone Mac component. Wiring it to a native main-PSX Twinkle
profile remains open; it does not establish game initialization or play.

The generator checks SHA256 of MAME `rtc65271.cpp/.h` from
`deb2f54fe7300e7a192690318beb860159bfd52a`. It extracts the actual register,
calendar, alarm, SQW and interrupt method bodies. BSD-3-Clause attribution to
Raphael Nabet and R. Belmont is retained in `rtc65271_LICENSE.txt`. The host
adapter supplies device ownership, explicit initial date, three timers and
source-format NVRAM serialization. Twinkle declares no optional default RTC
region, so cold user RAM/XRAM starts at source zero defaults, with binary mode.
The initial date represents the source framework's host-time seed; elapsed
time afterward comes only from explicit emulation timestamps.

The source allocates begin-update, end-update and SQW timers in that order.
The adapter retains that order for equal deadlines. It uses integer
attoseconds internally, so every power-of-two divider through 8192 Hz and
its half-period remain exact. Monotonic public nanosecond advances do not
truncate each periodic interval. The update begins once per second when the
actual DV bits equal `0x20` and SET is clear; UIP lasts 1984 microseconds.
SET cancels UIP, clears UIE as the source does and leaves the pending end event
to abort naturally. Coarse and fine advances produce identical registers and
IRQ callback sequences. Public mutation, advancing or destruction from a
synchronous IRQ callback is rejected and latched as a host-interface fault.

Source quirks remain visible rather than being corrected into a different
RTC model:

- Register A reads indicate DV `0x20` even when the stored DV bits halt updates.
- The first cold VRT read returns zero and sets `0x80` for the next read.
- Cold mode is binary, with the source's zero-based host-seeded 12-hour format.
  Its subsequent literal 12-hour increment path is retained, including the
  tested 11 AM to `0x8c` transition.
- The BCD leap-year test uses the raw year bits: BCD 2012 rolls February 28
  into March 1, while BCD 2024 reaches February 29. BCD year 99 increments to
  `0xa0`; binary year 99 rolls to zero. Tests preserve both source results.
- Alarm fields `c0`–`ff` mean don't-care. Register C reads clear interrupt flags.
  SQW/periodic divider state follows the source even when SQWE is clear.

The two source month-table accesses have explicit bounds guards. Invalid
guest month values remain writable, but a rollover that would index outside
the source table stops with a device fault before that access. The adapter
does not clamp the month, invent a date or continue after undefined behavior.

## Persistence and verification

Source NVRAM is exactly 4152 bytes: version 0, two masked controls, three alarm
bytes, 50 user bytes and all 4096 XRAM bytes. Import preserves the source's
control masks, drops saved RS bits and sets VRT. Clock values, current indirect
register/page and timer phase are not part of this NVRAM format or a machine
save state. Wrong size/version is rejected before object creation. The source
device has no reset override; a main-board soft reset should retain this object
and its continuous clock. Reloading the game-generated image in a new RTC
retains all persisted bytes, resets indirect/page state and reports valid VRT.

O2 and ASan/UBSan tests cover all XRAM pages, indirect aliases, source register
masks, binary/BCD and 12/24-hour paths, leap/year quirks, UIP boundaries, SET
abort, alarms, register-C acknowledgement, exact divider phase, timer-slice
equivalence, full NVRAM and callback/time/bounds failures.

An independent original IIDX2 run in MAME 0.287 records 14,240 RTC reads and
35,618 writes from the actual main CPU through the original backup-clear
workflow. The observer
keeps all tap handles alive and observes physical, KSEG0 and KSEG1 aliases;
it never replaces bus data or seeds initialized game NVRAM. Replaying every
timestamped transaction through this component produces identical responses,
all 64 final RTC registers, the page/index/SQW state, all 4096 XRAM bytes and
the complete 4152-byte MAME NVRAM file, in both O2 and ASan/UBSan. The final
timestamp is 120,014,513,017 ns. The initialized image has 3264 nonzero XRAM
bytes, SHA256 `1690ebbdb077edecfa7eb4bafde28ade53a491c7f2f34daa14f0cbd33a2b8dcd`.
This trace covers original backup pages, VRT checks and actual game-generated
persistent data. The game leaves the RTC divider stopped; alarm/calendar/
divider coverage comes from the source vectors above, not from pretending
the game exercised them all. A separate first-boot 60-second trace also passes
230 reads and 230 writes with identical registers, XRAM and source NVRAM.

The first-boot observer records 2301 actual main-CPU IN0 reads, including 599
reads with Start pressed. That early Start pulse reaches the game while its
sound checks are still running. A later pulse at 65–70 seconds completes
backup clearing and reaches the title in the independent reference; the full
initialization observer records 5675 IN0 reads / 83 pressed reads. This fixes
the reference's operator sequence without supplying an initialized NVRAM seed.
Native main-board startup still needs work; these are device/protocol results,
not native player evidence.
An independent warm MAME restart also reaches the title without an input
override, and reloading the generated NVRAM in this component retains every
persisted byte with source VRT/index/page startup behavior.

```sh
python3 libpcsxcore/arcade/rtc65271_generate.py ../mamedev --check
make -f tests/Makefile.rtc65271 test sanitize

rtc_probe_dir=$(mktemp -d /tmp/iidx2-rtc.XXXXXX)
PCSX_TWINKLE_RTC_TRACE_DIR="$rtc_probe_dir" mame bmiidx2 \
  -rompath "$PWD/roms" -video none -sound none -skip_gameinfo -nothrottle \
  -autoboot_delay 0 -seconds_to_run 120 \
  -nvram_directory "$rtc_probe_dir/trace-nv" \
  -cfg_directory "$rtc_probe_dir/trace-cfg" \
  -snapshot_directory "$rtc_probe_dir/snap" \
  -autoboot_script "$PWD/tests/capture_twinkle_rtc.lua"
build/rtc65271-tests/rtc65271 --mame-trace "$rtc_probe_dir"
build/rtc65271-tests/rtc65271-sanitize --mame-trace "$rtc_probe_dir"
```

The original audited `bmiidx2`/`gq863` ROMs and three CHDs are required for the
optional MAME run. Evidence is retained under ignored
`artifacts/boards-mac-20261015/twinkle/bmiidx2/rtc65271`.
