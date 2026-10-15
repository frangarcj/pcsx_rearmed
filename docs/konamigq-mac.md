# Crypt Killer / Konami System GQ on Mac

Crypt Killer (`cryptklr`, GQ420 UAA) now boots its original BIOS and HDD in
the native Mac interpreter. A 4800-frame NEON run reaches the animated attract
scene with all three INSERT COIN labels, 15802 sector reads, 314 SCSI commands,
312 DMA5 requests, nonzero source PCM audio and zero board/media faults.
A separate 6500-frame GL/NEON replay reaches P1 gameplay, and its live SDL
audio matches the raw native output exactly. A separate 6580-frame warm GL restart
verifies reload and retained EEPROM. A further 7200-frame GL/NEON replay now
registers 2,000 original-game points and retains two hearts, compared with zero
points and one heart in its control run.

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
player input. Native NEON boot and nonzero PCM output now pass. GL/NEON player entry and live SDL audio now pass. A warm GL restart also verifies reload and retained EEPROM. Calibration
and DRC retain separate checks.

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

## Player replay and exact SDL audio

The first replay used two-frame coin/start pulses and consumed all inputs but
stayed in attract with CREDIT 0; it is not a player pass. Sixteen-frame pulses
and four inserted coins enter P1 gameplay. An independent MAME input check
shows CREDIT 1/2 after a single coin with the factory settings.

The 6500-frame replay consumes 382/382 events. Both GL and NEON show P1 with two
hearts, a skeleton in a corridor and the RELOAD prompt after emptying the gun.
Both runs read 26892 HDD sectors across 414 SCSI commands and 412 DMA5 requests,
with zero faults/errors, and finish at the same PC and source time. Their entire
4 MiB RAM dumps match (SHA256
`dad4453a0408bdd148c54736d8b1b2a1392b163e65886301e8c06199d3153e0a`).

Both source WAVs and the real interactive SDL capture are byte-identical:
5215107 stereo frames at 48000 Hz, 1403510 nonzero frames, peak 11982,
zero clipping and no unmatched chip samples. WAV SHA256 is
`c0022cb596145d4bd2f2e87f944d5cfadc874ce8f0bf9e7382d960e1117d5161`.
Live capture finishes with `Interactive GQ audio: error=0`. GL capture uses the
actual OpenGL back buffer. The source EEPROM remains the original 128-byte image
in these runs. The separate warm restart/reload run below verifies those paths.

Logs, input CSV, GL/NEON PNGs and `cryptklr-player-validation.json` are stored in
`artifacts/boards-mac-20261014`. WAVs and complete RAM dumps remain under
`/tmp/pcsx-boards-mac-20261014`. This is bounded P1 coverage; a scored hit,
three simultaneous players, full-game completion and physical Vita are not
established by these captures.

## Warm restart and reload

A new GL process imports the previous run's 128-byte EEPROM and repeats the
player sequence. At frame 6500 it holds `p1button2` for 16 frames with no later
shots. The final frame 6580 retains the P1 HUD and two hearts, removes the RELOAD
prompt, and shows refilled ammunition. The prior frame-6500 run has the empty
ammo prompt. This verifies reload through the original game's digital input.

The warm run consumes 384/384 events, reads 26892 sectors, executes 414 SCSI
commands and 412 DMA5 requests, and reports zero board/media faults. Audio is
5279293 stereo frames, 1467696 nonzero frames, peak 11982, zero clipping and no
unmatched samples. EEPROM is byte-identical before/after, source SHA1
`d9ff0458478ee42b7b14f101be96dd2e45edbba3`; the game does not need to rewrite
factory settings during this short replay. The original HDD has no game writes
in these runs. The independent native fixture proves overlay writes persist;
it is not substituted for a claim that the authentic game wrote sectors.

