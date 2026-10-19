# Native Capcom ZN host adapter

`libpcsxcore/systemzn.c` composes the pinned Sony ZN common-board devices with the Capcom ZN daughterboard for the 43 currently packageable `capcom_zn_state` game profiles. Its profile table comes from `scripts/capcom_zn_boards.json` through `scripts/generate_capcom_zn_profiles.py`; both are pinned to MAME `sony/zn.cpp` revision `deb2f54fe7300e7a192690318beb860159bfd52a`. Regenerate and verify the table with:

```sh
python3 scripts/generate_capcom_zn_profiles.py
python3 scripts/generate_capcom_zn_profiles.py --check
```

The loader accepts only the preparer's exact 14-key `board.ini`, one of those 43 profiles, and each source-sized region: 512 KiB selected BIOS, 36 or 48 MiB banked ROM, 512 KiB country ROM, 256 KiB audio ROM, 4 MiB QSound samples, two exact 8-byte CAT702 keys, and the exact 8 KiB QSound HLE firmware payload. The DSP image is decoded as 4096 little-endian logical 16-bit words. All regions must match their declared sizes; missing or malformed inputs fail before CPU initialization.

The two BIOS roots and two Gallop Racer `glpracr_state` games are rejected. The source config instantiates the ZNMCU rather than the separate undumped `upd78081`, so only the preparer's specifically reviewed, unused `upd78081` NO_DUMP block is omitted; no substitute firmware is generated. The source's 16-entry, 4 MiB bank selector is retained, with a read bounds fault whenever a complete selected window exceeds the actual declared banked-ROM region. No upper bytes are synthesized.

The adapter configures the PSX main RAM size, BIOS image, GPU type 2 and VRAM geometry from the profile: COH1000C uses 1 MiB VRAM; COH1002C and COH3002C use 2 MiB. The source CPU rates (67.7376 MHz for ZN-1, 100 MHz for ZN-2) and GPU rate are validated from `board.ini`. With generic arcade source timing enabled, `PSXCLK` is the source CPU input clock / 2: 33.8688 MHz for ZN-1 and 50 MHz for ZN-2. The adapter converts those cycles to the 8 MHz Z80 budget and fixed 33.8688 MHz SIO time with retained fractional remainders. SIO callbacks and MCU/EEPROM deadlines share the fixed serial time domain. Exact whole-machine timing equivalence remains unverified.

Both PSX SIO blocks are owned by this adapter and mapped at `0x1f801040..0x1f80105f`. SIO0 SCK and TXD drive the common CAT702/ZNMCU lines. SIO0 RXD is the wired-AND of CAT702 0, CAT702 1 and MCU TXD, matching MAME's `update_sio0_rxd`; the MCU DSR output is returned to SIO0. SIO0 and SIO1 IRQ callbacks set their respective I_STAT requests. Each SIO output callback advances the MCU and AT28C16 to the SIO event cycle before changing a pin, then the final board sync advances them to the host cycle. MAME-default ZNMCU analog ports are `0xff`, DIP input is `0xff`, and trackball values are zero.

Inputs are active-low raw MAME ports. `systemzn_set_input` accepts only controls actually wired by the selected Capcom input profile; the two-player button and KICK harness remaps differ among `capcom4b`, `capcom6b`, `plsmaswd`, `strider2`, and `tgmj`. Unsupported or unknown controls return an error instead of setting a guessed bit. Coin input bits remain distinct from the common-board coin-counter register.

QSound uses the actual MAME-default HLE component and verified caller media. The optional WAV capture contains only QSound stereo PCM at the source integer rate 24,038 Hz; it does not mix PSX SPU output. The loader imports/exports the exact 2080-byte AT28C16 data-and-ID image through `systemzn_eeprom_import/export`.

The native core and harness route memory accesses and CPU reset/synchronization
through this adapter. Run its generated guest tests with:

```sh
make -f Makefile.mac-harness capcom-zn-test
```

The fixtures execute MIPS BIOS code and Z80 sound code for COH1000C, COH1002C
and COH3002C. They verify JALR calls through fixed and selected ROM banks,
independent upper RAM, country ROM, EEPROM programming, the real SIO/MCU
header and IRQ7, frame-indexed input press/release, GPU fill/readback and
nonzero QSound PCM from ten executed commands. Separate checks cover both
VRAM geometries, instruction-cache fetches, exact EEPROM data/ID persistence
and rejection of malformed profiles before guest execution. All fixture
ROMs, keys and coefficients are generated test data; authentic media is
verified by the preparer. Street Fighter EX, Star Gladiator and Rival Schools
now pass [short Mac interpreter/GL gameplay checks](capcom-zn-mac.md) with
nonzero QSound capture. The fixtures also independently verify main CPU to
Z80/SIO clock ratios. Full timing equivalence and the mainboard SPU audio mix
remain pending.

