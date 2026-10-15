# Source M24C02-W pin model

`libpcsxcore/arcade/m24c02.{cpp,h}` retains MAME's I2C state machine for the
256-byte M24C02, with its 16-byte write page. The generator checks both original
`i2cmem.cpp/.h` SHA256s from revision
`deb2f54fe7300e7a192690318beb860159bfd52a` and extracts the pin/address methods.
Framework logging is removed by macros; the source protocol is unchanged.
The BSD-3-Clause notice credits smf.

The single-owner caller supplies the actual 256 mutable EEPROM bytes. Pins
start at the source levels; E0/E1/E2 and write protection are exposed. Source
START/repeated START, device selection, ACK/NACK, random/sequential reads,
address wrap, software reset and buffered page writes are retained. A STOP
commits a pending page exactly as source. No timer, ready response or reset
method is added: this source device runs on its I2C pins and has no reset
override. Persist the bytes separately from the volatile protocol state.

```sh
make -f tests/Makefile.m24c02 test sanitize
python3 libpcsxcore/arcade/m24c02_generate.py ../mamedev --check
```

The generated vectors cover initial pins, random/sequential reads across the
last address, wrong-device NACK, an overlength wrapping write page committed
only at STOP, write protection, and reconstruction over persisted bytes.

An independent MAME run of original IIDX2 supplies 3,709 actual MMIO
transactions, including 2,422 security reads. `capture_twinkle_security.lua`
observes all three address aliases without replacing data or mutating the
EEPROM. Replaying the bus in ascending ioport field-mask order (SDA before
SCL) matches every read response and the final 256 bytes under O2 and
ASan/UBSan. This component proof is separate from original native gameplay.

To reproduce, create an empty evidence directory, set
`PCSX_TWINKLE_SECURITY_TRACE_DIR` to its absolute path, and run the audited
original `bmiidx2` set in MAME for 60 seconds with
`-autoboot_script tests/capture_twinkle_security.lua`, a fresh NVRAM/config
location and an explicit snapshot directory. Replay with:

```sh
build/m24c02-tests/m24c02 --source-trace /path/to/evidence/transactions.csv
build/m24c02-tests/m24c02-sanitize --source-trace /path/to/evidence/transactions.csv
```

The script also writes `initial.bin` and `final.bin` beside the CSV; these
original EEPROM bytes and all other media stay outside Git.
