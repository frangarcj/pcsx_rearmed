# Third-Party Notices

## libchdr in the native Mac harness

The harness uses the existing pinned libchdr submodule at revision
`9ccd3a78e239246ee6e1166e1be82ab0ff0dcddb`. Copyright Romain Tisserand,
Aaron Giles and the copyright holders named in its component sources. Full
BSD license text is retained in `deps/libchdr/LICENSE.txt` and in the CHD,
codec and helper sources. The build-local bitstream copy retains Aaron Giles'
BSD-3-Clause notice and applies unsigned-shift fixes; it does not alter the
submodule. Bundled LZMA is public domain (see `deps/libchdr/deps/lzma-26.02/LICENSE`),
Zstandard's embedded BSD terms are in `deps/libchdr/deps/zstd-1.5.7/zstddeclib.c`,
and dr_flac's terms are in `deps/libchdr/include/dr_libs/dr_flac.h`.

## PsyCross / Psy-X

Source: <https://github.com/OpenDriver2/PsyCross>

The PS Vita native GPU and renderer backend contains modified derivatives of
PsyCross GPU and renderer code.

License: MIT

Copyright (c) 2020 REDRIVER2 Project

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## MAME CD layout and CDDA

The read-only CHD CD backing in `libpcsxcore/arcade/chd_cd.*` derives its track
layout and CDDA behavior from MAME `cdrom.cpp` (Aaron Giles, R. Belmont) and
`cdda.cpp` (Aaron Giles, smf), at the pinned revision. Its complete BSD-3-Clause
license is retained in `libpcsxcore/arcade/chd_cd_LICENSE.txt`. Compressed media
is decoded by the pinned libchdr described above.

## MAME TMS57002 DASP

The standalone TMS57002 decoder, execution core, instruction list and generator
in `libpcsxcore/arcade/tms57002*` derive from Olivier Galibert's MAME sources at
commit `deb2f54fe7300e7a192690318beb860159bfd52a`. The upstream BSD-3-Clause
notices are retained in source headers and the complete license is in
`libpcsxcore/arcade/tms57002/LICENSE.txt`. The local wrapper replaces device,
clock, memory and stream plumbing while preserving the instruction algorithms.

## MAME MC68000 and NSCSI media devices

Standalone MC68000 handlers, decoder and shim in `libpcsxcore/arcade/m68000*`
derive from Olivier Galibert's BSD-3-Clause MAME core at the pinned revision
`deb2f54fe7300e7a192690318beb860159bfd52a`. The full notice is retained in
`libpcsxcore/arcade/m68000/LICENSE.txt`; `.lst` and generator source are retained
and execution handlers are generated under the build directory.

The NSCSI HDD and CD command/media components derive from Olivier Galibert's
BSD-3-Clause `src/devices/bus/nscsi/hd.cpp`, `cd.cpp` and the common HLE target
at the same MAME revision. Full notices are retained in
`libpcsxcore/arcade/nscsi_hd_LICENSE.txt` and `nscsi_cd_LICENSE.txt`.

## MAME Namco System 12

Source: <https://github.com/mamedev/mame/blob/deb2f54fe7300e7a192690318beb860159bfd52a/src/mame/namco/namcos12.cpp>

The experimental System 12 board adapter derives its memory map, banking,
and ROM DMA behavior from MAME's `namcos12.cpp`. The preparation utility
also uses that driver's ROM declarations. Upstream file notice:

```text
license:BSD-3-Clause
copyright-holders:smf
```

Copyright: smf (upstream does not specify a year).
License: BSD 3-Clause.


Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## MAME H8/300H and Namco C352

The standalone H8/3002 interpreter and instruction generator in
`libpcsxcore/arcade/h8*` derive from MAME's
`src/devices/cpu/h8/h8.cpp`, `h8.lst`, `h8make.py`, and H8/3002
peripheral sources. Copyright holders: Olivier Galibert.

The sample engine in `libpcsxcore/arcade/c352.{c,h}` derives from
`src/devices/sound/c352.cpp`. Copyright holders: R. Belmont, superctr.

Both are pinned to MAME commit `deb2f54fe7300e7a192690318beb860159bfd52a`
and licensed BSD-3-Clause. The BSD conditions and disclaimer reproduced above
apply to these copyright holders too; original notices are retained in each
source and generated instruction file.

