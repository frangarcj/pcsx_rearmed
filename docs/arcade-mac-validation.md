# Representative arcade board validation on Mac

The current goal is to run representative games across the PlayStation-derived
arcade board families. A ROM/profile parser, a synthetic guest or a passing
component test does not establish game compatibility. A passing row requires
observed player gameplay, exercised inputs, recorded device diagnostics and
appropriate sound evidence; title/demo/installation screens are recorded as
intermediate results. Physical Vita is outside this Mac pass. Raw captures
pruned under [the storage policy](build-storage.md) remain historical results;
retained logs/CSVs/hashes do not constitute a fresh audio or gameplay rerun.

| Family / configuration | Representatives | Current runtime evidence |
| --- | --- | --- |
| Namco System 12 | Soul Calibur, Tekken 3/Tag, Ehrgeiz; special gun/JVS/link/CDXA cabinets | Existing bounded Mac gameplay. Ehrgeiz World now passes interpreter GL/NEON Stage 1 player combat, 250/250 events, identical RAM/C352 WAV and zero unmapped/H8 unsupported operations; `ehrgeiz-mac.md`. See also `system12.md` and `system12-special-mac.md` |
| Namco System 11 | Tekken, Tekken 2, Soul Edge | Existing bounded Mac interpreter/NEON/GL gameplay; `system11-mac.md` |
| Namco System 10 MEM(M)/MEM(N) | Mr. Driller 2/G, Gamshara | Existing bounded Mac gameplay and SPU capture; `system10-mac.md` |
| Capcom ZN-1/ZN-2 | Street Fighter EX, Star Gladiator, Rival Schools | Existing bounded GL gameplay and QSound; `capcom-zn-mac.md` |
| Taito G-Net COH1002T/COH3002T, all four card types | Psyvariar Japan, Go By RC, Mawasunda, G-Darius G | Four bounded interpreter/GL player runs; interactive SDL analog controls, rotation and mixed source-clock SPU/Zoom audio; `gnet-mac.md` and `gnet-mac-interactive.md` |
| Taito FX1S | Psychic Force (`psyforce`) | Native interpreter/GL PLAYER1 selection and combat, 146/146 inputs, nonzero YM2610B and zero watchdog/device faults in the later replay; `taito-fx1-mac.md`. Earlier coin sequence still resets in native and MAME |
| Taito FX1Z, 1/2 MiB VRAM | RayStorm (`raystorm`), G-Darius Ver.2 (`gdarius2`) | Both interpreter/GL interactive player runs pass with exercised controls, live SPU/Zoom mix, persisted FRAM/EEPROM and zero device faults; `taito-fx1z-mac.md` |
| Tecmo TPS common SPU map | Gallop Racer 2 (`glpracr2`) | Interpreter/GL interactive player race, 138/138 inputs, exact source-gain SDL/SPU capture and zero board faults; `tecmo-tps-mac.md` |
| Tecmo TPS Z80/YMZ280B/FIFO | Cool Boarders Arcade Jam (`cbaj`) | Source-audited; native interpreter GL/NEON P1 downhill race, 200/200 inputs, original Z80/YMZ sound, dual FIFO startup/commands, live mixed SPU/YMZ audio and zero faults; `cbaj-tps-mac.md` |
| Tecmo TPS Z80 link | Gallop Racer 2 Link (`glpracr2l`) | Source-audited; interpreter GL/NEON local player race, 138/138 inputs, real extra Z80 and source-gain SPU audio pass. Source link ports are unmapped; cabinet transport remains unimplemented/unvalidated; `tecmo-tps-link-mac.md` |
| Video System/Visco ZN-1 | Aero Fighters Special (`aerofgts`) | USA source-audited; native interpreter/GL 1P shooting, 112/112 inputs, portrait presentation, exact source-gain SPU/SDL capture and zero faults; `visco-zn-mac.md` |
| Atlus ZN-1 | Heaven's Gate (`hvnsgate`) | Source-audited; native interpreter/GL Jin combat, 198/198 inputs, MC68000/YMZ280B IRQs, mixed SPU/YMZ audio and zero faults; `atlus-zn-mac.md` |
| Time Warner/Atari ZN-1 | Primal Rage II (`primrag2`) | Source-audited ROM/EEPROM/CHD; native interpreter GL/NEON Arik player combat, 82/82 inputs, VIA HDD/DMA5 into 8 MiB RAM, watchdog, source-gain SPU/SDL sound and zero faults; `primal-zn-mac.md` |
| Acclaim ZN-1 ROM/sound | NBA Jam Extreme (`nbajamex`) | Native GL/NEON P1 Dallas–Seattle match, 114/114 inputs, live clock 2:43 / score 0–2; passive control stays in help at 3:00. Original ADSP2181/RAX sound, source ROM/SRAM banking, warm NVRAM, four-player controls and timed SPU0.35/RAX1 mix; zero faults/overruns. `nbajam-zn-mac.md` |
| Acclaim ZN-1 disk | Judge Dredd (`jdredd`) | Original ROM/CHD verified; native interpreter GL/NEON P1 shooting, 238/238 inputs, offscreen reload, score 100 / health 100%, exact SPU0.35 SDL capture, warm EEPROM player restart and zero device faults. Common source no-op read fix resolves loader failure. `jdredd-zn-mac.md` |
| Metro ZN storage variants | Bust a Move 2 (`bam2`, `bam2a`) | Original ROMs reach P1 HEAT score 662 / You Lose on HDD/GL, CD/GL and HDD/NEON, 743/743 inputs, identical RAM/SPU WAV and zero faults. Live SDL SPU0.35 playback and dedicated/generic input regressions pass. H8 firmware remains NO_DUMP, source MCU status is provisional and no disk data is accessed; ATA transport/music and complete board validation remain pending. `metro-zn-mac.md` |
| Eighting/Raizing ZN | Bloody Roar 2 (`bldyror2`), Brave Blade (`brvblade`) | Both World sets source-audited; native interpreter/GL player combat/shooter gameplay with source MC68000/YMF271, live SPU/YMF audio, portrait rotation and zero device faults; `raizing-zn-mac.md` |
| Konami System GV common map | Hyper Athlete (`hyperath`) | Authenticated BIOS/EEPROM/CD prepared. Interpreter GL/NEON P1 running replay, 110/110 inputs, identical RAM/raw SPU/speaker WAVs and zero faults. Source-swapped SPU0.75/CDDA1 mix now passes original-media and warm live SDL checks; live cue differences are retained separately. Authentic DRC remains pending; `konamigv-mac.md` |
| Konami System GV special maps | Simpsons Bowling; Tokimeki Seal Version; Beat the Champ; Dead Eye | Simpsons original installation matches MAME flash/EEPROM; GL/NEON player bowl with pins down, warm state and live SPU verified (`simpbowl-mac.md`). Tokimeki original `tmoshs` reaches player scenes and generates an operator TEST PRINT (`tokimeki-mac.md`). Beat the Champ TEST initialization matches MAME's 512 KiB flash; GL/NEON P1 football/ball flight, 597/597 inputs, identical RAM/SPU WAV and zero faults (`btchamp-mac.md`). Dead Eye original Sharp flash/guns now pass GL/NEON P1 shooting, offscreen reload and score 15, 1548/1548 inputs and identical RAM/SPU WAV (`kdeadeye-mac.md`); a GP0 cache-clear/retail-MoveImage confusion causing GL SIGBUS was fixed. Shared source SPU/CDDA mixing now also passes original Beat the Champ GL/NEON football replay with identical WAVs; original Plus/final reports and remaining game-specific audio checks stay open |
| Konami System GQ | Crypt Killer (`cryptklr`) | Original BIOS/sound/PCM ROMs, source EEPROM defaults and HDD CHD fully audited/prepared. Native interpreter GL/NEON P1 gameplay, 382/382 events, hearts/enemy/empty-ammo prompt, real HDD loads and zero faults. GL/NEON RAM and both raw/SDL 48 kHz dual-K054539 WAVs are byte-identical. Separate warm GL restart consumes 384/384 events, refills ammo and retains EEPROM. A new 7,200-frame original GL/NEON replay registers 2,000 points and keeps two hearts, versus zero/one in its no-later-shot control, with equal RAM/EEPROM/WAV and zero faults. A separate 480-event GL/NEON replay activates all three players and verifies independent game-latched gun positions with identical RAM/EEPROM/WAV. Calibration and isolated Coin 3 entry remain pending; `konamigq-mac.md` |
| Konami System 573 | Hyper Bishi Bashi Champ; DDR 2nd Mix analog; Percussion Freaks 4th Mix KAA digital | Hyperbbc original flash/timekeeper/BIOS/H8 response passes interpreter GL/NEON P1 minigame, 1902/1902 inputs, identical RAM/SPU WAV and zero faults. Live gain-1 SPU/SDL and complete checksummed flash/NVRAM cold/warm persistence pass; source-default cassette is explicitly labeled. DDR original cassette/CD are now audited, source CR-589 and analog stage bus integrated, and native GL/NEON cold installation completes with all eight flashes identical to MAME. DDR warm GL/NEON 1st Stage/PERFECT passes with 92/92 inputs, identical SPU WAV and original nonzero CDDA decoding. DDR source gain-1 speaker mix now passes headless GL/NEON (identical WAVs) and live SDL (zero errors; first coin cue differs by two samples). Digital original game/device chips and ZS01 readback through the ZI board bus now pass audit/protocol tests; digital I/O/FPGA encrypted RAM-to-MAS MPEG/component checks also pass. Native digital CPU/MMIO, drum controls and full NV3 cold/warm tests now pass GL/NEON, with strict original chip/CD identity rejection. CPU-clocked digital audio and source gain-1 SPU/CDDA/digital mixing now pass synthetic MIPS/MPEG checks, with identical GL/NEON/live SDL WAVs and all nine MPEG rates tested. The original KAA CD now passes full data audit; native GL/NEON cold installation matches all eight MAME flash chips, mutable ZS01 and the updated CR-589 buffer byte for byte. Original GL/NEON and live SDL reach scored 1st Stage (1,150; matched no-pad control zero), consume 1,495/1,495 events and play nonzero MPEG/SPU with zero faults. Headless WAVs match; live effect differences are measured separately. Song-counter alignment now removes a measured 5.749-second boot-phase difference, but MAME still scores 25,950 versus native 1,150 at tick 1,042; Retained GL/NEON MPEG observations are identical, and all 599 common MPEG frame numbers match MAME source byte cursors. The sampled MPEG onset/timer also differ; three/four-tick input probes still differ in score. Judgement fidelity and full-song results remain open; `system573-mac.md` |
| Konami Twinkle | beatmania IIDX 2nd style (`bmiidx2`) | Audited original media boots through the native PSX, original HDD/sound CPU, RTC/security and source UART/JVC devices. Native backup clear, warm title/tutorial and a real scored P1 note (1,204) pass interpreter GL/NEON with equal RAM/VRAM/NVRAM/speaker WAV. SDL playback of the same original run also matches all four artifacts exactly; live keyboard/turntable controls and pause Restart have separate original-run evidence and user confirmation. Full-song clear/result and watchdog reset sequencing remain open; `twinkle-mac.md` |
| Retail PlayStation | Ehrgeiz PAL (`SLES02111`) | Native real-BIOS/CD interpreter GL/NEON intro and player combat, 414/414 events, identical 2 MiB RAM/SPU WAV. Quest, memory-card/warm checks, live SDL sound and authentic DRC remain pending; `ehrgeiz-mac.md` |