Evidence includes `cryptklr-warm-gl.log`, `cryptklr-warm-gl.png`, the paired
before/after EEPROM images and `cryptklr-player-reload.csv` in the validation
directory. This closes bounded Mac P1 startup/shooting/reload/audio/persistence
coverage. Scored hits are established by the following replay. The following multi-player checks extend this to three active players;
calibration, full analog sound routing, DRC, save states and physical Vita
retain separate checks.


## Scored P1 hit with a control run

The 2026-10-15 replay extends the existing startup/reload sequence. After the
source gun has been reloaded at frames 6,500–6,516, it sends three ordinary
trigger pulses at 6,530, 6,600 and 6,670 (each held 16 frames), raw X=243 and
Y=150/135/135. The source gun conversion subtracts 125 from raw X, so the
first shot reaches screen position (118,150), over the corridor skeleton.
An intermediate 6,580-frame capture shows the struck enemy's weapon flying
away and a current score of 1,000 in original guest RAM.

At frame 7,200, the three-shot GL and NEON runs each have **2,000 points and
two hearts**. The NEON control executes the same first 384 events, including
all earlier misses and the reload, and adds no later shots: it has **zero
points and one heart** at the same frame. All 396 positive-run events and
384 control events are consumed. The capture shows the additional heart;
this HUD does not show the numeric score during the round.

Read-only inspection of the original program identifies the P1 array at
`802377c8` (player stride `58` hex), with lives at `802377cc` and current
score at `802377d4`. The actual hit path adds 100 at `8003e230`; exhausted
enemy HP adds another 900 at `8003e280`. The result rendering path also reads
that same per-player score field. No game code, score, enemy data or initialized
game state is modified to produce this evidence; only cabinet input events are
supplied. The disassembly notes are retained as `score-observation.txt`.

All runs execute the original BIOS/HDD and have zero board/disk/sound faults,
26,892 HDD reads, 414 SCSI commands and 412 DMA5 requests. Both positive runs
produce exactly 5,776,734 stereo PCM16 frames at 48 kHz, with peak 11,982,
no clipping and no unmatched chip samples. Their complete artifacts match:

| Positive-run artifact | GL/NEON SHA256 |
| --- | --- |
| 4 MiB main RAM | `6e44aa31f8c34a370119187e903b65a53384b7f7fd1c9cbc0185d0e4c4230653` |
| Dual-K054539 WAV | `720a13846336389b3b4d6bbf8528bdd62b5742282a500f52e298229e40f39c7f` |
| 128-byte EEPROM | `9fefc91150cce83d3e5ad3906eefc62f781cb234e5d7cb53a8712613744df81f` |

The EEPROM remains the source factory table. Original disk writes remain zero.
VRAM dumps differ between the two renderer paths and are retained separately;
the equality checks above cover main RAM, EEPROM and sound. GL uses its actual
back-buffer capture, and both renderer captures have been visually inspected.
The earlier actual player images are `cryptklr-long-gl.png` and
`cryptklr-long-neon.png`; `cryptklr-player-gl.png` retains the earlier failed
entry attempt described above.

`tests/check_systemgq_original_harness.py` generates both exact input streams,
runs the normal original-media loader for a NEON control and GL/NEON positive
runs, and checks the scores/lives, original award instructions, all inputs,
source identity, native device/audio diagnostics, factory EEPROM and equal
artifacts. The new original runs pass its `--verify-only` mode. The combined
runner commands match those used for the captured runs; the combined launcher
has not itself been rerun as one command.

```sh
python3 tests/check_systemgq_original_harness.py \
  --neon build/mac-harness/pcsx-harness \
  --gl build/mac-harness-vitagl-like/pcsx-harness \
  --profile roms/prepared/cryptklr --evidence-dir /tmp/cryptklr-scored-hits
python3 tests/check_systemgq_original_harness.py --verify-only \
  --evidence-dir artifacts/boards-mac-20261015/gq/cryptklr
```

