# Native Taito FX-1S host adapter

`libpcsxcore/systemtaito.c/.h` composes the existing Sony ZN common board,
ZNMCU, two CAT702 devices, AT28C16 EEPROM, both PlayStation SIO variants and
`taito_fx1` daughterboard. Source facts are pinned to MAME
`deb2f54fe7300e7a192690318beb860159bfd52a`, principally
`src/mame/sony/zn.cpp/.h`, `znmcu.cpp` and the devices used by that configuration.
The native host glue is GPL-2.0-or-later; reused device ports retain their
upstream BSD-3-Clause notices and redistribution terms. See
[taito-fx1-port.md](taito-fx1-port.md) for the daughterboard implementation and its limits.

The source registry contains 22 Taito FX-1 declarations. Nine game profiles are
eligible for this FX1S adapter: `sfchamp`, `sfchampo`, `sfchampu`, `sfchampj`,
`psyforce`, `psyforcej`, `psyforcex`, `mgcldate`, and `mgcldtex`. BIOS roots and
The shared host now also accepts the twelve FX1Z profiles through the explicit
`--taito-fx1z` loader, using their ROM-backed Zoom daughterboard and FRAM. The
BIOS root remains rejected. See [FX1Z Mac validation](taito-fx1z-mac.md).
Regenerate the native table with:

```sh
python3 scripts/generate_taito_fx1_profiles.py
python3 scripts/generate_taito_fx1_profiles.py --check
```

The generator re-extracts the registry from the pinned checkout rather than
trusting arbitrary JSON. It also records the original ROM CRCs, load layouts
and erase values for internal source-reference checks. These are not an
additional native loader authentication policy.

## Media and geometry

Use `scripts/prepare_taito_fx1_board.py` to validate the original ROM archives
against source CRC/SHA1 declarations and assemble the package. The native loader
requires every one of its 25 `board.ini` keys exactly once, rejects unknown keys,
and compares each value against the generated eligible source profile. It
requires `board_execution_verified=false`. Exact output region sizes are:

| File | Bytes |
|---|---:|
| `main.bin` | 524288 |
| `banked.bin` | 16777216 |
| `audiocpu.bin` | 131072 |
| `adpcma.bin` | 2097152 or 4194304, according to the selected set |
| `cat702_key0.bin` | 8 |
| `cat702_key1.bin` | 8 |

The loader checks source metadata and geometry, while the preparer establishes
authentic ROM provenance. Manually assembled packages and synthetic fixtures
are accepted when they match the profile and sizes; native load success does
not prove authentic ROMs or game boot. No keys, audio firmware, sample data or
missing `upd78081` ROM are supplied or fabricated. The source FX1S configuration
uses the distinct ZNMCU and does not instantiate the undumped `upd78081`; the
preparer records that exact reviewed omission.

COH1000T selects 4 MiB of main RAM, 512 KiB BIOS, type-2 GPU semantics and 1 MiB
VRAM with 512 rows. The metadata records CXD8530CQ crystal 67,737,600 Hz and
CXD8561Q crystal 53,693,175 Hz. The host still uses the existing PCSX fixed
33,868,800 Hz scheduling clock and interpreter timing; it does not implement a
new CXD8530CQ CPU or independently rescale the GPU crystal timing from metadata.

## Host contract

Load before PSX memory/GPU initialization. Other active native board adapters
and allocated PSX main RAM prevent loading. `mem_reset` copies BIOS, clears
main RAM and initializes the 8 MiB ROM window's executable LUT entries for the
physical, KSEG0 and KSEG1 aliases. Bank changes invalidate those translated
instruction ranges. Banks 0/1 map the 16 MiB region; source bank selections 2/3
remain selected but invalidate executable LUTs and fault on attempted ROM reads
instead of inventing contents.

Call `systemtaito_sync(psx_cycle)` at host scheduling boundaries and before
reset. MMIO read/write methods also synchronize before touching devices. Widths
are naturally aligned 1/2/4-byte accesses with little-endian lanes. Common ZN
and FX1 daughterboard maps are delegated to their existing device adapters.
`0x1f801040..0x1f80105f` belongs to the adapter's SIO0/1, not console-controller
SIO. CAT702 outputs and MCU TXD are ANDed into SIO0 RXD, MCU DSR drives SIO0 DSR,
and SIO0 SCK/TXD drive the authentic common-device pin state machines. SIO1 has
no invented receive peer. IRQs latch PSX I_STAT bits 7/8.

