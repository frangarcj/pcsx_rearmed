# Shared PSX arcade NOR flash engine

`libpcsxcore/arcade/nor_flash.c` provides a standalone command engine for the
flash profiles currently identified in MAME's PlayStation arcade drivers. It
does not make a board bootable by itself; each board still needs its own
mapping, loader, persistence policy, and CPU integration.

The Intel x16 profiles follow `src/devices/machine/intelfsh.cpp/.h` at pinned
MAME commit `deb2f54fe7300e7a192690318beb860159bfd52a` for these parts.

The constructor takes a caller-owned, writable memory image and requires its
length to exactly match the selected part. The engine borrows the image without
initializing or copying it, so existing ROM/NVRAM contents are retained.
Destroying the device does not free the image. API bus addresses are byte offsets
for x8 chips and 16-bit word offsets for x16 chips. Intel word reads and writes
use the byte ordering in MAME's 16-bit `intelfsh` wrapper.

Implemented profiles and MAME geometry are:

| Profile | Image bytes | Bus | ID bytes | Erase geometry |
| --- | ---: | ---: | --- | --- |
| Intel 28F640J5 | 8 MiB | x16 | `89/15` | 64 KiB blocks |
| Intel E28F400B | 512 KiB | x16 | `89/4471` | 16/8/8/96/128 KiB blocks |
| Intel TE28F160 | 2 MiB | x16 | `b0/00d0` | 64 KiB blocks |
| Fujitsu MBM29F016A | 2 MiB | x8 | `04/ad` | 64 KiB sectors |
| AMD 29F040 | 512 KiB | x8 | `01/a4` | 64 KiB sectors |
| SST 39SF040 | 512 KiB | x8 | `bf/b7` | 4 KiB sectors |

MAME's System 10 and System 12 board notes identify the 64-Mbit Intel device as
DA28F640J5. The device registry exposes `INTEL_28F640J5`; there is no
28F640K3 profile in the inspected upstream `intelfsh` implementation, so this
engine does not silently alias K3 to J5.

Intel profiles support array/ID/status modes, single-word program setup, block
erase confirmation, status clear, and master-lock status. Like MAME,
block-lock configuration reads as zero and are not implemented; master lock is
exposed in ID mode but does not impose extra write protection in MAME's model.
The E28F400B and TE28F160 profiles also expose MAME's generic AMD-compatible
unlock/autoselect/program/sector-erase/chip-erase command path. Its x16 bus
addresses are word offsets. That path reports a Sharp manufacturer ID (`b0`)
for TE28F160, as the pinned MAME constructor does, despite the Intel device
name. Their x16 programming assigns the supplied word to the image, matching
MAME's `FM_WRITEPART1` behavior. The older 28F640J5 profile retains its
previous bit-clearing behavior for API compatibility. AMD/Fujitsu profiles
support the AMD unlock, autoselect, byte-program, sector-erase, and chip-erase
commands. Fujitsu MBM29F016A byte programming assigns the supplied byte, including
zero-to-one transitions, matching the pinned `FM_BYTEPROGRAM` source model.
The other existing x8 profiles retain their bit-clearing behavior. SST uses
MAME's 39SF040 unlock aliasing and 4 KiB sector geometry.

E28F400B's `0x20`/`0xd0` erase follows MAME's asymmetric boot-block map: 16 KiB
at the bottom, two 8 KiB parameter blocks, one 96 KiB block, then 128 KiB
blocks. The first three blocks take 300 ms and the larger blocks take 600 ms.
Its AMD-compatible `0x80` erase sequence follows MAME's separate generic 64 KiB
sector path. TE28F160 and 28F640J5 use 64 KiB Intel erase blocks with a
one-second ready interval. `nor_flash_sector_size()` reports byte sizes, using
the address in that part's bus units.

Erase array contents change at the command's confirm write, as in MAME, while
ready/status behavior remains timed. `nor_flash_advance_us()` advances a virtual
clock: Intel 28F640J5 and TE28F160 block erase takes 1 second; Fujitsu and AMD
sector erase takes 1 second and chip erase takes 16 seconds; SST sector erase
takes 125 ms and chip erase takes 1 second. AMD-style status polling toggles
DQ6/DQ2 while erasing.
The MAME implementation does not schedule a separate busy interval for byte
program operations, so this port keeps those writes immediate instead of
inventing a program delay. The E28F400B and TE28F160 AMD-compatible chip erase
uses the source's 16-second interval; E28F400B Intel block erase uses the
part-specific 300/600 ms intervals documented above.

Commands outside these implemented profiles and sequences return an explicit
`NOR_FLASH_ERR_UNSUPPORTED` or `NOR_FLASH_ERR_SEQUENCE`; out-of-range bus
accesses return `NOR_FLASH_ERR_RANGE`. Intel write-buffer programming,
per-block locks, x16 AMD variants other than the two named G-Net parts,
boot-sector variants, SST page programming, and other Intel flash families are
not implemented. MAME's block-lock and write-buffer commands remain
unsupported on the G-Net x16 profiles.

Run the synthetic protocol tests with:

```sh
make -C tests -f Makefile.nor_flash clean run
```

The traces cover geometry validation, borrowed data, autoselect IDs, Intel
status/master-lock commands, G-Net x16 word programming and Intel/AMD-compatible
erase paths, AMD unlock/program/erase, bit-clearing behavior for x8 profiles,
busy status toggles and timing, invalid sequences, and bounds checks.
