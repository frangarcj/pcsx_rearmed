#!/usr/bin/env python3
"""Generated System 10 guests: bank changes, upper RAM, SIO and DMA/SMC.

Runs the same signatures with the interpreter and the production ari64 core.
Temporary media is removed between boards; no ROMs or raw captures retained.
"""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

import check_system10_harness as s


def make_media(directory, model):
    name = 'mrdrilr2' if model == 'MEMM' else 'chocovdr'
    profile = s.PROFILES[name]
    regions = s._nand_payload(profile, model)
    code = s.make_program(model)[:-5]  # Replace the final input polling loop.
    code += [s._lui(16, 0x8021)]
    if model == 'MEMM':
        # JR lies at the final word of a code page; its slot is on the next.
        for bank, target, offset in ((0, 0x9f400ff8, 0x80),
                                     (0x10, 0x9f400ff8, 0x84),
                                     (0, 0x9f400ff8, 0x88),
                                     (0, 0xbf400ff8, 0x8c)):
            code += [s._lui(10, 0x1fb4), s._sh(0, 10, bank * 2)]
            if bank < 0x10:
                code += [s._lw(0, 10, 0), 0]
            code += [s._lui(11, target >> 16), s._ori(11, 11, target & 0xffff),
                     s._jalr(11), 0, s._sw(17, 16, offset)]
        # Direct Intel flash array code must differ from passive banked ROM.
        code += [s._lui(10, 0x1fb4), s._sh(0, 10, 0),
                 s._lui(11, 0x9f40), s._ori(11, 11, 0x800), s._jalr(11), 0,
                 s._sw(17, 16, 0x90)]
        regions['data'] = bytearray(0x2000000)
        struct.pack_into('<4I', regions['data'], 0xff8,
                         s._addiu(17, 0, 0x6b), 0x03e00008, 0, 0)
    else:
        # Execute, rewrite and re-enter a MEM(N) shared-RAM function.
        code += [s._lui(8, 0xbf50), s._ori(8, 8, 0x400)]
        for offset, word in enumerate((s._addiu(17, 0, 0x11), 0x03e00008, 0)):
            code += [s._lui(10, word >> 16), s._ori(10, 10, word & 0xffff), s._sw(10, 8, offset * 4)]
        code += [s._jalr(8), 0, s._sw(17, 16, 0x80)]
        word = s._addiu(17, 0, 0x22)
        code += [s._lui(10, word >> 16), s._ori(10, 10, word & 0xffff), s._sw(10, 8, 0),
                 s._jalr(8), 0, s._sw(17, 16, 0x84)]
        # Compile a RAM function, then replace its instructions through NAND DMA5.
        code += [s._lui(8, 0x8021)]
        for offset, word in enumerate((s._addiu(17, 0, 0x33), 0x03e00008, 0)):
            code += [s._lui(10, word >> 16), s._ori(10, 10, word & 0xffff), s._sw(10, 8, offset * 4)]
        code += [s._jalr(8), 0, s._sw(17, 16, 0x88),
                 s._lui(10, 0x1f41), s._sb(0, 10, 0),
                 s._lui(10, 0x1f42), s._sb(0, 10, 0),
                 s._lui(10, 0x1f43), s._addiu(11, 0, 1), s._sb(11, 10, 0),
                 s._lui(10, 0x1f44), s._addiu(11, 0, 3), s._sb(11, 10, 0),
                 s._lui(10, 0x1f80), s._ori(10, 10, 0x10d0),
                 s._lui(11, 1), s._ori(11, 11, 4), s._sw(11, 10, 4),
                 s._lui(11, 0x21), s._sw(11, 10, 0),
                 s._lui(11, 0x0100), s._ori(11, 11, 0x0200), s._sw(11, 10, 8),
                 s._jalr(8), 0, s._sw(17, 16, 0x8c)]
        raw = bytearray(regions['nand0'])
        helper = struct.pack('<4I', s._addiu(17, 0, 0x44), 0x03e00008, 0, 0)
        raw[0x301 * 528:0x301 * 528 + 16] = s.encode_memn_bytes(helper)
        regions['nand0'] = raw
    # Preserve serial-register signatures before the polling loop changes GPRs.
    code += [s._sw(25, 16, 0xa0), s._sw(26, 16, 0xa4), s._sw(27, 16, 0xa8),
             s._lui(8, 0x1fba)]
    poll = 0xbfc00000 + 4 * len(code)
    code += [s._lhu(14, 8, 4), 0, s._sw(14, 9, 0), s._j(poll), 0]
    plain = bytearray(0x62000 if model == 'MEMM' else 0x20000)
    if model == 'MEMM':
        assert len(code) * 4 <= 0x400
        struct.pack_into('<4I', plain, 0x400, s._addiu(17, 0, 0x5a), 0x03e00008, 0, 0)
        struct.pack_into('<4I', plain, 0xff8, s._addiu(17, 0, 0x5a), 0x03e00008, 0, 0)
    struct.pack_into(f'<{len(code)}I', plain, 0, *code)
    if model == 'MEMM':
        raw = bytearray(regions['nand'])
        raw[:len(plain)] = s.encode_memm_boot(plain)
        helper = struct.pack('<4I', s._addiu(17, 0, 0x7c), 0x03e00008, 0, 0)
        # The Intel image stores each x16 bus word in big-endian byte order.
        for offset in range(0, len(helper), 2):
            raw[0x800 + offset:0x802 + offset] = helper[offset:offset + 2][::-1]
        regions['nand'] = raw
    else:
        raw = bytearray(regions['nand0'])
        encoded = s.encode_memn_bytes(plain)
        for offset in range(0, len(encoded), 512):
            start = (0x40 + offset // 512) * 528
            raw[start:start + 512] = encoded[offset:offset + 512]
        regions['nand0'] = raw
    s.prepare.package_regions(name, profile, regions, directory, {'synthetic_fixture': True})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--host', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='system10-drc-') as temp:
        root = Path(temp)
        for model in ('MEMM', 'MEMN'):
            profile = root / model
            make_media(profile, model)
            for cpu in ('interpreter', 'ari64'):
                for threaded in (False, True):
                    subprocess.run([str(args.host.resolve()), str(args.core.resolve()),
                                    str(profile), model, cpu] + (['threaded'] if threaded else []),
                                   check=True, timeout=120)
            shutil.rmtree(profile)


if __name__ == '__main__':
    main()