## MAME serial settings and RTC

`libpcsxcore/arcade/serial_nvram.c` adapts the protocols from
`src/mame/namco/namco_settings.cpp` (copyright holder Olivier Galibert) and
`src/devices/machine/rtc4543.cpp` (copyright holder R. Belmont), at the same
pinned MAME revision. Both upstream sources are BSD-3-Clause; the full BSD
conditions and disclaimer above apply to these copyright holders.

## MAME CAT702 security device

`libpcsxcore/arcade/cat702.{c,h}` adapts the CAT702 transform and pin protocol
from `src/devices/machine/cat702.cpp`, copyright holder smf, at MAME commit
`deb2f54fe7300e7a192690318beb860159bfd52a`. The upstream source is
BSD-3-Clause; the full BSD conditions and disclaimer above apply to this
copyright holder.

## MAME Namco System 10 decryption and NAND

`libpcsxcore/arcade/namcos10_crypto*` adapts `src/mame/namco/ns10crypt.cpp`
(copyright holder Andreas Naive), the profiles and banking from
`src/mame/namco/namcos10.cpp` (copyright holders smf, windyfairy), and read
behavior from `src/devices/machine/nandflash.cpp` (copyright holder Raphael
Nabet), at the pinned revision above. The full BSD-3-Clause conditions and
disclaimer apply to these copyright holders; a full copy is also retained in
`libpcsxcore/arcade/namcos10_crypto_LICENSE.txt`.

## MAME Konami cassette security

`libpcsxcore/arcade/konami_security*` adapts the X76F041, X76F100 and DS2401
devices from `src/devices/machine`, copyright holder smf, at the pinned
revision above. The full BSD-3-Clause conditions and disclaimer apply to
this copyright holder and are retained in `konami_security_LICENSE.txt`.

## MAME NOR flash

`libpcsxcore/arcade/nor_flash.{c,h}` adapts the command protocols from
`src/devices/machine/intelfsh.cpp`, copyright holder Aaron Giles, at the
pinned revision above. The full BSD-3-Clause conditions and disclaimer
apply to this copyright holder and are retained in the source.

## MAME PlayStation GPU type 1

The CXD8538Q command fields, status behavior and texture interleave equations
in the GPU_NEON and GPULIB adapters derive from
`src/devices/video/psx.cpp`, copyright 2003-2014 smf, at the pinned revision
above. The full BSD-3-Clause conditions and disclaimer apply to this
copyright holder.

## MAME Namco System 11

`libpcsxcore/arcade/namcos11_board*` and `namcos11_keycus*` adapt the bus maps
and protection handlers from `src/mame/namco/namcos11.cpp` and `ns11prot.cpp`,
copyright holder smf, at the pinned revision above. The full BSD-3-Clause
conditions and disclaimer apply to this copyright holder.

`libpcsxcore/arcade/m37710*` adapts the M37702/M37710 processor from
`src/devices/cpu/m37710`, copyright holders R. Belmont and Karl Stenerud.
The full BSD-3-Clause license is retained in `m37710_LICENSE.txt`.

## MAME Sony ZN common board

`libpcsxcore/arcade/zn_board.{c,h}` adapts the common ZN-1/ZN-2 I/O and security
wiring from `src/mame/sony/zn.cpp`, copyright holders smf and R. Belmont, at the pinned
revision above. The full BSD-3-Clause conditions and disclaimer apply to
these copyright holders.

## MAME AT28C16 parallel EEPROM

`libpcsxcore/arcade/at28c16.{c,h}` adapts `src/devices/machine/at28c16.cpp`,
copyright holder smf, at the pinned revision above. The full BSD-3-Clause
license is retained in `at28c16_LICENSE.txt`.

## MAME Namco System 10 board maps

`libpcsxcore/arcade/namcos10_board.{cpp,h}` and `namcos10_io.{c,h}` adapt the
MEM(M)/MEM(N) bus maps, base I/O/I2C/interrupt state and boot-copy logic in `src/mame/namco/namcos10.cpp`, copyright holders smf
and windyfairy, at the pinned revision above. The full BSD-3-Clause conditions
and disclaimer apply to these copyright holders.

## MAME Z80 processor

