# Representative arcade board validation on Mac

The current goal is to run representative games across the PlayStation-derived
arcade board families. A ROM/profile parser, a synthetic guest or a passing
component test does not establish game compatibility. A passing row requires
observed player gameplay, exercised inputs, recorded device diagnostics and
appropriate sound evidence; title/demo/installation screens are recorded as
intermediate results. Physical Vita is outside this Mac pass.

| Family / configuration | Representatives | Current runtime evidence |
| --- | --- | --- |
| Namco System 12 | Soul Calibur, Tekken 3/Tag; special gun/JVS/link/CDXA cabinets | Existing bounded Mac gameplay; see `system12.md` and `system12-special-mac.md` |
| Namco System 11 | Tekken, Tekken 2, Soul Edge | Existing bounded Mac interpreter/NEON/GL gameplay; `system11-mac.md` |
| Namco System 10 MEM(M)/MEM(N) | Mr. Driller 2/G, Gamshara | Existing bounded Mac gameplay and SPU capture; `system10-mac.md` |
| Capcom ZN-1/ZN-2 | Street Fighter EX, Star Gladiator, Rival Schools | Existing bounded GL gameplay and QSound; `capcom-zn-mac.md` |
| Taito G-Net COH1002T/COH3002T, all four card types | Psyvariar Japan, Go By RC, Mawasunda, G-Darius G | Four bounded interpreter/GL player runs; interactive SDL analog controls, rotation and mixed source-clock SPU/Zoom audio; `gnet-mac.md` and `gnet-mac-interactive.md` |
| Taito FX1S | Psychic Force (`psyforce`) | Native interpreter/GL PLAYER1 selection and combat, 146/146 inputs, nonzero YM2610B and zero watchdog/device faults in the later replay; `taito-fx1-mac.md`. Earlier coin sequence still resets in native and MAME |
| Taito FX1Z, 1/2 MiB VRAM | RayStorm (`raystorm`), G-Darius Ver.2 (`gdarius2`) | Both interpreter/GL interactive player runs pass with exercised controls, live SPU/Zoom mix, persisted FRAM/EEPROM and zero device faults; `taito-fx1z-mac.md` |
| Tecmo TPS common SPU map | Gallop Racer 2 (`glpracr2`) | Interpreter/GL interactive player race, 138/138 inputs, exact source-gain SDL/SPU capture and zero board faults; `tecmo-tps-mac.md` |
| Tecmo TPS sound/link variants | Cool Boarders Arcade Jam (`cbaj`), Gallop Racer 2 Link (`glpracr2l`) | Z80/YMZ280B/FIFO and Z80 link configurations require separate native coverage; media/runtime pending |
| Video System/Visco ZN-1 | Aero Fighters Special (`aerofgts`) | Source COH1002V configuration uses common ZN/SPU plus its own ROM map and portrait presentation; media/adapter/runtime pending |
| Atlus ZN-1 | Heaven's Gate (`hvnsgate`) | Source COH1001L adds 68000/YMZ280B; media/adapter/runtime pending |
| Time Warner/Atari ZN-1 | Primal Rage II (`primrag2`) | Source COH1000W uses 8 MiB RAM, VT83C461 HDD and DMA5, with a 600 ms watchdog; media/adapter/runtime pending |
| Acclaim ZN-1 ROM/sound | NBA Jam Extreme (`nbajamex`) | Source adds SRAM/bank mapping and Acclaim RAX sound; media/adapter/runtime pending |
| Acclaim ZN-1 disk | Judge Dredd (`jdredd`) | Source adds ATA HDD/IRQ10 and a separate VBlank handler; media/adapter/runtime pending |
| Metro ZN storage variants | Bust a Move 2 (`bam2`, `bam2a`) | Source has HDD/CD configurations and marks both nonworking with imperfect sound. Complete native integration and game evidence remain pending |
| Eighting/Raizing ZN | Bloody Roar 2 (`bldyror2`), Brave Blade (`brvblade`) | Additional public split archives downloaded and wrapper hashes checked; source chip audit, adapter and runtime pending |
| Konami System GV common map | Hyper Athlete (`hyperath`) | Authenticated BIOS/EEPROM/CD prepared. Interpreter/GL enters a P1 running event with 110/110 input events, nonzero SPU and no device faults; `konamigv-mac.md`. CDDA mixing and authentic DRC remain pending |
| Konami System GV special maps | Trackball, shooting, flash and printer variants | Separate source peripherals; representative selection/runtime pending |
| Konami System GQ | Crypt Killer (`cryptklr`) | ROM archive available; CHD and native board runtime pending |
| Konami System 573 | ROM/flash and analog/digital sound generations | Source devices/components only; representative media, complete board and runtime pending |
| Konami Twinkle | beatmania IIDX | BIOS archive available; game media, native PSX integration and runtime pending |

The pinned `zn.cpp` inventory also contains the distinct Sony ZN daughterboards
and storage/sound variants listed above. Passing Capcom or Raizing alone does
not establish their compatibility. Regional clones do not substitute for distinct CPU/GPU,
sound or peripheral configurations. Retail PlayStation is inventoried separately
from the arcade boards.

## Current next checks

- Psychic Force: PLAYER1 selection and combat pass with the later coin/start
  replay. Independent MAME reaches both screens, and its independently configured
  EEPROM matches native byte for byte. The earlier coin sequence still enters a
  VSync wait from an IRQ callback and resets in both implementations; retain it
  as a timing/callback investigation. Full live SPU/YM mixing and DRC remain open.
- Hyper Athlete: the source DMA `dma_finished` write callback now delivers
  CHCR `01000201` data. The 28-byte MODE SELECT no longer stalls, and the P1
  running event passes a bounded interpreter/GL run. A native MMIO regression
  requires MODE SELECT to change the target's block size. CDDA mixing is still
  pending; authentic ARM64 DRC still uses console serial MMIO handlers.
- Common Tecmo TPS now passes Gallop Racer 2 player racing and live native sound.
  FX1Z now passes RayStorm/G-Darius Ver.2 with its ROM-backed Zoom daughterboard
  and both VRAM configurations. Proceed to Raizing and the remaining source
  families; TPS sound/link variants retain separate pending rows.

Local evidence is in `artifacts/boards-mac-20261014`; scratch/reference runs are
under `/tmp/pcsx-boards-mac-20261014`. The user-linked Internet Archive collection
supplied Psychic Force, RayStorm, G-Darius Ver.2, Gallop Racer 2 and Hyper Athlete.
Additional public archives come from `mame-0.260-roms-split_202310`; their archive
size/SHA1 checks do not replace the pinned source ROM/CHD audit. MAME source is
`deb2f54fe7300e7a192690318beb860159bfd52a`, reference emulator 0.287.

No missing firmware, keys or CHDs have been substituted. Original BIOS/ROM/EEPROM
packages remain separate from mutable runtime NVRAM. ROMs, CHDs, binaries and
captures are not committed.
