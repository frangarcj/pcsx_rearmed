# Native G-Net board adapter

The native adapter composes the pinned MAME G-Net map with the existing Sony
ZN common board implementation. Source is MAME commit
`deb2f54fe7300e7a192690318beb860159bfd52a`, primarily
`src/mame/sony/taitogn.cpp` and `src/mame/sony/zn.cpp`.

The strict `board.ini` loader selects any of the 41 source registry entries:
the two BIOS roots or one of the 39 game profiles in
[`gnet-media-profiles.md`](gnet-media-profiles.md). A game profile selects its
MAME BIOS root/default option, PC Card type, CHD filename and exact CHD SHA-1.
Each package supplies these assembled BIOS regions:

| Region file | MAME region | Size | Source check |
|---|---|---:|---|
| `maincpu.bin` | `maincpu:rom` | 512 KiB | Exact root-set SHA1 |
| `cat702_0.bin` | `cat702_1` | 8 bytes | Exact root-set SHA1 |
| `cat702_1.bin` | `cat702_2` | 8 bytes | Exact root-set SHA1 |
| `firm.bin` | `firm` | 2 MiB | Exact root-set SHA1 |
| `eprom.bin` | `eprom` | 1 MiB | Selected COH-3002T BIOS option and exact SHA1; COH-1002T source region is synthesized as erased `0xff` |

For a game profile, the source CHD is named `<DISK_IMAGE>.chd` in the package
directory and opened with the registry's exact MAME SHA-1. The CHD stays
read-only; writes use an overlay named `<mame_set>.overlay`, flushed on
unload. Optional `chd_file` and `overlay_file` keys make those names explicit;
`chd_file` must match the source filename.

The sound program and three sample-flash devices are absent from the BIOS-root
ROM declarations. MAME creates those flash devices in erased state, so the
adapter initializes them to `0xff`; it does not substitute media. The source's
`upd78081` region is a NO_DUMP device that this G-Net machine configuration
does not instantiate. This does not imply that all games or PCMCIA media work.

BIOS-root packages keep the original `board.ini` keys and use the source
default EPROM option. A game package selects a MAME set and can explicitly
choose an available COH-3002T BIOS option:

```ini
format=pcsx-aow-gnet-v1
mame_set=chaoshea
mame_source_commit=deb2f54fe7300e7a192690318beb860159bfd52a
board_model=ZN2
cpu_device=CXD8661R
cpu_clock_hz=100000000
gpu_device=CXD8654Q
gpu_clock_hz=53693175
main_ram_size=4194304
vram_size=2097152
game_execution_verified=false
bios_option=v1
chd_file=chaosheat.chd
overlay_file=chaoshea.overlay
```

The selectable COH-3002T options are `v1`, `v2`, `mb2009`, and `mb2011`;
COH-1002T is fixed and uses its erased EPROM region. BIOS and CHD hashes are
checked against the generated source registry. The loader records each source
game's machine, input, initializer, and flags. Source-profile JP1 defaults,
Go By RC analog controls and Mawasunda handles/outputs are applied; other
per-game extensions remain unverified. See [representative Mac gameplay and
startup fixes](gnet-mac.md) for the four authenticated media runs.

The adapter configures the source 4 MiB PSX RAM and 2 MiB VRAM, Sony ZN CAT702,
EEPROM and SIO devices, then routes CPU accesses through `gnet_zoom_bus_*` and
the shared ZN map. Game profiles attach CHD-backed ATA/PC Card storage to the
RF5C296 callbacks; BIOS-root profiles have no card. Core cycles advance Zoom
and storage timers on monotonic board time. The Mac validation uses four
hash-checked BIOS/CHD packages; mutable flash persists separately in
`flash.nvram`, as described in [the Mac validation notes](gnet-mac.md).

The 6.25 MHz adapter clock is the MN10200's effective execution-cycle rate:
the pinned MAME device is configured at 12.5 MHz and converts external clocks
to CPU cycles through its internal divide-by-two.