The pinned `zn.cpp` inventory also contains the distinct Sony ZN daughterboards
and storage/sound variants listed above. Passing Capcom or Raizing alone does
not establish their compatibility. Regional clones do not substitute for distinct CPU/GPU,
sound or peripheral configurations. Retail PlayStation is inventoried separately
from the arcade boards.

## Current next checks

- Cabinet links: Kart Duel and Gallop Racer 2 Link pass local game checks;
  transport between two cabinets is neither implemented nor validated. The
  pinned source leaves those link paths unemulated/unmapped.
- Mutable flash: System 10 and G-Net have exercised programming/persistence;
  Simpsons installation matches MAME byte for byte. Beat the Champ Sharp
  initialization also matches MAME and reaches a P1 event. Dead Eye exact source
  Sharp flash seeds cold/warm state and reaches scored P1 shooting. System 573 DDR native GL/NEON installation now matches all eight MAME flash chips;
  DDR speaker output now passes headless/live checks. The original digital KAA
  installation also matches all eight MAME flash chips, ZS01 and CR-589 update
  buffer; bounded scored drums/live sound pass, with judgement alignment open.
- Printers: Technodrive responds at its JVS printer endpoint but has no
  validated complete ticket. Tokimeki Seal Version produces its original
  operator test page in virtual paper output; final game reports, original
  Plus sheets and physical printers remain open.