`libpcsxcore/arcade/z80*` adapts `src/devices/cpu/z80`, copyright holders
Juergen Buchmueller and Andrei I. Holub (the generator credit uses Andre
I. Holub). The original opcode list and generator are retained; the full
BSD-3-Clause license is in `z80_LICENSE.txt`.

## MAME Capcom ZN daughterboard

`libpcsxcore/arcade/capcom_zn.{c,h}` adapts the main/audio bus maps, banks,
sound latch and periodic interrupt wiring from `src/mame/sony/zn.cpp`,
copyright holders smf and R. Belmont, at the pinned revision above. The full
BSD-3-Clause conditions and disclaimer apply to these copyright holders.

## MAME DSP16 / DSP16A processor

`libpcsxcore/arcade/dsp16*` adapts the interpreter and peripherals in
`src/devices/cpu/dsp16`, at the pinned revision above. The original copyright
credit (Vas Crabb) and full BSD-3-Clause license are retained in `dsp16_LICENSE.txt`.

## MAME Sony ZN MCU

`libpcsxcore/arcade/zn_mcu.{c,h}` adapts the serial/input/timer model in
`src/mame/sony/znmcu.cpp`, copyright holder smf, at the pinned revision above.
The full BSD-3-Clause conditions and disclaimer are retained in the source.

## MAME Taito sound command link

`libpcsxcore/arcade/taito_sound_comm.{c,h}` adapts the TC0140SYT/PC060HA
protocol in `src/mame/shared/taitosnd.cpp`, copyright holder Philip Bennett.
The full BSD-3-Clause license is retained in `taito_sound_comm_LICENSE.txt`.

## MAME QSound LLE device

`libpcsxcore/arcade/qsound.{cpp,h}` adapts the DSP16A wrapper from
`src/devices/sound/qsound.cpp` (copyright Vas Crabb) and sample-ROM mapping
from `src/emu/dirom.ipp` (copyright Olivier Galibert). The full BSD-3-Clause
license and credits are retained in `qsound_LICENSE.txt`.

## MAME QSound HLE device

`libpcsxcore/arcade/qsound_hle.{cpp,h}` adapts the PCM/ADPCM, filters, echo,
delay and state-machine algorithms from `src/devices/sound/qsoundhle.cpp`,
copyright holders superctr and Valley Bell. ROM-space mapping also adapts
`src/emu/dirom.ipp`, copyright Olivier Galibert. Full BSD-3-Clause notices are
retained in `qsound_hle_LICENSE.txt`.

## MAME YM2610 / YM2610B and YMFM

`libpcsxcore/arcade/ym2610.{cpp,h}` adapts the sound-wrapper contracts in
`src/devices/sound/ymopn.cpp` and `ymfm_mame.h`, copyright Aaron Giles.
The required YMFM engine sources are retained unmodified in `arcade/ymfm/`,
copyright 2021 Aaron Giles. Full BSD-3-Clause terms are in `ymfm/LICENSE`.

## MAME Fujitsu MB3773 watchdog

`libpcsxcore/arcade/mb3773.{c,h}` adapts the fixed watchdog timeout, edge and
reset behavior of `src/devices/machine/mb3773.cpp`, copyright smf, at the
pinned revision. Full BSD-3-Clause terms are retained in `mb3773_LICENSE.txt`.

## MAME Taito FX-1S daughterboard

`libpcsxcore/arcade/taito_fx1.{c,h}` adapts the FX base and FX-1S maps, banks
and device wiring in `src/mame/sony/zn.cpp`, copyright smf and R. Belmont.
Its composed Z80, YMFM, TC0140SYT and MB3773 devices retain their respective
upstream BSD-3-Clause notices listed above.

## MAME PlayStation SIO0 / SIO1

`libpcsxcore/arcade/psx_sio.{c,h}` adapts the serial shifter, registers, pins
and timers in `src/devices/cpu/psx/sio.cpp` and `sio.h`, copyright 2003–2011
smf, at the pinned revision. Full BSD-3-Clause terms are retained in
`psx_sio_LICENSE.txt`.

## MAME NCR53CF96 SCSI controller

`libpcsxcore/arcade/ncr53cf96.{cpp,h}` adapts the controller, initiator phases,
registers and DMA behavior in `src/devices/machine/ncr53c90.cpp/.h` and NSCSI
wire constants from `nscsi_bus.cpp/.h`, copyright Olivier Galibert.
Full BSD-3-Clause terms are retained in `ncr53cf96_LICENSE.txt`.