This closes the scored-hit gap in bounded P1 Mac validation. The following
checks extend simultaneous-player coverage. Gun calibration, analog module
routing, complete game progression, DRC and physical Vita retain their own checks.


### Multi-player follow-up

Two further NEON 7,200-frame probes (`three-player.csv` and
`three-player-staggered.csv`) activate P1 and P2 simultaneously. Each player
has 1,000 current points; P1 has one heart and P2 has two. All 452/452 and
464/464 events are consumed with zero device/audio faults. P3 still shows
INSERT COIN and remains inactive in guest RAM. Adding separated Coin 3/Start 3
pulses in the second probe produces identical final RAM and audio, so this
is not counted as successful three-player validation.

An independent MAME 0.287 process starts from its own source-default EEPROM
and receives the staggered CSV through the actual source ioport fields.
At 120.4 emulated seconds all three players are active: P1 has 4,000 points,
P2 has 1,000 and P3 has zero, with lives 2/2/3. Its final EEPROM exactly
matches the native source factory image. The callback uses the native
source-derived video rate to schedule CSV frames; it does not establish
cycle-identical input delivery between emulators. The reference Lua, log,
snapshot and NVRAM are retained under `mame-3p`. The native/reference entry
and progression discrepancy remains under investigation. The positive P1
score result above and the cooperative P1/P2 run are separate evidence.


### Three active players with common credit

A subsequent normal-cabinet replay adds four Coin 1 pulses at frames
5,400/5,460/5,520/5,580 and P3 Start at 5,640/5,760. It activates all three
players in native execution. A final P3 aim at raw (330,170), followed by a
trigger at frames 7,120–7,136, proves that the game reads a distinct third-gun
position. No game data or source devices change between these probes; the
successful entry uses actual additional common-cabinet coin/start input.
This is separate from validating an isolated Coin 3 entry sequence or matching
MAME's phase of entry exactly, which remain open.

The final 7,200-frame GL and NEON runs consume all **480/480 events** and
show all three colored player HUDs with active shotguns. Original RAM has
active state `0101` for each player and the following independent state:

| Player | Current score | Lives | Last game-latched screen aim |
| --- | --- | --- | --- |
| P1 | 1,000 | 2 | (118,135) |
| P2 | 1,000 | 1 | (118,150) |
| P3 | 0 | 3 | (205,170) |

The gun positions are checked in each original player structure at offset
`14` hex from its base. A changed raw port alone does not establish that the
guest consumed it; the final P3 shot supplies that read/latch evidence. P3
fires into the empty sky in this bounded capture, so this check does not
claim a successful P3 enemy hit. All three reload/trigger sequences are sent
through the existing cabinet API.

Both runs again have zero board/disk/sound faults, the same original-sector
counts and exactly 5,776,734 stereo frames at 48 kHz. Peak is 11,607, with
zero clipping/unmatched samples. The complete GL/NEON artifacts match:

| Three-player artifact | GL/NEON SHA256 |
| --- | --- |
| Main RAM | `6b4680f81166fc98bf0cc8a59a803c328a97e6fac01de0faface65682e40d8b2` |
| Dual-K054539 WAV | `bed5503270e7227d761183a6783107870a2e13b7874bcf862e1a6070ccf4f92d` |
| EEPROM | `9fefc91150cce83d3e5ad3906eefc62f781cb234e5d7cb53a8712613744df81f` |

The `three-player-final-*` captures have been visually inspected. The original
verifier's `--three-players` option generates this exact CSV, runs both extra
original-media replays and requires active states, all scores/lives/latched gun
positions, diagnostics and equal RAM/WAV/EEPROM. The collected five original
runs pass `--verify-only --three-players`. No physical three-controller setup
or mouse/gamepad multi-player session is established by these CSV replays.

```sh
python3 tests/check_systemgq_original_harness.py --verify-only --three-players \
  --evidence-dir artifacts/boards-mac-20261015/gq/cryptklr
```
