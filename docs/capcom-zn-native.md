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
