#!/usr/bin/env python3
"""Generated Capcom ZN-1/ZN-2 DRC and sound-worker checks; no game ROMs."""
import argparse
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import check_capcom_zn_harness as s


def make_media(directory, name):
    s.media(directory, name)
    banked = bytearray((directory / 'banked.bin').read_bytes())
    count = len(banked) // 0x400000 - 1
    for bank in range(count):
        struct.pack_into('<4I', banked, (bank + 1) * 0x400000 + 0x100,
                         s._addiu(17, 0, bank + 1), 0x03e00008, 0, 0)
        struct.pack_into('<I', banked, (bank + 1) * 0x400000, s._addiu(17, 0, 0x66 + bank))
    struct.pack_into('<2I', banked, 0x3ffff8, s._addiu(17, 0, 0x60), 0x03e00008)
    (directory / 'banked.bin').write_bytes(banked)
    country = bytearray((directory / 'country.bin').read_bytes())
    struct.pack_into('<4I', country, 0x100, s._addiu(17, 0, 0x4e), 0x03e00008, 0, 0)
    struct.pack_into('<2I', country, 0x7fff8, s._addiu(17, 0, 0x72), 0x03e00008)
    (directory / 'country.bin').write_bytes(country)
    code = list(struct.unpack(f'<{len(s.program()) // 4}I', s.program()))[:-6]
    # These devices retain their pin/FIFO state on soft reset. A new BIOS
    # transaction must deselect the MCU and explicitly reset the SIO first.
    at = code.index(s._addiu(9, 0, 0x8c))
    code[at:at] = [s._addiu(9, 0, 0x0c), s._sb(9, 8, 0x300)]
    at = code.index(s._lui(8, 0x1f80)) + 1
    code[at:at] = [s._addiu(9, 0, 0x40), s._sh(9, 8, 0x104a)]
    code += [s._lui(26, 0x8030)]
    for reg in range(16, 26):
        code.append(s._sw(reg, 26, 0x100 + (reg - 16) * 4))

    def select(bank):
        code.extend([s._lui(8, 0xbfb0), s._addiu(9, 0, bank), s._sb(9, 8, 0)])

    def call(address, slot):
        code.extend([s._lui(27, address >> 16), s._ori(27, 27, address & 0xffff),
                     s._jalr(27), 0, s._sw(17, 26, slot)])

    for bank in range(count):
        select(bank)
        call(0xbf400100 if bank & 1 else 0x9f400100, 0x180 + bank * 4)
    for i, bank in enumerate((0, 1, 0)):
        select(bank); call(0x9f400100, 0x1c0 + i * 4)
    for i, bank in enumerate((0, 1)):
        select(bank); call(0xbf3ffff8, 0x1d0 + i * 4)
    call(0xbfb80100, 0x1d8)
    call(0xbfbffff8, 0x1dc)
    code.append(s._sw(8, 26, 0x1e0))  # BIOS's first LUI in the delay slot
    # Execute and rewrite ordinary PSX RAM under the DRC.
    code += [s._lui(14, 0x8030), s._ori(14, 14, 0x400)]
    for offset, word in enumerate((s._addiu(17, 0, 0x31), 0x03e00008, 0)):
        code += [s._lui(11, word >> 16), s._ori(11, 11, word & 0xffff), s._sw(11, 14, offset * 4)]
    code += [s._jalr(14), 0, s._sw(17, 26, 0x300)]
    word = s._addiu(17, 0, 0x42)
    code += [s._lui(11, word >> 16), s._ori(11, 11, word & 0xffff), s._sw(11, 14, 0),
             s._jalr(14), 0, s._sw(17, 26, 0x304)]
    code += [s._lui(8, 0x1fa0), s._lui(9, 0x1fb4)]
    loop = 0xbfc00000 + len(code) * 4
    code += [s._i(0x24, 8, 14, 0), 0, s._sw(14, 26, 0),
             s._lw(15, 9, 0x10), 0, s._sw(15, 26, 4), s._j(loop), 0]
    rom = bytearray(0x80000)
    struct.pack_into(f'<{len(code)}I', rom, 0, *code)
    (directory / 'main.bin').write_bytes(rom)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--host', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='zn-drc-') as temp:
        for name in ('sfex', 'rvschool'):
            media = Path(temp) / name
            make_media(media, name)
            for cpu in ('interpreter', 'ari64'):
                results = []
                for threaded in (False, True):
                    cmd = [str(args.host.resolve()), str(args.core.resolve()), str(media), name, cpu]
                    if threaded: cmd.append('threaded')
                    run = subprocess.run(cmd, timeout=120, capture_output=True, text=True)
                    print(run.stdout + run.stderr, end='', flush=True)
                    run.check_returncode()
                    results.append(re.findall(r'RESULT .*', run.stderr))
                assert len(results[0]) == 2 and results[0] == results[1], results
                print(f'{name} {cpu}: serial/threaded audio counts and hashes identical PASS', flush=True)
            shutil.rmtree(media)


if __name__ == '__main__':
    main()