## MAME ZOOM ZSG-2 sound device

`libpcsxcore/arcade/zsg2.{c,h}` adapts the synthesizer, sample decoder, filters,
registers and four sound outputs in `src/devices/sound/zsg2.cpp/.h`, copyright
Olivier Galibert, R. Belmont, hap and superctr. Full BSD-3-Clause terms and
attribution are retained in `zsg2.LICENSE`.

## MAME Konami K056800 MIRAC sound interface

`libpcsxcore/arcade/k056800.{c,h}` adapts the host/sound registers and IRQ
handshake in `src/devices/sound/k056800.cpp/.h`, copyright Ville Linde.
Full BSD-3-Clause terms are retained in `k056800_LICENSE.txt`.

## MAME 93C46 serial EEPROM

`libpcsxcore/arcade/eeprom93c46.{c,h}` adapts the 16-bit 93C46 command protocol
and storage timing in `src/devices/machine/eepromser.cpp` and `eeprom.cpp`,
copyright Aaron Giles. Full BSD-3-Clause terms are retained in
`eeprom93c46_LICENSE.txt`.

## MAME Konami GQ EEPROM defaults

`scripts/konamigq_boards.json` retains the 64-word EEPROM default table from
`src/mame/konami/konamigq.cpp`, copyright R. Belmont and smf. Full BSD-3-Clause
terms are retained in `scripts/konamigq_LICENSE.txt`. The extractor records
hardware and media declarations; it copies no driver implementation.

## MAME NSCSI bus and target protocol

`libpcsxcore/arcade/nscsi_bus.{cpp,h}` and `nscsi_target.{cpp,h}` adapt the
open-collector bus and target protocol from `src/devices/machine/nscsi_bus.cpp/.h`
and `nscsi_hle.cpp/.h`, copyright Olivier Galibert. Full BSD-3-Clause terms
are retained in `nscsi_LICENSE.txt`.

## MAME Konami K054539 PCM device

`libpcsxcore/arcade/k054539.{c,h}` adapts the PCM/DPCM voices, registers,
reverb and timer from `src/devices/sound/k054539.cpp/.h`, copyright Olivier
Galibert. Full BSD-3-Clause terms are retained in `k054539_LICENSE.txt`.

## MAME Intel 8251 and Fujitsu MB89371 serial devices

`libpcsxcore/arcade/i8251.{c,h}` and `mb89371.{c,h}` adapt
`src/devices/machine/i8251.cpp/.h`, `mb89371.cpp/.h`, `clock.cpp/.h`, and
`src/emu/diserial.cpp/.h`. Copyright holders are smf, Robbbert, R. Belmont,
Carl, Miodrag Milanovic, Vas Crabb and hap. Full BSD-3-Clause terms and
attribution are retained in `i8251_mb89371_LICENSE.txt`.

## MAME Konami GV and GQ common boards

`libpcsxcore/arcade/konamigv.{cpp,h}` and `konamigq.{cpp,h}` adapt the shared
hardware maps and wiring from `src/mame/konami/konamigv.cpp` and `konamigq.cpp`,
copyright R. Belmont and smf, at the pinned MAME revision above. Full
BSD-3-Clause notices are retained in `konamigv_LICENSE.txt` and
`konamigq_LICENSE.txt`.

## MAME Panasonic MN10200

`libpcsxcore/arcade/mn10200*` adapts the MN1020012A instruction and peripheral
register implementation from `src/devices/cpu/mn10200/mn10200.cpp/.h`, copyright
Olivier Galibert, R. Belmont and hap, at the pinned MAME revision above. Full
BSD-3-Clause terms are retained in `mn10200_LICENSE.txt`.

## MAME Taito Zoom sound board

The Taito Zoom sound-board adapter in `libpcsxcore/arcade/zoom_board.*` derives
its bus map, flash sample reader and reset wiring from MAME
`src/mame/sony/taito_zm.cpp/.h` and `taitogn.cpp`, copyright Olivier Galibert,
hap, superctr, cam900 and smf, BSD-3-Clause. Retained terms are in
`libpcsxcore/arcade/zoom_board_LICENSE.txt`.

