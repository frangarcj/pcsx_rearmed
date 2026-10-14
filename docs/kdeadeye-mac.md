# Dead Eye on Mac

The original `kdeadeye` package now executes through the native Konami GV
adapter with two gun ports and Sharp flash. Source remains MAME
`deb2f54fe7300e7a192690318beb860159bfd52a`, `konami/konamigv.cpp`.
`MACHINE_NOT_WORKING | MACHINE_IMPERFECT_GRAPHICS | MACHINE_IMPERFECT_SOUND`
is retained. Component and generated-guest passes are separate from the
original game's player evidence below.

## Verified original media

The shared BIOS is `999a01.7e`, 524288 bytes, SHA1
`02a82a2fe1fba0404517c3602324bfa64e23e478`. EEPROM `kdeadeye.25c` is 128
bytes, CRC `3935d2df`, SHA1 `cbb855c475269077803c380dbc3621e522efe51e`.
Flash `kdeadeye.flash` is 524288 bytes, CRC `f87f18cf`, SHA1
`67e5f76eb2b938942bd67dbb30508d4603e35f15`. The source added this freshly
initialized default in commit `e1e75e70`, dated 2026-07-05.

The current public [MDK package](https://mdk.cab/game/kdeadeye) supplied
the EEPROM/flash ZIP (8528 bytes, file SHA1
`1b1a32ca91c5755238986246999d1bf8d262e287`) and `054uaa01.chd` (377787538
bytes, file SHA1 `f76814bc1e90ccee68eaf43dd7a089f48945bbd4`). Individual
ROM CRC/SHA1 checks and the preparer's complete CHD verification pass.
The CHD v5 source identity is `a05079e4e5024ca66b7f6b81de74695d86c62dd8`,
data SHA1 `dd934897db521d9266846d5f6946ccee40485d7f`, with 549527040 logical
bytes, 19584-byte hunks and thirteen tracks: one MODE2_RAW plus twelve audio,
with RW_RAW subchannels. Native media exposes last LBA 224610. Source marks
this disk GOOD, while documenting eighty sectors with Q-subcode CRC errors.

The old Internet Archive 0.185 `kdeadeye.chd` has header SHA1
`3c737c51717925be724dcb93d30769649029b8ce`; it was rejected after reading
its header and was not substituted for `054uaa01`. Older 170-byte ROM ZIPs
contain only the EEPROM and do not satisfy the new flash declaration.

```sh
python3 scripts/prepare_konamigv_board.py --set kdeadeye \
  --mame-source ../mamedev --rompath roms \
  --archive roms/kdeadeye/kdeadeye-current.zip \
  --output-dir roms/prepared/kdeadeye
```

## Source daughterboard and Mac controls

Sharp LH28F400 occupies `1f380000–1f3fffff`, with 512 KiB storage, x16
transactions, B0/ED IDs, 64 KiB uniform erase blocks and a one-second erase
status interval. Word programming and stored bytes use the source flash
engine's big-endian storage. No unused trackball device is instantiated.

P1 X/Y are `1f680080`/`1f680090`, P2 X/Y `1f6800a0`/`1f6800b0`.
Each occupies a 32-bit MMIO slot with a 16-bit port mask and zero upper bits.
Both guns default to X `0100`, Y `0077`. `1f6800c0` is a separate buttons
port: active-low bit 0 is P1 trigger and bit 1 P2 trigger, other low bits
read high and upper bits zero. `1f6800e0` remains a write no-op. The common
P1/P2 low seven bits and P3/P4 controls are unknown/inactive in this cabinet;
unused controls are rejected rather than connected to generic GV buttons.

CSV accepts `p1gunx`, `p1guny`, `p2gunx`, `p2guny` from 0 through 65535,
matching the source port mask. The source UI's normal aiming range is
X `004c–01bb`, Y `0000–00ef`; it is not a narrower CPU register mask.
Mac mouse and each of two gamepad sticks map the displayed viewport to that
range. Left mouse click fires P1; each gamepad's south button fires its player.
P1 mouse aim retains priority until its gamepad stick moves. Outside-window
aim supplies raw X 0/Y 255 and still allows the trigger; the game decides
whether this reloads. Right click has no source P1 second button to drive.
Keyboard uses 5/6 coin, 1/2 Start, A/Z trigger, T TEST and 9 service.
Focus loss releases held controls, and replay isolates live input.

Original `flash.bin` is verified independently even on warm startup. Missing
mutable state copies that exact source seed into memory. `flash.nvram` has a
64-byte header, set-specific magic, a binding to BIOS/EEPROM/CD/original flash,
payload SHA1 and version/size fields. Wrong binding, size or payload rejects
without overwriting the existing state. Saves use a temporary file and rename.
Simpsons and Beat the Champ retain their existing erased defaults and formats.
The INI parser uses a 64-bit field bitmap: Dead Eye requires the two additional
flash fields; other GV profiles reject them. Unknown/duplicate keys and
missing fields remain errors.

## Native checks and original player runs

The component suite verifies both guns' defaults/extrema/byte lanes, independent
active-low triggers, unmapped holes, no-op writes, Sharp ID/program/erase timing,
invalid inputs and monotonic-time failures. Optimized and ASan/UBSan builds pass.
The generated MIPS guest executes real input and flash MMIO, checks unused
common ports, aliases and both triggers, programs `1234`, and verifies separate
process warm persistence plus corrupt state rejection. Both GL and NEON pass.
Production SDL tests exercise two gamepads, mouse bounds/priority, offscreen
trigger, focus release and replay isolation. Ten original-profile rejection
cases cover flash INI fields and missing/truncated/corrupt source flash.
Common GV, Simpsons and Tokimeki native regressions pass; the no-CHD build links.

```sh
make -f tests/Makefile.kdeadeye test sanitize
make -f Makefile.mac-harness GPU=vitagl-like kdeadeye-test gnet-frontend-test
make -f Makefile.mac-harness GPU=vitagl-like gp0-control-test
make -f Makefile.mac-harness GPU=neon kdeadeye-test
build/mac-harness-vitagl-like/pcsx-harness \
  --system-gv roms/prepared/kdeadeye --interactive
```

The 2400-frame original cold run reaches attract/title animation in GL and
NEON with identical RAM/SPU WAV and zero device faults. Its saved flash remains
identical to the source seed. A first one-credit replay correctly stays at
`1 MORE CREDIT TO START`; the default price is two credits. Four coin pulses
then repeated Start/trigger inputs enter P1, consume two credits, show the
instructions and advance into the first stage with the P2 HUD inactive.

At 6400 frames both renderers consume 1452/1452 events, show enemy/innocent
targets and the original `SHOOT OFF SCREEN TO RELOAD` prompt after actual
ammunition consumption. P1 has lost one life and still has score zero at that
snapshot. RAM SHA256 is
`3e7aa751c9d8ee8d112d527efaadeb92c2d7ada8ba8d28b53ddb3c6a5a334ab7`,
SPU WAV SHA256 `46006f4220f661e5d2f4a4d6bf7a207a74c9794c8c73f6cf4102fa6504ae3ff3`.
SPU delivers 4714924 stereo frames at 44100 Hz, peak 3863. Device faults are
zero, there are 154 SCSI DMA requests and the real gun/buttons ports are read.
This establishes an exercised player scene and firing; it is not scored-hit,
calibration, two-player or complete-game evidence.

The final 7200-frame replay adds offscreen X 0/Y 255 trigger pulses, then aims
at the three orange enemy targets' chest positions. The original game reloads
and registers hits: P1 score is 15, while the green innocent target remains
visible. P1 has no remaining lives at the final `PUSH BUTTON` screen. Both
renderers consume 1548/1548 events with zero device faults, 167 SCSI DMA requests,
20888 gun byte-lane reads and 5222 trigger byte-lane reads. PC `80027378`,
4076064005 cycles and 457876384 interpreter blocks match. Final RAM SHA256 is
`b443da12afee4f54d75388bbd40c0d95b9c5f64cc7266a48a0e16b3752bb6ec3`;
SPU WAV SHA256 `55e0115f291562406024686c87f3df0add5eba75840ce062a277206a012dcbd6`.
SPU delivers 5304634 stereo frames at 44100 Hz, peak 3869. Warm flash still
matches the original seed; the run does not invent installed state or hits.

This longer replay initially crashed GL with SIGBUS, independently reproduced
under LLDB. `NativeRenderer_CopyVRAM` received a nonexistent rectangle via
`ParsePrimitive`/`MoveImage`: the bridge forwarded the one-word GP0 `01h`
cache-clear command to PsyCross' unrelated five-word retail `DR_MOVE` parser.
The fix consumes GP0 control words before that retail parser, retaining `02h`
fill and normal draw packets. The backend already maintains coherent texture
caches on VRAM changes, so it needs no extra VRAM readback or synthetic copy.
The new production-bridge regression fails against the previous source and
passes after the fix, including adjacent draw packets and retained VRAM fill.
The corrected GL run reaches the same score/RAM/WAV as NEON and shuts down
cleanly. A separate Ehrgeiz USA HD interpreter regression reaches Godhand/Inoba
combat at 6200 frames, consumes 414/414 pad inputs and shuts down cleanly with
nonzero SPU. This shared bridge fix is not proof of upstream issue 8 being
resolved on physical Vita or of its original failure having the same cause.

The installed independent MAME 0.287 binary's `-listxml` lacks the newer default
flash ROM, although its CD/EEPROM match. Its first empty-flash reference reaches
attract scenes with a different flash state. A second isolated reference uses
the exact verified source flash bytes through the original flash NVRAM loader,
whose pinned `intelfsh.cpp` reads the same big-endian byte layout. No bytes are
patched or game checks skipped. With actual source coin/start/trigger/gun
fields it reaches the player instructions and the first stage. This distinction
is retained in `mame-listxml.xml` and the separate reference directories.

Evidence is retained under `artifacts/boards-mac-20261014/kdeadeye`; scratch
profiles and independent reference runs are in `/tmp/pcsx-kdeadeye-mac-20261014`.
Original package metadata continues to say `game_execution_verified=false`:
it certifies source media, not runtime. CDDA mixing, operator gun calibration,
P2 gameplay, authentic DRC, full save states and Vita remain separate
open checks. Nonzero SPU capture does not claim the twelve CD audio tracks play.
