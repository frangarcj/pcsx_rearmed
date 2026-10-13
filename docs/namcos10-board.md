# Namco System 10 MEM(M)/MEM(N) board adapter

`libpcsxcore/arcade/namcos10_board.{cpp,h}` is a standalone adapter for the
base MEM(M) and MEM(N) address maps from the pinned MAME
`src/mame/namco/namcos10.cpp` revision. It composes the existing Namco 10
crypto/NAND engine and Intel 28F640J5 flash model. It has no dependency on the
frontend or the System 12 board implementation.

The caller chooses `NS10_BOARD_MEMM` or `NS10_BOARD_MEMN`, supplies a MAME game
profile, a writable 4 MiB main program region, and the real media images. MEM(M)
requires a writable 8 MiB x16 flash image, a separate raw 8 MiB NAND-ROM region,
and its data-ROM region. MEM(N) requires each NAND dump as 528-byte pages
(512 data + 16 OOB), with enough pages for MAME's boot-copy ranges. All image
storage remains caller-owned. A table-based crypto profile also requires the
caller to supply its table bytes. Missing images, invalid geometries, unknown
profiles, unsupported crypto IVs, flash failures, and unsupported NAND writes
return explicit errors; creation does not synthesize keys or fill missing ROMs.

`ns10_board_read` and `ns10_board_write` accept physical PSX addresses,
1/2/4-byte widths and little-endian CPU values; KSEG0/KSEG1 aliases are
canonicalized. `mem_mask` uses MAME's ones-mean-written convention. `ns10_board_dma_read`
produces MEM(N) channel 5 NAND words in little-endian PSX RAM byte order for the
core to place at MADR. `ns10_board_advance_us` advances the MEM(M) Intel flash
clock and the connected base I/O timer. MEM(M) reproduces the MAME bank-index-by-address selection, ROM-region
versus direct-flash mode, game-profile unscrambling/decryption and 0x62000-byte
boot copy. MEM(N) reproduces the controller command/address/data sequence,
chip selection and lock bit, high-byte-first NAND wires, profile IV activation,
passive XOR/bit permutation, block-0/1 plaintext exception, and both boot-copy
ranges. The MEM/IO data-XOR register is decoded only for the MAME `ippo` and
`ippo2` profiles.

The native host adapter has [short Mac gameplay checks](system10-mac.md) for
Mr. Driller 2, Mr. Driller G and Gamshara. This module reports the MAME
base-map's EXIO expansion and CD-ROM/ATA peripherals as unimplemented.
An optional borrowed [base I/O module](namcos10-io.md) connects input ports,
EXIO identification, MAME's ACK-only I2C receiver and 100 Hz INT10 status/ack.
The caller wires the input and IRQ callbacks; board reset and elapsed-time
advancement also drive the connected I/O device. Its lifetime remains owned
by the caller. Without that module the base I/O aperture remains unsupported. Addresses in the main program ROM and unconfigured expansion
windows are left unmapped for the caller to handle. The MEM(N) R/B register
returns the selected chip's MAME callback state (`pin != 1`); MAME initializes
that state to zero, and NAND reset emits low then high callbacks, leaving the
visible register at zero. Valid single-column `0x30` read confirms are accepted
after the required address cycles. MEM(N) programming/erase is supported when
the caller supplies writable NAND backing; read-only configurations still
reject writes. The runtime uses writable in-memory images for settings and
preserves source files. MEM(M)
bank-register reads return the value used by MAME's current predictor, whose
source comment notes that its hardware behavior is uncertain.

Run its synthetic protocol checks with:

```sh
make -f tests/Makefile.namcos10-board test
```

The fixtures exercise MAME-derived MEM(M) and MEM(N) transaction traces,
independent fixed crypto vectors, the raw NAND boot-copy ordering, Intel flash
ID/program transactions and virtual time, shared-RAM partial writes, DMA byte
order, R/B reset/read-confirm state, connected base inputs/I2C/IRQ timing
and reset behavior, and explicit peripheral/write errors.
These synthetic tests validate
the adapter protocol; they do not establish boot or gameplay coverage.