Absolute peripheral cycles survive PSX cycle-counter resets. Each SIO0 pin
callback uses `psx_sio_elapsed_cycles()` at that event's deadline to advance
MCU/EEPROM before changing pins; then sync advances the remaining interval.
This prevents late multi-bit catch-up from rewinding the MCU timer or delaying
DSR pulses until the end of a host scheduling slice. Integer microseconds use
the fixed PSX clock. The sound CPU advances at 4 MHz through a retained rational
clock remainder, using the daughterboard's one-T-state YM2610B timing. Stereo
WAV capture uses the source integer stream frequency `8000000 / 144 = 55555` Hz.

`sync` returns -1 for a device/timing fault, 0 normally, and 1 while a watchdog
soft reset is pending. Its callback only sets a flag and increments a count;
the host must service 1 with a machine reset at an instruction/block boundary.
`systemtaito_reset_requested()` exposes the same retained request. There is no
inline/reentrant CPU reset. `reset` resets the daughterboard and clears this
request and the sound conversion fraction, while preserving peripheral time,
EEPROM state and the common ZN/SIO/CAT/MCU pin/register state. Pinned `zn_state`
has no `driver_reset` override and these common devices have no `device_reset`
override, so the host does not call the convenience `zn_board_reset` on a soft
reset. The existing daughterboard implements its own source reset lifecycle.

EEPROM import/export require the exact 2080-byte data-plus-ID image. WAV
open/close and report methods expose sound capture, source geometry, cycle
epoch, banks, Z80 PC/instruction counters, SIO status, watchdog requests,
serial callback count and faults. Destroy releases all adapter-owned devices
and loaded media.

## Inputs and checks

The `znt2p` source includes `zn2p` and its `PORT_MODIFY` exclusions. Accepted
active-low names are `p1_up/down/left/right`, `p2_up/down/left/right`,
`p1_button1/2/3`, `p2_button1/2/3`, `p1_start`, `p2_start`, `coin1`, `coin2`,
`test`, `service` and `tilt`. DIP names `bios_service_dip` and `test_mode_dip`
operate source bits 0x02 and 0x08. Unused button4/button5 and third/fourth-player
controls are rejected. DSW defaults to its source low nibble 0x0f; the MCU itself
masks that nibble into its status byte. Unused analog/trackball callbacks use
the upstream device defaults.

`systemtaito_selftest()` is nondestructive when a real or fixture board is
active: it checks source geometry, boardconfig, ROM byte order, SIO register
access and rejected source-disabled controls. With no adapter or allocated
console RAM, it constructs explicitly synthetic HALT-only Z80 media and CAT
keys for device traces, then destroys them. These traces cover common inputs,
ROM aliases/banks, EEPROM busy polling at 199/200 us through a PSX clock reset,
MCU status 0x1f through eight SIO deadlines in one catch-up, independent SIO1
IRQ, and deferred watchdog reset preserving the bank until host service. The
watchdog's isolated trace advances its own virtual clock without spending
20 million sound CPU cycles. Normal and ASAN/UBSAN native-device mock-host runs
pass; constituent daughterboard tests also pass. These checks do not verify
an authentic Taito game boot, full native PSX graphics/SPU behavior, real media
audio fidelity, or an end-to-end MAME machine oracle. The native core/frontend
now route board memory, reset and sync through this adapter with the
`--taito-fx1 REGION-DIR` option. The [native fixture](taito-fx1-native-test.md)
passes instruction-driven MIPS/Z80 execution with and without instruction
cache, including banked fetches, GPU, RAM, SIO/MCU/IRQ, sound packets, inputs,
EEPROM persistence and nonzero YM2610B PCM. It does not establish authentic
game compatibility.


## Authentic Mac follow-up

Psychic Force now has bounded PLAYER1 selection and combat evidence with
interpreter/GL, exercised controls and nonzero YM2610B. Its runtime EEPROM matches
an independently configured MAME image. An earlier coin timing still enters an
IRQ-callback VSync wait and resets in both implementations. See
[the commands, evidence and remaining checks](taito-fx1-mac.md).
