# Tokimeki Seal Version sensors and virtual printer on Mac

The authentic `tmoshs` BIOS, EEPROM and CD now execute in the native Mac GV
interpreter with GL and NEON. A bounded player replay reaches heroine selection
and a conversation/date scene; the game displays its heartbeat value. The
original operator PRINTER CHECK / TEST PRINT also produces an actual 800x600
RGB page through the game's own timed serial commands. This establishes an
operator test print, not a completed romantic-game result or a physical printer.

## Audited media

Source is MAME `deb2f54fe7300e7a192690318beb860159bfd52a`,
`src/mame/konami/konamigv.cpp`. BIOS `999a01.7e` is 512 KiB, SHA1
`02a82a2fe1fba0404517c3602324bfa64e23e478`; EEPROM `tmoshs.25c` is 128 bytes,
SHA1 `f18a0974a6be69dc179706643aab837ff61c2738`. CD `755jaa01.chd` has source
CHD SHA1 `fc742a0b763ba38350ba7eb5d775948632aafd9d`, data SHA1
`4a02e0990bef2051e2403467a1228ad7dc1ce103`, twelve tracks and 127984 last LBA.
Full `chdman verify` and the preparer's ROM CRC/SHA1 checks pass. Native loading
independently checks the pinned profile, BIOS/EEPROM hashes and CHD identity.
The original media and mutable EEPROM remain separate.

## Native peripheral path

The sensor/printer port at `1f680080` / `1f680090`, GPIO identity rotation,
heartbeat latch and GSR ADC follow the source protocol in `tokimeki-io.md`.
Default simulated heartbeat input 31 represents 80 bpm; input zero selects
Direct mode. GSR takes a raw value from 0 through 128. Inputs supply measurements
to the game; the emulator does not invent its assessment or score.

The game's memory-in command captures the actual renderer output at the next
rising vblank. GL reads the presented framebuffer only for that command;
NEON uses its presented RGB buffer. The logical image is copied to source-sized
800x600 paper. A validated print command marks the printer busy for one virtual
second and optionally writes `print-0001.ppm`, then successive numbered pages.
Unknown printer OSD commands remain unmodeled, as in the reference driver.

The optional output directory must already exist:

```sh
make -f Makefile.mac-harness GPU=vitagl-like BUILD=build/mac-harness-primal-gl -j8
mkdir -p /tmp/tokimeki-prints
build/mac-harness-primal-gl/pcsx-harness \
  --system-gv roms/prepared/tmoshs --interactive \
  --printer-dir /tmp/tokimeki-prints --frames 20000
```

Use `5` for a coin, arrows and `A` to select. `+`/`-` adjusts the simulated
heartbeat raw input by ten; `H` switches to Direct and sends a pulse.
PageUp/PageDown adjusts GSR by two. CSV controls are `heartbeat`, `gsr`,
`heartbeat_pulse`, `printer_empty` and `printer_fault`, in addition to the
source buttons, coin, service and test. Controls unused by this cabinet are
rejected rather than translated into additional buttons.

## Original-game evidence

Scratch evidence is under `/tmp/pcsx-boards-mac-20261014`. The 7200-frame player
replay consumes 98/98 events, reaches Shiori selection and displays heartbeat
080. The longer GL replay reaches the date/conversation scene. Both renderer
paths report zero peripheral and board faults. These runs do not finish a game
or exercise its final printed report.

For the operator print, press test at frame 3200 and release at 3216. At frame
4260 press Up, at 4320 Up again, then select at 4380 to enter PRINTER CHECK.
Select TEST PRINT at 5160; each direction/button is released twelve frames
later. Both 6000-frame GL and NEON replays consume 10/10 events, decode 44 printer
packets with zero rejected packets, capture twice and print once. Each
800x600 page contains the actual colorful Konami test artwork on white paper;
41 unknown OSD commands are recorded separately from device faults.

Both runs end at the same CPU cycle, block count and PC with zero board faults.
The exported pages show the same test artwork but are not pixel-identical:
GL includes scaling/raster differences and a black strip on the right. This
is a remaining printer-image fidelity issue; it does not count as byte-exact
reference output. NEON provides the cleaner logical image in this replay.

A separate original I/O CHECK replay enters that menu with the same test
sequence, then selects the first item at 4260. At 4500 it supplies GSR 64 and
heartbeat input 51; the 5100-frame screenshot displays `GSR SENSOR:40`, the
expected hexadecimal value. All 6/6 events are consumed with zero faults.

## Verification and limits

The component tests cover identity rotation, heartbeat deadlines, GSR serial
readout, packet rejection, one-second busy timing and all sixteen Plus cells,
under O2 and ASan/UBSan. `make -f Makefile.mac-harness tokimeki-test` uses an
explicit generated MIPS guest to exercise native GPIO, CPU-timed printer
pulses and capture of a real red GPU frame. The fixture cannot pass as authentic
media. SDL tests exercise the production heartbeat/GSR keyboard controls;
builds with BOARD=0 or HAVE_CHD=0 also compile and reject unavailable GV paths.

The `tmoshsp` source profile is accepted and its Plus page layout has component
coverage, but its original game/media have not been exercised. Base `tmosh`
and `tmoshspa` remain outside the accepted native profiles. Existing preparer
manifests retain their documentary unsupported-I/O label; the loader accepts
that exact legacy label for the two supported profiles.

CDDA media time now advances before playback/status commands, but CDDA PCM is
not yet mixed into speaker output. Nonzero SPU output therefore does not prove
complete speech/music. Physical printing, final game reports, authentic DRC,
physical Vita and whole-machine save states remain unvalidated.
