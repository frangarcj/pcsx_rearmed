# Sony ZN-1 / ZN-2 common board module

`libpcsxcore/arcade/zn_board.c` implements the common board I/O described in
MAME's `src/mame/sony/zn.cpp`, pinned to MAME commit
`deb2f54fe7300e7a192690318beb860159bfd52a`. The upstream file carries the
BSD-3-Clause license and credits; this port retains that license and attributes
the source in its header.

The module owns the shared input apertures, board-configuration byte, coin
output latch, security/input mux register, AT28C16 programming and busy polling, and the
ZN-2-only SPU clock workaround register. It optionally wires up zero, one, or
two daughterboard CAT702 devices, using the existing `cat702.c` pin-level
device. An optional borrowed [ZN MCU](zn-mcu.md) receives the actual mux
register selects and SIO0 clock, with its 50/5 us serial pulse timing. It does not claim that any ZN game boots.

| Hardware | ZN-1 in MAME | ZN-2 in MAME | Module behavior |
| --- | --- | --- | --- |
| Main CPU | CXD8530CQ, 67.7376 MHz | CXD8661R, 100 MHz | Profile metadata only; CPU execution stays with the host |
| GPU / VRAM | CXD8561Q, 1 or 2 MiB | CXD8654Q, 2 MiB | Profile metadata only; GPU stays with the host |
| Main RAM | 4 MiB default | 4 MiB default | Host/core memory map; size informs `boardconfig_r` |
| Shared board I/O | Inputs, config, security/mux, coin, EEPROM | Same | Implemented at byte-addressed physical addresses below |
| Extra read register | — | `0x1fa60000..01` toggles bit 3 | Implemented on reads at `0x1fa60000` |
| Daughterboard logic | Game-specific | Game-specific | Not implemented here; unknown addresses fail closed |

The MAME common map provides these address ranges:

| Address | Behavior |
| --- | --- |
| `0x1fa00000..03` | P1 active-low input byte on the low lane; the other three byte lanes read zero |
| `0x1fa00100..03` | P2 active-low input byte on the low lane; the other three byte lanes read zero |
| `0x1fa00200..03` | Service active-low input byte on the low lane; the other three byte lanes read zero |
| `0x1fa00300..03` | System/start/coin active-low input byte on the low lane; the other three byte lanes read zero |
| `0x1fa10000..03` | P3 active-low input byte on the low lane; the other three byte lanes read zero |
| `0x1fa10100..03` | P4 active-low input byte on the low lane; the other three byte lanes read zero |
| `0x1fa10200` | Board configuration |
| `0x1fa10300` | Security and ZNMCU mux select/readback latch |
| `0x1fa20000` | Coin counter/lockout output latch/readback |
| `0x1fa30000..03` | MAME `noprw`: reads zero; writes ignored |
| `0x1fa40000..03` | MAME no-op read range: reads zero |
| `0x1faf0000..07ff` | 2 KiB AT28C16 EEPROM |
| `0x1fb20000..07` | Unknown read returns all ones |
| `0x1fa60000..01` | ZN-2-only SPU workaround read |
| `0x1fa51c00..1dff` | ZN-2-only MAME no-op read range: reads zero |

These `nopr` handlers use MAME's default-zero program-map unmapped value.
They differ from the explicit `unknown_r` handler at `0x1fb20000`, which returns
all ones. Returning FF for the no-op ranges makes the original Judge Dredd
interrupt dispatcher enter a daughterboard transfer path that source does not
activate, preventing the synchronous HDD boot read. Independent MAME 0.287
read taps confirm zero at both common no-op apertures.

`boardconfig_r` returns fixed bits `0x60`, VRAM bit 3 when configured for 2
MiB, and RAM size in bits 1:0 (`1` for 4 MiB, `2` for 8 MiB, `3` for 16 MiB).
This follows the upstream handler, including its result for unsupported size
values. Coin writes preserve all eight data bits. MAME increments coin counters
from bits 0 and 4 and drives lockout from the inverse of bits 1 and 5.

Writes to `0x1fa10300` pass bits 2 and 3 to CAT702 chip selects, bit 4 to the
analog mux, bit 5 to the trackball mux, and `(data & 0x8c) != 0x8c` to the MCU
select callback. `zn_board_sio_clock` and `zn_board_sio_tx` deliver the SIO0
clock/TX wires to every populated CAT702; each CAT's dataout callback identifies
the chip index so the host can connect the appropriate SIO input/DSR path.
MAME creates CAT702 instances from daughterboard machine configurations, so the
module does not populate one unless the caller explicitly asks for it.

The common MAME `zn_base` machine config sets 4 MiB main RAM and creates GPU,
SPU, EEPROM, screen, and ZNMCU devices, but the address map for CPU memory and
BIOS ROM is provided by each game/daughterboard driver. Accordingly this C
module delegates unrecognized reads/writes to optional host callbacks and
returns `ZN_BOARD_UNMAPPED` if the host also declines them. The host remains
responsible for BIOS/game ROM, main RAM, scratchpad, GPU, SPU, SIO, IRQ, MCU
analog/trackball sampling, and save-state/persistence policy. The board module
provides raw I/O callbacks and device state; it does not assemble a game profile.

`zn_board_init` copies an optional 2048-byte data image or 2080-byte full
AT28C16 image. `zn_board_advance_to_us` supplies absolute emulated microseconds;
the caller must advance it with the board clock. Programming takes 200 us,
D7 reads poll busy globally, busy writes are ignored and resets preserve the
pending write and clock. High-voltage pins remain disconnected as in MAME.
`zn_board_eeprom_import/export` preserve the full data and ID image. Destroy
an initialized board with `zn_board_destroy` before initializing it again.
Initial image buffers are copied and do not serve as a mutable backing store.
The same absolute-clock API advances a connected MCU. `zn_board_reset` is an
initialization helper: it deselects that MCU and cancels its pending timer;
its trackball history is retained. The pinned `zn_state`, CAT702, MCU and
EEPROM have no machine-reset callbacks, so native adapters must preserve the
common board and serial pins across a machine soft reset instead of calling
this helper. MCU output callbacks belong to its caller and
must be wired to SIO0 DSR and the combined serial RX input. Destroying the
board leaves the borrowed MCU alive. `zn_board_sio_clock` reports timer errors.

Capcom, Taito, Tecmo, Raizing, Acclaim, and other ZN daughterboards add ROM
banks, sound CPUs, input muxes, protection and custom registers in the same
driver source. Those overlays are not guessed or treated as supported by this
common module. In particular, CAT702 pin-level support alone does not implement
a game's security profile or daughterboard.

Run focused tests with:

```sh
make -f tests/Makefile.zn-board test
```

The tests cover configuration values, input apertures, 199/200 us EEPROM deadlines, busy writes, reset persistence,
full EEPROM data/ID import/export, MCU mux/header/analog serial forwarding,
coin latch, mux callbacks, CAT702 wire forwarding, host-map delegation, the
ZN-2 read toggle, and unmapped-address failure behavior.
