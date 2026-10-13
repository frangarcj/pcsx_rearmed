# MAME PlayStation-derived system inventory

This inventory defines the MAME scope for full-board support. It contains **491
machine registrations in 10 driver files**, including BIOS roots and clone or
regional sets. The pinned source revision is MAME
[`deb2f54fe7300e7a192690318beb860159bfd52a`](https://github.com/mamedev/mame/tree/deb2f54fe7300e7a192690318beb860159bfd52a);
the installed listxml reports MAME 0.287.

| MAME driver | Board family | MAME sets | Example sets | Board configurations and notable peripherals | Current project support |
| --- | --- | ---: | --- | --- | --- |
| `sony/psx.cpp` | Retail PlayStation | 4 | `psu`, `psj` | SCPH NTSC/PAL; controller and memory-card ports, parallel I/O, CD-ROM | Console core and native PS-X EXE smoke pass; full retail machine/BIOS/media validation remains pending |
| `sony/zn.cpp` | Sony ZN-1/ZN-2; Capcom ZN; Tecmo TPS; Raizing ZN | 116 | `sfex`, `glpracr3`, `brvblade` | COH-1000/1001/1002 and COH-3002; variants include 1/2 MiB GPU memory, alternate sound CPUs, CAT702 security, and game I/O | Native synthetic Capcom COH1000C/1002C/3002C and Taito FX1S COH1000T MIPS/Z80, banks, SIO/MCU, EEPROM, inputs, GPU and sound pass; Street Fighter EX, Star Gladiator and Rival Schools pass short Mac interpreter/GL gameplay with QSound capture; authentic FX1 games and other daughterboards remain unverified |
| `sony/taitogn.cpp` | Taito G-Net (ZN-derived CPU/GPU) | 41 | `psyvaria`, `gdariusg`, `aerofgtsg` | COH-1002/3002 variants, Taito Zoom sound; CompactFlash and title-specific I/O on applicable sets | Native synthetic PSX interpreter/ARM64 DRC bus and storage execution fixtures pass. All 41 source profiles select BIOS/card/media identities; source ATA, PC Card and CHD composition fixtures pass, and the loader attaches verified CHD media. Four representative games have [Mac interpreter/GL gameplay](gnet-mac.md), captured Zoom PCM, persistent flash and source-profile analog I/O. Broader sets, event/vblank timing equivalence, frontend sound mixing and physical Vita remain pending |
| `konami/konamigv.cpp` | Konami System GV (Baby Phoenix) | 15 | `tmosh`, `weddingr` | CXD8530-class CPU; CXD8514/CXD8561 GPU variants, CD-ROM and game I/O | All 15 media profiles audited; seven common-map profiles have native integration and generated MIPS interpreter/DRC fixtures; authentic games and special I/O remain unverified |
| `konami/konamigq.cpp` | Konami System GQ | 1 | `cryptklr` | CXD8530BQ/CXD8538Q; MC68000 and TMS57002 sound system | Complete source profile and local ROM/HDD preparation tested; native board execution pending |
| `konami/ksys573.cpp` | Konami System 573 | 156 | `ddr4m`, `drmn`, `sys573` | CD-ROM, cassette, PC-card, flash and game I/O configurations | No arcade board support |
| `konami/twinkle.cpp` | Konami Twinkle | 19 | `bmiidx`, `bmiidx8` | CD/DVD media, MC68000/RF5C400 sound and network/session I/O | Standalone sound-board MC68000/RF5C400, sample RAM and mailbox/periodic IRQ fixtures pass; native PSX integration, ATA/media and authentic games remain pending |
| `namco/namcos10.cpp` | Namco System 10 | 45 | `taiko2`, `mrdrilrg`, `ballpom` | Main PCB and MEM PCB generations; optional EXIO, CD-ROM on some sets | Native synthetic MEM(M)/MEM(N) MIPS execution, bank fetch, NAND DMA, GPU and I/O pass; Mr. Driller 2/G and Gamshara have short Mac interpreter/GL gameplay and SPU capture; broader sets and add-on boards pending |
| `namco/namcos11.cpp` | Namco System 11 | 36 | `tekken`, `souledge`, `danceyes` | COH-100/110 CPU boards; motherboard and ROM daughterboard revisions; C76, C352, keycus | Tekken, Tekken 2 and Soul Edge pass short Mac interpreter/NEON/GL gameplay; broader sets and physical Vita pending |
| `namco/namcos12.cpp` | Namco System 12 / System 11 Plus | 58 | `tekken3`, `soulclbr`, `truckk` | COH-700/716; System 12 motherboard and ROM daughterboard revisions; JVS, H8 I/O, link/gun, CDXA add-on boards | COH-700 Soul Calibur Asia C has 12,000-frame DRC/NEON replays on Mac and ARMv7 Vita3K with observed Stage 2 match progress; nine special cabinets also have short Mac DRC/GL gameplay checks; timing equivalence and physical Vita remain pending |

The JSON inventory lists every set, including machine description, parent/ROM
parent, BIOS/runnable flags, CPU and audio chips with clocks, GPU and other
device references, slots, machine configuration switches, and media devices.
System-level notes give the board variants, known RAM/VRAM configurations,
peripherals, and the current support boundary. Values that cannot be assigned
to one set from MAME data are labeled at family level; per-set GPU and CPU
identities remain in the JSON records.

## Estimated work remaining (2026-10-05)

These ranges are engineering estimates of remaining work across the complete
family, including its declared peripherals and validation. They are not measured
compatibility percentages, do not count a parsed registration as an implemented
machine, and are not estimates of elapsed time. Shared PSX code is already
available, so even families without a native board port do not start from zero.
The original estimates began with only Soul Calibur Asia C validated. The
evidence rows include later Mac checks; the remaining-work ranges have not
all been recalibrated. No entire family is considered finished.

| Family | Estimated remaining | Evidence and principal gaps |
| --- | ---: | --- |
| Retail PlayStation | 20–35% | Existing console core and native CPU/GPU smoke; native BIOS/disc, controller, sound and retail-game validation pending |
| Sony ZN family (including Taito FX1) | 50–65% | Capcom and FX1S generated native guests pass; three Capcom games pass short Mac GL gameplay; authentic FX1 games and Tecmo/Raizing/other daughterboards pending |
| Taito G-Net | 80–90% | Native PSX interpreter/DRC bus/storage fixtures and Zoom composition pass; all 41 source profiles and CHD/ATA/PC Card composition are implemented and component-tested. Four representative games pass bounded Mac interpreter/GL gameplay with Zoom PCM, persistent flash and source-profile analog I/O. Broader sets, event timing equivalence, frontend sound mixing/bindings and physical Vita remain pending. This historical estimate has not been recalibrated from the Mac gameplay evidence. |
| Konami System GV | 45–60% | Common native board MIPS interpreter/DRC, DMA5, IRQ10 and CHD fixture tested; authentic BIOS/CD execution, sound and special I/O pending |
| Konami System GQ | 50–65% | HDD preparation, real CHD read/write NCR fixture, MC68000, TMS57002 and K056800/K054539 tested; complete board/gun wiring and real-game validation pending |
| Konami System 573 | 85–95% | Security cassette, flash and shared CD/SCSI foundations; board configurations, audio, I/O and game validation pending |
| Konami Twinkle | 90–95% | Sound-board MC68000/RF5C400, banked wave RAM and IRQ/mailbox fixtures pass; native PSX board, ATA/media/session I/O and game validation pending |
| Namco System 10 | 30–45% | Native MEM(M)/MEM(N) guests exercise crypto, bank fetch, NAND DMA, inputs and GPU; three base-board games pass short Mac gameplay; broader sets, add-ons and DRC pending |
| Namco System 11 | 25–40% | Native MIPS/C76 guests exercise banks, keycus, controls, EEPROM and sound; three authentic games pass Mac gameplay, including GPU type-1 GL; broader variants and physical Vita pending |
| Namco System 12 | 30–45% | Soul Calibur Asia C reaches Stage 2 on Mac/Vita3K; nine special gun/JVS/link/CDXA cabinets have short Mac DRC/GL checks; timing comparison, physical Vita and broader gameplay validation pending |

For the original **Soul Calibur Asia C + headless M1 harness** target alone,
the estimated remaining work is **15–25%**: sustained gameplay and audio/timing
comparison, broader control coverage and robustness. That estimate must not be
applied to all 58 System 12 registrations. See [recorded runtime evidence](system12.md).
The [ARMv7 Vita3K replay](system12-vita-gameplay.md) also completed 12,000 frames,
consumed all 177 inputs and retained 23 native captures with zero ARM, H8 or
unmapped-access diagnostics. It selected a different fighter from the Mac
replay, so it does not establish timing or state equivalence.
The source counts of 491 registrations include BIOS roots and regional clones;
one successful game does not establish compatibility with all those entries.

## Completeness method

The generator reads the complete output of MAME `-listxml` and selects machine
entries whose **main CPU** name matches Sony's CXD85xx/CXD86xx PlayStation SoC
family. For the pinned checkout, this produces the ten driver paths in the
matrix. An independent source audit of the pinned MAME tree found those same ten
driver files referencing the PSX CPU/GPU device family (`CXD85xx/CXD86xx`,
`psxirq`, `psxdma`, or `psxspu`). That cross-check found no additional
drivers. This boundary includes console sets plus arcade derivatives, shooters,
music games, and Raizing titles, while excluding unrelated generic MIPS
R3000/R3041 systems and PlayStation 2 Emotion Engine boards.

Generate from the pinned local MAME checkout and installed MAME:

```sh
python3 scripts/mame_psx_inventory.py \
  --mame-source ../mamedev \
  --mame-bin mame \
  --output scripts/mame_psx_systems.json
```

The source argument is checked against the pinned commit. A saved `-listxml`
file can be passed using `--listxml` for repeatable offline generation.
Inventory/config registration only describes MAME's declared machine setup; it
does not imply that a board boots or runs correctly in this project. RAM and
VRAM notes are based on the corresponding MAME machine configuration and
driver board notes. The machine-level device references and clocks are
extracted directly from listxml. `mame_driver` and `mame_features` retain the
upstream baseline separately from project status: the pinned catalog marks
358 sets imperfect and 133 preliminary. The target is the declared MAME
behavior, including those known upstream limitations.

## Shared components already tested

The generic [ROM assembler](rom-regions.md) parses all 491 pinned ROM blocks;
Soul Calibur's four assembled regions match the original preparer byte for
byte. CHDs and undumped regions remain explicit requirements. Parsing a
machine's assets does not imply that it boots.

- Namco System 12: H8/3002, C352, ADC/vblank, RTC/settings, JAMMA, ROM DMA and
  2 MiB type-2 GPU bring-up; [runtime evidence](system12.md). Native COH-700/716
  reset guests verify RAM/bank/input retention and absolute PCM phase with
  interpreter/DRC. Actual MAME C352 hooks confirm timed key-on/off and continuous
  reset sample phase. Native ADC/timer/SCI firmware traces now match 1440
  MAME value/address/cycle comparisons across three H8 slice sizes; SCI1
  RTC/settings wiring uses individual source clock edges; cold CPU-MMIO RX-start
  attotime phase is included. Production SCI1/RTC transfers add 64 exact
  address/value/cycle comparisons. The PSX/H8 mailbox fixture matches H8 bus
  cycles but exposes a main-CPU visibility mismatch against MAME's scheduler.
  SCI global timer armed/expiry/internal-deadline state adds 384 exact isolated
  observations, including a pending callback after stopping the clock. Production
  global timer arbitration, standby/reset contexts and full RTC/JVS waveforms
  remain open. A PSX instruction probe exposes excess native MULT/DIV timing;
  matching RTPS/branch intervals in that fixture do not establish complete CPU
  timing. Native H8
  port-B vblank polling now matches actual MAME
  reads with zero cycle difference on interpreter/ARI64, both short/long blocks
  and block/vblank-only frontend cadence. Seven screen mode/reset captures also
  match the standalone source screen component in exact attoseconds; native
  GP1/reset execution fixtures pass. See [fidelity evidence](system12-performance.md). These
  component gates do not establish complete family compatibility.
- GPU: type-1 command fields, interleaved 4/8-bit textures and linear 16-bit
  textures pass fixed MAME-derived vectors with both 1 MiB and 2 MiB VRAM;
  type-2 upper VRAM also passes a live MAME oracle.
- ZN: [common board I/O](zn-board.md), timed AT28C16 and CAT702 security
  transform/pin traces. The connected [ZN MCU](zn-mcu.md) follows the source
  serial/mux/timer model. [Capcom daughterboard](capcom-zn.md) tests execute its
  Z80, banks, latch/NMI and 250 Hz IRQ. The connected QSound HLE produces nonzero
  PCM from executed Z80 commands with identical output for divided budgets;
  the [native Capcom adapter](capcom-zn-native.md) executes generated MIPS/Z80
  guests for all three board geometries, with fixed/banked ROM execution,
  SIO/MCU/IRQ, inputs, EEPROM persistence and nonzero QSound PCM. Instruction
  cache and 1/2 MiB GPU tests pass. Street Fighter EX, Star Gladiator and
  Rival Schools now have [short Mac interpreter/GL gameplay checks](capcom-zn-mac.md)
  with QSound capture; ZN-2 sound/SIO clock conversion is corrected.
  The [PSX SIO0/SIO1](psx-sio-port.md) port tests baud deadlines, pin ordering,
  receive overruns and IRQs, including callback timestamps during catch-up.
  The [47-set Capcom registry and preparer](capcom-zn-boards.md) verifies media
  for 43 game candidates, including BIOS, CAT702 keys and QSound coefficients.
  The [DSP16/DSP16A interpreter](dsp16-port.md) passes independent arithmetic
  vectors and synthetic peripheral firmware. MAME defaults to QSound HLE;
  the [HLE device](qsound-hle-port.md) passes PCM/ADPCM, FIR, echo and timing
  vectors. Its optional [LLE device](qsound-port.md) executes generated DSP firmware
  for command consumption, sample reads and stereo serial output. Authentic
  firmware verification remains pending. Other daughterboards remain pending.
- Taito: [TC0140SYT/PC060HA sound command link](taito-sound-comm.md) tests
  cover nibble packets, NMI/reset and scheduler-barrier semantics. The
  [YM2610 and YM2610B](ym2610-port.md) wrappers retain the upstream YMFM engine,
  with FM/SSG, ADPCM and exact timer tests. The [MB3773 watchdog](mb3773-port.md)
  retains MAME's five-second deadline and falling-edge rearm. The connected
  [FX-1S daughterboard](taito-fx1-port.md) tests execute Z80 code for banking,
  sound commands, timer IRQ, audio and reset hold/release. The
  [native adapter](taito-fx1-native.md) passes generated MIPS/Z80 guests for
  bank fetches with and without instruction cache, GPU, RAM, SIO/MCU/IRQ,
  sound command exchange, inputs, EEPROM persistence and nonzero YM2610B PCM.
  Authentic Taito game boot remains unverified. The [22-set Taito FX-1 registry](taito-fx1-boards.md)
  packages source-verified media for nine FX1S games and rejects the other variants.
  The [ZSG-2 component](zsg2-port.md) passes sample decoding, 48-channel
  synthesis, four output routes, register, reset and timing tests, including
  sanitizers. Its literal stream-batch API retains MAME ramp parity; the
  standalone clock scheduler is explicitly a host model. The [TMS57002 effects DSP](tms57002-port.md) passes standalone host-load,
  arithmetic, external-memory and four-channel stream tests. The [MN10200 sound
  CPU](mn10200-port.md) and [standalone Zoom board](zoom-board-port.md) pass
  generated firmware execution, word-register access, mailbox lanes, reset
  synchronization and nonzero DSP audio fixtures, including sanitizers.
  The [native G-Net adapter](gnet-native.md) now connects the common Sony ZN
  map, flash/Zoom buses, selected CHD-backed ATA card and clock synchronization.
  All 41 source profiles select their BIOS/media identities.
  Generated MIPS interpreter/ARM64 DRC fixtures verify banked reads
  and the public nonzero PCM callback, including reset/load lifetime and
  sink disable. A separate executed synthetic MIPS storage fixture verifies
  Type-1 card unlock, IDENTIFY, sector reads/writes, overlay reopen and reset
  relock. Source CPU timing is integrated. [Four representative games](gnet-mac.md)
  now reach player gameplay with the Mac interpreter/GL path and captured Zoom
  PCM; mutable flash persists and source-profile analog controls are wired.
  Production PCM resampling/mixing, event/vblank timing equivalence, physical
  frontend analog bindings and broader authentic-game coverage remain pending.
  The [NOR engine](nor-flash.md) now has source-derived E28F400B/TE28F160
  profiles, with x16 programming, IDs, erase geometry and timing fixtures plus
  sanitizers. The [standalone G-Net flash/Zoom composition](gnet-zoom-flash-port.md)
  connects these devices with little-endian raw images, all four source banks,
  JP1 and sound reset wiring. Generated firmware tests execute the MN10200
  read-only program map while main-bus tests program Intel flash; partial lane
  writes and the card interface remain unsupported.
- Namco System 10: [35 crypto profiles and optional writable NAND](namcos10-crypto.md),
  with 441 fixed encrypted streams; [MEM(M)/MEM(N) board adapter](namcos10-board.md)
  tests cover boot copies, banks, flash, bus masks and DMA. The connected
  [base I/O module](namcos10-io.md) preserves MAME input/I2C/IRQ behavior.
  The [45-set media registry and preparer](system10-boards.md) verifies
  local media for supported base-map candidates and rejects extra requirements.
  Its [native adapter](system10-native.md) executes
  [generated MIPS guests](system10-native-test.md) for MEM(M) and MEM(N), with
  banked code, NAND DMA to upper RAM, GPU readback, IRQ acknowledgment and inputs.
  Mr. Driller 2, Mr. Driller G and Gamshara now have
  [short Mac interpreter/GL gameplay and SPU capture](system10-mac.md).
- Konami security cassettes: [X76F041, X76F100 and DS2401](konami-security.md)
  password/pin/timing traces; no System 573 boot claim.
- Konami Twinkle: the [sound-board component](twinkle-board.md) runs generated
  MC68000 firmware through its source bus map, RF5C400, banked 32 MiB sample
  RAM, CY7C131 mailbox and 60 Hz IRQ source. Normal and sanitizer fixtures
  verify IRQ handlers, reset/timer phase, byte lanes and nonzero PCM callbacks.
  Native PSX integration, ATA sample loading and authentic games remain pending.
- Konami System GV: [15-set source registry and local media preparer](konamigv-boards.md)
  preserves game EEPROM/CD pairing and audits ROM hashes, CHD SHA1, CD metadata
  and hunk integrity. The [native common-board adapter](konamigv-native.md)
  passes generated MIPS interpreter/DRC input, DMA5 and NCR IRQ10 fixtures
  against a real synthetic CHD. Authentic BIOS/CD execution and game-specific
  peripherals remain pending.
  The standalone [NCR53CF96 controller](ncr53cf96-port.md) passes register,
  selection, phase, DMA and timer tests, including sanitizers. A mounted-media
  backend is still required for disc execution. The connected
  [NSCSI bus/target protocol](nscsi-port.md) passes a complete NCR selection,
  CDB, DMA data-in, status, command-complete and disconnect transaction against
  an explicit fixture backend, including sanitizers. It supplies no built-in
  media responses.
  The [XM-5401 CD target](nscsi-cd-port.md) passes source command, TOC, block-size,
  media-change and CDDA-control tests plus complete NCR READ DMA transactions;
  its [CHD CD/audio backing](nscsi-chd-cd.md) passes actual compressed MODE1/raw
  sectors, pregap/postgap/TOC, real CDDA PCM, a full NCR transaction including
  terminal disconnect IRQ, and malformed-metadata/callback guards with
  sanitizers. Authentic game media execution remains pending.
  The [93C46 EEPROM component](eeprom93c46-port.md) passes serial commands,
  write protection, busy deadlines, storage byte order and selective reset
  tests, including sanitizers; native common-board persistence is connected.
  The [i8251/MB89371 serial components](standalone_i8251_mb89371.md) pass
  pin waveforms, async/sync parity, source reset retention, dual-channel clock
  ordering, loopback and timing tests, including sanitizers.
- Konami System GQ: the [complete source profile](konamigq-boards.md) records
  all ROM loads, writable HDD and source-default EEPROM without a boot claim.
  Its [local media preparer](konamigq-media.md) passes temporary HDD CHD
  verification, malformed media rejection and package/NVRAM preservation tests.
  The [native CHD HDD backing](chd-hd-backing.md) passes actual compressed-sector
  reads, validated overlay persistence and sanitizers with generated media.
  The [hard-disk SCSI target](nscsi-hd-port.md) and
  [complete CHD-backed NCR DMA fixture](chd-scsi-hd-test.md) pass read/write,
  source preflight/cache quirks, status, message, disconnect and persistence
  tests, including sanitizers.
  The [K056800 sound interface](k056800-port.md) passes
  cross-CPU register, mirrored offset, IRQ, acknowledgment and reset tests,
  including sanitizers. The standalone [K054539 PCM component](k054539-port.md)
  passes eight-voice PCM/DPCM, loop/reverse, pan/gain, reverb, register-port and
  timer tests, including sanitizers. The standalone
  [MC68000 core](m68000-port.md) generates the pinned direct full/partial
  instruction handlers and passes reset, bus lanes, arithmetic/flags, MOVEM,
  exceptions, STOP/IRQ/NMI/RTE, split budgets and callback/overflow tests with
  sanitizers. The [TMS57002 DSP](tms57002-port.md) also passes independent
  program/coefficient RAM, host upload/download, arithmetic, external-memory,
  callback-cycle, reset and audio tests with sanitizers. Lightgun I/O and the
  complete native board remain pending.
- Sound CPU: [Z80](z80-port.md) retains the MAME instruction and resumable
  bus-cycle engine. Independent arithmetic/flags and interrupt/timing tests
  pass; board sound wiring remains necessary.
- Shared NOR: [four flash chip profiles](nor-flash.md) with command/status and
  erase-timing tests; board wiring remains necessary.
- Namco System 11: [M37702/C76](m37710-port.md) and [board bus](namcos11-board.md)
  tests connect all nine KEYCUS handlers, C352, shared RAM, ROM banks and IRQs;
  the native harness executes generated MIPS code from all eight banks and
  C76 firmware for inputs/ADC, shared RAM and nonzero audio. Exact EEPROM
  persistence also passes. Tekken, Tekken 2 and Soul Edge have
  [short Mac gameplay validation](system11-mac.md), including type-1 GL.

The complete family port remains active. The matrix distinguishes runnable
board evidence from standalone device and media preparation work.