Build the standalone Vita core archive with
`make -f Makefile.libretro platform=vita ARCADE_SYSTEM12=1 ARCADE_SYSTEM_GNET=1 USE_LIBRETRO_VFS=0 USE_ASYNC_CDROM=0 USE_ASYNC_GPU=0 USE_ASYNC_SPU=0`.
With that opt-in build, the libretro frontend accepts a prepared profile
directory as game content when it contains `board.ini`. It configures the
board's memory and VRAM sizes before initializing the core, keeps the selected
CPU backend, routes the adapter's listed digital controls, resets and unloads
the board through its adapter lifecycle, and disables savestates while the
board is active because the current generic state format omits G-Net devices.
Normal console game loading remains available in the same build.

The public G-Net adapter API can now deliver Zoom PCM frames to an optional
callback. The sink is disabled by default and persists across board resets;
the libretro frontend still routes generic PSX SPU audio only and does not yet
mix or resample Zoom PCM. The EEPROM API is not connected to frontend save
storage. The native profile loader and board expose the source PC Card/ATA
path. Frontend save-state support, Zoom mixing, analog input bindings and
broader authentic-media coverage remain open. The Mac harness captures Zoom
PCM to WAV and has bounded interpreter/GL gameplay evidence for four profiles.

The audio callback receives normalized stereo floats and a ZSG timestamp in
the 25 MHz chip-clock domain. It runs synchronously while `systemgnet_sync`
advances the board, including when CPU MMIO routing triggers synchronization,
so clients should keep it bounded and avoid reentering or destroying the
board. Timestamps increase within a sound-device reset epoch and may restart
when those devices reset. The client owns the opaque pointer and must clear the
callback before freeing it; passing a null callback disables delivery.

The standalone Zoom regression also loads a synthetic MN10200 program that
uploads a small TMS57002 tone routine through the board pins and captures the
real `gnet_zoom` audio callback. It checks finite nonzero PCM, increasing ZSG
timestamps, and identical output for one-shot versus split cycle synchronization.
This validates the emulator's PCM path with test data. Separately, authentic
media runs install and execute the game sound program and samples and produce
nonzero PCM; see [the Mac validation notes](gnet-mac.md).

The selected BIOS root reports its MAME main-CPU input clock through
`systemgnet_main_clock_hz()` and `systemgnet_report()`. The shared arcade source
timing option uses the corresponding half-rate PSX cycle clock for COH1002T
(67.7376 MHz input, 33.8688 MHz PSX clock) or COH3002T (100 MHz input, 50 MHz
PSX clock). Device clocks remain independently configured; see
[`arcade-source-timing.md`](arcade-source-timing.md) for the timing model and
its limits.

`--system-gnet-fixture` is test-only and uses generated in-memory data. Its
executed MIPS program selects flash bank 1, reads the banked flash and constant
ID register through the PSX memory bus, and records both values in PSX RAM. It
also installs an audio sink before board load and reset, then advances the
synthetic MN10200/TMS57002 tone program and verifies finite nonzero PCM with
monotonic timestamps through the public adapter callback. The complete fixture
passes with both the interpreter and ARM64 DRC, including the PCM assertions
and sink-disable check. This checks the board map bridge, audio callback
lifecycle, and 4 MiB RAM path, not MAME assets, startup firmware, game boot, or
real-board compatibility. Registry and synthetic storage tests do not
establish an authentic G-Net boot; the separate Mac media runs provide that
evidence for four profiles.

`--system-gnet-storage-fixture <chd>` is a separate test-only path. It uses a
generated synthetic CHD and bypasses neither `systemgnet_load()` nor its
profile/hash checks. Executed PSX MIPS unlocks a synthetic Type-1 card through
RF5C296 memory space, reads ATA IDENTIFY and a sector through PIO, writes the
same sector through the PSX bus, and verifies the overlay after reopening.
Reset relocks the card. The fixture passes with the interpreter and ARM64 DRC;
it verifies the synthetic path and does not claim game startup or authentic
boot. Component tests cover both ATA CompactFlash and IDE HDD callback bridges
through the C API and RF5C296, including ASan/UBSan.