## DRC and Vita core 2

Capcom profiles now retain the selected MIPS DRC in both the production
libretro core and the native harness. A code-span provider exposes the fixed
ROM, selected 4 MiB ROM bank and country ROM through physical and KSEG aliases.
Changing the bank invalidates translations before publishing the new mapping.
Cross-window delay slots use the live interpreter path; RAM retains normal
self-modifying-code invalidation. Both SIO blocks use the board handlers under
the DRC. The shared ROM LUT helper also now advances the source across pages;
it previously repeated the first page throughout each mapped window.

The optional `pcsx_rearmed_system12_board_thread` worker runs the interpreted
Z80 and QSound on Vita core 2. MIPS/DRC remains on core 0 and the NEON GPU worker
on core 1. SIO, MCU, EEPROM, inputs and main IRQ scheduling stay on main.
Sound latch writes drain earlier work before delivering the Z80 NMI. Queue
publication groups up to 64 advances while preserving every original cycle
budget and deadline. A bounded PCM ring delivers callbacks on main and pumps
while waiting, including when one advance exceeds the ring's capacity. Reset,
worker changes and unload drain pending work before changing device ownership.
Other ZN daughterboards retain their existing CPU and sound paths.

Generated ZN-1 (`sfex`) and ZN-2 (`rvschool`) production-libretro checks cover
interpreter/DRC, serial/threaded sound, bank replacement and aliases, mapping
boundary delay slots, RAM code rewrites, SIO/MCU/EEPROM timing and IRQs, KICK
inputs, Z80 latch NMIs, ring backpressure, reset and worker restart. Both reset
passes produce identical audio counts and hashes with and without the worker.
The profile-switching test also checks worker teardown and restoration when
moving between System 10, System 11, Capcom ZN and console layouts.

```sh
python3 scripts/check_validation_storage.py --reserve-mib 96 && \
  python3 tests/check_zn_drc.py \
    --core build/mac-libretro/src/pcsx_rearmed_libretro.dylib \
    --host build/mac-libretro/test-zn-drc
```

The test host is built from `tests/test_zn_drc_libretro.c` with the libretro
headers and `-ldl`. On 2026-10-19, verified real Street Fighter EX media also
completed 1800 scripted NEON frames in all four CPU/worker combinations.
All audio WAV hashes matched; serial/threaded per-frame video hashes matched
within each CPU backend. Interpreter and DRC video sequences differ in timing.
Compact local evidence is in `artifacts/validation/zn-drc-thread-20261019/`;
raw WAV/PPM intermediates were hashed and removed. This does not establish
physical Vita performance or validate all 43 titles.

The NEON Vita executable also completed the same 1800-frame/42-event replay
in Vita3K with both GPU and board workers enabled. Its report confirms ari64,
43,875 sound work items containing 2,663,609 original advances, 240,677,278
Z80 cycles, 723,175 QSound frames, zero unsupported Z80 operations and zero
board faults; the frontend reports zero audio errors. Separate controls with
either worker disabled completed with the same final board state. The tested
executable SHA-256 is
`26fd2d0d56f8a2b4f37c953710c7b02ef34b436dfe5ba87c5e75f864fe08c35f`.
This updates the standalone NEON eboot, not the older root VPK.

Vita3K did not exit cleanly: completed runs reached `Stopping -> Idle` before
its existing `free(MemState)` host crash. The first run with both workers
instead aborted during play in the host's `handle_timeout/condvar_wait`
with `condition_variable wait failed: Invalid argument`; a repeat completed
without changing the guest executable. Preserve that intermittent host
failure separately from the successful guest result. Recoverable native
access signals followed by guest progress were allowed throughout.


The shared native `systemzn` motherboard now also has an explicit
`--tecmo-tps` loader for the source-reviewed common TPS map. That path uses
8 MiB ROM banks and PSX SPU rather than this Capcom daughterboard. Its profiles
and source CRC/padding checks are separate; the existing Capcom fixture remains
passing. See [Tecmo TPS Mac validation](tecmo-tps-mac.md).
