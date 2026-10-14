# Crypt Killer / Konami System GQ on Mac

Crypt Killer (`cryptklr`, GQ420 UAA) now boots its original BIOS and HDD in
the native Mac interpreter. A 4800-frame NEON run reaches the animated attract
scene with all three INSERT COIN labels, 15802 sector reads, 314 SCSI commands,
312 DMA5 requests, nonzero source PCM audio and zero board/media faults.
Player shooting, warm EEPROM, GL presentation and live SDL audio validation
remain separate pending checks.

## Original media and source

Pinned source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`,
`src/mame/konami/konamigq.cpp`. Three source ROM regions are prepared: PSX BIOS,
MC68000 program and K054539 PCM, each 512 KiB. The sound program retains the
source `ROM_LOAD16_WORD_SWAP` assembly. Factory EEPROM is the source-owned
128-byte 93C46 default table with its license, not a substituted chip dump.

The local ROM ZIP is 599689 bytes, SHA1
`aec839d70bde345aa6db445e52910b6c3841fd67`. The original HDD was downloaded from
the public `MAME_0.149_CHDs_C.zip` member
`MAME_0.149_CHDs_C/cryptklr/420uaa04.chd`. Compressed CHD size is 167194766 bytes;
archive-file SHA1 is `62910a2c1c9d7c66853bee022a939f82753ae0f5`. This differs from
the source CHD header/data+metadata SHA1, which is
`67cb1418fc0de2a89fc61847dc9efb9f1bebb347` and matches the pinned disk declaration.
The preparer runs full `chdman verify`, validates the header and GDDD geometry,
and audits ROM size/CRC/SHA before writing the package.

CHD v5 geometry: 1122 cylinders, 15 heads, 63 sectors, 512 bytes per sector;
logical size 542868480 bytes. The source SCSI disk is writable. Original input
media and mutable runtime disk/EEPROM state must remain separate in the
native adapter; its CHD backend stores writes in `420uaa04.overlay`.

```sh
python3 scripts/prepare_konamigq_board.py --set cryptklr \
  --mame-source ../mamedev --rompath roms \
  --output-dir roms/prepared/cryptklr
make -f tests/Makefile.konamigq test
```

The six-file package preserves `game_execution_verified=false`. Preparing it
or passing a synthetic sound/DMA guest does not change that claim.

## Hardware distinction and reference baseline

GQ uses CXD8530BQ at 67737600 Hz, 4 MiB main RAM and source type-1 CXD8538Q GPU
at 53693175 Hz with 2 MiB VRAM. Its HDD uses NCR53CF96 at 16 MHz, SCSI ID 0,
initiator ID 7 and IRQ10/DRQ-gated channel-5 DMA. This differs from Judge's
ATA daughterboard. Sound is MC68000 at 8 MHz, TMS57002 at 24 MHz and two
K054539 devices at 18.432 MHz; source routes each PCM stereo output at gain 1.
It also has MB89371 at 4 MHz and three gun pairs with distinct input ports.
The standalone adapter does not model the 058141 analog module.

The current O2 component baseline passes guest sound/EEPROM/IRQ and CHD-backed
NCR/SCSI HDD READ/WRITE DMA. Independent MAME 0.287 with original media reaches
ROM/RAM checks marked OK and the SOUND check at 30 emulated seconds. That is
an intermediate reference result. A separate 120-second reference run reaches
the animated three-player attract scene (all three INSERT COIN labels),
confirming that this original media boots in MAME. Neither reference run has
player input. Native NEON boot and nonzero PCM output now pass. Shooting, calibration,
warm persistence, GL player presentation, live SDL audio and DRC remain pending.

Source/media manifests, CHD information, download/audit logs, component logs
and MAME snapshots are in `artifacts/boards-mac-20261014`. The overall board
validation goal remains active; a passing Judge row does not close GQ, Metro,
GV variants, System 573 or Twinkle.

## Native adapter and regression checks

`--system-gq REGION-DIR` independently validates exact source profile fields,
ROM hashes, factory EEPROM hash and the HDD header SHA1/geometry. It selects
4 MiB PSX RAM, the source type-1 GPU with 2 MiB VRAM, source main/sound clocks,
NCR IRQ10 and DRQ-gated DMA5. Both PSX SIO devices retain their source idle state.
The source disk response, including the original REQUEST SENSE bytes, is kept.

`--audio FILE.wav` captures the sum of both K054539 stereo routes at 48000 Hz,
with source gain 1 for each chip and final signed-16 clipping. Live SDL audio
uses the same route and only applies the user's volume. GQ has no source SPU
route. EEPROM persistence uses `--nvram FILE` and its exact 128-byte image.

The three lightguns have independent raw X (9 bits) and Y (8 bits) inputs.
CSV controls are `p1gunx`/`p1guny` through player 3, coins/start and trigger/reload
as `pNbutton1`/`pNbutton2`. Interactive Mac P1 aiming uses the viewport-scaled
mouse; left click fires, right click reloads. Three gamepad sticks provide
source gun coordinates. Shooting outside the viewport delivers a trigger with
source offscreen coordinates. Focus loss releases held buttons.

The original firmware's hardware error was a failed sound-host handshake,
not a failed disk media check. A bounded main-CPU trace proves that the media
check returns success with the original sense reply. K056800 INT had only
notified its optional observer without updating MC68000 IRQ1. Direct delivery
and acknowledgement now let the original sound firmware perform the handshake;
the game then starts genuine HDD reads. No game firmware or reply is patched.

```sh
make -f tests/Makefile.konamigq test sanitize
make -f Makefile.mac-harness BUILD=build/mac-harness-primal-neon GPU=neon systemgq-test
make -f Makefile.mac-harness BUILD=build/mac-harness-primal-gl GPU=vitagl-like systemgq-test gnet-frontend-test
```

The explicitly synthetic `--system-gq-fixture` loader executes generated MIPS
and MC68000 programs. Tests cover bit-8 gun reads, source bus lanes and I_STAT
acknowledgement, real NCR READ/WRITE via DMA5, overlay persistence across process
restart, EEPROM export, exact stereo gain-1 mixing from each real PCM register
bank, and rejection of synthetic data by normal loading. Component tests execute
a real IRQ1 handler and RTE under O2 and ASan/UBSan. Adjacent GV, Judge and NBA
native fixture regressions also pass. Builds without BOARD or CHD explicitly
reject GQ. These checks do not establish player compatibility by themselves.

The 4800-frame boot takes 80.232 emulated seconds. The raw WAV contains 3851156
stereo frames at 48000 Hz, 32469 nonzero stereo frames, peak 1622, no clipping
and no unmatched chip samples. The final capture is `cryptklr-irq1.png` in the
validation evidence directory. DASP/058141 analog routing, authentic DRC,
whole-machine save states and physical Vita execution remain unvalidated.