The standalone G-Net flash-bank composition in `libpcsxcore/arcade/gnet_zoom.*`
uses the maps, little-endian flash regions, JP1 strap and reset wiring from
the same pinned `taitogn.cpp` and `taito_zm.cpp` sources. Their BSD-3-Clause
attribution and terms are retained in `zoom_board_LICENSE.txt`.

The RF5C296 controller in `libpcsxcore/arcade/rf5c296.*` and Taito PC Card
behavior in `libpcsxcore/arcade/gnet_pccard.*` derive from the pinned MAME
`src/devices/machine/rf5c296.cpp/.h` and
`src/devices/bus/pccard/ataflash.cpp/.h`, copyright smf, BSD-3-Clause.
The full attribution and terms are retained in
`libpcsxcore/arcade/gnet_pccard_LICENSE.txt`.

The source-generated ATA HLE, mass-storage and CompactFlash classes under
`libpcsxcore/arcade/ata_mame/` derive from the pinned MAME
`src/devices/machine/atahle.cpp/.h` and `atastorage.cpp/.h`, copyright smf,
BSD-3-Clause. Their upstream file notices are retained; the full conditions
and disclaimer in `libpcsxcore/arcade/gnet_pccard_LICENSE.txt` also apply to
these imported ATA sources. The adjacent `ata_storage.*` files supply the
standalone media and PC Card adapter.

## MAME Konami Twinkle sound board

The Twinkle sound-board adapter in `libpcsxcore/arcade/twinkle_board.*` derives
its bus map and interrupt wiring from MAME `src/mame/konami/twinkle.cpp`,
copyright smf and R. Belmont, BSD-3-Clause. Retained terms are in
`libpcsxcore/arcade/twinkle_board_LICENSE.txt`.

## Dynarmic Vita3K validation patch

`tools/vita-harness/patches/vita3k-neon.patch` adds A32 VADDHN translation to
the local Vita3K Dynarmic dependency at commit
`86458a0bd369d63ba4c2ef812cacbb6c9080c065`. Dynarmic is copyright 2017
merryhime and distributed under the 0BSD license; the full terms are retained
in `tools/vita-harness/patches/dynarmic-LICENSE.txt`. This patch affects the
validation emulator, while the Vita core continues to use its own ari64 DRC.

## Vita3K validation host patches

The teardown, Vulkan CPU-framebuffer screenshot and null-memcpy diagnostic patches under
`tools/vita-harness/patches` modify local Vita3K source, copyright the Vita3K
team, under GPL-2.0-or-later. The GPL version 2 terms are included in `COPYING`.
The private build manifest records source and patch hashes; these patches are
used by the validation host and are not linked into the Vita core.

## VitaGL validation patches

The shutdown and shader-cache patches under `tools/vita-harness/patches`
derive from the local VitaGL source at commit
`8a47034e078f7c39aa45e43493d7fe360a52a871`. The affected source headers retain
Rinnegatamante's copyright and LGPL-3.0-or-later notices. The upstream
`COPYING` file contains GPL version 3 terms and is retained verbatim in
`tools/vita-harness/patches/vitagl-COPYING.txt`. The private library build records
the source commit, ordered patch hashes, four flags and archive SHA-256.

## MAME Ricoh RF5C400

`libpcsxcore/arcade/rf5c400.{cpp,h}` adapts the sample engine and register map
from `src/devices/sound/rf5c400.cpp/.h`, copyright Ville Linde, with improvements
credited upstream to the hoot development team. The source revision is the
pinned MAME revision above. Full BSD-3-Clause terms are retained in
`rf5c400_LICENSE.txt`.

## MAME SH-2 and Namco special I/O

The SH-2 instruction handlers in `libpcsxcore/arcade/sh2_ops.inc` derive from
MAME `src/devices/cpu/sh/sh.cpp` and `sh2.cpp` at the pinned revision above,
copyright David Haywood, Juergen Buchmueller and R. Belmont (BSD-3-Clause).
The full notice is in `libpcsxcore/arcade/sh2_LICENSE.txt`.

The CDXA device map/peripheral behavior follows windyfairy’s
`namcos12_cdxa.cpp` and SH7014 devices. JVS packet behavior follows MAME’s
`jvshle.cpp` and `namcoio.cpp`; identities and capability responses were
checked against the original EMIO firmware through MAME. These upstream
components carry BSD-3-Clause notices. The standalone adapters are original
GPL-2.0-or-later integration code.
