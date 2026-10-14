# Source CR-589 ATAPI transport

The standalone engine imports pinned MAME
`deb2f54fe7300e7a192690318beb860159bfd52a` ATA HLE, ATAPI HLE,
T10 SPC/MMC and Matsushita CR-589 command bodies. Source paths and hashes are
recorded in `libpcsxcore/arcade/atapi_mame/PROVENANCE.txt`.
`python3 tools/port_mame_atapi.py --check` verifies generated files against the
pinned repository. BSD source notices are retained.

The adapter replaces device ownership, timers, save-item registration, CD-image
and CDDA objects with bounded standalone callbacks. The source command engine,
IDENTIFY words, packet limits, IRQ acknowledgement, transfer budgets and drive
buffer/identity behavior remain in those imported bodies. The original drive's
64 KiB buffer begins with the source default inquiry identity at offset `3ab`.
It is mutable NVRAM, not a replacement game ROM or firmware. The source `CC`
download command must supply the actual matching identity bytes.

CR-589 invokes the source MMC engine directly. The generic ATAPI CD-ROM wrapper's
media-change/no-medium injection does not apply to this source device. In
particular, an absent-disc READ(10) produces its actual source CHECK CONDITION
with zero/prior sense state; the adapter does not replace that behavior with
another drive's sense response. WRITE BUFFER skips the source's 32-byte prefix
and advances its offset by the full incoming payload length.

Host guards reject out-of-range drive-buffer accesses, malformed short prefix
payloads, missing callbacks, invalid track counts and callback reentry. MMC
responders use bounded scratch because some source routines emit a complete
allocation header even when an ATA transfer chunk is smaller. Failed real media
reads are recorded as faults; no successful fabricated sector is returned.
Track type, pregap and frame geometry come from parsed CHD metadata. Packed
MODE1 tracks cannot satisfy a raw-sector request by inventing ECC bytes.

Synthetic component tests cover signature/IDENTIFY, inquiry, packet/chunked data,
IRQ status versus alternate-status reads, CDDA start/pause/resume callbacks,
firmware download identity, buffer persistence and error/reentry cases. Optimized
and ASan/UBSan runs pass.

The separate original DDR 2nd Mix integration check requires the source CD SHA1,
reads its actual LBA 16 sector through ATAPI and compares it with the direct
backend, reads its 73-track TOC and plays the real second audio track. One second
produces 44,100 stereo frames, 43,170 nonzero frames, peak 26,019 and no drive
faults. This proves the drive/media component; complete native PSX game and
speaker-mix evidence is tracked separately in `system573-mac.md`.

```sh
make -f tests/Makefile.atapi-cr589 test sanitize
python3 tools/port_mame_atapi.py --check
make -f tests/Makefile.chd-cd atapi-original CHD=roms/ddr2m/895jaa02.chd
```
