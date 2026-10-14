# Crypt Killer / Konami System GQ Mac preparation

Crypt Killer (`cryptklr`, GQ420 UAA) is the next representative for native Mac
integration. The original ROMs and HDD are prepared and verified, and the
standalone GQ component baseline passes. This is **not a native game pass**:
`konamigq.cpp` still needs a PCSX execution/frontend adapter, DMA5 routing,
source GPU configuration, real gun inputs, sound output and persisted EEPROM.

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
media and mutable runtime disk/EEPROM state must remain separate when the
native adapter is connected; the CHD backend already supports separate overlays.

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
player input. Native PCSX boot,
shooting, calibration, nonzero sound, warm persistence, GL/NEON and DRC remain
pending.

Source/media manifests, CHD information, download/audit logs, component logs
and MAME snapshots are in `artifacts/boards-mac-20261014`. The overall board
validation goal remains active; a passing Judge row does not close GQ, Metro,
GV variants, System 573 or Twinkle.