- Psychic Force: PLAYER1 selection and combat pass with the later coin/start
  replay. Independent MAME reaches both screens, and its independently configured
  EEPROM matches native byte for byte. The earlier coin sequence still enters a
  VSync wait from an IRQ callback and resets in both implementations; retain it
  as a timing/callback investigation. Full live SPU/YM mixing and DRC remain open.
- Hyper Athlete: the source DMA `dma_finished` write callback now delivers
  CHCR `01000201` data. The 28-byte MODE SELECT no longer stalls, and the P1
  running event passes bounded interpreter GL/NEON runs. A native MMIO regression
  requires MODE SELECT to change the target's block size. Source-clocked SPU0.75/CDDA1 mixing now passes original
  Hyper Athlete and Beat the Champ GL/NEON replays and Hyper Athlete live SDL;
  authentic ARM64 DRC still uses console serial MMIO handlers.
- Common Tecmo TPS now passes Gallop Racer 2 player racing and live native sound.
  FX1Z now passes RayStorm/G-Darius Ver.2 with its ROM-backed Zoom daughterboard
  and both VRAM configurations. Raizing now passes Bloody Roar 2 combat and
  Brave Blade 1P shooting with its MC68000/YMF271 and portrait presentation.
  Video System/Visco now passes Aero Fighters Special USA player shooting with
  its 24-bank ROM map and live SPU audio. Atlus now passes Heaven's Gate player
  combat with its 68000/YMZ280B, independent IRQ2/IRQ3 and mixed sound. Cool
  Boarders Arcade Jam now passes P1 downhill racing in GL/NEON with its real
  Z80, YMZ280B, dual FIFOs and live SPU/YMZ mix. The separate TPS
  Gallop Racer 2 Link configuration now passes local racing in GL/NEON with
  its original extra Z80 ROM. Its physical cabinet transport remains open:
  source itself exposes no link devices/ports. Primal Rage II now passes actual
  Arik combat in GL/NEON with the original HDD, VIA/DMA5, 8 MiB RAM, six buttons
  and live SPU audio. Its intentional watchdog reset also preserves upper RAM.
  NBA Jam Extreme and Judge Dredd now pass native player validation. Judge's
  original cold restore, live P1 shots/offscreen reload, GL/NEON score and warm
  EEPROM restart pass. Remaining Metro, Konami GV variants/GQ/573/Twinkle,
  CDDA/DRC and physical link checks retain their separate open rows.

Local evidence is in `artifacts/boards-mac-20261014`; scratch/reference runs are
under `/tmp/pcsx-boards-mac-20261014`. The user-linked Internet Archive collection
supplied Psychic Force, RayStorm, G-Darius Ver.2, Gallop Racer 2 and Hyper Athlete.
Additional public archives come from `mame-0.260-roms-split_202310`; their archive
size/SHA1 checks do not replace the pinned source ROM/CHD audit. MAME source is
`deb2f54fe7300e7a192690318beb860159bfd52a`, reference emulator 0.287.

No missing firmware, keys or CHDs have been substituted. Original BIOS/ROM/EEPROM
packages remain separate from mutable runtime NVRAM. ROMs, CHDs, binaries and
captures are not committed.
